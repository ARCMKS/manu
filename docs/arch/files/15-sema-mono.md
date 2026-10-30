## `docs/arch/files/15-sema-mono.md`

# `src/sema_mono.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

Instantiates generic types and functions on demand, caches the
results, and analyzes instantiated function bodies. The
monomorphizer's data storage (`TemplateList`) and deep-copy
primitives live in `monomorph.c`; this file drives them.

**Called from:**

- `types.c`'s `resolve_inner`, via the `instantiate_generic`
  callback installed on the `TypeDefTable`.
- `sema.c`'s `analyze_expr`, for `AST_CALL` with an
  `instantiation` field.
- `sema_mono.c`'s `instantiate_option_of`, for builtins whose
  return type mentions `Option<T>`.

---

## §2 — Public interface

```c
void cache_reset(void);

Type* sema_instantiate_generic(void* generics, TypeExpr* te,
                               const SourceMap* smap, void* ctx,
                               uint32_t file_id, DiagnosticBag* bag);

int sema_instantiate_function(SemaCtx* sc, Token base,
                              TypeExpr* te, DiagnosticBag* bag);

int sema_instantiate_function_by_template(SemaCtx* sc, int tmpl_idx,
                                          Token base, TypeExpr* te,
                                          DiagnosticBag* bag);

Type* instantiate_option_of(SemaCtx* sc, Type* payload,
                            Span span, DiagnosticBag* bag);
```

---

## §3 — Implementation notes

### §3.1 The cache

