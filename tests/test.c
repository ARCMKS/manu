// ============================================================
// tests/test.c — Framework implementation, shared helpers, and
// the runner.
//
// The framework has three responsibilities:
//
//   1. Provide the two helpers every test uses:
//      test_run         compile and run a source string,
//                       returning the top of stack
//      test_run_capture as above, but captures OP_PRINT output
//
//   2. Own the shared VM output sink. Tests that do not
//      explicitly capture output have their VM writes
//      discarded, so the test runner's stderr remains the
//      only channel the reader sees.
//
//   3. Enumerate the test groups and run them.
//
// A test module exposes exactly one symbol: a `const TestGroup`.
// Adding a module means one `extern` and one pointer in
// `g_all_groups`. Adding a test means one entry in the module's
// own case table.
// ============================================================
#include "../include/test.h"
#include "../include/source_map.h"
#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/sema.h"
#include "../include/codegen.h"
#include "../include/chunk.h"
#include "../include/constant.h"
#include "../include/vm.h"
#include "../include/prism_ffi.h"
#include "../include/prelude.h"

int g_test_failures = 0;
static int   g_passed = 0;
static int   g_failed = 0;
static FILE* g_sink   = NULL;


// ------------------------------------------------------------
// Strict helpers.
//
// Each of these runs the compile pipeline and inspects the
// bag, so a test written with them cannot pass for the wrong
// reason (compilation failed but the value happened to be 0).
// ------------------------------------------------------------

// Compile + run, shared by test_output and test_run_capture.
// On success writes captured output to `buf` (if non-NULL) and
// returns true. On compile or runtime error, flushes the bag
// (or transfers it if bag_out is non-NULL), writes empty to
// `buf`, and returns false.
static bool test_compile_capture(const char* src,
                                  DiagnosticBag* bag_out,
                                  char* buf, size_t cap) {
    SourceMap smap; smap_init(&smap);
    uint32_t fid = (uint32_t)smap_add(&smap, "<t>", src);

    Lexer lex; lexer_init(&lex, src, fid);
    DiagnosticBag bag; diag_init(&bag);
    ConstTable consts; consts_init(&consts);
    TypeDefTable structs; typedef_table_init(&structs);
    FFITable ffi; ffitable_init(&ffi);
    SemaCtx sc; memset(&sc, 0, sizeof sc);

    AstNode* prog = parse_program(&lex, &smap, &bag);
    {
        AstNode* prelude = prelude_parse(&smap, &bag);
        ast_list_prepend_all(&prog->as.program.decls,
                             &prelude->as.program.decls);
        ast_free(prelude);
    }

    if (!diag_has_errors(&bag))
        sema_analyze(prog, &smap, &consts, &structs, &bag, &ffi, &sc);

    Chunk chunk; chunk_init(&chunk);

    if (!diag_has_errors(&bag))
        codegen_program(prog, &smap, &chunk, &bag,
                        &consts, &structs, &ffi, &sc);

    if (diag_has_errors(&bag)) {
        if (bag_out) {
            *bag_out = bag;
        } else {
            diag_flush(&bag, &smap);
            diag_free(&bag);
        }
        if (buf && cap > 0) buf[0] = '\0';
        chunk_free(&chunk);
        ast_free(prog);
        smap_free(&smap);
        return false;
    }

    FILE* tmp = tmpfile();
    VMConfig cfg = {
        .bytecode     = chunk.code,
        .bytecode_len = (size_t)chunk.count,
        .lines        = chunk.lines,
        .cols         = chunk.cols,
        .pool         = &chunk.pool,
        .funcs        = &chunk.funcs,
        .types        = &chunk.types,
        .ffi          = &ffi,
    };
    VM vm; vm_init(&vm, &cfg);
    vm.out = tmp ? tmp : g_sink;

    int rc = vm_run(&vm);
    if (rc != 0) {
        fprintf(stderr,
            "  [test] runtime error: %s\n", vm.error);
        if (bag_out) { *bag_out = bag; } else { diag_free(&bag); }
        if (buf && cap > 0) buf[0] = '\0';
        vm_free(&vm);
        chunk_free(&chunk);
        ast_free(prog);
        smap_free(&smap);
        if (tmp) fclose(tmp);
        return false;
    }

    if (tmp) {
        fflush(tmp); rewind(tmp);
        size_t n = fread(buf, 1, cap - 1, tmp);
        buf[n] = '\0';
        fclose(tmp);
    } else if (buf && cap > 0) {
        buf[0] = '\0';
    }

    // Success: transfer or free the bag as requested.
    if (bag_out) *bag_out = bag; else diag_free(&bag);
    vm_free(&vm);
    chunk_free(&chunk);
    ast_free(prog);
    smap_free(&smap);
    return true;
}

