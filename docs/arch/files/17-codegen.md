## `docs/arch/files/17-codegen.md`

# `include/codegen.h`, `src/codegen.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

Codegen walks the sema-annotated AST and emits bytecode into a
`Chunk`. It is the last compiler-side stage; downstream is only
the validator, the disassembler, and the VM.

**What it solves.** Translating every `AstKind` into a sequence
of opcodes; inlining constants; inserting RC drops; laying out
function bodies; patching forward calls; recording debug
locations.

**What it does not solve.** Type checking (sema did that). Name
resolution (sema did that). Slot assignment (sema did that). RC
ownership classification (sema's `holds_rc` flag). Codegen reads
the resolved AST; it does not re-resolve anything.

**Who depends on it.** `main.c` and the test harness call
`codegen_program`. The validator and the VM consume its output.

**The one rule.** Every `cg_expr` call leaves **exactly one
value** on the data stack. Void expressions leave `PUSH 0`. This
invariant is what makes statement codegen uniform and what every
case in the file upholds. It is I36 in `invariants.md`.

---

## §2 — Public interface

```c
void codegen_program(AstNode* program, const SourceMap* smap,
                     Chunk* chunk, DiagnosticBag* bag,
                     const ConstTable* consts,
                     const TypeDefTable* structs,
                     const FFITable* ffi,
                     const SemaCtx* sc);
```

**Preconditions.** The program has passed sema; every node's
`type` is set; every resolution field is populated. `chunk` is
`chunk_init`'d and empty (or will be appended to). `sc` is the
same `SemaCtx` sema produced.

**Postconditions.** On success, `chunk` holds the program: a
prologue, one entry per non-extern, non-template function, and
the fallback exits. On failure, partial bytecode may exist; the
caller discards the chunk if the bag has errors.

**The `program` argument is unused.** Codegen walks
`sc->funcs[]` instead. The parameter exists for ABI stability and
because a caller might expect it to be used; the first line of
the function is `(void)program;`.

**`ffi` is also unused.** Sema already populated the table; the
extern call sites carry an `ffi_idx` operand.

---

## §3 — The `Cg` struct

```c
typedef struct {
    Chunk*            chunk;
    DiagnosticBag*    bag;
    const SourceMap*  smap;
    const ConstTable* consts;
    const TypeDefTable* structs;
    uint32_t          current_line;
    uint32_t          current_column;

    int               scope_depth;
    LoopFrame         loops[MAX_LOOP_DEPTH];
    int               loop_count;

    const SemaCtx* sema;
    int codegen_of_sema[MAX_FUNCS];

    int in_string_build;

    RcScope rc_scopes[MAX_SCOPES];
    int     rc_scope_count;
} Cg;
```

**`chunk`** — the target. Every emission goes through
`emit`/`chunk_emit`/`chunk_add_func` on this pointer.

**`bag`** — for backend-limit diagnostics (nesting caps,
over-long print format chunks) and for `internal:` errors.

**`smap`** — for `set_span` (which decodes an AST node's span to
line/col) and for identifier comparison.

**`current_line`/`current_column`** — the debug location for the
next emitted word. Updated by `set_span` before each logical
instruction. This is a global side effect; the next `emit` uses
whatever `set_span` last wrote.

**`scope_depth`** — count of active lexical scopes. Incremented
by `cg_block` and by the explicit `OP_ENTER_SCOPE` emissions in
`AST_FOR_RANGE`/`AST_FOR_IN`/`AST_MATCH`. Consulted by
`AST_BREAK`/`AST_CONTINUE` to compute unwind counts.

**`loops[]`/`loop_count`** — the loop stack. Each `LoopFrame`
records the scope depths at which `break` and `continue` should
land, and the patch slots for both. `MAX_LOOP_DEPTH = 32`.

**`codegen_of_sema[]`** — maps a sema function index to a codegen
function index. See §11.

**`rc_scopes[]`/`rc_scope_count`** — parallel stack to
`scope_depth`, tracking RC-owning locals per scope. See §10.

**`in_string_build`** — a refcount incremented around
`BEGIN_STRING`/`END_STRING` emission. Currently never read.

---

## §4 — Emission primitives

```c
static void set_span(Cg* cg, const AstNode* n);
static void emit(Cg* cg, U64 w);
static int  emit_placeholder(Cg* cg, OpCode op);
static void patch_to_here(Cg* cg, int slot);
```

**`set_span`** decodes `n->span` via `smap_decode` and writes
line/col into `cg->current_line`/`current_column`. On decode
failure (`SPAN_NONE`, unknown file_id), writes `0`/`0`, which
downstream code treats as "no location".

**`emit`** is a one-line wrapper over `chunk_emit` that uses the
current line/col. Every emitted word (opcode or operand) gets the
same location.

**`emit_placeholder`** emits an opcode followed by a
zero-valued operand word, and returns the operand word's index.
The caller records the index and patches it later.

**`patch_to_here`** overwrites a previously-emitted placeholder
with the current `chunk->count` (i.e., the address of the next
instruction to be emitted).

**Forward-call patching** uses this pair: `emit_placeholder(OP_JMP)`
returns a slot; the code the jump should land on is emitted; then
`patch_to_here(slot)` fills the slot with the landing address.

**No jump is ever patched across a function boundary.** The
prologue's `CALL main 0` is patched by the loop that emits bodies
(§11).

---

## §5 — Type and memory helpers

```c
static uint32_t desc_of(Cg* cg, Type* t);
static MemKind  memkind_of(Type* t);
static U32      op_binary_code(BinOp op);
```

**`desc_of`** interns `t` into the chunk's `TypeTable` and returns
the descriptor index. That index is the operand of every
parameterized opcode. `desc_of(NULL)` is a codegen bug; the
current implementation emits a warning and interns NULL (which
maps to `TD_I64`). The warning surfaces a missing `type` field
without aborting.

**`memkind_of`** maps a `Type*` to the memory access width used
by `OP_LOAD_INDIRECT`/`OP_STORE_INDIRECT`/`OP_LOAD_ADDR_INDIRECT`.
`MEM_I8`/`MEM_I16`/`MEM_I32`/`MEM_I64` and their unsigned
counterparts for integers; `MEM_F32`/`MEM_F64` for floats;
`MEM_PTR` for pointers; `MEM_U64` for everything else.

**`op_binary_code`** maps an AST `BinOp` to a VM `OpBinary`.
Every case is a straight switch. `BIN_AND` maps to `OPB_ADD`
(the `&&` operator is desugared into control flow, so its
`OPB_ADD` value is never emitted); `BIN_OR` maps to `OPB_OR`.

---

## §6 — Print codegen

Print is the most complex builtin because it has three
compile-time-selected modes.

**Mode selection.** `cg_builtin_print` is called from `AST_CALL`'s
builtin dispatch (`builtin_id == 0`). It selects a mode by
scanning the first argument:

- Zero arguments → empty mode (just a newline).
- First argument is an `AST_STRING` with at least one bare `{}` →
  format mode.
- Otherwise → multi-argument mode (space-separated values).

`string_has_placeholder` does the scan. It walks the raw source
of the string literal between the quotes, skipping `{{` and `}}`
(two-byte skips), and returns true at the first bare `{}`.

**Common structure.** All three modes emit:

```
BEGIN_STRING                 (NATIVE_STR_BEGIN)
  ...render body...          (literal chunks and/or value prints)
  newline                    (cg_emit_newline)
END_STRING                   (NATIVE_STR_END)
CCALL stdout_write           (NATIVE_STDOUT_WRITE)
PUSH 0                       (void result)
```

**Format mode** (`cg_print_format_body`) walks the raw source
between the quotes. Literal chunks are accumulated and emitted via
`cg_emit_literal_chunk` (which handles `\n`, `\t`, `\r`, `\\`,
`\"`, `{{`, `}}`). Each placeholder consumes one argument from
`args[1..]`. Placeholder/argument count mismatch is reported and
emission stops. The literal chunk buffer is 1024 bytes; longer
chunks are silently truncated.

**Multi-arg mode** emits each argument separated by a single
space.

**Value rendering.** `cg_print_value` emits a value's
`TypeDesc` index (as an `OP_PUSH` operand) followed by
`CCALL NATIVE_PRINT_VALUE`. The VM's `native_print_value`
receives the value on the stack and the descriptor index, and
uses the descriptor to select the renderer. Auto-unwrapping: an
`rc<T>` where `T` is a scalar emits an `OP_LOAD_INDIRECT` first
(the value is stored in the RC block's payload); a compound
`rc<T>` does not (the handle already points at the compound
payload).

**`format` builtin.** `cg_builtin_format` emits the same body but
omits the `CCALL stdout_write` and the trailing `PUSH 0`. The
`END_STRING` leaves the string on the stack as the expression's
value.

---

## §7 — Pattern codegen

`cg_pattern_test` emits the test for one pattern against a value
held in `cur_slot`. Failing jumps are appended to a `PatchList`
that the caller patches to the arm's failure label.

**Per pattern kind:**

- **`AST_PAT_WILDCARD`** — nothing emitted.
- **`AST_PAT_BIND`** — unconditional: `LOAD_LOCAL cur_slot;
  STORE_LOCAL bind_slot`. No test.
- **`AST_PAT_INT`** — `LOAD_LOCAL cur_slot; PUSH value;
  BINARY EQ i64; JMP_IF_FALSE`. The `i64` descriptor is used
  because all integers occupy a full slot.
- **`AST_PAT_BOOL`** — same shape with `0`/`1`.
- **`AST_PAT_STRING`** — resolves the pattern's escapes, pools
  the result, and emits `LOAD_LOCAL cur_slot; LOAD_CONST;
  BINARY EQ string; JMP_IF_FALSE`.
- **`AST_PAT_VARIANT`** — two steps:
  1. `LOAD_LOCAL cur_slot; LOAD_INDIRECT 0 u64;
     PUSH tag; BINARY EQ i64; JMP_IF_FALSE`.
  2. For each subpattern `i`: `LOAD_LOCAL cur_slot;
     LOAD_INDIRECT (8 + i*8) u64; STORE_LOCAL tmp_slots[i]`,
     then recurse against `tmp_slots[i]`.

`tmp_slots[]` is written by sema's `analyze_pattern`; each
subpattern's temp slot holds its payload during the recursive
test.

---

## §8 — Expression codegen

`cg_expr` is a switch over `AstKind`. Contract: emits code that
leaves exactly one value on the stack. The cases group by
concern.

**Literals.** `OP_PUSH` for integers and bools. Float literals go
through the constant pool (with an `f32` fast path emitting the
32-bit pattern inline). String and C-string literals go through
the constant pool as `CONST_STRING` and `CONST_CSTRING`.

**Array literal.** Emit each element in source order, then
`OP_NEW_ARRAY n`. The VM pops in reverse and writes the block in
source order.

**Struct literal.** If the `TypeDef` is `TDEF_EXTERN_STRUCT`:
arena-allocate `struct_size` zeroed bytes via `NATIVE_ARENA_ZERO`,
store each field at its C offset via `OP_STORE_INDIRECT` with the
field's `memkind_of`. The result on the stack is the pointer.
For a regular Prism struct: emit each field's value in the
`TypeDef`'s field order (which is not necessarily the source
order of the initializer list), then `OP_NEW_ARRAY field_count`.

**Field access.** Two cases.

- `unit_tag >= 0` (unit-variant construction, e.g. `Color.Red`):
  `PUSH tag; OP_NEW_ENUM 0` or `OP_NEW_RC_ENUM 0` depending on
  whether `n->type` is `TY_RC`. The RC choice matters: the
  `OP_RC_DEC` emitted at scope exit assumes the payload lives on
  the RC heap.
- Otherwise (field access): evaluate the target, then
  `OP_LOAD_INDIRECT offset memkind`. If the target's type is
  `TY_REF`, an extra `OP_LOAD_INDIRECT 0 u64` peels the ref
  first. For extern struct fields, `memkind_of(declared_type)`
  selects the access width; a nested-by-value extern struct field
  emits `OP_LOAD_ADDR_INDIRECT` (the value's stack representation
  is its address).

**`AST_VARIANT_CTOR`.** Four shapes, dispatched by resolved
fields:

- Method call (`receiver_expr != NULL`): evaluate receiver, then
  args, then `OP_CALL`/`OP_FFI_CALL` with `argc + 1`.
- Module-qualified extern call (`mod_ffi_idx >= 0`): evaluate
  args, then `OP_FFI_CALL`.
- Module-qualified user call (`mod_func_idx >= 0`): evaluate
  args, then `OP_CALL`, with the operand mapped through
  `codegen_of_sema`.
- Enum variant construction: evaluate args, `PUSH tag`,
  `OP_NEW_ENUM`/`OP_NEW_RC_ENUM` (chosen by
  `tdef->is_rc_managed`), with `argc`.

**Variable reference.** Three sub-cases:

- `const_id >= 0`: inline the value (`PUSH` for i64/bool,
  `LOAD_CONST` for f64/string/cstring).
- `func_idx >= 0`: `PUSH codegen_of_sema[func_idx]` — a function
  used as a value.
- `local_index >= 0`: `OP_LOAD_LOCAL slot`.
- Otherwise: internal error.

**Binary and unary.** For `BIN_AND` and `BIN_OR`, short-circuit
code: evaluate left, `OP_DUP`, conditional jump, `OP_POP`,
evaluate right, patch. For everything else: evaluate left, then
right, then `OP_BINARY sub-op desc`. The descriptor is the *left
operand's* type, which for the current language is the unified
operand type. Unary emits operand, then `OP_UNARY sub-op desc`
with `desc_of(n->type)`.

**Function call.** A `builtin_id` dispatch for the builtins
(0–16); each has its own emission. User and extern functions
evaluate args, then `OP_CALL`/`OP_FFI_CALL`. The `func_idx` is
mapped through `codegen_of_sema`.

**`AST_CALL_INDIRECT`.** Evaluate callee (a function value, which
pushes its codegen index), evaluate args, `OP_CALL_INDIRECT argc`.

**`AST_DEREF`.** Evaluate operand, `OP_LOAD_INDIRECT offset
memkind`. Exception: for `*rc<compound>`, the identity is
emitted (no load). The RC handle already points at the payload;
a load would read the payload's first slot (length or tag).

**`AST_ALLOC_ARENA`.** If the operand is a local variable, emit
`OP_LOAD_ADDR_LOCAL slot`. Otherwise evaluate the operand and
`OP_ALLOC 8`. The result is a `ref<T>`.

**`AST_ADDR_OF_FIELD`.** Sema rewrites `&p.x` on an extern
struct field into this node. Evaluate the field's target (the
base pointer), then `OP_LOAD_ADDR_INDIRECT offset`.

