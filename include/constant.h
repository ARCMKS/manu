// ============================================================
// constant.h — Compile-time values.
//
// Two distinct but related structures:
//
//   ConstantPool  Runtime artifact, owned by a Chunk. Holds the
//                 literals (ints, bools, strings) that bytecode
//                 references by index via OP_LOAD_CONST. Strings
//                 live here because they can't fit in an
//                 instruction word; ints and bools are inline-able
//                 but occasionally flow through the pool anyway
//                 (e.g. as print separators).
//
//   ConstTable    Compile-time symbol table. Maps identifier names
//                 introduced by `const NAME = literal;` to their
//                 Constant values. Sema populates it; codegen reads
//                 it when a const name appears in an expression and
//                 inlines the literal value directly, producing no
//                 runtime load.
//
// A const is not a variable. `const MAX = 100;` costs nothing at
// runtime; every use of `MAX` compiles to a PUSH 100.
//
// The two structures do not share storage. A `const` declared
// in the source ends up in the ConstTable (indexed by name,
// resolved by sema) *and*, if it is ever used, as an inline
// immediate in the bytecode. It does not appear in the
// ConstantPool at all, because the compiler emits the value
// directly. The ConstantPool is only for values that codegen
// could not inline: multi-byte literals (strings, cstrings),
// and any operand word that was easier to pool than to encode.
// ============================================================
#ifndef PRISM_CONSTANT_H
#define PRISM_CONSTANT_H

#include "common.h"
#include "lexer.h"
#include "source_map.h"
#include "types.h"

// ============================================================
// Constant values
// ============================================================

// Discriminant for Constant.as.
//   CONST_I64     — signed 64-bit integer.
//   CONST_F64     — 64-bit float.
//   CONST_BOOL    — true / false.
//   CONST_STRING  — Prism string literal. RC-managed at
//                   runtime; the pool copies the bytes and
//                   the VM materializes them into the const
//                   arena with a sentinel refcount.
//   CONST_CSTRING — C string literal (`c"..."`). Not RC-
//                   managed; the VM materializes a NUL-
//                   terminated buffer in the const arena.
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
        // The string arm is used by both CONST_STRING and
        // CONST_CSTRING. `length` is authoritative; `bytes` is
        // always NUL-terminated at index `length` as a
        // convenience for the disassembler and the VM's print
        // handler, but the NUL is not counted and the string
        // may contain embedded NULs (in which case the extra
        // terminator is the only one visible to C-string APIs).
        struct {
            char*    bytes;    // owned by the pool, NUL-terminated
            uint32_t length;   // byte count, may include embedded NULs
        } str;
    } as;
} Constant;

// ============================================================
// Runtime constant pool
//
// Owned by a Chunk. Grown by codegen; consumed by the VM at
// LOAD_CONST and for string materialization at VM initialization.
// Freed with the chunk.
//
// The pool is a flat array of Constants. Indices into it are
// what OP_LOAD_CONST and OP_TRAP carry as their single
// operand. The VM never re-sorts or deduplicates the pool;
// whatever order codegen put entries in is the order the VM
// reads them.
// ============================================================
typedef struct {
    Constant* items;
    int       count;
    int       capacity;
} ConstantPool;

void pool_init(ConstantPool* p);
void pool_free(ConstantPool* p);

// Append a constant. Strings are copied into the pool's heap.
// Returns the index of the newly-added entry.
//
// There is no deduplication: adding the same string twice
// creates two pool entries. That is fine for the current
// compiler (codegen rarely repeats a literal), but is worth
// knowing if the pool ever grows large.
int  pool_add_i64(ConstantPool* p, I64 v);
int pool_add_f64(ConstantPool* p, double v);
int  pool_add_bool(ConstantPool* p, bool v);
int  pool_add_string(ConstantPool* p, const char* bytes, uint32_t length);
int  pool_add_cstring(ConstantPool* p, const char* bytes, uint32_t length);

// Bounds-checked lookup. Returns NULL for an invalid index; the
// callers that need a diagnostic check for NULL and report.
const Constant* pool_get(const ConstantPool* p, int idx);

// For diagnostics and disassembly only.
const char* const_kind_name(ConstKind k);

// ============================================================
// Compile-time named constants
//
// Session-independent: a const table is created per compilation
// by the caller (main.c or a test) and passed to both sema and
// codegen. Sema appends entries as it discovers `const`
// declarations; codegen reads entries by index from AST nodes
// that carry `const_id`.
//
// Names are compared by span through the SourceMap, exactly as
// locals and functions are. Duplicate names within a compilation
// are rejected at declaration time.
//
// The table is a *fixed-size* array. MAX_CONSTS is a hard cap;
// consts_declare returns -1 when full. This is unlike the other
// growable tables in the compiler, because the const table is
// small and because the fixpoint is easy to reason about. If
// it ever needs to grow, that is a one-line change to the
// struct and a small change to consts_declare.
// ============================================================
#define MAX_CONSTS 256

typedef struct {
    Token    name;
    Constant value;
    Type*    type;    // singletons: ty_i64() or ty_bool()
} ConstEntry;

typedef struct {
    ConstEntry entries[MAX_CONSTS];
    int        count;
} ConstTable;

void consts_init(ConstTable* c);

// Register a new constant. Returns the entry's index on success.
// Returns -1 if the table is full.
// Returns -2 if a constant with the same name has already been
// declared in this compilation; the caller reports the duplicate
// with a source position.
//
// `smap` is used only for name comparison. Ownership of `value`'s
// string bytes (if any) transfers to the caller; `consts` itself
// does not free them. In practice, string constants are
// rejected by sema today, so this contract is not currently
// exercised.
//
// The order of the two failure checks matters: duplicate is
// checked *before* fullness. A caller that ignores the return
// value will therefore see -2 for a duplicate even if the
// table happens to be full, which is the more useful
// diagnostic.
int  consts_declare(ConstTable* c, Token name, Constant value,
                    Type* type, const SourceMap* smap);

// Find the index of a named constant. Returns -1 if not found.
// Linear scan; the table is small.
int  consts_resolve(const ConstTable* c, const SourceMap* smap, Token name);

// Bounds-checked lookup. Returns NULL for an invalid index.
const ConstEntry* consts_get(const ConstTable* c, int idx);

#endif // PRISM_CONSTANT_H