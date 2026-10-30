
## `docs/arch/invariants.md`
# Invariants — the enforcement map

*Current as of commit `<hash>`, <date>.*

An invariant is a property that must hold at all times. Violating
one is a bug. This document lists every invariant the compiler
and the VM rely on, and — for each — where it is enforced.

The list is longer than it looks. Some invariants are structural
(they hold because the data structures make violating them
impossible), some are checked (there is a runtime or compile-time
check), and some are by-convention (the code behaves as if they
hold and would silently break if they didn't).

The third category is where bugs live. Every time you add a
feature or fix a bug, ask: does this introduce a new invariant?
Where will it be enforced? Can it be made structural? If it must
be a convention, is the convention written down somewhere?

---

## §1 — The three ways an invariant is enforced

**Structural.** The invariant holds because the data structures
make violating it impossible. Example: `OP_ENTER_SCOPE` on a
separate scope stack means a scope's arena mark can never be
overwritten by a value push. To violate this, you'd have to add
code that pushes onto the scope stack from elsewhere.

**Checked.** The invariant is verified at the moment it is
consumed. Example: `OP_LOAD_LOCAL` checks `fp + slot < sp`
before reading. If the check fails, it traps.

**By-convention.** The invariant is neither structural nor checked;
both sides of the code behave as if it holds, and if it doesn't,
the result is undefined. Example: "every expression leaves exactly
one value on the stack." No instruction checks this; a codegen bug
that leaves two values produces a silently wrong program.

Structural enforcement is best. Checks are next-best but cost time
at every execution. Convention is the cheapest to implement and
the most fragile.

**The compiler's reliability is a function of how many of its
invariants are structural rather than conventional.** Prism
currently has a mix; this document names which is which.

---

## §2 — Memory invariants

**I1. `xmalloc` / `xcalloc` / `xrealloc` either succeed or abort.**
Enforced in `common.c`. Every call site may assume non-NULL on
return. The compiler has no OOM recovery path.

**I2. Every AST node is allocated via `alloc_node`, which
`xcalloc`s.** Enforced in `ast.c`. Constructors rely on this to
set only the fields they care about; the rest are zero.

**I3. Every `AstNode*` field in a payload is owned by its
containing node.** Enforced by convention in `ast_free`. Every
node is freed exactly once. The tree is acyclic by construction
(no constructor creates a back-reference).

**I4. Every `TypeExpr` is owned by the AST node or `TypeExpr` that
contains it.** Same mechanism as I3.

**I5. Every `Type*` returned by a `ty_*` function is either a
primitive singleton or a heap allocation that is never freed.**
Enforced by the absence of `free` on any `Type*`. This is a
deliberate leak, bounded by the compilation's lifetime.

**I6. `TypeDef` owns its `fields` array and each variant's
`payloads` array; the AST borrows them.** Enforced in
`typedef_table_free`, which frees them. The AST's `AST_ENUM_VARIANT`
node points at the same `payloads` array as the `TypeDef`; the
`ast_free` case for `AST_ENUM_VARIANT` deliberately does *not* free
it. This was a real bug at one point (see `handbook/changelog.md`)
and is now guarded by that comment.

**I7. `AstList` owns its items.** Enforced by `ast_list_free`,
which recurses into each item.

**I8. `ConstantPool` owns its string bytes.** Enforced by
`pool_free`, which frees each `CONST_STRING`/`CONST_CSTRING`'s
`bytes`. Non-string constants have no owned heap.

**I9. `Chunk` owns its code, lines, cols arrays, its pool, its
func table, and its type table.** Enforced by `chunk_free`, which
frees all five.

**I10. The `TypeTable` is append-only.** Enforced by convention:
`typetable_intern` never removes or reorders entries. Descriptor
indices remain valid for the table's lifetime.

---

## §3 — Span and source-map invariants

**I11. `file_id == 0` is reserved; no legitimate file gets that
id.** Enforced by `smap_add` (which returns ≥ 1) and `smap_get`
(which rejects 0). `SPAN_NONE` is `{0, 0, 0}` and is unambiguously
invalid.

**I12. Every `Span` in the pipeline has `end >= start`, and
`start`, `end` are within the file's length.** Enforced by
construction in `lexer.c` (the `span_from` helper) and `parser.c`.
Not checked at decode time. A malformed span would produce a
`column` past the line's end or a `span_len` that underflows to
a very large `uint32_t`.

**I13. `smap_decode` is the only function that maps a `Span` to a
line/column.** Enforced by convention. If you find yourself
computing `off - line_start + 1` outside `source_map.c`, the work
belongs there.

**I14. `smap_span_text` always NUL-terminates, even on truncation
or failure.** Enforced in `source_map.c`.

**I15. `smap_add` returns values ≥ 1.** Enforced by the
`ensure_slot_zero` mechanism, which reserves slot 0.

**I16. For any file with `L` lines, `line_starts` has `L` entries,
`line_starts[0] == 0`, and `line_starts[i] > line_starts[i-1]`.**
Enforced by construction in `smap_add`, which counts newlines in
one pass and fills the array in another.

**I17. The `path` pointer in a `SourceFile` is borrowed.** Enforced
by `smap_free`, which does not free it. The caller must keep the
path alive for the map's lifetime.

---

## §4 — Type-system invariants

**I18. Primitive types are singletons and compare by pointer;
compound types compare by `type_eq`.** Enforced by construction.
`type_eq` starts with `if (a == b) return true;`, which catches
the singleton case; for compound types it recurses structurally.

**I19. `type_eq` is reflexive, symmetric, and transitive.**
Enforced by the structure of the function. A future change that
broke this would break the instantiation cache (`cache_lookup`
uses `type_eq` for argument comparison) and the `desc_matches`
function in `typetable_intern`.

**I20. Every `TypeDef` has a unique `(name, is_instantiated)`
combination within a compilation.** Enforced by the duplicate
check in `register_struct_or_enum`. The `is_instantiated` flag
is what lets a user-defined `Option` (in principle) coexist with
the prelude's `Option#i64` instantiation.

**I21. `is_instantiated == true` implies the `TypeDef` is a
monomorphized copy.** Enforced by sema's instantiation path,
which is the only writer of the flag.

**I22. `typedef_table_find` skips instantiated entries;
`typedef_table_find_in_file` does not.** Enforced in `types.c`.
The asymmetry is deliberate: unqualified name lookup must not
find `Option#i64` when the user writes `Option`; but resolving
a name *within* the template's own file (which is what `_in_file`
is for) must be able to find the mangled instantiation.

