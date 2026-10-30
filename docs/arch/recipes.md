# Documentation drafting — Session 16 (final)

Two things: `docs/arch/recipes.md`, then the consistency pass across the whole doc set.

---

## `docs/arch/recipes.md`

```markdown
# Recipes

*Current as of commit `<hash>`, <date>.*

Task-oriented. Each recipe is a checklist of the files to touch and
the specific edits in each, followed by a small worked example.

These are the tasks that come up repeatedly. The list is
deliberately narrow: tasks that appear in the codebase's history
or that a maintainer will need to repeat. Novel work (adding a
feature that has never existed) will not be covered.

The recipes assume you've read `arch/overview.md` and can
navigate `arch/files/*.md`.

---

## §1 — Adding an opcode

Adding an opcode means adding a new instruction to the VM's
vocabulary. Codegen emits it; the VM executes it; the disassembler
lists it; the validator checks its operands.

**Files to touch:** `vm.h`, `vm.c`, `disasm.c`, and one or more
emission sites in `codegen.c` if the opcode is to be used.

**Checklist:**

1. **`vm.h`** — add the new value to the `OpCode` enum, before
   `OP_COUNT`. Choose the position carefully: adding an opcode in
   the middle renumbers every subsequent opcode, which changes
   the on-disk encoding of any cached chunks. Adding at the end
   (before `OP_COUNT`) is safe.
2. **`vm.c`** — add a `case` to `vm_run`'s dispatch loop. Read any
   operands via `READ()`. Emit `PUSH`/`POP` for stack effects.
   Call `RT_ERR(...)` for runtime errors (which returns 1 from
   `vm_run`).
3. **`disasm.c`** — add a row to `OPINFO` with the mnemonic and
   the number of operand words. Add a case to `validate_bytecode`
   if the opcode has structural invariants (operand ranges,
   target validity). Add a case to the operand-rendering switch
   in `disassemble`.
4. **`codegen.c`** (if the opcode is emitted) — add the emission
   at the appropriate `cg_expr` or `cg_stmt` case, or at the
   prologue/fallback sites.

**Worked example: `OP_DUP2`.**

`vm.h`:
```c
OP_DUP2,
```
(inserted after `OP_DUP`, before `OP_LOAD_LOCAL`.)

`vm.c`:
```c
case OP_DUP2:
    if (vm->sp < 2) RT_ERR("dup2 on shallow stack%s", "");
    U64 a = vm->data_stack[vm->sp - 2];
    U64 b = vm->data_stack[vm->sp - 1];
    PUSH(a);
    PUSH(b);
    break;
```

`disasm.c`'s `OPINFO`:
```c
[OP_DUP2] = { "DUP2", 0 },
```

No validator case (no operands to check). No operand rendering
(no operands to render).

Emission site: `codegen.c`'s `AST_ASSIGN_OP` for `AST_INDEX`
emits `OP_DUP2` to duplicate the container and index before
`OP_INDEX_LOAD`. See `arch/files/17-codegen.md` §9.

**Test.** Add a test that exercises the emission. `OP_DUP2`'s
test is in `t_memory.c` (via the compound-index-assignment path)
and in `t_array.c` (via `a[i] = v` with a save-and-reload).

**Common mistakes.** Forgetting the `OPINFO` row produces a
silent desynchronization: the validator advances by 1 word for a
0-operand opcode but the real opcode has operands, so the listing
mis-steps. Forgetting the validator case means a bad operand
reaches the VM and traps at runtime instead of failing the
structural check.

---

## §2 — Adding an AST node kind

An AST node kind represents a new syntactic or semantic construct.
The node participates in parsing, free, monomorphization, sema,
and codegen.

**Files to touch:** `ast.h`, `ast.c`, `parser.c` (if the node is
produced by the parser), `sema.c` (or the appropriate sema file),
`codegen.c`, `monomorph.c`.

**Checklist:**

1. **`ast.h`** — add the new kind to `AstKind`. If the node has a
   payload, add a `struct { ... }` member to the union `as`. If
   the payload has a non-trivial initial state (a sentinel other
   than 0), document it.
2. **`ast.c`** — add a constructor (`ast_<kind>`). If the payload
   owns heap pointers or lists, add a case to `ast_free`.
3. **`parser.c`** (if parser-produced) — add the parsing logic.
   Add a corresponding `expect`/`match` sequence. Add a span.
4. **`sema.c`** (or the appropriate sema file) — add a case to
   `analyze_expr` or `analyze_stmt`. The case must return the
   node's resolved type (or void) and set every resolution field
   the node uses.
5. **`codegen.c`** — add a case to `cg_expr` or `cg_stmt`. The
   case must leave exactly one value on the stack (for `cg_expr`)
   or leave the stack balanced (for `cg_stmt`).
6. **`monomorph.c`** — add a case to `mono_copy_node` if the node
   can appear in a generic template. Deep-copy every owned
   pointer and reset every resolution field to its sentinel.
7. **`disasm.c`** — usually no change. The disassembler does not
   know about AST nodes.

**Worked example: `AST_SIZEOF`.**

`ast.h`:
```c
typedef enum {
    ...
    AST_SIZEOF,
    ...
} AstKind;
```

Union member:
```c
struct {
    TypeExpr*    te;
    struct Type* resolved;
    I64          size;
} sizeof_expr;
```

`ast.c`:
```c
AstNode* ast_sizeof(Span span, TypeExpr* te) {
    AstNode* n = alloc_node(AST_SIZEOF, span);
    n->as.sizeof_expr.te       = te;
    n->as.sizeof_expr.resolved = NULL;
    n->as.sizeof_expr.size     = 0;
    return n;
}
```

`ast_free` case:
```c
case AST_SIZEOF:
    type_expr_free(n->as.sizeof_expr.te);
    break;
```

`parser.c`'s `primary`: recognize `sizeof` followed by `(`, parse
the type expression, wrap in an `ast_sizeof`.

`sema.c`'s `analyze_expr`: resolve the type expression, reject
`void` and opaque types, set `size` and `resolved`, return
`ty_u64()`.

`codegen.c`'s `cg_expr`:
```c
case AST_SIZEOF:
    set_span(cg, n);
    emit(cg, OP_PUSH); emit(cg, (U64)n->as.sizeof_expr.size);
    break;
```

`monomorph.c`'s `mono_copy_node`:
```c
case AST_SIZEOF:
    c->as.sizeof_expr.te =
        mono_copy_type_expr(n->as.sizeof_expr.te, sub, smap);
    c->as.sizeof_expr.resolved = NULL;
    c->as.sizeof_expr.size     = 0;
    break;
```

**Test.** Add a parser test (produces the node), a sema test
(rejects `sizeof(void)`), a runtime test (`sizeof(i64) == 8`),
and a monomorph test (a generic function using `sizeof(T)`).

---

## §3 — Adding a builtin function

A builtin is a function-like construct the compiler knows about
that does not have a Prism source declaration. `print`, `len`,
`format`, `raw_offset`, `ffi_cstring` are builtins.

**Files to touch:** `sema.c`, `codegen.c`, and `vm.c` if the
builtin requires a new native.

**Checklist:**

1. **`sema.c`** — add a row to the `B[]` table inside
   `analyze_expr`'s `AST_CALL` case. Fields: name, min_args,
   max_args, ret_kind. The `ret_kind` is a code:
   - 0 = void, 1 = i64, 2 = string, 3 = `*u8`, 4 = `*void`,
   - 5 = same `*T` as arg 0, 6 = bool, 7 = `rc<[string]>`,
   - 8 = `Option<string>`, 9 = string.
2. **`sema.c`** — the arg-checking in the builtin loop analyzes
   each argument but does not check it against a target type
   (builtins are polymorphic). If the builtin has type
   constraints (e.g. "argument must be a pointer"), add an
   explicit check after analysis.
3. **`codegen.c`** — add a branch to `cg_expr`'s `AST_CALL`
   builtin dispatch (`if (n->as.call.builtin_id == N) { ... }`).
   Emit the sequence the builtin corresponds to.
4. **`vm.c`** (if the builtin needs a new native) — add an ID to
   `NativeId` in `vm.h`, implement the native, register it in
   `vm_init`.
5. **`disasm.c`** — add a row to `disasm.c`'s `NATIVE_NAMES`
   table if the builtin registers a native.

**Worked example: `NATIVE_RAW_OFFSET`.**

`vm.h`'s `NativeId`:
```c
NATIVE_RAW_OFFSET = 8,
```

`vm.c`'s native:
```c
static void native_raw_offset(VM* vm) {
    if (vm->sp < 3)
        NATIVE_ERR(vm, "raw_offset needs 3 operands%s", "");
    U64 elem_size = POP();
    I64 n         = (I64)POP();
    U64 p         = POP();
    PUSH((U64)((I64)p + n * (I64)elem_size));
}
```

`vm_init`:
```c
vm_register_native(vm, NATIVE_RAW_OFFSET, native_raw_offset);
```

`disasm.c`'s `NATIVE_NAMES`:
```c
[NATIVE_RAW_OFFSET] = "raw_offset",
```

`sema.c`'s `B[]` table:
```c
{ "raw_offset", 2, 2, 5 },
```
(ret_kind 5 means "same `*T` as arg 0".)

`codegen.c`:
```c
if (n->as.call.builtin_id == 9) {           // raw_offset
    AstNode* a0 = n->as.call.args.items[0];
    AstNode* a1 = n->as.call.args.items[1];
    cg_expr(cg, a0);
    cg_expr(cg, a1);
    U64 elem_size = 1;
    if (a0->type && a0->type->kind == TY_RAWPTR
        && a0->type->inner
        && a0->type->inner->kind != TY_VOID)
        elem_size = (U64)a0->type->inner->size;
    if (elem_size == 0) elem_size = 1;
    set_span(cg, n);
    emit(cg, OP_PUSH); emit(cg, elem_size);
    emit(cg, OP_CCALL);
    emit(cg, NATIVE_RAW_OFFSET);
    break;
}
```

**Test.** Add a `t_ffi_io.c` case exercising the builtin.

**Common mistakes.** Forgetting to register the native in
`vm_init` produces a runtime error `bad native index N`.
Forgetting the `ret_kind` mapping means the builtin's return type
defaults to `i64`, which may be wrong.

---

## §4 — Adding a keyword

A keyword is a reserved word recognized by the lexer. Prism has
26 keywords.

**Files to touch:** `lexer.h`, `lexer.c`, and `parser.c` (if the
keyword has a syntactic role).

**Checklist:**

1. **`lexer.h`** — add a `TOK_<NAME>` value to `TokenKind`.
   Insert before `TOK_EOF` and `TOK_ERROR` to avoid renumbering
   them.
2. **`lexer.c`'s `keyword_or_ident`** — add a `KW("name",
   TOK_NAME)` row.
3. **`lexer.c`'s `token_kind_name`** — add a case returning the
   keyword's text (e.g. `"if"`).
4. **`parser.c`** — add handling at each site where the keyword
   is valid. For a statement-starting keyword, add a branch in
   `statement`. For an operator, add a precedence level. For a
   modifier, add a check in the relevant declaration production.

**Worked example: `mut`.**

`lexer.h`:
```c
TOK_MUT,
```

`lexer.c`:
```c
KW("mut", TOK_MUT),
```

and

```c
case TOK_MUT: return "mut";
```

`parser.c`'s `let_declaration`: `bool is_mut = match(p, TOK_MUT);`
before parsing the identifier. `parse_type_expr`'s `ref` branch:
`bool is_mut = match(p, TOK_MUT);` before parsing the inner type.

**Test.** Add a `t_lexer.c` case for `mut` tokenization, and
`t_mut.c` cases for the syntax's behavior.

**Common mistakes.** Forgetting the `token_kind_name` case means
diagnostics print `?` for the token. Forgetting the parser site
means the keyword is recognized but does nothing.

---

## §5 — Adding a native function

A native is a `void (*)(VM*)` that the VM dispatches to via
`OP_CCALL`. Natives manipulate the data stack directly.

**Files to touch:** `vm.h`, `vm.c`, `disasm.c`.

**Checklist:**

1. **`vm.h`** — add a `NATIVE_<NAME>` value to `NativeId`, before
   `NATIVE_COUNT`.
2. **`vm.c`** — implement the native as a `static void`. Pop its
   arguments (from the top down; the last-pushed argument is on
   top), do its work, push its result. Use `NATIVE_ERR` for
   error reporting (which sets `native_failed` and returns).
3. **`vm.c`'s `vm_init`** — register the native.
4. **`disasm.c`'s `NATIVE_NAMES`** — add a row.
5. **`codegen.c`** — add an emission site that pushes the args
   and emits `OP_CCALL NATIVE_<NAME>`.

**Worked example: `NATIVE_MEMCPY`.**

`vm.h`:
```c
NATIVE_MEMCPY = 20,
```

`vm.c`:
```c
static void native_memcpy(VM* vm) {
    if (vm->sp < 3) NATIVE_ERR(vm, "memcpy needs 3 operands%s", "");
    U64 n   = POP();
    U64 src = POP();
    U64 dst = POP();
    if (dst && src && n)
        memcpy((void*)(uintptr_t)dst, (void*)(uintptr_t)src, (size_t)n);
    PUSH(0);
}
```

`vm_init`:
```c
vm_register_native(vm, NATIVE_MEMCPY, native_memcpy);
```

`disasm.c`:
```c
[NATIVE_MEMCPY] = "memcpy",
```

`codegen.c` (in the extern struct literal path):
```c
emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)slot);   // dst
...
cg_expr(cg, init->as.struct_init.value);         // src
emit(cg, OP_PUSH);
emit(cg, (U64)ftype->tdef->struct_size);         // n
emit(cg, OP_CCALL); emit(cg, NATIVE_MEMCPY);
emit(cg, OP_POP);   // discard the void result
```

**Test.** Add a test exercising the native end-to-end. For
`NATIVE_MEMCPY`, the extern-struct-literal path in `t_ffi.c`
exercises it.

**Common mistakes.** Natives must consume exactly the args they
pop and push exactly one result. Leaving the stack unbalanced
produces cascading failures. Forgetting `vm_register_native`
produces a runtime `bad native index` error.

---

## §6 — Adding a diagnostic

A diagnostic is a `diag_error(...)` call at a detection site.

**Files to touch:** the detection site; `docs/language/errors.md`
for the catalog; `tests/t_diagnostics.c` for a test.

**Checklist:**

1. **Detection site** — call `diag_error(bag, span, "format",
   args...)`. Choose the span carefully: it should point at the
   smallest source range that identifies the problem.
2. **`docs/language/errors.md`** — add an entry. Include the
   message text, the emitting file, the trigger, and a fix
   hint.
3. **`tests/t_diagnostics.c`** — add a test using `assert_error`
   with the message's distinctive substring as the needle.

**Worked example: `literal N does not fit in T`.**

`sema.c`'s `AST_INT` case:
```c
case AST_INT:
    if (!n->type) n->type = ty_i64();
    if (is_integer_type(n->type)
        && !uint_value_fits(n->as.int_lit.value, n->type)) {
        diag_error(bag, n->span,
            "literal %llu does not fit in %s",
            (unsigned long long)n->as.int_lit.value,
            type_name(n->type));
    }
    return n->type;
```

`docs/language/errors.md`:
```
### `literal <N> does not fit in <T>`
Emitted by: `sema.c`, in `AST_INT`.
Trigger: an integer literal in an annotated context whose value
exceeds the target type's range.
Fix: use a wider type or a smaller literal.
```

`tests/t_diagnostics.c`:
```c
static void d_literal_does_not_fit(void) {
    assert_error("fn main(): i64 { let x: i8 = 300; return 0; }",
                 "does not fit");
}
```

**Test.** Add to `t_diagnostics.c` and re-run.

**Common mistakes.** Using a vague needle (`"error"`,
`"expected"`) means the test passes when it shouldn't. Use a
substring that uniquely identifies the message.

---

## §7 — Adding a test

Tests live in `tests/t_*.c`. Each file exports one
`const TestGroup`.

**Files to touch:** the appropriate `t_*.c` file, and possibly
`tests/test.c` to register a new group.

**Checklist:**

1. **Choose the file.** Language behavior goes in the file
   corresponding to the language feature (`t_aggregate.c` for
   structs/enums, `t_memory.c` for RC/borrow, `t_control.c` for
   flow). Compiler-internal tests go in the file matching the
   subsystem.
2. **Write the test.** A `static void t_<name>(void)` that calls
   `test_value`, `test_output`, `assert_error`, or
   `assert_any_error`.
3. **Add a case entry** to the file's `cases[]` array.
4. **If a new file**, add an `extern const TestGroup g_<name>_group`
   to `tests/test.c` and a pointer in `g_all_groups`.

**Assertion choices:**

- **`test_value(src)`** — asserts the program compiled; returns
  the top of stack. Use when the test's outcome is a value.
- **`test_output(src, buf, cap)`** — asserts the program compiled
  and ran; captures stdout. Use when the outcome is output.
- **`assert_error(src, needle)`** — asserts at least one
  diagnostic contains `needle`. Use for negative tests.
- **`assert_any_error(src)`** — asserts at least one diagnostic
  exists. Use when any error suffices.
- **`ASSERT_EQ_INT(a, b)`** etc. within the test for value
  comparisons.

**Worked example.**

```c
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
```

Case entry:
```c
{ "enum_payloads", t_enum_payloads },
```

**Common mistakes.** Using `test_run(src, NULL)` when a value is
expected. `test_run` returns 0 on compile or runtime error, so a
test written with it can pass for the wrong reason. Use
`test_value` instead.

---

## §8 — Adding a new `Type` or `TypeDesc`

Adding a new primitive type is a cross-cutting change: the type
appears in the source, the compiler's type table, the runtime
descriptor table, the arithmetic dispatch, the cast matrix, and
the printer.

**Files to touch:** `types.h`, `types.c`, `vm.h`, `vm.c`,
`sema.c`, `codegen.c`, `disasm.c`.

**Checklist:**

1. **`types.h`'s `TypeKind`** — add the new kind. If it's a
   primitive, the size is fixed.
2. **`types.c`** — add a static `Type` singleton and a `ty_*`
   accessor. Add cases to `desc_kind_of`, `type_name`, and every
   predicate that should recognize it.
3. **`types.h`'s `TypeDescKind`** — add the parallel descriptor
   kind. `TD_COUNT` increments automatically.
4. **`types.c`'s `desc_kind_of`** — map the `TypeKind` to the
   `TypeDescKind`.
5. **`vm.c`** — add cases to the dispatch of every parameterized
   opcode that should support the type: `OP_BINARY`, `OP_UNARY`,
   `OP_CONVERT`, `NATIVE_PRINT_VALUE`.
6. **`sema.c`** — add cases to `binary_result_type`,
   `unary_result_type`, `cast_valid` if the type participates in
   these.
7. **`codegen.c`** — add cases to `memkind_of` if the type has a
   memory access width distinct from the existing ones.
8. **`disasm.c`** — add cases to `print_desc` if the type needs a
   non-default rendering.

**Worked example: adding `f32`.**

`types.h`:
```c
TY_F32,
```
and
```c
TD_F32,
```

`types.c`:
```c
static Type T_F32 = { TY_F32, 4, NULL, false, &TDEF_F32, NULL, 0, NULL };
Type* ty_f32(void) { return &T_F32; }
```

`desc_kind_of`:
```c
case TY_F32: return TD_F32;
```

`type_name`: `case TY_F32: return "f32";`.

`vm.c`'s `OP_BINARY`:
```c
case TD_F32: {
    float b, a;
    U32 tmp = POP(); memcpy(&b, &tmp, 4);
    tmp = POP();     memcpy(&a, &tmp, 4);
    ...
}
```

`vm.c`'s `OP_CONVERT`:
```c
case TD_F32: { ... }
```

`sema.c`'s `binary_result_type`:
```c
case BIN_ADD:
    if (is_float_type(t)) return t;   // covers f32 and f64
```

`codegen.c`'s `memkind_of`:
```c
case TY_F32: return MEM_F32;
```

`disasm.c`'s `print_desc`:
```c
case TD_F32: printf("f32"); break;
```

**Test.** Add a `t_float.c` case for f32 literals, arithmetic,
casts, and printing.

**Common mistakes.** Forgetting a case in `desc_kind_of` means the
type silently becomes `TD_I64` at runtime. Forgetting the
`OP_BINARY` case means arithmetic traps with `binary not
supported for descriptor kind N`.

---

## §9 — Adding an extern type kind

An extern type kind is a C-layout type. Prism has `struct`,
`opaque`, and `union`.

**Files to touch:** `lexer.h` and `parser.c` if new syntax,
`types.h` and `types.c` if a new `TypeKind`, `sema_types.c` for
layout, `codegen.c` for field access.

**Checklist:**

1. **`lexer.h`** — add a token if the syntax requires it.
2. **`parser.c`'s `extern_declaration`** — add the recognition
   branch.
3. **`ast.h`** — add flags to `struct_def` (`is_extern`,
   `is_opaque`, `is_union`) or a new field.
4. **`types.h`** — add a `TypeKind` or `TypeDefKind` if the new
   kind is structurally distinct.
5. **`sema_types.c`'s `resolve_extern_structs`** — add a layout
   branch for the new kind.
6. **`codegen.c`'s `AST_FIELD`** — add field-access behavior for
   the new kind.
7. **`vm.c`'s `vm_print_value`** — add rendering if the kind has
   a distinct runtime representation.

**Worked example: `opaque`.**

`parser.c`'s `extern_declaration`, after `extern type X =`:
```c
if (check(p, TOK_IDENT)
    && smap_span_equals(p->smap, p->cur.span, "opaque")) {
    advance(p);
    AstNode* def = ast_struct_def(name);
    def->as.struct_def.is_extern = true;
    def->as.struct_def.is_opaque = true;
    def->span = ...;
    return def;
}
```

`sema_types.c`'s `register_struct_or_enum`:
```c
td->is_opaque = is_opaque;
if (is_opaque) { /* skip layout */ }
```

`codegen.c`'s `AST_FIELD`: opaque types are rejected at sema, so
codegen never sees a field access on one.

**Test.** Add a `t_ffi.c` case using an opaque type in an extern
function's signature.

---

## §10 — Adding a `ConstKind`

A `ConstKind` is a compile-time value stored in the constant
pool. Currently `CONST_I64`, `CONST_F64`, `CONST_BOOL`,
`CONST_STRING`, `CONST_CSTRING`.

**Files to touch:** `constant.h`, `constant.c`, `vm.c`,
`codegen.c`, `disasm.c`.

**Checklist:**

1. **`constant.h`'s `ConstKind`** — add the new kind.
2. **`constant.h`'s `Constant` union** — add the corresponding
   arm if the new kind needs a distinct storage shape.
3. **`constant.c`** — add a `pool_add_*` function. Add a free
   branch in `pool_free` if the kind owns heap.
4. **`constant.c`'s `const_kind_name`** — add a case.
5. **`vm.c`'s `materialize_consts`** — add a materialization
   branch if the kind needs runtime setup.
6. **`vm.c`'s `OP_LOAD_CONST`** — add a push branch.
7. **`codegen.c`** — add emission at the sites where the new
   kind is produced (a literal constructor, a builtin, etc.).
8. **`disasm.c`** — add a rendering case in `dump_pool` and in
   the `LOAD_CONST` operand comment.

**Common mistakes.** Forgetting `materialize_consts` means the
kind's `const_ptrs` entry is never set; `OP_LOAD_CONST` then
pushes an uninitialized pointer.

---

## §11 — Extending the FFI's type set

An FFI type is one of the values in `FFIType`.

**Files to touch:** `prism_ffi.h`, `sema_ffi.c`, `vm.c`.

**Checklist:**

1. **`prism_ffi.h`'s `FFIType`** — add the new value.
2. **`sema_ffi.c`'s `ffi_kind_from_type`** — add a case mapping a
   Prism type to the new FFIType.
3. **`sema_ffi.c`'s `is_ffi_variadic_ok`** — decide whether the
   new type is allowed in variadic position; add a case if so.
4. **`vm.c`'s `ffi_type_of_kind`** — map the new FFIType to its
   `ffi_type*`.
5. **`vm.c`'s `ffi_call_dispatch`** — add a case for the new
   return type (narrowing, sign-extension, wrapping).

**Common mistakes.** Forgetting the `ffi_type_of_kind` case means
libffi receives `ffi_type_void`, which produces an ABI mismatch.

---

## §12 — Debugging a failing test

Not a code-change recipe; a procedure.

1. **Run the test in isolation.**
   ```
   ./tests.exe <filter>
   ```
   The filter is a substring of `group.case`. Running one test
   avoids noise from others.

2. **Look at the assertion message.** The test framework prints
   the file and line of the failure, the expression that failed,
   and the actual values (for `ASSERT_EQ_INT` and
   `ASSERT_EQ_STR`). For `assert_error`, it prints every
   diagnostic that was produced.

3. **Reproduce outside the framework.** If the test uses
   `test_value(src)`, write `src` to a file and run
   `manu run <file>`. The driver's output is richer than the
   test framework's.

4. **Look at the disassembly.** `manu --quiet=false run <file>`
   prints the bytecode listing.

5. **Look at the trace.** `manu --trace run <file>` enables the
   VM's per-instruction trace.

6. **Read the relevant invariant.** `arch/invariants.md` lists
   what must hold. The failing test's symptom (wrong value,
   wrong output, unexpected diagnostic) points at one or two
   invariants that could have been violated.

7. **Bisect the source.** If the test's source is complex,
   reduce it: remove statements, replace constructs with their
   equivalents, until the smallest failing program remains.
   Small programs are easier to reason about.

8. **Add temporary prints.** If the failure is subtle, add
   `fprintf(stderr, ...)` at candidate points in the compiler
   and rebuild. The compiler is one process; a rebuild is a few
   seconds.

9. **Check the recent changes.** If a test passed yesterday and
   fails today, `git log` on the files touched since then. The
   regression is usually in a recent commit.

For bug-specific methodology (how to find the cause, not just
which test failed), see `handbook/debugging.md`.