long long test_value(const char* src) {
    DiagnosticBag bag; diag_init(&bag);
    long long r = test_run(src, &bag);
    if (bag.error_count > 0) {
        fprintf(stderr, "  test_value: compilation failed\n");
        for (int i = 0; i < bag.count; i++)
            fprintf(stderr, "    diag: %s\n", bag.items[i].message);
        g_test_failures++;
        diag_free(&bag);
        return 0;
    }
    diag_free(&bag);
    return r;
}

void test_output(const char* src, char* buf, size_t cap) {
    DiagnosticBag bag; diag_init(&bag);
    bool ok = test_compile_capture(src, &bag, buf, cap);
    if (!ok) {
        fprintf(stderr, "  test_output: compilation or runtime failure\n");
        for (int i = 0; i < bag.count; i++)
            fprintf(stderr, "    diag: %s\n", bag.items[i].message);
        g_test_failures++;
    }
    diag_free(&bag);
}

void assert_error(const char* src, const char* needle) {
    DiagnosticBag bag; diag_init(&bag);
    (void)test_run(src, &bag);
    bool found = false;
    for (int i = 0; i < bag.count; i++) {
        if (strstr(bag.items[i].message, needle) != NULL) {
            found = true; break;
        }
    }
    if (!found) {
        fprintf(stderr,
            "  assert_error: no diagnostic contains \"%s\"\n", needle);
        fprintf(stderr, "  diagnostics seen (%d):\n", bag.count);
        for (int i = 0; i < bag.count; i++)
            fprintf(stderr, "    %s\n", bag.items[i].message);
        if (bag.count == 0) fprintf(stderr, "    (none)\n");
        g_test_failures++;
    }
    diag_free(&bag);
}

void assert_any_error(const char* src) {
    DiagnosticBag bag; diag_init(&bag);
    (void)test_run(src, &bag);
    if (bag.error_count == 0) {
        fprintf(stderr, "  assert_any_error: program compiled cleanly\n");
        g_test_failures++;
    }
    diag_free(&bag);
}

// ------------------------------------------------------------
// Helpers
//
// Both helpers build a Compilation-like flow inline. They do not
// use main.c's `Compilation` struct because tests are linked
// against the compiler library, not against the driver.
// ------------------------------------------------------------

