## `docs/language/reference.md`

# The Prism Language Reference

*Current as of commit `<hash>`, <date>.*

This document defines the Prism language as it exists in the
current implementation. It is descriptive, not prescriptive: where
this document and the compiler disagree, the compiler is right.

Every construct gets: a grammar production, a worked example, a
semantics paragraph, and an edge-case list. Code blocks marked
` ```prism runnable ` are executed by the test suite; if they don't
compile or don't produce the stated result, the test suite fails.

For an introduction to the language, see `tutorial.md`. For idioms
and conventions, see `patterns.md`. For diagnostic messages, see
`errors.md`.

---

## §1 — Lexical structure

### §1.1 Source files

A source file is a UTF-8-compatible byte sequence. Non-ASCII bytes
are lexer errors today: identifier classification uses `isalpha` and
`isalnum` in the C locale, which classify only ASCII letters.
Sources are expected to be ASCII.

A file is registered with the `SourceMap` before lexing begins. The
map assigns it a 1-based `file_id` and stores a copy of its bytes
plus a precomputed line-start table. Every token the lexer produces
carries a `Span` referring to a byte range in this copy.

### §1.2 Whitespace and comments

Space (`0x20`), tab (`0x09`), and carriage return (`0x0D`) are
whitespace. Newlines (`0x0A`) are also whitespace, but they are
significant for automatic semicolon insertion (§1.9).

Comments begin with `//` and continue to the end of the line or the
end of the file. They are discarded. There are no block comments.

### §1.3 Identifiers

```
identifier := ident_start ident_cont*
ident_start := [A-Za-z_]
ident_cont  := [A-Za-z0-9_]
```

A single `_` is lexed as an identifier but is treated specially in
patterns as a wildcard (§7.1).

### §1.4 Keywords

Twenty-eight reserved words:

```
fn  let  if  else  while  for  in  return  as
true  false  ref  rc  mut  const  break  continue
type  struct  enum  match  using  pub  extern  from
```

Two soft keywords, recognized by name where they appear:

- **`lib`** — recognized only immediately after `extern`.
- **`opaque`** — recognized only as the right-hand side of an
  `extern type X = ...` declaration. Elsewhere, `opaque` is an
  ordinary identifier.
- **`sizeof`** — recognized only when followed by `(`. Elsewhere,
  `sizeof` is an ordinary identifier.
- **`union`** — recognized only after `extern type X =`. Elsewhere,
  an ordinary identifier.

### §1.5 Integer literals

Decimal, hexadecimal (prefix `0x` or `0X`), or binary (prefix `0b`
or `0B`). No sign, no underscores, no octal.

```
42   0   10000000000   0xFF   0b1010
```

Sign is a unary operator (§3.6), not part of the literal. A literal
whose value doesn't fit in 64 bits is diagnosed at parse time with
`integer literal too large`; the value is set to 0 to allow
analysis to continue.

Hex and binary literals are parsed into `U64` and then stored as
`I64` in the AST. Values above `2^63 - 1` that appear in an integer
context are interpreted as two's-complement negative numbers.

### §1.6 Float literals

A run of digits, a single `.`, and a run of digits. No exponent.
No leading sign.

```
3.14   0.5   100.0
```

A leading digit is required. `1.5` is a float; `.5` is a syntax
error. `0..5` lexes as integer `0`, `..`, integer `5` because the
`.` is only consumed as a decimal point when the next character is a
digit.

A float literal is parsed by `strtod` after being copied into a
stack buffer. A literal longer than 63 bytes is truncated at the
copy; no diagnostic is emitted.

### §1.7 String literals

Two forms:

```
"..."    — Prism string (RC-managed at runtime)
c"..."   — C string (NUL-terminated at runtime)
```

Both run to the first unescaped `"` on the same line. A newline
before the closing quote is an error (unterminated string).

The `c` prefix must be immediately followed by `"`. `c "..."` lexes
as identifier `c` followed by a Prism string.

Escape sequences, resolved by codegen (not the lexer) at the point
of use:

| Escape | Meaning |
|---|---|
| `\n` | newline (0x0A) |
| `\t` | tab (0x09) |
| `\r` | carriage return (0x0D) |
| `\\` | backslash |
| `\"` | double quote |
| `\X` (other) | literal X (backslash dropped) |

The escape `\"` is *not* honored by the lexer's scan for the closing
quote. `"a\"b"` is lexed as the string `"a\"` followed by identifier
`b` followed by an unterminated string starting at the second `"`.
This is a known limitation; there is no way to write a string
containing a literal double quote today.

Prism strings may contain embedded NUL bytes. C strings may not;
the NUL terminator is added at materialization.

### §1.8 Punctuation and operators

```
(  )  {  }  [  ]  ;  :  ,  ?
+  -  *  /  &  .  !
..  ...  ->  =>  =  ==  !=  <  <=  >  >=
+=  -=  *=  /=  &&  ||
```

`->` is lexed but not used by the parser. `?` is the Result/Option
propagation operator (§11.2). `...` appears only in `extern fn`
parameter lists (§13.2). `+=`, `-=`, `*=`, `/=`, `&&`, `||` are
compound assignment and short-circuit logical operators.

### §1.9 Automatic semicolon insertion

When the lexer crosses a newline (or reaches end of input) and the
previous significant token is one of the following, it emits a
synthetic `TOK_SEMI` with a zero-length span at the first newline:

```
identifier  integer  float  string  cstring
true  false  )  }  ]  return
```

Consecutive newlines produce at most one semicolon: after the
synthetic `;`, the lexer's `prev_significant` is `TOK_SEMI`, which
is not itself a terminator.

Consequences:

- **`{` must appear on the same line as the `)` that closes a
  condition or parameter list.** `if (x)\n{ ... }` fails to parse.
- **Binary operators must trail their left operand.** `1 +\n 2`
  is valid; `1\n + 2` is not (the newline after `1` triggers ASI).
- **A statement may end with `}` instead of `;`.** The parser's
  `end_statement` accepts a `}` as an implicit terminator.

### §1.10 Reserved punctuation that isn't used

