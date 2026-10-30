// ============================================================
// parser.c — Recursive-descent parser with Pratt expression
// precedence and generic type arguments.
//
// Structure:
//   parse_program        top-level: fn, const, type, using, pub
//     function_declaration
//     const_declaration
//     type_declaration
//     using_declaration
//     pub_declaration
//     statement
//       let_declaration / if / while / for / match / return /
//       break / continue / block / assign / expr statement
//     expression         entry to the precedence chain
//       eq_expr → cmp_expr → add_expr → mul_expr → cast_expr
//       → unary → postfix → primary
//
// Error recovery:
//   - `expect` reports and returns the offending token without
//     consuming it.
//   - `statement` has a single exit point; any error triggers
//     `synchronize` before returning.
//   - `parse_program` skips to the next plausible declaration.
//
// Generics:
//   - `<T, U>` after a function or type name introduces type
//     parameters. Stored as Tokens on the AST node.
//   - `<T>` in a type expression produces TEX_GENERIC.
//   - `f<T>(...)` in an expression is disambiguated from
//     `f < T > (...)` by bounded lookahead: the `<` is treated
//     as a generic opener only if the matching `>` is
//     immediately followed by `(` or `.`.
//
// Reading notes:
//   - All parser functions take a Parser* and mutate it. The
//     return value is the AST node produced (or NULL on
//     unrecoverable failure at that production). Diagnostics
//     go into p->bag as a side effect.
//   - The `before = bag->error_count` pattern appears before
//     productions whose caller wants to know if *this*
//     production added errors. See statement(), block(), and
//     match_statement().
//
// Not covered here: the parser does not evaluate constant
// expressions, resolve types, or check that a construct is
// legal in its context. Those jobs belong to sema. The parser
// only accepts or rejects syntax; a syntactically valid node
// may still be semantically nonsense and is passed on.
// ============================================================
#include "../include/parser.h"
#include "../include/diagnostics.h"
#include "../include/types.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// ------------------------------------------------------------
// Parser state
//
// The parser holds two tokens of lookahead:
//   cur  — the token the parser is currently "on". All checks
//          and matches look at cur.
//   prev — the last consumed token. Used almost exclusively
//          for computing the end offset of a node's span
//          (many productions build a span from some earlier
//          start offset to `p->prev.span.end`).
//
// `bag` and `smap` are borrowed; the parser does not own them.
//
// `panic` is set true when a production reports an error and
// false again when synchronization completes. In practice the
// flag is set by expect() and read by callers of statement()
// to decide whether to synchronize; the flag itself is not
// consulted inside expect or synchronize (synchronize just
// clears it).
//
// `restrict_struct_lit` is a context-sensitive suppression
// flag. In a `for x in expr { ... }` the `{` that begins the
// body would otherwise be misread as a struct-literal brace
// when the iterable expression is a bare identifier (e.g.
// `for x in things { ... }`). Setting this flag true tells
// primary() not to treat a `{` after an identifier as a
// struct-literal opener. It is set around the iterable and
// the match scrutinee, and restored after.
// ------------------------------------------------------------
typedef struct {
    Lexer*           lex;
    Token            cur;
    Token            prev;
    DiagnosticBag*   bag;
    const SourceMap* smap;
    bool             panic;
    bool             restrict_struct_lit;
} Parser;

// ============================================================
// Token stream primitives
//
// advance / check / match / expect / end_statement /
// synchronize form the bottom layer. Every other production
// is expressed in terms of them.
// ============================================================

// Advance the parser by one token.
//
// Loops past TOK_ERROR tokens. A TOK_ERROR is not a real
// token in the language; it is the lexer's way of reporting
// an unrecognized byte or an unterminated string. Because the
// error is reported as a diagnostic and the token is
// discarded, downstream productions never see it, which keeps
// every other token-kind switch exhaustive without an error
// case. Each discarded error produces one diagnostic from
// here; the diagnostics carry the bad span, so the user can
// find the offending character.
//
// `p->prev = p->cur` runs *before* the loop, so at loop exit
// `prev` is the last non-error token (the previous real
// token), not the last error.
static void advance(Parser* p) {
    p->prev = p->cur;
    for (;;) {
        p->cur = lexer_next(p->lex);
        if (p->cur.kind != TOK_ERROR) break;
        diag_error(p->bag, p->cur.span, "unexpected character");
    }
}

// True iff the current token is of kind k. Non-consuming.
static bool check(Parser* p, TokenKind k) {
    return p->cur.kind == k;
}

// If the current token is of kind k, consume it and return
// true; otherwise leave the parser alone and return false.
// This is the "optional token" form used throughout the
// parser for optional punctuation.
static bool match(Parser* p, TokenKind k) {
    if (!check(p, k)) return false;
    advance(p);
    return true;
}

// Consume a token of the expected kind and return it.
//
// On mismatch: report a diagnostic naming the expected token
// (`what` is a human-readable description, e.g. "';' after
// variable declaration") and the actual token kind, set the
// panic flag, and return the *current* token without
// consuming it. Returning cur rather than an "error token"
// is deliberate: callers that use the returned Token only
// for its span get a usable location; callers that inspect
// the returned kind will see the wrong kind and should have
// checked p->bag (or p->panic) first.
//
// The "no consume on mismatch" rule is what lets synchronize
// see the same token the failing production saw, and is what
// makes recovery at statement boundaries deterministic.
static Token expect(Parser* p, TokenKind k, const char* what) {
    if (check(p, k)) {
        Token t = p->cur;
        advance(p);
        return t;
    }
    diag_error(p->bag, p->cur.span, "expected %s, got %s",
               what, token_kind_name(p->cur.kind));
    p->panic = true;
    return p->cur;
}

// Consume the terminator of a statement.
//
// Two tokens are accepted without consuming anything:
//   - TOK_SEMI: consumed.
//   - TOK_RBRACE: NOT consumed. A `}` closes the enclosing
//     block; the caller's loop will see it and stop. Treating
//     `}` as an implicit terminator is why `{ let x = 1 }` is
//     a valid block without a trailing `;`.
// Any other token goes through expect(p, TOK_SEMI, ctx), which
// reports and sets panic. `ctx` is a caller-supplied
// description used in the diagnostic ("';' after variable
// declaration").
static void end_statement(Parser* p, const char* ctx) {
    if (check(p, TOK_SEMI)) { advance(p); return; }
    if (check(p, TOK_RBRACE)) return;
    expect(p, TOK_SEMI, ctx);
}

// Panic-mode recovery: skip forward to the next plausible
// statement or declaration boundary.
//
// Called from statement() (and indirectly from block(), which
// calls statement) after a failed production. The parser is
// left sitting on a token that starts a new production, so
// the enclosing loop can call the appropriate parse function
// again.
//
// Stop conditions, checked in order:
//   1. If the last consumed token was `;`, stop immediately.
//      The previous statement ended cleanly; the error was in
//      the *following* token and we do not want to skip it.
//   2. If the current token is a "starter" keyword (let, if,
//      while, for, match, return, break, continue, fn, const,
//      type, using, pub) or `}`, stop. Those start a new
//      statement or terminate the current block.
//   3. If the current token is `;`, consume it and stop. The
//      statement we were in the middle of has ended.
//   4. Otherwise, advance and keep looking.
//
// The loop never returns without either hitting EOF or
// stopping via cases 1-3, so callers can rely on the parser
// being on a real token (possibly EOF) at return.
//
// `p->panic` is cleared on entry; the intent is that once
// recovery starts, the parse is no longer "in a panic" even
// though it may take several tokens to resync.
static void synchronize(Parser* p) {
    p->panic = false;
    while (!check(p, TOK_EOF)) {
        if (p->prev.kind == TOK_SEMI) return;
        switch (p->cur.kind) {
            case TOK_RBRACE:
            case TOK_LET:
            case TOK_IF:
            case TOK_WHILE:
            case TOK_FOR:
            case TOK_MATCH:
            case TOK_RETURN:
            case TOK_BREAK:
            case TOK_CONTINUE:
            case TOK_FN:
            case TOK_CONST:
            case TOK_TYPE:
            case TOK_USING:
            case TOK_PUB:
                return;
            case TOK_SEMI:
                advance(p);
                return;
            default:
                advance(p);
        }
    }
}

// ============================================================
// Forward declarations
//
// These exist so that productions can be defined in the file
// in a natural order without a strict "callees first" rule.
// Nothing here is non-obvious.
// ============================================================
static AstNode* expression(Parser* p);
static AstNode* block(Parser* p);
static AstNode* statement(Parser* p);
static TypeExpr* parse_type_expr(Parser* p);
static AstNode* extern_declaration(Parser* p, Token extern_kw);


// ============================================================
// peek_generic_call
//
// Called with the parser's current token being `<`. Returns true
// if the tokens form `<type (, type)*>` immediately followed by
// `(` or `.`. Uses a throwaway lexer copy; the parser's real
// lexer state is never touched.
//
// When the parser's current token is `<`, its underlying lexer
// has already consumed the `<` character, so a copy of the lexer
// starts cleanly at the first token after `<`.
//
// Why this function exists:
//   The expression grammar contains a genuine ambiguity. After
//   an identifier followed by `<`, the parser cannot tell
//   whether it is looking at
//       `f < T > (args)`     — a generic call
//       `f < T > (args)`     — also parses as comparison chain
//       `f < T > .name`      — a generic variant constructor
//   Both parses are syntactically valid. The parser resolves
//   the ambiguity by *lookahead*: only treat the `<` as a
//   generic opener if the tokens after the matching `>` are
//   `(` or `.`. That is a standard heuristic and matches what
//   Rust, Swift, and C# do in practice.
//
// The lookahead is bounded (64 tokens) for two reasons:
//   1. To prevent an infinite scan on unterminated generic
//      syntax. A `<` with no matching `>` should fail fast.
//   2. To bound the cost of a backtracking lookahead. Source
//      code with a deeply nested generic type inside a call is
//      possible but rare; 64 is generous for anything a human
//      writes.
//
// Return value semantics:
//   - true  — safe to consume `<` as a generic opener.
//   - false — leave the `<` alone; the caller will treat it
//             as a binary comparison operator or as part of a
//             larger expression.
//
// Note that a `{` after the `>` is also accepted as a
// continuation (see the `TOK_LBRACE` check below). That
// covers `Type<T> { field: v }` — a generic struct literal —
// which has the same shape as a generic call.
// ============================================================
static bool peek_generic_call(Parser* p) {
    if (!check(p, TOK_LT)) return false;

    // Copy the lexer. Lexer is trivially copyable and contains
    // no owning state, so the copy is safe to discard.
    Lexer probe = *p->lex;
    int   depth = 1;   // we are already inside one `<`

    for (int i = 0; i < 64; i++) {
        Token t = lexer_next(&probe);
        if (t.kind == TOK_LT) depth++;
        else if (t.kind == TOK_GT) {
            depth--;
            if (depth == 0) {
                // Matching `>` found. Peek one more token to
                // decide. The three accepted continuations:
                //   `(`  — function call: f<T>(...)
                //   `.`  — variant:      Type<T>.Variant
                //   `{`  — struct lit:   Type<T> { ... }
                Token nxt = lexer_next(&probe);
                return nxt.kind == TOK_LPAREN
                    || nxt.kind == TOK_DOT
                    || nxt.kind == TOK_LBRACE;
            }
        } else if (t.kind == TOK_EOF
                || t.kind == TOK_SEMI
                || t.kind == TOK_RBRACE) {
            // A `;` or `}` before the matching `>` means the
            // expression ended without a generic opener. EOF
            // likewise. Bail out to avoid scanning into
            // unrelated source.
            return false;
        }
    }
    // Ran out of lookahead budget. Treat as "not a generic
    // call" so the caller falls back to the comparison parse.
    return false;
}

