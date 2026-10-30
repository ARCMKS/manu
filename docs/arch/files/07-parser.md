## `docs/arch/files/07-parser.md`
# `include/parser.h`, `src/parser.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

The parser turns a token stream into an AST. It is a hand-written
recursive-descent parser for statements and declarations, with a
Pratt-style precedence chain for expressions.

**What it solves.** Syntax: which token sequences are legal, and
what AST they produce. It does not resolve names, does not check
types, does not evaluate constant expressions, does not know
whether a construct is legal in its context. A syntactically
valid node may be semantically nonsense; the parser passes it on.

**What it does not solve.** Everything from sema onward. The
parser is the boundary between "is this text well-formed" and
"does this text mean something."

**Who depends on it.** `main.c` and the module loader. Nothing
else calls `parse_program`.

**Shape.** One public function. Everything else — the `Parser`
struct, the precedence chain, the statement dispatcher, error
recovery, the generic-call lookahead — is private.

---

## §2 — Public interface

```c
AstNode* parse_program(Lexer* lex, const SourceMap* smap,
                       DiagnosticBag* bag);
```

**Preconditions.** `lex` has been initialized over a source buffer
whose `file_id` is already registered with `smap`. `smap` is the
same map. `bag` is initialized.

**Postconditions.**

- The returned pointer is non-NULL. The parser always produces an
  `AST_PROGRAM`, even for empty or wholly malformed input.
- The program's declaration list contains one node per top-level
  declaration the parser recognized. Malformed declarations are
  skipped; a region may produce zero nodes.
- Every node carries a `Span` whose `file_id` matches the input.
- Every diagnostic produced is in `bag`. The caller checks
  `diag_has_errors(bag)` before running sema.

**On failure.** The parser does not have a distinct failure return.
It reports into the bag and returns whatever program it built.
Sema is expected to be skipped if any error was reported.

**On allocation.** The function allocates AST nodes. It never
frees them; the caller is responsible for `ast_free` on the
returned program.

**On re-entrancy.** The function is not reentrant with the same
`Lexer`. It copies the lexer state into its own `Parser` struct
and does not touch the caller's lexer after the initial `advance`.

---

## §3 — Parser state

```c
typedef struct {
    Lexer*           lex;
    Token            cur;
    Token            prev;
    DiagnosticBag*   bag;
    const SourceMap* smap;
    bool             panic;
    bool             restrict_struct_lit;
} Parser;
```

**`cur`** is the current token; every check reads it.
**`prev`** is the previously consumed token, used almost
exclusively for span end offsets.
**`bag`** and **`smap`** are borrowed.

**`panic`** is set to true by `expect` on failure. It is read by
`function_declaration`'s signature-recovery path (to decide
whether to skip to the body). Elsewhere it is written but not
read. It is effectively a per-function-declaration recovery flag.

**`restrict_struct_lit`** is a context-sensitive suppression flag.
When true, `primary` does not treat `{` after an identifier as a
struct-literal opener. It is set around:
- The scrutinee of a `match`.
- The iterable and (if present) the range end of a `for`.

It is **not** set for `if` or `while` conditions. This is
deliberate but asymmetric; see §7.

The struct is trivially copyable, which is what makes
`lexer_peek` work.

---

## §4 — Token stream primitives

```c
static void advance(Parser* p);
static bool check(Parser* p, TokenKind k);
static bool match(Parser* p, TokenKind k);
static Token expect(Parser* p, TokenKind k, const char* what);
static void end_statement(Parser* p, const char* ctx);
static void synchronize(Parser* p);
```

**`advance`** moves to the next non-`TOK_ERROR` token. Errors are
reported as `"unexpected character"` and consumed silently. The
loop discards consecutive error tokens, so a malformed byte
sequence produces one diagnostic per bad byte rather than one per
`advance`.

**`check`** tests the current token's kind without advancing.

**`match`** combines check-and-advance, returning true on a match.

