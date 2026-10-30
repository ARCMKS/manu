
## `docs/arch/files/06-types.md`

# `include/types.h`, `src/types.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

`types.h` declares the type system in all three of its
representations and the tables that own them.

- **`Type`** — the compiler's recursive, structured view.
- **`TypeExpr`** — a syntax-level type expression, before
  resolution.
- **`TypeDef`** — a nominal type definition (a struct or enum),
  registered once and referenced by pointer from every `Type` that
  names it.
- **`TypeDesc`** — the runtime's flat, indexable view, emitted into
  bytecode operands by codegen and read by the VM at dispatch time.
- **`TypeDefTable`** — a registry of nominal types.
- **`ModuleTable`** — the per-file namespaces built from `using`
  declarations. Lives here because `type_expr_resolve` needs it.
- **`TypeTable`** — the runtime descriptor universe for one
  compilation.

**What it solves.** The three representations and the bridges
between them. The type-expression resolver. The runtime descriptor
interner.

**What it does not solve.** Generic instantiation (in
`monomorph.c` / `sema_mono.c`, delegated via a callback). Name
resolution (in `resolve.c`). Layout of regular Prism structs
(sema computes offsets).

**Who depends on it.** Everything from sema onward, and every file
that touches a type.

---

## §2 — `TypeKind` and the three representations

### §2.1 `TypeKind` — the compiler's discriminator

```c
typedef enum {
    TY_VOID,
    TY_I8, TY_I16, TY_I32, TY_I64,
    TY_U8, TY_U16, TY_U32, TY_U64,
    TY_F32, TY_F64,
    TY_BOOL,
    TY_STRING,
    TY_REF,
    TY_RC,
    TY_RAWPTR,
    TY_ARRAY,
    TY_STRUCT,
    TY_EXTERN_STRUCT,
    TY_FN,
    TY_ENUM,
} TypeKind;
```

Twenty-one kinds. The list partitions into three groups by what
fields of `Type` are meaningful.

**Scalars.** `TY_VOID`, `TY_I8..I64`, `TY_U8..U64`, `TY_F32`,
`TY_F64`, `TY_BOOL`, `TY_STRING`. Only `kind` and `size` are
meaningful.

**Single-inner compound.** `TY_REF` (with `is_mut`), `TY_RC`,
`TY_RAWPTR`, `TY_ARRAY`. `inner` points at the element type.

**Nominal.** `TY_STRUCT`, `TY_EXTERN_STRUCT`, `TY_ENUM`. `tdef`
points at the `TypeDef` that names the type. `size` is copied
from the `TypeDef`.

**Function.** `TY_FN`. `fn_params`, `fn_param_count`, `fn_ret`.

### §2.2 `Type` — the compiler's view

```c
struct Type {
    TypeKind kind;
    int      size;
    Type*    inner;
    bool     is_mut;
    TypeDef* tdef;
    Type**   fn_params;
    int      fn_param_count;
    Type*    fn_ret;
};
```

Sizes: scalars have their natural byte size, except `bool` and
`string` which are both fixed at 8 (they occupy a full slot at
runtime). Compound types are pointer-shaped (8 bytes) except
structs and extern structs, whose `size` comes from the `TypeDef`.

**Primitives are singletons.** `ty_i64()` always returns the same
`Type*`; pointer equality is valid for primitives. The singletons
are static structs in `types.c` (`T_I64`, `T_STRING`, etc.).

**Compound types are allocated fresh.** `ty_ref`, `ty_rc`,
`ty_rawptr`, `ty_array`, `ty_struct`, `ty_extern_struct`, `ty_fn`,
`ty_enum` each call `alloc_compound`, which `xcalloc`s a fresh
`Type`. Two calls with the same arguments produce two distinct
pointers. `type_eq` does structural comparison.

**`Type*` is never freed.** Every compound `Type` is leaked at
process exit. For a compiler that runs one compilation per process
this is bounded and acceptable; a REPL or language server would
need to change this.