// ============================================================
// Type expressions
//
// Grammar:
//   type_expr := '*' type_expr
//              | 'rc' '<' type_expr '>'
//              | 'ref' '<' 'mut'? type_expr '>'
//              | '[' type_expr ']'
//              | IDENT ('<' type_expr (',' type_expr)* '>')?
//              | IDENT '.' IDENT ('.' IDENT)*
//
// Recursive descent. Each branch consumes its prefix (or the
// leading IDENT) and recurses into `parse_type_expr` for the
// sub-expression. On any failure, returns NULL and has
// already reported into the bag; the caller must check for
// NULL before using the result (a NULL TypeExpr is not stored
// in any AST node -- the caller either replaces it with
// something benign or drops the enclosing declaration).
// ============================================================
static TypeExpr* parse_type_expr(Parser* p) {
    // Span start is captured before any token is consumed, so
    // it covers the leading `*`, `rc`, `ref`, `[`, or IDENT
    // of this type expression. The end of the span is filled
    // in by the specific branch below; for TEX_NAME it is the
    // name's span, for compound forms it is the closing
    // delimiter's span.
    Span span = p->cur.span;

    // Function type: `fn(T1, T2, ...) : Ret`. `TOK_FN` is a
    // reserved keyword and no other type production begins
    // with it, so this branch is unambiguous. The parameter
    // types are anonymous; only the return type has a name
    // in the sense of being a single TypeExpr.
    //
    // This is what makes `fn f(g: fn(i32) : i32, x: i32) : i32`
    // parse correctly: the inner `fn(i32) : i32` is consumed
    // atomically by the parse_type_expr call for `g`'s type,
    // and the outer `: i32` is then parsed as `f`'s return
    // type. There is no ambiguity because the inner call
    // stops at the first token that cannot belong to a
    // function type (the `,` or `)` of the outer parameter
    // list).
    if (check(p, TOK_FN)) {
        advance(p);
        expect(p, TOK_LPAREN, "'(' after 'fn' in function type");

        TypeExpr* fn = type_expr_fn(span);
        if (!check(p, TOK_RPAREN)) {
            do {
                type_expr_fn_add_param(fn, parse_type_expr(p));
            } while (match(p, TOK_COMMA));
        }
        expect(p, TOK_RPAREN, "')' after function parameter types");
        expect(p, TOK_COLON, "':' before function return type");
        TypeExpr* ret = parse_type_expr(p);
        type_expr_fn_set_return(fn, ret);
        return fn;
    }

    // `*T` — raw pointer. Recurses with the same precedence so
    // `*ref<i64>` is `*(ref<i64>)`.
    if (check(p, TOK_STAR)) {
        advance(p);
        return type_expr_rawptr(parse_type_expr(p), span);
    }

    // `rc<T>` — RC-managed wrapper.
    if (check(p, TOK_RC)) {
        advance(p);
        expect(p, TOK_LT, "'<' after 'rc'");
        TypeExpr* inner = parse_type_expr(p);
        expect(p, TOK_GT, "'>'");
        return type_expr_rc(inner, span);
    }

    // `ref<T>` or `ref<mut T>`.
    if (check(p, TOK_REF)) {
        advance(p);
        expect(p, TOK_LT, "'<' after 'ref'");
        bool is_mut = match(p, TOK_MUT);
        TypeExpr* inner = parse_type_expr(p);
        expect(p, TOK_GT, "'>'");
        return type_expr_ref(inner, is_mut, span);
    }

    // `[T]` — array type.
    if (check(p, TOK_LBRACKET)) {
        advance(p);
        TypeExpr* el = parse_type_expr(p);
        expect(p, TOK_RBRACKET, "']' after array element type");
        return type_expr_array(el, span);
    }

    // Bare identifier: could be a simple name, a generic
    // (`Name<T>`), or a qualified path (`a.b.c`). Dispatch on
    // the token that follows the identifier.
    if (check(p, TOK_IDENT)) {
        Token first = p->cur;
        advance(p);

        // Generic: `Name<T, U>`. The empty list `Name<>` is
        // accepted (the inner `if (!check(GT))` skips the
        // do-while when the next token is already `>`,
        // producing a generic with zero arguments). Whether
        // zero-argument generics are semantically valid is
        // sema's problem, not the parser's.
        if (check(p, TOK_LT)) {
            advance(p);
            TypeExpr* g = type_expr_generic(first, span);
            if (!check(p, TOK_GT)) {
                do {
                    type_expr_generic_add_arg(g, parse_type_expr(p));
                } while (match(p, TOK_COMMA));
            }
            expect(p, TOK_GT, "'>' after type arguments");
            return g;
        }

        // Qualified path: `a.b.c`. The local array `path[8]`
        // is a hard cap on path length (8 components). The
        // loop collects components until a non-`.` is seen;
        // if the cap is hit, a diagnostic is emitted and the
        // loop breaks, and the TypeExpr is built with what
        // was collected so far. This means a too-long path
        // yields a partially-valid AST plus an error rather
        // than NULL, which lets sema continue and report
        // downstream issues.
        if (check(p, TOK_DOT)) {
            Token path[PRISM_MAX_PATH_COMPONENTS];
            int   n = 0;
            path[n++] = first;
            while (match(p, TOK_DOT)) {
                if (n >= PRISM_MAX_PATH_COMPONENTS) {
                    diag_error(p->bag, p->cur.span, "type path too long");
                    break;
                }
                path[n++] = expect(p, TOK_IDENT, "type name after '.'");
            }
            return type_expr_qualified(path, n, span);
        }

        return type_expr_name(first);
    }

    diag_error(p->bag, p->cur.span, "expected type");
    p->panic = true;
    // Advance past the offending token so callers do not
    // re-report the same one. Exception: closing delimiters
    // (`,`, `)`, `]`, `}`) are left in place because the
    // caller's `expect` produces a more accurate error and
    // needs the delimiter to recover.
    if (p->cur.kind != TOK_RPAREN
        && p->cur.kind != TOK_RBRACKET
        && p->cur.kind != TOK_RBRACE
        && p->cur.kind != TOK_COMMA) {
        advance(p);
    }
    return NULL;
}

// ------------------------------------------------------------
// Type parameter lists: `<T, U, V>`
//
// Called after the name of a `fn`, `struct`, or `enum` to
// parse its generic parameter list, if any. Returns the
// number of parameters parsed (0 if no `<` is present).
//
// `out` is a caller-owned array; `max` is its length (always
// 8 at every call site, matching the fixed-size type_params[8]
// arrays in AST_FUNC / AST_STRUCT_DEF / AST_ENUM_DEF). If the
// source declares more than `max` parameters, the extras are
// parsed (so the `>` is consumed and the rest of the
// declaration parses) but not stored, and one diagnostic per
// extra parameter is emitted.
//
// The parse shape is a do-while so at least one parameter is
// required after `<`. `<>` is therefore a parse error (the
// first expect(IDENT) fails), which matches the language's
// rules: a generic with no parameters is not expressible.
// ------------------------------------------------------------
static int parse_type_params(Parser* p, Token* out, int max) {
    if (!check(p, TOK_LT)) return 0;
    advance(p);
    int n = 0;
    do {
        Token t = expect(p, TOK_IDENT, "type parameter name");
        if (n < max) out[n++] = t;
        else diag_error(p->bag, t.span, "too many type parameters");
    } while (match(p, TOK_COMMA));
    expect(p, TOK_GT, "'>' after type parameters");
    return n;
}

// ============================================================
// Expression parsing
//
// The precedence chain, from lowest to highest:
//   expression -> eq_expr
//   eq_expr    -> cmp_expr   (== !=)
//   cmp_expr   -> add_expr   (< <= > >=)
//   add_expr   -> mul_expr   (+ -)
//   mul_expr   -> cast_expr  (* /)
//   cast_expr  -> unary      (as T, left-assoc, loops)
//   unary      -> postfix    (- ! * & rc)
//   postfix    -> primary    ([] . () ?)
//   primary    -> literals, identifiers, calls, etc.
//
// Each binary level is implemented by calling binary_chain
// with its operator table; see binary_chain below. The chain
// functions are named after the level they implement.
//
// Cast is handled by cast_expr rather than by binary_chain
// because `as T` is a postfix-like operator with a *type* on
// the right, not an expression; it cannot be expressed as a
// binary_chain call without a second parse rule.
//
// There is no `&&` or `||`. Prism has no logical connectives
// at the AST level today; a `&&` would be parsed as an error
// (probably by the `&` unary / address-of rule swallowing the
// first `&` and the second `&` being unexpected). Noting for
// the design record.
// ============================================================
static AstNode* unary(Parser* p);
static AstNode* primary(Parser* p);

// Postfix loop: repeatedly apply whichever postfix operator
// is next, until none matches, then return the accumulated
// expression.
//
// Three postfix forms:
//   `[expr]` — array index.
//   `.name`  — field access, or method call if followed by
//              `(`, or variant construction on an expression.
//   `?`      — try / error-propagation operator.
//
// Details worth knowing:
//
// Index. Span of the resulting node starts at the `[` and
// ends at the matching `]`. Because `[` is a token that can
// only appear postfix (there is no prefix `[` in this
// grammar), the loop is free to treat any `[` as postfix.
//
// Dot. Two cases:
//   - If the token after the name is `(`, this is a *method
//     call*. It is represented as an AST_VARIANT_CTOR with a
//     NULL type_name and a receiver_expr set to the LHS. That
//     unification lets the resolver, which knows about
//     methods, decide what to do; the parser does not have
//     enough information yet.
//   - Otherwise, it is a field access (AST_FIELD). Sema will
//     later reclassify it as a variant tag if the LHS turns
//     out to be an enum value.
//
// Question mark. `e?` becomes AST_TRY. The span is inherited
// from the wrapped expression rather than from the `?`
// token; a future revision might widen the span to include
// the `?`, but that would require knowing the `?` token's
// offset, which is already consumed at the point the node is
// built.
static AstNode* postfix(Parser* p) {
    AstNode* e = primary(p);
    for (;;) {
        if (check(p, TOK_LBRACKET)) {
            Token lb = p->cur;
            advance(p);
            AstNode* index = expression(p);
            Token rb = expect(p, TOK_RBRACKET, "']'");
            // Span from the `[` to the `]`.
            e = ast_index(e, index,
                (Span){ lb.span.start, rb.span.end, lb.span.file_id });
            continue;
        }
        if (check(p, TOK_DOT)) {
            advance(p);
            Token f = expect(p, TOK_IDENT, "field or method name after '.'");
            if (check(p, TOK_LPAREN)) {
                advance(p);
                // Method call: build a VARIANT_CTOR with the
                // LHS as the receiver. type_name is zero (a
                // zero Token has file_id 0, which sema will
                // recognize as "no type name"), variant_name
                // is the method name.
                AstNode* vc = ast_variant_ctor((Token){0}, f);
                vc->as.variant_ctor.receiver_expr = e;
                if (!check(p, TOK_RPAREN)) {
                    do {
                        ast_list_push(&vc->as.variant_ctor.args,
                                      expression(p));
                    } while (match(p, TOK_COMMA));
                }
                expect(p, TOK_RPAREN, "')'");
                e = vc;
                continue;
            }
            e = ast_field(e, f);
            continue;
        }
        if (match(p, TOK_QUESTION)) {
            e = ast_try(e, e->span);
            continue;
        }
        return e;
    }
}

