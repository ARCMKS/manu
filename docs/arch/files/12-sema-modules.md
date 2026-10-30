## `docs/arch/files/12-sema-modules.md`

# `src/sema_modules.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

Populates and validates the `ModuleTable` — the per-file namespace
built from `using` declarations. Consumed by `resolve.c` (for
name lookup) and by `type_expr_resolve` (for qualified type names).

Two entry points, called from `sema_analyze`:

- `build_module_table` — before Pass A.
- `validate_selective_imports` — after Pass A, before Pass B.

The read-only accessors (`modtable_find`, `fnamespace_resolve`,
`fnamespace_find_selective`) live in `resolve.c`; this file only
writes.

---

## §2 — Public interface

```c
void build_module_table(ModuleTable* mt, AstNode* program,
                        DiagnosticBag* bag);
void validate_selective_imports(AstNode* program, SemaCtx* sc,
                                DiagnosticBag* bag);
```

**`build_module_table`.** Walks the merged program's top-level
declarations for `AST_USING` nodes. Populates one
`FileNamespace` per file that has imports.

**`validate_selective_imports`.** For each `USING_SELECTIVE`
entry, resolves the imported name in the target file and checks
visibility. Reports "no such item" or "private" errors.

**Private helpers:** `modtable_get_or_add`, `fnamespace_add`,
`fnamespace_add_selective`.

---

## §3 — Implementation notes

### §3.1 Keying

A `FileNamespace` is keyed by the file that *contains* the
`using` declaration (`d->span.file_id`), not by the target file.
This is what makes `modtable_find(&sc->modtable, current_file)`
return the caller's imports.

### §3.2 The three `USING_MODULE` cases

For a `USING_MODULE` declaration, the loader-set fields
`resolved_file_id` and `resolved_path_count` determine the
interpretation:

| Condition | Interpretation | Action |
|---|---|---|
| `resolved_path_count == path_count` | Whole-module import | Add to `imports[]`, local name = alias or last component |
| `resolved_path_count == path_count - 1` | One trailing symbol | Add to `selective[]` as sugar for a single-name import |
| Otherwise | Nested symbols | Diagnostic; not supported |

The `trailing == 1` case is why `using std.io.print` works as a
selective import of `print` from `std/io.manu`.

### §3.3 `USING_SELECTIVE`

Every entry in `names[]` becomes a selective import. The parser
has already resolved each entry's `local` name (either the source
name or the alias).

### §3.4 `USING_ALIAS`

Skipped by both entry points. Type aliases are handled entirely
elsewhere (in practice: nowhere; the feature is a no-op).

### §3.5 `resolved_file_id == 0` is a silent hole

If the loader failed to resolve a `using` (which it reports), the
import is still recorded with `module_file_id == 0`. Later
lookups against file_id 0 return NULL, so the failure surfaces
as "unknown name" at each use site rather than as a duplicate
error at the import. `INFERRED`: a deliberate deferral, not a
bug.

### §3.6 Selective import validation

`validate_selective_imports` uses `resolve_in_module` (from
`resolve.c`) to look up each imported source name in the target
module. If the result is `RD_FUNC` and `!is_pub`, report
`'X.Y' is private; declared in <path> and not marked 'pub'`.

Types are accepted without a visibility check here; their
`is_pub` is checked at each use site by the qualified-name
resolver in `types.c`. Constants and templates are accepted
without a check. The asymmetry is documented in §4.

---

## §4 — Invariants

- A `FileNamespace` is only ever created for a file that has at
  least one non-alias `using`.
- `modtable_get_or_add` never returns NULL.
- Every entry in `imports[]` has a nonzero `local_name.span.file_id`
  (the parser produces real tokens).
- Every selective entry's `local_name` is either equal to
  `source_name` (non-aliased) or a distinct token.

---

## §5 — Known limitations

**Types are not checked for `pub` at import time.** A selective
import of a private type succeeds here; the visibility error
fires later when the type is used. Two diagnostics for one
problem, or one for a type that's imported but never used.

**Constants and templates are accepted without validation.**
A selective import of a nonexistent constant or template
succeeds at this layer and fails at the use site.

**The `trailing > 1` error message points at the wrong thing.**
It suggests `using a.b.c.{ Symbol }` but `a.b.c` must resolve to
a file for the suggestion to work. The diagnostic is honest but
unhelpful.

**`resolved_file_id == 0` is a silent hole.** See §3.5.

**No dedup of identical imports.** Two `using std.io` in the
same file produce two entries in `imports[]`. The first match
wins in every lookup; the second is dead.

---

## §6 — Extension points

- **Constants and templates in selective imports.** Would
  require `validate_selective_imports` to consult the constant
  table and the template list.
- **Import-time `pub` check for types.** Would require knowing
  the type's declaring file at import time. Currently the
  check is deferred to use.
- **Dedup of identical imports.** A pre-check before
  `fnamespace_add`.
- **A `.` `{...}` form for whole-module imports with selective
  additions.** Not currently expressible.