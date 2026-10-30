#include "../include/test.h"

static void t_if_else(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { if (1 < 2) { return 10; } else { return 20; } }"), 10);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { if (1 > 2) { return 10; } else { return 20; } }"), 20);
}

static void t_while(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut i = 0;\n"
        "  let mut sum = 0;\n"
        "  while (i < 5) { sum = sum + i; i = i + 1; }\n"
        "  return sum;\n"
        "}"), 10);
}

static void t_for_range(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut s = 0;\n"
        "  for i in 0..5 { s = s + i; }\n"
        "  return s;\n"
        "}"), 10);
    ASSERT_EQ_INT(test_value(
        "fn sum_to(n: i64): i64 {\n"
        "  let mut s = 0;\n"
        "  for i in 0..n { s = s + i; }\n"
        "  return s;\n"
        "}\n"
        "fn main(): i64 { return sum_to(10); }"), 45);
}

static void t_for_in(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut s = 0;\n"
        "  for x in [10, 20, 30] { s = s + x; }\n"
        "  return s;\n"
        "}"), 60);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut s = 0;\n"
        "  let a = rc [1, 2, 3, 4];\n"
        "  for x in a { s = s + x; }\n"
        "  return s;\n"
        "}"), 10);
}

static void t_break_continue(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut s = 0;\n"
        "  for i in 0..10 {\n"
        "    if (i == 5) { break }\n"
        "    s = s + i;\n"
        "  }\n"
        "  return s;\n"
        "}"), 10);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut s = 0;\n"
        "  for i in 0..6 {\n"
        "    if (i == 3) { continue }\n"
        "    s = s + i;\n"
        "  }\n"
        "  return s;\n"
        "}"), 12);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut total = 0;\n"
        "  for i in 0..5 {\n"
        "    for j in 0..5 {\n"
        "      if (j == 2) { break }\n"
        "      total = total + 1;\n"
        "    }\n"
        "  }\n"
        "  return total;\n"
        "}"), 10);
    assert_error(
        "fn main(): i64 { break; return 0; }", "outside of a loop");
    assert_error(
        "fn main(): i64 { continue; return 0; }", "outside of a loop");
}

static void t_for_errors(void) {
    assert_error(
        "fn main(): i64 { let x = 5; for i in x { print(\"{}\", i); } return 0; }",
        "cannot iterate");
    assert_error(
        "fn main(): i64 {\n"
        "  let m: i32 = 0;\n"
        "  let n: i8 = 10;\n"
        "  for i in m..n { print(\"{}\", i); }\n"
        "  return 0;\n"
        "}", "different types");
}

static const TestCase cases[] = {
    { "if_else",         t_if_else         },
    { "while",           t_while           },
    { "for_range",       t_for_range       },
    { "for_in",          t_for_in          },
    { "break_continue",  t_break_continue  },
    { "for_errors",      t_for_errors      },
};
const TestGroup g_control_group = {
    "control", cases, sizeof(cases) / sizeof(cases[0])
};