// Parse the value of a TOK_INT token, honoring 0x / 0b
// prefixes. The token's span is over the source bytes; base
// detection reads the first two characters. Overflow is
// reported and the value is reset to 0 so analysis continues
// without spurious downstream errors.
static U64 parse_int_value(Parser* p, Token t) {
    const char* src = p->lex->source;
    uint32_t i = t.span.start;
    uint32_t e = t.span.end;
    U64 v = 0;

    if (i + 1 < e && src[i] == '0'
        && (src[i+1] == 'x' || src[i+1] == 'X')) {
        for (i += 2; i < e; i++) {
            char c = src[i];
            U64 d = (c >= '0' && c <= '9') ? (U64)(c - '0')
                  : (c >= 'a' && c <= 'f') ? (U64)(c - 'a' + 10)
                  : (U64)(c - 'A' + 10);
            if (v > (UINT64_MAX - d) / 16ULL) {
                diag_error(p->bag, t.span, "integer literal too large");
                return 0;
            }
            v = v * 16ULL + d;
        }
    } else if (i + 1 < e && src[i] == '0'
               && (src[i+1] == 'b' || src[i+1] == 'B')) {
        for (i += 2; i < e; i++) {
            U64 d = (U64)(src[i] - '0');
            if (v > (UINT64_MAX - d) / 2ULL) {
                diag_error(p->bag, t.span, "integer literal too large");
                return 0;
            }
            v = v * 2ULL + d;
        }
    } else {
        for (; i < e; i++) {
            U64 d = (U64)(src[i] - '0');
            if (v > (UINT64_MAX - d) / 10ULL) {
                diag_error(p->bag, t.span, "integer literal too large");
                return 0;
            }
            v = v * 10ULL + d;
        }
    }
    return v;
}

// Primary expressions.
//
// Handles everything that is not a binary, unary, or postfix
// operator. The switch dispatches on the leading token kind.
//
// On an unexpected token, the parser reports, advances past
// the offending token, and returns a placeholder AST_INT(0).
// The placeholder exists so the caller does not have to
// handle NULL from every call to primary; it is a real node
// with a real span, just with a zero value. Sema would never
// see it in practice because the bag now has an error and
// the driver will stop before sema, but if the driver ever
// runs sema anyway, the placeholder is harmless.
static AstNode* primary(Parser* p) {
    Token t = p->cur;
    switch (t.kind) {
        // Integer literal. Parsed inline by a decimal accumulator
        // over the token's source bytes. No sign, no radix, no
        // separators: those are lexer-level decisions, and the
        // lexer only emits TOK_INT for a run of ASCII digits.
        //
        // Overflow is not checked here. A literal too large for
        // I64 wraps. Sema is expected to reject the wrapped
        // value if it matters (e.g. by checking the resulting
        // AST_INT against the target type). The parser's job is
        // just to produce a syntactically valid I64.
        case TOK_INT: {
            advance(p);
            return ast_int(t.span, parse_int_value(p,t));
        }
        // Float literal. Copied into a stack buffer (capped at
        // 63 bytes plus NUL) and parsed by strtod. A float
        // literal longer than 63 bytes is truncated *at the
        // copy*, not at the value; this is unlikely to matter
        // for any real program but is worth knowing. strtod
        // accepts a leading `.` or `1.` form, which the lexer
        // does not produce; if the lexer ever relaxes its
        // float rules, this call site will still work.
        case TOK_FLOAT: {
            advance(p);
            char buf[64];
            uint32_t n = t.span.end - t.span.start;
            if (n >= sizeof buf) n = sizeof buf - 1;
            memcpy(buf, p->lex->source + t.span.start, n);
            buf[n] = '\0';
            double v = strtod(buf, NULL);
            return ast_float(t.span, v);
        }
        // String and C-string literals. The AST node stores the
        // Token (with its span), not the string bytes. Escape
        // processing, if any, happens later; the parser does
        // not decode the literal.
        case TOK_STRING:
            advance(p);
            return ast_string(t.span, t);
        case TOK_CSTRING:
            advance(p);
            return ast_cstring(t.span, t);
        

        // Boolean literals.
        case TOK_TRUE:
            advance(p);
            return ast_bool(t.span, true);
        case TOK_FALSE:
            advance(p);
            return ast_bool(t.span, false);
            
        // Identifier: could be a variable, a call, a method
        // call, a variant constructor, or a struct literal. The
        // dispatch is by lookahead.
        case TOK_IDENT: {
            // `sizeof(T)` — soft keyword. Recognized only when
            // the identifier is exactly `sizeof` and the next
            // token is `(`. Otherwise it is a normal
            // identifier, so `let sizeof = 5` still parses.
            if (smap_span_equals(p->smap, t.span, "sizeof")) {
                // NOTE: p->cur is the 'sizeof' token so lexer_peek() returns the next
                // but doesn't advance the real lexer
                Token ahead = lexer_peek(p->lex);
                if (ahead.kind == TOK_LPAREN) {
                    advance(p);   // consume 'sizeof'
                    advance(p);   // consume '('
                    TypeExpr* te = parse_type_expr(p);
                    Token rp = expect(p, TOK_RPAREN,
                                    "')' after sizeof type");
                    Span full = { t.span.start, rp.span.end,
                                t.span.file_id };
                    return ast_sizeof(full, te);
                }
            }
            
            advance(p);

            // -- Generic call / variant / struct lit: `f<T>...`
            // The `<` is treated as a generic opener only if
            // peek_generic_call says so (i.e. the matching `>`
            // is followed by `(`, `.`, or `{`). Otherwise the
            // `<` is left alone and the main expression
            // precedence chain will treat it as comparison.
            //
            // After `<`, three continuations are possible:
            //   `f<T>(args)`        -> AST_CALL with instantiation
            //   `T<U>.Variant(...)` -> AST_VARIANT_CTOR with instantiation
            //   `T<U> { field: v }` -> AST_STRUCT_LIT with instantiation
            //
            // The struct-literal case is gated on
            // !p->restrict_struct_lit, since some contexts
            // (e.g. the scrutinee of a `match`, the iterable
            // of a `for`) must not treat `{` as a struct
            // literal.
            if (check(p, TOK_LT) && peek_generic_call(p)) {
                advance(p);   // '<'
                TypeExpr* g = type_expr_generic(t, t.span);
                if (!check(p, TOK_GT)) {
                    do {
                        type_expr_generic_add_arg(g, parse_type_expr(p));
                    } while (match(p, TOK_COMMA));
                }
                expect(p, TOK_GT, "'>' after type arguments");

                // Call: `f<T>(...)`.
                if (check(p, TOK_LPAREN)) {
                    advance(p);
                    AstNode* call = ast_call(t);
                    call->as.call.instantiation = g;
                    if (!check(p, TOK_RPAREN)) {
                        do {
                            ast_list_push(&call->as.call.args, expression(p));
                        } while (match(p, TOK_COMMA));
                    }
                    expect(p, TOK_RPAREN, "')'");
                    return call;
                }

                // Variant constructor: `Type<T>.Variant` or
                // `Type<T>.Variant(args)`. The parenthesised
                // form is optional.
                if (check(p, TOK_DOT)) {
                    advance(p);
                    Token vn = expect(p, TOK_IDENT, "variant name");
                    AstNode* vc = ast_variant_ctor(t, vn);
                    vc->as.variant_ctor.instantiation = g;
                    if (check(p, TOK_LPAREN)) {
                        advance(p);
                        if (!check(p, TOK_RPAREN)) {
                            do {
                                ast_list_push(&vc->as.variant_ctor.args,
                                              expression(p));
                            } while (match(p, TOK_COMMA));
                        }
                        expect(p, TOK_RPAREN, "')'");
                    }
                    return vc;
                }

                // Generic struct literal: `Type<T> { field: v }`.
                // Suppressed when restrict_struct_lit is set.
                if (check(p, TOK_LBRACE) && !p->restrict_struct_lit) {
                    advance(p);
                    AstNode* lit = ast_struct_lit(t);
                    lit->as.struct_lit.instantiation = g;
                    if (!check(p, TOK_RBRACE)) {
                        // Field list. The loop condition accepts
                        // a trailing comma before `}` (the
                        // `!check(RBRACE)` guard) so `{ a: 1, }`
                        // is legal but `{ a: 1, , b: 2 }` is not.
                        do {
                            Token fn = expect(p, TOK_IDENT, "field name");
                            expect(p, TOK_COLON, "':' after field name");
                            AstNode* v = expression(p);
                            ast_list_push(&lit->as.struct_lit.inits,
                                          ast_struct_init(fn, v));
                        } while (match(p, TOK_COMMA) && !check(p, TOK_RBRACE));
                    }
                    Token rb = expect(p, TOK_RBRACE, "'}'");
                    lit->span = (Span){ t.span.start, rb.span.end,
                                        t.span.file_id };
                    return lit;
                }

                // Fell through all three continuations. Report
                // and produce a placeholder.
                diag_error(p->bag, p->cur.span,
                    "expected '(', '.', or '{' after type arguments");
                return ast_int(t.span, 0);
            }

            // -- Non-generic function call: `f(args)`.
            if (check(p, TOK_LPAREN)) {
                advance(p);
                AstNode* call = ast_call(t);
                if (!check(p, TOK_RPAREN)) {
                    do {
                        ast_list_push(&call->as.call.args, expression(p));
                    } while (match(p, TOK_COMMA));
                }
                expect(p, TOK_RPAREN, "')'");
                return call;
            }

            // -- Dotted access from a bare identifier:
            //    `Type.Variant(...)`  -> variant ctor
            //    `a.b`                -> field access on var `a`
            //
            // The parser cannot distinguish these without
            // knowing whether `a` is a type or a value; the
            // resolver decides. The two AST forms produced
            // here (VARIANT_CTOR vs VAR+FIELD) reflect the
            // *shape* of the syntax: with parens it is a
            // construction, without parens it is a field
            // access. Sema reinterprets as needed.
            if (check(p, TOK_DOT)) {
                advance(p);
                Token vn = expect(p, TOK_IDENT, "field or variant name");

                // NEW: `lib.fn<T>(...)` or `Type.Variant<T>(...)`.
                // The qualified form of the generic call. Without
                // this, the `<` is read as a less-than operator
                // and the whole thing parses as a comparison
                // chain. `peek_generic_call` is the same
                // lookahead used for bare generic calls.
                if (check(p, TOK_LT) && peek_generic_call(p)) {
                    advance(p);   // '<'
                    TypeExpr* g = type_expr_generic(t, t.span);
                    if (!check(p, TOK_GT)) {
                        do {
                            type_expr_generic_add_arg(g, parse_type_expr(p));
                        } while (match(p, TOK_COMMA));
                    }
                    expect(p, TOK_GT, "'>' after type arguments");
    
                    AstNode* vc = ast_variant_ctor(t, vn);
                    vc->as.variant_ctor.instantiation = g;
                    if (check(p, TOK_LPAREN)) {
                        advance(p);
                        if (!check(p, TOK_RPAREN)) {
                            do {
                                ast_list_push(&vc->as.variant_ctor.args,
                                              expression(p));
                            } while (match(p, TOK_COMMA));
                        }
                        expect(p, TOK_RPAREN, "')'");
                    }
                    return vc;
                }
                
                if (check(p, TOK_LPAREN)) {
                    advance(p);
                    AstNode* vc = ast_variant_ctor(t, vn);
                    if (!check(p, TOK_RPAREN)) {
                        do {
                            ast_list_push(&vc->as.variant_ctor.args,
                                          expression(p));
                        } while (match(p, TOK_COMMA));
                    }
                    expect(p, TOK_RPAREN, "')'");
                    return vc;
                }
                AstNode* var = ast_var(t);
                return ast_field(var, vn);
            }

            // -- Non-generic struct literal: `Name { field: v }`.
            // Suppressed when restrict_struct_lit is set; see
            // the parser state struct comment and the two call
            // sites (match_statement and for_statement).
            if (!p->restrict_struct_lit && check(p, TOK_LBRACE)) {
                advance(p);
                AstNode* lit = ast_struct_lit(t);
                if (!check(p, TOK_RBRACE)) {
                    do {
                        Token fn = expect(p, TOK_IDENT, "field name");
                        expect(p, TOK_COLON, "':' after field name");
                        AstNode* v = expression(p);
                        ast_list_push(&lit->as.struct_lit.inits,
                                      ast_struct_init(fn, v));
                    } while (match(p, TOK_COMMA) && !check(p, TOK_RBRACE));
                }
                Token rb = expect(p, TOK_RBRACE, "'}'");
                lit->span = (Span){ t.span.start, rb.span.end, t.span.file_id };
                return lit;
            }

            // Not a call, not dotted, not a struct literal:
            // a plain variable reference.
            return ast_var(t);
        }

        // Parenthesised expression: `(expr)`. The parentheses
        // are not represented in the AST; the inner expression
        // is returned directly. That means the parenthesised
        // expression's span is the inner expression's span,
        // not the wider `(...)` span. Sema and codegen do not
        // rely on the outer span, so this is fine today.
        case TOK_LPAREN: {
            advance(p);
            AstNode* e = expression(p);
            expect(p, TOK_RPAREN, "')'");
            return e;
        }
        // Array literal: `[e1, e2, ...]`. Empty `[]` is
        // accepted. The span is widened after the loop to
        // cover the closing `]`.
        case TOK_LBRACKET: {
            advance(p);
            AstNode* lit = ast_array_lit(t.span);
            if (!check(p, TOK_RBRACKET)) {
                do {
                    ast_list_push(&lit->as.array_lit.elements, expression(p));
                } while (match(p, TOK_COMMA));
            }
            Token rb = expect(p, TOK_RBRACKET, "']'");
            lit->span = (Span){ t.span.start, rb.span.end, t.span.file_id };
            return lit;
        }
        // Anything else: report and produce a placeholder. The
        // `advance(p)` here is important: without it, the
        // caller (postfix, unary, ...) would loop forever on
        // the same token.
        default: {
            diag_error(p->bag, p->cur.span,
                "expected expression, got %s", token_kind_name(t.kind));
            // Closing delimiters are left in place so the
            // caller's `expect` can produce a targeted error
            // (e.g. "expected ')'") and recover to a block
            // or list boundary. Advancing past them loses the
            // caller's context and cascades into an EOF error.
            bool is_closer = (t.kind == TOK_RPAREN
                              || t.kind == TOK_RBRACKET
                              || t.kind == TOK_RBRACE);
            if (!is_closer) advance(p);
            return ast_int(t.span, 0);
        }
    }
}