**`AST_ALLOC_RC`.** Four cases by operand kind:

- Array literal → `OP_NEW_RC_ARRAY n`.
- Struct literal → `OP_NEW_RC_ARRAY field_count`.
- Variant ctor → `OP_NEW_RC_ENUM payload_count` after pushing the
  tag.
- Unit variant (`AST_FIELD` with `unit_tag >= 0`) →
  `OP_NEW_RC_ENUM 0` after pushing the tag.
- Otherwise (scalar) → evaluate value, `OP_ALLOC_RC 8`.

**`AST_CAST`.** If the source or target is a raw pointer, emit
nothing (bit-preserving). Otherwise evaluate operand and emit
`OP_CONVERT from-desc to-desc`.

**`AST_SIZEOF`.** `PUSH size`.

**`AST_TRY`.** Emit `cg_expr(operand)`, then
`emit_placeholder(OP_TRY_JUMP_IF_OK)`, then
`cg_emit_all_rc_drops(cg)`, then `OP_RET`, then
`patch_to_here(jmp_ok)`. On success the VM replaces the block
with the payload and jumps past the RET; on failure the block
stays on the stack and the RET returns it.

---

## §9 — Statement codegen

`cg_stmt` dispatches by statement kind. The contract: leaves the
stack balanced (no net change).

