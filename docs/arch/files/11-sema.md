## `docs/arch/files/11-sema.md`

# Semantic analysis — `sema.h`, `sema.c`, `sema_modules.c`,
# `sema_ffi.c`, `sema_types.c`, `sema_mono.c`, `resolve.h`,
# `resolve.c`, `sema_internal.h`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

Sema is the middle of the compiler. It reads the parser's
unannotated AST and produces the same AST with every name
resolved, every type checked, every local assigned a slot, every
generic instantiated, and every escape-rule violation reported.

**What it solves.** Name resolution, type checking, local
allocation, slot assignment, escape-rule enforcement, constant
registration, generic instantiation, module namespace construction,
FFI table construction, exhaustiveness checking, and per-node
type annotation.

**What it does not solve.** It does not parse (the parser does
that), it does not emit bytecode (codegen does that), and it does
not resolve generic type arguments ahead of demand (instantiation
is on-demand, triggered by use sites).

**Who depends on it.** Codegen reads the annotated AST and the
`SemaCtx`'s function table. Everything else — parser, monomorph,
types, chunk — is either upstream or downstream of sema.

**The single biggest fact.** Sema mutates the AST in place. There
is no separate "typed AST" type. Every field sema fills in —
`type`, `local_index`, `const_id`, `holds_rc`, `func_idx`,
`ffi_idx`, `builtin_id`, `tag`, `offset`, `sdef_index`,
`tdef_index`, `unit_tag`, `hidden_slot`, the various slot
fields — lives on the parser's `AstNode`. Codegen reads them
directly.

---

## §2 — The six-file split

Sema is one logical pass implemented across six files. The split
is by responsibility, not by pass.

| File | Responsibility |
|---|---|
| `sema.c` | The orchestrator and body analysis. The largest file. |
| `sema_modules.c` | Module table construction and validation. |
| `sema_ffi.c` | FFI table construction, type translation. |
| `sema_types.c` | Nominal type registration, C-ABI layout. |
| `sema_mono.c` | Generic instantiation and the cache. |
| `resolve.c` | Centralized name resolution. |

The internal header `sema_internal.h` declares the cross-file
entry points. Nothing outside sema includes it. The public header
`sema.h` declares only `sema_analyze` and the two state structs
(`SemaCtx`, `FnLocal`) that codegen needs to see.

The split is not load-bearing for correctness; the passes could
all live in one file. It exists because `sema.c` alone was
~5000 lines, and the file was easier to navigate when split. A
reader who wants the whole picture should read `sema_analyze`
first (`sema.c`), then follow its calls outward.

---

## §3 — Public interface

### §3.1 The one public function

```c
bool sema_analyze(AstNode* program, const SourceMap* smap,
                  ConstTable* consts, TypeDefTable* structs,
                  DiagnosticBag* bag, FFITable* ffi,
                  SemaCtx* sc);
```

**Preconditions.** `program` is the merged AST_PROGRAM with the
prelude prepended. `smap` is the same map used during parsing.
`consts`, `structs`, `bag`, and `ffi` are caller-initialized.
`sc` is caller-allocated.

**Return.** `true` iff no errors were reported. Warnings and
notes do not affect the return value; callers may inspect the
bag.

**Postconditions.** Every expression node has a non-NULL `type`.
Every `AST_VAR` has exactly one of `local_index >= 0` or
`const_id >= 0`. Every `AST_LET` and `AST_VAR` whose type is
`rc<T>` or `string` has `holds_rc == true`. Every `AST_FUNC` has
`func_idx >= 0` (concrete) or was registered as a template
(generic). The `structs` table's `generics` is non-NULL and its
`instantiate_generic` callback is installed. Every generic
declaration has been registered as a template.

**On failure.** Sema does not stop on the first error. It
reports as many as it can and continues. On a severe structural
error (an unresolvable type, an undefined name), the affected
node's `type` may remain NULL; downstream passes are not run
because the driver checks `diag_has_errors` before proceeding.

**Idempotency.** The function is not idempotent. Calling it
twice on the same program would re-analyze bodies, re-register
types, re-register constants, and re-instantiate templates.
Callers do not call it twice.

### §3.2 The state structs

```c
typedef struct {
    Token name;
    Type* type;
    int   slot;
    bool  holds_rc;
    bool  is_mut;
} Local;

typedef struct {
    Token name;
    int   param_count;
    Type* param_types[8];
    Type* return_type;
    bool  return_annotated;
    uint32_t file_id;
    bool     is_pub;
    bool     is_instantiated;
    bool     is_extern;
    bool     is_variadic;
    int      ffi_idx;
    TypeDef* method_owner_tdef;
    AstNode* decl;
    char     name_buf[128];
} FuncSig;

typedef struct {
    FuncSig          funcs[MAX_FUNCS];
    int              func_count;
    const SourceMap* smap;
    ConstTable*      consts;
    TypeDefTable*    structs;
    ModuleTable      modtable;
    uint32_t         prelude_file_id;
} SemaCtx;

typedef struct {
    Local    locals[MAX_LOCALS];
    int      local_count;
    int      scope_first[MAX_SCOPES];
    int      scope_depth;
    FuncSig* current_sig;
    uint32_t current_file;
    int      loop_depth;
} FnLocal;
```

