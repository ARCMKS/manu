## `docs/arch/files/10-chunk.md`
# `include/chunk.h`, `src/chunk.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

A `Chunk` is the self-contained bytecode artifact. It bundles the
instruction stream and the side tables that make it runnable and
debuggable.

**What it contains.** The code array, the parallel line and
column arrays, the function table, the constant pool, and the
runtime type table.

**What it does not contain.** Source text (only spans via the
tokens in the func table), the `SourceMap` (passed separately to
anything that renders locations), the `TypeDefTable` (compiler-only).

**Who depends on it.** Codegen produces it; the validator and
the disassembler read it; the VM consumes it. Ownership passes
from codegen to the driver (`main.c` or the test harness).

**The self-contained property** is what makes `Chunk` the
dividing line between "compiler" and "runtime". Everything before
the `Chunk` is compiler internals; everything after consumes
only the `Chunk`.

---

## §2 — Public interface

### §2.1 Data structures

```c
typedef struct {
    Token name;
    char  name_buf[128];
    U64   entry_pc;
} FuncEntry;

typedef struct {
    FuncEntry* items;
    int        count;
    int        capacity;
} FuncTable;

typedef struct {
    U64*         code;
    uint32_t*    lines;
    uint32_t*    cols;
    int          count;
    int          capacity;
    ConstantPool pool;
    FuncTable    funcs;
    TypeTable    types;
} Chunk;
```

**`FuncEntry`.** One callable function's entry point.

- `name` — the original source `Token`. Used for span lookups
  in diagnostics (via `smap_span_text`).
- `name_buf` — a printable name. For a regular function this is
  the source name; for an instantiation it is the mangled name
  (`sq#i32`).
- `entry_pc` — the word offset of the function's first
  instruction.

**`Chunk`.** The whole artifact.

- `code[]` — the instruction stream, one `U64` per word.
- `lines[]`, `cols[]` — parallel arrays of debug locations, one
  entry per code word.
- `count` — the length of all three parallel arrays.
- `capacity` — the allocated size.
- `pool` — the constant pool.
- `funcs` — the function entry table.
- `types` — the runtime type descriptor table.

The three parallel arrays always have the same length. Only
`chunk_emit` grows them, and it grows all three together.

### §2.2 Functions

```c
void chunk_init(Chunk* c);
void chunk_free(Chunk* c);
int  chunk_emit(Chunk* c, U64 word, uint32_t line, uint32_t col);
void chunk_patch(Chunk* c, int at, U64 v);
int  chunk_add_func(Chunk* c, Token name, const char* name_str,
                    U64 entry_pc);
```

**`chunk_init`.** Zeroes the chunk and calls `pool_init` and
`typetable_init`. Does not allocate the code/lines/cols triple;
the first `chunk_emit` allocates.

**`chunk_free`.** Frees `code`, `lines`, `cols`, the pool, the
func table, and the type table. Then calls `chunk_init` so the
chunk is reusable.

**`chunk_emit`.** Append one word and its debug location. Grows
the three parallel arrays by doubling from 64. Returns the index
of the word just written. Note that this appends **one word**,
not one instruction; callers that emit multi-word instructions
call it once per word.

**`chunk_patch`.** Overwrite an already-emitted word. No bounds
check; the caller is expected to hold a slot returned by
`chunk_emit`.

**`chunk_add_func`.** Register a function and return its index.
The index is what `OP_CALL` carries. `name` is the source token;
`name_str` is copied into `name_buf` (truncated at 127 bytes by
`snprintf`). The returned index is stable for the chunk's
lifetime (append-only).

---

## §3 — Implementation notes

### §3.1 Growth

`chunk_emit` grows the three parallel arrays in one branch:

```c
if (c->count >= c->capacity) {
    c->capacity = c->capacity ? c->capacity * 2 : 64;
    c->code  = (U64*)     xrealloc(c->code,
                                   sizeof(U64)      * (size_t)c->capacity);
    c->lines = (uint32_t*)xrealloc(c->lines,
                                   sizeof(uint32_t) * (size_t)c->capacity);
    c->cols  = (uint32_t*)xrealloc(c->cols,
                                   sizeof(uint32_t) * (size_t)c->capacity);
}
```

The three arrays are grown together so they cannot get out of
sync. This is the only place that grows them.

### §3.2 Debug information

Every emitted word gets a `(line, col)` pair. Multi-word
instructions duplicate the pair across all their words. This is
what the VM's error renderer wants: any word's location is
available without decoding the instruction's word count.

A chunk with no debug info would have `lines == NULL`. The
current implementation always populates the arrays; there is no
"release mode" that drops them.

### §3.3 Ownership of names

The `FuncEntry.name` token is a source span; it refers into the
`SourceMap`, which the chunk does not own. The map must outlive
the chunk. `chunk_free` does not touch the token.

The `name_buf` is a fixed-size inline array; no heap is involved.

### §3.4 `chunk_patch` and the no-bounds-check decision

`chunk_patch` writes into `code[at]` without checking `at`. The
caller is expected to have held a slot returned by `chunk_emit`.
A buggy patch is a compiler bug, not a source-program bug; the
bounds check would add cost to every patch for no benefit.

### §3.5 The func table's role at runtime

`OP_CALL` reads its operand as an index into `chunk->funcs.items`
and jumps to `items[idx].entry_pc`. The `name` and `name_buf` are
not used by the dispatch loop; they're used by the error renderer
(`main.c`'s `render_runtime_error`) to spell function names in the
call trace.

---

## §4 — Invariants

- `code`, `lines`, and `cols` have the same length, always.
- For every instruction, all of its operand words are inside
  `[0, count)`. A truncated instruction is a validator error,
  not a runtime condition.
- Every `FuncEntry.entry_pc` points at the first word of the
  function's body.
- Every `CALL` operand's index is a valid `funcs.items` index.
- Every `TypeDesc` index in an instruction is a valid index into
  `types`.
- The chunk owns its `code`, `lines`, `cols` arrays, its pool,
  its funcs table, and its types table. It does not own the
  source tokens in `FuncEntry.name`.

---

## §5 — Known limitations

**No jump table instructions.** Each jump is a separate
comparison or an indexed dispatch built from `JMP_IF_FALSE`s.

**Debug info is always emitted.** No way to compile "release"
without lines and cols. The cost is 8 bytes per code word; for a
modest program this is negligible.

**`chunk_patch` has no bounds check.** See §3.4.

**No compression.** The code array is a flat sequence of `U64`
words. There is no bytecode optimization pass that would shrink
it.

**The FuncTable is populated in codegen order.** Function
indices are source-order (with instantiations appended). No
attempt is made to reorder for locality.

**No "named" chunks.** The disassembler takes a name string as a
separate argument; the chunk itself does not carry a name.

**`FuncEntry.name_buf` is a fixed 128 bytes.** Longer mangled
names are truncated.

---

## §6 — Extension points

- **A "release" mode that drops debug info.** Would make
  `lines` and `cols` NULL and skip the corresponding emissions.
  The VM already handles NULL debug arrays.
- **Chunk merging.** Two chunks could be concatenated with
  offset adjustment; not implemented.
- **A name field on the chunk.** Useful for the disassembler's
  header line.
- **Bytecode optimization.** No pass exists today; a peephole
  or constant-folding pass would operate on the `code` array
  before `validate_bytecode`.
- **A `chunk_patch_word_only` variant.** Currently
  `chunk_patch` only patches the code word, not the line/col.
  This is what backpatching wants.
- **Interning of strings across chunks.** Not applicable; a
  compilation has one chunk.
