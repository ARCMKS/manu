// ============================================================
// chunk.c — Bytecode container implementation.
//
// Three responsibilities:
//   1. Growth and ownership of the code/lines/cols triple.
//   2. The constant pool and type table are initialized here
//      (they belong to the Chunk) but their own state is
//      managed by constant.c and types.c.
//   3. FuncTable growth.
//
// The whole file is allocation-and-bookkeeping; there is no
// instruction encoding or decoding here. That is codegen's
// (for emit) and disasm's (for decode) job. This file only
// knows that a chunk is a growable array of U64 words and a
// couple of side tables.
// ============================================================
#include "../include/chunk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Zero the chunk and its sub-structures. The parallel arrays
// start NULL/0; the first chunk_emit will allocate. The funcs
// table is also NULL/0; the first chunk_add_func will allocate.
// The pool and type table are zeroed through their own init
// functions so that the growth policies stay in one place.
void chunk_init(Chunk* c) {
    c->code     = NULL;
    c->lines    = NULL;
    c->cols     = NULL;
    c->count    = 0;
    c->capacity = 0;
    pool_init(&c->pool);

    c->funcs.items    = NULL;
    c->funcs.count    = 0;
    c->funcs.capacity = 0;

    typetable_init(&c->types);
}

// Release everything and reset. Order matters only insofar as
// each free must be paired with the allocation that produced
// it; the code, lines, and cols arrays are independent
// allocations, and the pool and type table free their own
// sub-arrays. chunk_init at the end restores the zero state,
// so the chunk is safe to reuse.
void chunk_free(Chunk* c) {
    free(c->code);
    free(c->lines);
    free(c->cols);
    pool_free(&c->pool);
    free(c->funcs.items);
    typetable_free(&c->types);
    chunk_init(c);
}

// Append one instruction word with its debug location.
//
// Growth: start at 64, double when full. The three parallel
// arrays are grown together in the same branch so they can
// never get out of sync. This is the *only* place that grows
// them; any future change to the layout must preserve that
// property or the parallel-array invariant is lost.
//
// Returns the index of the word just written, which is what
// chunk_patch expects. Callers that need to patch a
// placeholder use this return value (or the one from the
// immediately-following emit, for two-word placeholders) to
// record the slot.
int chunk_emit(Chunk* c, U64 word, uint32_t line, uint32_t col) {
    if (c->count >= c->capacity) {
        c->capacity = c->capacity ? c->capacity * 2 : 64;
        c->code  = (U64*)     xrealloc(c->code,
                                       sizeof(U64)      * (size_t)c->capacity);
        c->lines = (uint32_t*)xrealloc(c->lines,
                                       sizeof(uint32_t) * (size_t)c->capacity);
        c->cols  = (uint32_t*)xrealloc(c->cols,
                                       sizeof(uint32_t) * (size_t)c->capacity);
    }
    c->code[c->count]  = word;
    c->lines[c->count] = line;
    c->cols[c->count]  = col;
    return c->count++;
}

// Patch one already-emitted word. No bounds check: the caller
// is expected to have held onto a slot returned by chunk_emit
// and to be patching exactly that slot. A wrong `at` will
// silently corrupt the code array; a defensive bounds check
// here would be more expensive than it is worth, since a
// buggy patch is a compiler bug, not a source-program bug.
void chunk_patch(Chunk* c, int at, U64 v) { c->code[at] = v; }

// Register a function and return its func_idx.
//
// Growth: doubling from 8, same policy as everywhere else in
// the compiler. The function's name is copied by value (it is
// a Token, so it is just a span); no heap is involved.
//
// The returned index is stable for the lifetime of the chunk
// because the funcs table is append-only. codegen emits CALL
// instructions with this index as the operand; the VM looks it
// up in chunk->funcs.items at call time.
int chunk_add_func(Chunk* c, Token name, const char* name_str,
                   U64 entry_pc) {
    if (c->funcs.count >= c->funcs.capacity) {
        c->funcs.capacity = c->funcs.capacity
                          ? c->funcs.capacity * 2 : 8;
        c->funcs.items = (FuncEntry*)xrealloc(
            c->funcs.items,
            sizeof(FuncEntry) * (size_t)c->funcs.capacity);
    }
    int idx = c->funcs.count;
    FuncEntry* e = &c->funcs.items[idx];
    e->name     = name;
    e->entry_pc = entry_pc;
    // Copy the printable name with truncation to fit.
    // snprintf always NUL-terminates.
    snprintf(e->name_buf, sizeof e->name_buf, "%s",
             name_str ? name_str : "<unnamed>");
    c->funcs.count++;
    return idx;
}