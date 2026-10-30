// ============================================================
// codegen.h — Bytecode generation entry point.
//
// codegen_program takes a semantically-analyzed program and
// emits bytecode into a Chunk. The Chunk is owned by the caller
// and freed by the caller; codegen appends to it.
//
// The ConstTable is the caller's, populated by sema, and read
// here so that references to compile-time constants can be
// inlined as PUSH immediates. Codegen never mutates the table.
//
// Entry point:
//   The program's entry point is the function named `main`. The
//   prologue emits CALL main; HALT. If no function named `main`
//   exists, an error is reported into the bag and no bytecode
//   is emitted. This is intentional: a program without an entry
//   point is not runnable.
//
// Function ordering:
//   Function bodies are emitted in source order. A CALL site
//   that references a function defined later in the source is
//   backpatched after all bodies are laid out; forward calls
//   therefore work.
//
// The SemaCtx is passed in for two reasons:
//   1. Its funcs[] table is what codegen iterates: the AST's
//      top-level declarations include generic templates, and
//      codegen must skip them. sc->funcs[] is the concrete,
//      non-generic set.
//   2. The `codegen_of_sema` mapping (built inside
//      codegen_program) translates a sema func index to a
//      codegen func index. A CALL node carries the sema
//      index; the emitted OP_CALL operand must be the
//      codegen index.
//
// The `program` parameter is unused (kept for ABI stability
// and documentation); codegen walks sc->funcs[] instead. See
// the `(void)program;` at the top of codegen_program.
// ============================================================
#ifndef PRISM_CODEGEN_H
#define PRISM_CODEGEN_H

#include "ast.h"
#include "chunk.h"
#include "source_map.h"
#include "constant.h"
#include "diagnostics.h"
#include "prism_ffi.h"
#include "sema.h"

// Compile a program into `chunk`.
//
// Preconditions:
//   - The program has passed sema; every node's `type` is set.
//   - `consts` has been populated by sema.
//   - `smap` is the same map used during parsing and sema.
//   - `sc` has been filled by sema_analyze.
//
// Postconditions:
//   - On success (no errors in the bag), `chunk` holds a complete
//     program: a CALL main prologue, all function bodies, and a
//     HALT at the end. Wait — HALT is in the prologue, not the
//     end; see the layout diagram in codegen.c.
//   - On failure, `chunk` may hold partial bytecode; the caller
//     checks the bag and discards the chunk if errors were
//     reported.

void codegen_program(AstNode* program, const SourceMap* smap,
                     Chunk* chunk, DiagnosticBag* bag,
                     const ConstTable* consts,
                     const TypeDefTable* structs,
                     const FFITable* ffi,
                     const SemaCtx* sc);   

#endif // PRISM_CODEGEN_H