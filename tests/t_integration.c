// Whole programs exercising many features at once.
#include "../include/test.h"

static void t_arithmetic_and_return(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let a = 15\n"
        "  let b = 27\n"
        "  return a + b\n"
        "}\n"), 42);
}

static void t_ifs_loops_and_shadowing(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut i = 0\n"
        "  let mut sum = 0\n"
        "  while (i < 5) {\n"
        "    sum = sum + i\n"
        "    i = i + 1\n"
        "  }\n"
        "  { let sum = 1000; }\n"        // shadow
        "  return sum\n"
        "}\n"), 10);
}

static void t_bare_return_and_asi(void) {
    ASSERT_EQ_INT(test_value(
        "fn noop() {\n"
        "  return\n"
        "}\n"
        "fn main(): i64 {\n"
        "  noop()\n"
        "  return 7\n"
        "}\n"), 7);
}

static void t_stray_semicolon_and_blank_lines(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "\n"
        "  let a = 5\n"
        "  ;\n"
        "\n"
        "  return a\n"
        "}\n"), 5);
}

static void t_single_line_blocks(void) {
    ASSERT_EQ_INT(test_value("fn main(): i64 { return 42 }"), 42);
    ASSERT_EQ_INT(test_value("fn main(): i64 { let a = 3; return a + 2; }"), 5);
}

static void t_print_modes(void) {
    char buf[128];
    test_output(
        "fn main(): i64 {\n"
        "  print()\n"
        "  print(42)\n"
        "  print(1, 2, 3)\n"
        "  print(\"x = {}, y = {}\", 5, 6)\n"
        "  print(true, false)\n"
        "  return 0\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "\n42\n1 2 3\nx = 5, y = 6\ntrue false\n");
}

static void t_string_concat_and_len(void) {
    char buf[64];
    test_output(
        "fn main(): i64 {\n"
        "  let a = \"hello\" + \" \" + \"world\"\n"
        "  print(\"{}\", len(a))\n"
        "  print(a)\n"
        "  return 0\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "11\nhello world\n");
}

static void t_mixed_flow_program(void) {
    char buf[128];
    test_output(
        "type P = struct { x: i64, y: i64 }\n"
        "fn P.sum(p: P): i64 { return p.x + p.y }\n"
        "fn main(): i64 {\n"
        "  let p = P { x: 3, y: 4 }\n"
        "  let mut total = 0\n"
        "  for i in 0..3 {\n"
        "    total = total + p.sum()\n"
        "  }\n"
        "  print(\"{}\", total)\n"        // 21
        "  return 0\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "21\n");
}

static const TestCase cases[] = {
    { "arithmetic_and_return",       t_arithmetic_and_return       },
    { "ifs_loops_and_shadowing",     t_ifs_loops_and_shadowing     },
    { "bare_return_and_asi",         t_bare_return_and_asi         },
    { "stray_semicolon_blank_lines", t_stray_semicolon_and_blank_lines },
    { "single_line_blocks",          t_single_line_blocks          },
    { "print_modes",                 t_print_modes                 },
    { "string_concat_and_len",       t_string_concat_and_len       },
    { "mixed_flow_program",          t_mixed_flow_program          },
};
const TestGroup g_integration_group = {
    "integration", cases, sizeof(cases) / sizeof(cases[0])
};
