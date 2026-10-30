## `docs/arch/files/09-constant.md`

# `include/constant.h`, `src/constant.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

Two distinct structures share the "constant" name.

**`ConstantPool`** is a runtime artifact owned by a `Chunk`. It
holds the literal values referenced by `OP_LOAD_CONST`: integers,
floats, booleans, Prism strings, and C strings. Strings live here
because they don't fit in an instruction word; integers and
booleans are inline-able but occasionally flow through the pool
anyway (as print separators, for example).

**`ConstTable`** is a compile-time symbol table. It maps `const
NAME = literal;` declarations to a `Constant` value and a `Type`.
Sema populates it; codegen reads it to inline the value at every
use site. A `const` costs nothing at runtime.

**What they do not share.** A `const` declared in source ends up
in the `ConstTable` (indexed by name, resolved by sema) and, if
used, as an inline immediate in the bytecode. It does **not**
appear in the `ConstantPool`. The pool is only for values that
codegen could not inline: multi-byte literals (strings,
cstrings), and any operand word that was easier to pool than to
encode.

**Who depends on them.** Sema (registers constants in the
`ConstTable`), codegen (reads the `ConstTable`, adds entries to
the `ConstantPool`), and the VM (reads the pool at
`OP_LOAD_CONST` and at `vm_init` for string materialization).

---

## §2 — Public interface

### §2.1 Constant values

```c
typedef enum {
    CONST_I64,
    CONST_F64,
    CONST_BOOL,
    CONST_STRING,
    CONST_CSTRING,
} ConstKind;

typedef struct {
    ConstKind kind;
    union {
        I64  i64;
        F64  f64;
        bool b;
        struct {
            char*    bytes;
            uint32_t length;
        } str;
    } as;
} Constant;
```

Five kinds. The `str` arm is used by both `CONST_STRING` and
`CONST_CSTRING`. `length` is authoritative; `bytes` is
NUL-terminated at index `length` as a convenience for the
disassembler and the VM's print handler, but the NUL is not
counted and the string may contain embedded NULs.

### §2.2 Runtime constant pool

```c
typedef struct {
    Constant* items;
    int       count;
    int       capacity;
} ConstantPool;

void pool_init(ConstantPool* p);
void pool_free(ConstantPool* p);
int  pool_add_i64(ConstantPool* p, I64 v);
int  pool_add_f64(ConstantPool* p, double v);
int  pool_add_bool(ConstantPool* p, bool v);
int  pool_add_string(ConstantPool* p, const char* bytes, uint32_t length);
int  pool_add_cstring(ConstantPool* p, const char* bytes, uint32_t length);
const Constant* pool_get(const ConstantPool* p, int idx);
const char* const_kind_name(ConstKind k);
```

**`pool_init`, `pool_free`.** Zero-initialize and release. The
free loop releases each `CONST_STRING` and `CONST_CSTRING`'s
`bytes` pointer, then the items array.

**`pool_add_*`.** Append and return the new index. Growth is
doubling from 16. There is **no deduplication**: adding the same
string twice creates two pool entries.

**`pool_get`.** Bounds-checked; returns NULL for a NULL pool, a
negative index, or an out-of-range index.

**`const_kind_name`.** Human-readable name for a `ConstKind`.

### §2.3 Compile-time constant table

```c
#define MAX_CONSTS 256

typedef struct {
    Token    name;
    Constant value;
    Type*    type;
} ConstEntry;

typedef struct {
    ConstEntry entries[MAX_CONSTS];
    int        count;
} ConstTable;

void consts_init(ConstTable* c);
int  consts_declare(ConstTable* c, Token name, Constant value,
                    Type* type, const SourceMap* smap);
int  consts_resolve(const ConstTable* c, const SourceMap* smap, Token name);
const ConstEntry* consts_get(const ConstTable* c, int idx);
```

**`consts_init`.** Zeroes `count`. Does not clear the entries
array; entries past `count` are dead.

**`consts_declare`.** Register a new constant. Returns the
entry's index on success, `-1` if the table is full, `-2` if a
constant with the same name has already been declared.

The **duplicate check runs before the full check**, so a
redeclaration is reported as a duplicate even in a full table.

**`consts_resolve`.** Find a constant by name. Linear scan;
returns `-1` if not found.

**`consts_get`.** Bounds-checked; returns NULL for a NULL table
or an out-of-range index.

