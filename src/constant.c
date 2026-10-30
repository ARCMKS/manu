// ============================================================
// constant.c — Constant pool and compile-time constant table.
//
// Two structures, both holding compile-time-known values, but
// with different lifetimes and purposes:
//
//   ConstantPool  Owned by a Chunk. Runtime artifact. Strings
//                 are copied into the pool and freed when the
//                 pool is freed.
//
//   ConstTable    Owned by the caller (main.c, a test, or a
//                 future driver). Compile-time symbol table.
//                 Entries are registered by sema and read by
//                 codegen.
//
// This file does not own diagnostic reporting beyond the two
// cases explicitly documented on `consts_declare`; the callers
// decide what to say and where.
// ============================================================
#include "../include/constant.h"
#include "../include/source_map.h"
#include <stdlib.h>
#include <string.h>

// ============================================================
// Runtime constant pool
// ============================================================

// Zero a pool. No allocation until the first pool_add_*.
void pool_init(ConstantPool* p) {
    p->items    = NULL;
    p->count    = 0;
    p->capacity = 0;
}

// Free every owned string and the items array, then reset.
//
// Only CONST_STRING and CONST_CSTRING carry a heap-owned
// `bytes` pointer. I64/F64/BOOL are inline; freeing them would
// be a bug. The check is on `kind`, not on the presence of a
// pointer, because a zero-valued Constant would otherwise look
// like it had a NULL `bytes` and could be skipped harmlessly —
// which is fine but misleading. Being explicit is clearer.
void pool_free(ConstantPool* p) {
    for (int i = 0; i < p->count; i++) {
        if (p->items[i].kind == CONST_STRING
            || p->items[i].kind == CONST_CSTRING)
            free(p->items[i].as.str.bytes);
    }
    free(p->items);
    pool_init(p);
}

// Grow the pool if needed. Returns the index of the next slot
// without writing to it; the caller fills the entry and returns.
//
// Note that this does NOT increment count. The caller is
// responsible for that after it has filled the entry. This
// split exists so that a future change that needs to inspect
// the slot before committing (e.g. deduplication) can do so
// without having to roll back a count increment on the failure
// path.
static int pool_reserve(ConstantPool* p) {
    if (p->count >= p->capacity) {
        p->capacity = p->capacity ? p->capacity * 2 : 16;
        p->items = (Constant*)xrealloc(
            p->items, sizeof(Constant) * (size_t)p->capacity);
    }
    return p->count;
}

int pool_add_i64(ConstantPool* p, I64 v) {
    int idx = pool_reserve(p);
    Constant* c = &p->items[idx];
    c->kind   = CONST_I64;
    c->as.i64 = v;
    p->count++;
    return idx;
}
// Float constants are stored by value; the VM reconstitutes
// the bit pattern from `as.f64` when it loads them (see
// OP_LOAD_CONST in vm.c). This is why there is no "float bits"
// vs "float value" distinction here — the union member is
// typed as `F64`, and codegen writes the double directly.
int pool_add_f64(ConstantPool *p, double v){
    int idx = pool_reserve(p);
    Constant* c = &p->items[idx];
    c->kind = CONST_F64;
    c->as.f64 = v;
    p->count++;
    return idx;
}
int pool_add_bool(ConstantPool* p, bool v) {
    int idx = pool_reserve(p);
    Constant* c = &p->items[idx];
    c->kind = CONST_BOOL;
    c->as.b = v;
    p->count++;
    return idx;
}

// The bytes are copied. The pool keeps a NUL-terminated copy for
// the convenience of the disassembler and the VM's print handler,
// even though the stored `length` is authoritative (the string
// may contain embedded NUL bytes).
//
// The extra NUL byte allocated past `length` is *not* part of
// the string; it is a sentinel that makes the pool's copy safe
// to pass to C APIs that expect a C string. The VM will build
// its own runtime representation (RC block or C-string buffer)
// from the authoritative bytes, not from this NUL-terminated
// copy.
int pool_add_string(ConstantPool* p, const char* bytes, uint32_t length) {
    int idx = pool_reserve(p);
    Constant* c = &p->items[idx];
    c->kind = CONST_STRING;

    c->as.str.bytes = (char*)xmalloc((size_t)length + 1);
    if (length > 0) memcpy(c->as.str.bytes, bytes, length);
    c->as.str.bytes[length] = '\0';
    c->as.str.length = length;

    p->count++;
    return idx;
}