**`AST_BLOCK`.** Delegates to `cg_block`.

**`AST_LET`.** Evaluate the initializer. If `holds_rc` and the
initializer is a local RC value, emit `LOAD_LOCAL; RC_INC` to
share the reference. Register the local in the current `RcScope`.
**No `STORE_LOCAL`** — the initializer's value is the local; the
frame grows to include it implicitly.

**`AST_ASSIGN`.** Four target shapes:

- `AST_VAR`: evaluate value; if the value is an RC local, share
  it (`LOAD_LOCAL; RC_INC`); if the target holds RC, drop the
  old value (`LOAD_LOCAL; RC_DEC desc`); `STORE_LOCAL`.
- `AST_DEREF`: evaluate target (the pointer), then value,
  `STORE_INDIRECT offset memkind`.
- `AST_INDEX`: evaluate container, index, value, `INDEX_STORE
  container-desc`.
- `AST_FIELD`: evaluate target (peeling a ref if present),
  evaluate value, `STORE_INDIRECT offset memkind`.

**`AST_ASSIGN_OP`.** Compound assignment. For each target shape,
the pattern is:

1. Evaluate the target's current value (with `DUP`/`DUP2` where
   the target's address must be preserved).
2. If the value is RC (`temp_slot >= 0`), save the old value to
   the temp.
