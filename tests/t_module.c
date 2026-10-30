#include "../include/test.h"
#include "../include/module.h"
#include "../include/chunk.h"
#include "../include/vm.h"
#include "../include/codegen.h"
#include "../include/sema.h"
#include <stdio.h>
#include <string.h>

static void write_file(const char* p, const char* c) {
    FILE* f = fopen(p, "wb");
    if (f) { fwrite(c, 1, strlen(c), f); fclose(f); }
}
static void rm(const char* p) { remove(p); }

static void t_simple_and_aliased_import(void) {
    write_file("t_m_lib.manu", "pub fn helper(): i64 { return 42; }\n");
    write_file("t_m_main.manu",
        "using t_m_lib\n"
        "using t_m_lib as L\n"
        "fn main(): i64 { return 0; }\n");
    ModuleLoader ml; modload_init(&ml);
    modload_add_include(&ml, ".");
    DiagnosticBag bag; diag_init(&bag);
    ASSERT_TRUE(modload_load(&ml, "t_m_main.manu", &bag));
    ASSERT_EQ_INT(ml.file_count, 2);
    modload_free(&ml); diag_free(&bag);
    rm("t_m_lib.manu"); rm("t_m_main.manu");
}

static void t_qualified_and_selective_call(void) {
    write_file("t_q_lib.manu",
        "pub fn dbl(x: i64): i64 { return x * 2; }\n"
        "pub fn inc(x: i64): i64 { return x + 1; }\n");
    write_file("t_q_main.manu",
        "using t_q_lib\n"
        "using t_q_lib.{ inc as plus1 }\n"
        "fn main(): i64 { return t_q_lib.dbl(20) + plus1(0); }\n");
    ModuleLoader ml; modload_init(&ml);
    modload_add_include(&ml, ".");
    DiagnosticBag bag; diag_init(&bag);
    ASSERT_TRUE(modload_load(&ml, "t_q_main.manu", &bag));
    AstNode* prog = modload_merge(&ml);
    ConstTable consts; consts_init(&consts);
    TypeDefTable structs; typedef_table_init(&structs);
    FFITable ffi; ffitable_init(&ffi);
    SemaCtx sc; memset(&sc, 0, sizeof sc);
    ASSERT_TRUE(sema_analyze(prog, &ml.smap, &consts, &structs, &bag, &ffi, &sc));
    Chunk chunk; chunk_init(&chunk);
    codegen_program(prog, &ml.smap, &chunk, &bag, &consts, &structs, &ffi, &sc);
    ASSERT_FALSE(diag_has_errors(&bag));
    VMConfig cfg = { .bytecode = chunk.code, .bytecode_len = (size_t)chunk.count,
                     .lines = chunk.lines, .cols = chunk.cols,
                     .pool = &chunk.pool, .funcs = &chunk.funcs, .types = &chunk.types };
    VM vm; vm_init(&vm, &cfg);
    ASSERT_EQ_INT(vm_run(&vm), 0);
    ASSERT_EQ_INT(vm_top(&vm), 41);
    vm_free(&vm); chunk_free(&chunk); ast_free(prog);
    diag_free(&bag); modload_free(&ml);
    rm("t_q_lib.manu"); rm("t_q_main.manu");
}

static void t_visibility_rejected(void) {
    write_file("t_p_lib.manu", "fn secret(): i64 { return 0; }\n");
    write_file("t_p_main.manu",
        "using t_p_lib\n"
        "fn main(): i64 { return t_p_lib.secret(); }\n");
    ModuleLoader ml; modload_init(&ml);
    modload_add_include(&ml, ".");
    DiagnosticBag bag; diag_init(&bag);
    ASSERT_TRUE(modload_load(&ml, "t_p_main.manu", &bag));
    AstNode* prog = modload_merge(&ml);
    ConstTable consts; consts_init(&consts);
    TypeDefTable structs; typedef_table_init(&structs);
    FFITable ffi; ffitable_init(&ffi);
    SemaCtx sc; memset(&sc, 0, sizeof sc);
    (void)sema_analyze(prog, &ml.smap, &consts, &structs, &bag, &ffi, &sc);
    ASSERT_TRUE(bag.error_count > 0);
    int found = 0;
    for (int i = 0; i < bag.count; i++)
        if (strstr(bag.items[i].message, "private")) found = 1;
    ASSERT_TRUE(found);
    ast_free(prog); diag_free(&bag); modload_free(&ml);
    rm("t_p_lib.manu"); rm("t_p_main.manu");
}

static void t_unqualified_cross_file_rejected(void) {
    write_file("t_u_lib.manu", "pub fn helper(): i64 { return 42; }\n");
    write_file("t_u_main.manu",
        "using t_u_lib\n"
        "fn main(): i64 { return helper(); }\n");
    ModuleLoader ml; modload_init(&ml);
    modload_add_include(&ml, ".");
    DiagnosticBag bag; diag_init(&bag);
    ASSERT_TRUE(modload_load(&ml, "t_u_main.manu", &bag));
    AstNode* prog = modload_merge(&ml);
    ConstTable consts; consts_init(&consts);
    TypeDefTable structs; typedef_table_init(&structs);
    FFITable ffi; ffitable_init(&ffi);
    SemaCtx sc; memset(&sc, 0, sizeof sc);
    (void)sema_analyze(prog, &ml.smap, &consts, &structs, &bag, &ffi, &sc);
    ASSERT_TRUE(bag.error_count > 0);
    ast_free(prog); diag_free(&bag); modload_free(&ml);
    rm("t_u_lib.manu"); rm("t_u_main.manu");
}

static void t_cycle_and_missing_rejected(void) {
    write_file("t_c_a.manu", "using t_c_b\npub fn fa(): i64 { return 0; }\n");
    write_file("t_c_b.manu", "using t_c_a\npub fn fb(): i64 { return 0; }\n");
    ModuleLoader ml; modload_init(&ml);
    modload_add_include(&ml, ".");
    DiagnosticBag bag; diag_init(&bag);
    ASSERT_FALSE(modload_load(&ml, "t_c_a.manu", &bag));
    int found = 0;
    for (int i = 0; i < bag.count; i++)
        if (strstr(bag.items[i].message, "circular")) found = 1;
    ASSERT_TRUE(found);
    modload_free(&ml); diag_free(&bag);
    rm("t_c_a.manu"); rm("t_c_b.manu");

    write_file("t_none.manu", "using t_does_not_exist\nfn main(): i64 { return 0; }\n");
    ModuleLoader m2; modload_init(&m2);
    modload_add_include(&m2, ".");
    DiagnosticBag b2; diag_init(&b2);
    ASSERT_FALSE(modload_load(&m2, "t_none.manu", &b2));
    modload_free(&m2); diag_free(&b2);
    rm("t_none.manu");
}

static const TestCase cases[] = {
    { "simple_and_aliased",   t_simple_and_aliased_import },
    { "qualified_and_select", t_qualified_and_selective_call },
    { "visibility_rejected",  t_visibility_rejected },
    { "unqualified_rejected", t_unqualified_cross_file_rejected },
    { "cycle_missing",        t_cycle_and_missing_rejected },
};
const TestGroup g_modules_group = {
    "modules", cases, sizeof(cases) / sizeof(cases[0])
};