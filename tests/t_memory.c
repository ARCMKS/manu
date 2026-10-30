#include "../include/test.h"

// ---- RC basics ----
static void t_rc_local_scope(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { let r = rc 5; return 7; }"), 7);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { let r = rc 5; let s = r; return 7; }"), 7);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let r = rc 5;\n"
        "  { let s = r; let t = rc 9; }\n"
        "  return 7;\n"
        "}"), 7);
}

static void t_rc_reassign(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { let mut r = rc 5; r = rc 10; return 7; }"), 7);
}

static void t_rc_return_rules(void) {
    ASSERT_EQ_INT(test_value(
        "fn make(): rc<i64> { return rc 42; }\n"
        "fn main(): i64 { let r = make(); return 7; }"), 7);
    assert_error(
        "fn f(r: rc<i64>): rc<i64> { return r; }\n"
        "fn main(): i64 { return 0; }", "fresh");
    assert_error(
        "fn f(): rc<i64> { let x = 5; return rc &x; }\n"
        "fn main(): i64 { return 0; }", "");
}

static void t_rc_return_borrowed_param_rejected(void) {
    // A parameter is borrowed. Returning it would give the
    // caller a reference the callee doesn't own; sema rejects.
    assert_error(
        "fn f(p: rc<i64>): rc<i64> { return p; }\n"
        "fn main(): i64 { return 0; }\n", "fresh");
}

static void t_rc_null_noop(void) {
    ASSERT_EQ_INT(test_value(
        "fn maybe(): rc<i64> { if (false) { return rc 1; } }\n"
        "fn main(): i64 { let s = maybe(); return 7; }"), 7);
}

static void t_rc_early_return_drops(void) {
    // The leak-fix test: RC local in scope, early return, no
    // crash on subsequent iterations (which would happen if the
    // block double-freed or leaked severely).
    ASSERT_EQ_INT(test_value(
        "fn f(n: i64): i64 {\n"
        "  let r = rc n;\n"
        "  if (n < 0) { return -1; }\n"
        "  return 0;\n"
        "}\n"
        "fn main(): i64 {\n"
        "  let mut i = 0;\n"
        "  while (i < 100) {\n"
        "    f(-1);\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return 7;\n"
        "}"), 7);
}

// ---- References ----
static void t_ref_read_and_write(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { let x = 5; let p = &x; return *p; }"), 5);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let x = 5;\n"
        "  let p = &mut x;\n"
        "  *p = 42;\n"
        "  return x;\n"
        "}"), 42);
}

static void t_ref_rejections(void) {
    assert_error(
        "fn f(): ref<i64> { let x = 5; return &x; }\n"
        "fn main(): i64 { return 0; }", "reference");
    assert_error(
        "fn f(p: ref<ref<i64>>): i64 { return 0; }\n"
        "fn main(): i64 { return 0; }", "");
    assert_error(
        "fn main(): i64 {\n"
        "  let x = 5;\n"
        "  let p = &x;\n"
        "  *p = 42;\n"
        "  return x;\n"
        "}", "immutable");
}

// ---- Immutability ----
static void t_let_mut_enforcement(void) {
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 { let mut x = 5; x = 10; return x; }"), 10);
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut s = 0;\n"
        "  for i in 0..5 { s = s + i; }\n"
        "  return s;\n"
        "}"), 10);
    assert_error(
        "fn main(): i64 { let x = 5; x = 10; return x; }",
        "immutable binding");
    assert_error(
        "fn f(x: i64): i64 { x = x + 1; return x; }\n"
        "fn main(): i64 { return f(5); }", "immutable binding");
}

static void t_param_mut(void) {
    ASSERT_EQ_INT(test_value(
        "fn f(mut x: i64): i64 { x = x + 1; return x; }\n"
        "fn main(): i64 { return f(5); }"), 6);
}

static void t_rc_expr_discarded_no_leak(void) {
    // Discarded concats: correctness check (leak would not show
    // as a wrong value, but a double-free would corrupt).
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  for i in 0..1000 { \"a\" + \"b\"; }\n"
        "  return 42;\n"
        "}"), 42);
}

