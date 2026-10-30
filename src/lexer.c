// ============================================================
// lexer.c — Zero-allocation scanner producing Span-tagged tokens.
//
// The lexer no longer tracks line or column. It emits byte-range
// spans into the file identified by `file_id`; a SourceMap is the
// only authority on what those offsets mean.
//
// Reading order for this file:
//   1. The five tiny cursor primitives at the top (at_end, peek,
//      peek2, advance, match). Everything else is built on them.
//   2. span_from / make, the two constructors that turn the
//      current cursor position into a Token.
//   3. skip_ws_and_comments — the only function that crosses
//      newlines, and therefore the only place ASI can fire.
//   4. keyword_or_ident — the identifier-to-keyword lookup.
//   5. is_asi_terminator — the "can a `;` be inserted after this
//      token?" predicate.
//   6. lexer_next — the main dispatch loop.
//   7. lexer_peek — a one-line wrapper.
//   8. token_kind_name — pure lookup, no lexer state.
//
// The whole file is allocation-free and re-entrant per Lexer
// instance; no globals, no static mutable state.
// ============================================================
#include "../include/lexer.h"
#include <stdint.h>
#include <string.h>
#include <ctype.h>

// --- Cursor primitives ---------------------------------------
//
// These five functions are the only things that read or write
// l->cur. Every higher-level routine below is expressed in terms
// of them, which is why the byte-level invariants of the lexer
// can be checked in one place.
//
// Conventions:
//   - The source buffer is NUL-terminated. '\0' is the end
//     sentinel; it is never a valid part of any token.
//   - peek/peek2 return '\0' past the end so callers do not need
//     a separate at_end check before inspecting the next byte.
//     This is what lets is_ident_cont(peek(l)) be safe without a
//     guard: if we are at EOF, peek returns '\0' and the loop
//     terminates.
//   - advance does NOT bounds-check. It relies on the NUL
//     sentinel: callers must ensure they only advance past bytes
//     that exist. Every call site below either just checked
//     at_end or consumed a byte that was already peeked.

// True when the cursor is sitting on the terminating NUL. This
// is the lexer's only notion of "end of input"; it never counts
// bytes against a stored length, because the NUL terminator is
// what the caller contract in lexer.h promises.
static bool  at_end(Lexer* l)   { return *l->cur == '\0'; }
// Current byte without consuming it. Returns '\0' at EOF.
static char  peek(Lexer* l)     { return *l->cur; }
// The byte after the current one, without consuming either.
// Written as `l->cur[0] ? l->cur[1] : '\0'` rather than the
// tempting `l->cur[1]` alone: at EOF, cur[1] is one past the
// terminator and reading it is technically UB. The guard makes
// peek2 return '\0' whenever we are already at end, so callers
// like the `//` comment check can safely ask "is the byte after
// the current one another slash?" even at the very end.
static char  peek2(Lexer* l)    { return l->cur[0] ? l->cur[1] : '\0'; }
// Consume one byte and return it. The post-increment is what
// advances the cursor; callers rely on this both for its return
// value and for its side effect.
static char  advance(Lexer* l)  { return *l->cur++; }
// Conditional consume: if the next byte is `c`, consume it and
// return true; otherwise leave the cursor alone and return
// false. This is the workhorse for two-character operators
// (==, !=, <=, >=, ->, =>) and for the `.` vs `..` decision.
static bool  match(Lexer* l, char c) { if (peek(l) == c) { advance(l); return true; } return false; }

// Identifier character classes. isalpha/isalnum are invoked with
// an unsigned-char cast so that a byte with the high bit set
// (e.g. UTF-8 continuation bytes in a stray non-ASCII identifier)
// does not index into the ctype table with a negative value,
// which is UB on platforms where char is signed. Prism
// identifiers are ASCII-only in practice; the cast is a
// defensive measure, not a feature.
static bool is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool is_ident_cont(char c)  { return isalnum((unsigned char)c) || c == '_'; }