`TOK_EQ` (the bare `=`) is declared in `lexer.h` but never emitted;
the lexer emits `TOK_ASSIGN` for `=`. This is a dead enum value.

`TOK_ARROW` (`->`) is emitted but the parser never consumes it.

---

## §2 — Types

### §2.1 Primitive types

| Type | Size | Range / notes |
|---|---|---|
| `void` | 0 | Only as a return type |
| `i8` `i16` `i32` `i64` | 1 / 2 / 4 / 8 | Signed integers |
| `u8` `u16` `u32` `u64` | 1 / 2 / 4 / 8 | Unsigned integers |
| `f32` `f64` | 4 / 8 | IEEE 754 |
| `bool` | 8 | Stored as `0` or `1` in a 64-bit slot |
| `string` | 8 | Pointer to an RC block `[len][bytes]` |

All values occupy a full 64-bit stack slot at runtime. The declared
width matters for arithmetic (a `i8 + i8` wraps at 8 bits; see
§3.3) and for FFI marshalling, but not for the storage layout.

### §2.2 Compound types

```
type_expr := '*' type_expr
           | 'rc' '<' type_expr '>'
           | 'ref' '<' 'mut'? type_expr '>'
           | '[' type_expr ']'
           | 'fn' '(' (type_expr (',' type_expr)*)? ')' ':' type_expr
           | qualified_type

qualified_type := IDENT ('<' type_expr (',' type_expr)* '>')?
                | IDENT '.' IDENT
```

| Type | Representation |
|---|---|
| `ref<T>` | 8-byte pointer into the lexical arena (or a stack slot for `&local`) |
| `ref<mut T>` | Same, but assignment through it is allowed |
| `rc<T>` | 8-byte pointer to an RC block (`RcHeader` then payload) |
| `*T` | 8-byte raw pointer |
| `[T]` | 8-byte pointer to `[u64 length][elements…]` |
| `Type` (struct) | 8-byte pointer to `[u64 field_count][fields…]` |
| `Type` (enum) | 8-byte pointer to `[u64 tag][payloads…]` |
| `fn(T1, T2) : R` | 8-byte codegen function index (a `U32` in a 64-bit slot) |
| `Result<T, E>` | RC-managed enum; equivalent to `rc<Enum>` at use sites |
| `Option<T>` | RC-managed enum; equivalent to `rc<Enum>` at use sites |

`Result` and `Option` are declared by the compiler prelude. Their
`TypeDef.is_rc_managed` is `true`, which means every use site
resolves them to `rc<...>`. See §11 for details.

### §2.3 Escape rules