// Same as pool_add_string but for the `c"..."` form. The bytes
// stored are the same bytes; the kind is what tells the VM to
// materialize a bare C-string in the const arena rather than an
// RC block.
int pool_add_cstring(ConstantPool* p, const char* bytes, uint32_t length) {
    int idx = pool_reserve(p);
    Constant* c = &p->items[idx];
    c->kind = CONST_CSTRING;
    c->as.str.bytes = (char*)xmalloc((size_t)length + 1);
    if (length > 0) memcpy(c->as.str.bytes, bytes, length);
    c->as.str.bytes[length] = '\0';
    c->as.str.length = length;
    p->count++;
    return idx;
}

// Bounds-checked lookup. Returns NULL for a NULL pool, a
// negative index, or an out-of-range index. Callers that
// want to report a missing entry do so themselves.
const Constant* pool_get(const ConstantPool* p, int idx) {
    if (!p || idx < 0 || idx >= p->count) return NULL;
    return &p->items[idx];
}

// Human-readable name of a constant kind. Used by
// diagnostics and by the disassembler's pool dump. The
// trailing `return "?"` after the switch is defensive for
// a future kind added to the enum without a matching case.
const char* const_kind_name(ConstKind k) {
    switch (k) {
        case CONST_I64:    return "i64";
        case CONST_F64:    return "f64";
        case CONST_BOOL:   return "bool";
        case CONST_STRING: return "string";
        case CONST_CSTRING: return "cstring";
            
    }
    return "?";
}

// ============================================================
// Compile-time constant table
// ============================================================

// Zero the count. The entries array itself is *not* cleared:
// entries past `count` are dead, and the array is stack- or
// caller-owned, so memsetting 256*sizeof(ConstEntry) bytes
// would be a needless cost. If a caller reads entries[i] for
// i >= count that is a bug; the API is count-bounded.
void consts_init(ConstTable* c) {
    c->count = 0;
}

// Register a new constant.
//
// Check order:
//   1. Duplicate. This is a user-facing error and is reported
//      as -2 even if the table is also full, because the
//      duplicate is the more actionable problem. The check
//      is a linear scan of the existing names via
//      smap_tokens_equal, exactly like local and function
//      lookup.
//   2. Fullness. -1 if count >= MAX_CONSTS.
//   3. Otherwise, append and return the new index.
//
// The value is copied into the entry by value (Constant is a
// POD union). For string constants this means the *pointer* is
// copied, not the bytes: the caller retains ownership of the
// bytes. This is documented on the header but worth repeating:
// if a future caller ever registers a string constant, it must
// not free the bytes until the ConstTable is done with them.
int consts_declare(ConstTable* c, Token name, Constant value,
                   Type* type, const SourceMap* smap) {
    // Duplicate check first: a redeclaration is a user error and
    // should be reported as such, even if the table happens to be
    // full at the same moment.
    for (int i = 0; i < c->count; i++) {
        if (smap_tokens_equal(smap, c->entries[i].name, name))
            return -2;
    }

    if (c->count >= MAX_CONSTS) return -1;

    int idx = c->count;
    c->entries[idx].name  = name;
    c->entries[idx].value = value;
    c->entries[idx].type  = type;
    c->count++;
    return idx;
}

// Find a constant by name. Linear scan; returns -1 if not
// found. Used by sema to resolve a bare identifier that is
// not a local, and by codegen to look up a const's value
// when inlining it (the AST node carries the index, so
// codegen actually uses consts_get, not consts_resolve; the
// two coexist because sema and codegen need different
// lookups).
int consts_resolve(const ConstTable* c, const SourceMap* smap, Token name) {
    for (int i = 0; i < c->count; i++) {
        if (smap_tokens_equal(smap, c->entries[i].name, name))
            return i;
    }
    return -1;
}

// Bounds-checked get by index. Returns NULL for a NULL table
// or an out-of-range index.
const ConstEntry* consts_get(const ConstTable* c, int idx) {
    if (!c || idx < 0 || idx >= c->count) return NULL;
    return &c->entries[idx];
}