// Initialize a lexer. `source` is borrowed for the lifetime of
// the lexer; the caller (typically main.c) owns the buffer and
// must keep it alive. `file_id` is stamped into every Span the
// lexer produces; it is the id returned by smap_add for this
// same buffer, so spans can be resolved by smap_decode later.
//
// token_start_offset is set to 0 because the first real token
// will set it in lexer_next; leaving it zeroed avoids reading
// uninitialized state if someone calls span_from before the
// first lexer_next (they shouldn't, but the cost is zero).
//
// prev_significant is seeded with TOK_EOF for a specific reason:
// the ASI check fires when prev_significant is a statement
// terminator, and TOK_EOF is *not* in that set. So the very
// first token of the file can never be preceded by a synthetic
// `;` even if the file begins with blank lines. Seeding with
// TOK_IDENT (say) would insert a spurious TOK_SEMI at the top
// of every file that starts with a newline.
void lexer_init(Lexer* lex, const char* source, uint32_t file_id) {
    lex->source             = source;
    lex->cur                = source;
    lex->file_id            = file_id;
    lex->token_start_offset = 0;
    // TOK_EOF is not a terminator, so the first token of the file
    // can never be preceded by a synthetic semicolon
    lex->prev_significant = TOK_EOF;
}

// Build a Span covering [token_start_offset, cur). Called only
// after the token body has been consumed (so cur is one past the
// last byte of the lexeme) and after token_start_offset has been
// set to the first byte of the lexeme. The cast to uint32_t is
// safe because the source buffer is bounded by the ~4 GiB span
// address space established in common.h; a larger file would
// overflow the Span type anyway.
static Span span_from(Lexer* l) {
    Span s;
    s.start   = l->token_start_offset;
    s.end     = (uint32_t)(l->cur - l->source);
    s.file_id = l->file_id;
    return s;
}

// Convenience constructor: build a Token of kind k with a span
// derived from the current cursor position. Every token the
// lexer emits goes through this except the synthesized TOK_SEMI,
// which needs a span at a position the cursor has already
// passed (the first newline), so it constructs the Token by hand
// in lexer_next.
static Token make(Lexer* l, TokenKind k) {
    Token t; t.kind = k; t.span = span_from(l); return t;
}

// Skip whitespace and comments. Returns true if any newline was
// crossed, and writes the offset of the *first* newline crossed to
// *first_nl. That offset becomes the span of any synthetic `;`.
//
// Why "first newline" and not "last" or "current": when a
// statement ends and one or more blank lines follow, the ASI
// semicolon should be attributed to the newline that ended the
// statement, not to some later blank line. Using the first
// newline keeps the caret under the actual line terminator in
// diagnostics, which matches how a reader thinks about the
// statement boundary. Once `saw_nl` is true the code stops
// overwriting *first_nl, so only the first one wins.
//
// Caller contract:
//   - *first_nl must be a valid pointer. It is written on entry
//     (to 0) and again at the first newline. If no newline is
//     crossed the caller sees 0, which is also a legitimate
//     offset; the caller distinguishes "no newline" by the
//     return value, not by inspecting *first_nl.
//   - The function consumes leading whitespace, `//` line
//     comments, and newlines. It does NOT consume block
//     comments; Prism does not have them (only `//`).
//
// Whitespace set: space, tab, carriage return. `\r` is treated
// as whitespace so CRLF files do not put a stray byte in front
// of tokens. `\n` is handled separately because it is the only
// thing that can trigger ASI.
//
// `//` comment handling: once the two slashes are seen, consume
// bytes up to (but not including) the next '\n' or EOF. The
// outer loop then re-enters and sees the '\n' (or EOF), which
// is exactly the state ASI wants. Comments therefore cannot
// hide a newline from ASI.
//
// Return value semantics: true iff we crossed at least one '\n'
// during this call. The trailing-whitespace case (EOF reached
// without a newline) returns whatever was accumulated so far.
static bool skip_ws_and_comments(Lexer* l, uint32_t* first_nl) {
    bool saw_nl = false;
    *first_nl = 0;
    for (;;) {
        char c = peek(l);
        // Fast path: ordinary horizontal whitespace. `\r` is here
        // rather than next to `\n` so that CRLF sequences are
        // treated as "whitespace, then newline" and the newline
        // still triggers ASI on the following iteration.
        if (c == ' ' || c == '\t' || c == '\r') { advance(l); continue; }
        // Line comment. The peek2 check is safe even at EOF
        // because peek2 returns '\0' past the terminator. We do
        // not consume the terminating newline here; the outer
        // loop will see it on the next iteration and record it.
        if (c == '/' && peek2(l) == '/') {
            while (!at_end(l) && peek(l) != '\n') advance(l);
            continue;
        }
        // Newline. Record the *first* one we see (the saw_nl
        // guard makes subsequent newlines no-ops for the offset),
        // then consume it and keep going.
        if (c == '\n') {
            if (!saw_nl) {
                *first_nl = (uint32_t)(l->cur - l->source);
                saw_nl = true;
            }
            advance(l);
            continue;
        }
        // Anything else means the token body starts here; stop.
        // The cursor is left on the first non-whitespace,
        // non-comment byte, which is what lexer_next expects.
        return saw_nl;
    }
}

