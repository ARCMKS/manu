## `docs/handbook/changelog.md`

# Changelog

*Current as of commit `<hash>`, <date>.*

Reverse-chronological. Every meaningful change to the compiler,
VM, language, or tooling gets an entry. Bug fixes and features
go here; documentation-only changes do not.

Format: `### <date>` followed by bullet entries. Each entry is
one line, present tense, naming the subsystem.

The list is not exhaustive of every commit. It is a record of
what a future maintainer would want to know about.

---

### `<date>` — Documentation set rewrite

- **Docs:** replaced the pre-existing doc set
  (`base.md`, `README.md`, `REFERENCE.md`, `e2e.md`, `ffi.md`,
  `handbook.md`, `language.md`) with the current `docs/` tree.
  The old files are in `docs/_old/`.
- **Docs:** new set is organized by audience (language users,
  compiler contributors, maintainers) with separate trees under
  `docs/language/`, `docs/arch/`, and `docs/handbook/`.

### `<date>` — Test suite refactor

- **Tests:** consolidated from ~330 tests across 28 files to
  ~93 tests across 10 files. Prior test suite had many
  duplicates and stale assertions; the refactor kept
  diagnostics-needle tests, boundary tests, and integration
  tests, and dropped smoke tests that were redundant with
  other coverage.
- **Tests:** added `test_value`, `test_output`, `assert_error`,
  and `assert_any_error` helpers to `tests/test.c` and
  `tests/test.h`. These gate on the compiler's error count
  and cannot pass for the wrong reason.
- **Tests:** added `t_diagnostics.c`, one test per
  compiler-emittable diagnostic message.
- **Tests:** fixed `test_run` to return 0 on a runtime error
  rather than reading `vm_top`, which was returning the last
  argument of the failing instruction.

### `<date>` — RC scope tracking in codegen

- **Codegen:** added an `RcScope` stack parallel to
  `cg->scope_depth`. Every scope-introducing construct
  (`cg_block`, `AST_FOR_RANGE`, `AST_FOR_IN`, `AST_MATCH`)
  registers its RC locals; scope exit and early exits
  (`AST_RETURN`, `AST_BREAK`, `AST_CONTINUE`, `AST_TRY`
  failure) emit drops for the appropriate scopes.
- **Codegen:** added `cg_push_rc_scope`,
  `cg_register_rc_local`, `cg_emit_scope_drops`,
  `cg_emit_all_rc_drops`, `cg_emit_n_scope_drops`.
- **Bug fix:** `AST_FOR_IN` was missing its rc-scope pushes;
  RC locals declared in the loop body were registered in the
  enclosing scope and dropped on reused slots. Fixed and
  covered by `t_memory.c`'s `for_in_body_rc`.

### `<date>` — Five correctness fixes

- **Sema:** `AST_VARIANT_CTOR`'s enum-construction path used
  `rd1` (a `rd_none()` sentinel) instead of `rd` (the
  `resolve_name` result). Every non-generic enum payload
  constructor was rejected as "unknown type or module".
  Fixed by using `rd`.
- **VM:** `OP_DUP2` read `data_stack[sp-2]` twice instead of
  `sp-2` and `sp-1`. `arr[i] += n` produced garbage because
  the subsequent `INDEX_LOAD` read `arr[arr[i]]`. Fixed by
  reading `sp-1` on the second read.
- **Codegen:** `AST_ASSIGN_OP` for `AST_FIELD` evaluated the
  RHS twice and dropped the wrong reference. Restructured to
  match the `AST_VAR` case.
- **Sema:** `AST_LET` called `coerce_to_target` twice; a
  widening that needed a runtime conversion emitted a doubled
  `OP_CONVERT`. Fixed by deleting the redundant call.
- **Codegen:** `AST_FIELD` for a unit variant always emitted
  `OP_NEW_ENUM`, even for RC-managed enums (`Option.None`).
  The scope-exit drop then treated the arena pointer as an RC
  payload and corrupted the heap on Windows. Fixed by
  selecting `OP_NEW_RC_ENUM` when `n->type->kind == TY_RC`.

