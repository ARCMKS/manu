// ============================================================
// chunk.h — Bytecode container.
//
// A Chunk bundles the instruction stream and the auxiliary
// tables that make it useful at runtime:
//
//   code[]    the instruction words
//   lines[]   one uint32_t per word: the source line it came from
//   cols[]    one uint32_t per word: the source column
//   funcs[]   function entry table, indexed by func_idx
//   pool      the constant pool (strings, integers, ...)
//   types     runtime type descriptors, indexed by desc_idx
//
// The line and column arrays are debug information, not
// required to execute. A Chunk with lines == NULL runs
// identically; runtime errors simply report less context.
//
// Ownership and lifetime:
//   - A Chunk owns its code, lines, and cols arrays, its pool,
//     its funcs table, and its types table. chunk_free releases
//     all of them.
//   - The tokens stored in FuncEntry.name are spans into a
//     SourceMap the Chunk does not own; the SourceMap must
//     outlive the Chunk.
//   - The TypeDesc indices stored in instructions refer to
//     entries in `types`, which this Chunk owns.
//
// Wire format invariants (relied on by vm.c and disasm.c):
//   - Instruction stream is a flat array of U64 words.
//   - The first word of each instruction is the OpCode.
//     Operands follow as consecutive U64 words.
//   - For every instruction, all of its operand words are
//     inside [0, count). A truncated instruction is a
//     validator error, not a runtime condition.
//   - A CALL word carries a `func_idx` operand that indexes
//     `funcs.items`; the callee's entry_pc is looked up there.
// ============================================================
#ifndef PRISM_CHUNK_H
#define PRISM_CHUNK_H

#include "common.h"
#include "constant.h"
#include "lexer.h"
#include "types.h"

// One function entry in the FuncTable.
typedef struct {
    Token name;              // original source token (for fallback
                             // span lookup; may be a template's name
                             // for an instantiation)
    char  name_buf[128];     // printable name: source name for regular
                             // functions, mangled name ("sq#i32") for
                             // instantiations
    U64   entry_pc;
} FuncEntry;

// Growable array of FuncEntry. Growth is doubling (see
// chunk_add_func in chunk.c). Index into `items` is the
// func_idx that OP_CALL carries.
typedef struct {
    FuncEntry* items;
    int        count;
    int        capacity;
} FuncTable;

// The whole compiled artifact.
//
// Three parallel arrays (code, lines, cols) are grown together
// and must always have the same `count`. The invariant is
// maintained by chunk_emit, which is the only writer.
//
// `pool` holds literals referenced by OP_LOAD_CONST and by
// OP_TRAP (trap messages). `funcs` holds every callable
// function's entry point. `types` holds the runtime type
// descriptors that parameterized opcodes (OP_BINARY, OP_PRINT,
// OP_INDEX_LOAD, OP_INDEX_STORE, OP_LEN, OP_CONVERT) reference
// by index.
//
// Note that Chunk does not contain the source text: it only
// stores spans (as Tokens) and line/col arrays. The SourceMap
// is passed alongside it to anything that needs to render a
// location.
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

// Zero a Chunk. Safe on uninitialized stack storage. Does not
// allocate; the first emit will allocate the code/lines/cols
// triple.
void chunk_init(Chunk* c);

// Free every owned sub-array and reset the Chunk to its zero
// state. The reset means the Chunk is usable again after
// chunk_free without a separate init, matching the pattern
// used by diagnostic.c and source_map.c.
void chunk_free(Chunk* c);

// Append one instruction word and return its index. The `line`
// and `col` arguments are debug info; they are copied into the
// parallel arrays and read by the VM's error renderer and by
// disasm.
//
// Note: this appends *one word*, not one instruction. Callers
// that emit multi-word instructions call it once per word;
// there is no "instruction" abstraction at this layer. The
// (line, col) pair is therefore duplicated across an
// instruction's words, which is what the error renderer wants.
int  chunk_emit(Chunk* c, U64 word, uint32_t line, uint32_t col);

// Overwrite an already-emitted word. `at` is a word index into
// code[], as returned by chunk_emit. No bounds check.
//
// Used to backpatch jump and call targets once the destination
// PC is known. The caller is responsible for making sure the
// slot it patches was emitted as a placeholder (see
// emit_placeholder in codegen.c).
void chunk_patch(Chunk* c, int at, U64 v);

// Register a function in the FuncTable and return its index
// (the func_idx that OP_CALL will carry). `entry_pc` is where
// the function's first instruction lives in code[].
//
// `name` is the source Token (used for span lookups in
// diagnostics). `name_str` is the printable name — for
// regular functions the same as the token's text, for
// instantiations the mangled name. It is copied into the
// FuncEntry; the caller keeps ownership of its own string.
int  chunk_add_func(Chunk* c, Token name, const char* name_str,
                    U64 entry_pc);

#endif // PRISM_CHUNK_H