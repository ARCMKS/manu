## `docs/arch/files/04-lexer.md`

# `include/lexer.h`, `src/lexer.c`

*Current as of commit `<hash>`, <date>.*

---

## §1 — Purpose

The lexer is a zero-allocation scanner. It reads source bytes and
produces `Token` values, each carrying a `TokenKind` and a `Span`.
It is the boundary between raw source and the parser.

**What it solves.** Classifying bytes into tokens; automatic
semicolon insertion; recognizing keywords, literals, and operators.
It does not track line or column (the `SourceMap` does that); it
does not resolve names; it does not interpret escapes.

**What it does not solve.** Strings with escaped quotes; Unicode
identifiers; block comments; `&&` and `||` at the lexer level (they
are lexed, but only as one token each; see §6); numeric bases
beyond decimal, hex, and binary; exponent floats.

**Who depends on it.** The parser. Nothing else reads source bytes
forward for classification.

**Zero-allocation:** a `Token` is a 24-byte value produced on the
stack. The parser never stores a `Token` as an owned object; it
copies the value into AST nodes and the AST holds only the span.
Raw bytes are recovered on demand from the `SourceMap` via
`smap_span_text`.

---

## §2 — Token vocabulary

```c
typedef enum {
    // Literals
    TOK_INT, TOK_FLOAT, TOK_IDENT, TOK_STRING, TOK_CSTRING,
    // Keywords
    TOK_FN, TOK_LET, TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR, TOK_IN,
    TOK_RETURN, TOK_AS,
    TOK_TRUE, TOK_FALSE, TOK_REF, TOK_RC, TOK_MUT, TOK_CONST,
    TOK_BREAK, TOK_CONTINUE,
    TOK_TYPE, TOK_STRUCT, TOK_ENUM, TOK_MATCH, TOK_USING, TOK_PUB,
    // Punctuation
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_SEMI, TOK_COLON, TOK_COMMA, TOK_QUESTION,
    TOK_PLUS, TOK_MINUS, TOK_ARROW, TOK_FATARROW, TOK_STAR, TOK_SLASH,
    TOK_AMP, TOK_DOT, TOK_DOTDOT, TOK_ELLIPSIS,
    TOK_EQEQ, TOK_BANGEQ,
    TOK_LT, TOK_LE, TOK_GT, TOK_GE,
    TOK_BANG,
    TOK_ASSIGN,
    TOK_AMPAMP, TOK_PIPEPIPE,
    TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN, TOK_STAR_ASSIGN, TOK_SLASH_ASSIGN,
    TOK_EXTERN, TOK_FROM,
    // End
    TOK_EOF, TOK_ERROR
} TokenKind;
```

Fifty-eight values, grouped by category. `TOK_EOF` is returned
once per source and thereafter is stable. `TOK_ERROR` is returned
for characters the lexer cannot classify (unterminated string,
unrecognized byte). The parser filters `TOK_ERROR` in `advance`.

**Dead tokens.** `TOK_EQ` (the bare `=`) is declared in earlier
versions but not in the current `TokenKind`. The compiler emits
`TOK_ASSIGN` for `=`. `TOK_ARROW` (`->`) is emitted but the
parser never consumes it.

**`TOK_ELLIPSIS`** is used only in `extern fn` parameter lists.

**`TOK_QUESTION`** is the Result/Option propagation operator.

---

## §3 — Token and scanner state

```c
typedef struct {
    TokenKind kind;
    Span      span;
} Token;

typedef struct {
    const char* source;
    const char* cur;
    uint32_t    file_id;
    uint32_t    token_start_offset;
    TokenKind   prev_significant;
} Lexer;
```

**`Token`.** A 24-byte value. For most tokens, `span.end` is the
byte after the last character of the token. For `TOK_SEMI` emitted
by ASI, `span.start == span.end` and both point at the newline's
offset (or the file end, for the synthetic semicolon after the
last token).

**`Lexer`.** The scanner state.

- `source` — base pointer, never advanced.
- `cur` — the read cursor. Advances over every byte the lexer
  consumes, including whitespace and comments.
- `file_id` — the id assigned by `smap_add`. Copied into every
  span.
- `token_start_offset` — the byte offset of the current token's
  first byte.
- `prev_significant` — the kind of the last non-`TOK_ERROR` token.
  Used exclusively by ASI. Initialized to `TOK_EOF` so the first
  token of a file never gets a synthetic semicolon before it.

`lexer_peek` copies the entire `Lexer` struct, calls `lexer_next`
on the copy, and discards the copy.

---

## §4 — Public interface