static void t_for_in_body_rc(void) {
    // RC local declared inside a for-in body. If the rc
    // scope is not pushed for the body, the local's drop
    // lands in the enclosing block, hits a reused slot,
    // and double-frees or reads garbage. Passing this test
    // is the smoke check.
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let a = rc [1, 2, 3];\n"
        "  let mut total = 0;\n"
        "  for x in a {\n"
        "    let r = rc x;\n"
        "    total = total + *r;\n"
        "  }\n"
        "  return total;\n"
        "}"), 6);
}

static void t_builtin_rc_discarded(void) {
    // args() returns rc<[string]>. As an expression statement
    // it must be RC_DEC'd, not POP'd. The test cannot detect a
    // leak directly; it runs the program (which must not crash)
    // and returns the value. Run under ASAN for the leak check.
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  args();\n"
        "  let a = args();\n"
        "  return len(a);\n"
        "}\n"), 0);
}

static void t_rc_return_owning_local(void) {
    // The local r owns its reference. return transfers that
    // ownership to the caller. Before the fix, r's reference
    // was dropped before RET, freeing the block.
    ASSERT_EQ_INT(test_value(
        "fn make(): rc<i64> {\n"
        "  let r = rc 42;\n"
        "  return r;\n"
        "}\n"
        "fn main(): i64 {\n"
        "  let v = make();\n"
        "  return *v;\n"
        "}\n"), 42);
}

static void t_rc_return_shared_local(void) {
    // Two locals share a reference. Returning one must leave
    // the block at refcount 1 for the caller (the other
    // local's drop brings it from 2 to 1). Before the fix,
    // both locals were dropped, freeing the block, and the
    // caller received a dangling pointer.
    ASSERT_EQ_INT(test_value(
        "fn make(): rc<i64> {\n"
        "  let a = rc 99;\n"
        "  let b = a;\n"
        "  return b;\n"
        "}\n"
        "fn main(): i64 {\n"
        "  let v = make();\n"
        "  return *v;\n"
        "}\n"), 99);
}

static void t_struct_rc_field_reassignment(void) {
    // P has an RC-typed field, so P is RC-managed. Reassigning
    // p must drop the old p (which deep-drops its field), not
    // leak.
    ASSERT_EQ_INT(test_value(
        "type P = struct { s: string }\n"
        "fn main(): i64 {\n"
        "  let mut p = P { s: \"a\" + \"b\" };\n"
        "  p = P { s: \"c\" + \"d\" };\n"
        "  return len(p.s);\n"
        "}\n"), 2);
}

static void t_struct_rc_field_return(void) {
    // A struct with RC content is returnable by value (its
    // type is rc<P>, which is not the bare struct return that
    // sema's escape rules forbid).
    ASSERT_EQ_INT(test_value(
        "type P = struct { s: string }\n"
        "fn make(): P { return P { s: \"hi\" }; }\n"
        "fn main(): i64 {\n"
        "  let p = make();\n"
        "  return len(p.s);\n"
        "}\n"), 2);
}

static void t_struct_rc_field_nested(void) {
    // Transitive RC content: Outer's field is Inner, which
    // has an RC field.
    ASSERT_EQ_INT(test_value(
        "type Inner = struct { s: string }\n"
        "type Outer = struct { i: Inner, n: i64 }\n"
        "fn main(): i64 {\n"
        "  let o = Outer { i: Inner { s: \"hi\" }, n: 7 };\n"
        "  return len(o.i.s) + o.n;\n"
        "}\n"), 9);
}

static void t_struct_no_rc_stays_arena(void) {
    // A struct with no RC content is unaffected: it stays on
    // the arena and does not acquire a refcount.
    ASSERT_EQ_INT(test_value(
        "type P = struct { x: i64, y: i64 }\n"
        "fn main(): i64 {\n"
        "  let p = P { x: 3, y: 4 };\n"
        "  return p.x + p.y;\n"
        "}\n"), 7);
}

