## `docs/arch/files/08-monomorph.md`
# `include/monomorph.h`, `src/monomorph.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

The monomorphizer handles Prism's generics. It has two
responsibilities:

1. **Template storage.** A `GenericTemplate` records the AST of a
   generic declaration (a function, struct, or enum with one or
   more type parameters), the parameter names, and the file it
   was declared in.
2. **AST substitution.** `mono_copy_node` and `mono_copy_type_expr`
   deep-copy a subtree, replacing every reference to a parameter
   with a concrete `Type`.

**What it solves.** Prism handles generics by monomorphization:
each distinct instantiation gets its own copy of the AST with the
type parameters substituted. The copy is then compiled like any
non-generic declaration.

**What it does not solve.** Resolving concrete `Type*` arguments
(that is sema's job), registering the resulting `TypeDef` (that
is `sema_mono.c`'s job), or caching instantiations (that is
`sema_mono.c`'s `InstCache`). `monomorph.c` provides the storage
and the deep-copy primitive; the driver is `sema.c` /
`sema_mono.c`.

**Who depends on it.** Sema (which registers templates and calls
`mono_copy_node`), and `resolve.c` (which queries the template
list). Nothing outside sema calls these functions directly.

**Opacity.** The API uses `void*` for the generics handle so the
concrete `TemplateList` type stays private. Every public function
that takes `void* generics` casts it back on entry.

---

## §2 — Public interface

### §2.1 Data structures

```c
typedef struct {
    AstNode* decl;
    Token    name;
    Token    params[8];
    int      param_count;
    uint32_t file_id;
    bool     is_builtin;
} GenericTemplate;

typedef struct {
    Token* params;
    Type** args;
    int    count;
} Subst;
```

**`GenericTemplate`.** One generic declaration.

- `decl` — points at the original declaration in the merged
  program AST. The template does not own the AST; it borrows it.
- `name` — the declaration's name token.
- `params[8]` — the type parameter names, in order. The cap is 8,
  matching the parser's cap.
- `file_id` — the file the declaration lives in.
- `is_builtin` — copied from the declaration. True for the prelude.

**`Subst`.** A substitution: parameter names paired with concrete
`Type*`. `params` and `args` are parallel arrays of length
`count`. Borrowed view; the caller owns the arrays.

### §2.2 Functions

```c
void mono_init(void** generics_out);
void mono_free(void* generics);

void mono_register_template(void* generics, AstNode* decl,
                            const SourceMap* smap);

GenericTemplate* mono_find_template(void* generics, const SourceMap* smap,
                                    Token base);
GenericTemplate* mono_find_template_in_file(void* generics,
                                            const SourceMap* smap,
                                            Token base,
                                            uint32_t file_id);

AstNode*  mono_copy_node(AstNode* n, const Subst* sub, const SourceMap* smap);
TypeExpr* mono_copy_type_expr(TypeExpr* te, const Subst* sub,
                              const SourceMap* smap);

void mono_mangle(char* out, size_t cap, const char* base,
                 Type** args, int arg_count);

int              mono_template_count(void* generics);
GenericTemplate* mono_template_at(void* generics, int i);
```

**`mono_init`.** Allocates a new `TemplateList` and stores its
address through `generics_out`.

**`mono_free`.** Frees the list's items array and the list itself.
Does not free the template ASTs; those are borrowed.

**`mono_register_template`.** Appends a template. The declaration's
kind must be `AST_FUNC`, `AST_STRUCT_DEF`, or `AST_ENUM_DEF` with
a non-zero `type_param_count`. Other kinds are silently ignored.

**`mono_find_template`.** Linear scan for a template whose name
matches `base`. Cross-file: any registered template with a
matching name matches.

**`mono_find_template_in_file`.** Same, restricted to templates
declared in the given `file_id`. Used by
`sema_instantiate_function`'s resolver path.

**`mono_copy_node`.** Deep-copies an AST subtree with
substitution. Every resolution cache (`local_index`, `func_idx`,
`tag`, `tdef_index`, `sdef_index`, `offset`, `sub_count`) is reset
to its sentinel so the copy goes through sema like a fresh node.

**`mono_copy_type_expr`.** Deep-copies a type expression with
substitution.

**`mono_mangle`.** Writes `base#arg1#arg2...` into `out`. Truncates
silently.

**`mono_template_count`, `mono_template_at`.** Iteration over the
template list. Used by `instantiate_option_of` in `sema_mono.c`.

---

## §3 — Implementation notes

### §3.1 `TemplateList`

```c
typedef struct {
    GenericTemplate* items;
    int              count;
    int              capacity;
} TemplateList;
```

Grows by doubling from 8. `mono_free` frees `items` and the list
itself.

### §3.2 Registration

`mono_register_template` grows the items array, then switches on
the declaration's kind to copy the name and type parameters. The
switch handles `AST_FUNC`, `AST_STRUCT_DEF`, `AST_ENUM_DEF`; a
`default: break` leaves `name` and `params` zeroed.

The `smap` parameter is cast to `(void)smap` and never used. It is
part of the API for symmetry with other name-comparing functions
and for future use.

`file_id` is copied from `decl->span.file_id`. `is_builtin` is
copied from `decl->is_builtin`.

### §3.3 Substitution in type expressions

`mono_copy_type_expr` is a switch over `TypeExprKind`. The
substitution point is a single check at the top:

```c
if (te->kind == TEX_NAME) {
    for (int i = 0; i < sub->count; i++) {
        if (smap_tokens_equal(smap, te->name, sub->params[i]))
            return type_expr_resolved(sub->args[i], te->span);
    }
    return type_expr_name(te->name);
}
```

A `TEX_NAME` whose text matches a substitution's parameter is
replaced by a `TEX_RESOLVED` wrapping the concrete `Type`. Any
other `TEX_NAME` is copied as-is.

Compound expressions (`TEX_REF`, `TEX_RC`, `TEX_RAWPTR`,
`TEX_ARRAY`, `TEX_FN`, `TEX_GENERIC`, `TEX_QUALIFIED`) are
recursively copied.

**`TEX_GENERIC`.** The base name is copied verbatim; only the
arguments are recursively copied. A parameter name in the base
position is not substituted because the language does not allow a
type parameter to be used as a generic constructor.

**`TEX_QUALIFIED`.** The path array is copied into a local buffer
of `PRISM_MAX_PATH_COMPONENTS` (16) entries and passed to
`type_expr_qualified`, which copies it again.

**`TEX_RESOLVED`.** The `resolved` pointer is copied verbatim. No
substitution is attempted.

### §3.4 Substitution in AST nodes

`mono_copy_node` allocates a fresh node via `ast_alloc_node`,
copies `is_pub`, then switches on `n->kind`. Each case follows a
uniform pattern:

1. Copy non-pointer payload fields (names, tags, booleans, op
   enums).
2. For each `AstNode*` child, recurse via `mono_copy_node`.
3. For each `TypeExpr*` field, recurse via `mono_copy_type_expr`.
4. For each `AstList`, `ast_list_init` the copy and push the
   recursively-copied items.
5. Reset resolution caches to their sentinels.

**Which sentinels are reset:**

- `AST_FUNC` — `type_param_count = 0`, `func_idx = -1`,
  `ffi_idx = -1`, `return_type = NULL`, each `params[i].type = NULL`.
- `AST_STRUCT_DEF` — `type_param_count = 0`, `sdef_index = -1`,
  each field's `type = NULL` and `offset = 0`.
- `AST_ENUM_DEF` — `type_param_count = 0`, `tdef_index = -1`,
  each variant's `payloads = NULL` and `payload_count = 0`.
- `AST_LET` — `type = NULL`.
- `AST_VAR` — `func_idx = -1` (the other two sentinels come from
  `ast_alloc_node`).
- `AST_CALL` — `builtin_id = -1`, `func_idx = -1`, `ffi_idx = -1`,
  `instantiation = NULL`.
- `AST_VARIANT_CTOR` — `tdef_index = -1`, `tag = -1`,
  `mod_func_idx = -1`, `mod_ffi_idx = -1`, `instantiation = NULL`.
- `AST_STRUCT_LIT` — `sdef_index = -1`, `hidden_slot = -1`.
- `AST_STRUCT_INIT` — `offset = -1`, `field_type = NULL`.
- `AST_FIELD` — `owner = NULL`, `offset = 0` (unit_tag is
  preserved because it is intrinsic to the field name).
- `AST_FOR_RANGE`, `AST_FOR_IN` — all slot fields = -1.
- `AST_MATCH` — `hidden_slot = -1`.
- `AST_MATCH_ARM` — no caches.
- `AST_PAT_BIND` — `slot = -1`.
- `AST_PAT_VARIANT` — `tag = -1`, `sub_count = 0`,
  `tmp_slots[]` left zeroed.

**`AST_FUNC`** drops `type_param_count` to 0 because the copy is
a concrete instantiation, not a template.

**`AST_ENUM_VARIANT`** preserves `tag`. The variant's order is
stable across instantiation; the tag is the same on the copy.

**`AST_DEREF`** preserves `offset`. It is a syntactic adjustment,
not a resolved value.

**`AST_ADDR_OF_FIELD`** is only produced by sema, so the
monomorphizer never sees one. The case exists to keep the switch
exhaustive.

**`AST_CONST`** and **`AST_USING`** deep-copy their payloads.
Templates never contain these kinds (they're top-level-only and
the parser doesn't put them inside function bodies), but the
cases exist.

**`AST_PROGRAM`** prints an error and returns a zeroed node. The
monomorphizer should never see one; the case is defensive.

The `default: break` at the bottom of the switch covers leaf
kinds (`AST_INT`, `AST_FLOAT`, etc.). Since `ast_alloc_node`
already produced a zeroed node, the copy is valid for those.
If a future kind with children is added and not handled here, the
default would silently produce a shallow copy — a bug. See §5.

### §3.5 Mangling

`mono_mangle` writes `base#arg1#arg2...` with `#` as separator.
`snprintf` handles the buffer growth. Truncation is silent.

The mangled name is a display artifact. Type identity is nominal,
by `TypeDef*` pointer. Two `TypeDef`s whose mangled names happen
to collide (which is possible if `mono_mangle` truncates) are
still distinct types.

The `type_name` call for each argument returns a pointer into a
rotating 8-slot ring. Since each argument's rendering is
`snprintf`'d into the output buffer before the next `type_name`
call, the ring does not cause wrong output. It does mean a caller
that invokes `type_name` on the result would see one of the
intermediate buffers, not the final name.

### §3.6 File-scoped vs. file-agnostic lookup

`mono_find_template` searches all templates; two templates with
the same name in different files both match and the first one
wins. `mono_find_template_in_file` restricts to a specific file.

The choice between them is the caller's. In
`sema_instantiate_generic`, the lookup is:

```c
GenericTemplate* tmpl = mono_find_template(generics, smap, te->name);
if (!tmpl) {
    tmpl = mono_find_template_in_file(generics, smap, te->name, file_id);
}
```

The file-agnostic lookup is tried first; the file-scoped one is a
fallback.

---

## §4 — Invariants

- A `GenericTemplate`'s `decl` pointer is non-NULL and points at a
  top-level declaration of the merged program AST. The template
  does not own it and must not free it.
- A `GenericTemplate`'s `param_count` is at most 8 and matches the
  declaration's own `type_param_count`.
- A `Subst`'s `params` and `args` arrays are at least `count`
  elements long. Both are borrowed.
- `mono_copy_node` returns a freshly allocated tree that shares no
  owned pointer with the input.
- `mono_copy_node` and `mono_copy_type_expr` are idempotent in
  the sense that a second copy of the same tree produces an
  equivalent but distinct tree.
- `mono_find_template` returns a pointer valid until the list is
  next modified.
- `mono_mangle` always NUL-terminates `out` within `cap` bytes.

---

## §5 — Known limitations

**Templates are not file-scoped.** `mono_find_template` searches
all templates regardless of `file_id` or `is_pub`. A generic
declared in one file is visible for instantiation from any other
file, even if the declaration was not `pub`. Non-generic
functions and types have a visibility check; generics do not.

**Template ASTs are not copied at registration.** The template
stores the pointer to the original declaration. The original AST
lives until the whole compilation finishes, and the template list
is freed before the program AST, so the ordering is safe. A
future change that freed the program AST earlier would break
this.

**`mono_register_template` silently ignores non-function/struct/
enum kinds.** The `default: break` leaves a zeroed template.
Not reachable from the current call site.

**`mono_register_template`'s `smap` argument is unused.** It is
cast to void. The parameter exists for API symmetry.

**`mono_copy_node`'s `default` case produces an empty node.**
Any AST kind not explicitly handled would silently produce a
shallow copy — the copy would have no children even if the
original did. This is currently unreachable (every kind the
monomorphizer sees is handled), but a new kind added to the AST
without a corresponding case here would hit this bug.

**`mono_copy_node` on `AST_PROGRAM` prints an error and returns
a zeroed node.** Not reachable.

**File-scoped visibility is a latent bug.** See above.

**`Subst` is not validated.** A `Subst` whose `params` array is
shorter than the template's `param_count`, or whose `args` array
is NULL, produces undefined behavior. Sema's callers always
construct well-formed `Subst`s.

**`type_name` ring interaction.** With more than 8 type
arguments, `mono_mangle`'s intermediate renders cycle the ring.
The final output is correct because each argument is rendered
before the next.

**Truncation in `mono_mangle` is silent.** Two distinct
instantiations whose mangled names are truncated to the same
string would have `TypeDef.name_buf` collide. Not observable in
practice (the names are display-only), but worth knowing.

**No depth limit on `mono_copy_node` recursion.** Bounded by
source syntactic depth.

---

## §6 — Extension points

- **File-scoped lookup.** Adding a `file_id` parameter to
  `mono_find_template` (and enforcing it) would close the
  visibility gap. Requires sema to pass the current file id at
  every call site.
- **A template cache keyed on (name, arg_count, args).** The
  current `InstCache` in `sema_mono.c` already does this for
  instantiation results. Moving it here would consolidate the
  logic; not needed.
- **Generic constraints.** A `where T: Trait` clause would
  require new AST nodes, parser support, and validation during
  substitution. Not on the roadmap.
- **Copying `AST_CONST` and `AST_USING` deeply.** The current
  cases deep-copy `initializer` / `alias_target` and copy the
  path array. If a future kind with additional owned pointers is
  added, the copy would need extending.
- **A `mono_dump_template` for debugging.** Prints a template's
  name, parameters, and declaring file.
- **`mono_mangle` with a separator that cannot appear in an
  identifier.** `#` is already safe, but a longer or more
  readable separator (e.g. `::`) would improve diagnostics.
- **Validating `Subst`.** A `mono_subst_init` helper that
  cross-checks against a template would prevent the latent bug.
