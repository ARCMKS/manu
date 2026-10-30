#include "../include/test.h"

static void t_calls_and_recursion(void) {
    ASSERT_EQ_INT(test_value(
        "fn add(a: i64, b: i64): i64 { return a + b; }\n"
        "fn main(): i64 { return add(3, 4); }"), 7);
    ASSERT_EQ_INT(test_value(
        "fn fact(n: i64): i64 { if (n <= 1) { return 1; } return n * fact(n - 1); }\n"
        "fn main(): i64 { return fact(5); }"), 120);
}

static void t_forward_calls(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { return helper(4); }\n"
        "fn helper(x: i64): i64 { return x * 2; }"), 8);
    assert_error(
        "fn add(a: i64, b: i64): i64 { return a + b; }\n"
        "fn main(): i64 { return add(1); }", "expects");
    assert_error(
        "fn add(a: i64, b: i64): i64 { return a + b; }\n"
        "fn main(): i64 { return add(1, true); }",
        "cannot implicitly convert");
}

static void t_methods(void) {
    ASSERT_EQ_INT(test_value(
        "type P = struct { x: i64 }\n"
        "fn P.get(p: P): i64 { return p.x; }\n"
        "fn P.add(p: P, n: i64): i64 { return p.x + n; }\n"
        "fn main(): i64 { let p = P { x: 10 }; return p.get() + p.add(32); }"), 52);
    ASSERT_EQ_INT(test_value(
        "type A = struct { a: i64 }\n"
        "type B = struct { b: i64 }\n"
        "fn A.val(x: A): i64 { return x.a; }\n"
        "fn B.val(x: B): i64 { return x.b + 100; }\n"
        "fn main(): i64 {\n"
        "  let a = A { a: 1 };\n"
        "  let b = B { b: 2 };\n"
        "  return a.val() + b.val();\n"
        "}"), 103);
    assert_error(
        "type P = struct { x: i64 }\n"
        "fn main(): i64 { let p = P { x: 1 }; return p.nope(); }",
        "no method");
}

static void t_generic_functions(void) {
    ASSERT_EQ_INT(test_value(
        "fn identity<T>(x: T): T { return x }\n"
        "fn main(): i64 { return identity<i64>(42); }"), 42);
    char buf[32];
    test_output(
        "fn identity<T>(x: T): T { return x }\n"
        "fn main(): i64 {\n"
        "  print(\"{}\", identity<i64>(1));\n"
        "  print(\"{}\", identity<bool>(false));\n"
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "1\nfalse\n");
}

static void t_generic_structs(void) {
    ASSERT_EQ_INT(test_value(
        "type Box<T> = struct { value: T }\n"
        "fn main(): i64 {\n"
        "  let b: Box<i64> = Box<i64> { value: 99 };\n"
        "  return b.value;\n"
        "}"), 99);
    char buf[32];
    test_output(
        "type Box<T> = struct { value: T }\n"
        "fn main(): i64 {\n"
        "  let inner: Option<i64> = Option<i64>.Some(7);\n"
        "  let outer: Box<Option<i64>> = Box<Option<i64>> { value: inner };\n"
        "  match outer.value {\n"
        "    Option<i64>.Some(v) => { print(\"{}\", v); }\n"
        "    Option<i64>.None    => { print(\"none\"); }\n"
        "  }\n"
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "7\n");
}

static void t_generic_rejections(void) {
    assert_error(
        "fn identity<T>(x: T): T { return x }\n"
        "fn main(): i64 { return identity<i64, bool>(5); }", "");
    assert_error(
        "fn main(): i64 { return identity<i64>(5); }", "");
}

static const TestCase cases[] = {
    { "calls_and_recursion",  t_calls_and_recursion },
    { "forward_calls",        t_forward_calls       },
    { "methods",              t_methods             },
    { "generic_functions",    t_generic_functions   },
    { "generic_structs",      t_generic_structs     },
    { "generic_rejections",   t_generic_rejections  },
};
const TestGroup g_functions_group = {
    "functions", cases, sizeof(cases) / sizeof(cases[0])
};