### `<date>` — FFI fixes

- **Sema:** `is_option_ptr_type` correctly recognizes
  `Option<*T>` for both argument and return positions.
- **VM:** `ffi_resolve` returns `int` and propagates errors
  via `RT_ERR`; earlier versions called `RT_ERR` from a
  `void` function, discarding the error and leaving
  `fn_ptr` NULL.
- **VM:** `ffi_arg_types[]` moved from a stack-local to a
  field of `FFIFunc` because libffi stores a pointer to it,
  not a copy.
- **VM:** added a fallback `GetProcAddress` with `_<symbol>`
  for 32-bit Windows cdecl symbol decoration.
- **Tests:** variadic FFI tests are guarded with
  `#ifndef _WIN32`; Windows `GetProcAddress` does not
  reliably resolve variadic CRT symbols from
  `ucrtbase.dll`.

### `<date>` — `?` operator and `Result`/`Option` fixes

- **Sema:** `AST_TRY`'s desugaring correctly unwraps the
  `rc<Enum>` from the operand's type.
- **VM:** `OP_TRY_JUMP_IF_OK` inline-drops the enum block on
  the success path (transferring the payload to the caller
  without an RC_INC, since the payload's refcount is
  independent).

### `<date>` — `let mut` and immutability

- **Lexer:** `mut` token.
- **Parser:** `let mut` syntax, `mut` on parameters, `mut`
  in `ref<mut T>`.
- **Sema:** immutability enforcement. A `let` binding without
  `mut` cannot be reassigned; a parameter without `mut`
  cannot be reassigned; a `ref<T>` (immutable reference)
  cannot be written through.
- **AST:** `Local.is_mut`, `Param.is_mut`, `AstNode.let.is_mut`.

### `<date>` — Methods

- **Parser:** `fn Type.method(receiver, ...)` syntax.
- **AST:** `AstNode.func.method_owner` and the
  `AST_VARIANT_CTOR.receiver_expr` field.
- **Sema:** method resolution. One level of `rc<T>`/`ref<T>`
  unwrapping for the receiver.
- **Codegen:** the receiver is passed as argument 0 of the
  underlying free function.

### `<date>` — `sizeof`, `try`, `BIN_AND`/`BIN_OR`

- **Lexer:** `?` token (`TOK_QUESTION`).
- **Parser:** postfix `?` (produces `AST_TRY`), `sizeof(T)`
  (soft keyword), `&&`/`||` operators.
- **AST:** `AST_TRY`, `AST_SIZEOF`, `BIN_AND`, `BIN_OR`.
- **Sema:** `?` typing (requires annotated return type),
  `sizeof` typing (returns `u64`).
- **Codegen:** `OP_TRY_JUMP_IF_OK` emission for `?`;
  short-circuit emission for `&&`/`||`; `PUSH size` for
  `sizeof`.
- **VM:** `OP_TRY_JUMP_IF_OK`.

### `<date>` — `extern type` (struct, opaque, union)

- **Parser:** `extern type X = struct { ... }`,
  `extern type X = union { ... }`, `extern type X = opaque`.
- **AST:** `AstNode.struct_def.is_extern`, `is_opaque`,
  `is_union`.
- **Types:** `TY_EXTERN_STRUCT`, `TDEF_EXTERN_STRUCT`.
- **Sema:** C-ABI layout fixpoint (`resolve_extern_structs`),
  field validation, nested-by-value extern structs.
- **Codegen:** `AST_FIELD` for extern struct fields (leaf
  widths via `memkind_of`; nested structs push their address).
- **VM:** `NATIVE_MEMCPY` for nested extern struct copies,
  `NATIVE_ARENA_ZERO` for extern struct literal allocation.

### `<date>` — FFI

