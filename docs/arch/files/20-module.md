## `docs/arch/files/20-module.md`

# `include/module.h`, `src/module.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

Multi-file compilation. Recursively loads a root file and every
file it imports, then flattens the set of `ParsedFile`s into one
`AST_PROGRAM` for sema.

**What it solves.** `using` path resolution, cycle detection,
duplicate-file dedup, and the merge step that produces the
single-program shape sema expects.

**What it does not solve.** Symbol resolution across files (sema
does that). Type-checking imports (sema does that). Relative-path
imports (`./foo`) — all paths are resolved against include
directories.

**Called from:** `main.c`'s `compile_multi`.

---

## §2 — Data structures

```c
typedef struct {
    uint32_t file_id;
    char*    canonical_path;
    AstNode* program;
    bool     is_root;
} ParsedFile;

typedef struct {
    SourceMap    smap;
    ParsedFile*  files;
    int          file_count;
    int          file_capacity;
    const char** include_dirs;
    int          include_count;
    int          include_capacity;
    int*         loading;
    int          loading_count;
    int          loading_capacity;
} ModuleLoader;
```

**`ParsedFile`.** One loaded file. `file_id` is the SourceMap id.
`canonical_path` is owned by the loader. `program` is the parsed
`AST_PROGRAM`, owned by the loader (until merge transfers its
decls).

**`ModuleLoader`.** The loader state. `smap` owns the source
content. `files` is the load-order list. `include_dirs` are the
search paths. `loading` is the DFS stack for cycle detection.

---

## §3 — Public interface

```c
void modload_init(ModuleLoader* ml);
void modload_free(ModuleLoader* ml);
void modload_add_include(ModuleLoader* ml, const char* dir);

bool modload_load(ModuleLoader* ml, const char* path, DiagnosticBag* bag);

AstNode* modload_merge(ModuleLoader* ml);
```

**`modload_init`.** Zeroes the loader and calls `smap_init`.

**`modload_free`.** Frees each `ParsedFile`'s canonical path and
program, the files array, the include paths, the loading stack,
and the SourceMap. Re-zeroes the loader.

**`modload_add_include`.** Appends a search directory. Order is
priority order.

**`modload_load`.** Loads the root file and its transitive
imports. Returns false on any load error.

**`modload_merge`.** Concatenates every file's decls into a
single `AST_PROGRAM`. Transfers ownership; each file's decl list
is emptied.

---

## §4 — Implementation notes

### §4.1 Path resolution

`resolve_against_includes` tries each include directory in order,
attempting `<dir>/<rel>` then `<dir>/<rel>.manu`. First hit wins.

The fixed `.manu` extension is a language-level choice.

`canonicalize_path` calls `realpath` (Unix) or `_fullpath`
(Windows). Two spellings of the same file produce the same
canonical string; this is what dedup and cycle detection rely on.

### §4.2 Longest-prefix resolution

`resolve_module_path` tries the full path, then progressively
shorter prefixes. The first prefix that resolves to a file is the
module; the trailing components (if any) name symbols inside it.

The output `out_symbol_count` is the number of trailing
components. The loader sets:

- `u->resolved_file_id` — the file's id.
- `u->resolved_path_count = path_count - symbol_count`.

For `using util.io.print` where `util/io.manu` exists:

- `resolve_against_includes("util/io/print")` misses.
- `resolve_against_includes("util/io")` hits.
- `symbol_count = 1`.
- `resolved_file_id = util/io.manu`'s id.
- `resolved_path_count = 2`.

### §4.3 The DFS and cycle detection

`load_file` handles one file. Steps:

1. Canonicalize the path.
2. `find_file_by_canonical`. If found and on the loading stack,
   reject as a cycle. If found and not on the stack (a diamond),
   return the existing index.
3. Read the file.
4. `smap_add`.
5. Lex + parse.
6. Append a `ParsedFile`. Record `file_id` and `canonical_path`.
7. `push_loading`.
8. `load_imports` (recursive).
9. `loading_count--`.

The `loading` stack tracks files currently being processed. A
`using` that resolves to a file on the stack is a cycle.

### §4.4 `load_imports`

For each `AST_USING` declaration in the file's program:

- Skip `USING_ALIAS`.
- `resolve_module_path` to a file path.
- On failure, report `cannot find module 'x.y.z'`.
- `load_file` recursively.
- Record `resolved_file_id` and `resolved_path_count` on the
  `AstUsing` payload.

### §4.5 `modload_merge`

```c
AstNode* modload_merge(ModuleLoader* ml) {
    AstNode* merged = ast_program();
    for (int i = 0; i < ml->file_count; i++) {
        AstList* fl = &ml->files[i].program->as.program.decls;
        for (int j = 0; j < fl->count; j++)
            ast_list_push(&merged->as.program.decls, fl->items[j]);
        fl->count = 0;
    }
    return merged;
}
```

The transfer is by pointer. Each declaration retains its original
span, so `file_id` per declaration is preserved. Sema's
`build_module_table` reconstructs per-file scoping from those
spans.

**The file ASTs remain alive** (their decl lists are emptied but
the `AstNode`s are owned by the loader until `modload_free`). The
merged program does not own them; ownership is shared by
convention until both are freed.

### §4.6 The CLI's include-path seeding

`main.c` seeds the loader with:

1. The root file's directory.
2. The current directory (`.`).
3. CLI `-I` paths, in order.

The order gives CLI paths the lowest priority.

---

## §5 — Invariants

- A `ParsedFile`'s `file_id` is the id returned by `smap_add`.
- Every file's `canonical_path` is unique within the loader.
- Every file appears in `files` exactly once.
- A file on the `loading` stack is being processed; a `using`
  that resolves to it is a cycle.
- `modload_merge` returns a program whose decls list contains
  every loaded file's declarations in load order.
- After merge, each `ParsedFile.program->as.program.decls.count`
  is 0.

---

## §6 — Known limitations

**No relative-path imports.** `using ./foo` is not supported; all
paths are resolved against include directories.

**No `pub using` (re-export).** The parser rejects it.

**Include-dir search is O(dirs) per import.** Linear; fine for
the current scale.

**The loader parses every reachable file but does not analyze.**
A file with a parse error aborts the whole load, not just that
file's subtree.

**No per-file directory scoping.** A `using` in a subdirectory is
resolved against the same include list as one in the root.

**`modload_load` reads every file eagerly.** No lazy loading; no
"only load what's referenced" mode.

**Path canonicalization may fail.** A path that cannot be
canonicalized is duplicated verbatim, which can produce duplicate
`ParsedFile` entries if the same file is reached via two
non-canonical spellings.

---

## §7 — Extension points

- **Relative-path imports.** Would require detecting a leading
  `./` or `../` and resolving against the importing file's
  directory instead of the include list.
- **`pub using`.** Would require parser support and a loader
  change to record re-exports.
- **Lazy loading.** Would require a two-phase parse (declarations
  first, bodies on demand).
- **A dependency graph.** Would enable parallel parsing of
  independent files.
- **Per-file include directories.** Would let a subdirectory's
  imports resolve relative to itself.