```c
void  lexer_init(Lexer* lex, const char* source, uint32_t file_id);
Token lexer_next(Lexer* lex);
Token lexer_peek(Lexer* lex);
const char* token_kind_name(TokenKind k);
```

**`lexer_init`.** Initializes the scanner. The source buffer is
borrowed; the lexer never frees it. The `file_id` must already be
registered with the map.

**`lexer_next`.** Returns the next token, advancing the scanner.
Returns `TOK_EOF` once and then again on every subsequent call.
Returns `TOK_ERROR` on malformed input. May emit a synthetic
`TOK_SEMI` at a newline if ASI conditions are met.

**`lexer_peek`.** Returns the next token without advancing.
Implemented by copying the `Lexer` struct.

**`token_kind_name`.** Returns a short, human-readable name for a
`TokenKind`. Used in parser diagnostics.

---

## §5 — Implementation notes

### §5.1 The five cursor primitives

Every higher-level routine is built on these:

```c
static bool  at_end(Lexer* l);    // *cur == '\0'
static char  peek(Lexer* l);      // *cur
static char  peek2(Lexer* l);     // cur[0] ? cur[1] : '\0'
static char  advance(Lexer* l);   // *cur++
static bool  match(Lexer* l, char c);  // peek-and-advance-if-equal
```

The source buffer is NUL-terminated. The NUL is the end-of-input
sentinel; it is never a valid part of any token. `peek` and
`peek2` return `'\0'` past the end so callers do not need a
separate bounds check. `advance` does not bounds-check; the caller
is responsible for ensuring a character is available.

The `peek2` guard (`l->cur[0] ? l->cur[1] : '\0'`) avoids reading
one byte past the NUL, which would be technically UB.

### §5.2 Identifier classification

```c
static bool is_ident_start(char c);  // isalpha(c) || c == '_'
static bool is_ident_cont(char c);   // isalnum(c) || c == '_'
```

Both wrap the C `<ctype.h>` functions. The casts to `unsigned char`
are mandatory because the `isalpha`/`isalnum` functions have
undefined behavior for negative `char` values on platforms where
`char` is signed.

### §5.3 Whitespace, comments, and ASI preparation

`skip_ws_and_comments` consumes whitespace (space, tab, CR),
`//` comments, and newlines. It returns true if at least one
newline was crossed, and writes the offset of the *first* newline
to a caller-supplied out parameter.

The newline offset is captured before the `advance`, so it points
at the `\n` byte itself. That is the offset a synthetic
`TOK_SEMI` receives.

Comment handling: `//` to end of line. The comment consumes
everything up to but not including the newline; the newline is
seen by the outer loop and counted for ASI.

### §5.4 Keyword classification

```c
static TokenKind keyword_or_ident(const char* s, int n);
```

A linear scan of 26 keywords via a `KW` macro. Length check first,
then `memcmp`. A two-character identifier fails most of the 26
tests on the length check alone.

### §5.5 ASI terminator set

```c
static bool is_asi_terminator(TokenKind k);
```

Ten kinds: `TOK_IDENT`, `TOK_INT`, `TOK_FLOAT`, `TOK_STRING`,
`TOK_TRUE`, `TOK_FALSE`, `TOK_RPAREN`, `TOK_RBRACE`,
`TOK_RBRACKET`, `TOK_RETURN`.

The set is "tokens that can legitimately end a statement".
Including `TOK_RBRACE` is the deliberate divergence from Go's
rule; in Prism, `}` ends a block statement or a match arm body,
and a semicolon after it is redundant but harmless.

`TOK_RETURN` is included for the bare-return form `return\n`.

**Not terminators:** `TOK_BREAK`, `TOK_CONTINUE` (the parser calls
`end_statement` after them), all operators, all opening
delimiters.

### §5.6 The main scanner

`lexer_next` is a single function. Its flow:

1. Skip whitespace and comments, capturing whether and where a
   newline was crossed.
2. **ASI check.** If a newline was crossed or the input is
   exhausted, and `prev_significant` is a terminator, emit a
   synthetic semicolon. The offset is the recorded first-newline
   position if a newline was crossed, otherwise the current
   cursor. Update `prev_significant` to `TOK_SEMI` so a second
   consecutive call cannot emit two semicolons.
3. Record `token_start_offset`.
4. **End of input.** Return `TOK_EOF`, updating
   `prev_significant`.
5. **Dispatch on the first character.** Consume it and branch:
   - `c"` — C string literal (two-byte lookahead).
   - Identifier start — identifier or keyword.
   - Digit — integer or float.
   - `"` — string literal.
   - Punctuation switch for operators and delimiters.