**`expect`** is the primary diagnostic source. On success, consumes
the token and returns it. On failure, reports `expected X, got Y`
and returns the *current* token **without advancing**. The
"no consume on mismatch" rule is what lets `synchronize` see the
same token the failing production saw.

**`end_statement`** consumes a `;` if present, returns silently
on `}`, otherwise calls `expect(p, TOK_SEMI, ctx)`.

**`synchronize`** is panic-mode recovery: skip forward to the
next plausible statement boundary. Stop conditions:
- The previous token was `;`.
- The current token is `}`, or any statement-starting keyword.
- The current token is `;` (consumed).

The loop never returns without hitting EOF or one of those cases.

---

## §5 — Implementation notes

### §5.1 The precedence chain

Nine levels, from loosest to tightest:

```
expression → or_expr → and_expr → eq_expr → cmp_expr
          → add_expr → mul_expr → cast_expr → unary
          → postfix → primary
```

Each binary level (`or_expr`, `and_expr`, `eq_expr`, `cmp_expr`,
`add_expr`, `mul_expr`) is implemented via `binary_chain`:

```c
static AstNode* binary_chain(Parser* p,
                             AstNode* (*next)(Parser*),
                             const TokenKind* ops,
                             const BinOp*     bins,
                             int              n);
```

The function parses a left operand via `next`, then loops while the
current token is one of `ops`. Each iteration folds an
`ast_binary` node. The loop shape gives left-associativity for
free.

`cast_expr` is separate because its right-hand side is a
`TypeExpr`, not an expression. It loops on `as`, allowing
`x as i8 as i64`.

`unary` recurses into itself for right-associativity, so `--x` and
`*&x` parse as nested unaries.

### §5.2 Postfix

`postfix` starts with `primary` and loops on:

- `[expr]` — array/string indexing.
- `.name(args)` — method call. Produced as an `AST_VARIANT_CTOR`
  with `receiver_expr` set. The parser cannot tell whether
  `a.b(args)` is a method call on a local, a module call, or a
  variant constructor; sema disambiguates.
- `.name` — field access or unit-variant construction. Produced
  as `AST_FIELD`.
- `?` — try operator. Produced as `AST_TRY`.

The `.` case is where method syntax is created. `p.method(args)`
becomes `AST_VARIANT_CTOR(type_name = zero, variant_name = method,
receiver_expr = p, args = [...])`. The zero `type_name` (with
`file_id == 0`) is the sentinel for "no type name; this is a
method call."

### §5.3 Primary

`primary` handles every base case. The identifier case is the
most complex, with four sub-cases:

1. **`sizeof(T)`** — recognized only when the identifier is
   exactly `sizeof` and the next token is `(`. Otherwise
   `sizeof` is an ordinary identifier.
2. **Generic call / variant / struct literal** — detected by
   `check(p, TOK_LT) && peek_generic_call(p)`.
3. **Function call `f(args)`** — detected by `check(p, TOK_LPAREN)`.
4. **Dotted access from a bare identifier** — `a.b` or
   `Type.Variant(args)`.
5. **Struct literal `Name { field: v, ... }`** — detected by
   `check(p, TOK_LBRACE) && !p->restrict_struct_lit`.
6. **Plain identifier** — the fallback.

The order matters. `sizeof` is checked first so a user-defined
identifier named `sizeof` still works in non-call position.
Generic-call detection comes before plain-call detection because
the generic form starts with `<` and needs to consume the type
arguments before looking for `(`.

### §5.4 `peek_generic_call`

```c
static bool peek_generic_call(Parser* p);
```

Called with the current token being `<`. Copies the lexer state
into a throwaway `Lexer`, scans up to 64 tokens tracking `<`/`>`
nesting, and returns true iff the matching `>` is immediately
followed by `(`, `.`, or `{`.

The three accepted continuations cover:
- `f<T>(args)` — generic function call.
- `Type<T>.Variant` — generic variant constructor.
- `Type<T> { field: v }` — generic struct literal.