Four rules constrain the compound types. Each is enforced during
type resolution (`types.c`'s `resolve_inner`) and at return sites
(`sema.c`'s `analyze_stmt`).

1. **No nested references.** `ref<ref<T>>`, `rc<ref<T>>`,
   `*ref<T>`, and `[ref<T>]` are all rejected. The diagnostic names
   the outer constructor.

2. **No `ref<T>` return types.** A function cannot return a
   reference. Checked at both signature resolution and inference.

3. **No `*T` return types.** A function cannot return a raw pointer.

4. **No `[T]`, `struct`, or `enum` return types.** A function
   cannot return an aggregate directly. The return type must be
   wrapped in `rc<...>`.

Additionally, `ref<T>` cannot contain an extern struct, and no
compound type can contain an extern struct by value. Pointers to
extern structs are allowed.

The rules exist because they eliminate every path by which an
arena pointer could outlive its scope. With all four enforced,
there is no need for a marking pass; the type system *is* the
escape analysis.

### §2.4 Type equality

Two types are equal iff `type_eq` returns true. The rules:

- Primitives compare by kind (they are singletons in `types.c`, so
  pointer equality is a valid fast path).
- `ref<T>` compares the mutability flag and recurses into `T`.
- `rc<T>`, `[T]` recurse into `T`.
- `*T` compares by pointee, *except* that `*void` unifies with any
  other pointer type (the language's `void*`).
- `fn(...)` compares parameter types and return type structurally.
- Struct and enum types compare their `TypeDef` pointers (nominal
  equality).

`is_rc_managed` is a property of the `TypeDef`, not the `Type`.
Two `Type*` with the same shape are equal iff their `tdef` pointers
are equal for struct/enum, or structurally for everything else.

---

## §3 — Expressions

### §3.1 Precedence

From lowest to highest:

| Level | Operators | Associativity |
|---|---|---|
| 1 | `\|\|` | left |
| 2 | `&&` | left |
| 3 | `==` `!=` | left |
| 4 | `<` `<=` `>` `>=` | left |
| 5 | `+` `-` | left |
| 6 | `*` `/` | left |
| 7 | `as T` | left |
| 8 | unary `-` `!` `*` `&` `rc` | right |
| 9 | postfix `[e]` `.f` `.m(e)` `?` | left |

Parentheses override precedence. All binary operators are left-
associative; unary operators are right-associative.

### §3.2 Arithmetic

`+` `-` `*` `/` are defined on integers and floats. Both operands
must have the same type. Division by zero is a runtime error.

**Integer arithmetic wraps at the operand's width.**

```
runnable
fn main(): i64 {
    let a: i8 = 120;
    let b: i8 = 20;
    // 140 wraps to -116 in i8
    if (a + b != -116) { return 1; }
    return 0;
}
```

Signed and unsigned integers use separate VM paths: signed
division of `INT_MIN` by `-1` is the signed path's behavior; unsigned
division never has an undefined case.

`+` is also defined on strings (concatenation).

### §3.3 Comparison

`==` and `!=` are defined on: all scalars, `string`, and `*T`.

`<` `<=` `>` `>=` are defined on: all integers, all floats, and
`string` (lexicographic).

Comparing two raw pointers with `<` is not defined. Comparing two
structs, enums, arrays, or references is not defined; there is no
structural equality at runtime for aggregates.

Comparison of two integers of different types is not defined; the
operands must have the same type. A literal operand is adapted to
the other operand's type if it fits (see §4.3).

### §3.4 Logical operators

`&&` and `||` take two `bool` operands and yield `bool`. Both are
short-circuiting.

```
runnable
fn main(): i64 {
    if (true || (1 / 0 > 0)) { return 1; }
    return 0;
}
```

### §3.5 Casts

```
cast_expr := unary ('as' type_expr)*
```

`as` binds tighter than any binary operator and looser than unary.
Casts are left-associative: `x as i8 as i64` casts to `i8` first,
then to `i64`.

**Castable pairs** (`cast_valid` in `types.c`):

- Scalar to scalar, any direction: integers, floats, `bool`.
- `*T` to `*U`, any pointer to any other pointer.
- `*T` to `i64` or `u64`, and back.

Everything else is a compile error.

**Cast semantics:**

| From | To | Behavior |
|---|---|---|
| integer | integer | Truncate to width, sign- or zero-extend |
| integer | float | C-cast semantics |
| integer | bool | `0` becomes `false`, otherwise `true` |
| float | integer | Truncate toward zero; NaN or out-of-range traps at runtime |
| float | float | Widening or narrowing |
| float | bool | `0.0` becomes `false`, NaN becomes `false`, otherwise `true` |
| bool | integer | `true` = 1, `false` = 0 |
| pointer | pointer | Bit-preserving; no instruction emitted |
| pointer | integer | Bit-preserving |
| integer | pointer | Bit-preserving |

### §3.6 Unary operators

```
unary := '-' unary
       | '!' unary
       | '*' unary
       | '&' 'mut'? unary
       | 'rc' unary
       | postfix
```

`-e` — arithmetic negation. Operand must be a signed integer or a
float. Unsigned negation is rejected.

`!e` — logical not. Operand must be `bool`.

`*e` — dereference. Operand must be `ref<T>`, `rc<T>`, or `*T`.
The result type is the pointee. Dereferencing a pointer to an
opaque type or a value-representation-less extern struct is
rejected.

`&e` — borrow. If `e` is a local variable, the result is a
`ref<T>` pointing at the local's stack slot (no allocation). If
`e` is any other expression, its value is copied into an 8-byte
arena cell and a `ref<T>` to the cell is produced.

`&mut e` — same, but produces `ref<mut T>`. Writes through the
reference are allowed.

`rc e` — allocate on the RC heap. For scalars, produces a
single-cell `rc<T>`. For literals of compound types (array
literal, struct literal, variant constructor, unit variant), the
value is built directly on the RC heap. `rc variable` is
rejected for compound types.

### §3.7 Postfix operators

```
postfix := primary postfix_op*
postfix_op := '[' expr ']'
            | '.' IDENT '(' args? ')'
            | '.' IDENT
            | '?'
```

**`e[i]`** — index. `e` must be `[T]`, `rc<[T]>`, or `string`.
The index must be an integer. The result is `T` for arrays and
`u8` for strings. Bounds are checked at runtime; out-of-range
access traps.

**`e.name(args)`** — method call. The receiver `e` is evaluated
first, then the arguments. The receiver is passed as argument 0.
The method must exist on `e`'s type (or the type's `rc<T>` or
`ref<T>` wrapper; one level is unwrapped).

**`e.name`** — field access on a struct, or unit-variant
construction on an enum (`Color.Red`).

**`e?`** — Result/Option propagation. See §11.2.

### §3.8 Primary expressions

```
primary := INT | FLOAT | STRING | CSTRING
         | 'true' | 'false'
         | 'sizeof' '(' type_expr ')'
         | IDENT ( '::' type_args )? '(' args? ')'      // call
         | IDENT ( '::' type_args )? '.' IDENT args?    // variant/method
         | IDENT ( '::' type_args )? struct_lit          // struct lit
         | IDENT                                          // variable
         | '(' expr ')'
         | '[' expr (',' expr)* ','? ']'                 // array lit
```

(The `::` in the grammar above stands for `<T, …>`; see the
disambiguation note below.)

**Integer, float, bool, string literals** — see §1.5–§1.7.

**`sizeof(T)`** — the size in bytes of `T`. Yields `u64`. Rejected
for `void` and for opaque types.

**Function call `f(args)`** — the callee is resolved by name. If
the name is a local of function type, the call is indirect. If the
name is a builtin, the builtin's argument rules apply (§14). If
the name is a user function, its arity and argument types are
checked. Argument types are checked with implicit widening (§5).

**Generic call `f<T, U>(args)`** — explicit instantiation. See §9.

**Variant constructor `Type.Variant(args)`** — constructs an enum
value. The parenthesised form is optional for nullary variants.

**Struct literal `Type { field: value, ... }`** — constructs a
struct value. Every field must be initialized exactly once; a
missing or unknown field is a compile error.

**Array literal `[e1, e2, ...]`** — all elements must have the
same type. Empty arrays are rejected.

**Parenthesized `(e)`** — the parentheses do not create a node;
`(e)` is the same AST as `e`.

### §3.9 The generic call disambiguation

`<` is ambiguous between "less than" and "generic opener". The
parser resolves this with a bounded lookahead (`peek_generic_call`
in `parser.c`): the `<` is treated as a generic opener iff the
matching `>` is immediately followed by `(`, `.`, or `{`.

```
f<T>(x)          // generic call
Type<T>.Variant  // generic variant
Type<T> { ... }  // generic struct literal
a < b > (c)      // comparison chain, not a call
```

The disambiguation is textually a heuristic. It matches the
behavior of Rust, Swift, and C#. A user who wants `f < T > (x)`
parsed as three comparisons must parenthesize: `(f < T) > (x)`.

---

## §4 — Type inference

### §4.1 Local types

`let x = e` infers the type of `x` from `e`. `let x: T = e`
requires `e` to be of type `T` (or to widen to `T`; see §5).

### §4.2 Parameter and return types

Parameters default to `i64` if unannotated. Return types are
inferred from the first value-return in the body.

A function with no value-return and no annotation has return type
`void`.

A function whose first value-return is on a recursive call has an
unresolvable return type. Two examples that fail:

```
fn main() { return get(); }
fn get() { return 42; }
```

The call to `get` is analyzed before `get`'s return type is
inferred, so the call site sees `NULL`. The diagnostic is
`cannot infer return type of 'get' at this call site`.

```
fn odd(n: i64) { if (n == 0) { return false; } return even(n - 1); }
fn even(n: i64) { if (n == 0) { return true; } return odd(n - 1); }
```

The two functions are mutually recursive; neither can establish a
return type before the other.

The workaround is an explicit annotation: `fn get(): i64 { ... }`.

### §4.3 Literal adaptation

An integer or float literal in a context with a known target type
takes the target type if the value fits.

```
let x: i8 = 100;      // 100 has type i8
let y: i8 = 300;      // error: 300 does not fit in i8
let z = x + 5;        // 5 adapts to i8
```

The adaptation applies:

- To the initializer of an annotated `let`.
- To the integer or float literal operand of a binary expression
  when the other operand has a known non-literal type.
- To the arguments of a function call when the parameter has an
  annotated type.
- To the bounds of a `for..range` when the other bound has a
  known non-literal type.
- To the elements of an array literal when the array's element
  type is known from context.

If the literal doesn't fit, it is not adapted and the target
type's constraint produces `operands have different types` or
`cannot implicitly convert`.

### §4.4 Compound type inference

`rc e` in an `rc<T>` context propagates `T` into `e`. `&e` in a
`ref<T>` context propagates `T` into `e`. A struct literal in a
struct-typed context uses the context's `TypeDef`. These
propagations are what make `let x: rc<[i32]> = rc [1, 2, 3];`
type-check.

---

## §5 — Widening

An expression of type `A` is accepted in a position that expects
`B` if `type_widens_to(A, B)` is true. The rule is that every
value representable as `A` is representable exactly as `B`.

| From | To |
|---|---|
| `i8` | `i16`, `i32`, `i64` |
| `i16` | `i32`, `i64` |
| `i32` | `i64` |
| `u8` | `u16`, `u32`, `u64`, `i16`, `i32`, `i64` |
| `u16` | `u32`, `u64`, `i32`, `i64` |
| `u32` | `u64`, `i64` |
| `u64` | (nothing) |
| `i8` `u8` | `f32`, `f64` |
| `i16` `u16` | `f32`, `f64` |
| `i32` `u32` | `f64` |
| `i64` `u64` | (nothing) |
| `f32` | `f64` |

When a widening crosses the int↔float boundary or narrows a float
(`f32 → f64`), codegen inserts a runtime `OP_CONVERT`. Widening
between two integers of the same signedness is a no-op at the
VM level.

Widening is only applied at binding sites: `let`, assignment,
argument passing, return, struct field initialization, array
element initialization. It is not applied inside an expression:
`a + b` still requires both operands to have the same type.

---

## §6 — Statements

```
stmt := let_stmt | assign_stmt | expr_stmt
      | if_stmt | while_stmt | for_stmt | match_stmt
      | break_stmt | continue_stmt | return_stmt
      | block
```

Statements appear only inside function bodies.

### §6.1 `let`

```
let_stmt := 'let' 'mut'? IDENT (':' type_expr)? '=' expr term
```

Introduces a local binding. `mut` is required for reassignment.
Without `mut`, an assignment to the binding is a compile error.

```
let x = 5;              // immutable, inferred type
let mut y = 5;          // mutable, inferred type
let z: i8 = 5;          // immutable, annotated type
let mut w: f64 = 5.0;   // both
```

The initializer is required. `let x: T;` (declaration without
initializer) is a syntax error.

### §6.2 Assignment

```
assign_stmt := assign_target '=' expr term
assign_target := IDENT
              | expr '.' IDENT
              | expr '[' expr ']'
              | '*' unary
```

Assignment is a statement, not an expression. `x = y = 5` is a
syntax error.

The target's mutability is checked: a `let` binding without `mut`,
a parameter (which is immutable by default), or a `const`
declaration cannot be assigned to. A `ref<T>` (immutable reference)
cannot be written through; a `ref<mut T>`, `*T`, or `rc<T>` can.

An assignment's value must widen to the target's type (§5).

### §6.3 Compound assignment

```
assign_stmt := assign_target ('+=' | '-=' | '*=' | '/=') expr term
```

Expands to `target = target <op> expr`. The target must be a
mutable lvalue. The operation must be defined on the target's type
(for `+=`, that includes string concatenation).

### §6.4 `if`

```
if_stmt := 'if' '(' expr ')' block ('else' block)?
```

The condition must be `bool`. `else` is optional. `else if` is
written as `else { if (...) { ... } }` (which is what the parser
produces).

`if` is a statement, not an expression; it does not produce a
value.

### §6.5 `while`

```
while_stmt := 'while' '(' expr ')' block
```

The condition must be `bool`. The body is re-evaluated until the
condition is false.

### §6.6 `for`

```
for_stmt := 'for' IDENT 'in' expr '..' expr block   // range
          | 'for' IDENT 'in' expr block              // iteration
```

**Range form.** Both bounds must be integers of the same type.
The loop variable takes values `start, start+1, ..., end-1`.
Both bounds are evaluated once at loop entry.

**Iteration form.** The iterable must be `[T]`, `rc<[T]>`, or
`string`. Iterating a string yields `u8` bytes, one per byte.

The loop variable is a fresh local scoped to the body. It is
immutable by default (the compiler's implementation marks it
mutable internally, but no user-facing assignment to it is
accepted).

### §6.7 `match`

```
match_stmt := 'match' expr '{' arm* '}'
arm        := pattern '=>' stmt (',' | ';')?
```

The scrutinee is evaluated once and cached in a hidden local. Each
arm's pattern is tested against it in source order. The first arm
whose pattern matches executes; the others are skipped.

Exhaustiveness is checked:

- For enum scrutinees: every variant must be covered by a variant
  pattern, or a wildcard/bind arm must be present.
- For `bool` scrutinees: both `true` and `false`, or a wildcard.
- For all other types: a wildcard or bind is required.

Subpatterns are not checked for exhaustiveness. A match with a
wildcard arm can still fail at runtime on a nested variant pattern;
the compiler emits a defensive trap after the last arm.

Arms may be separated by `,`, `;`, or nothing (ASI supplies the
terminator).

### §6.8 `return`

```
return_stmt := 'return' expr? term
```

`return` without a value is legal in a function whose return type
is `void` or unannotated with no value-returns.

The value's type must match the enclosing function's return type
(or widen to it). A function whose return type is not annotated
uses the first value-return to establish it.

Returning `ref<T>`, `*T`, `[T]`, `struct`, or `enum` is rejected
(§2.3).

Returning a borrowed RC value (a local, a field, an array element,
or a dereferenced value) is rejected with `cannot return a
borrowed value`. A fresh expression is required: concatenation,
allocation, or a function call whose return type is `rc<T>` or
`string`.

### §6.9 `break` and `continue`

Legal only inside a loop. `break` exits the innermost loop;
`continue` skips the rest of the body and re-tests the loop
condition (for `while` and `for..in`) or runs the increment (for
`for..range`). Scope unwinding is automatic: codegen emits enough
`OP_EXIT_SCOPE` (and RC drops) to leave any nested blocks between
the statement and the loop.

### §6.10 Blocks

A block is `{ stmt* }`. Blocks introduce a scope; locals declared
inside are not visible outside.

A block is a statement, so it can appear anywhere a statement can.
This is why `{ let x = 1; }` is legal as a nested scope.

---

## §7 — Patterns

```
pattern := '_'
         | IDENT
         | INT
         | 'true' | 'false'
         | STRING
         | IDENT '<' type_args '>' '.' IDENT ('(' pattern_list ')')?
         | IDENT '.' IDENT ('(' pattern_list ')')?
         | IDENT '(' pattern_list ')'                     // bare variant
```

### §7.1 Wildcard

`_` matches any value and binds nothing. It is lexed as an
identifier of length 1 whose source byte is `_`, and recognized as
a wildcard by the parser.

### §7.2 Bind

`x` matches any value and binds it to a local `x` of the
scrutinee's type. The binding is immutable and scoped to the arm's
body.

### §7.3 Literals

`0`, `-1`, `true`, `"hello"`. Integer patterns are range-checked
against the scrutinee's type. String patterns compare byte-wise.

### §7.4 Variant patterns

```
Type.Variant           // unit variant
Type.Variant(p, q)     // payload variant
Variant(p, q)          // bare form (type from scrutinee)
```

The type name is optional; when omitted, the scrutinee's enum type
supplies it. When written, the name must match the scrutinee's enum
(either exactly or, for generic enums, as a base-name prefix).

Subpatterns bind against the variant's payloads in order. The
subpattern count must match the payload count.

### §7.5 Nested patterns

`Outer.Wrap(Inner.Ok(v))` is a nested variant pattern. Each level
introduces temporary locals for the payloads and recurses.

### §7.6 Match arm bodies

An arm's body is a statement. A `{ ... }` block works; a bare
expression statement such as `print(n)` also works. The parser's
statement dispatcher decides which.

---

## §8 — Functions and methods

### §8.1 Function declaration

```
fn_decl := 'fn' method_name type_params? '(' params? ')' ret? block
method_name := IDENT
             | IDENT '.' IDENT
type_params := '<' IDENT (',' IDENT)* '>'
params := param (',' param)*
param := 'mut'? IDENT (':' type_expr)?
ret := ':' type_expr
```

The return type annotation is optional. Parameters without
annotations default to `i64`. Parameters with `mut` may be
reassigned inside the function body; without `mut`, they are
immutable.

Hard caps: 8 parameters, 8 type parameters.

### §8.2 Method declaration

A function whose name is `Type.name` is a method on the type
`Type`. The first parameter is the receiver by convention; its
type determines the receiver's class.

```
type Point = struct { x: i64, y: i64 }

fn Point.translate(p: Point, dx: i64, dy: i64): Point {
    return Point { x: p.x + dx, y: p.y + dy };
}

fn Point.is_origin(p: Point): bool {
    return p.x == 0 && p.y == 0;
}
```

A method's owner type must be a nominal type: a user struct or
enum. Builtin primitives have no method namespace today.

### §8.3 Method call

`recv.name(args)` dispatches on `recv`'s static type. There is no
vtable, no dynamic dispatch, no method resolution at runtime.

One level of `rc<T>` or `ref<T>` wrapping the receiver is
unwrapped before the method is looked up. `p.x` where `p` is a
`ref<Point>` is legal.

### §8.4 Extern methods

An `extern fn` can have a dotted name:

```
extern lib "libc.so.6" as c
extern fn String.len(s: string): u64 from c
```

The declaration's first parameter is the receiver. The call site
uses method syntax: `s.len()`.

### §8.5 Limitations

No overloading, no default arguments, no variadic user functions,
no `p.method` without parentheses.

---

## §9 — Generics

### §9.1 Declaration

```
fn identity<T>(x: T): T { return x }
type Box<T> = struct { value: T }
type Pair<A, B> = enum { Both(A, B), Neither }
```

Generic functions and types are registered as templates in the
monomorphizer. They are not themselves compiled; each instantiation
produces a concrete copy.

Hard caps: 8 type parameters per declaration.

### §9.2 Explicit instantiation

```
identity<i64>(5)             // generic function call
Box<i64> { value: 5 }        // generic struct literal
Option<i64>.Some(5)          // generic variant constructor
let b: Box<i64> = ...;       // generic type in an annotation
```

The type arguments must match the template's parameter count.

### §9.3 Instantiation caching

Each `(template, arguments)` pair is cached. Two calls to the same
template with equal arguments return the same concrete `TypeDef`
or function. `type_eq` (structural for compound types, nominal for
struct/enum) determines equality.

Cache overflow (`MAX_INST_CACHE = 256`) silently disables further
caching, which can produce duplicate `TypeDef`s with the same
mangled name. Nominal comparison by `tdef` pointer would treat
them as unequal.

### §9.4 Implicit inference

For a generic enum with one type parameter, a bare variant
constructor with exactly one payload can infer the type from the
payload:

```
let x: Option<i64> = Option.Some(5);   // explicit
let y = Option.Some(5);                // y: Option<i64>
```

The inference is limited to this case. It does not apply to
generic functions (which need explicit type arguments) or to
generic types with two or more parameters.

An unannotated `let` with a bare variant constructor whose type
cannot be inferred is an error: `cannot infer type arguments for
'Option'`.

### §9.5 Return-type context

A bare variant constructor with no local binding uses the enclosing
function's annotated return type as the type context:

```
fn find(): Option<i64> {
    if (true) { return Option.Some(1); }
    return Option.None;
}
```

The `Option.Some` and `Option.None` get their type arguments from
the return type annotation. Without the annotation, they'd be
errors.

### §9.6 Visibility

An instantiated generic type is `is_pub = true` unconditionally,
regardless of the template's visibility. This is why the prelude's
`Result` and `Option` (which are not marked `pub`) are usable from
any file.

The template itself is found by name across all files; there is no
file-scoped visibility for generic templates currently.

---

## §10 — Memory model

### §10.1 The two tiers

Prism has two memory tiers, both visible in the source as type
constructors:

- **The lexical arena** — allocated by `&e`, freed when the
  enclosing scope exits. Fast, no per-value bookkeeping.
- **The RC heap** — allocated by `rc e`, freed when the last
  reference is dropped. Slower, automatic, no scope constraint.

The choice between the two is made at the source level: `&e`
produces `ref<T>`; `rc e` produces `rc<T>`.

### §10.2 The escape rules

See §2.3. In summary: no nested references, no `ref<T>` or `*T`
return types, and no direct return of an aggregate.

### §10.3 RC insertion

A binding whose type is `rc<T>` or `string` owns a reference.
The reference is dropped when the binding goes out of scope, on
early `return`, on `break`/`continue` past the binding's scope,
and on `?` failure.

A binding from another RC binding shares the reference: an
`RC_INC` runs at the new binding's declaration, and both are
dropped at scope exit.

A function that returns `rc<T>` must return a fresh allocation.
Returning a borrowed value is rejected with `cannot return a
borrowed value`.

### §10.4 Arena lifetime

An arena value is valid until the enclosing scope exits. The
compiler inserts `OP_ENTER_SCOPE`/`OP_EXIT_SCOPE` around every
block, `for..range`, `for..in`, and `match`.

`&x` where `x` is a local does not allocate; it produces a pointer
to the local's stack slot. The pointer is valid until the frame
returns. A `ref<T>` to a local can be passed to a function that
takes `ref<T>`; it cannot be returned (rule 2 of §2.3).

### §10.5 `rc`'s literal restriction

For a compound type (array, struct, enum), `rc e` requires `e`
to be a literal form:

```
rc [1, 2, 3]
rc Point { x: 0, y: 0 }
rc Color.Red
rc Option.None
```

`rc variable` is rejected with `rc on a [T] variable is not yet
supported; use \`rc <literal>\` directly`. The reason is that a
general `rc variable` would need to copy the aggregate, which
codegen doesn't currently implement.

`rc scalar` (e.g. `rc 5`) allocates an 8-byte cell and copies the
scalar in.

---

## §11 — Error handling

### §11.1 `Result` and `Option`

Both are declared by the prelude:

```
type Result<T, E> = enum { Ok(T), Err(E) }
type Option<T>    = enum { Some(T), None }
```

Both are RC-managed. Every use site resolves them as `rc<...>`.

### §11.2 The `?` operator

Postfix `?` on a `Result` or `Option` expression:

```
let x = may_fail()?;
```

If the value is `Ok(v)` or `Some(v)`, the operator yields `v`. If
it is `Err(e)` or `None`, the operator returns the value from the
enclosing function.

Constraints:

- The enclosing function must have an annotated return type that
  resolves to `Result<_, _>` or `Option<_>`.
- If both the operand and the return type are `Result`, their
  error payload types must match.
- The operand must itself be `Result` or `Option`.

`?` cannot be used in a function returning an unannotated type or
an unrelated type.

### §11.3 Matching on Result / Option

`match` on a `Result` or `Option` is exhaustive if both variants
appear. A missing variant is a compile error.

```
match result {
    Result.Ok(v) => { print("ok: {}", v); }
    Result.Err(e) => { print("error: {}", e); }
}
```

### §11.4 The variant tag convention

`Ok` and `Some` have tag 0. `Err` and `None` have tag 1. This is
a hard contract: the `?` operator's runtime check tests tag 0 for
success. The prelude declares its variants in this order.

A user-defined enum has tags assigned in source order. A user
enum whose first variant is meant to be "success" and whose second
is meant to be "failure" would work with `?` if the enum's name
mangled to something starting with `Result` or `Option`. In
practice, `?` is only used with the prelude types.

---

## §12 — Modules

### §12.1 Imports

```
using std.io                     // whole-module import
using std.io as io2              // with alias
using std.io.{ print, println }  // selective import
using std.io.{ print as p }      // selective with alias
using Name = TypeExpr            // type alias
```

A dotted path `std.io` resolves to `std/io.manu` in the first
include directory that contains it.

### §12.2 Visibility

`pub` on a declaration exports it. Cross-file references to non-
`pub` items are errors.

`pub` applies to `fn`, `const`, `type`, and `extern`. It does not
apply to `using`.

### §12.3 Qualified calls

`module.function(args)` resolves to the function in the module's
file, checking visibility. The generated bytecode is a plain
`CALL` to the function's codegen index.

### §12.4 Selective imports

A selective import binds the source name (or the alias) locally:

```
using std.io.{ print as p }
p("hello");
```

Selective imports of functions and types are supported. Selective
imports of constants are not.

### §12.5 Type aliases

`using Name = TypeExpr` parses and is stored in the AST as
`AST_USING` with kind `USING_ALIAS`. It currently has no effect.
The type resolver does not consult alias declarations.

### §12.6 Cycles

A file that imports itself, directly or transitively, is rejected
at load time with `circular import involving '<path>'`.

### §12.7 The prelude

Every file implicitly imports the prelude. `Result` and `Option`
are in scope without a `using` declaration.

---

## §13 — FFI

### §13.1 Declaring a library

```
extern lib "ucrtbase.dll" as ucrt
extern lib "libc.so.6"                    // alias derived: "libc"
extern lib "/usr/lib/libm.so" as m
```

The path is passed verbatim to the OS loader (`dlopen` on Unix,
`LoadLibraryA` on Windows). The alias is what `from` clauses name.
If `as` is omitted, the alias is the basename up to the first `.`.

Hard cap: 16 libraries.

### §13.2 Declaring a function

```
extern fn strlen(s: *u8): u64 from ucrt
extern fn printf(fmt: *u8, ...): i32 from ucrt
```

Parameter and return types must be one of: `i8`–`i64`, `u8`–`u64`,
`f32`, `f64`, `bool`, `*T`, `void`, or `Option<*T>`. Everything
else is rejected with `not FFI-compatible`.

The return type annotation is required.

The `from` clause is required. The alias must have been declared
with `extern lib`.

Variadic: a trailing `...` marks the function variadic. Only the
last position is allowed. Variadic arguments must be integers,
`bool`, or pointers; floats are rejected.

Hard cap: 8 fixed parameters.

### §13.3 Declaring an extern struct

```
extern type Point = struct {
    x: f64,
    y: f64,
}

extern type SDL_Rect = struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
}
```

Fields are laid out using the C ABI for the host platform:
natural alignment per field, total size rounded up to the struct's
maximum alignment.

Allowed field types: integers, floats, `*T`, nested extern structs.

Rejected: `ref<T>`, `rc<T>`, `string`, `[T]`, opaque extern
structs by value, regular structs and enums.

### §13.4 Declaring an opaque extern type

```
extern type SDL_Window = opaque
extern type FILE = opaque
```

An opaque type is a name with no layout. It exists only as the
pointee of `*T`. Its size is unknown; `sizeof(Opaque)` is an error,
`sizeof(*Opaque)` is 8.

Field access on an opaque type is an error. Dereferencing `*Opaque`
is an error.

### §13.5 `Option<*T>` — nullable pointers

C APIs use `NULL` for "no value". Prism models nullable pointers
with `Option<*T>` and marshals automatically.

```
extern type FILE = opaque
extern fn fopen(path: *u8, mode: *u8): Option<*FILE> from c
extern fn fclose(f: *FILE): i32 from c
```

On the call side, `Option.Some(p)` marshals to `p`; `Option.None`
marshals to `NULL`. On the return side, a NULL C return marshals
to `Option.None`; a non-NULL return to `Option.Some(p)`.

`Option<*T>` is only recognized as a *whole* parameter or return
type. `Option<*T>` nested inside another type is not FFI-compatible.

### §13.6 Calling

```
let n = strlen(c"hello");              // n = 5
puts(c"hello");
let r = fclose(f);
```

Extern functions are called like any other function. Method
syntax works if the declaration uses a dotted name.

### §13.7 Raw pointer operations

The language has no `null` literal and no pointer arithmetic
operators. Raw memory is manipulated through builtins:

| Builtin | Signature | Purpose |
|---|---|---|
| `raw_null()` | `() -> *void` | Null pointer |
| `raw_is_null(p)` | `(*T) -> bool` | Test for null |
| `raw_offset(p, n)` | `(*T, i64) -> *T` | Advance by `n` elements |
| `raw_load_u8(p, i)` | `(*u8, i64) -> u8` | Read a byte |
| `raw_store_u8(p, i, v)` | `(*u8, i64, u8) -> void` | Write a byte |

Pointer *comparison* (`p == q`, `p != q`) is allowed as an
ordinary operator.

### §13.8 The `ffi_*` builtins

| Builtin | Signature | Purpose |
|---|---|---|
| `ffi_cstring(s)` | `(string) -> *u8` | Prism string → NUL-terminated C string in the arena |
| `ffi_string(p)` | `(*u8) -> string` | NUL-terminated bytes → Prism string; NULL yields empty |
| `ffi_error()` | `() -> string` | Last `dlerror` / `GetLastError` |
| `ffi_errno()` | `() -> i64` | Last captured `errno` |
| `ffi_alloc(size)` | `(u64) -> *void` | Zeroed C heap allocation |
| `ffi_free(p)` | `(*void) -> void` | Free a pointer from `ffi_alloc` |

### §13.9 Limitations

- No struct-by-value marshalling.
- No callbacks (Prism functions passed to C).
- No `Result<*T, E>` returns from extern functions.
- No variadic float arguments.
- No `dlclose`; libraries live for the process.
- A crashing C function crashes the VM. No signal handling.

---

## §14 — Builtins

### §14.1 `print` and `format`

Both accept variadic arguments. Three compile-time modes:

- **Empty** — `print()` emits a newline.
- **Multi-argument** — `print(a, b, c)` emits each value, space-
  separated, then a newline.
- **Format** — `print("x = {}", n)` uses `{}` as placeholders. `{{`
  and `}}` are literal braces. The placeholder count must match
  the argument count minus one.

`format` produces a `string` and does not write to stdout.

Values print as:

- Integers — decimal.
- Floats — `%g` format.
- Booleans — `true` or `false`.
- Strings — their bytes.
- Arrays — `[a, b, c]`.
- Structs — `{a, b, c}`.
- Enums — the variant name.
- Pointers — their numeric value.

### §14.2 `len`

`len(x)` returns the length of `x` in elements (arrays) or bytes
(strings). `x` must be `string`, `[T]`, or `rc<[T]>`.

### §14.3 `sizeof`

`sizeof(T)` returns the size of `T` in bytes as a `u64`. Rejected
for `void` and for opaque types.

### §14.4 Raw pointer and FFI builtins

See §13.7 and §13.8.

### §14.5 `args`, `read_line`, `read_all`

- `args()` returns `rc<[string]>`, the program's command-line
  arguments.
- `read_line()` reads one line from stdin, returns
  `Option<string>`. `None` on EOF with no bytes read.
- `read_all()` reads all of stdin, returns `string`.

---

## §15 — Grammar

EBNF, describing the parser as implemented.

```
program         := decl* EOF
decl            := fn_decl | const_decl | type_decl
                 | using_decl | extern_decl | pub_decl

fn_decl         := 'fn' method_name type_params? '(' params? ')' ret? block
method_name     := IDENT | IDENT '.' IDENT
type_params     := '<' IDENT (',' IDENT)* '>'
params          := param (',' param)*
param           := 'mut'? IDENT (':' type_expr)?
ret             := ':' type_expr

const_decl      := 'const' IDENT (':' type_expr)? '=' expr term

type_decl       := 'type' IDENT type_params? '=' struct_body
                 | 'type' IDENT type_params? '=' enum_body
struct_body     := 'struct' '{' (field (',' field)* ','?)? '}'
field           := IDENT ':' type_expr
enum_body       := 'enum' '{' (variant (',' variant)* ','?)? '}'
variant         := IDENT ('(' type_expr (',' type_expr)* ')')?

using_decl      := 'using' using_module
                 | 'using' using_selective
                 | 'using' IDENT '=' type_expr
using_module    := path ('as' IDENT)?
using_selective := path '.' '{' using_name (',' using_name)* ','? '}'
using_name      := IDENT ('as' IDENT)?
path            := IDENT ('.' IDENT)*

extern_decl     := 'extern' 'lib' STRING ('as' IDENT)? term
                 | 'extern' 'fn' IDENT '(' extern_params? ')' ':'
                              type_expr 'from' IDENT term
                 | 'extern' 'type' IDENT '=' 'struct' struct_body
                 | 'extern' 'type' IDENT '=' 'union' struct_body
                 | 'extern' 'type' IDENT '=' 'opaque'
extern_params   := extern_param (',' extern_param)* (',' '...')?
extern_param    := 'mut'? IDENT (':' type_expr)?

pub_decl        := 'pub' fn_decl
                 | 'pub' const_decl
                 | 'pub' type_decl
                 | 'pub' extern_decl

block           := '{' stmt* '}'
stmt            := let_stmt | if_stmt | while_stmt | for_stmt
                 | match_stmt | break_stmt | continue_stmt
                 | return_stmt | assign_stmt | expr_stmt | block

let_stmt        := 'let' 'mut'? IDENT (':' type_expr)? '=' expr term
if_stmt         := 'if' '(' expr ')' block ('else' block)?
while_stmt      := 'while' '(' expr ')' block
for_stmt        := 'for' IDENT 'in' expr '..' expr block
                 | 'for' IDENT 'in' expr block
match_stmt      := 'match' expr '{' match_arm* '}'
match_arm       := pattern '=>' stmt (',' | ';')?
break_stmt      := 'break' term
continue_stmt   := 'continue' term
return_stmt     := 'return' expr? term
assign_stmt     := assign_target ('=' | '+=' | '-=' | '*=' | '/=') expr term
assign_target   := IDENT | expr '.' IDENT | expr '[' expr ']' | '*' unary
expr_stmt       := expr term

term            := ';' | '}' | EOF

expr            := or_expr
or_expr         := and_expr ('||' and_expr)*
and_expr        := eq_expr ('&&' eq_expr)*
eq_expr         := cmp_expr (('==' | '!=') cmp_expr)*
cmp_expr        := add_expr (('<' | '<=' | '>' | '>=') add_expr)*
add_expr        := mul_expr (('+' | '-') mul_expr)*
mul_expr        := cast_expr (('*' | '/') cast_expr)*
cast_expr       := unary ('as' type_expr)*
unary           := ('-' | '!' | '*' | '&' 'mut'? | 'rc') unary
                 | postfix
postfix         := primary postfix_op*
postfix_op      := '[' expr ']'
                 | '.' IDENT '(' args? ')'
                 | '.' IDENT
                 | '?'

primary         := INT | FLOAT | STRING | CSTRING
                 | 'true' | 'false'
                 | 'sizeof' '(' type_expr ')'
                 | IDENT generic_args? '(' args? ')'
                 | IDENT generic_args? '.' IDENT ('(' args? ')')?
                 | IDENT generic_args? struct_lit
                 | IDENT
                 | '(' expr ')'
                 | '[' expr (',' expr)* ','? ']'

generic_args    := '<' type_expr (',' type_expr)* '>'
struct_lit      := '{' (IDENT ':' expr) (',' IDENT ':' expr)* ','? '}'
args            := expr (',' expr)*

type_expr       := '*' type_expr
                 | 'rc' '<' type_expr '>'
                 | 'ref' '<' 'mut'? type_expr '>'
                 | '[' type_expr ']'
                 | 'fn' '(' (type_expr (',' type_expr)*)? ')' ':' type_expr
                 | qualified_type
qualified_type  := IDENT ('<' type_expr (',' type_expr)* '>')?
                 | IDENT '.' IDENT

pattern         := '_'
                 | IDENT
                 | INT
                 | 'true' | 'false'
                 | STRING
                 | IDENT generic_args? '.' IDENT ('(' pattern_list ')')?
                 | IDENT '.' IDENT ('(' pattern_list ')')?
                 | IDENT '(' pattern_list ')'
pattern_list    := pattern (',' pattern)*
```

Type paths with more than two components (`a.b.C`) parse but fail
at sema with `type paths with more than two components are not
yet supported`.

---

## §16 — Diagnostics

Every diagnostic the compiler can emit is documented in
`errors.md`, with the trigger, the phase, and an example. This
section is a pointer.

---

## §17 — Not specified

A few behaviors are emergent rather than specified. They are
documented here for the record; they are not language guarantees.

- **Evaluation order of arguments.** Left-to-right in the current
  implementation. Not tested.
- **Order of side effects in multi-argument `print`.** Left-to-right.
- **Recursion depth limits.** Bounded by `CALL_STACK_MAX = 256`.
  A program that recurses more deeply traps.
- **The `arena high-water = N bytes` line** printed at end of
  `manu run` is diagnostic output, not a language feature.
- **The memory layout of `struct` values.** Field offsets are
  computed by sema and are 8-byte aligned. The layout is not
  user-visible; the source refers to fields by name.
- **The concrete representation of `enum` values.** A `[tag]
  [payloads…]` block. Not user-visible; the source pattern-matches
  on variants.

A future implementation could change any of these without breaking
source compatibility.