// Unary operators.
//
//   `-e`  — arithmetic negation.
//   `!e`  — logical not.
//   `*e`  — dereference.
//   `&[mut] e` — arena allocation (borrow-then-allocate).
//               `&mut e` marks the result as mutable.
//   `rc e`     — RC-heap allocation.
//
// Precedence: all unary operators bind tighter than any
// binary operator and looser than postfix (so `*p.x` is
// `*(p.x)`, and `-f()` is `-(f())`).
//
// Right-recursion: the unary branches call `unary(p)` (not
// `expression(p)`) so `--x` and `*&x` parse as nested
// unaries. This is the standard rule.
static AstNode* unary(Parser* p) {
    Token t = p->cur;

    if (match(p, TOK_MINUS)) return ast_unary(UN_NEG, unary(p), t.span);
    if (match(p, TOK_BANG))  return ast_unary(UN_NOT, unary(p), t.span);
    if (match(p, TOK_STAR))  return ast_deref(unary(p), t.span);

    // `&e` or `&mut e`. Both map to AST_ALLOC_ARENA; the flag
    // distinguishes them.
    if (match(p, TOK_AMP)) {
        bool is_mut = match(p, TOK_MUT);
        AstNode* operand = unary(p);
        return ast_alloc_arena(operand, t.span, is_mut);
    }

    // `rc e`. No mutability flag; RC handles are always
    // mutable through the handle.
    if (match(p, TOK_RC)) {
        return ast_alloc_rc(unary(p), t.span);
    }

    return postfix(p);
}

// One level of the binary precedence chain.
//
// The level is described by three parallel arrays: the token
// kinds this level recognizes, the BinOp each maps to, and a
// count. The function first calls `next` (the next-tighter
// level) to get a left operand, then loops as long as the
// current token is one of `ops`. For each match, it consumes
// the operator, calls `next` again for the right operand, and
// folds them into an AST_BINARY.
//
// Associativity: all binary operators in Prism are
// left-associative. The loop shape (`left = bin(op, left,
// right)`) implements that directly.
//
// Precedence: the caller (`cast_expr`, `mul_expr`, ...)
// chooses which level this call represents by passing `next`
// as the next-tighter level function. `eq_expr` is the
// top-level entry (via `expression`), and `cast_expr` is the
// bottom (its `next` is `unary`).
//
// The `ops` and `bins` arrays must have the same length `n`;
// that invariant is enforced by construction at each call
// site and not checked here.
static AstNode* binary_chain(Parser* p,
                             AstNode* (*next)(Parser*),
                             const TokenKind* ops,
                             const BinOp*     bins,
                             int              n) {
    AstNode* left = next(p);
    for (;;) {
        int found = -1;
        for (int i = 0; i < n; i++) {
            if (check(p, ops[i])) { found = i; break; }
        }
        if (found < 0) return left;
        Token op = p->cur;
        advance(p);
        AstNode* right = next(p);
        left = ast_binary(bins[found], left, right, op.span);
    }
}

// Cast operator: `e as T`, left-associative and tighter than
// any binary operator but looser than unary.
//
// Written as its own function rather than a binary_chain call
// because the right-hand side is a TypeExpr, not an
// expression. The loop allows chained casts: `x as i32 as
// i64`.
//
// The span of a cast node starts at the operand and ends at
// the end of the last type expression, which `p->prev` points
// to after parse_type_expr returns.
static AstNode* cast_expr(Parser* p) {
    AstNode* e = unary(p);
    while (match(p, TOK_AS)) {
        TypeExpr* target = parse_type_expr(p);
        Span full = { e->span.start, p->prev.span.end, e->span.file_id };
        e = ast_cast(e, target, full);
    }
    return e;
}

// The five binary precedence levels. Each is a thin wrapper
// over binary_chain. The operator tables are `static const`
// so they live in read-only memory and are not re-created on
// every call.
//
// Precedence, loosest to tightest: eq, cmp, add, mul. That
// matches C's ordering for these operators (C uses `==` below
// relational, relational below additive, additive below
// multiplicative), which is what most programmers expect.
//
// `expression` is the entry point used by everything outside
// this section; it forwards to the loosest level.
static AstNode* mul_expr(Parser* p) {
    static const TokenKind ops[] = { TOK_STAR, TOK_SLASH };
    static const BinOp     bins[] = { BIN_MUL, BIN_DIV };
    return binary_chain(p, cast_expr, ops, bins, 2);
}
static AstNode* add_expr(Parser* p) {
    static const TokenKind ops[] = { TOK_PLUS, TOK_MINUS };
    static const BinOp     bins[] = { BIN_ADD, BIN_SUB };
    return binary_chain(p, mul_expr, ops, bins, 2);
}
static AstNode* cmp_expr(Parser* p) {
    static const TokenKind ops[] = { TOK_LT, TOK_LE, TOK_GT, TOK_GE };
    static const BinOp     bins[] = { BIN_LT, BIN_LE, BIN_GT, BIN_GE };
    return binary_chain(p, add_expr, ops, bins, 4);
}
static AstNode* eq_expr(Parser* p) {
    static const TokenKind ops[] = { TOK_EQEQ, TOK_BANGEQ };
    static const BinOp     bins[] = { BIN_EQ,   BIN_NE };
    return binary_chain(p, cmp_expr, ops, bins, 2);
}

static AstNode* and_expr(Parser* p) {
    static const TokenKind ops[] = { TOK_AMPAMP };
    static const BinOp     bins[] = { BIN_AND };
    return binary_chain(p, eq_expr, ops, bins, 1);
}
static AstNode* or_expr(Parser* p) {
    static const TokenKind ops[] = { TOK_PIPEPIPE };
    static const BinOp     bins[] = { BIN_OR };
    return binary_chain(p, and_expr, ops, bins, 1);
}
static AstNode* expression(Parser* p) { return or_expr(p); }