### §2.3 `TypeExpr` — the syntactic form

```c
typedef enum {
    TEX_NAME, TEX_REF, TEX_RC, TEX_RAWPTR, TEX_ARRAY,
    TEX_QUALIFIED, TEX_GENERIC, TEX_RESOLVED, TEX_FN,
} TypeExprKind;

typedef struct TypeExpr {
    TypeExprKind kind;
    Span         span;
    Token        name;
    bool         is_mut;
    struct TypeExpr* inner;
    Token* path; int path_count;
    struct TypeExpr** args; int arg_count; int arg_capacity;
    Type* resolved;
    struct TypeExpr** params; int param_count; int param_capacity;
    struct TypeExpr* ret;
} TypeExpr;
```

Nine kinds:

- `TEX_NAME` — a bare identifier (`i64`, `Player`).
- `TEX_REF` — `ref<T>` or `ref<mut T>`. `is_mut` distinguishes.
- `TEX_RC` — `rc<T>`.
- `TEX_RAWPTR` — `*T`.
- `TEX_ARRAY` — `[T]`.
- `TEX_QUALIFIED` — a dotted path (`a.b.c`).
- `TEX_GENERIC` — `Name<arg1, arg2, …>`.
- `TEX_RESOLVED` — carries a `Type*` directly. Used by the
  monomorphizer during deep-copy to substitute a concrete type for
  a parameter reference.
- `TEX_FN` — `fn(T1, T2) : R`.

Like `AstNode`, the struct is a single shape with kind-specific
fields. Readers dispatch on `kind` before touching any other field.

**Ownership.** A `TypeExpr` owns its `inner` (recursively), its
`args` array and each element, its `path` array, and its
`params`/`ret` for `TEX_FN`. It does **not** own `resolved`, which
is a `Type*` and never freed.

`type_expr_free` recursively releases everything.

### §2.4 `TypeDesc` — the runtime view

```c
typedef enum {
    TD_VOID,
    TD_I8, TD_I16, TD_I32, TD_I64,
    TD_U8, TD_U16, TD_U32, TD_U64,
    TD_F32, TD_F64,
    TD_BOOL,
    TD_STRING,
    TD_REF,
    TD_RC,
    TD_RAWPTR,
    TD_ARRAY,
    TD_STRUCT,
    TD_EXTERN_STRUCT,
    TD_ENUM,
    TD_COUNT
} TypeDescKind;

typedef struct {
    TypeDescKind kind;
    uint32_t     aux;
    uint32_t     size;
    uint32_t     count;
} TypeDesc;
```

Twenty-one kinds, parallel to `TypeKind`. The two enums are
deliberately not the same type: a future change to `TypeKind`'s
ordering would not break bytecode that references `TypeDescKind`
values.

**`aux`'s meaning depends on `kind`:**

- For `TD_REF` / `TD_RC` / `TD_RAWPTR` / `TD_ARRAY` — the interned
  index of the inner descriptor.
- For `TD_STRUCT` / `TD_EXTERN_STRUCT` — the starting index into
  `TypeTable.fields`.
- For `TD_ENUM` — the starting index into `TypeTable.enum_variants`.
- For scalars — 0.

**`size`** is the byte size of a value of this type.

**`count`** is the number of fields (structs) or variants (enums);
0 for everything else.

### §2.5 `TypeDef` — the nominal definition

```c
typedef enum {
    TDEF_STRUCT,
    TDEF_EXTERN_STRUCT,
    TDEF_ENUM,
    TDEF_BUILTIN,
} TypeDefKind;

struct TypeDef {
    TypeDefKind kind;
    char        name_buf[128];
    Token       name;
    bool        is_pub;
    bool        is_instantiated;
    bool        is_opaque;
    bool        is_union;

    StructField* fields; int field_count; int field_capacity;
    int          struct_size;
    int          struct_align;

    EnumVariantDef* variants; int variant_count; int variant_capacity;
    bool            is_rc_managed;
};
```

