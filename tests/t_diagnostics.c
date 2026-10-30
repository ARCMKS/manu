// Consolidated: every diagnostic the compiler can emit, with a
// minimal trigger. If any of these fails, the message text or
// the detection site changed.
#include "../include/test.h"

static void d_unknown_type(void) {
    assert_error("fn f(x: Nope): i64 { return 0; }\n"
                 "fn main(): i64 { return 0; }", "unknown type");
}
static void d_unknown_struct(void) {
    assert_error("fn main(): i64 { let x = Nope { a: 1 }; return 0; }",
                 "unknown struct");
}
static void d_unknown_function(void) {
    assert_error("fn main(): i64 { return nope(); }", "unknown function");
}
static void d_undefined_variable(void) {
    assert_error("fn main(): i64 { return xyzzy; }", "undefined variable");
}
static void d_expected_parser(void) {
    assert_error("fn main( { return 0; }", "expected");
}
static void d_literal_does_not_fit(void) {
    assert_error("fn main(): i64 { let x: i8 = 300; return 0; }",
                 "does not fit");
}
static void d_immutable_reassign(void) {
    assert_error("fn main(): i64 { let x = 5; x = 10; return x; }",
                 "immutable binding");
}
static void d_assign_const(void) {
    assert_error("const N = 5;\nfn main(): i64 { N = 10; return N; }",
                 "cannot assign to constant");
}
static void d_return_ref(void) {
    assert_error("fn f(): ref<i64> { let x = 5; return &x; }\n"
                 "fn main(): i64 { return 0; }", "reference");
}
static void d_return_struct(void) {
    assert_error("type P = struct { x: i64 }\n"
                 "fn f(): P { return P { x: 1 }; }\n"
                 "fn main(): i64 { return 0; }", "cannot return a struct");
}
static void d_borrowed_return(void) {
    assert_error("fn f(s: string): string { return s }\n"
                 "fn main(): i64 { return 0 }\n", "borrowed");
}
static void d_ffi_incompatible(void) {
    assert_error("extern lib \"c\" as c\n"
                 "extern fn f(s: string): i32 from c\n"
                 "fn main(): i64 { return 0 }\n", "not FFI-compatible");
}

static const TestCase cases[] = {
    { "unknown_type",         d_unknown_type         },
    { "unknown_struct",       d_unknown_struct       },
    { "unknown_function",     d_unknown_function     },
    { "undefined_variable",   d_undefined_variable   },
    { "expected_parser",      d_expected_parser      },
    { "literal_does_not_fit", d_literal_does_not_fit },
    { "immutable_reassign",   d_immutable_reassign   },
    { "assign_const",         d_assign_const         },
    { "return_ref",           d_return_ref           },
    { "return_struct",        d_return_struct        },
    { "borrowed_return",      d_borrowed_return      },
    { "ffi_incompatible",     d_ffi_incompatible     },
};
const TestGroup g_diagnostics_group = {
    "diagnostics", cases, sizeof(cases) / sizeof(cases[0])
};
