// ============================================================
// ffi.h — Foreign function interface tables.
//
// A FFITable is filled by sema (all type info is known at
// compile time) and read by codegen and the VM. The resolved
// fn_ptr and ffi_cif fields are filled lazily by the VM on
// first call.
//
// Flow, in one place:
//   1. The parser produces an AST_FUNC with is_extern = true
//      for every `extern fn` declaration, and an AST_EXTERN_LIB
//      for every `extern lib "..." [as alias]` declaration.
//   2. Sema collects the libs into FFITable.libs and the
//      extern functions into FFITable.funcs, translating the
//      Prism types into FFIType codes. If a type is not
//      FFI-compatible, sema reports an error and does not
//      register the function.
//   3. Codegen emits OP_FFI_CALL <ffi_idx> <argc> for every
//      extern call site.
//   4. The VM looks up the FFIFunc by index, and on first use
//      dlopen/LoadLibraryA's the library and dlsym/GetProcAddress's
//      the symbol. libffi's ffi_call performs the actual ABI
//      marshalling.
//
// This file is header-only. Its `ffitable_init` is inline
// because it is a two-field zeroing and there is no reason to
// have a translation unit for it.
// ============================================================
#ifndef PRISM_FFI_H
#define PRISM_FFI_H

#include "common.h"

#ifdef PRISM_FFI_AVAILABLE
#include <ffi.h>
#endif

// The set of C types Prism can pass or receive through the
// FFI. This is deliberately small and closed: adding a new
// FFI-compatible Prism type means adding a case to
// ffi_kind_from_type in sema.c and (for libffi) a mapping in
// ffi_type_of_kind in vm.c. Everything else (structs, enums,
// rc-managed values, refs, arrays) is not FFI-compatible and
// is rejected by sema at declaration time.
//
// FFI_BOOL is passed as a uint8_t at the ABI level, matching
// what most C libraries use for `_Bool` parameters. The
// return value is normalized to 0/1 by the VM's dispatch.
typedef enum {
    FFI_VOID,
    FFI_I8,  FFI_I16, FFI_I32, FFI_I64,
    FFI_U8,  FFI_U16, FFI_U32, FFI_U64,
    FFI_F32, FFI_F64,
    FFI_BOOL,
    FFI_PTR,
} FFIType;

// One shared library. `alias` is what the source names in
// `from` clauses; `path` is what gets handed to the OS loader.
// `handle` and `loaded` are managed by the VM: on first FFI
// call into this library, the VM loads it, stores the handle,
// and sets loaded = true. Handles are never closed; the
// process exits with them open.
typedef struct {
    char  alias[64];     // short name used in `from` clauses
    char  path[256];     // path passed to dlopen / LoadLibraryA
    void* handle;        // filled lazily; never closed
    bool  loaded;
} FFILib;

// One extern function.
//
// Fixed fields (set by sema):
//   symbol      — the C symbol to look up.
//   lib_idx     — index into FFITable.libs.
//   is_variadic — trailing `...` in the declaration.
//   arg_count   — number of *fixed* args (excludes the
//                 variadic tail).
//   ret_type    — the C return type, as an FFIType.
//   arg_types[] — the fixed args' FFITypes.
//
// Option<*T> special-casing:
//   `Option<*T>` and `Option<*void>` appear in FFI signatures
//   to model nullable pointers cleanly. Sema records two
//   flags so codegen and the VM can unwrap/wrap without a
//   full enum representation on the C side.
//
//   returns_option_ptr  — the C return is a raw pointer that
//                         should be wrapped in an
//                         Option<*T> before being pushed.
//   arg_is_option_ptr[] — the corresponding arg is passed as
//                         a raw pointer, unwrapped from
//                         Option<*T>. None becomes NULL.
//
// Runtime-resolved (VM fills on first call):
//   fn_ptr        — the resolved function pointer.
//   cif_ready     — true once ffi_prep_cif[_var] has run.
//   last_varargc  — for variadic functions, the arg count
//                   the current cif was prepared with. If a
//                   later call has a different count, the
//                   cif is rebuilt. Non-variadic functions
//                   leave this at the fixed arg_count.
//
// libffi state (only present when PRISM_FFI_AVAILABLE):
//   cif           — the prepared call interface.
//   ffi_arg_types — the libffi type pointers the cif refers
//                   to. These must outlive the cif (libffi
//                   stores the pointer array, not a copy),
//                   which is why they live on the FFIFunc
//                   rather than on the VM's call frame.
//
//   The ffi_arg_types array is sized 16 because that is the
//   max arg count the VM's ffi_call_dispatch accepts.
typedef struct {
    char    symbol[128];
    int     lib_idx;
    bool    is_variadic;
    int     arg_count;      // fixed args (excludes the `...`)
    FFIType ret_type;
    FFIType arg_types[8];

    bool    returns_option_ptr; // wrap pointer return in Option<>
    bool    arg_is_option_ptr[8];   // unwrap option<*T> arg before call

    // Runtime-resolved (VM fills on first call):
    void*   fn_ptr;
    bool    cif_ready;
    int     last_varargc;   // for varargs; rebuild if arg count changes
#ifdef PRISM_FFI_AVAILABLE
    ffi_cif cif;
    ffi_type* ffi_arg_types[16];    // must persist for the cif's lifetime, libffi stuff
#endif
} FFIFunc;

// The whole FFI table. Sized inline: 16 libraries, 128
// functions. Both caps are hard limits enforced by sema; a
// program that exceeds them is a compile error (see the
// "too many extern functions" and "too many extern lib
// declarations" diagnostics in sema.c).
typedef struct {
    FFILib  libs[16];    int lib_count;
    FFIFunc funcs[128];  int func_count;
} FFITable;

// Zero the two counters. The arrays themselves are left
// uninitialized; sema is expected to memset each entry when
// it registers one. There is no ffitable_free because
// nothing in the FFITable is heap-allocated (the libffi
// pointers are set up by the VM and owned by libffi).
static inline void ffitable_init(FFITable* t) {
    t->lib_count = 0;
    t->func_count = 0;
}

#endif // PRISM_FFI_H