3. Evaluate the RHS.
4. `OP_BINARY sub-op desc`.
5. If RC, `LOAD_LOCAL temp; RC_DEC desc`.
6. Store through the target.

**`AST_EXPR_STMT`.** Evaluate the expression; discard the result.
The choice between `OP_RC_DEC` and `OP_POP` is made by
`expr_produces_fresh_rc`, which classifies the expression by kind:
`format`/`ffi_error`/`ffi_string` calls, RC-returning user/extern
calls, RC variant constructors, unit variant constructions, RC
allocs, string concats, string literals, and try results are
fresh; everything else (locals, field reads, indexes, derefs) is
borrowed. Borrowed values are popped; fresh values are dropped.

**`AST_IF`.** Evaluate condition, `JMP_IF_FALSE else`, then-branch,
`JMP end` (if an else exists), else-branch, patch.

**`AST_WHILE`.** Loop start = current pc. Evaluate condition,
`JMP_IF_FALSE end`. Body. Patch continue targets. `JMP start`.
Patch the `JMP_IF_FALSE` to here (end). Patch break targets to
end.

**`AST_FOR_RANGE`.** `ENTER_SCOPE` (loop scope) + rc-scope push.
Evaluate start and end onto the stack as the var and end slots.
Loop: `LOAD_LOCAL var; LOAD_LOCAL end; BINARY LT i64;
JMP_IF_FALSE end`. Body scope: `ENTER_SCOPE` + rc-scope push.
Body. RC-scope pop + `EXIT_SCOPE`. Patch continues. Increment
var (`LOAD_LOCAL var; PUSH 1; BINARY ADD i64; STORE_LOCAL var`).
`JMP start`. Patch `JMP_IF_FALSE`. RC-scope pop + `EXIT_SCOPE`
(loop scope). Patch breaks.