`prev_significant` is updated on every return path, including the
ASI path and the `TOK_EOF` path.

### §5.7 Number scanning

**Integer:** a run of digits. Hex and binary prefixes (`0x`, `0b`)
are recognized and the loop consumes the appropriate digits.

**Float:** a run of digits with at most one `.`, and the `.` is
only consumed if the following character is a digit. This
distinguishes `1.5` (float) from `0..5` (int, dotdot, int).

No exponent notation. No leading sign (unary `-` is parsed by the
parser). No underscores in literals.

### §5.8 String scanning

A string ends at the first `"` on the same line. A newline before
the closing quote produces `TOK_ERROR`.

Escape sequences are not resolved by the lexer. The scan for the
closing quote does not account for `\"`, so a string literal
containing an escaped double quote cannot be lexed. This is a
known limitation (see §6).

### §5.9 Punctuation

A single `switch` on the first character. Multi-character tokens
use `match` for the second character:

- `.` — `...` (three bytes), `..` (two bytes), or `.`
- `-` — `->` or `-=` or `-`
- `=` — `==` or `=>` or `=`
- `!` — `!=` or `!`
- `<` — `<=` or `<`
- `>` — `>=` or `>`
- `&` — `&&` or `&`
- `|` — `||` or `TOK_ERROR`
- `+` — `+=` or `+`
- `*` — `*=` or `*`
- `/` — `/=`, or the start of a comment (handled earlier), or `/`

Unrecognized bytes produce `TOK_ERROR`.

---

## §6 — Invariants

- Every token the lexer returns carries a span whose `file_id`
  equals the one passed to `lexer_init`.
- For non-ASI tokens, `span.start < span.end` and both offsets lie
  within `[0, strlen(source)]`.
- For an ASI-emitted `TOK_SEMI`, `span.start == span.end` and the
  offset points at the first newline crossed since the previous
  token, or at the end of the file if `TOK_EOF` is reached with a
  terminator as `prev_significant`.
- `prev_significant` never holds `TOK_ERROR` after a `lexer_next`
  call returns, except in the unterminated-string case (where the
  error is the returned token).
- `cur >= source` and `cur <= source + strlen(source) + 1`.

---

## §7 — Known limitations

**Strings cannot contain unescaped quotes.** The scan for the
closing quote does not account for `\"`. A proper fix would make
the scan escape-aware.

**No `&&` or `||` at the AST level.** Both are lexed as single
tokens (`TOK_AMPAMP`, `TOK_PIPEPIPE`) but the parser does not
currently produce binary nodes for them. The parser has no
precedence level for these operators; `a && b` is a parse error.

**No numeric bases beyond decimal, hex, binary.** No octal. No
exponent floats. No underscores in numeric literals.

**No Unicode identifiers or strings.** Classification uses
`isalpha`/`isalnum` in the C locale, which classify only ASCII
letters. A UTF-8 source file produces `TOK_ERROR` on the first
multi-byte character.

**`TOK_ARROW` is lexed but unused.** A reader might expect `->` to
mean something.

**`peek2` is asymmetric with `advance`.** `peek2` explicitly guards
against reading past NUL; `advance` does not. Safe because every
`advance` is preceded by a peek that returned a non-NUL character.

**Keyword lookup is linear.** Twenty-six `memcmp`s per identifier.
Not a hot path.

**No source-position bounds checking.** The lexer relies on the
NUL terminator. A buffer without a NUL would read past the end.

**`lexer_peek` copies the entire Lexer struct.** Cheap (40 bytes)
but not free. Used sparingly.

---

## §8 — Extension points

- **Escape-aware string scanning.** A self-contained change to
  the string branch.
- **`TOK_CHAR` for character literals.** Would need a new lexer
  branch and a new token kind.
- **Numeric bases beyond hex and binary.** The number branch
  would need extension; codegen's integer parsing would need to
  handle the new prefixes.
- **An explicit `Lexer.end` pointer.** Would remove the reliance
  on the NUL terminator and allow binary input.
- **An error token carrying a reason.** Currently `TOK_ERROR` has
  no payload; the parser produces a generic "unexpected character"
  for every error token. A variant or an error field would let the
  parser produce better diagnostics.
- **Lexer-driven diagnostics.** The lexer could report errors
  directly into a `DiagnosticBag` rather than producing
  `TOK_ERROR` tokens. Not currently done; the lexer has no
  reference to a bag.
- **`lexer_peek2`** for two-token lookahead. Not needed today.