Bailout tokens: `TOK_EOF`, `TOK_SEMI`, `TOK_RBRACE`. A `;` or `}`
before the matching `>` means the expression ended without a
generic opener; the scan stops.

The 64-token cap prevents infinite scans on unterminated generic
syntax. It is generous for any realistic type argument list.

This is the only place in the parser that does unbounded
lookahead. Everything else uses one token.

### §5.5 Type expressions

`parse_type_expr` handles six forms:

```
type_expr := 'fn' '(' params? ')' ':' type_expr
           | '*' type_expr
           | 'rc' '<' type_expr '>'
           | 'ref' '<' 'mut'? type_expr '>'
           | '[' type_expr ']'
           | IDENT ('<' type_expr (',' type_expr)* '>')?
           | IDENT '.' IDENT ('.' IDENT)*
```

The `fn(T1, T2) : R` form is parsed first because `fn` is a
reserved keyword and no other type production starts with it.

`rc`, `ref`, `*`, and `[` are keywords or punctuation that cannot
begin an identifier, so they are tested before the identifier
case.

Qualified paths (`a.b.c`) collect components into a fixed stack
array of `PRISM_MAX_PATH_COMPONENTS` (16) entries. Overflow
produces a diagnostic and truncates.

Generics (`Name<...>`) parse after the base identifier. The
argument list is a comma-separated `parse_type_expr` sequence.
Empty generic lists `Name<>` are accepted by the parser (the
inner `if (!check(GT))` skips the do-while), though sema will
report them as arity mismatches if the template has parameters.

### §5.6 Type parameter lists

```c
static int parse_type_params(Parser* p, Token* out, int max);
```

Called after the name of a `fn`, `struct`, or `enum`. Returns
the number of parameters parsed (0 if no `<`). The cap is 8 at
every call site, matching `type_params[8]` in the AST.

Excess parameters are parsed (so the `>` is consumed and the
rest parses) but not stored, one diagnostic per extra.

`<>` is a parse error (the first `expect(IDENT)` fails), which
matches the language's rule that a generic with no parameters is
not expressible.

### §5.7 Statements

`statement` dispatches on the leading token. Every branch is
covered in `language/reference.md` §6. The dispatch is an
`if-else` chain, in this order:

`let`, `if`, `while`, `for`, `match`, `break`, `continue`,
`return`, block (`{`), expression statement or assignment.

The `break`/`continue` branches call `end_statement` themselves
(they have no body). `return` calls `return_statement` which
handles both the bare and the value forms.

The expression-statement fallback parses an expression, then
checks for `=`, `+=`, `-=`, `*=`, `/=`. The `=` path is
`AST_ASSIGN`; the compound paths are `AST_ASSIGN_OP`; the
no-operator path is `AST_EXPR_STMT`.

### §5.8 Error recovery

The `before = bag->error_count` pattern appears in five places:
`statement`, `block`, `match_statement`, `function_declaration`,
`parse_program`. In each, the enclosing production snapshots the
error count, parses a sub-production, and if the count went up,
takes recovery action.

For `statement`, recovery is `synchronize`. For `block`, recovery
is setting `p->panic = true` and continuing (the next iteration
will parse a fresh statement). For `match_statement`, recovery is
breaking out of the arm loop. For `function_declaration`, recovery
is skipping forward to `{` or a top-level starter. For
`parse_program`, recovery is skipping forward to the next
top-level starter keyword.

The placeholder-substitution pattern (`if (!n) n = ast_int(...)`)
appears where a NULL node would break the parent's invariant.
In practice this happens only at the top of `statement` when the
error occurred before any node was created.

### §5.9 Top-level loop

`parse_program` dispatches on the leading keyword: `fn`, `const`,
`type`, `using`, `pub`, `extern`. A leading `;` is skipped.

After any declaration, if `bag->error_count > before`, the loop
advances until the current token is one of `fn`, `const`, `type`,
`using`, `pub`, `extern`, or EOF. This is a coarser recovery than
`synchronize`; it uses the top-level keyword set rather than the
statement set.