// ============================================================
// Patterns and match
//
// Pattern grammar:
//   pattern := '_'
//            | IDENT
//            | IDENT '<' type (',' type)* '>' '.' IDENT ('(' pat (',' pat)* ')')?
//            | IDENT '.' IDENT ('(' pat (',' pat)* ')')?
//            | INT
//            | 'true'
//            | 'false'
//            | STRING
//
// The wildcard `_` is lexed as an identifier of length 1 and
// recognized by inspecting the source byte. This is a
// deliberate shortcut: `_` is not a keyword, and treating it
// as a special identifier keeps the lexer simpler. The cost
// is that any other one-character identifier starting with
// `_` (i.e. exactly `_`) would also be a wildcard, but `_`
// is the only such identifier by construction.
// ============================================================
static AstNode* parse_struct_body(Parser* p, Token name, Span start_span);
static AstNode* parse_pattern(Parser* p) {
    Token t = p->cur;

    // Wildcard: an identifier whose source text is exactly
    // "_". The span_len check (== 1) makes this a cheap
    // pre-filter before inspecting the source byte.
    if (check(p, TOK_IDENT) && span_len(t.span) == 1
        && p->lex->source[t.span.start] == '_') {
        advance(p);
        return ast_pat_wildcard(t.span);
    }

    // Any other identifier: a binding, a bare variant, or a
    // generic variant pattern. All three start with an
    // identifier and then look at what follows.
    if (match(p, TOK_IDENT)) {
        // Optional generic arguments: `Option<i64>.Some(x)`.
        TypeExpr* inst = NULL;
        if (check(p, TOK_LT)) {
            advance(p);
            inst = type_expr_generic(t, t.span);
            if (!check(p, TOK_GT)) {
                do {
                    type_expr_generic_add_arg(inst, parse_type_expr(p));
                } while (match(p, TOK_COMMA));
            }
            expect(p, TOK_GT, "'>' after type arguments");
        }

        // Dot: variant pattern. The instantiation, if any, is
        // attached to the PAT_VARIANT node.
        if (check(p, TOK_DOT)) {
            advance(p);
            Token vn = expect(p, TOK_IDENT, "variant name");
            AstNode* vp = ast_pat_variant(t, vn, t.span);
            vp->as.pat_variant.instantiation = inst;
            if (match(p, TOK_LPAREN)) {
                if (!check(p, TOK_RPAREN)) {
                    do {
                        ast_list_push(&vp->as.pat_variant.subpatterns,
                                      parse_pattern(p));
                    } while (match(p, TOK_COMMA));
                }
                expect(p, TOK_RPAREN, "')'");
            }
            return vp;
        }

        // Bare variant pattern with payload: `Some(x)`.
        // Written with a zero type_name; sema resolves it
        // against the scrutinee's enum. Unambiguous because
        // a bind pattern can never be followed by `(`.
        if (check(p, TOK_LPAREN)) {
            advance(p);
            AstNode* vp = ast_pat_variant((Token){0}, t, t.span);
            if (!check(p, TOK_RPAREN)) {
                do {
                    ast_list_push(&vp->as.pat_variant.subpatterns,
                                  parse_pattern(p));
                } while (match(p, TOK_COMMA));
            }
            expect(p, TOK_RPAREN, "')'");
            return vp;
        }
        
        // No dot: this is a plain binding. If `inst` was
        // parsed (i.e. the user wrote `Name<T>` in a binding
        // position, which is not legal), report and free the
        // unused TypeExpr. Freeing here is important because
        // ownership of the TypeExpr was transferred to us and
        // it is not being attached to any node.
        if (inst) {
            diag_error(p->bag, p->cur.span,
                "expected '.' after type arguments in pattern");
            type_expr_free(inst);
        }
        return ast_pat_bind(t);
    }

    // Literal patterns. Each consumes one token and produces
    // a corresponding PAT_* node.
    if (check(p, TOK_INT)) {
        advance(p);
        return ast_pat_int(t.span, (I64)parse_int_value(p, t));
    }
    if (match(p, TOK_TRUE))  return ast_pat_bool(t.span, true);
    if (match(p, TOK_FALSE)) return ast_pat_bool(t.span, false);
    if (check(p, TOK_STRING)) {
        advance(p);
        return ast_pat_string(t.span, t);
    }

    // Anything else is not a pattern. Report, advance past
    // the offending token so the caller's loop makes progress,
    // and return a wildcard as a placeholder.
    diag_error(p->bag, t.span, "expected pattern");
    advance(p);
    return ast_pat_wildcard(t.span);
}

// `match scrutinee { arm ... }`.
//
// The scrutinee is parsed with restrict_struct_lit set, so
// that an identifier immediately before the opening `{` is
// not misread as a struct-literal type name. Without this,
// `match x { ... }` would try to parse `x { ... }` as a
// struct literal and then find no arms.
//
// Each arm is `pattern => body`. The body is a block
// (curly-braced). On error in any of the three parts
// (pattern, `=>`, body), the arm is abandoned and the loop
// breaks; the parser is left just before whatever token
// caused the failure, and the closing `}` is consumed by the
// expect at the end.
//
// The error-count check is what makes this robust: instead of
// trying to recover mid-arm (which would require a decision
// about where the next arm starts), the parser simply stops
// the match and lets parse_program's outer recovery handle
// the rest.
//
// Optional commas between arms are accepted, as are
// semicolons between arms (the `if (match(SEMI)) continue;`
// at the top of the loop), so a user can write
//   `match x { A => {...} B => {...} }`
// or
//   `match x { A => {...}, B => {...}, }`
// or even
//   `match x { A => {...}; B => {...} }`.
static AstNode* match_statement(Parser* p) {
    Token kw = p->prev;

    // Suppress struct-literal parsing for the scrutinee, so
    // that `match x { ... }` does not read the arm-brace as
    // a struct literal. Restore the previous value
    // afterwards (the flag can be nested inside another
    // expression context).
    bool prev_restrict = p->restrict_struct_lit;
    p->restrict_struct_lit = true;
    AstNode* scrutinee = expression(p);
    p->restrict_struct_lit = prev_restrict;

    expect(p, TOK_LBRACE, "'{' after match scrutinee");

    AstNode* m = ast_match(scrutinee, kw.span);

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        // Blank statements between arms are tolerated.
        if (match(p, TOK_SEMI)) continue;

        // `before` snapshots the diagnostic count so we can
        // detect whether *this* arm produced any error. If
        // it did, we stop parsing arms entirely; trying to
        // resync inside the braces would require guessing
        // where the next arm starts.
        int   before = p->bag->error_count;
        Token arm_start = p->cur;

        AstNode* pat = parse_pattern(p);
        if (p->bag->error_count > before) break;

        expect(p, TOK_FATARROW, "'=>' after pattern");
        if (p->bag->error_count > before) break;

        // Arm body: any statement. A block `{ ... }` still
        // works (it is a statement); a bare expression such as
        // `print(n)` is wrapped by the statement parser into an
        // expression statement. The match loop's optional comma
        // remains available to separate arms on one line; on
        // multiple lines, ASI terminates the expression statement
        // and the next arm starts on the following line.
        AstNode* body = statement(p);
        if (!body) {
            diag_error(p->bag, arm_start.span,
                "match arm body cannot be empty");
            break;
        }
        if (p->bag->error_count > before) break;

        // Arm span from the start of the pattern to the end
        // of the body, which is where p->prev points after
        // block() returns.
        AstNode* arm = ast_match_arm(pat, body,
            (Span){ arm_start.span.start, p->prev.span.end,
                    arm_start.span.file_id });
        ast_list_push(&m->as.match.arms, arm);

        // Optional comma after the arm. Ignored if absent.
        match(p, TOK_COMMA);
    }

    Token rb = expect(p, TOK_RBRACE, "'}'");
    m->span = (Span){ kw.span.start, rb.span.end, kw.span.file_id };
    return m;
}

// ============================================================
// Statements
//
// A statement is one of:
//   let / if / while / for / match / break / continue /
//   return / block / assignment / expression statement.
//
// All statement productions return an AstNode* (or NULL for
// a bare `;`, which is consumed and dropped). The caller
// (block() or statement()) is responsible for pushing
// non-NULL nodes into a block's stmts list.
// ============================================================

// `let [mut] name [: T] = init;`.
//
// `let_kw` is the `let` token, already consumed by the
// caller; it is used for the node's span start.
//
// The initializer is *required* (`expect(p, TOK_ASSIGN, ...)`
// without an optional path). A declaration-only `let` is not
// currently supported, even though the AST_LET node allows
// `init == NULL`. If a future revision supports
// `let x: i64;`, the change is in this function.
//
// Span: the node's span covers from `let` through the end of
// the initializer (or the terminator, whichever the AST
// requires). The end offset is taken from `p->prev.span.end`
// after end_statement returns, which is the terminator's end
// if a `;` was consumed, or the previous token's end if a
// `}` was left for the caller.
static AstNode* let_declaration(Parser* p, Token let_kw) {
    bool is_mut = match(p, TOK_MUT);
    Token name = expect(p, TOK_IDENT, "variable name");

    // Optional type annotation. NULL means "infer".
    TypeExpr* type_expr = NULL;
    if (match(p, TOK_COLON)) type_expr = parse_type_expr(p);

    expect(p, TOK_ASSIGN, "'=' after variable name");
    AstNode* init =  expression(p);
    end_statement(p, "';' after variable declaration");

    AstNode* n = ast_let(name, init);
    n->as.let.type_expr = type_expr;
    n->as.let.is_mut    = is_mut;
    // Widen the span to start at `let`.
    n->span = (Span){ let_kw.span.start, p->prev.span.end, let_kw.span.file_id};
    return n;
}

// `if (cond) { ... } [else { ... }]`.
//
// Note that the condition is always parenthesised in the
// source grammar. There is no paren-less `if x { }` form.
// This is why the `expect(LPAREN)` and `expect(RPAREN)`
// calls are mandatory rather than optional.
//
// `else` attaches to the nearest `if`. The parser does not
// support `else if` as a special form; a chain of `else if`
// works because `block()` is called for the else branch and
// the block starts with `if` (parsed as a nested if
// statement). The result is a right-leaning tree of AST_IF
// nodes.
static AstNode* if_statement(Parser* p) {
    Token kw = p->prev;
    expect(p, TOK_LPAREN, "'(' after 'if'");
    AstNode* cond = expression(p);
    expect(p, TOK_RPAREN, "')' after condition");
    AstNode* then_ = block(p);
    AstNode* else_ = NULL;
    if (match(p, TOK_ELSE)) else_ = block(p);
    AstNode* n = ast_if(cond, then_, else_);
    n->span = (Span){ kw.span.start, p->prev.span.end, kw.span.file_id };
    return n;
}

// `while (cond) { ... }`. Same parenthesisation rule as `if`.
static AstNode* while_statement(Parser* p) {
    Token kw = p->prev;
    expect(p, TOK_LPAREN, "'(' after 'while'");
    AstNode* cond = expression(p);
    expect(p, TOK_RPAREN, "')' after condition");
    AstNode* body = block(p);
    AstNode* n = ast_while(cond, body);
    n->span = (Span){ kw.span.start, p->prev.span.end, kw.span.file_id };
    return n;
}

// `for var in iterable { ... }` OR
// `for var in start..end { ... }`.
//
// The two forms are syntactically distinguished by the
// presence of `..` after the first expression. The parser
// reads the first expression and then checks for `..`:
//   - If found, parse the end expression and build a
//     FOR_RANGE node.
//   - Otherwise, build a FOR_IN node with the first
//     expression as the iterable.
//
// The first expression is parsed with restrict_struct_lit
// set, because the `{` that starts the body would otherwise
// be a candidate struct-literal opener when the iterable is
// a bare identifier. Example: `for x in things { ... }` would
// without the flag try to read `things { ... }` as a struct
// literal.
//
// The end expression (if any) is also parsed with the flag
// still set. That is intentional: the body's `{` follows the
// end expression, and the same ambiguity applies.
static AstNode* for_statement(Parser* p) {
    Token kw = p->prev;

    Token var = expect(p, TOK_IDENT, "loop variable name");
    expect(p, TOK_IN, "'in' after loop variable");

    bool prev = p->restrict_struct_lit;
    p->restrict_struct_lit = true;

    AstNode* first = expression(p);

    if (match(p, TOK_DOTDOT)) {
        AstNode* end = expression(p);
        p->restrict_struct_lit = prev;
        AstNode* body = block(p);
        AstNode* n = ast_for_range(var, first, end, body);
        n->span = (Span){ kw.span.start, p->prev.span.end, kw.span.file_id };
        return n;
    }

    p->restrict_struct_lit = prev;
    AstNode* body = block(p);
    AstNode* n = ast_for_in(var, first, body);
    n->span = (Span){ kw.span.start, p->prev.span.end, kw.span.file_id };
    return n;
}

