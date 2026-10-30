## `docs/arch/files/16-resolve.md`

# `include/resolve.h`, `src/resolve.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

Centralized name resolution. Given a name and the file it was
written in, returns which declaration it refers to.

**Scope.** Every declaration at the top level of some file:
functions, types, templates, constants, module aliases. Does not
resolve locals (that is `FnLocal`'s job in `sema.c`).

**Report policy.** No diagnostics. Unresolvable names return
`RD_NONE`. The caller decides whether that is an error. This
keeps the resolver free of message-shaping policy.

---

## §2 — Public interface

```c
ResolvedDecl resolve_name(const SemaCtx* sc, uint32_t file_id, Token name);
ResolvedDecl resolve_qualified(const SemaCtx* sc, uint32_t file_id,
                               Token alias, Token name);
ResolvedDecl resolve_template(const SemaCtx* sc, uint32_t file_id, Token name);
ResolvedDecl resolve_in_module(const SemaCtx* sc, uint32_t file_id, Token name);
TypeDef* resolved_tdef(const SemaCtx* sc, ResolvedDecl rd);

FileNamespace* modtable_find(const ModuleTable* mt, uint32_t file_id);
int fnamespace_resolve(const FileNamespace* fn, const SourceMap* smap,
                       Token name);
const SelectiveImport* fnamespace_find_selective(const FileNamespace* fn,
                                                 const SourceMap* smap,
                                                 Token name);
```

**`rd_none`** is a static-inline helper returning
`(ResolvedDecl){ RD_NONE, -1, NULL }`.

---

## §3 — Implementation notes

### §3.1 Precedence within a file

`find_in_scope` calls four sub-lookups in order:

1. `find_func_in_scope`.
2. `find_type_in_scope`.
3. `find_template_in_scope`.
4. `find_const_in_scope`.

The order is arbitrary but fixed; changing it changes shadowing
behavior for the rare case of a name reused across kinds. No
current code depends on a specific order; the fixed order is a
documentation of the current behavior.

Each sub-lookup filters by `file_id`. `find_func_in_scope` and
`find_type_in_scope` also skip `is_instantiated` entries.

### §3.2 `resolve_name`

Four-step precedence:

1. `find_in_scope(sc, file_id, name)`.
2. `find_selective(sc, file_id, name)`.
3. `builtin_tdef_by_name` on the extracted name.
4. `find_in_scope(sc, sc->prelude_file_id, name)`, if
   `prelude_file_id != 0` and the current file isn't the prelude.

The prelude lookup is the mechanism by which `Result` and
`Option` are in scope in every file without a `using`. It is
identical to the file-local lookup, just against the prelude's
`file_id`.

### §3.3 `find_selective`

```c
const FileNamespace* fns = modtable_find(&sc->modtable, file_id);
if (!fns) return rd_none();
const SelectiveImport* si = fnamespace_find_selective(fns, sc->smap, name);
if (!si) return rd_none();
return find_in_scope(sc, (uint32_t)si->module_file_id, si->source_name);
```

The local name is what the caller wrote; the source name is what
the target module declared. The lookup in the target module uses
the source name.

### §3.4 `resolve_qualified`

`alias` must name a whole-module import of the current file.
`fnamespace_resolve` returns the target `file_id`; then
`find_in_scope` for the target. If `alias` is a selective
import, the lookup fails (it is not a module alias).

### §3.5 `resolve_template`

Same shape as `resolve_name`, restricted to template results.
Steps: file-local template, selective-import template, prelude
template.

### §3.6 `resolve_in_module`

Direct `find_in_scope` on a specific file. Used by
`validate_selective_imports` to check that a name exists in the
target module.

### §3.7 `resolved_tdef`

```c
TypeDef* resolved_tdef(const SemaCtx* sc, ResolvedDecl rd) {
    switch (rd.kind) {
        case RD_TYPE:
            if (rd.index < 0 || rd.index >= sc->structs->count)
                return NULL;
            return &sc->structs->items[rd.index];
        case RD_BUILTIN_TYPE:
            return rd.builtin;
        default:
            return NULL;
    }
}
```

Two cases with non-NULL results. The `RD_BUILTIN_TYPE` case
returns the static `TypeDef` from `types.c`. Every other kind
returns NULL.

---

## §4 — Invariants

- `find_in_scope` returns a `ResolvedDecl` whose `index` is
  valid for the returned kind, or `rd_none()`.
- `resolve_name` never returns a `ResolvedDecl` with
  `RD_MODULE`. Module aliases are looked up only by
  `resolve_qualified`.
- `resolved_tdef` returns NULL for any kind other than `RD_TYPE`
  and `RD_BUILTIN_TYPE`.
- Linear scans: `find_*_in_scope` is `O(count)` for each table.
  Total lookup is `O(functions + types + templates + constants)`.
  For current compilations the sum is small.

---

## §5 — Known limitations

**Linear scans.** Every lookup scans every entry in every table.
Fine at current scale; a hash table keyed on name would be the
next step if the number of declarations grew past a few hundred.

**No caching.** The same name looked up twice does the same
linear scan twice. Not a hot path today.

**No memoization of `resolve_name`.** No caller relies on it.

**Cross-file resolution for free functions requires the target
file to be an import.** A function declared in file B is only
visible from file A if A has imported B. This is enforced by the
`imports[]` lookup in `resolve_qualified` and by the fact that
`find_in_scope` filters by `file_id`.

**The four-step precedence is fixed.** No extension point for
user-defined shadowing rules.

**No module scoping.** A module's declarations are all in one
flat namespace; nested modules would need a different data
structure.

**`resolve_in_module` bypasses the caller's scope.** If the
target module's name shadows a local, the resolver would not
know. Used only by import validation, where no locals exist.

---

## §6 — Extension points

- **Hash-table lookup.** Keyed on `(file_id, name)` for
  file-scoped, `(name)` for cross-file.
- **A `resolve_name_cached` variant.** Would memoize the result
  for the current `sema_analyze` call.
- **Per-file type namespaces.** Would require splitting the
  type-table lookup further; the current design has a global
  type table keyed by file.
- **Module scoping.** Would require a hierarchical module
  structure; the current design is a flat list of files.
- **Better diagnostics for unresolvable names.** Currently
  `resolve_name` returns `RD_NONE` and the caller produces the
  message. A resolver-side "closest match" hint would be a
  caller-facing improvement.