---

## §6 — Invariants

- `parse_program` returns a non-NULL `AST_PROGRAM` on every path.
- Every `expect` failure reports exactly one diagnostic.
- Every AST node has a valid `Span`.
- Every name in the AST is a `Token` whose `span.file_id` matches
  the one passed to `lexer_init`.
- Type parameter and parameter counts are capped at 8. Excess is
  diagnosed and discarded.
- Path component counts are capped at 16.
- Generic lookahead is bounded to 64 tokens.
- A block's statement list contains only non-NULL statements;
  leading `;` produces NULL and the caller skips it.
- `p->prev` after `advance` is the last non-`TOK_ERROR` token
  seen.

---

## §7 — Known limitations

**No `&&` or `||` at the AST level.** Both are lexed as single
tokens. The parser produces no binary node for them and there
is no precedence level for them. A user writing `a && b` gets a
parse error at the `&&`.

**No `mut` on `let` inside some contexts.** `let mut x = 5` is
parsed. Compound parameter forms like `fn f(mut x: i64)` are
parsed. The parser's coverage is complete for the current
language, but the grammar has no extension points for a future
`mut` in other positions.

**`restrict_struct_lit` asymmetry.** The flag is set for `match`
scrutinees and `for` sources but not for `if` or `while`
conditions. Struct literals at the start of an `if` or `while`
condition must be parenthesized.

**`for` sources have an overly restrictive flag.** The
`restrict_struct_lit` flag is active for the entire `for` source
expression, including nested array literals. `for x in [Point {
x: 0 }] { ... }` will not parse the inner struct literal. The
workaround is to bind the array to a `let` first.

**Integer literals wrap past 64 bits.** The parser's
`parse_int_value` catches overflow and reports `integer literal
too large`, resetting the value to 0.

**`peek_generic_call` is heuristic.** `f < x > (y)` parses as a
generic call even if the user intended three comparisons. This
matches the behavior of Rust, Swift, and C#. The workaround is
parenthesization.

**No `else if` sugar.** `else if` parses as `else` followed by a
block whose single statement is an `if`. The result is a
right-leaning tree of `AST_IF` nodes. Not a bug, but a reader
of the AST will see the nesting.

**`panic` is set but rarely read.** See §3. It is read only in
`function_declaration`'s signature-recovery check. Everywhere
else it is written and ignored.

**`restrict_struct_lit` is a flag, not a set of positions.** A
future syntax change that needed two different suppression
contexts would need a more expressive representation.

**Integer literal parsing produces a `U64`.** A literal above
`2^63 - 1` is stored as a two's-complement negative value. Whether
that is the intended interpretation depends on context.

---

## §8 — Extension points

- **A boolean-operator level.** Adding `&&` and `||` to the
  parser's precedence chain is a two-line addition to the
  `binary_chain` call graph. Requires lexer support (already
  present: `TOK_AMPAMP`, `TOK_PIPEPIPE`) and a `BIN_AND`/`BIN_OR`
  AST node (already present in `BinOp`).
- **A public `parse_expr` or `parse_stmt` entry point.** Would
  let a REPL or an interactive tool parse fragments.
- **An error-recovery callback.** The current `synchronize` is
  private and hardcodes the recovery set. A caller-supplied
  callback would let a tool choose what to skip to.
- **`else if` sugar.** Purely cosmetic; the current parse is
  correct.
- **Removing the `restrict_struct_lit` asymmetry.** Requires
  deciding whether `if (Point { x: 0 }).x == 0` should parse
  (it currently doesn't). The standard resolution is
  parenthesization, which the grammar requires anyway.
- **A real `panic` mechanism.** Currently the field is a
  leftover. A real cascade-suppression scheme would use it to
  skip further diagnostics within a malformed production.
- **Bounded generic lookahead.** Replacing the 64-token cap with
  a proper recursive scan that aborts on unbalanced brackets
  would be more robust.