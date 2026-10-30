## `docs/arch/runtime.md`

# The Prism Runtime Model

*Current as of commit `<hash>`, <date>.*

This document describes how the Manu VM executes Prism bytecode. It's
for anyone who wants to read `codegen.c`, `vm.c`, or `disasm.c`, or
who wants to add an opcode or a builtin. It assumes you're comfortable
with stack machines and C.

The document is descriptive, not prescriptive. Everything here is
what the code does. Where a design choice is non-obvious, the reason
is given, and where a design choice is currently wrong, the issue is
named.

If you read only one thing, read §6 (the calling convention) and §9
(the RC insertion algorithm). Everything else is elaboration.

---

## §1 — The shape of the machine

Manu is a 64-bit stack machine. It reads instructions from an array
of 64-bit words, mutates a small set of registers, and pushes and
pops from a data stack. There is no tagged-union value representation:
every value on the data stack is a `U64`, and its *type* is known only
to the compiler.

The VM has:

- **Five registers** (§3): `pc`, `sp`, `fp`, `ap`, `amp`.
- **Three stacks** (§4): data, call, scope.
- **Three memory regions** (§5): the lexical arena, the const arena,
  and the RC heap.

Everything else — function calls, lexical scoping, reference counting,
FFI marshalling — is a pattern of register mutations that the compiler
has agreed to emit instructions for. The VM itself has no concept of
a "function" or a "scope". It has a program counter, a stack, and a
switch statement.

This is the single most important thing to internalize:

> **The semantics of Prism live in the compiler, not in the VM.**

When `codegen.c` emits `CALL func#0 argc=2`, it is relying on a
*contract* with the VM about what `CALL` does to `fp`, `sp`, and
`pc`. The contract is not enforced by the VM. It is assumed. If a
codegen change violates it, the VM will produce wrong results
silently. The validator in `disasm.c` catches structural errors
(opcodes out of range, jump targets out of bounds), but it cannot
catch a codegen bug that produces structurally valid but semantically
wrong bytecode.

---

## §2 — The five registers

Read this table twice. The definitions are short, and getting one
wrong will make every other section incomprehensible.

| Register | Type | Meaning |
|---|---|---|
| `pc` | `U64` | Index into `bytecode[]`. Points at the **next word to read**. |
| `sp` | `U32` | Index into `data_stack[]`. Points at the **next free slot**. |
| `fp` | `U32` | Index into `data_stack[]`. The **first argument** of the current call. |
| `ap` | `U8*` | Bump pointer into the lexical arena. The **next free byte**. |
| `amp` | `U8*` | Arena mark. Saved on scope entry, restored on scope exit. |

Three of these are deliberately counter-intuitive:

- **`sp` is not "the top of the stack".** It is one past the top. The
  top-of-stack value is `data_stack[sp - 1]`. Pushing writes to
  `data_stack[sp]` and then increments; popping decrements and then
  reads `data_stack[sp]`. This is the convention that makes PUSH and
  POP symmetrical.

- **`fp` is not "the base of the frame".** It is the index of the
  first argument. Argument `i` for `i` in `[0, argc)` is at
  `data_stack[fp + i]`. Local variables declared inside the callee
  start at slot `argc` and go up. So a function with three
  parameters has its locals at slots 3, 4, 5, …

