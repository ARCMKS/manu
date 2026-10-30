// ============================================================
// module.h — Multi-file compilation loader.
//
// A ModuleLoader is the driver for a multi-file compilation.
// It owns:
//
//   - the SourceMap that all loaded files' tokens refer to;
//   - the ParsedFile array: one entry per file, each carrying
//     its canonical path, its parsed AST, and its root flag;
//   - the include-path list used to resolve `using a.b.c` module
//     paths to file paths;
//   - the loading stack used for cycle detection.
//
// Loading is a depth-first walk:
//   modload_load(root) reads the root file, parses it, pushes
//   it, and recursively loads every `using` module it names
//   (unless already loaded). The result is a topologically
//   sorted set of files in load order (root first, then its
//   imports, then their imports, ...).
//
// The loader is deliberately separated from sema. Sema wants
// a flat list of declarations; the loader produces that via
// modload_merge, which concatenates every file's top-level
// decls into a single AST_PROGRAM. Cross-file identity is
// maintained by the file_id field on each decl's span: a
// `using` declaration in file A that names a type in file B
// has both A's and B's file_ids available for the resolver
// (in types.c) to disambiguate.
//
// What the loader does NOT do:
//   - It does not rename anything. Symbol collisions across
//     files are handled by the resolver, which uses file_id
//     to distinguish them.
//   - It does not resolve `using` symbols to actual
//     declarations; that is sema's job (see
//     build_module_table and validate_selective_imports in
//     sema.c).
// ============================================================
#ifndef PRISM_MODULE_H
#define PRISM_MODULE_H

#include "ast.h"
#include "source_map.h"
#include "diagnostics.h"

// One loaded file.
//
// Fields:
//   file_id        — the SourceMap id assigned when the file
//                    was added. Every token lexed from this
//                    file carries this file_id in its span.
//   canonical_path — the canonicalized path (realpath'd on
//                    Unix, _fullpath'd on Windows). Owned by
//                    the loader. Two different spellings of
//                    the same file must produce the same
//                    canonical string for dedup to work.
//   program        — the parsed AST_PROGRAM. Owned by the
//                    loader. `modload_merge` transfers the
//                    decls out of it (see below).
//   is_root        — true for the file named on the command
//                    line, false for its imports. Used by
//                    the loader's diagnostics to distinguish
//                    "the file you asked for" from "a file
//                    pulled in transitively".
typedef struct {
    uint32_t    file_id;
    char*       canonical_path;
    AstNode*    program;
    bool        is_root;
} ParsedFile;

// The whole loader state.
//
// Fields:
//   smap               — the source map. Owned by the loader.
//   files /            — the ParsedFile array, in load order.
//   file_count /         The file_id on each entry is the
//   file_capacity        source map id; the array index is
//                        unrelated.
//   include_dirs /     — search paths, in priority order.
//   include_count /      Each string is owned by the loader.
//   include_capacity
//   loading /          — the DFS stack: indices into `files`
//   loading_count /      of files that are currently being
//   loading_capacity     loaded (i.e. their `using` decls
//                        have been read but not fully
//                        processed). Used to detect cycles:
//                        a `using` that resolves to a file
//                        already on the stack is a cycle.
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

void modload_init(ModuleLoader* ml);
void modload_free(ModuleLoader* ml);
void modload_add_include(ModuleLoader* ml, const char* dir);

// Load a root file and all of its transitive imports.
// Returns true on success (no diagnostics reported) and false
// otherwise. On failure, the loader may be partially
// populated; the caller is expected to check the bag and
// either render it and abort, or free the loader and abort.
bool modload_load(ModuleLoader* ml, const char* path, DiagnosticBag* bag);

// Produce a single AST_PROGRAM whose decls are the
// concatenation of every loaded file's decls.
//
// Ownership: the returned program owns the decls; the
// ParsedFile entries' decl lists are emptied by the merge
// (count set to 0, items pointer left allocated but unused).
// The caller frees the returned program with ast_free.
//
// The file_id on each decl's span remains the id the
// declaring file was registered under, so the resolver can
// still tell which file a decl came from. That is why the
// merge is a concatenation rather than a rewrite: file
// identity is preserved.
AstNode* modload_merge(ModuleLoader* ml);

#endif // PRISM_MODULE_H