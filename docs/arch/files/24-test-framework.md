## `docs/arch/files/24-test-framework.md`

# `include/test.h`, `tests/test.c`, `tests/test_main.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

A minimal table-driven test framework. A test is a
`static void f(void)` calling one or more `ASSERT_*` macros. A
test module exports one `const TestGroup`. `test.c` lists all
groups; `test_main.c` is the entry point.

**Philosophy.** Assertions fail the enclosing test (return from
the function) rather than aborting the process. Tests keep
running after a failure; the runner reports all failing tests in
one pass.

**No setup/teardown.** Tests are independent. A test that needs
fixtures builds them itself.

---

## §2 — Public interface

```c
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

extern int g_test_failures;

#define ASSERT_TRUE(cond)       ...
#define ASSERT_FALSE(cond)      ...
#define ASSERT_EQ_INT(a, b)     ...
#define ASSERT_EQ_STR(a, b)     ...
#define ASSERT_TOK(tok, expected) ...

long long test_run(const char* src, DiagnosticBag* bag_out);
void      test_run_capture(const char* src, char* buf, size_t cap);
long long test_value(const char* src);
void      test_output(const char* src, char* buf, size_t cap);
void      assert_error(const char* src, const char* needle);
void      assert_any_error(const char* src);

int  test_run_all(const char* filter, bool quiet);
```

**`TestCase`/`TestGroup`.** The registry shape. Each test module
declares one `const TestGroup` and `test.c` lists them.

**`g_test_failures`.** Global counter. Every failed assertion
increments it and returns from the enclosing test. The runner
snapshots before and after each test; a delta of 1 marks the
test failed.

**Assertions.**

- `ASSERT_TRUE(cond)` / `ASSERT_FALSE(cond)`.
- `ASSERT_EQ_INT(a, b)` — widens to `long long`.
- `ASSERT_EQ_STR(a, b)` — `strcmp` (NULL is a test bug, not a
  condition).
- `ASSERT_TOK(tok, expected)` — `ASSERT_EQ_INT((tok).kind,
  expected)`.

**`test_run`.** Compiles and runs `src`, returns the top of
stack. On a runtime error, prints the message and returns 0 (the
top-of-stack value is undefined after a failure). The bag, if
requested, is transferred to the caller.

**`test_run_capture`.** Same, but redirects the VM's stdout to a
`tmpfile` and reads the buffer into `buf`.

**`test_value`.** Asserts the program compiled; returns the top
of stack. On compile failure, prints the diagnostics and
increments `g_test_failures`.

**`test_output`.** Asserts the program compiled and ran;
captures stdout.

**`assert_error`.** Asserts at least one diagnostic contains
`needle`.

**`assert_any_error`.** Asserts at least one diagnostic.

**`test_run_all`.** Runs every test whose `group.case` string
contains `filter` (or all if `filter == NULL`). Prints
`[ RUN ]`/`[ OK ]`/`[ FAIL ]` lines unless `quiet`. Returns 0 on
all-pass.

---

## §3 — Implementation notes

### §3.1 The `test.c` registry

Each test module has a `const TestGroup g_<name>_group` symbol.
`test.c` declares them all `extern` and lists them in
`g_all_groups[]`. Adding a module is two edits: the `extern`
declaration and a pointer in the array.

### §3.2 `test_compile_capture`

The core helper used by `test_value`, `test_output`, and
`test_run_capture`. It runs the full pipeline:

1. `smap_init`, `smap_add`.
2. `lexer_init`, `parse_program`.
3. `prelude_parse`, prepend.
4. `sema_analyze` (if no errors).
5. `codegen_program` (if no errors).
6. If errors: transfer or free the bag, free the chunk and
   program, return false.
7. Otherwise: `tmpfile`, `VMConfig`, `vm_init`, `vm_run`.
8. On runtime error: print the message, return false.
9. On success: read the tmpfile into `buf`, transfer or free
   the bag, return true.

Every stage allocates its own tables. The helper does not reuse
state across calls; each test gets a fresh pipeline.

### §3.3 The output sink

Non-captured VM output is redirected to a shared `tmpfile`
(`g_sink`) so the runner's stderr stays clean. `test_run_all`
opens `g_sink` once and closes it at exit.

`test_value` and `test_run` set `vm.out = g_sink`. `test_output`
and `test_run_capture` set `vm.out` to their own `tmpfile`.

### §3.4 Assertion macro shape

```c
#define ASSERT_EQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", \
                __FILE__, __LINE__, #a, #b, _a, _b); \
        g_test_failures++; return; \
    } \
} while (0)
```

The `do { ... } while (0)` wrapper is the standard
multi-statement-macro idiom. Each assertion evaluates its
operands once, stores them in locals, and compares. On failure,
increments the counter and returns from the enclosing test.

### §3.5 `test_run`'s runtime-error handling

On a `vm_run` failure, `test_run` prints the runtime message and
returns 0 rather than reading `vm_top`. The stack is in an
undefined state after a runtime error; any value read would be
whatever the failing instruction left, typically the last
argument.

This was a source of a historical test bug: `test_run` used to
read `vm_top` unconditionally, and a program that failed at an
FFI call would return the FFI call's last argument (42, in the
observed case) rather than 0.

---

## §4 — Invariants

- `g_test_failures` is monotonically non-decreasing.
- Every test's failure produces at least one line on stderr.
- `test_run_all` returns 0 iff `g_test_failures` was unchanged
  across the run.
- `test_value`, `test_output`, `assert_error`, and
  `assert_any_error` increment `g_test_failures` on failure and
  leave it unchanged on success.

---

## §5 — Known limitations

**No `ASSERT_EQ_F64`.** Float tests compare printed output.

**No per-test setup/teardown.** Tests are independent.

**No timeout.** A test that hangs hangs the suite.

**Assertions abort the enclosing test only.** A test that wants
to report multiple failures must reset `g_test_failures` after
each.

**`ASSERT_EQ_STR` on a NULL pointer segfaults.** The macros do
not guard against NULL.

**No ASAN-friendly cleanup.** The framework assumes short-lived
test processes. The `TypeDefTable` is deliberately leaked per
`test_run` call (comment in `test.c`).

**The `test_compile_capture` helper is not in the header.** It's
static to `test.c`; only the four public helpers are exposed.

**No machine-readable output.** The runner's stdout is plain
text.

**`test_run` returns 0 both for "program returned 0" and "program
failed to compile" unless the caller passes a bag.** The
`test_value` helper exists to close this gap.

---

## §6 — Extension points

- **`ASSERT_EQ_F64`.** Would need an epsilon parameter or exact
  comparison.
- **Setup/teardown hooks.** `TestGroup` could gain optional
  function pointers.
- **A timeout.** `test_run_all` could `fork` and wait with a
  timeout.
- **A `--json` output mode.** Would make CI integration easier.
- **Richer filtering.** Regex filters instead of substring
  matching.
- **Per-test isolation.** Running each test in a forked child
  would isolate crashes. Currently a crash kills the whole
  suite.
