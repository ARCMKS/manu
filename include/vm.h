// ============================================================
// vm.h — Virtual machine: opcodes, state, API.
//
// A 64-bit stack machine with two memory tiers and three
// logical stacks (data, call, scope). The instruction set is
// parameterized: type-sensitive opcodes carry a descriptor
// index, not a hard-coded type tag. Adding new types is a
// descriptor, not an opcode.
//
// Machine model, in one page:
//
//   Data stack       The evaluation stack. Every value is a
//                    U64. All operations push/pop 64-bit
//                    words. Locals are addressed by slot
//                    into the current frame's region of
//                    this stack (fp + slot).
//
//   Call stack       One CallFrame per active call. A frame
//                    records the caller's return pc, fp,
//                    arena mark, and scope depth, plus the
//                    callee's func_idx for diagnostics.
//
//   Scope stack      One ScopeFrame per OP_ENTER_SCOPE. On
//                    exit, restores both the data stack's
//                    sp and the arena's ap. This is what
//                    makes lexical scoping free at runtime:
//                    the compiler emits ENTER/EXIT_SCOPE,
//                    the VM just bookmarks and restores.
//
//   Arena            Bump allocator (ARENA_MAX bytes). Reset
//                    on scope exit. All non-RC heap values
//                    live here.
//
//   RC heap          malloc-backed blocks with a refcount
//                    header (RcHeader) preceding the
//                    payload. Freed when the count hits 0.
//
//   Const arena      A second bump allocator, sized once at
//                    vm_init from the constant pool, holding
//                    materialized string constants. Never
//                    freed until vm_free. String constants
//                    have a sentinel refcount so RC_DEC on
//                    them is a no-op.
//
// The dispatch loop reads one U64 word per opcode, plus
// inline operand words. Errors set vm->error and return 1.
// ============================================================
#ifndef PRISM_VM_H
#define PRISM_VM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "common.h"
#include "constant.h"
#include "chunk.h"
#include "prism_ffi.h"

// ---- Machine limits ----
//
// All are fixed sizes; exceeding any is a clean runtime
// error from the VM's own checks (see RT_ERR below), not a
// segfault.
//
//   DATA_STACK_MAX    words on the evaluation stack.
//   CALL_STACK_MAX    simultaneous call frames.
//   SCOPE_STACK_MAX   nested ENTER_SCOPE without matching
//                     EXIT_SCOPE.
//   ARENA_MAX         bytes in the lexical arena. Reset by
//                     EXIT_SCOPE; a program whose peak
//                     lexical-scope allocation exceeds this
//                     will hit "arena out of memory".
//   CONST_ARENA_MAX   bytes in the const arena. Sized at
//                     init from the constant pool, so this
//                     is an upper bound only.
//   MAX_NATIVES       native dispatch table slots. The
//                     NativeId enum below must fit.
#define DATA_STACK_MAX   4096
#define CALL_STACK_MAX    256
#define SCOPE_STACK_MAX   256
#define ARENA_MAX       (1 << 20)
#define CONST_ARENA_MAX (1 << 16)
#define MAX_NATIVES       32


// Output sink sizing. The sink is the VM's buffered output
// destination. During string-building (BEGIN/END_STRING) the
// sink accumulates into `buf`; between builds it targets a
// FILE*. The inline buffer avoids a malloc for short
// strings; longer strings grow to the heap.
//
//   SINK_STACK_MAX  nested BEGIN_STRING depth.
//   SINK_INLINE_CAP bytes in the inline buffer.
#define SINK_STACK_MAX 64
#define SINK_INLINE_CAP 128

// What a sink writes to.
//   SINK_STDOUT, SINK_STDERR — write to the FILE* in `file`.
//   SINK_STRING              — append to `buf`, growing it
//                              if needed. Used by BEGIN/END
//                              _STRING to build a string.
typedef enum {
    SINK_STDOUT = 0,
    SINK_STDERR,
    SINK_STRING,
} SinkKind;

// The sink. Used by the OP_BEGIN_STRING / OP_END_STRING pair
// and by the print path (which uses the same mechanism to
// assemble a line before writing it as one CCALL).
//
// Fields:
//   kind       — what to do with written bytes.
//   file       — target FILE* for STDOUT / STDERR sinks.
//   inline_buf — small stack buffer for short strings. `buf`
//                points here until the string outgrows it.
//   buf        — the current buffer. Points at inline_buf or
//                at a heap allocation.
//   len        — bytes written into buf.
//   cap        — capacity of buf.
//
// Invariant: `buf != NULL` implies `buf == inline_buf ||
// buf is a heap allocation`. vm_free frees buf if it is not
// inline_buf.
typedef struct {
    SinkKind kind;
    FILE*    file;
    uint8_t  inline_buf[SINK_INLINE_CAP];
    uint8_t* buf;
    size_t   len;
    size_t   cap;
} Sink;