// `return [expr];`. The value is optional: a `return;` or a
// `return` immediately before `}` produces a RETURN node
// with value == NULL.
//
// The "immediately before `}`" case matters because the
// lexer's ASI inserts a semicolon before the `}`, so a
// `return` at the end of a block is followed by a synthetic
// `;`. Both checks (SEMI and RBRACE) are needed because the
// ASI behavior depends on the preceding token; a `return`
// followed directly by `}` without a newline is also legal.
static AstNode* return_statement(Parser* p) {
    Token kw = p->prev;
    AstNode* value = NULL;
    if (!check(p, TOK_SEMI) && !check(p, TOK_RBRACE))
        value = expression(p);
    end_statement(p, "';' after return");
    AstNode* n = ast_return(kw.span, value);
    n->span = (Span){ kw.span.start, p->prev.span.end, kw.span.file_id };
    return n;
}

// Parse one statement.
//
// Returns the AST node, or NULL for a bare `;` (which is
// consumed and discarded as a no-op). Every other path
// returns a node.
//
// Error recovery: the `before` snapshot lets the function
// detect whether this statement produced any diagnostics.
// If it did, synchronize() is called to skip to the next
// statement boundary before returning. This is the single
// recovery point for all statement-level errors; the
// individual productions do not attempt recovery themselves.
//
// The one refinement: if `n` is NULL at recovery time (which
// happens only if the error occurred before the statement
// production was chosen), a placeholder AST_INT is created
// so the caller has something to push into the block. This
// is only relevant when the error happened at the top of
// statement() itself, e.g. an unexpected token that does not
// start any statement.
static AstNode* statement(Parser* p) {
    int before = p->bag->error_count;

    // Bare `;`: consume and return NULL. The caller will not
    // push anything into the block.
    if (check(p, TOK_SEMI)) {
        advance(p);
        return NULL;
    }

    Token kw = p->cur;
    AstNode* n = NULL;

    // Dispatch on the leading keyword. Each branch consumes
    // the keyword and calls the corresponding production.
    // Note that the break/continue branches call
    // end_statement themselves (they have no body to parse
    // and must consume the `;` before returning).
    if (match(p, TOK_LET)) {
        n = let_declaration(p, kw);
    } else if (match(p, TOK_IF)) {
        n = if_statement(p);
    } else if (match(p, TOK_WHILE)) {
        n = while_statement(p);
    } else if (match(p, TOK_FOR)) {
        n = for_statement(p);
    } else if (match(p, TOK_MATCH)) {
        n = match_statement(p);
    } else if (match(p, TOK_BREAK)) {
        n = ast_break(p->prev.span);
        end_statement(p, "';' after 'break'");
    } else if (match(p, TOK_CONTINUE)) {
        n = ast_continue(p->prev.span);
        end_statement(p, "';' after 'continue'");
    } else if (match(p, TOK_RETURN)) {
        n = return_statement(p);
    } else if (check(p, TOK_LBRACE)) {
        n = block(p);
    } else {
        // Expression statement or assignment. The parser
        // cannot distinguish these at the top: it parses an
        // expression, then checks for `=`. If present, the
        // result is an ASSIGN with that expression as target;
        // otherwise, an EXPR_STMT wrapping the expression.
        //
        // No target validation here (the target could be a
        // call, which is not a legal LHS); sema handles that.
        AstNode* e = expression(p);

        BinOp compound_op;
        bool  is_compound = false;
        switch (p->cur.kind) {
            case TOK_ASSIGN:       is_compound = false; break;
            case TOK_PLUS_ASSIGN:  compound_op = BIN_ADD; is_compound = true; break;
            case TOK_MINUS_ASSIGN: compound_op = BIN_SUB; is_compound = true; break;
            case TOK_STAR_ASSIGN:  compound_op = BIN_MUL; is_compound = true; break;
            case TOK_SLASH_ASSIGN: compound_op = BIN_DIV; is_compound = true; break;
            default: break;
        }

        if (is_compound || check(p, TOK_ASSIGN)) {
            advance(p);   // consume the operator
            AstNode* v = expression(p);
            end_statement(p, "';'");
            n = is_compound ? ast_assign_op(compound_op, e, v)
                            : ast_assign(e, v);
            n->span = (Span){ e->span.start, v->span.end, e->span.file_id };
        } else {
            end_statement(p, "';'");
            n = ast_expr_stmt(e);
        }
    }

    // Single recovery point. See function header.
    if (p->bag->error_count > before) {
        if (!n) n = ast_int(p->cur.span, 0);
        synchronize(p);
    }
    return n;
}

// Parse a `{ ... }` block.
//
// The opening brace is required (the caller has already
// verified that the current token is `{` in most cases, but
// block() also calls expect so a caller that passes a
// non-brace token gets a proper diagnostic).
//
// The loop parses statements until `}` or EOF. Each
// iteration takes an error-count snapshot; if a statement
// produced an error, `panic` is set on the parser, which
// affects how the *next* statement's caller may behave. (In
// practice, statement() already ran synchronize after its
// own error, so the panic flag is mostly belt-and-braces.)
//
// The closing `}` is consumed by the final expect. Its
// failure mode is a missing brace, which is reported with
// a clear message.
static AstNode* block(Parser* p) {
    Token brace = expect(p, TOK_LBRACE, "'{'");
    AstNode* b = ast_block(brace.span);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        int before = p->bag->error_count;
        AstNode* s = statement(p);
        if (s) ast_list_push(&b->as.block.stmts, s);
        if (p->bag->error_count > before) p->panic = true;
    }
    expect(p, TOK_RBRACE, "'}'");
    return b;
}

// ============================================================
// Top-level declarations
// ============================================================

// `fn name(params) : ret { body }` or
// `fn Owner.name(params) : ret { body }`.
//
// `fn_kw` is the `fn` token, already consumed by the caller.
//
// Method detection: if the token after the function name is
// `.`, the function is a method. The name before the dot is
// stored as `method_owner`; the name after is the actual
// method name. A method declaration looks like:
//     fn Point.move(self: ref<Point>) { ... }
// where `Point` is the owner and `move` is the method.
//
// Parameter parsing: an inline loop rather than a helper,
// because the arrays are stack-local and small. Up to 8
// parameters are stored; extras are diagnosed and dropped
// (after freeing their TypeExpr, which the parser owns at
// that point).
//
// The body is a block, always. Extern declarations (no body)
// go through extern_fn_declaration instead.
static AstNode* function_declaration(Parser* p, Token fn_kw) {
    Token name = expect(p, TOK_IDENT, "function name");

    // Method: `fn Owner.name(...)`. Rewrite `name` to the
    // method name and remember the owner separately.
    Token method_owner = {0};
    if (check(p, TOK_DOT)) {
        advance(p);
        method_owner = name;
        name = expect(p, TOK_IDENT, "method name after '.'");
    }

    Token tparams[8];
    int   tparam_count = parse_type_params(p, tparams, 8);

    expect(p, TOK_LPAREN, "'(' after function name");

    // Three parallel arrays for the parameters. Using
    // separate arrays (rather than an array of Param) avoids
    // needing Param to be visible here, but is otherwise
    // identical.
    Token    pnames[8];
    TypeExpr* ptypes[8];
    bool     pmuts[8];
    int      pcount = 0;

    if (!check(p, TOK_RPAREN)) {
        do {
            bool is_mut = match(p, TOK_MUT);
            Token pn = expect(p, TOK_IDENT, "parameter name");
            TypeExpr* pt = NULL;
            if (match(p, TOK_COLON)) pt = parse_type_expr(p);

            if (pcount < 8) {
                pnames[pcount] = pn;
                ptypes[pcount] = pt;
                pmuts[pcount]  = is_mut;
                pcount++;
            } else {
                diag_error(p->bag, pn.span, "too many parameters (max 8)");
                type_expr_free(pt);
            }
        } while (match(p, TOK_COMMA));
    }
    expect(p, TOK_RPAREN, "')' after parameters");

    // Optional return type. Absent means "unit" (void).
    TypeExpr* ret_expr = NULL;
    if (match(p, TOK_COLON)) ret_expr = parse_type_expr(p);

    // Signature recovery. If the parameter list or the return
    // type failed, panic is set. Skip forward to either the
    // opening brace of this function's body (so we can still
    // parse the body and surface its diagnostics) or a
    // top-level starter (so parse_program can resync cleanly).
    // The 8-parameter and 8-type-param arrays stay partially
    // filled; `pcount` and `tparam_count` reflect whatever
    // we did manage to read.
    if (p->panic) {
        while (!check(p, TOK_LBRACE) && !check(p, TOK_EOF)
               && !check(p, TOK_FN)    && !check(p, TOK_CONST)
               && !check(p, TOK_TYPE)  && !check(p, TOK_USING)
               && !check(p, TOK_PUB)   && !check(p, TOK_EXTERN))
            advance(p);
        // If we stopped at a top-level starter, return a stub
        // function node; parse_program will pick up from here.
        if (!check(p, TOK_LBRACE)) {
            AstNode* f = ast_func(name, NULL);
            f->span = (Span){ fn_kw.span.start, p->prev.span.end,
                              fn_kw.span.file_id };
            return f;
        }
        // We found `{`; parse the body so its own errors
        // surface in the right context.
    }
    

    AstNode* body = block(p);

    // Assemble the AST node. The constructor allocates and
    // sets most fields; this function fills in the ones that
    // the constructor left at their defaults.
    AstNode* f = ast_func(name, body);
    for (int i = 0; i < pcount; i++) {
        f->as.func.params[i].name      = pnames[i];
        f->as.func.params[i].type_expr = ptypes[i];
        f->as.func.params[i].type      = NULL;
        f->as.func.params[i].is_mut    = pmuts[i];
    }
    f->as.func.param_count      = pcount;
    f->as.func.return_type_expr = ret_expr;
    f->as.func.type_param_count = tparam_count;
    for (int i = 0; i < tparam_count; i++)
        f->as.func.type_params[i] = tparams[i];

    f->span = (Span){ fn_kw.span.start, p->prev.span.end, fn_kw.span.file_id };
    f->as.func.method_owner = method_owner;
    return f;
}