- **`amp` is not "the current mark".** It is the *enclosing* scope's
  mark. On scope entry, the previous `amp` is saved on the scope
  stack and the new `amp` is set to the current `ap`. On scope exit,
  `ap` is reset to `amp` (freeing the scope's allocations) and then
  `amp` is restored from the scope stack. The two-step dance is what
  makes nested scopes work.

There is a sixth field, `current_pc`, which is not a register in the
execution sense. The dispatch loop sets `vm->current_pc = vm->pc`
at the top of every iteration. Runtime errors report `current_pc`,
not the advanced `pc`, so the error location points at the *start*
of the failing instruction. See `vm.c`'s dispatch loop and the
`RT_ERR` macro.

---

## §3 — The three stacks

### §3.1 The data stack

```c
U64 data_stack[DATA_STACK_MAX];   // DATA_STACK_MAX = 4096
U32 sp;
U32 fp;
```

Every value the program manipulates lives here. Integers, floats
(bit patterns), pointers, string-block pointers, RC-block pointers
— all `U64`. There is no separate storage for locals: a local is a
slot in the data stack, addressed as `data_stack[fp + slot]`.

The stack is a flat array, not a linked structure. `sp` is its
extent. Values below `fp` belong to callers; values above `fp`
belong to the current function (arguments first, then locals).

### §3.2 The call stack

```c
typedef struct {
    U64  return_pc;
    U32  fp;
    U8*  amp;
    U32  scope_depth;
    int  func_idx;
    U64  call_pc;
} CallFrame;

CallFrame call_stack[CALL_STACK_MAX];   // CALL_STACK_MAX = 256
U32 call_depth;
```

One frame per active call. Pushed by `OP_CALL`, popped by `OP_RET`.
This is a *save area*, not a value stack. On `OP_CALL`, the caller's
`return_pc`, `fp`, `amp`, and `scope_depth` are captured into a new
frame; on `OP_RET` they're restored.

`func_idx` and `call_pc` are diagnostics-only. `func_idx` is the
callee's index in the chunk's function table, used by the error
trace renderer to spell the callee's name. `call_pc` is the `pc` of
the `OP_CALL` instruction itself (not the return address), used by
the trace to show "called from here".

### §3.3 The scope stack

```c
typedef struct {
    U32 sp;
    U8* amp;
} ScopeFrame;

ScopeFrame scope_stack[SCOPE_STACK_MAX];   // SCOPE_STACK_MAX = 256
U32 scope_depth;
```

One frame per `OP_ENTER_SCOPE`. `OP_EXIT_SCOPE` restores both fields.
This is *separate from the data stack* deliberately; an earlier
design pushed arena marks onto the data stack, which corrupted
locals whenever a scope contained both values and allocations. The
separation is the fix.

Note that `scope_depth` is also saved on the call frame. `OP_CALL`
does not reset it; `OP_RET` restores it. So a callee that enters
scopes and returns without exiting them has its scope depth silently
reset by the RET. See §6.4.

### §3.4 Why three and not one

A single combined stack would need to know, for each entry, whether
it's a value, a call frame, or a scope frame. That would require
tagged entries and a runtime dispatch on the tag at every
push/pop. Keeping them separate means each push/pop is a plain
assignment.

The trade-off is that `OP_RET` has to update three things at once
(the data stack, the call frame, and the arena mark). That's
acceptable because RET happens once per call, not once per
instruction.

---

## §4 — The three memory regions

### §4.1 The lexical arena

```c
#define ARENA_MAX (1 << 20)   // 1 MiB
U8* arena;
U8* ap;
U8* amp;
```

A bump allocator. `arena_alloc` rounds `ap` up to an 8-byte boundary,
checks the request fits within `arena + ARENA_MAX`, and advances
`ap`. Alignment is 8 bytes because every value the arena can hold
is either a `U64` or a block whose first word is a `U64` length or
tag.

Reclamation is wholesale. `OP_ENTER_SCOPE` saves the current `ap`
into a scope frame; `OP_EXIT_SCOPE` resets `ap` to the enclosing
scope's mark. Everything the scope allocated is reclaimed in O(1)
regardless of how much there was.

The arena is fixed at 1 MiB. Overflow is a runtime error
(`"arena out of memory"`), not a growth. The size is chosen so that
any reasonable program's peak lexical-scope allocation fits; a
program that doesn't fit is either doing something pathological or
should be using the RC heap.

Arena allocations are valid until the enclosing scope exits. The
three escape rules (§10 of `language/reference.md`) exist to make
sure a `ref<T>` never outlives the scope that produced it.

### §4.2 The const arena

```c
U8* const_arena;
U8* cap;
U8** const_ptrs;
size_t const_arena_size;
```

A second bump allocator, sized once at VM init from the chunk's
constant pool. It holds string literals and C-string literals,
materialized into blocks at init and never freed until the VM is
destroyed.

Two layouts:

- **`CONST_STRING`** — `[RcHeader][u64 length][bytes]`. The header's
  `ref_count` is set to the sentinel `0xFFFFFFFF`, which makes
  `OP_RC_INC` and `OP_RC_DEC` no-ops. This is what lets a string
  literal be dropped uniformly with a runtime-produced string
  without the RC system trying to free a block that was never
  `malloc`'d.
- **`CONST_CSTRING`** — `[bytes][NUL]`. No header. This is what C
  functions receive when Prism passes a `c"..."` literal.

`const_ptrs[i]` is the payload pointer for pool entry `i` (payload
means "after the RcHeader" for a `CONST_STRING`; for a `CONST_CSTRING`
it is the base pointer). `OP_LOAD_CONST` reads this array directly.

If the total size exceeds `CONST_ARENA_MAX` (64 KiB), `vm_init`
aborts with `"const arena overflow"`. This is a hard limit and a
compile-time-detectable one, but no pass checks it currently; a
program with more string-literal bytes than fit in the const arena
crashes on startup rather than being rejected. `INFERRED`: this is a
latent bug in the compiler, not the VM.

### §4.3 The RC heap

```c
typedef struct {
    U32 ref_count;
    U32 padding;
} RcHeader;
```

Every RC-managed block has an 8-byte `RcHeader` before its payload.
`ref_count` is a 32-bit reference count; `padding` keeps the header
at 8 bytes so the payload is 8-aligned.

The RC heap is `malloc`-backed; there is no freelist, no pool, no
arena. Blocks are freed when their refcount hits zero (see §8).

Three sentinel values are worth remembering:

- `ref_count == 0xFFFFFFFF` — const-arena string. `RC_INC`/`RC_DEC`
  short-circuit. The block is never freed by the runtime; it lives
  in the const arena until the VM is destroyed.
- `ref_count == 0` — should never be observed. `RC_DEC` frees before
  the count is readable at zero.
- `payload == NULL` — a valid RC value meaning "no reference".
  `RC_INC`/`RC_DEC` on NULL are no-ops. This is what makes the
  "function with no value-return on some path" case safe: the caller
  reads a NULL, `holds_rc` is false at the call site, and no drop
  is emitted.

### §4.4 The relationship between the regions

The compiler chooses a region for each allocation at compile time
based on the type:

- A value whose type is `ref<T>` — allocated in the lexical arena.
- A value whose type is `rc<T>` — allocated on the RC heap.
- A string literal or C-string literal — materialized in the const
  arena at init.
- A non-RC struct, enum, or array value — allocated in the lexical
  arena (they're `[tag][payloads…]` blocks, structurally identical
  to `rc<...>` blocks minus the header).

Sema rejects several combinations that would mix the two (a `rc`
value can't contain a `ref`, and vice versa; see §10 of the language
reference). The rules are the type-system half of the escape
analysis.

---

## §5 — The calling convention

### §5.1 The contract

At the bytecode level:

- **Arguments are pushed left-to-right by the caller.** The last
  argument is on top of the stack when the CALL executes.
- **`OP_CALL func_idx argc`** sets `fp = sp - argc`, jumps to
  `funcs[func_idx].entry_pc`, and saves the caller's registers in
  a new call frame.
- **`OP_RET`** pops the return value, resets `ap = amp`, truncates
  `sp` to `fp`, pushes the return value, and restores the caller's
  registers from the frame.

The callee sees its arguments at slots `0..argc-1`. It sees its
locals at slots `argc..`. It never sees slots below `fp`; those
belong to the caller.

### §5.2 `OP_CALL`, step by step

The relevant code from `vm.c`:

```c
case OP_CALL: {
    U64 func_idx = READ();
    U64 argc     = READ();

    if (!vm->funcs || func_idx >= (U64)vm->funcs->count)
        RT_ERR("bad call target %llu", ...);
    if (vm->call_depth >= CALL_STACK_MAX)
        RT_ERR("call stack overflow%s", "");
    if ((U64)vm->sp < argc)
        RT_ERR("stack underflow on call%s", "");

    CallFrame* f = &vm->call_stack[vm->call_depth++];
    f->return_pc   = vm->pc;
    f->fp          = vm->fp;
    f->amp         = vm->amp;
    f->scope_depth = vm->scope_depth;
    f->func_idx    = (int)func_idx;
    f->call_pc     = vm->current_pc;

    vm->fp  = vm->sp - (U32)argc;
    vm->amp = vm->ap;
    vm->pc  = vm->funcs->items[func_idx].entry_pc;
    break;
}
```

Five steps, in order:

1. **Read operands.** `func_idx` and `argc` are the two operand
   words. `READ()` advances `pc`.
2. **Bounds-check.** A bad function index, a full call stack, or a
   stack too shallow to hold the arguments all abort with a runtime
   error.
3. **Push a frame.** The current `return_pc` (which is the pc
   *after* the operands), `fp`, `amp`, and `scope_depth` are
   captured. `call_pc` is set to `current_pc` — the pc of the CALL
   instruction itself, used for the error trace.
4. **Set the new `fp`.** `sp - argc` is the index of the first
   argument. From the callee's perspective, argument `0` is at slot
   `0`, argument `1` at slot `1`, etc.
5. **Jump.** The callee's `entry_pc` is looked up in the func table.

Two subtleties:

- **`amp` is set to `ap` (the current bump pointer), not the caller's
  `amp`.** The callee's arena scope starts fresh. Any arena
  allocation the callee does is reclaimed when the callee RETs.
  This is what makes arena-based allocation safe across calls.

- **`scope_depth` is not reset.** The callee inherits the caller's
  scope depth as its starting point. Any `OP_ENTER_SCOPE` the callee
  executes grows the stack from there. On RET, `scope_depth` is
  restored from the call frame, discarding any unexited scopes.
  See §6.4 for why this is important.

### §5.3 `OP_RET`, step by step

```c
case OP_RET: {
    if (vm->call_depth == 0)
        RT_ERR("return outside function%s", "");
    if (vm->sp <= vm->fp)
        RT_ERR("return with no value%s", "");

    U64 ret = POP();

    vm->ap = vm->amp;

    vm->sp = vm->fp;
    PUSH(ret);

    CallFrame* f = &vm->call_stack[--vm->call_depth];
    vm->fp          = f->fp;
    vm->amp         = f->amp;
    vm->scope_depth = f->scope_depth;
    vm->pc          = f->return_pc;
    break;
}
```

Six steps:

1. **Check.** Depth zero or a stack at-or-below the frame base is a
   runtime error.
2. **Pop the return value.** This is the value the callee produced.
3. **Reset `ap = amp`.** The callee's arena allocations are freed in
   one assignment. Whatever the callee allocated (its locals'
   arena-allocated content, temporary arrays, etc.) is now gone.
4. **Truncate the data stack.** `sp = fp` drops the callee's
   arguments and locals. This is the caller-cleanup convention: the
   callee does not need to know how many arguments it received to
   leave the stack in the right shape.
5. **Push the return value.** The caller sees exactly one value
   where it pushed `argc` arguments.
6. **Restore the caller's registers.** `fp`, `amp`, and `scope_depth`
   come from the frame. `pc` jumps to `return_pc`.

`OP_RET` does three things at once (release the arena, truncate the
data stack, restore the registers). The combination is why a single
opcode is sufficient; splitting it would require the caller to do
cleanup work that the callee is better positioned to do.

### §5.4 A worked trace

Take this program:

```
fn main(): i64 {
    let a = 27
    let b = 15
    return a + b
}
```

`codegen.c` produces this bytecode. Offsets are in words.

```
 0  CALL   0 0          ; 3 words: opcode, func_idx, argc
 3  HALT
 4  ENTER_SCOPE
 5  PUSH   27           ; 2 words
 7  PUSH   15           ; 2 words
 9  LOAD_LOCAL 0        ; 2 words
11  LOAD_LOCAL 1        ; 2 words
13  BINARY ADD i64      ; 3 words: opcode, sub-op, desc-idx
16  RET
17  EXIT_SCOPE
18  PUSH   0            ; 2 words
20  RET
```

Initial state: `pc = 0`, `sp = 0`, `fp = 0`, `call_depth = 0`,
`scope_depth = 0`, `ap = amp = arena`.

**`pc=0: CALL 0 0`**
- Read `func_idx = 0`, `argc = 0`. `pc = 3`.
- Push frame: `{return_pc = 3, fp = 0, amp = arena, scope_depth = 0,
  func_idx = 0, call_pc = 0}`. `call_depth = 1`.
- `fp = sp - argc = 0`.
- `amp = ap = arena`.
- `pc = funcs[0].entry_pc = 4`.

**`pc=4: ENTER_SCOPE`**
- `scope_stack[0] = {sp = 0, amp = arena}`. `scope_depth = 1`.
- `amp = ap = arena`.

**`pc=5: PUSH 27`**
- `data_stack[0] = 27`. `sp = 1`.
- This is local `a`. Its slot is `fp + 0 = 0`.

**`pc=7: PUSH 15`**
- `data_stack[1] = 15`. `sp = 2`.
- This is local `b`. Its slot is `fp + 1 = 1`.

**`pc=9: LOAD_LOCAL 0`**
- Push `data_stack[fp + 0] = data_stack[0] = 27`.
- `data_stack[2] = 27`. `sp = 3`.

**`pc=11: LOAD_LOCAL 1`**
- Push `data_stack[1] = 15`.
- `data_stack[3] = 15`. `sp = 4`.

**`pc=13: BINARY ADD i64`**
- Pop `b = 15`, pop `a = 27`.
- Push `a + b = 42`.
- `data_stack[2] = 42`. `sp = 3`.

**`pc=16: RET`**
- Pop `ret = 42`. `sp = 2`.
- `ap = amp = arena`.
- `sp = fp = 0`. Push 42. `data_stack[0] = 42`. `sp = 1`.
- Pop frame. Restore `fp = 0`, `amp = arena`, `scope_depth = 0`,
  `pc = return_pc = 3`. `call_depth = 0`.

**`pc=3: HALT`**
- Return 0 from `vm_run`.

`vm_top(&vm)` reads `data_stack[sp-1] = data_stack[0] = 42`. The
driver prints "return value = 42".

### §5.5 Recursion

Because `fp` is saved and restored on every call, recursion works
without any special handling. A call to `fact(n-1)` sets up a fresh
frame with a fresh `fp`; the callee's `a` is at a different physical
slot than the caller's. The two frames are on the same physical
array, but their `fp` values differ, so they don't alias.

The recursion depth is bounded by `CALL_STACK_MAX = 256`. A program
that recurses deeper than 256 frames traps with
`"call stack overflow"`. There is no tail-call optimization.

---

## §6 — The scope lifetime

### §6.1 `OP_ENTER_SCOPE`

```c
case OP_ENTER_SCOPE: {
    if (vm->scope_depth >= SCOPE_STACK_MAX)
        RT_ERR("scope stack overflow%s", "");
    vm->scope_stack[vm->scope_depth].sp  = vm->sp;
    vm->scope_stack[vm->scope_depth].amp = vm->amp;
    vm->scope_depth++;
    vm->amp = vm->ap;
    break;
}
```

Three things:

1. Save the current `sp` and `amp` in a fresh scope frame.
2. Increment `scope_depth`.
3. Set `amp = ap`. Any allocation the scope makes bumps `ap` above
   the new `amp`.

### §6.2 `OP_EXIT_SCOPE`

```c
case OP_EXIT_SCOPE: {
    if (vm->scope_depth == 0)
        RT_ERR("scope stack underflow%s", "");
    vm->ap = vm->amp;
    vm->scope_depth--;
    vm->sp  = vm->scope_stack[vm->scope_depth].sp;
    vm->amp = vm->scope_stack[vm->scope_depth].amp;
    break;
}
```

Three things:

1. Reset `ap = amp`. The scope's arena allocations are gone.
2. Decrement `scope_depth`.
3. Restore `sp` and `amp` from the scope frame.

### §6.3 Nested scopes

Consider:

```
fn f() {
    let a = 1;
    {
        let b = 2;
        {
            let c = 3;
        }
    }
}
```

Three `ENTER_SCOPE`s push three frames:

| depth | scope_stack[depth] |
|---|---|
| 0 | `{sp = sp₀, amp = arena}` |
| 1 | `{sp = sp₀+1, amp = arena}` |
| 2 | `{sp = sp₀+2, amp = arena}` |

Three `EXIT_SCOPE`s pop them in reverse order. After the innermost
exits, `ap` is reset to `amp` (the innermost frame's saved `amp`,
which is `arena`), and `sp` is restored to `sp₀+2` — the extent
before the innermost `ENTER_SCOPE`.

The nesting is not tracked explicitly; the scope stack is a stack,
so LIFO order is automatic. The compiler emits `ENTER`/`EXIT` pairs
that nest properly.

### §6.4 The interaction with the calling convention

The scope stack and the call stack share `scope_depth`. This is
deliberate but subtle.

`OP_CALL` captures `scope_depth` into the new call frame. It does
*not* reset it. A callee that executes `ENTER_SCOPE` at its start
pushes onto the caller's scope stack.

If the callee returns without executing `EXIT_SCOPE` (for example,
because it `RET`ed from inside an `if` block that entered a scope),
its scopes remain on the scope stack. `OP_RET` restores
`scope_depth` from the call frame, which effectively discards them.

This works because the callee's RET also resets `ap = amp`, which
is the arena mark the callee started with. Any arena allocation the
callee's unexited scopes made is reclaimed at the same time.

The consequence for codegen: **`OP_RET` must be preceded by RC
drops for the callee's own RC locals**. The scope stack tracks
arena marks; it does not track RC references. See §9.

### §6.5 When scopes don't match

The compiler is expected to emit balanced `ENTER`/`EXIT` pairs on
every path *except* early returns. Early returns are handled by
`OP_RET`'s restore of `scope_depth` from the frame.

The validator (`validate_bytecode` in `disasm.c`) does not check
scope depth or balance. It's a structural check only. A codegen bug
that emitted an unmatched `ENTER_SCOPE` would produce valid bytecode
that eventually traps with `"scope stack overflow"`.

---

## §7 — The one-value invariant

> **Every expression, when evaluated, leaves exactly one value on
> the data stack.**

Void expressions leave `0`. Expression statements are followed by
an `OP_POP` or an `OP_RC_DEC` to consume the value. Statement
codegen assumes the invariant holds; it never tracks stack depth
at the statement level.

The invariant is what makes `if`-as-statement, expression statements,
and the whole shape of `cg_expr`/`cg_stmt` work. Every `cg_expr`
case either pushes a value or emits a sequence that ends with a
push. Every `cg_stmt` case either consumes a value or leaves the
stack unchanged.

There is no runtime check for the invariant. If a codegen change
breaks it (say, by emitting a branch that doesn't push on one path),
the program will produce wrong results silently. The test suite is
the only defense.

The invariant is checked structurally by `arch/invariants.md` §I21
in the enforcement map.

---

## §8 — RC insertion: how codegen keeps the counts honest

This is the part of codegen that is most likely to have bugs, and
it's the part that changed most recently. Read it carefully.

### §8.1 The compile-time view

`codegen.c` maintains an **RC scope stack**, parallel to
`cg->scope_depth`:

```c
typedef struct {
    int   slot;
    Type* type;
} RcLocal;

typedef struct {
    RcLocal items[MAX_LOCALS];
    int     count;
} RcScope;

typedef struct {
    ...
    RcScope rc_scopes[MAX_SCOPES];
    int     rc_scope_count;
} Cg;
```

The invariant is: **`cg->rc_scope_count == cg->scope_depth` at every
statement boundary.** Each `OP_ENTER_SCOPE`-emitting construct
pushes an `RcScope`; each `OP_EXIT_SCOPE`-emitting construct pops one.

Every `let` whose value is RC-managed registers itself in the
innermost `RcScope` via `cg_register_rc_local`. The registration
records the local's slot and its `Type*` (so the eventual `RC_DEC`
can look up the descriptor for a deep drop).

### §8.2 The runtime primitives

Three opcodes matter:

- **`OP_RC_INC`** — pops a payload pointer. If NULL, no-op. If the
  header's `ref_count` is the const-arena sentinel, no-op. Otherwise
  increments.
- **`OP_RC_DEC`** — reads a descriptor operand, pops a payload
  pointer. If NULL, no-op. If the sentinel, no-op. Otherwise
  decrements; if the count reaches zero, `rc_drop_deep` runs and
  the block is freed.
- **`OP_ALLOC_RC`, `OP_NEW_RC_ARRAY`, `OP_NEW_RC_ENUM`** — the
  allocations. Each `malloc`s a block, writes the header with
  `ref_count = 1`, and pushes the payload pointer.

`rc_drop_deep(vm, value, desc)` is the interesting one. It examines
the descriptor:

- `TD_STRING` — decrement, free.
- `TD_RC` — decrement; if zero, walk the payload based on the
  inner descriptor, then free.
- `TD_STRUCT`, `TD_ARRAY`, `TD_ENUM` — walk the payload's fields,
  elements, or variant payloads, dropping each in turn.
- Primitives, `TD_REF`, `TD_RAWPTR` — no-op.

The "walk the payload" step is what makes recursive RC structures
(e.g. a struct containing an `rc<[...]>`) drop cleanly: each
sub-value's header is decremented, and if it reaches zero, its own
`rc_drop_deep` runs.

### §8.3 The drop sites

There are five places codegen emits drops:

1. **Block exit.** `cg_block` emits a drop for every RC local in
   the innermost scope before the `OP_EXIT_SCOPE`. This handles the
   normal case: a local goes out of scope, its reference drops.

2. **`return`.** `AST_RETURN` emits drops for *all* active scopes
   (via `cg_emit_all_rc_drops`) before the `OP_RET`. This handles
   the early-return case: a function that returns from inside a
   block with RC locals must drop them.

3. **`break` / `continue`.** Both emit drops for the scopes being
   unwound (via `cg_emit_n_scope_drops`). The count comes from
   `cg->scope_depth - lf->entry_scope_depth` (for break) or
   `cg->scope_depth - lf->continue_scope_depth` (for continue).

4. **`?` failure.** `AST_TRY` emits drops for all active scopes
   before the `OP_RET` on the failure path.

5. **`let` reassignment and RC assignments.** When a local is
   reassigned and the old value was RC, the drop happens before
   the `OP_STORE_LOCAL`. When a local is reassigned *from another
   RC local*, an `OP_RC_INC` runs first to share the reference.

Each of these is emitted by a specific code path. If you add a new
statement kind that can jump out of a scope, you must decide
whether it needs a drop before the jump.

### §8.4 The early-exit paths

The interesting cases are (2), (3), and (4). All three are
consequences of the same fact: **`OP_RET`, `OP_JMP`, and the try
failure path exit scopes without executing the corresponding
`OP_EXIT_SCOPE`.** The scope stack tracks arena marks; it does not
track RC references. So codegen must emit the drops explicitly.

The `cg_emit_all_rc_drops` and `cg_emit_n_scope_drops` helpers do
this by walking the *compile-time* `RcScope` stack (they do not
modify it — the runtime EXIT_SCOPEs handle that).

The distinction:

- `cg_emit_all_rc_drops` — drops every RC local in every active
  scope. Used by `return` and `?`-failure, which jump out of the
  function entirely.
- `cg_emit_n_scope_drops` — drops the N innermost scopes' RC
  locals. Used by `break` and `continue`, which jump to a loop
  boundary but stay in the function.

Both walk the scopes from innermost to outermost. The order matters
for the same reason it matters in a manual drop sequence: an inner
scope may hold a reference *to* an outer scope's value.

### §8.5 The deep drop

`rc_drop_deep` is invoked from `OP_RC_DEC` when the refcount
reaches zero. It walks the payload to find RC-managed sub-values
and drops them before freeing the outer block.

For a struct `P = struct { s: string }`, when a `rc<P>` drops:

1. `OP_RC_DEC` reads the outer block's header.
2. Refcount reaches zero.
3. `rc_drop_deep` examines the outer's descriptor (`TD_RC` with
   inner `TD_STRUCT`), walks the struct payload, reads the `s`
   field, and calls `rc_drop_deep` on the string.
