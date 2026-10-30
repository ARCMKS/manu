// ============================================================
// disasm.h — Bytecode listing and structural validation.
//
// Two distinct entry points:
//
//   disassemble       prints a human-readable listing of a
//                     code array. This is a debug tool; the
//                     output goes to stdout and is not meant
//                     to be machine-parsed.
//
//   validate_bytecode a structural check that the VM's
//                     assumptions about bytecode hold. This
//                     is the safety net between codegen and
//                     vm_run: if codegen produced bad
//                     bytecode, validate catches it before
//                     the VM tries to execute it.
//
// Neither function is part of the compilation pipeline per
// se; both are inspection tools that main.c calls between
// codegen and execution.
// ============================================================
#ifndef PRISM_DISASM_H
#define PRISM_DISASM_H

#include "common.h"
#include "constant.h"
#include "source_map.h"
#include "chunk.h"
#include "types.h"

// Full listing. Takes the whole Chunk so that the disassembler
// has access to the func table (for user-function names) as
// well as the code, lines, cols, pool, and type table.
//
// `smap` is used to resolve func-token spans for any function
// whose printable name_buf is empty (defensive; codegen fills
// it for every entry). It may be NULL, in which case names
// fall back to "<unknown>".
//
// `file_path` and `name` are unchanged from the previous
// signature: the file path is used in the location column,
// and `name` is the listing title.
void disassemble(const Chunk* chunk,
                 const SourceMap* smap,
                 const char* file_path,
                 const char* name);

// Structural sanity check. Returns 0 if OK, -1 (and prints to stderr)
// otherwise. Checks: opcodes in range, operand words in range, jumps
// and calls in range, LOAD_CONST idx in pool.
//
// The validator does not execute, does not print the program,
// and does not reason about types. It checks exactly the
// invariants that the VM's dispatch loop assumes:
//   - the first word of each instruction is a valid OpCode;
//   - every instruction's operand words are inside the code;
//   - every jump target is inside the code;
//   - every CALL target is a valid index into `funcs`;
//   - every constant index is inside `pool`;
//   - every parameterized opcode's sub-operand is a valid
//     value (OpBinary / OpUnary indices, descriptor indices).
//
// The `funcs` and `types` parameters may be NULL, in which
// case the corresponding checks are skipped. main.c passes
// real pointers.
int validate_bytecode(const U64* code, size_t length,
                      const ConstantPool* pool,
                      const FuncTable* funcs,
                      const TypeTable* types);

// Exposed so the VM trace and error paths can name opcodes.
// Returns "<unknown>" for an out-of-range opcode and
// "<unnamed-op>" for a valid opcode with no name entry (which
// should not happen; the table is filled by designated
// initializers).
const char* disasm_op_name(int op);

#endif // PRISM_DISASM_H