// extern lib "path" (as ident)?
// extern fn name(params) : ret from alias
//
// `extern_kw` is the `extern` token, already consumed. The
// current token is `fn`, which is consumed by advance at the
// top. (Note the local `fn_kw` capture is unused apart from
// a `(void)fn_kw` at the end; it exists so the sub-grammar
// reads naturally. Preserved as-is.)
//
// Parameter parsing here differs from function_declaration in
// three ways:
//   1. `...` is accepted as the final "parameter" and sets
//      is_variadic. It must appear after a comma (or as the
//      only parameter) and terminates the list.
//   2. `mut` on a parameter is diagnosed and dropped, because
//      an extern function has no body and so has no place to
//      use mutability. The `mut` token is still consumed so
//      parsing continues.
//   3. The return type annotation is required. If the colon
//      is missing, a diagnostic is reported but the parse
//      continues (panic is set; the caller will recover).
//      The `parse_type_expr` after the failed colon will
//      produce a NULL if the current token is not a type,
//      which is fine because the resulting node is going to
//      be discarded at the first sign of trouble anyway.
//
// The `from lib_alias` clause is mandatory and terminates the
// declaration. The alias names the library the extern binds
// to; it must match a previously-declared `extern lib`
// (checked by sema, not here).
static AstNode* extern_fn_declaration(Parser* p, Token extern_kw) {
    Token fn_kw = p->cur;                // 'fn' is the current token
    advance(p);

    Token name = expect(p, TOK_IDENT, "function name");
    expect(p, TOK_LPAREN, "'(' after function name");

    Token     pnames[8];
    TypeExpr* ptypes[8];
    int       pcount = 0;
    bool      is_variadic = false;

    if (!check(p, TOK_RPAREN)) {
        for (;;) {
            // `...` terminates the parameter list. The break
            // exits the for(;;) and leaves the current token
            // just after the ellipsis; the outer expect(RPAREN)
            // will then expect `)`.
            if (check(p, TOK_ELLIPSIS)) {
                advance(p);
                is_variadic = true;
                break;
            }
            bool is_mut = match(p, TOK_MUT);
            (void)is_mut;
            Token pn = expect(p, TOK_IDENT, "parameter name");
            // A second `mut` after the name is an error (the
            // first `mut` already consumed the prefix form).
            // Report but keep parsing.
            if (match(p, TOK_MUT)) {
                diag_error(p->bag, p->prev.span,
                    "extern parameters cannot be marked 'mut'; "
                    "extern functions have no body");
            }
            TypeExpr* pt = NULL;
            if (match(p, TOK_COLON)) pt = parse_type_expr(p);
            if (pcount < 8) {
                pnames[pcount] = pn;
                ptypes[pcount] = pt;
                pcount++;
            } else {
                diag_error(p->bag, pn.span, "too many parameters (max 8)");
                type_expr_free(pt);
            }
            // Comma between parameters. If absent, the loop
            // ends (the next token should be `)`).
            if (!match(p, TOK_COMMA)) break;
        }
    }
    expect(p, TOK_RPAREN, "')' after parameters");

    // Return type is required. The `if (!match(...))` reports
    // a diagnostic but continues; parse_type_expr may still
    // produce a valid TypeExpr if the current token happens
    // to be a type, which is the "user forgot the colon but
    // wrote the type anyway" case.
    if (!match(p, TOK_COLON)) {
        diag_error(p->bag, p->cur.span,
            "extern fn requires a return type annotation (': type')");
        p->panic = true;
    }
    TypeExpr* ret_expr = parse_type_expr(p);

    expect(p, TOK_FROM, "'from' clause after return type");
    Token lib_alias = expect(p, TOK_IDENT, "library alias");
    end_statement(p, "';' after extern fn");

    AstNode* f = ast_func(name, NULL);   // NULL body: extern fn
    for (int i = 0; i < pcount; i++) {
        f->as.func.params[i].name      = pnames[i];
        f->as.func.params[i].type_expr = ptypes[i];
        f->as.func.params[i].type      = NULL;
    }
    f->as.func.param_count      = pcount;
    f->as.func.return_type_expr = ret_expr;
    f->as.func.type_param_count = 0;    // externs cannot be generic
    f->as.func.is_extern        = true;
    f->as.func.is_variadic      = is_variadic;
    f->as.func.lib_alias        = lib_alias;
    f->as.func.ffi_idx          = -1;   // assigned by sema

    f->span = (Span){ extern_kw.span.start, p->prev.span.end, extern_kw.span.file_id };
    (void)fn_kw;
    return f;
}

// Dispatch on the kind of `extern` declaration.
//
// Two forms:
//   extern lib "path" [as alias];    -> AST_EXTERN_LIB
//   extern fn ...                     -> extern_fn_declaration
//
// The `lib` keyword is not a reserved word; it is matched by
// name (smap_span_equals against "lib"). This lets users keep
// `lib` as an ordinary identifier elsewhere without conflict.
// The alternative would be to add a TOK_LIB keyword, but the
// current design avoids keyword proliferation.
//
// Anything else after `extern` is an error: report, set
// panic, return NULL. The caller (parse_program or
// pub_declaration) will recover.
static AstNode* extern_declaration(Parser* p, Token extern_kw) {
    // extern lib "path" (as ident)?
    if (check(p, TOK_IDENT)
        && smap_span_equals(p->smap, p->cur.span, "lib")) {
        advance(p);
        if (!check(p, TOK_STRING)) {
            diag_error(p->bag, p->cur.span,
                "expected library path string after 'lib'");
            p->panic = true;
            return NULL;
        }
        Token path = p->cur;
        advance(p);
        // Optional `as ident`.
        Token alias = {0};
        if (match(p, TOK_AS)) alias = expect(p, TOK_IDENT, "library alias");
        end_statement(p, "';' after extern lib");
        AstNode* n = ast_extern_lib(path, alias);
        n->span = (Span){ extern_kw.span.start, p->prev.span.end,
                          extern_kw.span.file_id };
        return n;
    }
    if (match(p, TOK_TYPE)) {
        Token name = expect(p, TOK_IDENT, "extern type name");
        expect(p, TOK_ASSIGN, "'=' after extern type name");

        // extern type T = struct {}
        if (check(p, TOK_STRUCT)) {
            advance(p);
            AstNode* def = parse_struct_body(p, name, extern_kw.span);
            def->as.struct_def.is_extern = true;
            return def;
        }

        // extern type T = union{}
        if (check(p, TOK_IDENT)
            && smap_span_equals(p->smap, p->cur.span, "union")) {
            advance(p);
            AstNode* def = parse_struct_body(p, name, extern_kw.span);
            def->as.struct_def.is_extern = true;
            def->as.struct_def.is_union  = true;
            return def;
        }
        
        // `extern type X = opaque` — a name with no layout.
        // `opaque` is a soft keyword (an ordinary identifier
        // whose spelling is checked here); the user can still
        // define a variable named `opaque` elsewhere.
        if (check(p, TOK_IDENT)
            && smap_span_equals(p->smap, p->cur.span, "opaque")) {
            advance(p);
            AstNode* def = ast_struct_def(name);
            def->as.struct_def.is_extern = true;
            def->as.struct_def.is_opaque = true;
            def->span = (Span){ extern_kw.span.start, p->prev.span.end,
                                extern_kw.span.file_id };
            return def;
        }
        diag_error(p->bag, p->cur.span,
            "expected 'struct' or 'opaque' after '='");
        p->panic = true;
        return NULL;
    }
    if (check(p, TOK_FN)) {
        return extern_fn_declaration(p, extern_kw);
    }
    diag_error(p->bag, p->cur.span,
        "expected 'lib' or 'fn' after 'extern'");
    p->panic = true;
    return NULL;
}

// `const NAME = expr;`. The initializer is a full expression
// tree; whether it is actually constant-foldable is checked
// by sema, not here. The parser accepts any expression.
static AstNode* const_declaration(Parser* p, Token const_kw) {
    Token name = expect(p, TOK_IDENT, "constant name");

    // Optional `: Type`. When present, the constant's stored
    // type is the annotation; when absent, it is inferred
    // from the literal's kind (i64 / bool / f64).
    TypeExpr* type_expr = NULL;
    if (match(p, TOK_COLON))
        type_expr = parse_type_expr(p);

    expect(p, TOK_ASSIGN, "'=' after constant name");
    AstNode* init = expression(p);
    end_statement(p, "';' after constant declaration");

    AstNode* c = ast_const(name, init);
    c->as.const_decl.type_expr = type_expr;
    c->span = (Span){ const_kw.span.start, p->prev.span.end,
                      const_kw.span.file_id };
    return c;
}

// Parse the `{ f: T, ... }` body of a struct definition. The
// opening brace is required; a trailing comma before `}` is
// accepted. `name` is the struct's name (already consumed by
// the caller). `start_span` is the span that the resulting AST
// node's span will start at (the `type` or `extern` keyword).
//
// Shared by `type_declaration` (for `type X = struct { ... }`)
// and `extern_declaration` (for `extern type X = struct { ... }`).
// The caller may set additional flags (is_extern, type_params)
// on the returned node.
static AstNode* parse_struct_body(Parser* p, Token name, Span start_span) {
    expect(p, TOK_LBRACE, "'{'");
    AstNode* def = ast_struct_def(name);

    if (!check(p, TOK_RBRACE)) {
        do {
            Token fn = expect(p, TOK_IDENT, "field name");
            expect(p, TOK_COLON, "':' after field name");
            TypeExpr* ft = parse_type_expr(p);
            ast_list_push(&def->as.struct_def.fields,
                          ast_struct_field(fn, ft));
        } while (match(p, TOK_COMMA) && !check(p, TOK_RBRACE));
    }
    Token rb = expect(p, TOK_RBRACE, "'}'");
    def->span = (Span){ start_span.start, rb.span.end, start_span.file_id };
    return def;
}

// `type Name<T,...> = struct { ... }` or
// `type Name<T,...> = enum { ... }`.
//
// `type_kw` is the `type` token, already consumed.
//
// Two declaration forms share this function; the `struct`
// and `enum` keywords after `=` select which. Note that the
// `enum` case assigns tags 0, 1, 2, ... in declaration order
// inside the parser (a `tag` local). That is a bit
// unusual: normally tag assignment would be sema's job, but
// doing it here lets the parser produce a fully-formed
// AST_ENUM_VARIANT without a second pass. Sema may reorder
// or renumber later if the language ever supports explicit
// discriminants.
//
// Field/variant lists accept trailing commas via the
// `... && !check(RBRACE)` loop condition.
//
// Errors: a missing `struct`/`enum` after `=` is reported
// and the function returns NULL (the caller drops the
// declaration). A missing type name is reported the same
// way. A missing `{` or `}` is reported by expect and the
// parse continues with a partially-built node.
static AstNode* type_declaration(Parser* p, Token type_kw) {
    if (!check(p, TOK_IDENT)) {
        diag_error(p->bag, p->cur.span, "expected type name after 'type'");
        p->panic = true;
        return NULL;
    }
    Token name = p->cur;
    advance(p);

    Token tparams[8];
    int   tparam_count = parse_type_params(p, tparams, 8);

    expect(p, TOK_ASSIGN, "'=' after type name");

    // `= struct { ... }`
    if (match(p, TOK_STRUCT)) {
        AstNode* def = parse_struct_body(p, name, type_kw.span);
        def->as.struct_def.type_param_count = tparam_count;
        for (int i = 0; i< tparam_count; i++){
            def->as.struct_def.type_params[i] = tparams[i];
        }
        return def;
    }

    // `= enum { ... }`
    if (match(p, TOK_ENUM)) {
        expect(p, TOK_LBRACE, "'{'");
        AstNode* def = ast_enum_def(name);
        def->as.enum_def.type_param_count = tparam_count;
        for (int i = 0; i < tparam_count; i++)
            def->as.enum_def.type_params[i] = tparams[i];

        int tag = 0;
        if (!check(p, TOK_RBRACE)) {
            for (;;) {
                Token vn = expect(p, TOK_IDENT, "variant name");
                AstNode* v = ast_enum_variant(vn);
                v->as.enum_variant.tag = tag++;

                // Optional payload list: `Name(T1, T2, ...)`.
                // Wrapped as AST_TYPE_EXPR_NODE so the
                // TypeExprs fit in the generic AstList.
                if (match(p, TOK_LPAREN)) {
                    if (!check(p, TOK_RPAREN)) {
                        do {
                            TypeExpr* te = parse_type_expr(p);
                            ast_list_push(&v->as.enum_variant.payload_exprs,
                                          ast_type_expr_node(te));
                        } while (match(p, TOK_COMMA));
                    }
                    expect(p, TOK_RPAREN, "')'");
                }
                ast_list_push(&def->as.enum_def.variants, v);
                // After a variant, `,` continues the list, but
                // a `}` may also appear (trailing comma or
                // end of list). The two-level check handles
                // both: first `if (!match(COMMA)) break;`,
                // then `if (check(RBRACE)) break;` so a
                // trailing comma is fine.
                if (!match(p, TOK_COMMA)) break;
                if (check(p, TOK_RBRACE)) break;
            }
        }
        Token rb = expect(p, TOK_RBRACE, "'}'");
        def->span = (Span){ type_kw.span.start, rb.span.end,
                            type_kw.span.file_id };
        return def;
    }

    diag_error(p->bag, p->cur.span,
        "expected 'struct' or 'enum' after '='");
    p->panic = true;
    return NULL;
}