// Map an identifier's byte range to a keyword TokenKind, or to
// TOK_IDENT if the bytes are not a reserved word.
//
// `s` points at the first byte of the identifier, `n` is its
// length. This is a hand-rolled linear scan rather than a hash
// table: the keyword set is small and known, memcmp on short
// strings is fast, and this avoids a startup cost for programs
// that use few keywords. If the set grows past ~50 entries a
// hash would pay off; not yet.
//
// The KW macro exists so each entry is one line and the length
// check cannot drift out of sync with the literal. `sizeof(kw)-1`
// is the length of the string literal without its NUL, computed
// at compile time. The order of checks is: length first (cheap,
// rejects almost every identifier immediately), then memcmp
// (only runs on length match). That ordering is what makes the
// linear scan acceptable.
//
// Invariants relied on by the call site:
//   - `s` and `n` describe a valid identifier already classified
//     by is_ident_start / is_ident_cont. The function does not
//     re-validate; passing a non-identifier byte range just
//     returns TOK_IDENT.
static TokenKind keyword_or_ident(const char* s, int n) {
    #define KW(kw, tok) if (n == (int)sizeof(kw)-1 && memcmp(s, kw, sizeof(kw)-1) == 0) return tok
    KW("fn",        TOK_FN);
    KW("let",       TOK_LET);
    KW("mut",       TOK_MUT);
    KW("const",     TOK_CONST);
    KW("if",        TOK_IF);
    KW("else",      TOK_ELSE);
    KW("while",     TOK_WHILE);
    KW("for",       TOK_FOR);
    KW("in",        TOK_IN);
    KW("break",     TOK_BREAK);
    KW("continue",  TOK_CONTINUE);
    KW("return",    TOK_RETURN);
    KW("as",        TOK_AS);
    KW("true",      TOK_TRUE);
    KW("false",     TOK_FALSE);
    KW("ref",       TOK_REF);
    KW("rc",        TOK_RC);
    KW("type",      TOK_TYPE);
    KW("struct",    TOK_STRUCT);
    KW("enum",      TOK_ENUM);
    KW("match",     TOK_MATCH);
    KW("using",     TOK_USING);
    KW("pub",       TOK_PUB);
    KW("extern",    TOK_EXTERN);
    KW("from",      TOK_FROM);
    #undef KW
    return TOK_IDENT;
}

// Does this token kind end a statement for ASI purposes?
//
// The set mirrors Go's rule with one addition: `}` is included so
// that a block statement is implicitly terminated. When we later
// add tail expressions, `}` will need to leave this set — the
// lookahead exception planned for that step will handle it.
//
// Rationale for each entry:
//   - TOK_IDENT: an expression statement's last token is usually
//     an identifier (`foo;`), so a newline after an identifier
//     commonly ends a statement. This is the reason Prism (and
//     Go) require operators at line ends when a statement is
//     meant to continue.
//   - TOK_INT, TOK_FLOAT, TOK_STRING, TOK_TRUE, TOK_FALSE: same
//     reasoning for the other literal-ending expression forms.
//   - TOK_RPAREN, TOK_RBRACKET: closes a call or index; the
//     surrounding statement is done.
//   - TOK_RBRACE: closes a block (function body, if body, etc.),
//     which is a complete statement on its own.
//   - TOK_RETURN: a bare `return` with no value. Go treats this
//     as terminator because `return` at the end of a line means
//     "return nothing". Prism follows the same convention here.
//
// Everything else (operators, opening brackets, commas, the
// keywords that start statements) is NOT a terminator, so a
// newline after them does not insert a `;`. This is what makes
// `let x = 1 +\n 2` parse as a single expression rather than
// `let x = 1 +; 2`.
static bool is_asi_terminator(TokenKind k) {
    switch (k) {
        case TOK_IDENT:
        case TOK_INT:
        case TOK_FLOAT:
        case TOK_STRING:
        case TOK_TRUE:
        case TOK_FALSE:
        case TOK_RPAREN:
        case TOK_RBRACE:
        case TOK_RBRACKET:
        case TOK_RETURN:
            return true;
        default:
            return false;
    }
}