**I23. `TypeDef.is_rc_managed == true` implies that every use
site of a value of that type is wrapped in `ty_rc`.** Enforced
partly in `sema_instantiate_generic` and partly at the various
resolution sites. **Fragile:** the wrapping happens at each
call site that resolves a `TEX_NAME` or `TEX_GENERIC` to a
`TypeDef`. There are three sites that need to unwrap one level
of `TY_RC` when inspecting a resolved type; forgetting one
would silently produce a plain `ty_enum` where an `rc<enum>` is
expected. See §10.

**I24. `TypeExpr` fields not meaningful for a kind are zeroed.**
Enforced by `xcalloc` in the constructors. Readers dispatch on
`kind` before touching any field.

**I25. `type_expr_resolve` returns the same `Type*` for the
same resolved structure.** Not enforced — compound types are
allocated fresh on each call. Two resolves of `[i64]` produce
two distinct `Type*` with equal structure. `type_eq` handles the
comparison; identity comparison is only valid for primitives.

---

## §5 — Sema invariants

**I26. Every `Local`'s `slot` equals its index in `FnLocal.locals`
at the time of `declare`.** Enforced by `declare`. `exit_scope`
reduces `local_count` but leaves the array intact; the next
`declare` overwrites.

**I27. `FnLocal.local_count` after `exit_scope` equals its value
before the corresponding `enter_scope`.** Enforced by the
`scope_first[]` stack.

**I28. Every `AST_VAR` after sema has `local_index >= 0` XOR
`const_id >= 0`, unless a diagnostic was reported.** Enforced by
`analyze_expr`'s `AST_VAR` case.

**I29. Every `FuncSig.codegen_idx >= 0` corresponds to a function
codegen will emit.** Enforced by sema's Pass A, which sets
`codegen_idx` for concrete user functions, and by codegen's
Phase 1 loop, which assigns codegen indices by iterating the same
`sc->funcs[]` and skipping the same functions (externs and
templates). The two enumerations must stay in sync — this is
by-convention. If a future change adds a new kind of non-emitted
function, both loops must skip it identically.

**I30. Nested `analyze_func_body` calls do not disturb each
other's per-function state.** Enforced structurally by the
`SemaCtx` / `FnLocal` split. The per-compilation state (function
table, type table, constant table, module table) is shared and
mutated; the per-function state (locals, scopes, current
signature, loop depth) is local to each call.