**`AST_FOR_IN`.** Same structure. Loop scope holds `arr`, `len`,
`idx`; body scope holds `var`. The iterable is evaluated once;
if it's `TY_RC`, the `arr_slot` is registered as an RC local.
RC-scope pushes and pops bracket both scopes.

**`AST_MATCH`.** `ENTER_SCOPE` + rc-scope push. Evaluate the
scrutinee, `STORE_LOCAL hidden_slot`. For each arm: emit the
pattern test with a fresh `PatchList`; emit the arm's body;
emit `JMP end`; patch the arm's pattern-fail jumps to the next
arm's start. After the last arm, `OP_TRAP` with a
`"match: no arm matched"` pool string. Patch the `JMP end`s to
here. RC-scope pop + `EXIT_SCOPE`.

**`AST_RETURN`.** Evaluate the return value (or `PUSH 0` for a
bare return). `cg_emit_all_rc_drops(cg)` to drop every active
scope's RC locals. `OP_RET`.

**`AST_BREAK`/`AST_CONTINUE`.** For each scope between the
current depth and the target depth (from `LoopFrame`), emit
`cg_emit_n_scope_drops` (once, for all N scopes), then
`OP_EXIT_SCOPE` N times. Then `JMP` to a placeholder recorded in
the loop frame.