long long test_run(const char* src, DiagnosticBag* bag_out) {
    //fprintf(stderr, "  [tr] enter\n"); fflush(stderr);

    SourceMap smap; smap_init(&smap);
    uint32_t fid = (uint32_t)smap_add(&smap, "<test>", src);
    //fprintf(stderr, "  [tr] smap_add fid=%u\n", fid); fflush(stderr);

    Lexer lex; lexer_init(&lex, src, fid);
    DiagnosticBag bag; diag_init(&bag);
    ConstTable consts; consts_init(&consts);
    TypeDefTable structs; typedef_table_init(&structs);
    FFITable ffi; ffitable_init(&ffi);
    SemaCtx sc; 
    memset(&sc, 0, sizeof sc);
    //fprintf(stderr, "  [tr] tables init\n"); fflush(stderr);

    AstNode* prog = parse_program(&lex, &smap, &bag);
    //fprintf(stderr, "  [tr] parse done, errors=%d\n", bag.error_count);
    fflush(stderr);

    {
        AstNode* prelude = prelude_parse(&smap, &bag);
        //fprintf(stderr, "  [tr] prelude parsed, decls=%d\n",
        //        prelude->as.program.decls.count); fflush(stderr);
        ast_list_prepend_all(&prog->as.program.decls,
                             &prelude->as.program.decls);
        ast_free(prelude);
        //fprintf(stderr, "  [tr] prelude merged, total=%d\n",
        //        prog->as.program.decls.count); fflush(stderr);
    }

    if (!diag_has_errors(&bag))
        sema_analyze(prog, &smap, &consts, &structs, &bag, &ffi, &sc);
    //fprintf(stderr, "  [tr] sema done, errors=%d\n", bag.error_count);
    fflush(stderr);

    Chunk chunk; chunk_init(&chunk);
    long long result = 0;

    if (!diag_has_errors(&bag)) {
        codegen_program(prog, &smap, &chunk, &bag, &consts, &structs, &ffi, &sc);
        //fprintf(stderr, "  [tr] codegen done, words=%d\n", chunk.count);
        fflush(stderr);
        if (!diag_has_errors(&bag)) {
            VMConfig cfg = {
                .bytecode     = chunk.code,
                .bytecode_len = (size_t)chunk.count,
                .lines        = chunk.lines,
                .cols         = chunk.cols,
                .pool         = &chunk.pool,
                .funcs        = &chunk.funcs,
                .types        = &chunk.types,
                .ffi          = &ffi,
            };
            VM vm; vm_init(&vm, &cfg);
            if (g_sink) vm.out = g_sink;
            int rc = vm_run(&vm);
            if (rc != 0) {
                fprintf(stderr, "  [test] runtime error: %s\n", vm.error);
                // Leave result at 0. Do NOT read vm_top: the stack is
                // in an undefined state after a runtime error, and any
                // value read there is whatever the failing instruction
                // left on the stack (usually the last argument).
                result = 0;
            } else {
                result = (long long)vm_top(&vm);
            }
            vm_free(&vm);
        }
    }

    chunk_free(&chunk);
    //fprintf(stderr, "  [tr] chunk_free\n"); fflush(stderr);
    ast_free(prog);
    //fprintf(stderr, "  [tr] ast_free\n"); fflush(stderr);

    if (bag_out) {
        *bag_out = bag;
    } else {
        /* No bag requested — surface any diagnostics so failures are
           visible. Tests that intentionally check errors pass a bag. */
        if (bag.count > 0)
            diag_flush(&bag, &smap);
        diag_free(&bag);
    }
    smap_free(&smap);
    //fprintf(stderr, "  [tr] exit\n"); fflush(stderr);
    return result;
}