---

## §3 — Implementation notes

### §3.1 The pool's reserve-then-commit pattern

`pool_add_*` uses a helper:

```c
static int pool_reserve(ConstantPool* p) {
    if (p->count >= p->capacity) {
        p->capacity = p->capacity ? p->capacity * 2 : 16;
        p->items = (Constant*)xrealloc(
            p->items, sizeof(Constant) * (size_t)p->capacity);
    }
    return p->count;
}
```

The helper does not increment `count`. The caller fills the entry
and then increments. The split exists so a future change that
wanted to inspect the slot before committing (for deduplication)
could do so without rolling back on a failure path.

### §3.2 String storage

`pool_add_string` and `pool_add_cstring` copy the caller's bytes
into a fresh allocation of `length + 1` bytes, NUL-terminate,
and store the pointer. The pool owns the bytes; `pool_free`
frees them.

The two functions differ only in the `kind` they set. The VM
uses the kind to decide the runtime layout (RC-shaped block for
strings, bare NUL-terminated buffer for cstrings).

### §3.3 Name comparison

`consts_declare` and `consts_resolve` compare names via
`smap_tokens_equal`, which is byte-based and file-agnostic. Two
`const` declarations in different files with the same name
collide; there is no per-file scoping for constants.

### §3.4 `ConstTable` is fixed-size

Unlike the pool (which grows) and every other table in the
compiler (which grows), the `ConstTable` is a fixed array of
`MAX_CONSTS = 256` entries. This is deliberate: the table is
small, the fixpoint is easy to reason about, and the growth would
be a one-line change if ever needed.

Overflow is diagnosed, not silently truncated.

---

## §4 — Invariants

- `ConstantPool.count <= ConstantPool.capacity`.
- Every entry in `[0, count)` has a valid `kind` and a value
  appropriate to it.
- For `CONST_STRING` and `CONST_CSTRING`, `bytes` is a heap
  pointer owned by the pool, `bytes[length] == '\0'`, and `length`
  reflects the actual byte count (which may differ from
  `strlen(bytes)` if the string contains embedded NULs).
- Every non-`CONST_STRING`/`CONST_CSTRING` entry has no owned
  heap.
- `ConstTable.count <= MAX_CONSTS`.
- Every entry in `[0, count)` has a non-NULL `type` and a valid
  `value`.
- The `name` of every entry is a `Token` whose span refers to a
  file registered with the `SourceMap` used at declaration time.

---

## §5 — Known limitations

**No string constants.** `const S = "hi";` is rejected by sema
with a dedicated message. The reason is that a string constant
would need to be materialized as an arena block at runtime,
which codegen does not do for constants.

**No arithmetic initializers.** `const X = 1 + 2;` is rejected.
The initializer must be a literal (integer, float, or boolean).

**No per-file scoping.** Duplicate names across files are errors.

**No selective import of constants.** A `using lib.{ X }` where
`X` is a constant has no effect; the resolver does not consult
the constant table for imported names.

**Fixed `MAX_CONSTS = 256`.** A program with more constants is
rejected. In practice the number of `const` declarations is
small.

**No pool deduplication.** `pool_add_string(p, "x", 1)` twice
creates two entries. Not a correctness issue; a performance and
memory concern at scale.

**`const_kind_name` has no default case.** The switch covers
every `ConstKind`; a future kind added without a case would fall
through to the trailing `return "?"`.

**`consts_init` does not clear the entries array.** A caller that
reads `entries[i]` for `i >= count` reads stale data. The API is
`count`-bounded; no caller does this.

---

## §6 — Extension points

- **String constants.** Requires codegen to materialize a
  `CONST_STRING` at a use site into an arena block (or to
  materialize at VM init and reference it via `const_ptrs`).
- **Constant folding.** A `const_fold(AstNode*) -> Constant`
  function would let arithmetic initializers work. The
  const-from-literal code in `sema.c` is the natural extension
  point.
- **Per-file constant scoping.** Would require `ConstEntry` to
  carry a `file_id` and `consts_resolve` to take one.
- **Selective import of constants.** Would require the resolver
  to consult the constant table for imported names.
- **A `consts_free`.** Not needed; the table has no owned heap.
- **Deduplication in the pool.** A hash or a linear scan before
  `pool_reserve` would eliminate duplicate entries. Adds
  complexity for a rare case.