- **Sema:** `sema_ffi.c` with `build_library_table`,
  `library_table_find`, `ffi_kind_from_type`,
  `is_option_ptr_type`, `is_ffi_variadic_ok`.
- **AST:** `AST_EXTERN_LIB`, `AstNode.func.is_extern`,
  `lib_alias`, `is_variadic`, `ffi_idx`.
- **Codegen:** `OP_FFI_CALL` emission.
- **VM:** `ffi_resolve` (library loading, symbol resolution,
  cif preparation) and `ffi_call_dispatch` (argument popping,
  `Option<*T>` marshalling, result wrapping).
- **Header:** `prism_ffi.h` with `FFILib`, `FFIFunc`, `FFITable`.
- **Builtins:** `ffi_cstring`, `ffi_string`, `ffi_error`,
  `ffi_errno`, `ffi_alloc`, `ffi_free`, `raw_null`,
  `raw_is_null`, `raw_offset`, `raw_load_u8`, `raw_store_u8`.

### `<date>` — `Result`, `Option`, and the prelude

- **Prelude:** `prelude.c` with `Result<T, E>` and `Option<T>`
  declared in Prism, marked `is_builtin = true`.
- **Types:** `TypeDef.is_rc_managed` propagated from
  `is_builtin`.
- **Sema:** every use site of an RC-managed enum wraps the
  type in `ty_rc(...)`.
- **Codegen:** variant constructions and unit variants of
  RC-managed enums emit `OP_NEW_RC_ENUM` (with a fix at one
  point for `AST_FIELD`'s unit-variant case).

### `<date>` — Multi-file compilation

- **Module loader:** `module.c` with `modload_load`,
  `modload_merge`, cycle detection, canonical path dedup,
  longest-prefix resolution.
- **Sema:** `sema_modules.c` with `build_module_table` and
  `validate_selective_imports`.
- **Driver:** `compile_multi` in `main.c` shared by `run`
  and `check`.

### `<date>` — Generics (monomorphization)

- **Monomorph:** `monomorph.c` with `TemplateList`,
  `GenericTemplate`, `Subst`, `mono_copy_node`,
  `mono_copy_type_expr`, `mono_mangle`.
- **Sema:** `sema_mono.c` with `sema_instantiate_generic`,
  `sema_instantiate_function`, the instantiation cache.
- **Types:** `TypeExprKind` grew `TEX_RESOLVED`, `TEX_FN`.

### `<date>` — The VM

- **VM:** `vm.c` dispatch loop, arena, RC heap, sinks,
  parameterized opcodes, FFI path.
- **Chunk:** `chunk.c` bytecode container with parallel
  code/lines/cols arrays and function table.

### `<date>` — Foundation

- **Common:** `common.h`/`common.c` with type aliases,
  `Span`, allocators.
- **Diagnostics:** `diagnostics.h`/`diagnostic.c` with
  `DiagnosticBag` and stderr renderer.
- **Source map:** `source_map.c` with file registry and
  span decoder.
- **Lexer:** `lexer.c` with ASI.
- **AST:** `ast.h`/`ast.c` with the node tree.
- **Parser:** `parser.c` with the Pratt chain.
- **Types:** `types.c` with `Type`, `TypeExpr`, `TypeDef`,
  `TypeDesc`, `type_expr_resolve`, `typetable_intern`.

---

## How to add an entry

After a meaningful change lands:

1. Add a bullet under the current date's section (or create a
   new section if none exists for today).
2. Name the subsystem (`Sema:`, `Codegen:`, `VM:`, `Tests:`,
   `Docs:`).
3. One line per change. Bug fixes name the symptom briefly
   ("`AST_FOR_IN` was missing its rc-scope pushes").
4. Feature additions name the new surface ("`let mut`
   syntax and immutability enforcement").
5. If the change is large enough to warrant a document
   reference, add a parenthetical (`see decisions.md §D16`).

Do not list every commit. List what a future maintainer would
want to know about.