static void t_enum_rc_payload(void) {
    // A user enum with an RC-typed payload is RC-managed.
    ASSERT_EQ_INT(test_value(
        "type E = enum { A(string), B(i64) }\n"
        "fn main(): i64 {\n"
        "  let e = E.A(\"hi\");\n"
        "  match e {\n"
        "    E.A(s) => { return len(s); }\n"
        "    E.B(_) => { return 0; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n"), 2);
}

static void t_view_reassignment_survives(void) {
    // p.s is reassigned while x holds a view. Under the fix,
    // x INC'd at bind; the reassign DEC's the old block to 1
    // instead of 0; x still points at valid memory.
    ASSERT_EQ_INT(test_value(
        "type P = struct { s: string }\n"
        "fn main(): i64 {\n"
        "  let mut p = P { s: \"a\" + \"b\" };\n"
        "  let x = p.s;\n"
        "  p.s = \"hi\" + \" hello\";\n"
        "  p.s = \"some long string literally sure\";\n"
        "  p = P { s: \"c\" + \"d\" };\n"
        "  return len(x);\n"
        "}\n"), 2);
}

static void t_view_via_assign(void) {
    ASSERT_EQ_INT(test_value(
        "type P = struct { s: string }\n"
        "fn main(): i64 {\n"
        "  let mut x = \"\";\n"
        "  {\n"
        "    let mut p = P { s: \"a\" + \"b\" };\n"
        "    x = p.s;\n"
        "    p.s = \"c\" + \"d\";\n"
        "  }\n"
        "  return len(x);\n"
        "}\n"), 2);
}

static void t_view_loop_survives(void) {
    // The loop variable owns; reassigning arr[0] inside the
    // body cannot free the current element under the var.
    ASSERT_EQ_INT(test_value(
        "fn main(): i64 {\n"
        "  let mut arr = rc [\"a\" + \"b\", \"c\" + \"d\", \"e\" + \"f\"];\n"
        "  let mut n = 0;\n"
        "  for x in arr {\n"
        "    arr[0] = \"xyz\" + \"xyz\";\n"
        "    n = n + len(x);\n"
        "  }\n"
        "  return n;\n"
        "}\n"), 6);
}

static const TestCase cases[] = {
    { "rc_local_scope",           t_rc_local_scope           },
    { "rc_reassign",              t_rc_reassign              },
    { "rc_return_rules",          t_rc_return_rules          },
    { "rc_return_borrowed_param_rejected", t_rc_return_borrowed_param_rejected},
    { "rc_null_noop",             t_rc_null_noop             },
    { "rc_early_return_drops",    t_rc_early_return_drops    },
    { "ref_read_and_write",       t_ref_read_and_write       },
    { "ref_rejections",           t_ref_rejections           },
    { "let_mut_enforcement",      t_let_mut_enforcement      },
    { "param_mut",                t_param_mut                },
    { "rc_expr_discarded",        t_rc_expr_discarded_no_leak},
    { "for_in_body_rc",           t_for_in_body_rc           },
    { "builtin_rc_discarded",     t_builtin_rc_discarded     },
    { "rc_return_owning_local",   t_rc_return_owning_local   },
    { "rc_return_shared_local",   t_rc_return_shared_local   },
    { "struct_rc_field_reassignment", t_struct_rc_field_reassignment },
    { "struct_rc_field_return",       t_struct_rc_field_return       },
    { "struct_rc_field_nested",       t_struct_rc_field_nested       },
    { "struct_no_rc_stays_arena",     t_struct_no_rc_stays_arena     },
    { "enum_rc_payload",              t_enum_rc_payload              },
    { "view_reassignment_survives", t_view_reassignment_survives },
    { "view_via_assign",            t_view_via_assign            },
    { "view_loop_survives",         t_view_loop_survives         },
};
const TestGroup g_memory_group = {
    "memory", cases, sizeof(cases) / sizeof(cases[0])
};