// ============================================================
// using declarations
//
// Three forms, all sharing the `using` keyword:
//
//   using a.b.c [as X];       -> USING_MODULE
//   using a.b.{x, y as z};    -> USING_SELECTIVE
//   using Name = TypeExpr;    -> USING_ALIAS
//
// The parser distinguishes them by the token after the first
// identifier:
//   - `=`            -> alias form (parse the TypeExpr).
//   - `.` `{`        -> selective form.
//   - `.` non-`{`    -> continue accumulating a dotted path.
//   - anything else  -> module form (no more path components),
//                       then optionally `as X`.
//
// The path array is a fixed stack buffer of size
// MAX_PATH_COMPONENTS (16). A longer path is reported as
// "module path too long" and the function returns NULL.
// ============================================================

static AstNode* using_declaration(Parser* p, Token using_kw) {
    Token first = expect(p, TOK_IDENT, "module path or alias name");

    // `using Name = TypeExpr;`
    if (match(p, TOK_ASSIGN)) {
        TypeExpr* te = parse_type_expr(p);
        end_statement(p, "';' after type alias");
        AstNode* n = ast_using_alias(first, te);
        n->span = (Span){ using_kw.span.start, p->prev.span.end,
                          using_kw.span.file_id };
        return n;
    }

    // Accumulate a dotted path. `first` is already in it.
    Token path[PRISM_MAX_PATH_COMPONENTS];
    int   path_count = 0;
    path[path_count++] = first;

    for (;;) {
        if (!check(p, TOK_DOT)) break;
        advance(p);

        // `.{ ... }` — selective import. The path accumulated
        // so far names the module; the braces contain the
        // list of imported names with optional aliases.
        if (check(p, TOK_LBRACE)) {
            advance(p);
            AstNode* n = ast_using_selective(path, path_count);
            if (!check(p, TOK_RBRACE)) {
                do {
                    Token sym   = expect(p, TOK_IDENT, "imported name");
                    Token alias = {0};
                    bool  has_alias = false;
                    if (match(p, TOK_AS)) {
                        alias = expect(p, TOK_IDENT, "alias name");
                        has_alias = true;
                    }
                    ast_using_selective_add(n, sym, alias, has_alias);
                } while (match(p, TOK_COMMA) && !check(p, TOK_RBRACE));
            }
            Token rb = expect(p, TOK_RBRACE, "'}'");
            n->span = (Span){ using_kw.span.start, rb.span.end,
                              using_kw.span.file_id };
            end_statement(p, "';' after selective using");
            return n;
        }

        // Another path component. Check the cap first.
        if (path_count >= PRISM_MAX_PATH_COMPONENTS) {
            diag_error(p->bag, p->cur.span, "module path too long");
            p->panic = true;
            return NULL;
        }
        path[path_count++] = expect(p, TOK_IDENT, "path component");
    }

    // No more `.`, so we have a complete module path. Optional
    // `as alias` and then the statement terminator.
    Token alias     = {0};
    bool  has_alias = false;
    if (match(p, TOK_AS)) {
        alias     = expect(p, TOK_IDENT, "alias name");
        has_alias = true;
    }

    end_statement(p, "';' after using");
    AstNode* n = ast_using_module(path, path_count, alias, has_alias);
    n->span = (Span){ using_kw.span.start, p->prev.span.end,
                      using_kw.span.file_id };
    return n;
}

// `pub` prefix. Valid on `fn`, `extern ...`, `const`, `type`.
//
// The `pub` token is already consumed by the caller (which
// captured it as `pub_kw`). This function dispatches on the
// next token, calls the corresponding declaration parser,
// and stamps `is_pub = true` on the result.
//
// The span is widened to start at `pub` in each case,
// overwriting whatever the sub-parser set. That way a
// diagnostic pointing at a `pub fn` includes the `pub`.
//
// The error case (anything else after `pub`) reports and
// returns NULL. No recovery inside this function; the caller
// (parse_program) will skip forward.
static AstNode* pub_declaration(Parser* p, Token pub_kw) {
    Token kw = p->cur;

    if (match(p, TOK_FN)) {
        AstNode* n = function_declaration(p, kw);
        if (n) {
            n->is_pub = true;
            n->span = (Span){ pub_kw.span.start, p->prev.span.end,
                              pub_kw.span.file_id };
        }
        return n;
    }
    if (match(p, TOK_EXTERN)) {
        AstNode* n = extern_declaration(p, kw);
        if (n) {
            n->is_pub = true;
            n->span = (Span){ pub_kw.span.start, p->prev.span.end,
                              pub_kw.span.file_id };
        }
        return n;
    }
    if (match(p, TOK_CONST)) {
        AstNode* n = const_declaration(p, kw);
        if (n) {
            n->is_pub = true;
            n->span = (Span){ pub_kw.span.start, p->prev.span.end,
                              pub_kw.span.file_id };
        }
        return n;
    }
    if (match(p, TOK_TYPE)) {
        AstNode* n = type_declaration(p, kw);
        if (n) {
            n->is_pub = true;
            n->span = (Span){ pub_kw.span.start, p->prev.span.end,
                              pub_kw.span.file_id };
        }
        return n;
    }

    diag_error(p->bag, p->cur.span,
        "expected 'fn', 'const', or 'type' after 'pub'");
    p->panic = true;
    return NULL;
}

// ============================================================
// Entry point
// ============================================================

// Parse one source file into an AST_PROGRAM.
//
// Initialization:
//   - Zero the Parser state (all fields NULL/false/0).
//   - Wire lex, bag, smap from the caller.
//   - Call advance once to load the first real token into
//     `cur`. Before this call, `cur` is a zero-valued Token
//     with kind TOK_INT (== 0). That is technically a valid
//     token kind; without the initial advance, a source file
//     that starts with an integer would be silently treated
//     as if it had already been read.
//
// Main loop:
//   - Skip bare `;` at top level (a file with empty
//     declarations between others is tolerated).
//   - Dispatch on the leading keyword to the appropriate
//     declaration parser.
//   - Every branch pushes a non-NULL result into the
//     program's decls list. Some branches (type, pub,
//     extern) can return NULL on a reported error; those
//     are checked with `if (d)`.
//
// Recovery:
//   - `before` snapshots the diagnostic count so a failed
//     declaration can trigger a skip. The skip advances
//     until the current token is one of the top-level
//     starter keywords or EOF, discarding tokens in
//     between. This is a coarser recovery than the
//     statement-level synchronize, and it is what lets the
//     parser resync after a badly-mangled top-level
//     declaration without re-entering a bad production.
//
// Return: always a valid AST_PROGRAM node, never NULL. On a
// completely failed parse, `decls` may be empty.
AstNode* parse_program(Lexer* lex, const SourceMap* smap,
                       DiagnosticBag* bag) {
    Parser p;
    memset(&p, 0, sizeof p);
    p.lex  = lex;
    p.bag  = bag;
    p.smap = smap;
    advance(&p);

    AstNode* prog = ast_program();

    while (!check(&p, TOK_EOF)) {
        // Empty statement at top level: harmless, skip.
        if (match(&p, TOK_SEMI)) continue;

        int   before = bag->error_count;
        Token kw     = p.cur;

        // Dispatch on the leading keyword. Note that each
        // branch re-checks for its specific token via match;
        // the `kw` capture is only for the sub-parser's span
        // start.
        if (match(&p, TOK_FN)) {
            ast_list_push(&prog->as.program.decls,
                          function_declaration(&p, kw));
        } else if (match(&p, TOK_CONST)) {
            ast_list_push(&prog->as.program.decls,
                          const_declaration(&p, kw));
        } else if (match(&p, TOK_TYPE)) {
            AstNode* d = type_declaration(&p, kw);
            if (d) ast_list_push(&prog->as.program.decls, d);
        } else if (match(&p, TOK_USING)) {
            ast_list_push(&prog->as.program.decls,
                          using_declaration(&p, kw));
        } else if (match(&p, TOK_PUB)) {
            AstNode* d = pub_declaration(&p, kw);
            if (d) ast_list_push(&prog->as.program.decls, d);
        } else if (match(&p, TOK_EXTERN)) {
            AstNode* d = extern_declaration(&p, kw);
            if (d) ast_list_push(&prog->as.program.decls, d);
        } else {
            // Not a top-level starter. Report and skip one
            // token; the resync loop below will discard
            // whatever else is between here and the next
            // starter.
            diag_error(bag, p.cur.span,
                "expected 'fn', 'const', 'type', 'using', 'extern' or 'pub' "
                "at top level, got %s",
                token_kind_name(p.cur.kind));
            advance(&p);
            p.panic = true;
        }

        // Top-level recovery: after any declaration that
        // produced errors, skip tokens until we land on
        // something that starts a new top-level declaration.
        // The set of stop tokens matches the branch set
        // above; note that `extern` is NOT in the stop set,
        // so a mangled extern is discarded along with the
        // surrounding tokens. That is a slight asymmetry
        // with the dispatch above (which does recognize
        // `extern`); noting for a future cleanup.
        if (bag->error_count > before) {
            while (!check(&p, TOK_EOF)
                && !check(&p, TOK_FN)   && !check(&p, TOK_CONST)
                && !check(&p, TOK_TYPE) && !check(&p, TOK_USING)
                && !check(&p, TOK_PUB)  && !check(&p, TOK_EXTERN))
                advance(&p);
        }
    }

    return prog;
}