**I31. Every generic instantiation is cached by
`(template, args)`; two calls with the same arguments produce
the same `FuncSig` and `TypeDef`.** Enforced by the instantiation
cache in `sema_mono.c`. Cache overflow (`MAX_INST_CACHE = 256`)
silently disables further caching, which can produce duplicate
`TypeDef`s with the same mangled name. Nominal comparison via
`tdef` pointer would then treat them as unequal.

**I32. Every `AST_RETURN` in an annotated function returns a
value compatible with the annotation.** Enforced by
`analyze_stmt`'s `AST_RETURN` case, which calls `coerce_to_target`.

**I33. Every `AST_LET` binds a value of the variable's declared
or inferred type.** Enforced by `analyze_stmt`'s `AST_LET` case,
also via `coerce_to_target`. Note: `coerce_to_target` may insert
an `AST_CAST` node, which is a mutation of the AST during sema.

**I34. Every function `FuncSig.return_type` is non-NULL after
Pass B.** Enforced by `analyze_func_body`'s tail: if the return
type was not annotated and no value-return established it, it
is set to `ty_void()`.

**I35. Every `AST_MATCH` is exhaustive at the top level.**
Enforced by `check_exhaustive` in `sema.c`. Subpatterns are not
checked for exhaustiveness; a match with a wildcard arm at the
top level can still fail at runtime on a nested variant pattern
that doesn't match any arm's subpattern. The `OP_TRAP` at the
end of every match handles this case.

---

## §6 — Codegen invariants

**I36. Every expression leaves exactly one value on the stack.**
Enforced by convention. The most load-bearing invariant in the
compiler. Every `cg_expr` case must uphold it; violating it
produces stack corruption without a diagnostic. Void expressions
leave `PUSH 0`.

**I37. `cg->rc_scope_count == cg->scope_depth` at every statement
boundary.** Enforced by convention. Every construct that emits
`OP_ENTER_SCOPE` must call `cg_push_rc_scope`; every construct
that emits `OP_EXIT_SCOPE` must call `cg_emit_scope_drops` and
`cg->rc_scope_count--`. The two are paired. A mismatch produces
either a leak (drops emitted for the wrong scope) or a
double-free (drops emitted twice for the same scope).

**I38. Every `FuncEntry.entry_pc` points at the first word of the
function's body.** Enforced by `chunk_add_func` being called
before the body is emitted.

**I39. Every forward `CALL`'s placeholder is patched before the
chunk is handed to the VM.** Enforced by construction: the
prologue's `CALL main` is patched by the loop that emits bodies,
which runs before `codegen_program` returns. If a placeholder
were never patched, the target would be 0, which is `CALL 0 0`
— an infinite loop.

**I40. Every jump target is within the emitted code.** Enforced
by construction (patch targets are always the current end of the
chunk) and validated by `validate_bytecode`.

**I41. Every `desc_of(t)` produces a descriptor consistent with
the operand's actual type.** Enforced by sema setting `node->type`
correctly. **Fragile:** `desc_of(NULL)` silently produces a
`TD_I64` descriptor and emits a warning. If sema failed to set
a type, codegen would produce a descriptor that doesn't match
the runtime value.

**I42. The RC drop is emitted on every path that exits a scope
with RC locals.** Enforced by the systematic placement of
`cg_emit_scope_drops` (block exit), `cg_emit_all_rc_drops`
(return, try-failure), and `cg_emit_n_scope_drops` (break,
continue). **Fragile:** a new statement kind that can exit a
scope without going through one of these paths must add its own
drops. The `AST_FOR_IN` case was missing its rc-scope push at
one point; see `handbook/changelog.md`.

**I43. Every value whose sema type is `rc<T>` is allocated by an
`OP_NEW_RC_*` opcode.** Enforced by convention at each allocation
site. **Fragile:** the `AST_FIELD` unit-variant path was using
`OP_NEW_ENUM` for RC-managed enums at one point, which put the
block in the arena while the scope-exit drop treated it as RC
heap memory. The bug surfaced as heap corruption on Windows. See
`handbook/debugging.md` for the worked case.

---

## §7 — VM invariants

**I44. `sp <= DATA_STACK_MAX` before every push.** Checked on
most pushes; a few paths (like the `OP_STORE_LOCAL` extension
loop) rely on the compiler to emit balanced code.

**I45. `fp + slot < sp` before every `LOAD_LOCAL`.** Checked.
`OP_STORE_LOCAL` extends the stack if needed.

**I46. `ap <= arena + ARENA_MAX` before every allocation.**
Checked in `arena_alloc`.