// Native function slots. Each id maps to one C function
// registered in vm->natives[]. The dispatch is through
// OP_CCALL <native_id>.
//
// Ordering note: the enum values are the numbers that
// codegen emits, so reordering them requires updating
// codegen's emission sites. In practice each native's
// numeric value is only produced in one place in codegen
// (see the builtin dispatch in cg_expr's AST_CALL case),
// so the coupling is manageable but real.
//
// Summary:
//   0 stdout_write      one string arg, writes to vm->out.
//   1 stderr_write      one string arg, writes to stderr.
//   2 ffi_cstring       prism string -> C string in arena.
//   3 ffi_string        C string -> prism string (RC).
//   4 ffi_error         last dlerror/dlopen message (string).
//   5 ffi_errno         captured errno after last ffi call.
//   6 raw_null          push 0.
//   7 raw_is_null       ptr -> bool.
//   8 raw_offset        (ptr, n, elem_size) -> ptr.
//   9 raw_load_u8       (ptr, i) -> u8.
//  10 raw_store_u8      (ptr, i, v) -> void.
//  11 ffi_alloc         size -> *void (zeroed malloc).
//  12 ffi_free          ptr -> void.
typedef enum {
    NATIVE_STDOUT_WRITE = 0,
    NATIVE_STDERR_WRITE = 1,
    NATIVE_FFI_CSTRING  = 2,
    NATIVE_FFI_STRING   = 3,
    NATIVE_FFI_ERROR    = 4,
    NATIVE_FFI_ERRNO    = 5,
    NATIVE_RAW_NULL     = 6,
    NATIVE_RAW_IS_NULL  = 7,
    NATIVE_RAW_OFFSET   = 8,
    NATIVE_RAW_LOAD_U8  = 9,
    NATIVE_RAW_STORE_U8 = 10,
    NATIVE_FFI_ALLOC    = 11,
    NATIVE_FFI_FREE     = 12,
    NATIVE_ARGS         = 13,
    NATIVE_READ_LINE    = 14,
    NATIVE_READ_ALL     = 15,
    NATIVE_STR_BEGIN    = 16,
    NATIVE_STR_END      = 17,
    NATIVE_PRINT_VALUE  = 18,
    NATIVE_ARENA_ZERO   = 19,
    NATIVE_MEMCPY       = 20,
    NATIVE_COUNT
} NativeId;