4. The string's refcount decrements; if zero, it's freed.
5. The struct block is freed.

This is O(fields + elements + payloads) per drop, which for typical
data is small. A deep structure (say a list of a thousand elements
each containing a struct with a string) takes a thousand sub-drops.
That's the cost of automatic memory management without a GC.

### §8.6 What the RC system does not do

- **No cycle detection.** A pair of RC blocks pointing at each other
  will leak. Prism does not provide weak references or a cycle
  collector.
- **No finalizers.** Dropping a block does not call user code.
- **No atomicity.** `RC_INC` and `RC_DEC` are plain reads and writes.
  A multi-threaded program would race. The compiler is single-threaded
  and the runtime provides no synchronization.
- **No relocation.** RC blocks are never moved. Their addresses are
  stable until freed.

---

## §9 — Parameterized opcodes

### §9.1 The design

Most opcodes have a fixed behavior. `OP_PUSH` pushes its operand;
`OP_JMP` jumps to its operand; `OP_ALLOC` allocates the given size.

A few opcodes are parameterized by a **runtime type descriptor**:

```c
typedef enum {
    TD_VOID, TD_I8, TD_I16, TD_I32, TD_I64,
    TD_U8, TD_U16, TD_U32, TD_U64,
    TD_F32, TD_F64, TD_BOOL, TD_STRING,
    TD_REF, TD_RC, TD_RAWPTR, TD_ARRAY,
    TD_STRUCT, TD_EXTERN_STRUCT, TD_ENUM,
    TD_COUNT
} TypeDescKind;

typedef struct {
    TypeDescKind kind;
    uint32_t     aux;
    uint32_t     size;
    uint32_t     count;
} TypeDesc;
```