**`Local`.** One local binding. `name` is a zero-valued `Token`
for compiler-generated slots (pattern temporaries, cached
end-values, hidden scrutinees). `slot` duplicates the array index
at declaration time. `holds_rc` marks bindings that own an RC
reference and must be dropped at scope exit. `is_mut` reflects
the source's `let mut` or `mut` parameter.

**`FuncSig`.** One function signature. `is_instantiated` marks
monomorphized copies (skipped by `find_func` during unqualified
lookup). `is_extern` marks externs (which have no body and are
called through `OP_FFI_CALL`). `ffi_idx` is the FFI table index
for externs, `-1` otherwise. `method_owner_tdef` is the
`TypeDef*` for methods, NULL for free functions. `decl` points at
the AST_FUNC node (for templates, at the monomorphized copy).
`name_buf` holds the printable name (mangled for instantiations).

**`SemaCtx`.** Per-compilation state. Lives across every
function body. Owns the module table (freed at the end of
`sema_analyze`). Borrows everything else.

**`FnLocal`.** Per-function state. Created fresh inside
`analyze_func_body`, discarded on return. This split is what
makes nested body analysis (a template instantiated from inside
another body) reentrant: the inner `analyze_func_body` creates
its own `FnLocal` and the outer one's is untouched.

### §3.3 Name resolution (`resolve.h`)

```c
typedef enum {
    RD_NONE = 0,
    RD_FUNC,
    RD_TYPE,
    RD_TEMPLATE,
    RD_CONST,
    RD_MODULE,
    RD_BUILTIN_TYPE,
} ResolveKind;

typedef struct {
    ResolveKind kind;
    int         index;
    TypeDef*    builtin;
} ResolvedDecl;

ResolvedDecl resolve_name(const SemaCtx* sc, uint32_t file_id, Token name);
ResolvedDecl resolve_qualified(const SemaCtx* sc, uint32_t file_id,
                               Token alias, Token name);
ResolvedDecl resolve_template(const SemaCtx* sc, uint32_t file_id, Token name);
ResolvedDecl resolve_in_module(const SemaCtx* sc, uint32_t file_id, Token name);
TypeDef* resolved_tdef(const SemaCtx* sc, ResolvedDecl rd);
```

**`ResolvedDecl`.** A tagged result. The `index` field's meaning
depends on `kind`:
- `RD_FUNC` — index into `sc->funcs`.
- `RD_TYPE` — index into `sc->structs->items`.
- `RD_TEMPLATE` — index into the monomorphizer's template list.
- `RD_CONST` — index into the constant table.
- `RD_MODULE` — the target `file_id`.
- `RD_BUILTIN_TYPE` — index is `-1`; the `builtin` field points
  at the primitive's `TypeDef`.

**`resolve_name`.** Resolve a bare name in a file's scope:
declarations in the file, then selective imports, then builtin
types, then the prelude.

**`resolve_qualified`.** Resolve `alias.name` where `alias` names
an imported module.

**`resolve_template`.** Like `resolve_name` but restricted to
`RD_TEMPLATE` results.

**`resolve_in_module`.** Resolve a name in a specific file,
bypassing the caller's scope. Used by import validation.

**`resolved_tdef`.** Extract the `TypeDef*` from a `ResolvedDecl`
whose kind is `RD_TYPE` or `RD_BUILTIN_TYPE`. Returns NULL for
any other kind.

---

## §4 — The five passes

`sema_analyze` runs five passes in a fixed order. The order is
load-bearing; changing it would break specific features.

### §4.1 Before the passes

Three steps run before any pass:

1. **`build_module_table`.** Walks the merged program's
   `AST_USING` declarations and populates `sc->modtable`. Runs
   first because the resolver needs the module table for
   qualified-name lookups.
2. **`cache_reset`.** Frees the instantiation cache from a
   previous compilation. The cache is a file-static in
   `sema_mono.c`; this entry point is the only way to clear it.
3. **Monomorphizer hookup.** If `structs->generics` is NULL,
   call `mono_init`. Install `sema_instantiate_generic` as the
   `instantiate_generic` callback on the type table. This is what
   makes generic resolution work: when `types.c` encounters a
   `TEX_GENERIC`, it calls back into sema.
4. **Library table.** `build_library_table` populates a
   `LibraryTable` from the program's `AST_EXTERN_LIB`
   declarations; then the entries are copied into the caller's
   `FFITable.libs[]`. Runs before Pass A because Pass A resolves
   `from alias` clauses against it.