// Memory access width for LOAD_INDIRECT and STORE_INDIRECT.
// Determines how many bytes are read/written and (for
// sub-word integers) how the value is sign/zero-extended
// into the VM's 64-bit slot.
//
// MEM_PTR is a raw 8-byte read/write with no interpretation;
// it is what regular Prism struct fields use, and what
// pointer-typed fields use as well. Separating it from MEM_U64
// is purely for readability of the disassembly.
typedef enum {
    MEM_I8 = 0, MEM_I16, MEM_I32, MEM_I64,   // sign-extend on load
    MEM_U8,     MEM_U16, MEM_U32, MEM_U64,   // zero-extend on load
    MEM_F32,    MEM_F64,                     // raw bits, no extension
    MEM_PTR,                                 // 8-byte raw
    MEM_COUNT
} MemKind;
// ============================================================
// Opcodes
//
// Wire format: each instruction starts with one U64 word
// containing the opcode. Operands follow as additional U64
// words. The OPINFO table in disasm.c is the authoritative
// operand count for each opcode.
//
// Operand conventions:
//   <desc_idx>      index into the chunk's TypeTable.
//   <slot>          data-stack slot relative to fp.
//   <offset>        byte offset for indirect access.
//   <pool_idx>      index into the constant pool.
//   <pc>            word offset into the code array.
//   <func_idx>      index into the chunk's FuncTable.
//   <argc>          number of arguments (already on the stack).
//   <OpBinary/Unary> sub-opcode from the enums below.
//
// Descriptor-parameterized opcodes (BINARY, UNARY, PRINT,
// INDEX_LOAD, INDEX_STORE, LEN, CONVERT) take a desc_idx so
// the VM can dispatch on the runtime shape of the type
// rather than on a fixed tag. That is the "adding new types
// is a descriptor, not an opcode" design.
// ============================================================
typedef enum {
    OP_HALT = 0,

    OP_PUSH,
    OP_POP,
    OP_DUP,
    OP_DUP2,

    // Local variable access. Slots are relative to vm->fp.
    OP_LOAD_LOCAL,
    OP_STORE_LOCAL,
    OP_LOAD_ADDR_LOCAL,      // pushes &data_stack[fp + slot]

    // Indirect memory access (through a pointer).
    // LOAD_INDIRECT  <offset> <mem_kind>: pop addr, push the
    //   value at addr+offset read with the given width.
    // STORE_INDIRECT <offset> <mem_kind>: pop value then addr,
    //   write value at addr+offset with the given width.
    // LOAD_ADDR_INDIRECT <offset>: pop addr, push addr+offset.
    //   Used to compute the address of a nested extern struct
    //   field, and by the sema-rewritten `&p.x` form.
    OP_LOAD_INDIRECT,
    OP_STORE_INDIRECT,
    OP_LOAD_ADDR_INDIRECT,   

    // Constant pool.
    OP_LOAD_CONST,
    // Container length (string or array); takes desc_idx.
    OP_LEN,

    // Parameterized arithmetic/comparison.
    OP_BINARY,             // <OpBinary>  <desc_idx>
    OP_UNARY,              // <OpUnary>   <desc_idx>

    // Control flow.
    OP_JMP,
    OP_JMP_IF_FALSE,
    OP_JMP_IF_TRUE,
    OP_CALL,
    OP_CALL_INDIRECT,
    OP_CCALL,
    OP_RET,

    // Scope bracket. ENTER saves (sp, amp); EXIT restores
    // both. Also resets the arena to the saved mark.
    OP_ENTER_SCOPE,
    OP_EXIT_SCOPE,

    // Arena and RC allocation of a single 8-byte value.
    OP_ALLOC,
    OP_ALLOC_RC,

    // Aggregates.
    OP_NEW_ARRAY,
    OP_NEW_RC_ARRAY,

    OP_INDEX_LOAD,
    OP_INDEX_STORE,
    OP_NEW_ENUM,           // <payload_count>
    OP_NEW_RC_ENUM,        // <payload_count>
    OP_TRAP,               // <pool_idx>

    // Refcount traffic. RC_INC has no operand (increment is
    // type-independent). RC_DEC takes a <desc_idx> so the VM
    // can deep-drop RC-managed payloads of structs, arrays,
    // and enums before freeing the outer block. Both are
    // no-ops on null and on const-arena strings (sentinel
    // refcount).
    OP_RC_INC,
    OP_RC_DEC,

    // Numeric conversion between descriptors.
    OP_CONVERT,

    // Foreign function call.
    OP_FFI_CALL,

    // `?` propagation: if top-of-stack is an RC enum with
    // tag 0, replace with payload and jump; else fall
    // through (the caller's RET returns the failure value).
    OP_TRY_JUMP_IF_OK,
    
    OP_COUNT,
} OpCode;

// Binary sub-ops. Used as the first operand of OP_BINARY.
// The order is fixed and is part of the wire format; do not
// reorder without updating codegen and disasm together.
typedef enum {
    OPB_ADD = 0,
    OPB_SUB,
    OPB_MUL,
    OPB_DIV,
    OPB_EQ,
    OPB_NE,
    OPB_LT,
    OPB_LE,
    OPB_GT,
    OPB_GE,
    OPB_AND,
    OPB_OR,
    OPB_COUNT
} OpBinary;

// Unary sub-ops. Two values only.
typedef enum {
    OPU_NEG = 0,
    OPU_NOT,
    OPU_COUNT
} OpUnary;

// ============================================================
// State
// ============================================================

// One call frame.
//
// On OP_CALL the VM captures the caller's state into a fresh
// frame, then adjusts fp and pc for the callee. On OP_RET
// the frame is popped and the saved state is restored.
//
// Fields:
//   return_pc    — where the caller resumes after RET.
//   fp           — caller's frame pointer (the base of the
//                  caller's locals region). Restored on RET.
//   amp          — caller's arena mark. Restored on RET so
//                  that anything allocated by the callee is
//                  reclaimed.
//   scope_depth  — caller's scope depth. Restored on RET.
//   func_idx     — the callee's index in the chunk's func
//                  table. Used for trace/error rendering.
//   call_pc      — the pc of the OP_CALL instruction itself
//                  (not the return address). Used by the
//                  error trace to show "called from <here>".
typedef struct {
    U64  return_pc;
    U32  fp;
    U8*  amp;
    U32  scope_depth;
    int  func_idx;
    U64  call_pc;
} CallFrame;

// One scope frame. Saved by OP_ENTER_SCOPE and restored by
// OP_EXIT_SCOPE.
//
//   sp  — data stack pointer at scope entry. On exit the
//         stack is truncated back to this, which drops any
//         locals declared in the scope.
//   amp — arena mark at scope entry. On exit the arena is
//         reset to this, which frees every arena allocation
//         made inside the scope.
typedef struct {
    U32 sp;
    U8* amp;
} ScopeFrame;