**I47. `amp` is always a valid prior `ap`.** Enforced by
construction. Only `OP_ENTER_SCOPE`, `OP_CALL`, and `OP_RET`
set `amp`, and each sets it to a value that was previously read
from `ap` or saved on the stack.

**I48. Every call frame's `return_pc` is the address just after
the `CALL` instruction.** Enforced by `OP_CALL`. `return_pc =
vm->pc` after the two operand reads.

**I49. Every `OP_RET` corresponds to a prior `OP_CALL`, and
`call_depth` never goes negative.** Checked. `OP_RET` at depth 0
is a runtime error.

**I50. Every `LOAD_CONST idx` has a valid materialized pointer
for `idx`.** Enforced by `materialize_consts` at VM init.
**Fragile:** a new `ConstKind` requires a branch in
`materialize_consts` and a branch in `OP_LOAD_CONST`'s dispatch.
Forgetting one produces a runtime error (`unknown constant kind`).

**I51. `const_arena_size` is at least the total size computed
in `materialize_consts`.** Enforced by `materialize_consts`
itself, which aborts if the total would exceed `CONST_ARENA_MAX`.
**Fragile:** a very large program with many string literals
would abort at VM init, not at compile time. The compiler could
compute the total in `codegen.c` and reject at compile time.
Currently it does not.

**I52. `RcHeader.ref_count == 0xFFFFFFFF` implies the block is
in the const arena.** Enforced by `materialize_consts`, which
sets the sentinel on all const-arena string headers. `RC_INC`
and `RC_DEC` check for it and short-circuit.

**I53. Every RC block's header is at `payload - 8`.** Enforced
by construction: `OP_ALLOC_RC`, `OP_NEW_RC_ARRAY`, and
`OP_NEW_RC_ENUM` all `malloc` a block of `sizeof(RcHeader) +
payload_size` and push `(h + 1)`. Any code that reads a header
uses `((RcHeader*)payload) - 1`.

---

## §8 — FFI invariants

**I54. Every `FFIFunc.fn_ptr` is non-NULL before `ffi_call`.**
Enforced by `ffi_resolve` returning a status and the caller
(`ffi_call_dispatch`) checking it. **Was broken** at one point;
`RT_ERR` inside a `void` helper silently discarded the error
return. The fix made `ffi_resolve` return `int` and propagate.

**I55. `FFIFunc.ffi_arg_types[]` persists for the lifetime of the
`cif`.** Enforced by making `ffi_arg_types` a field of `FFIFunc`
rather than a stack local. libffi stores a *pointer* to the
array, not a copy.

**I56. `Option<*T>` marshals to NULL for None and to the payload
pointer for Some.** Enforced by the unwrap code in
`ffi_call_dispatch` and the wrap code in the `returns_option_ptr`
branch. **Fragile:** the recognizer `is_option_ptr_type` in
`sema_ffi.c` uses a string prefix match against the mangled name
(`"Option#"`). A user-defined enum whose mangled name happens to
start with `Option#` would be misrecognized.

**I57. Every variadic call passes at least the fixed argument
count.** Checked in sema's `AST_CALL` extern path. The VM
re-prepares the cif when the argument count changes between
calls to the same variadic function.

**I58. Every `Option<*T>` value handed to or returned from the
FFI is RC-managed.** Enforced by the type system: `Option<T>` is
declared in the prelude with `is_builtin = true`, so its
`TypeDef.is_rc_managed` is `true`, and every use site is wrapped
in `ty_rc`. The FFI marshalling code assumes this; it would
misbehave on a plain (non-RC) enum whose name happened to match.

---

## §9 — The single sources of truth

Some invariants are enforced structurally by naming a single
authority. When the code has a question, it asks that authority.

- **`SourceMap`** — the only authority on what a `Span` means.
  `smap_decode` is the only function that computes line/column;
  `smap_span_text` is the only function that reads bytes out of a
  span.
- **`TypeDef`** — the only authority on a nominal type's structure.
  Field offsets, variant tags, and `is_rc_managed` all live here.
  A `Type*` with `kind == TY_STRUCT` or `TY_ENUM` carries a
  pointer to its `TypeDef`; there is no other source for the
  structure.
- **`FuncSig.codegen_idx`** — the only authority on a function's
  codegen index. Every `CALL` operand is either a direct codegen
  index (from a `FuncSig`) or a computed one (from
  `codegen_of_sema[sema_index]`, which reads the `FuncSig`).
- **`RcHeader`** — the only authority on an RC block's refcount.
  No other structure caches the count.