A descriptor is 16 bytes. `aux` is an index or a start position,
depending on `kind`. `size` is the byte size of a value of this
type. `count` is the number of fields or variants (structs and
enums only).

Descriptors are interned by `typetable_intern` in `types.c`. Two
structurally identical types share one descriptor. An index into
the `TypeTable` is 32 bits.

### §9.2 The parameterized set

These opcodes take a descriptor index as an operand:

| Opcode | Operands | Dispatch on |
|---|---|---|
| `OP_BINARY` | sub-op, desc-idx | desc kind determines numeric interpretation |
| `OP_UNARY` | sub-op, desc-idx | desc kind determines negation type |
| `OP_LEN` | desc-idx | desc kind determines container layout |
| `OP_INDEX_LOAD` | desc-idx | desc kind determines element width |
| `OP_INDEX_STORE` | desc-idx | desc kind determines element width |
| `OP_RC_DEC` | desc-idx | desc kind determines deep-drop walk |
| `OP_CONVERT` | from-desc-idx, to-desc-idx | both determine conversion |

`OP_CCALL` takes a native index; some natives additionally pop a
descriptor index from the stack. `NATIVE_PRINT_VALUE`, for example,
expects the stack to be `[value][desc-idx]` and reads both.

### §9.3 Why descriptors and not opcodes