### §4.2 Pass 0 — nominal type registration

Walks top-level declarations. For each `AST_STRUCT_DEF` or
`AST_ENUM_DEF`:

- If the declaration has type parameters, register it as a
  template via `mono_register_template`. Do not add it to the
  concrete type table.
- Otherwise, check for a duplicate name (via
  `typedef_table_find`), then add a `TypeDef`. Set `kind`,
  `is_pub`, `is_rc_managed` (from `is_builtin`), initial size,
  alignment. Record `sdef_index` or `tdef_index` on the AST node.

The pass is implemented by `register_struct_or_enum` in
`sema_types.c`.

**Why it runs first.** Type registration must complete before
any other pass because every other pass (fields, signatures,
bodies) can reference a type declared later in the source.

### §4.3 Pass 0b — fields, variants, layout

Walks top-level declarations. For each concrete (non-template)
struct and enum:

- **Structs.** `resolve_struct_fields` resolves each field's
  `TypeExpr`, checks for rejected field types (`ref<T>`, extern
  structs by value), assigns an offset
  (`8 + field_count * 8`), and appends to `td->fields`.
- **Enums.** `resolve_enum_variants` resolves each variant's
  payload types, checks for duplicates and rejected payloads,
  assigns tags in source order, and appends to `td->variants`.

Extern structs are handled separately by `resolve_extern_structs`,
which runs before the main Pass 0b loop. It resolves all field
types first, then lays out to a fixpoint. The fixpoint handles
forward references between extern structs: a struct whose
nested extern struct field hasn't been sized yet is retried on
the next pass. Any struct still unsized at the end is part of a
cycle and is reported.

**Why it's separate from Pass 0.** A struct field can reference
a type declared later in the same file. Pass 0 registers every
type name first, so Pass 0b's resolution always finds a name.

### §4.4 Pass A — signatures and constants

One walk over top-level declarations. Order matters within the
loop.

**Extern functions.** For each `AST_FUNC` with `is_extern`:

1. Look up the `from` alias in the library table.
2. Resolve the return type. Special case: `Option<*T>` is
   recognized by `is_option_ptr_type` and mapped to `FFI_PTR`
   with a flag.
3. Resolve each parameter type; same `Option<*T>` special case;
   reject extern-struct-by-value.
4. Register the `FFIFunc` in `ffi->funcs[]`.
5. Register a `FuncSig` in `sc->funcs[]`. Set `is_extern = true`,
   `ffi_idx`, `return_annotated = true`.
6. Record `func_idx` on the AST node.

**Regular functions.** For each `AST_FUNC` without `is_extern`:

- If `type_param_count > 0`, register as a template.
- Otherwise, register a `FuncSig`. Resolve parameter types
  (unannotated defaults to `i64`). Resolve the return type if
  annotated; else set `return_type = NULL` if `has_value_return`
  is true, otherwise `ty_void()`.
- Detect methods: if `method_owner` is a real token, look up the
  owner `TypeDef` and record it in `method_owner_tdef`.
- Check for duplicate names: for free functions, only within the
  same file; for methods, on the same owner regardless of file.

**Constants.** For each `AST_CONST`:

1. Extract the initializer via `const_from_literal` (accepts
   `AST_INT`, `AST_FLOAT`, `AST_BOOL`, `AST_UNARY(UN_NEG)`, and
   rejects `AST_STRING` with a dedicated message).
2. If the declaration has a type annotation, resolve it and check
   that the literal fits (or matches the kind).
3. Register the constant in the caller's `ConstTable`.
4. Record `const_id` on the AST node.

**Why Pass A must complete before Pass B.** A function body can
call a function declared later in the source. Pass A populates
the function table for every declaration before Pass B begins.

### §4.5 Selective import validation

`validate_selective_imports` walks `AST_USING` declarations with
`USING_SELECTIVE` and, for each imported name:

- Look up the source name in the target module via
  `resolve_in_module`.
- If the name resolves to a function, check `is_pub`.
- If the name resolves to a type, accept (visibility is checked
  at use sites).
- Otherwise, report "module 'X' has no item 'Y'".

**Why it runs after Pass A.** The function table must be
populated before the module can be checked for the imported name.

### §4.6 Pass B — bodies

For each non-extern, non-template `AST_FUNC`:

