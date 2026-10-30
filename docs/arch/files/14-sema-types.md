## `docs/arch/files/14-sema-types.md`
# `src/sema_types.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

Pass 0 (nominal type registration) and Pass 0b (fields, variants,
C-ABI layout) of semantic analysis. Registers every concrete
struct and enum in the `TypeDefTable`, resolves field and
variant types, and computes layouts.

**Called from:** `sema_analyze`'s Pass 0 and Pass 0b loops.

**What it does not do:** resolve type names (that's
`types.c`'s `type_expr_resolve`, invoked by this file). Register
generic declarations as concrete types (those go to the
monomorphizer).

---

## §2 — Public interface

```c
void register_struct_or_enum(AstNode* d, SemaCtx* sc,
                             DiagnosticBag* bag);

void resolve_struct_fields(TypeDef* td, AstNode* d, SemaCtx* sc,
                           DiagnosticBag* bag);

void resolve_enum_variants(TypeDef* td, AstNode* d, SemaCtx* sc,
                           DiagnosticBag* bag);

void resolve_extern_structs(AstNode* program, SemaCtx* sc,
                            DiagnosticBag* bag);
```

Private helpers: `c_size_of`, `c_align_of`.

---

## §3 — Implementation notes

### §3.1 `register_struct_or_enum` — Pass 0

Per declaration:

- Generic (`type_param_count > 0`): `mono_register_template`,
  return. The declaration does not get a `TypeDef` in this pass.
- Extern with type parameters: reject (extern types cannot be
  generic).
- Duplicate name (`typedef_table_find`): report, return.
- Otherwise: `typedef_table_add` and initialize kind, `is_pub`,
  `is_opaque`, `is_union`, `is_rc_managed` (from `is_builtin`),
  `struct_size`, `struct_align`. Record `sdef_index` or
  `tdef_index` on the AST node.

Initial sizes: `TDEF_STRUCT` starts at 8 bytes (the field-count
slot) and `struct_align = 8`. `TDEF_EXTERN_STRUCT` starts at 0
with `struct_align = 1`; `resolve_extern_structs` fills both.
`TDEF_ENUM` has no size (enums are always 8-byte pointers at
runtime).

### §3.2 `resolve_struct_fields` — Pass 0b for regular structs

For each field:

- Resolve the `TypeExpr`.
- Reject `TY_EXTERN_STRUCT` (by-value field), report.
- Reject `TY_REF` (would dangle).
- Assign `offset = 8 + field_count * 8`.
- Append to `td->fields`.
- Increment `td->struct_size` by 8.

The 8-byte stride is uniform: every field occupies one VM slot.
No packing, no sub-word fields, no alignment beyond 8.

### §3.3 `resolve_enum_variants` — Pass 0b for enums

For each variant:

- Reject duplicate names within the enum.
- Cap `payload_exprs.count` at `MAX_VARIANT_PAYLOADS` (16).
  Excess payloads are clamped; the diagnostic reports the
  original count.
- Allocate `vd->payloads` as a fresh `Type**` (with `pc ? pc :
  1` sizing to never call `xcalloc(0, …)`).
- Resolve each payload; reject `TY_REF` and `TY_EXTERN_STRUCT`.
- Assign `tag = variant_count - 1` (source order, 0-based).
- Mirror `payloads` and `tag` back onto the AST node.

### §3.4 `resolve_extern_structs` — C-ABI layout

Two passes over the extern struct declarations, collected into
arrays first.

**Pass 1 — field types.** For each field, resolve the type and
reject anything not in the C-compatible leaf set: `ref`, `rc`,
`string`, regular `struct`/`enum`, array, opaque extern structs
by value. Only the following pass:

- `i8`–`i64`, `u8`–`u64`, `f32`, `f64`.
- `*T` (any pointee, including opaque extern structs).
- Nested extern structs by value.

For every rejected field, the type is replaced with `ty_i64()`
so layout can continue; the diagnostic has already fired.

**Pass 2 — fixpoint layout.** Iterate over the collection until
no struct's layout changes. A struct is "ready" when every
nested extern struct field has been sized. Every iteration that
lays out at least one struct sets `progress = true` and the
loop continues.

For a struct (non-union):

```
offset = 0
max_align = 1
for each field:
    align = c_align_of(field.type)
    offset = align_up(offset, align)
    field.offset = offset
    append field to td->fields
    offset += c_size_of(field.type)
    max_align = max(max_align, align)
struct_size  = align_up(offset, max_align)
struct_align = max_align
```

For a union (`is_union`):

```
max_size = 0
max_align = 1
for each field:
    field.offset = 0
    max_size = max(max_size, c_size_of(field.type))
    max_align = max(max_align, c_align_of(field.type))
struct_size  = align_up(max_size, max_align)
struct_align = max_align
```

Any struct still at `struct_size == 0` after the fixpoint is part
of a cycle and is reported.

### §3.5 `c_size_of` / `c_align_of`

Both operate on the `Type*` of a field. They assume the caller
has already validated the field type against the C-compatible
set; the `default` cases return 8 and 1 respectively as
defensive fallbacks.

Nested extern structs delegate to the referenced `TypeDef`'s
`struct_size` and `struct_align`, which is why the fixpoint is
required: the sizes are not known until the nested struct has
been laid out.

---

## §4 — Invariants

- A `TypeDef` registered by `register_struct_or_enum` has
  `sdef_index` or `tdef_index` set on its AST node.
- `struct_size >= 8` for a regular struct with at least one
  field; every field's offset is `8 + 8 * i` for its index `i`.
- Every extern struct with a non-cyclic layout has
  `struct_size > 0` and `struct_align >= 1`.
- `EnumVariantDef.tag == variant_count - 1` at insertion time.
- Payload count per variant is `<= 16`.
- Duplicate variant names within one enum are impossible.
- `TypeDef.name_buf` is populated from the AST node's name
  token.

---

## §5 — Known limitations

**No sub-word struct fields.** Every field is 8 bytes. A
`Point { x: i8, y: i8 }` has `struct_size = 24`, not 10.

**No alignment-optimized ordering.** Fields are laid out in
source order, with gaps for alignment. A future reordering
pass would reduce `struct_size` for mixed-width structs.

**Duplicate names across files are rejected.** No per-file
type namespace.

**`resolve_extern_structs` collects declarations twice.** Once
to count, once to fill. Minor; a growable array would be one
pass, but the count is small.

**Extern structs with no fields are rejected as a diagnostic,
not a validation error at the ABI level.** The check
(`fl->count == 0`) reports and marks the struct as laid out
with `struct_size = 1`.

**No support for bit fields.** Not expressible in the source
grammar.

**No support for anonymous nested structs/unions.** Every extern
struct is a named type.

**The C-ABI layout assumes LP64 or LLP64.** On a platform with
a different C ABI, the layout algorithm would need revisiting.
Prism currently targets x86_64 Windows and Linux, both of which
use one of those models.

**`resolve_struct_fields` and `resolve_enum_variants` do not
check for `is_opaque` or `is_union` flags.** Only
`resolve_extern_structs` consults `is_union`.

---

## §6 — Extension points

- **Field reordering by alignment.** A post-pass over
  `td->fields` would reorder to pack tighter; changes the
  observable layout, so any change would need to be
  opt-in.
- **Sub-word field layouts.** Would require new VM memory
  access opcodes and updates to the field-load codegen.
- **Anonymous unions.** Would require a new field kind.
- **Bit fields.** Would require new source syntax, new layout
  rules, and new load/store opcodes.
- **Debug dumps of computed layouts.** No printer exists.
