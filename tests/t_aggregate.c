#include "../include/test.h"

// ---- Structs ----
static void t_struct_basics(void) {
    char buf[64];
    test_output(
        "type P = struct { x: i64, y: i64 }\n"
        "fn main(): i64 {\n"
        "  let mut p = P { y: 4, x: 3 };\n"
        "  p.x = 100;\n"
        "  print(\"{}\", p.x + p.y);\n"    // 104
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "104\n");
}

static void t_struct_nested_and_argument(void) {
    char buf[64];
    test_output(
        "type Inner = struct { v: i64 }\n"
        "type Outer = struct { inner: Inner, tag: i64 }\n"
        "fn sum(o: Outer): i64 { return o.inner.v + o.tag; }\n"
        "fn main(): i64 {\n"
        "  let o = Outer { inner: Inner { v: 42 }, tag: 8 };\n"
        "  print(\"{}\", sum(o));\n"       // 50
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "50\n");
}

static void t_struct_print(void) {
    char buf[32];
    test_output(
        "type P = struct { x: i64, y: i64 }\n"
        "fn main(): i64 { let p = P { x: 1, y: 2 }; print(p); return 0; }",
        buf, sizeof buf);
    ASSERT_EQ_STR(buf, "{1, 2}\n");
}

static void t_struct_return_rules(void) {
    assert_error(
        "type P = struct { x: i64 }\n"
        "fn make(): P { return P { x: 1 }; }\n"
        "fn main(): i64 { return 0; }", "cannot return a struct");
    ASSERT_EQ_INT(test_value(
        "type P = struct { x: i64 }\n"
        "fn make(): rc<P> { return rc P { x: 7 }; }\n"
        "fn main(): i64 { let p = make(); return p.x; }"), 7);
    assert_error(
        "type P = struct { x: i64 }\n"
        "fn main(): i64 { let p = P { x: 1 }; return p.y; }",
        "no field");
}

// ---- Enums ----
static void t_enum_variants_and_match(void) {
    char buf[64];
    test_output(
        "type Color = enum { Red, Green, Blue }\n"
        "fn main(): i64 {\n"
        "  let c = Color.Green;\n"
        "  match c {\n"
        "    Color.Red => { print(\"red\"); }\n"
        "    Color.Green => { print(\"green\"); }\n"
        "    Color.Blue => { print(\"blue\"); }\n"
        "  }\n"
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "green\n");
}

static void t_enum_payloads(void) {
    char buf[64];
    test_output(
        "type Shape = enum { Circle(i64), Rect(i64, i64) }\n"
        "fn main(): i64 {\n"
        "  let s = Shape.Rect(3, 4);\n"
        "  match s {\n"
        "    Shape.Circle(r) => { print(\"c{}\", r); }\n"
        "    Shape.Rect(w, h) => { print(\"{}x{}\", w, h); }\n"
        "  }\n"
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "3x4\n");
}

static void t_enum_nested_pattern(void) {
    char buf[32];
    test_output(
        "type Inner = enum { Ok(i64), Err }\n"
        "type Outer = enum { Wrap(Inner), Nothing }\n"
        "fn main(): i64 {\n"
        "  let o = Outer.Wrap(Inner.Ok(42));\n"
        "  match o {\n"
        "    Outer.Wrap(Inner.Ok(v)) => { print(\"got {}\", v); }\n"
        "    _ => { print(\"other\"); }\n"
        "  }\n"
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "got 42\n");
}

static void t_enum_exhaustiveness(void) {
    assert_error(
        "type Color = enum { Red, Green, Blue }\n"
        "fn main(): i64 {\n"
        "  let c = Color.Red;\n"
        "  match c {\n"
        "    Color.Red => { print(\"r\"); }\n"
        "  }\n"
        "  return 0;\n"
        "}", "not exhaustive");
    assert_error(
        "fn main(): i64 { match true { true => { print(\"y\"); } } return 0; }",
        "");
}

static void t_enum_rejections(void) {
    assert_error(
        "type C = enum { A, B, A }\n"
        "fn main(): i64 { return 0; }", "duplicate variant");
    assert_error(
        "type C = struct { x: i64 }\n"
        "type C = enum { A }\n"
        "fn main(): i64 { return 0; }", "duplicate type");
    assert_error(
        "type Color = enum { Red, Green }\n"
        "fn main(): i64 { let c = Color.Blue; return 0; }", "no variant");
}

// ---- Arrays ----
static void t_array_basics(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { let a = [10, 20, 30]; return a[0] + a[1] + a[2]; }"),
        60);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut a = [1, 2, 3];\n"
        "  a[1] = 42;\n"
        "  return a[1];\n"
        "}"), 42);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut a = [1, 2, 3];\n"
        "  let i = 2;\n"
        "  a[i] = 99;\n"
        "  return a[2];\n"
        "}"), 99);
}