1. Create a fresh `FnLocal`, set `current_sig` and `current_file`.
2. `enter_scope`.
3. Declare parameters as locals at slots `0..param_count-1`.
   Parameters are `holds_rc = false` (they're borrowed).
4. `analyze_stmt` on the body.
5. If the function was unannotated and no value-return established
   a return type, set `return_type = ty_void()`.
6. Propagate `return_type` to the AST node.
7. `exit_scope`.

**Order.** Bodies are analyzed in source order. This matters
for return-type inference: a function whose return type is
inferred from its body becomes visible to callers analyzed later
in the same pass. Callers analyzed earlier would see
`return_type == NULL` and report `cannot infer return type at
this call site`.

---

## §5 — Body analysis

Body analysis is the largest single subsystem. It lives in
`sema.c` and is implemented by `analyze_expr`, `analyze_stmt`,
and `analyze_pattern`, plus a set of helpers.

### §5.1 `analyze_expr`

A large switch over `AstKind`. The contract: on success, returns
the resolved `Type*` and writes it to `n->type`. On failure,
returns NULL and has reported at least one diagnostic.

Every case either annotates the node with its resolved type or
sets resolution indices (`local_index`, `const_id`, `func_idx`,
`ffi_idx`, `builtin_id`, `tag`, `offset`, `sdef_index`,
`tdef_index`, `unit_tag`). The recursion order matches source
evaluation order: left, then right for binary; operand first for
unary; receiver first, then args for calls.

### §5.2 The literal adaptation pattern

The mechanism that lets an integer literal take the type of a
non-literal operand.

```c
static bool is_adaptive_literal(const AstNode* n);
static void pre_type_literal(AstNode* e, Type* target);
```

`is_adaptive_literal` returns true for an untyped `AST_INT` or
`AST_FLOAT`. `pre_type_literal` writes the target type onto the
literal if it fits (or the target is a float and the literal is a
float). The literal's `analyze_expr` case then skips its default
`ty_i64` assignment.

The pattern is applied at four call sites:

- **`AST_LET`.** If the annotation exists and the initializer is
  an adaptive literal, pre-type it before analysis.
- **`AST_BINARY`.** If one operand is an adaptive literal and the
  other is not, analyze the non-literal first, then pre-type the
  literal to its type.
- **`AST_FOR_RANGE`.** Same as binary: analyze the non-literal
  bound first, pre-type the literal.
- **Function calls, struct literals, variant constructors,
  array literals.** The parameter type, field type, or element
  type is known from the resolved signature or definition, so
  the literal is pre-typed.

**Why it exists.** Without it, every integer literal would
default to `i64`, and `let x: u8 = 5;` would fail with a type
mismatch. The adaptation makes literals context-sensitive.

### §5.3 Widening coercion

```c
static bool coerce_to_target(AstNode** slot, Type* target, Type* got,
                             Span span, DiagnosticBag* bag);
```

Called at every site that checks "does this value fit this
target". Behavior:

- If `type_eq(target, got)`, no-op, return true.
- Else if `type_widens_to(got, target)`:
  - If no runtime conversion is needed (i8 → i64), no-op.
  - If a conversion is needed (i32 → f64, f32 → f64), wrap
    `*slot` in an `AST_CAST` node.
  - Return true.
- Else report `cannot implicitly convert X to Y` and return false.

The cast insertion is a mutation of the AST during sema. The cast
node carries the target type and a `type_expr_resolved` for the
syntactic side, so codegen sees a well-formed cast.

**Note on the double-coerce bug.** In an earlier revision,
`AST_LET` called `coerce_to_target` twice on the same pair,
which produced a `CAST(CAST(x))` for widening conversions. The
current code calls it once at the end of the case, after the
void check.

### §5.4 The RC-managed enum wrapping

`Result` and `Option` are declared in the prelude with
`is_builtin = true`. `register_struct_or_enum` (in
`sema_types.c`) copies `is_builtin` to `TypeDef.is_rc_managed`.

Every use site of an RC-managed type is wrapped in `ty_rc` by
the resolver. So a field of type `Option<i64>` has `Type*` whose
`kind == TY_RC` and whose `inner` is a `TY_ENUM`.

Three code paths need to unwrap one level to see the underlying
enum:

- **`analyze_pattern` (variant patterns).** The pattern's
  scrutinee type is unwrapped before checking for `TY_ENUM`.
- **`analyze_expr` (`AST_VARIANT_CTOR`).** The constructed type
  is wrapped if `td->is_rc_managed`.
- **`analyze_expr` (`AST_FIELD` unit variant).** Same wrapping.

This is the "three-use-site unwrapping" fragility noted in
`invariants.md` §10.2. A fourth site would need to remember the
unwrap. The structural fix — making `is_rc_managed` transparent
at the type level — is under consideration but not implemented.

### §5.5 Method resolution

A method call `recv.name(args)` is produced by the parser as an
`AST_VARIANT_CTOR` with `receiver_expr` set. `analyze_expr`'s
`AST_VARIANT_CTOR` case handles it:

1. Analyze `receiver_expr`.
2. Unwrap one level of `TY_RC`, `TY_REF`, or `TY_RAWPTR` to get
   the base type.
3. Require a `TypeDef` (a nominal type).
4. Look up a `FuncSig` in `sc->funcs[]` whose
   `method_owner_tdef` matches the receiver's `tdef` and whose
   name matches `variant_name`.
5. Check arity against `sig->param_count - 1` (the receiver is
   parameter 0).
6. Type-check the non-receiver args.
7. Set `mod_func_idx` (or `mod_ffi_idx` for extern methods).
8. Result type is the method's return type.

### §5.6 The `?` operator

`AST_TRY` (produced by the parser for postfix `?`) is
desugared at the sema level. The constraints:

- The enclosing function must have an annotated return type
  that resolves to `Result<_, _>` or `Option<_>`.
- The operand must resolve to `rc<Result<...>>` or
  `rc<Option<...>>`.
- If both the operand and the return type are `Result`, their
  error payload types must match.

The result type is the operand's first variant's first payload
(`Ok`'s payload or `Some`'s payload).

**Recognition by name prefix.** The check for "is this Result"
uses `strncmp(outer_td->name_buf, "Result", 6) == 0`. This works
because `mono_mangle` produces names like `Result#i64#string`. A
user-defined enum whose mangled name happened to start with
`Result` would be misrecognized; this is not reachable because
`?`'s operands come from function returns, which come from
signatures that name `Result` explicitly.

### §5.7 Statements

`analyze_stmt` is the second-largest function. It dispatches over
`AstKind` and handles: `AST_BLOCK`, `AST_LET`, `AST_ASSIGN`,
`AST_ASSIGN_OP`, `AST_IF`, `AST_WHILE`, `AST_FOR_RANGE`,
`AST_FOR_IN`, `AST_BREAK`, `AST_CONTINUE`, `AST_MATCH`,
`AST_RETURN`, `AST_EXPR_STMT`.

The `AST_LET` case is the most complex. It resolves the
annotation if present, pre-types the initializer if it's an
adaptive literal, analyzes the initializer, coerces to the
annotation's type, computes `holds_rc`, and declares the local.

The `AST_RETURN` case enforces four rules:

- No `ref<T>` return.
- No `*T` return.
- No `[T]`, `struct`, or `enum` return.
- No returned borrowed RC values.

The borrowed-value check uses a syntactic pattern: an
`AST_VAR` with `holds_rc == false` is borrowed; an `AST_FIELD`
with `unit_tag < 0` is borrowed; an `AST_INDEX` or `AST_DEREF`
is borrowed. Everything else is considered owned.

### §5.8 Scope management

```c
static bool enter_scope(FnLocal* fl, DiagnosticBag* bag, Span where);
static void exit_scope(FnLocal* fl);
static int  declare(FnLocal* fl, Token name, Type* type, bool holds_rc,
                    bool is_mut);
static int  declare_anon(FnLocal* fl, Type* type);
static int  resolve(FnLocal* fl, const SourceMap* smap, Token name);
```

`enter_scope` records the current `local_count` on the scope
stack. `exit_scope` restores it. `declare` appends a named local;
`declare_anon` appends an unnamed local. `resolve` scans backward
through `locals[]` for a name match.

Anonymous locals have a zero-valued `Token` name (file_id == 0),
which no source name can match. They are used for pattern
temporaries, cached end-values, match scrutinees, and for-in
loop state.

---

## §6 — Nominal type registration (`sema_types.c`)

### §6.1 `register_struct_or_enum`

Handles Pass 0 for one declaration. Skips generic declarations
(they go to the monomorphizer). Checks for duplicate names.
Creates a `TypeDef` via `typedef_table_add` and fills in the
kind-specific initial state.

For an extern struct, `kind = TDEF_EXTERN_STRUCT`, `struct_size
= 0` and `struct_align = 1` (both filled in by
`resolve_extern_structs`). For a regular struct, `kind =
TDEF_STRUCT`, `struct_size = 8`, `struct_align = 8`. For an enum,
`kind = TDEF_ENUM`.

`is_rc_managed` is copied from `decl->is_builtin`. Only the
prelude's declarations have `is_builtin == true`.

### §6.2 `resolve_struct_fields`

Resolves each field's `TypeExpr`, checks for rejected field types
(`ref<T>`, extern structs by value), assigns an offset
(`8 + field_count * 8`), and appends to `td->fields`.
`struct_size` grows by 8 per field.

### §6.3 `resolve_enum_variants`

Resolves each variant's payload types, checks for duplicate
variant names, caps payload count at 16 (matching the AST's
`tmp_slots[16]`), checks for rejected payload types, assigns tags
in source order. `payloads` is a fresh `Type**` array per
variant.

### §6.4 `resolve_extern_structs`

The C-ABI layout fixpoint. Two passes:

**Pass 1.** Resolve each extern struct's field types. Reject
`ref<T>`, `rc<T>`, `string`, regular `struct`/`enum`, opaque
extern structs by value. Accept the C-compatible leaf set:
integers, floats, pointers, nested extern structs.

**Pass 2.** Lay out each struct whose extern struct fields are
already sized. Iterate to a fixpoint. For a union
(`is_union == true`), every field is at offset 0 and the size is
the maximum field size rounded up to the alignment. For a
struct, fields are laid out at natural-aligned offsets.

The fixpoint is what makes forward references between extern
structs work without a topological sort.

---

## §7 — Module table (`sema_modules.c`)

### §7.1 `build_module_table`

Walks the merged program's `AST_USING` declarations. For each
non-alias `using`, records the import into the appropriate
`FileNamespace`.

Three cases for a `USING_MODULE`:

- `resolved_path_count == path_count` — a whole-module import.
  Add to `imports[]`.
- `resolved_path_count == path_count - 1` — a single trailing
  symbol. Sugar for a selective import; add to `selective[]`.
- Anything else — nested symbol paths are not supported.

For a `USING_SELECTIVE`, every entry in `names[]` becomes one
selective import.

`FileNamespace` entries are keyed by the file that contains the
`using`, not by the target file.

### §7.2 `validate_selective_imports`

Walks `AST_USING` declarations with `USING_SELECTIVE`. For each
imported name, look up the source name in the target module via
`resolve_in_module` and check visibility.

### §7.3 The `modtable_get_or_add` growth

`FileNamespace`s grow by doubling from 8. `imports[]` and
`selective[]` each grow by doubling from 4.

---

## §8 — FFI construction (`sema_ffi.c`)

### §8.1 `build_library_table`

Walks `AST_EXTERN_LIB` declarations. Extracts the path from the
string literal's span (stripping the quotes). Derives the alias
from the basename if `as` is not written.

Hard cap: 16 libraries.

### §8.2 `ffi_kind_from_type`

Maps a `Type*` to an `FFIType`. Accepts: `void`, the eight
integer types, `f32`, `f64`, `bool`, `*T`. Rejects everything
else.

### §8.3 `is_option_ptr_type`

Recognizes `Option<*T>`: an `rc<Enum>` whose `TypeDef`'s mangled
name starts with `"Option#"`, whose first variant has exactly one
payload, and that payload is a `TY_RAWPTR`.

The string prefix is the recognition mechanism. It is fragile in
principle but sound in practice because `Option#...` can only be
produced by the prelude's `Option` template.

### §8.4 `is_ffi_variadic_ok`

Returns true for integers, `bool`, and raw pointers. Rejects
floats (the C default-promotion rules require float-to-double
promotion, which `ffi_prep_cif_var` does not apply
automatically).

---

## §9 — Generic instantiation (`sema_mono.c`)

### §9.1 The instantiation cache

```c
typedef struct {
    GenericTemplate* tmpl;
    Type**           args;
    int              arg_count;
    TypeDef*         type_result;
    int              func_result;
} InstEntry;

typedef struct {
    InstEntry items[MAX_INST_CACHE];
    int       count;
} InstCache;

static InstCache g_cache;
```

`MAX_INST_CACHE = 256`. The cache is a file-static (global to the
compilation, not per-`SemaCtx`). `cache_reset` frees the cached
`args` arrays and zeroes the cache; it runs at the top of every
`sema_analyze`.

Lookup: linear scan with `tmpl` pointer identity and `type_eq`
on the args.

Insert: if the cache is full, returns NULL. Both instantiation
functions handle a NULL return by skipping the cache update but
still completing the instantiation. Cache overflow produces
duplicate instantiations, which are correct but waste memory.
Not observed in practice.

### §9.2 `sema_instantiate_generic`

Installed as the `instantiate_generic` callback on the
`TypeDefTable`. Called from `types.c`'s `resolve_inner` when a
`TEX_GENERIC` is resolved.

Flow:

1. Resolve each type argument via `type_expr_resolve`. Cap at 8.
2. Find the template (`mono_find_template`, then
   `mono_find_template_in_file` as fallback).
3. Check arity.
4. Cache lookup.
5. Deep-copy the template's AST with substitution.
6. Register a new `TypeDef` with the mangled name. Set
   `is_instantiated = true`, `is_rc_managed` from the template's
   `is_builtin`.
7. Cache the result.
8. Resolve the copy's fields or variants against the concrete
   substitution. Fill in the `TypeDef`'s `fields` or `variants`.
9. Free the AST copy.
10. Return the type, wrapped in `ty_rc` if `is_rc_managed`.

**Why step 7 is on the copy, not the `TypeDef`.** The AST copy
carries the type expressions that need resolving. The `TypeDef`
only holds the resolved results.

**Why the copy is freed.** The `TypeDef` holds everything
codegen will need. The AST copy's only remaining purpose was to
carry the syntactic field/variant data during resolution.

### §9.3 `sema_instantiate_function`

Called from `analyze_expr`'s `AST_CALL` case when the call has an
explicit type-argument list.

Flow is parallel to the type instantiation, with two differences:

- The result is an index into `sc->funcs[]`, not a `TypeDef*`.
- The AST copy is **not freed**. It's stored as `sig->decl` and
  the body is analyzed against it.

The body analysis calls `analyze_func_body`, which is the same
function used for concrete declarations. This is what makes an
instantiated function behave exactly like a source-written one.

**Ordering note.** The cache entry's `func_result` is set
*before* `analyze_func_body` runs. This prevents infinite
recursion on self-recursive generic functions: the recursive call
finds the cache entry and returns its index without
re-instantiating.

The earlier code (before the fix) set `func_result` *after*
body analysis, which caused infinite loops on recursive
generics. See `handbook/changelog.md`.

### §9.4 `instantiate_option_of`

Helper used by builtins whose return type is `Option<something>`
(currently only `read_line`). Iterates the template list looking
for a template named `Option`, then constructs a `TEX_GENERIC`
with one resolved argument and calls `instantiate_generic`.

---

## §10 — Name resolution (`resolve.c`)

### §10.1 `find_in_scope`

The full "declared in this file" lookup, in precedence order:

1. Functions (`find_func_in_scope`).
2. Types (`find_type_in_scope`).
3. Templates (`find_template_in_scope`).
4. Constants (`find_const_in_scope`).

Every stage filters by `file_id` and skips `is_instantiated`
entries for funcs and types.

The precedence order is arbitrary but fixed. Changing it would
change which declaration shadows which for the (rare) case of a
name reused across kinds.

### §10.2 `find_selective`

Given a name, look up the enclosing file's `FileNamespace` and
find a matching selective import. On a match, delegate to
`find_in_scope` for the target file.

### §10.3 `resolve_name`

Four steps:

1. `find_in_scope` for the current file.
2. `find_selective` for selective imports.
3. `builtin_tdef_by_name` for primitive types.
4. `find_in_scope` for the prelude's `file_id`.

**Why primitives come after the file's own declarations.** The
language allows a user to shadow a builtin with a `type` named
after it. In practice no test exercises this and no realistic
program does, but the ordering is deliberate.

**Why the prelude is last.** User code can shadow prelude
declarations, though no realistic program does.

### §10.4 `resolve_qualified`

Looks up `alias` in the current file's `imports[]`, gets the
target file's `file_id`, then calls `find_in_scope` for the
target file.

### §10.5 `resolve_in_module`

Direct `find_in_scope` on a specific file, bypassing the caller's
scope. Used by import validation.

---

## §11 — Cross-cutting concerns

### §11.1 The instantiation reentrancy

Instantiating a generic from inside another function's body
creates a fresh `FnLocal` for the copy. This is what makes
nested body analysis safe. The `SemaCtx` is shared (function
table, type table, constant table, module table); the `FnLocal`
is per-body.

Any code that calls `analyze_func_body` from inside a body
walk (currently only `sema_instantiate_function`) must not
touch the outer `FnLocal`. The design makes this impossible by
not passing the outer `FnLocal` into the inner call.

### §11.2 The `codegen_of_sema` bridge

Sema's function table (`sc->funcs[]`) includes externs and
instantiated generics. Codegen's function indices exclude both.
The bridge is `FuncSig.codegen_idx` (set by codegen) and the
`codegen_of_sema[]` array (built by `codegen_program`). Any
`AST_CALL` operand that will become an `OP_CALL`'s func index
must go through this mapping. `AST_VAR` used as a function value
also uses it (`as.var.func_idx` carries a sema index; codegen
maps it).

### §11.3 The literal adaptation and RC-managed enums interact

A `let x: Option<i64> = Option.Some(5);` where the initializer
is a bare variant constructor: the annotation resolves to
`rc<Option#i64>` and pre-types nothing (variant constructors
aren't literals). The constructor's own analysis sees the
annotation's type via the enclosing function's return-type
context, or via `resolve_name` finding `Option` in the prelude.

Both mechanisms are in play; a reader tracing a specific case
should check whether the annotation, the enclosing return type,
or the prelude supplied the enum's type arguments.

### §11.4 The prelude's role

The prelude's declarations are prepended to the merged program by
`main.c` before `sema_analyze`. From sema's perspective, they are
ordinary top-level declarations with `is_builtin = true`.

`find_prelude_file_id` (called at the top of `sema_analyze`)
locates the prelude's `file_id` in the `SourceMap` by matching
`"<prelude>"` against each file's path. This is stored in
`sc->prelude_file_id` and used by `resolve_name` as the last
fallback.

---

## §12 — Invariants

- Every `Local.slot` equals its index in `FnLocal.locals[]` at
  the time of `declare`.
- `FnLocal.local_count` after `exit_scope` equals its value
  before the corresponding `enter_scope`.
- Every `AST_VAR` after sema has `local_index >= 0` XOR
  `const_id >= 0`, unless a diagnostic was reported.
- Every `FuncSig` is registered in `sc->funcs[]` with
  `param_count <= 8` and `param_types[i] != NULL` for
  `i < param_count`.
- Every instantiated `TypeDef` has `is_instantiated == true`.
- Every `TypeDef` produced by monomorphization has a mangled
  `name_buf`.
- Every generic declaration with `type_param_count > 0` is
  registered as a template.
- Every concrete function in the merged program is either
  registered in `sc->funcs[]` or was reported as a duplicate.
- The instantiation cache is empty at entry and contains at most
  `MAX_INST_CACHE` entries at exit.
- Every `FuncSig.codegen_idx >= 0` corresponds to a function
  codegen will emit. (This invariant is enforced by codegen, not
  sema. See `invariants.md` I29.)

---

## §13 — Known limitations

**Self-recursive generic functions infinite-loop (fixed).** The
cache-insertion-before-body-analysis ordering was corrected. See
`handbook/changelog.md`. The current code is safe.

**Duplicate instantiation on cache overflow.** When the
instantiation cache is full, both instantiation functions skip
the cache update but still complete the instantiation. A second
call with the same arguments re-instantiates instead of returning
the cached result. Bounded by the number of distinct
instantiations.

**`USING_ALIAS` (`using Name = Type`) is a no-op.** It parses
and is stored in the AST, but `type_expr_resolve` does not
consult alias declarations.

**Selective imports of constants are not handled.** The
`AST_VAR` resolution path has a no-op branch for the
selective-import case. A constant imported selectively will not
resolve.

**Duplicate type names across files are rejected.** There is no
per-file type namespace. Two files cannot each declare a `Point`.

**`ModuleTable`'s inner `imports[]` and `selective[]` arrays are
not freed.** `sema_analyze` frees the outer `files` array but
explicitly leaks the per-namespace arrays. The code comment
acknowledges this ("repeatedly interacted badly with heap
state"). Bounded by file count.

**Instantiated function ASTs are leaked.** Deliberate; codegen
needs them.

**`has_value_return` is purely syntactic.** A `return 5;` inside
`if (false) { ... }` still forces return-type inference rather
than defaulting to void.

**`covered[128]` bounds exhaustiveness checking.** An enum with
more than 128 variants mis-checks. Rejected at registration time
by `MAX_ENUM_VARIANTS = 128`.

**No duplicate-function detection within a file.** Two functions
with the same name in the same file both get `FuncSig` entries;
`find_func` returns the first. The second is dead code.

**No per-file type namespaces.** See above.

**`sema_analyze` is not idempotent.** See §3.1.

**No forward reference to a local.** A `let` binding is visible
only after its statement; using a local before its declaration is
`undefined variable`.

**Error recovery for expression analysis is coarse.** An
expression that fails to resolve produces NULL, which propagates
up as "cannot infer type" or "internal: unexpected" errors in the
enclosing context.

**`is_option_ptr_type` uses a name prefix.** Sound in practice
because only the prelude produces `Option#...` names.

**The `?` operator's Result check uses a name prefix.** Same
caveat.

**No `?` in `Option<_>` functions to convert `Result`.**
Cross-type propagation is not supported.

---

## §14 — Extension points

- **Per-file type namespaces.** Replacing the global
  `typedef_table_find` duplicate check with a file-scoped one
  would allow same-named types in different files. Would require
  the `TypeDefTable` to track declaring files and would affect
  every name resolution site.
- **`using Name = Type` support.** Would require
  `type_expr_resolve` to consult the file's namespace for
  `USING_ALIAS` entries and substitute the target type.
- **Selective imports of constants.** Would require the
  constant table to carry a `file_id` or an equivalent.
- **Better exhaustiveness.** Track which values are covered (for
  integer and string patterns) or at least warn when a
  non-wildcard match on a non-exhaustible type appears.
- **Duplicate-function detection.** A check before inserting
  into `f->funcs` would catch the second declaration.
- **Freeing the module table's inner arrays.** See §13.
- **`is_rc_managed` transparency at the type level.** Move the
  wrapping into `type_expr_resolve` so that every consumer sees
  `rc<Enum>` uniformly, eliminating the three-use-site unwrap.
- **Better error recovery.** A synthetic "error type" for
  failed resolutions would reduce cascading diagnostics.
- **A `SemaOptions` struct.** The seven-argument signature of
  `sema_analyze` would benefit from consolidation if more
  options are added.
- **A file-scoped template visibility check.** Currently
  `mono_find_template` is file-agnostic.
- **Debug dumps of the annotated AST.** No printer exists.