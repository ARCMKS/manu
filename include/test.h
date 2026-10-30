// ============================================================
// test.h — Test framework and module contract.
//
// A test is a `static void f(void)` that calls one or more
// ASSERT_* macros. Assertions bump `g_test_failures` and return
// from the enclosing test on failure. The runner compares the
// counter before and after each test.
//
// A module exposes exactly one symbol: a `const TestGroup`.
// `test.c` lists all groups; adding a new module is one extern
// plus one pointer in that list.
//
// Design notes:
//   - Assertions return from the enclosing test rather than
//     aborting the process. That lets a suite keep running
//     after a failure and report all failing tests in one
//     pass, which is much more useful than "fail fast".
//   - There is no setUp/tearDown. Tests are expected to be
//     independent; a test that needs fixtures constructs them
//     itself.
//   - The framework is intentionally tiny. It is not a
//     general-purpose unit test library; it exists to make
//     the compiler's own regression tests readable.
// ============================================================
#ifndef PRISM_TEST_H
#define PRISM_TEST_H

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#include "diagnostics.h"   // DiagnosticBag is used by test_run

typedef void (*TestFn)(void);

typedef struct {
    const char* name;
    TestFn      fn;
} TestCase;

typedef struct {
    const char*     group_name;
    const TestCase* cases;
    int             count;
} TestGroup;

// Global failure counter. Every ASSERT_* that fails increments it.
// The runner snapshots this before and after each test to
// decide pass/fail, so a test that fails and then *succeeds*
// (which cannot happen, because ASSERT_* return on failure)
// would be misreported. The convention is: one failure
// aborts the test, so the delta is 0 or 1.
extern int g_test_failures;

// ---- Assertion macros ---------------------------------------
//
// Each macro prints a message identifying the file, line, and
// failed expression, then increments g_test_failures and returns
// from the enclosing test function.
//
// The `do { ... } while (0)` wrapper is the standard
// multi-statement-macro idiom: it makes the macro usable in a
// single-statement context (e.g. `if (x) ASSERT_TRUE(y); else ...;`)
// without dangling-else surprises.
//
// All macros are expression-safe: they evaluate their operands
// exactly once (storing them in local temporaries where needed)
// and do not use them after the failure branch.

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL %s:%d: expected true: %s\n", \
                __FILE__, __LINE__, #cond); \
        g_test_failures++; return; \
    } \
} while (0)

#define ASSERT_FALSE(cond) do { \
    if ((cond)) { \
        fprintf(stderr, "  FAIL %s:%d: expected false: %s\n", \
                __FILE__, __LINE__, #cond); \
        g_test_failures++; return; \
    } \
} while (0)

// Integer equality. The two operands are widened to long long
// and compared once. This is the workhorse for most tests
// (tokens, bytecode indices, integer results), so it is worth
// noting that it will *not* detect overflow-related bugs in
// the operands themselves; it compares the widened values.
#define ASSERT_EQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", \
                __FILE__, __LINE__, #a, #b, _a, _b); \
        g_test_failures++; return; \
    } \
} while (0)

// String equality. Both operands must be non-NULL; a NULL
// operand will segfault inside strcmp rather than producing a
// clean assertion failure. That is acceptable for a test
// framework, where a NULL string is a test bug rather than a
// condition the test is trying to check.
#define ASSERT_EQ_STR(a, b) do { \
    const char* _a = (a); const char* _b = (b); \
    if (strcmp(_a, _b) != 0) { \
        fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", \
                __FILE__, __LINE__, _a, _b); \
        g_test_failures++; return; \
    } \
} while (0)

// Token-kind equality. A thin wrapper over ASSERT_EQ_INT that
// reads the `kind` field. Provided so tests read as
// `ASSERT_TOK(tok, TOK_LPAREN)` rather than
// `ASSERT_EQ_INT(tok.kind, TOK_LPAREN)`.
#define ASSERT_TOK(tok, expected) ASSERT_EQ_INT((tok).kind, (expected))

// ---- Shared helpers -----------------------------------------
//
// test_run: compile `src`, run it, return the top of stack.
//   If `bag_out` is non-NULL, ownership of the bag is transferred
//   to the caller (diag_free it when done). Spans in the bag refer
//   to a SourceMap the helper owns and frees — read only
//   `bag.items[i].message`.
//
//   This is an important caveat: the SourceMap is freed when
//   test_run returns, so any Span in the bag cannot be
//   rendered with a source excerpt. Tests that want to check
//   a *specific* diagnostic should look at the message text,
//   not at the span. If the test framework ever needs to
//   render spans, this contract would need to change.
//
// test_run_capture: as above, but captures everything written via
//   OP_PRINT into `buf` (NUL-terminated, truncated to `cap`).
//
// Both helpers treat a program that fails sema or codegen the
// same way: they return 0 and (for test_run) may or may not
// have diagnostics in the bag. There is no separate "compile
// failed" return; callers that want to distinguish a
// compile failure from a successful compile that returned 0
// must check the bag themselves.
long long test_run(const char* src, DiagnosticBag* bag_out);
void      test_run_capture(const char* src, char* buf, size_t cap);
// ---- Runner -------------------------------------------------
//
// Returns 0 on all-pass, 1 on any failure. `filter` selects a
// subset by substring of the fully-qualified "group.name". Pass
// NULL to run everything. `quiet` suppresses the per-test banner.
//
// A test "passes" iff g_test_failures did not increment during
// its execution. Because a failed assertion returns from the
// test, a single test can only ever increment the counter by
// 1, even if it has many failing asserts after the first.
int test_run_all(const char* filter, bool quiet);

// ---- Strict helpers ----
//
// test_run and test_run_capture return 0 on compilation failure,
// so a value-expecting test cannot tell "returned 0" from "did
// not compile". These four do not have that problem:
//
//   test_value       assert the program compiled; return the
//                    top of stack. Aborts the test on compile
//                    or runtime error.
//   test_output      assert the program compiled and ran;
//                    capture stdout into `buf` (NUL-terminated,
//                    truncated to `cap`).
//   assert_error     assert compilation failed with at least
//                    one diagnostic whose message contains
//                    `needle`. The test fails if the program
//                    compiled successfully or the needle is
//                    absent from every message.
//   assert_any_error assert compilation failed with at least
//                    one diagnostic, no message check.
long long test_value(const char* src);
void      test_output(const char* src, char* buf, size_t cap);
void      assert_error(const char* src, const char* needle);
void      assert_any_error(const char* src);

#endif // PRISM_TEST_H