**`AST_CONST`.** No emission. Constants are inlined at use
sites.

**`AST_ENUM_DEF`.** No emission. Enums have no runtime
representation as top-level declarations.

---

## §10 — RC-scope tracking

The RC-scope stack tracks which locals in which scope own an RC
reference. `cg->rc_scope_count` is parallel to `cg->scope_depth`;
the invariant `cg->rc_scope_count == cg->scope_depth` holds at
every statement boundary.

**Registration.** `AST_LET` calls `cg_register_rc_local(cg, slot,
type)` after emitting the initializer, when `holds_rc` is set.
The registration records the slot and the `Type*` (so the
eventual `RC_DEC` can look up the descriptor for a deep drop).

**Natural drops.** `cg_block` calls `cg_emit_scope_drops` for the
innermost `RcScope` before its `EXIT_SCOPE`. Same for the
explicit scope pushes in `AST_FOR_RANGE`/`AST_FOR_IN`/`AST_MATCH`.

**Early-exit drops.** Three code paths exit scopes without going
through the natural `EXIT_SCOPE`:

- `AST_RETURN`: `cg_emit_all_rc_drops` drops every scope from
  innermost to outermost, then `OP_RET`.
- `AST_BREAK` / `AST_CONTINUE`: `cg_emit_n_scope_drops(n)` drops
  the N scopes being unwound, then the `EXIT_SCOPE`s, then the
  jump.
- `AST_TRY` failure path: `cg_emit_all_rc_drops` before the
  `OP_RET` inside the try.

**The three helpers:**

- `cg_push_rc_scope` — pushes an empty scope, increments
  `rc_scope_count`.
- `cg_emit_scope_drops(cg, scope, where)` — emits drops for one
  scope's registered locals, in insertion order. Does not pop.
- `cg_emit_all_rc_drops(cg)` — drops every active scope from
  innermost to outermost. Does not pop.
- `cg_emit_n_scope_drops(cg, n)` — drops the N innermost scopes'
  locals. Does not pop.

**Why this is codegen's job, not the VM's.** The VM does not
track which locals hold RC references. `OP_EXIT_SCOPE` restores
the arena mark and the stack pointer; it does not walk any RC
list. Codegen knows (via `holds_rc`) and emits the drops
explicitly.

---

## §11 — Entry point

`codegen_program` runs four phases.

**Phase 1 — index assignment.** Walk `sc->funcs[]`. For each
function that is not extern and not a template (non-generic),
assign a codegen index starting at 0. Record the mapping in
`codegen_of_sema[]` and locate `main` by name.

A function with no codegen index (extern or template) never
appears in a `CALL` operand. Every `AST_CALL`'s `func_idx` is a
sema index that `codegen_of_sema` maps to a codegen index before
emission.

**Phase 2 — `main` check.** If `main` was not found, report
`no 'main' function declared` and return without emitting
anything.

**Phase 3 — prologue.** `CALL main 0; HALT`. Offsets 0–3. The
`CALL`'s `func_idx` is `main`'s codegen index (always 0 unless a
prior function is emitted first — but functions are emitted in
Phase 4, so `main`'s codegen index is assigned in Phase 1 and the
`CALL` is emitted before any body).

**Phase 4 — bodies.** Walk `sc->funcs[]` again. For each
codegen-eligible function, call `chunk_add_func` to record its
entry pc (which is `chunk->count` at the time of the call), then
`cg_stmt` on the body, then a fallback `PUSH 0; RET`.

**Emission order** is source order (with instantiations appended
by sema). Because `CALL` targets are looked up in the func table
at runtime (not backpatched), source order is not load-bearing.
The only forward patch is the prologue's `CALL main`, which is
resolved in Phase 1.

**Imbalance check.** After the loop, if `cg.scope_depth != 0` or
`cg.loop_count != 0`, a codegen bug is reported to stderr.