static void t_array_len_and_print(void) {
    char buf[64];
    test_output(
        "fn main(): i64 {\n"
        "  let a = [10, 20, 30];\n"
        "  print(\"{}\", len(a));\n"
        "  print(a);\n"
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "3\n[10, 20, 30]\n");
}

static void t_array_rejections(void) {
    assert_error(
        "fn main(): i64 { let a = [1, true]; return 0; }", "array element");
    assert_error(
        "fn main(): i64 { let a = []; return 0; }", "");
    assert_error(
        "fn make(): [i64] { return [1, 2, 3]; }\n"
        "fn main(): i64 { return 0; }", "cannot return an array");
}

// ---- Result / Option ----
static void t_result_and_option(void) {
    ASSERT_EQ_INT(test_value(
        "fn get(): Result<i64, i64> { return Result.Ok(42); }\n"
        "fn main(): i64 {\n"
        "  match get() {\n"
        "    Result.Ok(v) => { return v; }\n"
        "    Result.Err(_) => { return 0; }\n"
        "  }\n"
        "}"), 42);
    ASSERT_EQ_INT(test_value(
        "fn find(): Option<i64> { return Option.Some(5); }\n"
        "fn main(): i64 {\n"
        "  match find() {\n"
        "    Option.Some(v) => { return v; }\n"
        "    Option.None => { return 0; }\n"
        "  }\n"
        "}"), 5);
    ASSERT_EQ_INT(test_value(
        "fn find(): Option<i64> { return Option.None; }\n"
        "fn main(): i64 {\n"
        "  match find() {\n"
        "    Option.Some(v) => { return v; }\n"
        "    Option.None => { return 0; }\n"
        "  }\n"
        "}"), 0);
}

static void t_try_operator(void) {
    ASSERT_EQ_INT(test_value(
        "fn inner(): Result<i64, i64> { return Result.Ok(10); }\n"
        "fn outer(): Result<i64, i64> {\n"
        "  let x = inner()?;\n"
        "  return Result.Ok(x + 1);\n"
        "}\n"
        "fn main(): i64 {\n"
        "  match outer() {\n"
        "    Result.Ok(v) => { return v; }\n"
        "    Result.Err(_) => { return 0; }\n"
        "  }\n"
        "}"), 11);
    ASSERT_EQ_INT(test_value(
        "fn inner(): Result<i64, i64> { return Result.Err(99); }\n"
        "fn outer(): Result<i64, i64> {\n"
        "  let x = inner()?;\n"
        "  return Result.Ok(x + 1);\n"
        "}\n"
        "fn main(): i64 {\n"
        "  match outer() {\n"
        "    Result.Ok(_) => { return 0; }\n"
        "    Result.Err(e) => { return e; }\n"
        "  }\n"
        "}"), 99);
}

static void t_field_read_view(void) {
    // x is a view of p.s; it must not own a reference.
    ASSERT_EQ_INT(test_value(
        "type P = struct { s: string }\n"
        "fn main(): i64 {\n"
        "  let p = P { s: \"hi\" };\n"
        "  let x = p.s;\n"
        "  return len(x);\n"
        "}\n"), 2);
}

static void t_array_rc_store_shared(void) {
    // arr[0] must own a reference; s's drop at the inner
    // scope must leave arr[0] valid.
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut arr = [\"\", \"\", \"\"];\n"
        "  {\n"
        "    let s = \"a\" + \"b\";\n"
        "    arr[0] = s;\n"
        "  }\n"
        "  return len(arr[0]);\n"
        "}\n"), 2);
}

static const TestCase cases[] = {
    { "struct_basics",           t_struct_basics           },
    { "struct_nested_argument",  t_struct_nested_and_argument },
    { "struct_print",            t_struct_print            },
    { "struct_return_rules",     t_struct_return_rules     },
    { "enum_variants_match",     t_enum_variants_and_match },
    { "enum_payloads",           t_enum_payloads           },
    { "enum_nested_pattern",     t_enum_nested_pattern     },
    { "enum_exhaustiveness",     t_enum_exhaustiveness     },
    { "enum_rejections",         t_enum_rejections         },
    { "array_basics",            t_array_basics            },
    { "array_len_and_print",     t_array_len_and_print     },
    { "array_rejections",        t_array_rejections        },
    { "result_and_option",       t_result_and_option       },
    { "try_operator",            t_try_operator            },
    { "field_read_view",         t_field_read_view         },
    { "array_store_rc_shared",   t_array_rc_store_shared   },
};
const TestGroup g_aggregate_group = {
    "aggregate", cases, sizeof(cases) / sizeof(cases[0])
};