The alternative design is one opcode per (operation, type)
combination. Under that design, adding a new numeric type (say
`f128`) would require new opcodes for `ADD`, `SUB`, `MUL`, `DIV`,
and every comparison, plus new opcodes for every print, convert,
and index operation. That's roughly twenty new opcodes for one
new type.

With descriptors, adding a new type is a new `TypeDescKind`, a
new case in `desc_kind_of` (`types.c`), and a new case in the
per-opcode dispatch inside `vm_run`. That's roughly five small
changes and no new opcodes.

The trade-off is that the dispatch is two-level: the opcode switch
finds `OP_BINARY`, which then has an inner switch on the descriptor
kind. Adding ~3 branches of indirection per operation. For a VM
that's already an indirect-threaded interpreter, this is invisible.

### §9.4 The interning cost

`typetable_intern` is a linear scan of the descriptor array. For a
program with a hundred distinct types, this is a hundred
comparisons per intern. Interning happens at compile time, not
runtime; the runtime only reads descriptors by index. So the linear
scan cost is borne by the compiler, not the program.

The descriptor array is stored in the `Chunk`, so it's available to
the VM at load time. `Chunk.types` is a `TypeTable`. `codegen.c`
fills it via `desc_of` calls during emission. `vm_init` receives it
as part of `VMConfig` and the dispatch loop reads it on every
parameterized opcode.

### §9.5 Descriptor stability

Descriptors are interned once and never modified. A descriptor
index that a compiled instruction refers to will always refer to
the same descriptor. This is what makes descriptor indices safe
as wire-format operands.

The interning is append-only, so the array grows but never
reorders. A pointer into `tt->items` stays valid across a `intern`
call, but the pointer may be invalidated by the `xrealloc` if the
array grows. Callers that need a stable pointer across intern
calls should hold an index, not a pointer.

---

## §10 — Where to go next

If you want to add an opcode: read `arch/recipes.md` §Adding an
opcode. It walks through all the files you need to touch.

If you want to add an AST node kind: read `arch/recipes.md`
§Adding an AST node kind.

If you want to understand why a particular design choice was made:
read `handbook/decisions.md`.

If you want to find a bug: read `handbook/debugging.md` for the
methodology and `arch/invariants.md` for the enforcement map.

If you want to see the bytecode for a program: run
`manu --quiet=false run <file>` and look at the listing. The
disassembly format is documented in `arch/files/19-disasm.md`.