---

## §12 — Invariants

- **I36** (see `invariants.md`) — every `cg_expr` call leaves
  exactly one value on the data stack.
- **I37** — `cg->rc_scope_count == cg->scope_depth` at every
  statement boundary. Every `OP_ENTER_SCOPE`-emitting construct
  pushes an `RcScope`; every `OP_EXIT_SCOPE`-emitting construct
  pops one.
- **I38** — every `FuncEntry.entry_pc` points at the first word
  of the function's body.
- **I39** — every forward call's placeholder is patched before
  the chunk is handed to the VM.
- **I40** — every jump target is within the emitted code.
- **I41** — every `desc_of(t)` produces a descriptor consistent
  with the operand's actual type.
- **I42** — the RC drop is emitted on every path that exits a
  scope with RC locals.
- **I43** — every value whose sema type is `rc<T>` is allocated
  by an `OP_NEW_RC_*` opcode.

---

## §13 — Known limitations

**`MAX_LOOP_DEPTH = 32`, `MAX_BREAKS_PER_LOOP = 64`.** Nesting
beyond either cap produces a diagnostic.

**Format chunk buffer is 1024 bytes.** A print format string
longer than 1023 bytes per literal chunk is silently truncated.

**`cg_emit_literal_chunk`'s buffer is a fixed stack array.** Not
growable; the cap is `sizeof(buf) - 1 = 1023`.

**`desc_of(NULL)` warns but proceeds.** If a sema bug leaves an
expression's `type` NULL, codegen emits a `TD_I64` operand and
produces wrong bytecode. The warning is the only signal.

**Dead code after `HALT`.** The prologue's `HALT` is at offset 3;
function bodies follow. The disassembler does not stop at HALT
(it prints `(--- N words past this HALT ---)`), but the emitted
code never executes past it.

**`codegen_of_sema[]` is a stack-local in `codegen_program`,**
copied into the `Cg` struct. Minor.

**No optimization.** No constant folding, no peephole, no
dead-code elimination.

**`AST_FIELD`'s unit-variant RC choice was a source of a
historical bug.** The current code selects `OP_NEW_RC_ENUM` vs.
`OP_NEW_ENUM` by checking `n->type->kind == TY_RC`. Forgetting
this check put the block in the arena while the scope-exit drop
treated it as RC heap memory, corrupting the heap. See
`handbook/changelog.md`.

**`AST_ASSIGN_OP` for `AST_FIELD` was a historical bug.** The
value expression was evaluated twice and the old value's drop
was applied to the wrong slot. The current code evaluates once
and preserves the old value through the temp. See
`handbook/changelog.md`.

**No way to emit a chunk without debug info.** `lines` and
`cols` are always populated.

**Backend-limit diagnostics are the only non-`internal:` errors
codegen emits.** Sema is expected to have caught everything
user-visible.

---

## §14 — Extension points

- **Adding an opcode.** Emit it from a `cg_expr`/`cg_stmt` case;
  register it in `vm.h`, `vm.c`, and `disasm.c`'s `OPINFO`. See
  `arch/recipes.md` §Adding an opcode.
- **Adding an AST node kind.** Add a `case` to `cg_expr` or
  `cg_stmt`, and a corresponding case to `ast_free` and
  `mono_copy_node`. See `arch/recipes.md` §Adding an AST node
  kind.
- **A release mode that drops debug info.** Would thread a flag
  through `emit`; the `Chunk` supports NULL `lines`/`cols`.
- **RC-scope registration for a new statement kind.** Any new
  construct that declares RC locals must call
  `cg_register_rc_local`; any that exits a scope must call one of
  the three drop helpers before its `EXIT_SCOPE`.
- **Peephole optimization.** A post-pass over `chunk->code` before
  `validate_bytecode`.
- **A `dump_ast` debug printer.** No printer exists for the
  annotated AST.
- **Constant folding in `cg_expr`.** Would let `PUSH 1; PUSH 2;
  BINARY ADD` be replaced with `PUSH 3`.
- **Register allocation.** Not applicable; the VM has no
  registers in the traditional sense.