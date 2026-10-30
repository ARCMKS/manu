// ============================================================
// lexer.h — Zero-allocation tokenizer.
//
// The lexer produces a stream of Token values, each carrying a
// TokenKind plus a Span into a source file identified by file_id.
// It never allocates: a Token is a (kind, span) pair, and the
// source text is referred to by byte range, not copied. That is
// why the lexer needs a SourceMap-like owner of the source bytes
// and a file_id to tag each span with.
//
// The lexer does NOT track line/column. It only knows byte
// offsets into the file. Diagnostics code is responsible for
// turning a span into a human-readable line:col via the
// SourceMap. Keeping the lexer ignorant of lines is what keeps
// it fast and allocation-free.
//
// Automatic semicolon insertion (ASI) is handled here, not in
// the parser: the lexer synthesizes TOK_SEMI tokens at newlines
// when the previous significant token is a "statement-ending"
// kind. See is_asi_terminator in lexer.c for the exact set.
// ============================================================
#ifndef PRISM_LEXER_H
#define PRISM_LEXER_H

#include "common.h"
#include <stdint.h>

// All token kinds the lexer can emit.
//
// Groups:
//   - Literals:  INT, FLOAT, IDENT, STRING (Prism string),
//     CSTRING (the `c"..."` form used for FFI). STRING and
//     CSTRING share a byte-range span; the parser decides what
//     to do with the payload based on kind.
//   - Keywords:  one TokenKind per reserved word. The keyword
//     table in lexer.c maps an identifier's bytes to one of
//     these; anything not in the table becomes TOK_IDENT.
//   - Punctuation / operators: one TokenKind per distinct
//     lexeme. Multi-char operators (==, !=, <=, >=, ->, =>, ..,
//     ...) are single tokens; the lexer never emits a "partial"
//     form that the parser must re-join.
//   - EXTERN, FROM are listed at the bottom with the rest of
//     the "language feature" keywords for historical reasons;
//     the enum ordering has no semantic meaning beyond that
//     TOK_EOF and TOK_ERROR are last.
//   - TOK_EOF:   emitted exactly once at end of input, then
//     repeatedly if lexer_next is called again (the lexer does
//     not "stick" at EOF; each call re-checks at_end).
//   - TOK_ERROR: emitted for an unterminated string literal
//     (closing quote missing before newline or EOF) or an
//     unexpected character. It carries a span just like any
//     other token, so diagnostics can point at the bad bytes.
//     The lexer does not report an error itself; it just hands
//     back TOK_ERROR and the caller decides what to do.
typedef enum {
    // Literals
    TOK_INT, TOK_FLOAT, TOK_IDENT,TOK_STRING, TOK_CSTRING,
    // Keywords
    TOK_FN, TOK_LET, TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR, TOK_IN,TOK_RETURN, TOK_AS,
    TOK_TRUE, TOK_FALSE, TOK_REF, TOK_RC, TOK_MUT,TOK_CONST, TOK_BREAK, TOK_CONTINUE,
    TOK_TYPE, TOK_STRUCT, TOK_ENUM, TOK_MATCH, TOK_USING, TOK_PUB,
    // Punctuation
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
    TOK_SEMI, TOK_COLON, TOK_COMMA, TOK_QUESTION,
    TOK_PLUS, TOK_MINUS, TOK_ARROW,TOK_FATARROW, TOK_STAR, TOK_SLASH,
    TOK_AMP, TOK_DOT, TOK_DOTDOT, TOK_ELLIPSIS,
    TOK_EQEQ, TOK_BANGEQ,
    TOK_LT, TOK_LE, TOK_GT, TOK_GE,
    TOK_BANG,
    TOK_ASSIGN,
    TOK_AMPAMP,          // &&
    TOK_PIPEPIPE,        // ||
    TOK_PLUS_ASSIGN,     // +=
    TOK_MINUS_ASSIGN,    // -=
    TOK_STAR_ASSIGN,     // *=
    TOK_SLASH_ASSIGN,    // /=

    TOK_EXTERN, TOK_FROM,
    // End
    TOK_EOF, TOK_ERROR,
} TokenKind;

// A lexed token. Deliberately tiny (16 bytes on a 64-bit host)
// so that the parser can hold tokens by value without worrying
// about cost. The span points into the original source; nothing
// here owns any heap.
//
// Invariants:
//   - For every non-ERROR, non-EOF token, span.start < span.end
//     and the bytes [span.start, span.end) in the file named by
//     span.file_id are exactly the token's lexeme.
//   - For a synthesized TOK_SEMI (ASI), span.start == span.end;
//     the span is a zero-width point at the position of the
//     first newline crossed. This is the only zero-width token
//     the lexer emits.
//   - For TOK_EOF, span.start == span.end == offset of the
//     terminating NUL byte.
//   - For TOK_ERROR, the span covers whatever the lexer consumed
//     before bailing; it may be empty or cover a partial lexeme.
typedef struct {
    TokenKind kind;
    Span span;
} Token;

// Lexer state. The struct is trivially copyable, which is what
// makes lexer_peek (in lexer.c) work: it copies the whole Lexer,
// runs lexer_next on the copy, and throws the copy away. Any
// future field added here must preserve that property (no
// pointers-to-self, no owning allocations).
//
// Fields:
//   source             — the whole file as a NUL-terminated C
//                        string. The lexer never writes to it.
//   cur                — the read cursor: a pointer into `source`
//                        at the next byte to consume. The lexer
//                        advances this; the byte at *cur is the
//                        "current" character for peek().
//   file_id            — stamped into every Span the lexer
//                        emits. Must match the id the SourceMap
//                        assigned to this file (see smap_add).
//   token_start_offset — byte offset of the first byte of the
//                        token currently being scanned, i.e.
//                        cur - source at the moment we begin a
//                        token. Reset at the start of every
//                        lexer_next call after ASI has run.
//   prev_significant   — the kind of the last token lexer_next
//                        returned (including synthesized
//                        TOK_SEMI tokens; excluded nothing).
//                        Used only by the ASI check.
typedef struct {
    const char* source;
    const char* cur;
    uint32_t file_id;
    // where current token began
    uint32_t token_start_offset;
    // for automatic semicolon insertion(ASI)
    TokenKind prev_significant;
} Lexer;

// Initialize a lexer over `source`. `source` must be NUL-
// terminated and must outlive the lexer. `file_id` is the id
// the source was registered under in the SourceMap; it is
// copied verbatim into every emitted Span.
void  lexer_init(Lexer* lex, const char* source, uint32_t file_id);

// Consume and return the next token. Returns TOK_ERROR on bad
// input (unterminated string, unrecognized character). Never
// returns NULL; always returns a Token by value. Calling this
// after TOK_EOF has been returned continues to return TOK_EOF
// (the lexer is idempotent at end of input).
Token lexer_next(Lexer* lex);      // returns TOK_ERROR on bad input

// Non-destructive lookahead: returns what lexer_next would
// return without advancing the lexer. Implemented by copying
// the Lexer struct and running lexer_next on the copy. Because
// Lexer contains no owning state, the copy is safe to discard.
Token lexer_peek(Lexer* lex);

// Human-readable name for a TokenKind, used in error messages
// and disassembly. For keyword and operator kinds this is the
// lexeme itself ("fn", "=="); for classes it is a category name
// ("int", "ident", "float"). Never returns NULL; unknown kinds
// yield "?".
const char* token_kind_name(TokenKind k);

// Same as lexer_next but no diagnostic; useful inside tests.

#endif