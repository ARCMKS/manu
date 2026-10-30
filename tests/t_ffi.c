#include "../include/test.h"

#ifdef _WIN32
#  define LIBC_PATH "ucrtbase.dll"
#  define LIBM_PATH "ucrtbase.dll"
#else
#  define LIBC_PATH "libc.so.6"
#  define LIBM_PATH "libm.so.6"
#endif

#define LIBC_DECL "extern lib \"" LIBC_PATH "\" as libc\n"
#define LIBM_DECL "extern lib \"" LIBM_PATH "\" as m\n"

static void t_strlen_and_abs(void) {
    ASSERT_EQ_INT(test_value(
        LIBC_DECL
        "extern fn strlen(s: *u8): u64 from libc\n"
        "extern fn abs(x: i32): i32 from libc\n"
        "fn main(): i64 {\n"
        "  return strlen(c\"hello\") as i64 + abs(-37) as i64;\n"
        "}"), 42);
}

#ifndef _WIN32
static void t_variadic_snprintf(void) {
    ASSERT_EQ_INT(test_value(
        LIBC_DECL
        "extern fn sprintf(dst: *u8, n: u64, fmt: *u8, ...): i32 from libc\n"
        "fn main(): i64 {\n"
        "  let dst = ffi_alloc(16) as *u8;\n"
        "  sprintf(dst, 16, c\"%d\", 42);\n"
        "  let s = ffi_string(dst);\n"
        "  let n = len(s);\n"
        "  ffi_free(dst);\n"
        "  return n;\n"
        "}"), 2);
}
#endif

static void t_ffi_cstring_roundtrip(void) {
    ASSERT_EQ_INT(test_value(
        LIBC_DECL
        "extern fn strlen(s: *u8): u64 from libc\n"
        "fn main(): u64 { return strlen(ffi_cstring(\"dynamic\")); }\n"), 7);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { let s = ffi_string(c\"hello\"); return len(s); }"), 5);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { let s = ffi_string(raw_null()); return len(s); }"), 0);
}

static void t_raw_pointer_builtins(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let p = raw_null();\n"
        "  if (raw_is_null(p)) { return 1 }\n"
        "  return 0\n"
        "}"), 1);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { let s = c\"hello\"; return raw_load_u8(s, 0); }"), 104);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let s = c\"abcdef\";\n"
        "  let p = raw_offset(s, 2);\n"
        "  return raw_load_u8(p, 0);\n"
        "}"), 99);
}

static void t_raw_store(void) {
    char buf[64];
    test_output(
        "fn main(): i64 {\n"
        "  let p = ffi_cstring(\"    \");\n"
        "  raw_store_u8(p, 0, 104);\n"
        "  raw_store_u8(p, 1, 105);\n"
        "  raw_store_u8(p, 2, 33);\n"
        "  raw_store_u8(p, 3, 0);\n"
        "  print(\"{}\", ffi_string(p));\n"
        "  return 0;\n"
        "}\n", buf, sizeof buf);
    ASSERT_EQ_STR(buf, "hi!\n");
}

static void t_option_ptr_return(void) {
    ASSERT_EQ_INT(test_value(
        LIBC_DECL
        "extern fn getenv(name: *u8): Option<*u8> from libc\n"
        "fn main(): i64 {\n"
        "  match getenv(c\"PATH\") {\n"
        "    Option.Some(_) => { return 1 }\n"
        "    Option.None => { return 0 }\n"
        "  }\n"
        "}"), 1);
    ASSERT_EQ_INT(test_value(
        LIBC_DECL
        "extern fn getenv(name: *u8): Option<*u8> from libc\n"
        "fn main(): i64 {\n"
        "  match getenv(c\"PRISM_TEST_UNSET_VAR_XYZ_42\") {\n"
        "    Option.Some(_) => { return 0 }\n"
        "    Option.None => { return 1 }\n"
        "  }\n"
        "}"), 1);
}

static void t_option_ptr_arg(void) {
    ASSERT_EQ_INT(test_value(
        LIBC_DECL
        "extern fn free(p: Option<*u8>): void from libc\n"
        "fn make_none(): Option<*u8> { return Option.None }\n"
        "fn main(): i64 { let p = make_none(); free(p); return 0 }\n"), 0);
}

static void t_ffi_type_rejections(void) {
    assert_error(
        LIBC_DECL
        "extern fn bad(s: string): i32 from libc\n"
        "fn main(): i64 { return 0; }\n",
        "not FFI-compatible");
    assert_error(
        LIBC_DECL
        "extern fn printf(fmt: *u8, ...): i32 from libc\n"
        "fn main(): i64 { printf(c\"%f\\n\", 3.14); return 0; }\n",
        "variadic");
    assert_error(
        LIBC_DECL
        "extern fn bad(): Option<i64> from libc\n"
        "fn main(): i64 { return 0 }\n",
        "not FFI-compatible");
}

static void t_ffi_in_loop(void) {
    ASSERT_EQ_INT(test_value(
        LIBC_DECL
        "extern fn abs(x: i32): i32 from libc\n"
        "fn main(): i64 {\n"
        "  let mut s = 0;\n"
        "  for i in 0..5 { s = s + abs(-1) as i64; }\n"
        "  return s;\n"
        "}"), 5);
}

static const TestCase cases[] = {
    { "strlen_and_abs",       t_strlen_and_abs        },
#ifndef _WIN32
    { "variadic_snprintf",    t_variadic_snprintf     },
#endif
    { "ffi_cstring_roundtrip",t_ffi_cstring_roundtrip },
    { "raw_pointer_builtins", t_raw_pointer_builtins  },
    { "raw_store",            t_raw_store             },
    { "option_ptr_return",    t_option_ptr_return     },
    { "option_ptr_arg",       t_option_ptr_arg        },
    { "ffi_type_rejections",  t_ffi_type_rejections   },
    { "ffi_in_loop",          t_ffi_in_loop           },
};
const TestGroup g_ffi_group = {
    "ffi", cases, sizeof(cases) / sizeof(cases[0])
};