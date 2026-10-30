#include "../include/test.h"

// ---- Return-type inference ----
static void t_return_inference(void) {
    ASSERT_EQ_INT(test_value(
        "fn get() { return 42; }\n"
        "fn main(): i64 { return get(); }"), 42);
    ASSERT_EQ_INT(test_value(
        "fn noop() { }\n"
        "fn main(): i64 { noop(); return 7; }"), 7);
    ASSERT_EQ_INT(test_value(
        "fn fact(n: i64) {\n"
        "  if (n <= 1) { return 1; }\n"
        "  return n * fact(n - 1);\n"
        "}\n"
        "fn main(): i64 { return fact(5); }"), 120);
}

static void t_return_inference_rejected(void) {
    assert_error(
        "fn main() { return get(); }\n"
        "fn get() { return 42; }",
        "cannot infer return type");
    assert_error(
        "fn f() { if (true) { return 5; } return true; }\n"
        "fn main(): i64 { return 0; }",
        "cannot implicitly convert");
}

// ---- Annotations and literal adaptation ----
static void t_annotation_and_adaptation(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { let x: i8 = 100; return 0; }"), 0);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { let x: u32 = 4000000000; return 0; }"), 0);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { let x: i8 = 10; return x + 5; }"), 15);
    ASSERT_EQ_INT(test_value(
        "fn f(x: u8): i64 { return 0; }\n"
        "fn main(): i64 { return f(200); }"), 0);
}

static void t_annotation_mismatch_rejected(void) {
    assert_error(
        "fn main(): i64 { let a: i64 = true; return 0; }",
        "cannot implicitly convert");
    assert_error(
        "fn main(): i64 { let x: i8 = 300; return 0; }",
        "does not fit");
    assert_error(
        "fn main(): i64 { let x: u8 = -1; return 0; }",
        "");
    assert_error(
        "fn f(): bool { return 1; }\n"
        "fn main(): i64 { return 0; }",
        "cannot implicitly convert");
}

// ---- Arithmetic, wrapping, and mixed types ----
static void t_integer_wrapping(void) {
    char buf[32];
    test_output(
        "fn main(): i64 {\n"
        "  let a: i8 = 120;\n"
        "  let b: i8 = 20;\n"
        "  print(\"{}\", a + b);\n"
        "  let c: u8 = 250;\n"
        "  let d: u8 = 10;\n"
        "  print(\"{}\", c + d);\n"
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "-116\n4\n");
}

static void t_mixed_types_rejected(void) {
    assert_error(
        "fn main(): i64 {\n"
        "  let a: i8 = 10;\n"
        "  let b: i32 = 20;\n"
        "  return a + b;\n"
        "}", "different types");
    assert_error(
        "fn main(): i64 { let x = 1 + 2.0; return 0; }", "");
}

static void t_unsigned_negation_rejected(void) {
    assert_error(
        "fn main(): i64 { let x: u8 = 5; let y = -x; return 0; }",
        "unsigned");
}

// ---- Casts ----
static void t_integer_casts(void) {
    char buf[64];
    test_output(
        "fn main(): i64 {\n"
        "  let x: i64 = 300;\n"
        "  print(\"{}\", x as i8);\n"        // 44
        "  let y: i8 = -1;\n"
        "  print(\"{}\", y as u8);\n"        // 255
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "44\n255\n");
}

static void t_float_casts(void) {
    char buf[64];
    test_output(
        "fn main(): i64 {\n"
        "  let n: i64 = 42;\n"
        "  print(\"{}\", n as f64);\n"        // 42
        "  let f = 3.9;\n"
        "  print(\"{}\", f as i64);\n"        // 3
        "  let g = -3.9;\n"
        "  print(\"{}\", g as i64);\n"        // -3
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "42\n3\n-3\n");
}

static void t_bool_casts(void) {
    char buf[32];
    test_output(
        "fn main(): i64 {\n"
        "  let b = true;\n"
        "  print(\"{}\", b as i64);\n"        // 1
        "  let n: i32 = 5;\n"
        "  print(\"{}\", n as bool);\n"       // true
        "  let z: u8 = 0;\n"
        "  print(\"{}\", z as bool);\n"       // false
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "1\ntrue\nfalse\n");
}

static void t_cast_rejections(void) {
    assert_error(
        "fn main(): i64 { let s = \"42\"; let n = s as i64; return 0; }",
        "cannot cast");
    assert_error(
        "fn main(): i64 { let n: i64 = 42; let s = n as string; return 0; }",
        "");
    assert_error(
        "fn main(): i64 { let n: i64 = 42; let a = n as [i64]; return 0; }",
        "");
}

// ---- Constants ----
static void t_const_use(void) {
    ASSERT_EQ_INT(test_value(
        "const MAX = 100;\n"
        "fn main(): i64 { return MAX; }"), 100);
    ASSERT_EQ_INT(test_value(
        "const N = 5;\n"
        "fn main(): i64 { return N + N * N; }"), 30);
    ASSERT_EQ_INT(test_value(
        "const N = 5;\n"
        "fn main(): i64 { let N = 100; return N; }"), 100);
}

static void t_const_rejections(void) {
    assert_error(
        "const X = 1 + 2;\nfn main(): i64 { return X; }", "literal");
    assert_error(
        "const S = \"hi\";\nfn main(): i64 { return 0; }", "string constant");
    assert_error(
        "const X = 1;\nconst X = 2;\nfn main(): i64 { return X; }",
        "duplicate constant");
    assert_error(
        "const N = 5;\nfn main(): i64 { N = 10; return N; }",
        "cannot assign to constant");
}

// ---- Condition type check ----
static void t_condition_must_be_bool(void) {
    assert_error(
        "fn main(): i64 { if (1) { return 1; } return 0; }",
        "condition must be bool");
}

static const TestCase cases[] = {
    { "return_inference",           t_return_inference            },
    { "return_inference_rejected",  t_return_inference_rejected   },
    { "annotation_and_adaptation",  t_annotation_and_adaptation   },
    { "annotation_mismatch",        t_annotation_mismatch_rejected},
    { "integer_wrapping",           t_integer_wrapping            },
    { "mixed_types_rejected",       t_mixed_types_rejected        },
    { "unsigned_negation_rejected", t_unsigned_negation_rejected  },
    { "integer_casts",              t_integer_casts               },
    { "float_casts",                t_float_casts                 },
    { "bool_casts",                 t_bool_casts                  },
    { "cast_rejections",            t_cast_rejections             },
    { "const_use",                  t_const_use                   },
    { "const_rejections",           t_const_rejections            },
    { "condition_must_be_bool",     t_condition_must_be_bool      },
};
const TestGroup g_types_group = {
    "types", cases, sizeof(cases) / sizeof(cases[0])
};