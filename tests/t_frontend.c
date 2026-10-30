#include "../include/test.h"
#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/ast.h"
#include "../include/source_map.h"

static void t_lexer_token_stream(void) {
    Lexer l; lexer_init(&l, "fn main() { let x = 5; }", 1);
    ASSERT_TOK(lexer_next(&l), TOK_FN);
    ASSERT_TOK(lexer_next(&l), TOK_IDENT);
    ASSERT_TOK(lexer_next(&l), TOK_LPAREN);
    ASSERT_TOK(lexer_next(&l), TOK_RPAREN);
    ASSERT_TOK(lexer_next(&l), TOK_LBRACE);
    ASSERT_TOK(lexer_next(&l), TOK_LET);
    ASSERT_TOK(lexer_next(&l), TOK_IDENT);
    ASSERT_TOK(lexer_next(&l), TOK_ASSIGN);
    ASSERT_TOK(lexer_next(&l), TOK_INT);
    ASSERT_TOK(lexer_next(&l), TOK_SEMI);
    ASSERT_TOK(lexer_next(&l), TOK_RBRACE);
}

static void t_lexer_operators(void) {
    Lexer l; lexer_init(&l, "== != <= >= < > + - * / & ! ? rc mut", 1);
    ASSERT_TOK(lexer_next(&l), TOK_EQEQ);
    ASSERT_TOK(lexer_next(&l), TOK_BANGEQ);
    ASSERT_TOK(lexer_next(&l), TOK_LE);
    ASSERT_TOK(lexer_next(&l), TOK_GE);
    ASSERT_TOK(lexer_next(&l), TOK_LT);
    ASSERT_TOK(lexer_next(&l), TOK_GT);
    ASSERT_TOK(lexer_next(&l), TOK_PLUS);
    ASSERT_TOK(lexer_next(&l), TOK_MINUS);
    ASSERT_TOK(lexer_next(&l), TOK_STAR);
    ASSERT_TOK(lexer_next(&l), TOK_SLASH);
    ASSERT_TOK(lexer_next(&l), TOK_AMP);
    ASSERT_TOK(lexer_next(&l), TOK_BANG);
    ASSERT_TOK(lexer_next(&l), TOK_QUESTION);
    ASSERT_TOK(lexer_next(&l), TOK_RC);
    ASSERT_TOK(lexer_next(&l), TOK_MUT);
}

static void t_lexer_asi(void) {
    Lexer l; lexer_init(&l, "5\n6", 1);
    ASSERT_TOK(lexer_next(&l), TOK_INT);
    ASSERT_TOK(lexer_next(&l), TOK_SEMI);
    ASSERT_TOK(lexer_next(&l), TOK_INT);
    ASSERT_TOK(lexer_next(&l), TOK_SEMI);

    Lexer m; lexer_init(&m, "5 +\n6", 1);
    ASSERT_TOK(lexer_next(&m), TOK_INT);
    ASSERT_TOK(lexer_next(&m), TOK_PLUS);
    ASSERT_TOK(lexer_next(&m), TOK_INT);
    ASSERT_TOK(lexer_next(&m), TOK_SEMI);

    Lexer n; lexer_init(&n, "5\n\n\n6", 1);
    ASSERT_TOK(lexer_next(&n), TOK_INT);
    ASSERT_TOK(lexer_next(&n), TOK_SEMI);
    ASSERT_TOK(lexer_next(&n), TOK_INT);
}

static void t_parser_precedence(void) {
    const char* src = "fn main() { let x = 1 + 2 * 3; }";
    SourceMap smap; smap_init(&smap);
    uint32_t fid = (uint32_t)smap_add(&smap, "<p>", src);
    Lexer lex; lexer_init(&lex, src, fid);
    DiagnosticBag bag; diag_init(&bag);
    AstNode* p = parse_program(&lex, &smap, &bag);
    ASSERT_FALSE(diag_has_errors(&bag));
    AstNode* init = p->as.program.decls.items[0]
                       ->as.func.body->as.block.stmts.items[0]->as.let.init;
    ASSERT_EQ_INT(init->kind, AST_BINARY);
    ASSERT_EQ_INT(init->as.binary.op, BIN_ADD);
    ASSERT_EQ_INT(init->as.binary.right->as.binary.op, BIN_MUL);
    ast_free(p); diag_free(&bag); smap_free(&smap);
}

static void t_source_map_decode(void) {
    SourceMap sm; smap_init(&sm);
    uint32_t f = (uint32_t)smap_add(&sm, "x.manu", "abc\ndef\nghi\n");
    SpanDecoded d;
    Span s1 = { 4, 7, f };
    ASSERT_TRUE(smap_decode(&sm, s1, &d));
    ASSERT_EQ_INT(d.line, 2); ASSERT_EQ_INT(d.column, 1);
    Span s2 = { 8, 9, f };
    ASSERT_TRUE(smap_decode(&sm, s2, &d));
    ASSERT_EQ_INT(d.line, 3); ASSERT_EQ_INT(d.column, 1);
    ASSERT_FALSE(smap_decode(&sm, SPAN_NONE, &d));
    smap_free(&sm);
}

static const TestCase cases[] = {
    { "lexer_token_stream",  t_lexer_token_stream  },
    { "lexer_operators",     t_lexer_operators     },
    { "lexer_asi",           t_lexer_asi           },
    { "parser_precedence",   t_parser_precedence   },
    { "source_map_decode",   t_source_map_decode   },
};
const TestGroup g_frontend_group = {
    "frontend", cases, sizeof(cases) / sizeof(cases[0])
};