// The main dispatch. Returns the next token in the stream,
// synthesizing ASI semicolons as needed.
//
// Shape of a single call:
//   1. Skip whitespace and comments, remembering whether a
//      newline was crossed and where the first one was.
//   2. If a newline was crossed (or we hit EOF) and the previous
//      significant token can end a statement, emit a synthetic
//      TOK_SEMI and return. This happens at most once per call:
//      the caller must invoke lexer_next again to move past the
//      newline to the next real token. Multiple blank lines
//      therefore still produce exactly one `;`.
//   3. Otherwise, mark the start of the next token and read it
//      by first character (or two-character sequence) dispatch.
//   4. Update prev_significant with the returned kind so the
//      next call's ASI check sees the right predecessor.
//
// The order in step 2 matters: the ASI check runs *before*
// token_start_offset is updated, so a synthetic TOK_SEMI does
// not disturb the offset that the next real token will set.
Token lexer_next(Lexer* l) {
    uint32_t nl_offset = 0;
    bool saw_nl = skip_ws_and_comments(l, &nl_offset);

    // Automatic semicolon insertion. Fires at most once per call,
    // regardless of how many blank lines we crossed.
    //
    // Two conditions, both required:
    //   (a) saw_nl || at_end(l): we either crossed a newline or
    //       reached end of input. EOF counts as a statement
    //       boundary so a file whose last statement lacks a
    //       trailing newline still gets its terminating `;`.
    //   (b) is_asi_terminator(prev_significant): the previous
    //       token can legally end a statement.
    //
    // Span choice: if a newline was crossed, the span is a
    // zero-width point at the offset of the first newline, so a
    // diagnostic about the inserted `;` points at the line
    // terminator rather than at whatever token happens to follow.
    // If we are at EOF with no newline, the span is a zero-width
    // point at the EOF position.
    //
    // prev_significant is set to TOK_SEMI *before* returning, so
    // that if the caller immediately calls lexer_next again the
    // ASI check sees TOK_SEMI (which is not a terminator) and
    // will not emit a second consecutive `;`. This is what makes
    // a run of blank lines collapse to a single `;`.
    if ((saw_nl || at_end(l))&& is_asi_terminator(l->prev_significant)) {
        uint32_t off  = saw_nl ? nl_offset : (uint32_t)(l->cur - l->source);
        l->prev_significant = TOK_SEMI;
        Token t = { TOK_SEMI, { off, off, l->file_id } };
        return t;
    }

    // From here on we are reading a real token. Record its start
    // so span_from can compute the range once the body is done.
    l->token_start_offset = (uint32_t)(l->cur - l->source);

    // End of input. Emit TOK_EOF (a zero-width span at EOF) and
    // record it as the previous kind. Because TOK_EOF is not a
    // terminator, the next call's ASI check will not fire, so
    // repeated calls keep returning TOK_EOF without ever
    // synthesizing a `;` after it.
    if (at_end(l)) {
        Token t = make(l, TOK_EOF);
        l->prev_significant = t.kind;
        return t;
    }

    // Consume the first byte of the token and dispatch on it.
    // Note that `c` has already advanced past this byte, so all
    // the "consume the rest" loops below use peek/advance rather
    // than inspecting `c` again.
    char c = advance(l);
    Token result;

    // --- c"..." C-string literal -----------------------------
    // Recognized by the two-byte prefix `c"`. `c` alone would be
    // an identifier, but `c"` is unambiguous because a `"` can
    // never follow an identifier in valid Prism.
    //
    // Body scanning: consume until the closing `"`, a newline,
    // or EOF. Newlines are not allowed inside string literals,
    // so a newline before the closing quote is treated as an
    // unterminated-string error (TOK_ERROR). The span in that
    // case covers from the opening `c` through the byte before
    // the newline, so diagnostics can underline the bad literal.
    //
    // No escape processing is done here. Escapes (if any) are
    // the parser's / codegen's problem; the lexer hands back the
    // raw byte range.
    if (c == 'c' && peek(l) == '"') {
        advance(l);                     // consume opening quote
        while (!at_end(l) && peek(l) != '"' && peek(l) != '\n') advance(l);
        if (at_end(l) || peek(l) == '\n') {
            result = make(l, TOK_ERROR);
        } else {
            advance(l);                 // consume closing quote
            result = make(l, TOK_CSTRING);
        }
    }
    // --- Identifier or keyword -------------------------------
    // `c` is the first identifier byte. Consume all trailing
    // identifier characters, then classify the byte range via
    // keyword_or_ident. The length passed to keyword_or_ident is
    // computed by pointer subtraction rather than by storing it,
    // because token_start_offset is the only "start" state we
    // need and (cur - start) gives the length for free.
    else if (is_ident_start(c)) {
        while (is_ident_cont(peek(l))) advance(l);
        result = make(l, keyword_or_ident(l->source + l->token_start_offset,
                       (int)(l->cur - (l->source + l->token_start_offset))));
    }
    // --- Numeric literal -------------------------------------
    // `c` is the first digit. Consume digits greedily. A single
    // `.` is accepted as a decimal point only when:
    //   - we have not yet seen a `.` in this number (saw_dot),
    //     so `1.2.3` lexes as FLOAT(1.2) DOT INT(3) rather than
    //     swallowing the second dot, and
    //   - the character after the `.` is a digit, so `0..5` is
    //     INT(0) DOTDOT INT(5) rather than FLOAT(0.) DOT INT(5).
    // That second rule is what keeps the range operator `..`
    // lexable without a space. No exponent, no hex, no leading
    // sign; those would be separate tokens (`-` then INT).

    else if (isdigit((unsigned char)c)) {
        // Hex: `0x` / `0X`. Digits after the prefix are
        // hex; the token is still TOK_INT.
        if (c == '0' && (peek(l) == 'x' || peek(l) == 'X')) {
            advance(l);     //consume the 'x'
            while (isxdigit((unsigned char)peek(l))) advance(l);
            result = make(l, TOK_INT);
        }

        // Binary: `0b` / `0B`.
        else if (c == '0' && (peek(l) == 'b' || peek(l) == 'B')) {
            advance(l);     // consume 'b'
            while (peek(l) == '0' || peek(l) == '1') advance(l);
            result = make(l, TOK_INT);
        } else {
            bool saw_dot = false;
            for (;;) {
                char nx = peek(l);
                if (isdigit((unsigned char)nx)) {
                    advance(l);
                    continue;
                }
                // Accept `.` only as a decimal point: at most once,
                // and only when the next character is a digit. This
                // preserves `1.5` while leaving `0..5` as INT DOTDOT
                // rather than swallowing the range operator.
                if (nx == '.' && !saw_dot && isdigit((unsigned char)peek2(l))) {
                    saw_dot = true;
                    advance(l);
                    continue;
                }
                break;
            }
            result = make(l, saw_dot ? TOK_FLOAT : TOK_INT);
        }
    }
    // --- String literal --------------------------------------
    // Same scanning rules as CSTRING, minus the `c` prefix.
    // `c` here is the opening `"`. The body loop consumes until
    // a `"`, `\n`, or EOF. A newline or EOF before the closing
    // quote is an error (unterminated string); otherwise the
    // closing quote is consumed and the token is TOK_STRING.
    else if (c == '"') {
        while (!at_end(l) && peek(l) != '"' && peek(l) != '\n') advance(l);
        if (at_end(l) || peek(l) == '\n') {
            result = make(l, TOK_ERROR);
        } else {
            advance(l);   // consume closing quote
            result = make(l, TOK_STRING);
        }
    }
    // --- Punctuation and operators ---------------------------
    // Single-byte cases are direct. Multi-byte operators are
    // handled by calling match() for the optional second byte
    // (or, for `...`, by two explicit advances after looking
    // ahead). The lexer never backtracks: every branch commits
    // to a token kind after at most two bytes of lookahead.
    //
    // Case-by-case notes:
    //   `.`: `...` (ellipsis) is checked first; then `..`
    //        (dotdot, range) via match; otherwise a single DOT.
    //        Order matters: checking `..` before `.` is what
    //        makes `a..b` lex as IDENT DOTDOT IDENT.
    //   `-`: `->` via match, otherwise MINUS. (No `-=` or `--`.)
    //   `=`: `==` first, then `=>` (fat arrow), then plain `=`.
    //        The order is important because `==` and `=>` share
    //        a first byte with `=`.
    //   `!`: `!=` via match, otherwise BANG. (No unary `!` on
    //        numbers yet; the token exists for potential use.)
    //   `<`, `>`: two-byte comparison operators via match,
    //        otherwise LT/GT.
    //   everything else: TOK_ERROR. The lexer does not try to
    //        recover or skip; it hands back a one-byte error
    //        token whose span the caller can diagnose.
    else {
        switch (c) {
            case '(': result = make(l, TOK_LPAREN); break;
            case ')': result = make(l, TOK_RPAREN); break;
            case '{': result = make(l, TOK_LBRACE); break;
            case '}': result = make(l, TOK_RBRACE); break;
            case '[': result = make(l, TOK_LBRACKET); break;
            case ']': result = make(l, TOK_RBRACKET); break;
            case ';': result = make(l, TOK_SEMI);   break;
            case ',': result = make(l, TOK_COMMA);  break;
            case ':': result = make(l, TOK_COLON);  break;
            case '?': result = make(l, TOK_QUESTION); break;
            case '.':
                // Ellipsis is three bytes; `..` and `.` are
                // decided by match. The two explicit advances
                // consume the second and third dots without
                // needing a helper that takes a length.
                if (peek(l) == '.' && peek2(l) == '.') {
                    advance(l); advance(l);
                    result = make(l, TOK_ELLIPSIS);
                } else {
                    result = make(l, match(l,'.') ? TOK_DOTDOT : TOK_DOT);
                }
                break;
            case '+':
                result = make(l, match(l, '=') ? TOK_PLUS_ASSIGN : TOK_PLUS);
                break;
            case '*':
                result = make(l, match(l, '=') ? TOK_STAR_ASSIGN : TOK_STAR);
                break;
            case '/':
                result = make(l, match(l, '=') ? TOK_SLASH_ASSIGN : TOK_SLASH);
                break;
            case '&':
                result = make(l, match(l, '&') ? TOK_AMPAMP : TOK_AMP);
                break;
            case '|':
                // `|` alone is not a Prism token; `||` is.
                if (match(l, '|')) result = make(l, TOK_PIPEPIPE);
                else               result = make(l, TOK_ERROR);
                break;
            case '-':
                if (match(l, '>'))      result = make(l, TOK_ARROW);
                else if (match(l, '=')) result = make(l, TOK_MINUS_ASSIGN);
                else                    result = make(l, TOK_MINUS);
                break;
            // `=` has three forms and they must be tested in
            // most-specific-first order: `==` before `=>` before
            // `=`. A single `=` after both matches fail is
            // TOK_ASSIGN (which is what the parser treats as an
            // assignment or a `let` initializer, depending on
            // context).
            case '=': {
                if(match(l, '=')) result = make(l, TOK_EQEQ);
                else if(match(l, '>')) result = make(l, TOK_FATARROW);
                else result = make(l, TOK_ASSIGN);
                break;
            }
            case '!': result = make(l, match(l, '=') ? TOK_BANGEQ : TOK_BANG);   break;
            case '<': result = make(l, match(l, '=') ? TOK_LE     : TOK_LT);     break;
            case '>': result = make(l, match(l, '=') ? TOK_GE     : TOK_GT);     break;
            default:  result = make(l, TOK_ERROR); break;
        }
    }

    // Record the kind we are about to return so the next call's
    // ASI check sees this token as its predecessor. Note that
    // TOK_ERROR and TOK_EOF are recorded too; neither is in the
    // terminator set, so a bad token does not trigger a spurious
    // `;` insertion on the following newline.
    l->prev_significant = result.kind;
    return result;
}

