## `docs/arch/files/22-ffi-header.md`
# `include/prism_ffi.h`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

The FFI table, filled by sema and read by codegen and the VM.

**Lifetime.** Populated during Pass A. Mutated at runtime by the
VM (loading libraries, resolving symbols, preparing cifs). Never
freed explicitly; the table is a caller-owned struct in
`main.c`.

**Relationship to `sema_ffi.c`.** That file builds the table;
this header declares the shape. `vm.c` reads it. `codegen.c`
references the `ffi_idx` of extern calls but does not consult the
table directly.

---

## §2 — Data structures

```c
typedef enum {
    FFI_VOID,
    FFI_I8, FFI_I16, FFI_I32, FFI_I64,
    FFI_U8, FFI_U16, FFI_U32, FFI_U64,
    FFI_F32, FFI_F64,
    FFI_BOOL,
    FFI_PTR,
} FFIType;

typedef struct {
    char  alias[64];
    char  path[256];
    void* handle;
    bool  loaded;
} FFILib;

typedef struct {
    char    symbol[128];
    int     lib_idx;
    bool    is_variadic;
    int     arg_count;
    FFIType ret_type;
    FFIType arg_types[8];
    bool    returns_option_ptr;
    bool    arg_is_option_ptr[8];

    void*   fn_ptr;
    bool    cif_ready;
    int     last_varargc;
#ifdef PRISM_FFI_AVAILABLE
    ffi_cif cif;
    ffi_type* ffi_arg_types[16];
#endif
} FFIFunc;

typedef struct {
    FFILib  libs[16];    int lib_count;
    FFIFunc funcs[128];  int func_count;
} FFITable;
```

**`FFIType`.** The closed FFI type set. Adding a type requires a
new `FFIType`, a case in `ffi_kind_from_type` (in `sema_ffi.c`),
and a case in `ffi_type_of_kind` (in `vm.c`).

**`FFILib`.** One shared library. `alias` is what `from` clauses
name. `path` is passed to the OS loader. `handle` and `loaded`
are filled by the VM on first call.

**`FFIFunc`.** One extern function.

- Fixed fields set by sema: `symbol`, `lib_idx`, `is_variadic`,
  `arg_count`, `ret_type`, `arg_types[]`.
- `Option<*T>` flags: `returns_option_ptr` and
  `arg_is_option_ptr[]` mark nullable-pointer positions.
- Runtime-resolved: `fn_ptr`, `cif_ready`, `last_varargc`.
- libffi state (only when `PRISM_FFI_AVAILABLE`): `cif` and
  `ffi_arg_types[16]`.

**`FFITable`.** 16 libraries, 128 functions. Both caps are hard.
`ffitable_init` (inline) zeroes the counters.

---

## §3 — Implementation notes

### §3.1 `arg_count` vs. actual calls

`arg_count` is the number of **fixed** args. A variadic call can
pass more; the VM rebuilds the cif when the total count differs
from `last_varargc`.

### §3.2 `ffi_arg_types[16]`

The array lives on the `FFIFunc` rather than the stack because
`ffi_prep_cif` stores a pointer to it, not a copy. A
stack-allocated array would go out of scope before the first
`ffi_call`.

### §3.3 `Option<*T>` special-casing

Sema's `is_option_ptr_type` recognizes `Option<*T>` by name
prefix and structure. The two flags are set here; the VM's
`ffi_call_dispatch` reads them to unwrap and wrap.

`arg_is_option_ptr[]` is sized 8, matching the fixed-arg cap.

### §3.4 The `#ifdef PRISM_FFI_AVAILABLE` block

When the build does not link against libffi, `ffi.h` is not
included and the libffi state fields are absent. Every FFI native
in `vm.c` degrades to `RT_ERR("FFI support not compiled in")`.

The Makefile defines `PRISM_FFI_AVAILABLE` when it can find
libffi.

### §3.5 `ffitable_init` is inline

Two counter writes, no translation unit needed.

---

## §4 — Invariants

- `lib_count <= 16`, `func_count <= 128`.
- Every `FFIFunc.lib_idx` is a valid index into `libs[]`.
- Every `FFIFunc.arg_count` is at most 8.
- Every `FFIFunc.arg_types[i]` for `i < arg_count` is a valid
  `FFIType`.
- Every `FFIFunc.symbol` is a NUL-terminated string (truncated
  at 127 bytes by `smap_span_text`).
- Every `FFILib.path` is a NUL-terminated string (truncated at
  255 bytes).
- `handle` and `loaded` are consistent: `loaded` implies
  `handle != NULL`.
- `fn_ptr` and `cif_ready` are filled together; both are set by
  `ffi_resolve`.

---

## §5 — Known limitations

**Fixed caps.** 16 libraries and 128 functions, enforced by
sema diagnostics.

**The `symbol` field truncates at 127 bytes.** A long C symbol
name would be silently truncated. Not observed in practice.

**`arg_types` is capped at 8 fixed args.** A variadic function
with more fixed args than 8 is not bindable.

**`ffi_arg_types[16]` is only sized for the max total arg count
the VM accepts (16).** A caller passing more than 16 args is
rejected by `ffi_call_dispatch`, but the array size is a limit
even if a future change raised the acceptance count.

**No `dlclose`.** Handles live for the process.

**No unload/reload.** A library's `loaded` flag is monotonic.

**No per-function error state.** `vm->ffi_error` is the only
place a failure message is stored; it is overwritten by the next
failure.

---

## §6 — Extension points

- **A `FFI_FNPTR` type.** Would let a Prism function value be
  marshalled to a C function pointer.
- **Struct-by-value marshalling.** Would require a `ffi_type`
  for each C struct and a layout rule.
- **Richer variadic typing.** Currently all varargs are typed
  `sint64`; a per-arg `FFIType` for varargs would let `float`
  be supported (with the required promotion).
- **`dlclose`.** Would require tracking dependencies and call
  sites.
- **Per-function error state.** Would let each failure carry its
  own message.
- **Named library search paths.** Currently `path` is passed
  verbatim to the OS loader; the loader's own search rules
  apply.