void test_run_capture(const char* src, char* buf, size_t cap) {
    SourceMap smap; smap_init(&smap);
    uint32_t fid = (uint32_t)smap_add(&smap, "<t>", src);

    Lexer lex; lexer_init(&lex, src, fid);
    DiagnosticBag bag; diag_init(&bag);
    ConstTable consts; consts_init(&consts);
    TypeDefTable structs; typedef_table_init(&structs);
    FFITable ffi; ffitable_init(&ffi);
    SemaCtx sc; memset(&sc, 0, sizeof sc);

    AstNode* prog = parse_program(&lex, &smap, &bag);
    {
        AstNode* prelude = prelude_parse(&smap, &bag);
        ast_list_prepend_all(&prog->as.program.decls,
                             &prelude->as.program.decls);
        ast_free(prelude);
    }

    if (!diag_has_errors(&bag))
        sema_analyze(prog, &smap, &consts, &structs, &bag, &ffi, &sc);

    Chunk chunk; chunk_init(&chunk);
    
    if (!diag_has_errors(&bag)) {
        codegen_program(prog, &smap, &chunk, &bag, &consts, &structs, &ffi, &sc);
    }
    // If compilation failed, leave an empty buffer and report
    // the errors so the test failure is legible.
    if (diag_has_errors(&bag)) {
        diag_flush(&bag, &smap);
        buf[0] = '\0';
        chunk_free(&chunk);
        ast_free(prog);
        diag_free(&bag);
        smap_free(&smap);
        return;
    }

    FILE* tmp = tmpfile();

    // Every field here must match the shape test_run uses.
    // Missing .funcs or .types means the VM traps on the first
    // instruction and every capture test fails with "".
    VMConfig cfg = {
        .bytecode     = chunk.code,
        .bytecode_len = (size_t)chunk.count,
        .lines        = chunk.lines,
        .cols         = chunk.cols,
        .pool         = &chunk.pool,
        .funcs        = &chunk.funcs,
        .types        = &chunk.types,
        .ffi          = &ffi,
    };

    VM vm; vm_init(&vm, &cfg);
    vm.out = tmp ? tmp : g_sink;

    int rc = vm_run(&vm);
    if (rc != 0) {
        fprintf(stderr,
            "  [test_run_capture] runtime error: %s\n", vm.error);
        buf[0] = '\0';
        vm_free(&vm);
        chunk_free(&chunk);
        ast_free(prog);
        diag_free(&bag);
        smap_free(&smap);
        if (tmp) fclose(tmp);
        return;
    }

    if (tmp) {
        fflush(tmp); rewind(tmp);
        size_t n = fread(buf, 1, cap - 1, tmp);
        buf[n] = '\0';
        fclose(tmp);
    } else {
        buf[0] = '\0';
    }

    vm_free(&vm);
    chunk_free(&chunk);
    ast_free(prog);
    diag_free(&bag);
    smap_free(&smap);
}

// ------------------------------------------------------------
// Registry
//
// One extern + one pointer per test module. Adding a module is
// two edits here and nothing else.
// ------------------------------------------------------------
extern const TestGroup g_frontend_group;
extern const TestGroup g_types_group;
extern const TestGroup g_memory_group;
extern const TestGroup g_aggregate_group;
extern const TestGroup g_control_group;
extern const TestGroup g_functions_group;
extern const TestGroup g_modules_group;
extern const TestGroup g_ffi_group;
extern const TestGroup g_diagnostics_group;
extern const TestGroup g_integration_group;

static const TestGroup* const g_all_groups[] = {
    &g_frontend_group,
    &g_types_group,
    &g_memory_group,
    &g_aggregate_group,
    &g_control_group,
    &g_functions_group,
    &g_modules_group,
    &g_ffi_group,
    &g_diagnostics_group,
    &g_integration_group,
};
static const int g_group_count =
    (int)(sizeof(g_all_groups) / sizeof(g_all_groups[0]));

// ------------------------------------------------------------
// Runner
// ------------------------------------------------------------
int test_run_all(const char* filter, bool quiet) {
    if (!g_sink) g_sink = tmpfile();

    for (int gi = 0; gi < g_group_count; gi++) {
        const TestGroup* group = g_all_groups[gi];
        for (int i = 0; i < group->count; i++) {
            const TestCase* tc = &group->cases[i];

            if (filter) {
                char full[160];
                snprintf(full, sizeof full, "%s.%s",
                         group->group_name, tc->name);
                if (!strstr(full, filter)) continue;
            }

            int before = g_test_failures;
            if (!quiet)
                fprintf(stderr, "[ RUN  ] %s.%s\n",
                        group->group_name, tc->name);
            fflush(stderr);
            tc->fn();

            if (g_test_failures == before) {
                g_passed++;
                if (!quiet)
                    fprintf(stderr, "[  OK  ] %s.%s\n",
                            group->group_name, tc->name);
            } else {
                g_failed++;
                if (!quiet)
                    fprintf(stderr, "[ FAIL ] %s.%s\n",
                            group->group_name, tc->name);
            }
        }
    }

    fprintf(stderr, "\n=== %d passed, %d failed ===\n",
            g_passed, g_failed);
    if (g_sink) { fclose(g_sink); g_sink = NULL; }
    return g_failed == 0 ? 0 : 1;
}
