## `docs/arch/files/13-sema-ffi.md`

# `src/sema_ffi.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

Builds the compile-time FFI view: the `LibraryTable` (aliases and
paths from `extern lib`) and, via `ffi_kind_from_type`, the
per-function `FFIType` translations. The runtime side (`FFILib`,
`FFIFunc`, handles, `cif`) is produced here and consumed by the
VM; the compile-time half never executes `dlopen` or `libffi`.

**Called from:** `sema_analyze` (for the library table) and Pass
A's extern path (for type translation). Also from body analysis
for variadic argument checks.

**Callers of the results:** Pass A stores the `FFIFunc` entries
in the caller-owned `FFITable`. Codegen emits `OP_FFI_CALL` with
a `FFIFunc` index. The VM's `ffi_resolve`/`ffi_call_dispatch`
read the entries.

---

## §2 — Public interface

```c
typedef struct {
    char alias[64];
    char path[256];
} LibEntry;

typedef struct {
    LibEntry entries[16];
    int      count;
} LibraryTable;

void build_library_table(LibraryTable* lt, AstNode* program,
                         const SourceMap* smap, DiagnosticBag* bag);

int library_table_find(const LibraryTable* lt,
                       const SourceMap* smap, Token alias);

bool ffi_kind_from_type(const Type* t, FFIType* out);

bool is_option_ptr_type(const Type* t);
bool is_ffi_variadic_ok(const Type* t);
```

**`build_library_table`.** Resets `lt->count` to 0 and populates
from the program's `AST_EXTERN_LIB` nodes. Silent truncation for
paths and aliases longer than the inline buffers.

**`library_table_find`.** Linear scan; compares the alias's
extracted text (via `smap_span_text`) against each entry.

**`ffi_kind_from_type`.** The single translation point from
`TypeKind` to `FFIType`. Returns false for anything not in the
FFI-compatible set.

**`is_option_ptr_type`.** Recognizes `Option<*T>` for the
nullable-pointer marshalling convention.

**`is_ffi_variadic_ok`.** Returns true for the types allowed in a
variadic position.

---

## §3 — Implementation notes

### §3.1 Alias derivation

`derive_alias(path, out, cap)` extracts the basename and stops at
the first `.`. Both `/` and `\` are separators.

```
"SDL2"              -> "SDL2"
"./libfoo.so"       -> "libfoo"
"C:\\...\\user32.dll" -> "user32"
"/usr/lib/bar.a"    -> "bar"
```

Used only when the source omits `as`. Truncation to `cap - 1` is
silent.

### §3.2 Path extraction from the string literal

The lexer emits a `TOK_STRING` whose span covers the whole literal
including quotes. `build_library_table` strips the quotes by
using a span of `[start + 1, end - 1)`. The `end >= start + 2`
guard handles the degenerate 1-byte-span case.

### §3.3 `is_option_ptr_type`

The recognition is a four-way check:

1. `t->kind == TY_RC`.
2. `t->inner->kind == TY_ENUM` and the inner's `TypeDef` has
   `is_rc_managed`.
3. `strncmp(td->name_buf, "Option#", 7) == 0`.
4. `td->variant_count >= 1`, `variants[0].payload_count == 1`,
   and `variants[0].payloads[0]->kind == TY_RAWPTR`.

The name-prefix check is the fragile part. It is sound in
practice because only the prelude's `Option` template produces
names beginning with `Option#`, and the mangler uses `#` as the
separator (which cannot appear in a source identifier).

### §3.4 The two casts the FFI does not do

`ffi_kind_from_type` rejects `TY_BOOL`'s narrow representation
implicitly: `bool` is passed as `uint8_t` at the ABI level but
the VM stores it as 0/1 in a full slot. The FFI's marshalling
(the VM's `ffi_call_dispatch`) handles the narrowing; this file
only says "bool is FFI_BOOL".

`ffi_kind_from_type` rejects all compound types and non-numeric
scalars. The FFI-compatible set is exactly the leaf set from
`language/reference.md` §13.2.

### §3.5 `is_ffi_variadic_ok`

Accepts: `I8`–`I64`, `U8`–`U64`, `BOOL`, `RAWPTR`. Rejects
everything else, including floats.

Floats are rejected because C's default-argument-promotion rules
require variadic `float` to be passed as `double`, and
`ffi_prep_cif_var` does not apply the promotion. Passing an
`f32` in a variadic position would produce a silent ABI mismatch.

---

## §4 — Invariants

- `LibraryTable.count <= 16`.
- Every entry's `alias` is a nonempty NUL-terminated string.
- Every entry's `path` is a NUL-terminated string with no
  surrounding quotes.
- `ffi_kind_from_type` writes to `*out` only on a true return.
- `is_option_ptr_type` and `is_ffi_variadic_ok` are pure
  predicates (no side effects).

---

## §5 — Known limitations

**`is_option_ptr_type` uses a name prefix.** Sound in practice;
documented as fragile.

**No dedup of libraries.** Two `extern lib "libc.so.6"` clauses
produce two entries. `library_table_find` returns the first
match; the second is dead.

**No validation of the path string.** A path with internal NULs
would be truncated by `smap_span_text` (which NUL-terminates);
not a real case.

**No forwarding of export/import semantics.** All
`extern lib` declarations are treated identically; there is no
notion of a library being "always loaded" versus "lazy".

**The `LibraryTable` is caller-stack-allocated.** No dynamic
growth; the 16-entry cap is hard. Exceeding it produces a
diagnostic in `build_library_table`.

**`FFIType` set is closed.** Adding a new numeric type requires
a new `FFIType` and updates to `ffi_kind_from_type` and to the
VM's `ffi_type_of_kind` in `vm.c`.

---

## §6 — Extension points

- **Struct-by-value marshalling.** Would require a new `FFIType`
  and a layout rule; the C ABI's classification of structs into
  register and memory classes is not implemented.
- **Callbacks.** Would require a `FFI_FNPTR` type and a way to
  marshal a Prism function pointer to a C function pointer.
- **Return type `Result<*T, E>`.** Would require an error-code
  convention at the boundary.
- **Dedup of libraries.** A pre-check before appending.
- **Multiple ABIs.** No `stdcall`, no `fastcall`.