// RC block header. Every RC-managed block starts with one
// of these, 8 bytes, followed by the payload.
//
// ref_count == 0xFFFFFFFF is the "const arena" sentinel,
// meaning "never freed, never counted". RC_INC and RC_DEC
// both check for it and short-circuit.
//
// padding keeps the struct 8 bytes aligned so the payload
// that follows is 8-aligned, which matters for F64s and
// pointers stored in the payload.
typedef struct {
    U32 ref_count;
    U32 padding;
} RcHeader;

typedef struct VM VM;
typedef void (*NativeFn)(VM* vm);

struct VM {
    // ---- Program (borrowed from the caller) ----
    //
    // The VM does not own any of these; they live for the
    // life of the enclosing compilation and are freed by
    // the driver after vm_free. In particular, `bytecode`
    // is a pointer into the Chunk, not a copy.
    U64*                bytecode;
    size_t              bytecode_len;
    const uint32_t*     lines;
    const uint32_t*     cols;
    const ConstantPool* pool;
    const FuncTable*    funcs;
    const TypeTable*    types;

    // ---- FFI ----
    //
    // `ffi` is the compile-time FFI table filled by sema.
    // The VM mutates entries lazily (loading libraries,
    // resolving symbols, preparing cifs). The mutation is
    // confined to the FFIFunc/FFILib structs and does not
    // touch the compile-time fields.
    //
    // ffi_error and ffi_errno are the VM's "last FFI
    // failure" state; the ffi_error/ffi_errno builtins
    // read them.
    const FFITable* ffi;             // borrowed from VMConfig, never freed
    char            ffi_error[256];  // last dlerror message
    int             ffi_errno;       // captured errno after each call
    bool            native_failed;
    
    // ---- Execution state ----
    //
    // `pc` is the read cursor into bytecode. `current_pc`
    // is the pc of the instruction currently being
    // executed; errors report this, not the advanced pc.
    // `error_pc` and `error_func_idx` are the error
    // location; the driver's render_runtime_error reads
    // them.
    U64 pc;
    U64 current_pc;
    U64 error_pc;
    int error_func_idx;
    char error[256];

    // ---- Data stack ----
    //
    // `sp` is the next free slot. `fp` is the base of the
    // current frame's locals; slots are addressed as
    // data_stack[fp + slot]. On OP_CALL, fp is set to
    // sp - argc, so the first argument becomes slot 0 of
    // the callee's frame.
    U64 data_stack[DATA_STACK_MAX];
    U32 sp;
    U32 fp;

    // ---- Call stack ----
    CallFrame call_stack[CALL_STACK_MAX];
    U32 call_depth;

    // ---- Scope stack ----
    ScopeFrame scope_stack[SCOPE_STACK_MAX];
    U32 scope_depth;

    // ---- Arenas ----
    //
    // Lexical arena. `arena` is the base; `ap` is the
    // current bump pointer; `amp` is the mark of the
    // *enclosing* scope. EXIT_SCOPE resets `ap` to `amp`,
    // then restores `amp` from the popped scope frame.
    U8*  arena;
    U8*  ap;
    U8*  amp;

    // Const arena. Sized at init from the constant pool.
    // `cap` is the bump cursor. `const_ptrs[i]` is the
    // materialized pointer for pool entry i, or NULL for
    // non-string constants.
    U8*  const_arena;
    U8*  cap;
    U8** const_ptrs;
    size_t const_arena_size;

    // ---- Output ----
    //
    // `out` is the FILE* for the stdout_write native.
    // `sink` is the current sink; `sink_stack` and
    // `sink_depth` support nesting (format inside print).
    FILE* out;
    FILE* in;               // stdin (or source)
    const char* const* argv;      // program arguments (borrowed)
    int argc;
    bool  trace;

    Sink sink;
    Sink sink_stack[SINK_STACK_MAX];
    int  sink_depth;

    // ---- Natives ----
    //
    // Fixed-size dispatch table; vm_register_native fills
    // entries. Dispatch is a single indirect call.
    NativeFn natives[MAX_NATIVES];
};

// ============================================================
// Configuration
// ============================================================
typedef struct {
    U64*                bytecode;
    size_t              bytecode_len;
    const uint32_t*     lines;
    const uint32_t*     cols;
    const ConstantPool* pool;
    const FuncTable*    funcs;
    const TypeTable*    types;
    const FFITable*     ffi;
    const char**        prog_argv;
    int                 prog_argc;
} VMConfig;

// ============================================================
// API
// ============================================================
void vm_init(VM* vm, const VMConfig* cfg);
void vm_free(VM* vm);
int  vm_run(VM* vm);
I64  vm_top(VM* vm);
void vm_register_native(VM* vm, int slot, NativeFn fn);

#endif // PRISM_VM_H