```c
#define MAX_INST_CACHE 256

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

`g_cache` is file-static. Reset by `cache_reset`, called at the
top of every `sema_analyze`. `cache_reset` frees each entry's
`args` array and memsets the cache.

`cache_lookup` compares `tmpl` by pointer identity and each
argument by `type_eq`. `cache_insert` returns NULL if the cache
is full; both instantiation functions handle a NULL return by
skipping the cache update but still completing the
instantiation.

Both result fields are set independently: a template is either a
type template or a function template, so at most one is used.

### §3.2 `sema_instantiate_generic`

Flow:

1. Resolve each `te->args[i]` via `type_expr_resolve`. Bail on
   any failure. Cap at 8 arguments.
2. `mono_find_template` for the name. Fall back to
   `mono_find_template_in_file` if the first lookup misses.
3. Arity check: `tmpl->param_count == n`.
4. Cache lookup. If `e->type_result` is set, return the wrapped
   type.
5. `mono_copy_node` on `tmpl->decl` with the substitution.
6. `mono_mangle` for the mangled name.
7. `typedef_table_add` and initialize the `TypeDef`. Set
   `is_pub = true`, `is_instantiated = true`,
   `is_rc_managed = tmpl->is_builtin`. Set `sdef_index` or
   `tdef_index` on the copy.
8. Cache the result (`cache_insert` if the lookup missed, then
   `e->type_result = td`).
9. Resolve the copy's fields (struct) or variants (enum) against
   the concrete substitution. Same rejection rules as
   `resolve_struct_fields` / `resolve_enum_variants`.
10. `ast_free(copy)`.
11. Return `ty_struct(td)` or `ty_enum(td)`, wrapped in `ty_rc`
    if `td->is_rc_managed`.

**Why `is_pub = true` unconditionally.** An instantiated generic
is importable from any file. This is what makes `Result` and
`Option` usable everywhere despite the templates being
non-`pub`.

**Why the copy is freed.** The `TypeDef` holds everything codegen
will use. The AST's only remaining purpose was to carry the
syntactic field/variant data during resolution.

**The `is_rc_managed` copy.** `tmpl->is_builtin` is true for
prelude templates. A user-defined generic template has
`is_builtin == false`, so its instantiations are not RC-managed.
If a user wants RC-wrapped instantiations, they write
`rc<TheirGeneric<T>>` at the use site.

### §3.3 `sema_instantiate_function` and the `_by_template` variant

Two entry points with the same body. The `_by_template` variant
takes a template index directly (used by the module-call path
once the resolver has identified the template). The named
variant resolves the name to a template first via
`resolve_template`, then delegates.

Flow:

1. Resolve type arguments. Cap at 8.
2. Fetch the template (`mono_template_at` for the index form, or
   `mono_find_template` for the named form).
3. Arity check.
4. Cache lookup. If `e->func_result >= 0`, return it.
5. `mono_copy_node` with substitution.
6. Capacity check on `sc->func_count`.
7. `mono_mangle` for the mangled name.
8. Register a `FuncSig`. Set `is_instantiated = true`,
   `is_pub = true`. `file_id` is the template's, not the
   caller's. `decl = copy` (ownership transfers).
9. Resolve the copy's parameter types and return type. Default
   unannotated parameters to `i64`. If no return annotation,
   `return_type = ty_void()`.
10. Set `copy->as.func.func_idx` for post-sema passes.
11. **Cache the result before analyzing the body** — the
    `e->func_result = sidx` assignment runs before
    `analyze_func_body`.
12. `analyze_func_body(sc, sig, copy, bag)`.

**The copy is not freed.** Codegen needs it; the body's
annotations and the `FuncSig.decl` pointer require the copy to
live past `sema_analyze`.

**The cache-before-body ordering** is what prevents infinite
recursion on self-recursive generics. A recursive call finds
the cache entry and returns the index without re-instantiating.

### §3.4 `instantiate_option_of`

Iterates the template list looking for a template named
`Option`. If found, constructs a `TEX_GENERIC` with one
`type_expr_resolved` argument and calls the installed
`instantiate_generic` callback. The constructed `TEX_GENERIC` is
freed on return (which also frees the argument).

Used by `read_line` (whose return type is `Option<string>`).

### §3.5 The cache-miss path when the cache is full

When `cache_insert` returns NULL (cache at `MAX_INST_CACHE`),
the instantiation still runs, and the result is not stored. A
subsequent call with the same arguments re-runs the whole
instantiation, producing a *new* `TypeDef` (for type
instantiations) or a *new* `FuncSig` (for function
instantiations). Both have the same mangled name.

For type instantiations, the duplicate `TypeDef` is a different
pointer with the same name. Nominal comparison via `tdef`
pointer would treat them as unequal. This is a real bug at
scale; not observed in practice.

For function instantiations, the duplicate `FuncSig` gets a
different codegen index and its body is emitted twice. Both
are usable; the second is dead code.

---

## §4 — Invariants

- The instantiation cache is empty at entry to `sema_analyze`
  and contains at most `MAX_INST_CACHE` entries at exit.
- An `InstEntry` with `type_result != NULL` has that pointer
  valid for the compilation's lifetime.
- An `InstEntry` with `func_result >= 0` has a valid index into
  `sc->funcs[]` for the current `SemaCtx`.
- Every instantiated `TypeDef` has `is_instantiated == true`,
  `is_pub == true`, and a mangled `name_buf`.
- Every instantiated `FuncSig` has `is_instantiated == true`,
  `is_pub == true`, and a mangled `name_buf`.
- An instantiated function's `decl` points at a heap-allocated
  `AstNode` that outlives the compilation.

---

## §5 — Known limitations

**Cache overflow produces duplicates.** See §3.5.

**File-agnostic template lookup.** `sema_instantiate_function`
uses `mono_find_template` (via `resolve_template`, which for the
file-agnostic path delegates to `mono_find_template`). A private
generic in one file can be instantiated from another. This is a
visibility gap; see `invariants.md` and `handbook/limitations.md`.

**No instantiation depth limit.** A generic function that
recursively instantiates itself with different type arguments
would grow without bound until the instantiation cache or the
function table fills up. Not tested; would produce a
diagnostic-laden failure rather than a hang.

**No instantiation of `const` templates.** `const NAME = ...`
is not generic.

**Instantiated function ASTs leak.** Deliberate; codegen needs
them.

**`is_pub = true` for all instantiations.** Over-permissive but
required by the prelude's non-`pub` `Result`/`Option`.

---

## §6 — Extension points

- **File-scoped template lookup.** Add a `file_id` parameter to
  the lookup path and pass it from sema.
- **Better cache-overflow handling.** Replace the fixed array
  with a growable list, or a hash keyed on the template+args.
- **A generics constraint system.** Would require new AST
  nodes, parser support, and validation during instantiation.
- **Duplicate detection for `is_pub` templates.** Before
  registering, check that the template isn't already in the
  cache with a different `file_id`.