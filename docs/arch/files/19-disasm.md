# Documentation drafting — Session 12

Six short file references, one per remaining source file.

---

## `docs/arch/files/19-disasm.md`

```markdown
# `include/disasm.h`, `src/disasm.c`

*Current as of commit `<hash>`, <date>.*

---

## §1 — Purpose

Two inspection tools that operate on a `Chunk`:

- `disassemble` — a human-readable listing. Debug tool.
- `validate_bytecode` — a structural check run between codegen and
  execution.

`OPINFO[]`, the opcode metadata table, is the single source of
truth for opcode names and operand counts. Both tools read from it.

**Called from:** `main.c` (before execution) and the test harness.

---

## §2 — Public interface

```c
void disassemble(const Chunk* chunk, const SourceMap* smap,
                 const char* file_path, const char* name);

int  validate_bytecode(const U64* code, size_t length,
                       const ConstantPool* pool,
                       const FuncTable* funcs,
                       const TypeTable* types);

const char* disasm_op_name(int op);
```

**`disassemble`.** Prints a full listing to stdout: header, pool
dump, and decoded instruction stream. `smap` is used only to
recover function names from `FuncEntry.name_buf` when the buffer
is empty (defensive; codegen always fills it). `file_path` and
`name` are cosmetic.

**`validate_bytecode`.** Returns 0 on success, -1 on the first
structural violation (also printed to stderr). `pool`, `funcs`,
and `types` may be NULL; the corresponding checks are skipped.

**`disasm_op_name`.** Returns `"<unknown>"` for an out-of-range
opcode, `"<unnamed-op>"` for an unlisted opcode.

---

## §3 — `OPINFO`

```c
typedef struct {
    const char* name;
    int         operands;
} OpInfo;

static const OpInfo OPINFO[OP_COUNT] = { ... };
```

Designated initializers, one row per opcode. `operands` is the
**word count** after the opcode, not the number of logical
operands. A `CALL` has two words (`func_idx`, `argc`); a
`LOAD_INDIRECT` has two (`offset`, `mem_kind`); a `PUSH` has one
(the value).

Every opcode in the `OpCode` enum has a row. A missing row would
give the opcode `name == NULL` and `operands == 0`; the listing
would mis-step and the validator would miss operand checks.

**The `OP_CCALL` row is present.** An earlier revision lacked it;
the current code has it. Historical note only.

---

## §4 — Implementation notes

### §4.1 `operand_count`

```c
static int operand_count(int op) {
    if (op < 0 || op >= OP_COUNT) return 0;
    return OPINFO[op].operands;
}
```

The stride used by both `disassemble` and `validate_bytecode`.
An out-of-range opcode yields 0 (the disassembler renders the
opcode word as an "operand" in that case; harmless).

### §4.2 The listing

`disassemble` walks `code` with `i += 1 + operand_count(op)`.
Per instruction:

- **Location column**: reads `lines[i]` and `cols[i]`. If
  `lines[i] == 0` (no debug info), prints `<none>`.
- **Truncation guard**: if the operands would extend past
  `length`, prints `<truncated>` and stops.
- **Operand rendering**: a switch over the opcode. Handles
  `PUSH` (decimal), locals (slot numbers), indirect access
  (`offset`, `mem_kind`), `LOAD_CONST` (pool index + a comment
  with the pooled value), `LEN`/`INDEX_*`/`BINARY`/`UNARY`/
  `RC_DEC`/`CONVERT` (descriptors, rendered by `print_desc`),
  jumps (target pc), `CALL` (callee's printable name + argc),
  `CCALL` (native name), `FFI_CALL` (ffi index + argc),
  `ALLOC`/`ALLOC_RC` (size), `NEW_ARRAY`/`NEW_RC_ARRAY`/
  `NEW_ENUM`/`NEW_RC_ENUM` (count), `TRAP` (pool index +
  message).

**`print_desc`** renders a `TypeDesc` recursively. Structs and
enums render as `struct#N` and `enum` because their field and
variant tables aren't inline. The recursion has no cycle
detection; the interned `TypeTable` is acyclic.

**HALT does not stop the listing.** Dead code past a HALT is
worth seeing. The listing prints `(--- N words past this HALT
---)` and continues.

**Pool dump** is on by default (`debug_dump_pool = true`); a raw
hex dump is off (`debug_dump_raw = false`). Both are
compile-time constants.

### §4.3 The validator

Walks the code with the same stride. Per instruction:

- Opcode range check: `raw < OP_COUNT`.
- Truncation check: `i + 1 + n <= length`.
- Per-opcode checks (a switch): jumps land inside `[0, length)`;
  `CALL`'s `func_idx` is inside `funcs->count`; `CALL_INDIRECT`'s
  argc is at most 8; `FFI_CALL`'s argc is at most 16;
  `LOAD_CONST`'s pool index is inside the pool; descriptor
  operands (via `typetable_get`) resolve; `LOAD_INDIRECT`/
  `STORE_INDIRECT`'s `mem_kind` is inside `MEM_COUNT`;
  `BINARY`/`UNARY`'s sub-op is inside `OPB_COUNT`/`OPU_COUNT`;
  `ALLOC`/`ALLOC_RC`'s size is nonzero; `TRAP`'s pool index
  resolves; `TRY_JUMP_IF_OK`'s target is inside the code.

Returns -1 on the first violation, printing the pc and the
reason. After one violation the stride may be wrong, so the
validator stops rather than producing noise.

### §4.4 What the validator does not check

- Stack balance (would require a CFG walk).
- Type consistency (descriptor operands' shapes vs. surrounding
  code).
- `TypeTable` internal consistency (field and variant index
  ranges).
- Data-stack depth at function boundaries.
- That every emitted function body ends in a `RET`.

---

## §5 — Invariants

- Every `OpCode` value has an `OPINFO` row.
- `OPINFO[op].operands` matches the number of operand words the
  `vm_run` dispatch reads for `op`.
- `disassemble` advances by exactly `1 + operand_count(op)`.
- `validate_bytecode` returns 0 only if every check passed.

---

## §6 — Known limitations

**`OPINFO` is not derived from the code.** Adding an opcode to
`vm.h` requires a manual `OPINFO` row and a validator case.
Forgetting either produces silent misbehavior: the opcode is
listed as unnamed with zero operands, and the validator does not
check its operands.

**The listing's operand rendering is a large switch.** It is
not table-driven like `OPINFO`; a new opcode's operand rendering
must be added by hand.

**`disassemble` prints to stdout.** No `FILE*` parameter. Tests
that want to inspect a listing capture stdout at the process
level.

**No structured output.** The listing is plain text; there is no
JSON or machine-readable form.

**No disassembly of a single function.** The whole chunk is
listed. A future `disassemble_func` would help debug large
programs.

**No branch-target annotation.** A jump's target is printed as a
pc; there is no `-> 0x1234 <label>` form.

**`print_desc` has no cycle detection.** The interned table is
acyclic; a future non-interned descriptor would infinite-recurse.

---

## §7 — Extension points

- **A `FILE*` out parameter on `disassemble`.** Would let tests
  capture listings.
- **Structured output.** A `--json` flag would emit machine-
  readable form.
- **Function-scoped listing.** A `disassemble_func(chunk, idx)`
  would list one function's body plus a header.
- **Table-driven operand rendering.** Same shape as `OPINFO`;
  a `{ name, render }` pair per opcode.
- **Cycle detection in `print_desc`.** A depth limit or a
  visited set.
```