**Kind.** `TDEF_STRUCT` for user structs, `TDEF_EXTERN_STRUCT` for
extern structs, `TDEF_ENUM` for enums, `TDEF_BUILTIN` for primitive
types with nominal identity (`string`, `i64`, etc.).

**`name_buf`.** A NUL-terminated copy of the type's name (mangled
for instantiations, e.g. `Option#i64`). Used for diagnostics and
`type_name`. Truncated at 127 characters.

**`is_pub`.** Visibility from other files.

**`is_instantiated`.** True for a monomorphized copy. Skips by
`typedef_table_find` and `typedef_table_find_in_file` (the latter
does not skip, since instantiations are looked up by name in the
template's own file).

**`is_opaque`.** For extern types with no layout.

**`is_union`.** For extern union types. Fields laid out at offset
0, size = max of field sizes.

**Struct fields.** `fields` grows by doubling. `struct_size`
starts at 8 (the field-count slot) and increases by 8 per field
for regular structs, or by C-ABI layout for extern structs.
`struct_align` is the C-ABI maximum alignment.

**Enum variants.** `variants` grows by doubling. Each
`EnumVariantDef` has a name, a `name_buf`, a growable array of
payload `Type*`, and a `name` token.

**`is_rc_managed`.** Marks the enums whose every use site is
wrapped in `ty_rc(...)`. Currently set for the prelude's `Result`
and `Option` and for any user enum marked `is_builtin` (which no
user code can do). The set of such types is closed.

### §2.6 The three representations, compared

| Concept | `TypeExpr` | `Type` | `TypeDesc` |
|---|---|---|---|
| Purpose | syntax | compile-time structure | runtime dispatch |
| Form | tree | recursive | flat |
| Comparison | structural | `type_eq` | index equality |
| Ownership | owned by AST | leaked | owned by `TypeTable` |
| Source of | parser | `type_expr_resolve` | `typetable_intern` |

Bridges:

- `type_expr_resolve` — `TypeExpr` → `Type`.
- `typetable_intern` — `Type` → `TypeDesc` index.

---

## §3 — Public interface (grouped)

### §3.1 Primitive constructors

```c
Type* ty_void(void);
Type* ty_i8(void);   ... Type* ty_i64(void);
Type* ty_u8(void);   ... Type* ty_u64(void);
Type* ty_f32(void);  Type* ty_f64(void);
Type* ty_bool(void); Type* ty_string(void);
```

Each returns a pointer to a static `Type` singleton.

### §3.2 Compound constructors

```c
Type* ty_ref(Type* inner, bool is_mut);
Type* ty_rc(Type* inner);
Type* ty_rawptr(Type* inner);
Type* ty_array(Type* inner);
Type* ty_struct(TypeDef* tdef);
Type* ty_extern_struct(TypeDef* tdef);
Type* ty_fn(Type** params, int param_count, Type* ret);
Type* ty_enum(TypeDef* tdef);
```

Each `xcalloc`s a fresh `Type`. None of them checks its argument
for NULL; the callers are responsible.

### §3.3 Predicates

```c
const char* type_name(const Type* t);
bool        type_eq(const Type* a, const Type* b);
bool        type_contains_ref(const Type* t);
bool        type_has_value_representation(const Type* t);
bool        is_integer_type(const Type* t);
bool        is_signed_int(const Type* t);
bool        is_unsigned_int(const Type* t);
bool        is_float_type(const Type* t);
bool        is_scalar_type(const Type* t);
bool        is_equatable_type(const Type* t);
bool        is_ordered_type(const Type* t);
bool        cast_valid(const Type* from, const Type* to);
bool        int_value_fits(I64 v, Type* target);
bool        uint_value_fits(U64 v, Type* target);
bool        type_widens_to(const Type* from, const Type* to);
```

**`type_name`.** Renders a `Type` into a rotating ring of 8 static
buffers. Needed because recursive calls (a `ref` inside a `ref`)
would otherwise clobber each other's output. A caller that invokes
`type_name` more than 8 times in one expression sees the oldest
result overwritten. Struct and enum types return `tdef->name_buf`
directly, bypassing the ring.

**`type_eq`.** Structural equality. Fast path on pointer equality
(catches the primitive singletons). For `TY_RAWPTR`, `*void`
unifies with any other pointer type. For `TY_STRUCT` / `TY_ENUM` /
`TY_EXTERN_STRUCT`, equality is `tdef` pointer comparison
(nominal).

**`type_contains_ref`.** True if the type is a `TY_REF` or
contains one through `TY_RC`, `TY_RAWPTR`, `TY_ARRAY`. Does **not**
descend into struct or enum fields. Sound because field
registration rejects reference-typed fields.

**`type_has_value_representation`.** True iff the type is not an
extern struct. Extern structs live only behind pointers or as
fields of other extern structs.

**`cast_valid`.** Scalar-to-scalar, pointer-to-pointer,
pointer-to-integer, integer-to-pointer. Everything else is
rejected.

**`int_value_fits`.** Range check for an `I64` value against an
integer type. Every bound is written explicitly rather than
computed, so a reader can verify each one.

**`type_widens_to`.** The implicit widening relation. See
`language/reference.md` §5 for the table.

### §3.4 `TypeExpr` constructors and destructor

```c
TypeExpr* type_expr_name(Token name);
TypeExpr* type_expr_ref(TypeExpr* inner, bool is_mut, Span span);
TypeExpr* type_expr_rc(TypeExpr* inner, Span span);
TypeExpr* type_expr_rawptr(TypeExpr* inner, Span span);
TypeExpr* type_expr_array(TypeExpr* element, Span span);
TypeExpr* type_expr_qualified(Token* path, int n, Span span);
TypeExpr* type_expr_generic(Token base, Span span);
TypeExpr* type_expr_fn(Span span);
TypeExpr* type_expr_resolved(Type* t, Span span);

void type_expr_fn_add_param(TypeExpr* te, TypeExpr* param);
void type_expr_fn_set_return(TypeExpr* te, TypeExpr* ret);
void type_expr_generic_add_arg(TypeExpr* te, TypeExpr* arg);
void type_expr_free(TypeExpr* te);
```

Every constructor `xcalloc`s and sets kind and span. `path` is
copied into fresh heap memory. `args` grows by doubling from 4;
`params` grows by doubling from 4.

### §3.5 `TypeDefTable`

```c
typedef struct {
    TypeDef* items;
    int      count;
    int      capacity;

    void* generics;

    Type* (*instantiate_generic)(void* generics, TypeExpr* te,
                                 const SourceMap* smap, void* ctx,
                                 uint32_t file_id,
                                 DiagnosticBag* bag);
} TypeDefTable;

void     typedef_table_init(TypeDefTable* tt);
void     typedef_table_free(TypeDefTable* tt);
TypeDef* typedef_table_find(TypeDefTable* tt, const SourceMap* smap,
                            Token name);
TypeDef* typedef_table_find_in_file(TypeDefTable* tt, const SourceMap* smap,
                                    Token name, uint32_t file_id);
TypeDef* typedef_table_add(TypeDefTable* tt, Token name,
                           const SourceMap* smap);
TypeDef* builtin_tdef_by_name(const char* name);
```

**`generics`.** An opaque pointer. `types.c` passes it through to
the `instantiate_generic` callback; the concrete type is owned by
`monomorph.c`.

**`instantiate_generic`.** A function pointer installed by sema.
`type_expr_resolve` calls it whenever it encounters a `TEX_GENERIC`.

**`typedef_table_add`.** Grows by doubling from 8. Does not set
`kind`, `is_pub`, fields, or variants; the caller fills them in.

**`typedef_table_find`.** Skips `is_instantiated` entries.
**`typedef_table_find_in_file`.** Does not skip. This asymmetry is
deliberate.

### §3.6 Module namespace

```c
typedef struct { Token local_name; int module_file_id; } FileImport;

typedef struct {
    Token local_name;
    Token source_name;
    int   module_file_id;
} SelectiveImport;

typedef struct {
    uint32_t    file_id;
    FileImport* imports; int import_count; int import_capacity;
    SelectiveImport* selective; int selective_count; int selective_capacity;
} FileNamespace;

typedef struct {
    FileNamespace* files;
    int            file_count;
    int            file_capacity;
} ModuleTable;
```

`ModuleTable` is a flat array of `FileNamespace`, one per file
with imports. Lookups are linear scans. Used by
`type_expr_resolve` for `a.b.C` names and by sema's method
resolution.

### §3.7 Type expression resolution

```c
Type* type_expr_resolve(TypeExpr* te, const SourceMap* smap,
                        TypeDefTable* types,
                        const ModuleTable* modtable,
                        uint32_t file_id,
                        void* ctx,
                        DiagnosticBag* bag);
```

One entry point. Delegates to a private `resolve_inner`, which is
recursive on `te->inner` and `te->args`.

**Return value.** A `Type*` on success, NULL on any error. The
function reports at least one diagnostic before returning NULL.

### §3.8 Runtime type descriptors

```c
typedef struct {
    TypeDesc* items; int count; int capacity;
    uint32_t* fields; int field_count; int field_capacity;
    EnumVariantDesc* enum_variants; int enum_variant_count;
    int enum_variant_capacity;
    uint32_t* enum_payloads; int enum_payload_count;
    int enum_payload_capacity;
} TypeTable;

void typetable_init(TypeTable* tt);
void typetable_free(TypeTable* tt);
uint32_t typetable_intern(TypeTable* tt, const Type* t);
const TypeDesc* typetable_get(const TypeTable* tt, uint32_t idx);
```

`TypeTable` owns four parallel arrays: the descriptors themselves,
the concatenated struct field descriptors, the concatenated enum
variant names, and the concatenated enum payload descriptors.

**`typetable_intern`.** The bridge from `Type` to descriptor index.
Linear scan for a match via `desc_matches`, then append if no
match. Idempotent under `type_eq`. Passing NULL yields the index
of a default `TD_I64` descriptor.

**`typetable_get`.** Bounds-checked. Returns NULL for an
out-of-range index.

---

## §4 — Implementation notes

### §4.1 The resolver

`resolve_inner` is a switch over `TypeExprKind`. Its cases:

- `TEX_RESOLVED` — returns `te->resolved` directly.
- `TEX_GENERIC` — delegates to `types->instantiate_generic`.
- `TEX_NAME` — a four-step lookup: primitives (hardcoded string
  compare), file-local types, selective imports, then error.
- `TEX_QUALIFIED` — only `a.b` (two components) is supported.
  Looks up `a` in the enclosing file's module table and `b` in
  the target file's type table.
- `TEX_REF` / `TEX_RC` / `TEX_RAWPTR` / `TEX_ARRAY` — resolve
  `inner`, check `type_contains_ref(inner)`, wrap.
- `TEX_FN` — resolves each parameter and the return type.

Primitive names are checked first, before any table lookup. This is
why a user cannot shadow `i64` with their own type.

### §4.2 Interner phase structure

`typetable_intern` has two phases:

**Phase 1 — search.** Linear scan over `tt->items`, calling
`desc_matches` on each. Returns the first match.

**Phase 2 — insert.** Append a slot, set kind/size/count, then
fill kind-specific payload. Wrapper kinds recursively intern
`t->inner`. Structs append field descriptors to `tt->fields`.
Enums append variant names to `tt->enum_variants` and payload
descriptors to `tt->enum_payloads`.

The recursion may append more descriptors, so the new slot is
recorded before recursion (its index is captured, then count is
incremented, then sub-interning runs).

### §4.3 `desc_matches`

Compares an existing descriptor against a `Type`:

- Kind check first.
- For wrappers, compare the interned inner index.
- For structs, compare field count and each field's interned
  descriptor index.
- For enums, compare variant count, each variant's name via
  `strcmp`, and each payload's interned descriptor index.
- Scalars match on kind alone.

Struct equality is by shape (field descriptors). Enum equality
compares names and payload descriptors. This is more thorough
than the older version that compared only names; see
`handbook/changelog.md`.

---

## §5 — Invariants

- The 13 primitive constructors return the same pointers every
  time. Identity comparison on primitives is well-defined.
- Compound constructors produce fresh allocations. Identity
  comparison on compound types is not well-defined; `type_eq`
  must be used.
- Every `Type*` in the compiler is either a primitive singleton or
  a leaked compound allocation. No `Type*` is freed.
- `TypeDef.name_buf` holds a NUL-terminated copy of the type's
  mangled name, at most 127 characters.
- `TypeDef.is_instantiated` is true iff the type was produced by
  monomorphization.
- A `TypeExpr` owns its children; `type_expr_free` releases them
  exactly once.
- `TypeTable` is append-only. `typetable_intern` never removes or
  reorders entries.
- `typetable_intern(tt, t)` returns the same index for two `Type*`
  that compare equal under `type_eq`.
- A `TypeDesc` returned by `typetable_get` is read-only.

---

## §6 — Known limitations

**Compound `Type*` leak.** Every `ty_ref`, `ty_rc`, etc. leaks a
~48-byte struct. Bounded per compilation.

**`type_name` ring size.** 8 slots. More than 8 nested calls in
one expression cycles the buffer.

**`typedef_table_find` is O(n) in the number of types.** Fine at
current scales.

**`ModuleTable` inner arrays leak.** Sema's cleanup frees the outer
`files` array but not the per-namespace `imports` and `selective`
arrays. The code comment acknowledges this ("repeatedly interacted
badly with heap state"). Bounded by file count.

**`Type` identity is unstable for compound types.** A caller that
compares `Type*` with `==` on a compound type is comparing
allocation identities. Use `type_eq`.

**`type_contains_ref` does not descend into struct or enum
fields.** Sound because field registration rejects reference-typed
fields, but not obvious from the function name.

**Fixed-size name buffers.** `TypeDef.name_buf[128]`,
`EnumVariantDef.name_buf[64]`, `EnumVariantDesc.name_buf[64]`.
Truncation is silent.

**`TypeKind` and `TypeDescKind` are parallel but distinct.** A
future change to one must be mirrored in the other and in
`desc_kind_of`.

**`unmangle_name` in `types.c` is buggy for nested generics.**
`unmangle_name("Box#Option#i64")` produces `"Box<Option, i64>"`
instead of `"Box<Option<i64>>"`. It also reads past the string's
NUL. Not currently exercised; any future diagnostic that includes
such a name will be wrong.

**`typedef_table_add` leaves `kind = TDEF_STRUCT`** if the caller
forgets to set it.

**No interning of compound `Type*`.** Two calls with the same
arguments produce distinct pointers; `typetable_intern` re-interns
on each call.

**`ModuleTable`'s line count** grows with the number of files, not
the number of imports, so a single file with many imports has a
linear scan over a small table.

---

## §7 — Extension points

- **Interning compound `Type*`.** A hash table keyed on
  `(kind, inner, is_mut, tdef)` would make identity comparison on
  compound types well-defined.
- **Freeing compound types.** Requires an ownership model for
  `Type*` that does not exist.
- **A `type_dump` for debugging.** Renders a `Type` in its
  source-syntax form.
- **Hash-based name lookup.** If the type count grows past a few
  hundred per compilation.
- **Fixing `unmangle_name`.** A recursive renderer for the
  generic case.
- **Consolidating `tok_eq` into `source_map.c`.** Six private
  implementations exist across the tree.
- **`TypeDesc` for functions.** Currently `TY_FN` maps to `TD_I64`
  because function values are codegen indices. A first-class
  function descriptor would need a new kind.
- **Enum payload descriptors are interned but not exposed for
  lookup.** A future type-aware tool would need a `get_payload`
  accessor.