// Non-destructive lookahead. Copies the entire Lexer (source
// pointer, cursor, file_id, token_start_offset, prev_significant)
// into a local, runs lexer_next on the copy, and returns the
// result. The original lexer is untouched, so the next real
// lexer_next call sees the same input as if lexer_peek had not
// been called.
//
// This is O(1) in size (Lexer is small and contains no owning
// state) but not free: it re-scans the upcoming token. That is
// acceptable because lookahead is used sparingly (mostly one
// token deep in the parser's Pratt chain). If the parser ever
// needs deep lookahead, this is the function to revisit; a
// token ring buffer would be the usual fix. Not proposing that
// now.
Token lexer_peek(Lexer* l) {
    Lexer copy = *l;
    return lexer_next(&copy);
}

// Human-readable name for a TokenKind. Used in diagnostics,
// disassembly, and anywhere a token needs to be printed.
//
// Two naming conventions are mixed on purpose:
//   - Keyword and operator kinds return the lexeme itself
//     ("fn", "==", "->") because that is what a programmer
//     would type and what a parser error message should say.
//   - Category kinds return a descriptive word ("int",
//     "ident", "c-string") because there is no single lexeme
//     to show. A diagnostic that says "expected int" reads
//     better than "expected 12345".
//   - TOK_ERROR returns "<error>" with angle brackets so it
//     is visually distinct from any real lexeme.
//   - TOK_EOF returns "EOF".
//
// The trailing `return "?"` after the switch is defensive: if
// the enum ever grows a new kind and this switch is not
// updated, we print "?" rather than fall off the end. Some
// compilers warn about a missing default when every enum case
// is covered, so the fallback is placed after the switch rather
// than inside a default: label.
//
// This function is pure and has no Lexer parameter; the name
// is a historical artifact (it was once a method of Lexer).
const char* token_kind_name(TokenKind k) {
    switch (k) {
        case TOK_INT: return "int"; case TOK_IDENT: return "ident";
        case TOK_FLOAT: return "float";
        case TOK_FN: return "fn"; case TOK_LET: return "let";
        case TOK_IF: return "if"; case TOK_ELSE: return "else";
        case TOK_WHILE: return "while"; case TOK_RETURN: return "return";
        case TOK_FOR: return "for"; case TOK_IN: return "in";
        case TOK_AS: return "as"; 
        case TOK_BREAK: return "break"; case TOK_CONTINUE: return "continue";
        case TOK_TRUE: return "true"; case TOK_FALSE: return "false";
        case TOK_REF: return "ref";
        case TOK_RC: return "rc"; case TOK_MUT: return "mut";
        case TOK_TYPE: return "type"; case TOK_STRUCT: return "struct";
        case TOK_ENUM: return "enum"; case TOK_MATCH: return "match";
        case TOK_CONST: return "const"; case TOK_USING: return "using";
        case TOK_PUB: return "pub";
        case TOK_STRING: return "string";
        case TOK_CSTRING: return "c-string";
        case TOK_EXTERN: return "extern";
        case TOK_FROM: return "from";
        case TOK_LPAREN: return "("; case TOK_RPAREN: return ")";
        case TOK_LBRACE: return "{"; case TOK_RBRACE: return "}";
        case TOK_LBRACKET: return "["; case TOK_RBRACKET: return "]";
        case TOK_SEMI: return ";"; case TOK_COMMA: return ",";
        case TOK_COLON: return ":"; case TOK_QUESTION: return "?";
        case TOK_ARROW: return "->";
        case TOK_FATARROW: return "=>";
        case TOK_PLUS: return "+"; case TOK_MINUS: return "-";
        case TOK_STAR: return "*"; case TOK_SLASH: return "/";
        case TOK_AMP: return "&"; 
        case TOK_AMPAMP:       return "&&";
        case TOK_PIPEPIPE:     return "||";
        case TOK_PLUS_ASSIGN:  return "+=";
        case TOK_MINUS_ASSIGN: return "-=";
        case TOK_STAR_ASSIGN:  return "*=";
        case TOK_SLASH_ASSIGN: return "/=";
        case TOK_DOT: return "."; case TOK_DOTDOT: return "..";
        case TOK_ELLIPSIS: return "...";
        case TOK_EQEQ: return "==";
        case TOK_BANGEQ: return "!=";
        case TOK_LT: return "<"; case TOK_LE: return "<=";
        case TOK_GT: return ">"; case TOK_GE: return ">=";
        case TOK_BANG: return "!";
        case TOK_ASSIGN: return "=";
        case TOK_EOF: return "EOF"; case TOK_ERROR: return "<error>";
    }
    return "?";
}