- **The prelude** — the only authority on `Result` and `Option`'s
  structure. The compiler special-cases them by name and by
  structure in several places (`?` desugaring, FFI marshalling),
  but the underlying types come from the prelude source.

---

## §10 — Invariant drift

An invariant drifts when the code that enforces it and the code
that assumes it fall out of sync. Two drift patterns have been
observed in this project.

### §10.1 The two-table problem

Before the `SemaCtx` split, the compiler had a single `FnCtx`
holding both per-compilation and per-function state. Nested
instantiation clobbered the outer function's locals. The fix
introduced `SemaCtx` (per-compilation, stable across bodies) and
`FnLocal` (per-function, created and discarded by
`analyze_func_body`).

The general lesson: when an invariant becomes non-structural,
the fix is usually to introduce a single source of truth, not to
add a check. Checks catch violations; a single source of truth
makes violations impossible.

### §10.2 The three-use-site unwrapping problem

Because `Result` and `Option` are declared with
`is_rc_managed = true`, every place that resolves their types at
a use site gets back a `ty_rc(...)`. Three code paths need the
*inner* enum: pattern matching (`analyze_pattern`), variant
construction (`analyze_expr`'s `AST_VARIANT_CTOR`), and struct
literals that expect an RC-managed enum (`AST_STRUCT_LIT`).
Each of those unwraps one level of `TY_RC` before inspecting.

**This invariant is fragile.** A fourth code path that needs the
same unwrap would be easy to forget. The structural fix would be
to make `is_rc_managed` transparent at the type-system level:
the resolver returns `ty_rc(T)` but the type rules treat it as
`T` for the purposes of pattern matching and field access. This
is currently under consideration but not implemented. See
`handbook/decisions.md` for the reasoning.

### §10.3 The RC-scope invariant

The RC-scope tracking (I37) is by-convention. Every construct
that emits `OP_ENTER_SCOPE` must push an `RcScope`; every
`OP_EXIT_SCOPE` must pop one. The two are paired by hand in
`cg_block`, `AST_FOR_RANGE`, `AST_FOR_IN`, and `AST_MATCH`.

At one point, `AST_FOR_IN` was missing its push. Because the
`RcScope` stack was not initialized for that scope, RC locals
declared directly inside the loop body were registered in the
*enclosing* scope. At loop exit, the drop landed on a slot that
had already been reused. The bug was silent until a targeted
test (`t_rc.c`'s `for_in_body_rc`) exercised the case.

**The invariant is by-convention.** A structural fix would be
to bundle the `ENTER_SCOPE`-and-push and `EXIT_SCOPE`-and-pop
into single functions. That's a small refactor and might be
worth doing before more statement kinds are added.

---

## §11 — The three invariants most worth your attention

If you remember three invariants from this document, remember
these:

**I36 — one value per expression.** The deepest assumption in
codegen. Every case in `cg_expr` and `cg_stmt` assumes it. If
you add an opcode or an AST kind, verify it.

**I29 — codegen/sema index agreement.** The `codegen_of_sema`
mapping bridges sema's function table and codegen's. Any new
AST node that references a function must use a codegen index,
not a sema index. The compiler has one convention for how to
get it (`FuncSig.codegen_idx`), and every call site uses it.

**I23 — `is_rc_managed` implies `ty_rc` wrapping.** Every use
site of an RC-managed enum's type gets the RC wrapper. Forgetting
to unwrap it in a new code path produces wrong behavior. This
is the invariant most likely to be silently broken by a new
feature.

---

## §12 — The enforcement table

For quick reference, the enforcement type of each invariant:

| Invariant | Enforcement |
|---|---|
| I1–I10 | Structural (allocation / ownership by construction) |
| I11–I17 | Mixed structural (I11, I15, I16) and by-convention (I12, I13, I14) |
| I18–I25 | Structural (I18, I19, I24) and by-convention (I20–I23, I25) |
| I26–I35 | Structural (I26, I27, I30) and by-convention (I28, I29, I31–I35) |
| I36–I43 | By-convention, except I38, I39, I40 (structural) |
| I44–I53 | Checked (I44–I46, I48–I50) and structural (I47, I51–I53) |
| I54–I58 | Checked and structural, with fragile recognizers (I56, I58) |

The table makes the shape of the project's reliability visible:
the front end and the runtime are structurally sound; the middle
(sema and codegen) has the most convention-only invariants.

This is also where bugs are most likely to land. The five bugs
fixed in the recent session (§10.1–§10.3 and the ones in
`handbook/changelog.md`) were all by-convention violations.
