// ============================================================
// module.c — Multi-file compilation loader.
//
// Reading order:
//   1. Path helpers (dup_string, file_exists,
//      canonicalize_path, resolve_against_includes).
//   2. Lifecycle (modload_init, modload_free,
//      modload_add_include).
//   3. Disk I/O (read_file_content).
//   4. AST_USING path joining and resolution
//      (join_path, resolve_module_path).
//   5. File bookkeeping (find_file_by_canonical,
//      on_loading_stack, push_loading).
//   6. The recursive loader (load_file, load_imports).
//   7. modload_load — the entry point.
//   8. modload_merge — the flattening step.
//
// The loader is re-entrant within a single modload_load call
// (it uses the `loading` stack for cycle detection) but not
// thread-safe across calls. There is one ModuleLoader per
// compilation in practice.
//
// The loader owns every heap allocation in its state. Paths
// are duplicated into the loader's heap; content is read
// into a temporary buffer, then handed to smap_add (which
// copies it again), and the temporary is freed.
// ============================================================
#include "../include/module.h"
#include "../include/lexer.h"
#include "../include/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------
// Path helpers
// ------------------------------------------------------------

// Duplicate a C string into the loader's heap. Trivial
// wrapper over xmalloc+memcpy. Used for canonical paths and
// include-directory strings, both of which the loader owns
// and frees.
static char* dup_string(const char* s) {
    size_t n = strlen(s);
    char*  p = (char*)xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

// Does the path name a file we can open? Opens and closes
// the file rather than using access()/stat(), because the
// latter two have platform-specific behavior around
// permissions and directories that would need special-casing
// here. Opening is the operation we actually care about
// (the subsequent read will do the same).
static bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

// Canonicalize a path to an absolute, normalized form. Two
// different spellings of the same file must produce the same
// string so that dedup and cycle detection work.
//
// On Unix: realpath() resolves symlinks, ./ and ../, and
// makes the path absolute. On Windows: _fullpath() does an
// equivalent job but does not resolve symlinks by default
// (which is fine for dedup purposes here).
//
// Fallback: if canonicalization fails (e.g. the file does
// not exist yet — which can happen for a `using` path that
// is being tried speculatively), the path is duplicated
// verbatim. That is safe for dedup because a non-canonical
// spelling of a real file would only be reached if the file
// were later resolved to the same non-canonical string.
static char* canonicalize_path(const char* path) {
#ifdef _WIN32
    char buf[4096];
    if (_fullpath(buf, path, sizeof buf)) return dup_string(path);
#else
    char* r = realpath(path, NULL);
    if (r) return r;
#endif
    return dup_string(path);
}

// Resolve `rel` against the include directories. `rel` is the
// path as written by the user or produced by join_path — no
// extension is assumed. We try, in each include directory:
//
//     <include>/<rel>             // user supplied the extension
//     <include>/<rel>.manu        // user supplied the bare name
//
// The first hit wins. Returns a malloc'd string or NULL.
//
// The extension is fixed at ".manu". That is the language's
// canonical extension; a project that wants a different one
// would need a change here (or an option to modload_load).
//
// The buffers are 4096 bytes; a longer path component is
// truncated by snprintf, which would produce a
// non-existent path and cause the search to fail. Not a
// practical concern for realistic projects.
static char* resolve_against_includes(ModuleLoader* ml, const char* rel) {
    for (int i = 0; i < ml->include_count; i++) {
        char buf[4096];

        snprintf(buf, sizeof buf, "%s/%s", ml->include_dirs[i], rel);
        if (file_exists(buf)) return dup_string(buf);

        snprintf(buf, sizeof buf, "%s/%s.manu", ml->include_dirs[i], rel);
        if (file_exists(buf)) return dup_string(buf);
    }
    return NULL;
}


// ------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------

// Zero the loader and initialize its SourceMap. Called once
// per compilation before any add or load. Safe on
// uninitialized stack storage.
void modload_init(ModuleLoader* ml) {
    memset(ml, 0, sizeof *ml);
    smap_init(&ml->smap);
}

// Free everything the loader owns and reset.
//
// Order:
//   1. Per-file: canonical_path and program.
//   2. The files array itself.
//   3. Include-directory strings and the array.
//   4. The loading stack.
//   5. The SourceMap (which frees each file's content and
//      line_starts).
//   6. Zero the loader state.
//
// Note that the parsed programs are freed here even if the
// caller already called modload_merge: the merge empties
// each file's decl list (count = 0) but the AST_PROGRAM
// node itself is still owned by the ParsedFile and is
// freed by this function. ast_free on an empty AST_PROGRAM
// is safe.
void modload_free(ModuleLoader* ml) {
    for (int i = 0; i < ml->file_count; i++) {
        free(ml->files[i].canonical_path);
        ast_free(ml->files[i].program);
    }
    free(ml->files);
    for (int i = 0; i < ml->include_count; i++)
        free((char*)ml->include_dirs[i]);
    free(ml->include_dirs);
    free(ml->loading);
    smap_free(&ml->smap);
    memset(ml, 0, sizeof *ml);
}

// Append an include directory. Order matters: earlier
// entries are searched first by resolve_against_includes.
// The typical pattern in main.c is "directory of the root
// file, then '.'"; the CLI's -I paths are appended after.
//
// The string is duplicated; the caller's buffer may be a
// temporary.
void modload_add_include(ModuleLoader* ml, const char* dir) {
    if (ml->include_count >= ml->include_capacity) {
        ml->include_capacity = ml->include_capacity
                             ? ml->include_capacity * 2 : 8;
        ml->include_dirs = (const char**)xrealloc(ml->include_dirs,
            sizeof(char*) * (size_t)ml->include_capacity);
    }
    ml->include_dirs[ml->include_count++] = dup_string(dir);
}

// ------------------------------------------------------------
// Disk I/O
// ------------------------------------------------------------
//
// Read an entire file into a heap buffer. On failure (file
// not found or unreadable), returns NULL. The caller owns
// the returned buffer.
//
// The read is a plain fread of `n` bytes; a short read is
// not detected. On a regular file that is fine; on a
// non-seekable stream (fifo, character device) the ftell
// would be wrong and the read would be short. The loader
// only ever reads regular files, so this is not a live
// concern.
static char* read_file_content(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)xmalloc((size_t)n + 1);
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

// ------------------------------------------------------------
// AST_USING helpers
// ------------------------------------------------------------

// Join path components with '/' into `out`.
//
// Each Token in `toks` names one component of the path; its
// source bytes come from the SourceMap. `n` is the number of
// components. The components are copied verbatim; no case
// folding, no separator normalization. A component with a
// slash in it would produce a path with two slashes in a
// row, which file_exists would still try (and likely fail).
//
// Truncation: if the joined path would exceed cap-1 bytes,
// the loop breaks and the current partial path is returned.
// The caller is expected to have sized `out` generously
// (the loader uses 1024 and 4096).
static void join_path(ModuleLoader* ml, Token* toks, int n,
                      char* out, size_t cap) {
    size_t pos = 0;
    for (int i = 0; i < n; i++) {
        SourceFile* f = smap_get(&ml->smap, toks[i].span.file_id);
        // A component whose file_id is not in the map
        // (should not happen; every token was lexed from a
        // registered file) is silently skipped.
        if (!f) continue;
        if (i > 0 && pos < cap - 1) out[pos++] = '/';
        uint32_t len = span_len(toks[i].span);
        // Room check: if the component will not fit with a
        // trailing NUL, stop here. The caller sees a partial
        // path, which is unlikely to resolve.
        if (pos + len + 1 >= cap) break;
        memcpy(out + pos, f->content + toks[i].span.start, len);
        pos += len;
    }
    out[pos] = '\0';
}

// Try the full path, then progressively shorter prefixes, to
// find the longest module prefix that resolves to a file.
// Returns true and writes the resolved file path if found.
// `out_symbol_count` receives the number of trailing components
// that refer to symbols inside the module (0 for a pure module).
//
// The "progressively shorter prefix" strategy is what makes
// `using a.b.c` mean "resolve a.b.c; if that does not name a
// file, try a.b; if that does not, try a". The first prefix
// that resolves is the module; the trailing components
// (if any) name declarations inside that module.
//
// Example:
//   source: `using util.io.print`
//   include dir contains: util/io.manu  (but no util/io/print.manu)
//   -> resolve_module_path tries "util/io/print" (miss),
//      then "util/io" (hit). sym_count = 1.
//   -> the caller records resolved_file_id = util/io's id
//      and resolved_path_count = 2 (path_count - 1).
//
// The caller (load_imports) uses these to fill the AST_USING
// node's resolution fields. The sema's build_module_table
// then interprets trailing == 1 as "selective import of one
// symbol named by the last path component".
static bool resolve_module_path(ModuleLoader* ml,
                                Token* path, int path_count,
                                char*  out_file, size_t out_cap,
                                int*   out_symbol_count) {
    for (int k = path_count; k >= 1; k--) {
        char rel[1024];
        join_path(ml, path, k, rel, sizeof rel);

        char* resolved = resolve_against_includes(ml, rel);
        if (resolved) {
            size_t len = strlen(resolved);
            if (len + 1 >= out_cap) { free(resolved); return false; }
            memcpy(out_file, resolved, len + 1);
            free(resolved);
            *out_symbol_count = path_count - k;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------
// File bookkeeping
// ------------------------------------------------------------

// Find a loaded file by its canonical path. Returns the
// index into `files`, or -1 if not loaded.
//
// Linear scan on strcmp. The number of loaded files is
// small (tens, typically). If it ever grows, a hash keyed
// on the canonical path would be the natural next step.
static int find_file_by_canonical(ModuleLoader* ml, const char* canonical) {
    for (int i = 0; i < ml->file_count; i++)
        if (strcmp(ml->files[i].canonical_path, canonical) == 0) return i;
    return -1;
}

// Is the given file currently on the loading stack? Used for
// cycle detection: a `using` that resolves to a file already
// on the stack means the module graph has a cycle.
static bool on_loading_stack(ModuleLoader* ml, int file_idx) {
    for (int i = 0; i < ml->loading_count; i++)
        if (ml->loading[i] == file_idx) return true;
    return false;
}

// Push a file onto the loading stack.
//
// The stack is used as a "currently being processed" marker;
// the caller pops by decrementing loading_count directly
// (see load_file). There is no pop helper because the pop
// is a single decrement with no other bookkeeping.
static void push_loading(ModuleLoader* ml, int idx) {
    if (ml->loading_count >= ml->loading_capacity) {
        ml->loading_capacity = ml->loading_capacity
                             ? ml->loading_capacity * 2 : 16;
        ml->loading = (int*)xrealloc(ml->loading,
            sizeof(int) * (size_t)ml->loading_capacity);
    }
    ml->loading[ml->loading_count++] = idx;
}

// ------------------------------------------------------------
// Recursive loading
// ------------------------------------------------------------
static bool load_file(ModuleLoader* ml, const char* canonical_path,
                      bool is_root, DiagnosticBag* bag, int* out_idx);

// Load every `using` declaration in a file's AST.
//
// For each non-alias `using`, resolve its path to a file
// (via resolve_module_path). If resolution fails, report
// "cannot find module 'x.y.z'" and continue to the next
// using. If it succeeds, recursively load the target file
// and record the resolution on the AST_USING node:
//
//   resolved_file_id    = the file_id of the resolved file
//   resolved_path_count = the number of leading path
//                         components that named a file
//                         (the rest name symbols inside)
//
// A failure in load_file (cycle, missing file, parse error)
// causes this using to be skipped. The bag has the error,
// and the outer load will eventually fail too.
//
// Note that `load_file` is called with the file path, not
// the file_id: the loader's design is path-driven, with
// file_ids assigned as a side effect of loading.
static bool load_imports(ModuleLoader* ml, int file_idx,
                         DiagnosticBag* bag) {
    ParsedFile* pf   = &ml->files[file_idx];
    AstNode*    prog = pf->program;

    for (int i = 0; i < prog->as.program.decls.count; i++) {
        AstNode* d = prog->as.program.decls.items[i];
        if (d->kind != AST_USING) continue;

        AstUsing* u = &d->as.using_decl;
        // Alias forms (`using Name = TypeExpr`) do not name
        // files; they are handled entirely by sema.
        if (u->kind == USING_ALIAS) continue;

        char file_path[4096];
        int  sym_count = 0;
        if (!resolve_module_path(ml, u->path, u->path_count,
                                 file_path, sizeof file_path, &sym_count)) {
            // Diagnostic uses the joined path, not the
            // individual components; that is what a user
            // wrote.
            char shown[512];
            join_path(ml, u->path, u->path_count, shown, sizeof shown);
            diag_error(bag, d->span, "cannot find module '%s'", shown);
            continue;
        }

        // Recursively load the resolved file. The
        // `target_idx` output is used to fill the
        // resolution fields below.
        int target_idx;
        if (!load_file(ml, file_path, false, bag, &target_idx)) continue;
        u->resolved_file_id = (int)ml->files[target_idx].file_id;
        u->resolved_path_count = u->path_count - sym_count;
    }
    return !diag_has_errors(bag);
}

// Load one file (recursively, via load_imports).
//
// Contract: `canonical_path` must already be canonical
// (realpath'd). The caller (either modload_load for the
// root, or load_imports for a resolved using) is
// responsible for the canonicalization. This is a slight
// asymmetry: modload_load canonicalizes for the root but
// load_imports passes the resolved path directly, on the
// assumption that resolve_against_includes returned a path
// that was constructed from include-dir prefixes (which
// are assumed canonical) plus a user-written relative path.
// In practice this is fine because the include dirs are
// canonicalized when added; if a future include dir were
// non-canonical, two spellings of the same file could be
// loaded twice.
//
// Steps:
//   1. Canonicalize the given path (again; see note above).
//   2. If already loaded: check for a cycle; return the
//      existing index.
//   3. Read the file into memory.
//   4. Add it to the SourceMap (which copies the content).
//   5. Lex + parse it. Record the file_id on the AST.
//   6. Append a ParsedFile entry.
//   7. Push onto the loading stack.
//   8. Recursively load its `using` declarations.
//   9. Pop the loading stack.
//
// Cycle detection is at step 2: if the file is already
// loaded AND on the loading stack, we have a cycle (A
// imports B imports A). The diagnostic is reported and the
// function returns false. A file that is loaded but NOT on
// the stack is a diamond — shared by two importers — and is
// returned directly without re-loading.
//
// Failure at any step: report, free any partial allocations,
// return false. The caller (load_imports) will skip the
// using that triggered this.
static bool load_file(ModuleLoader* ml, const char* raw_path,
                      bool is_root, DiagnosticBag* bag, int* out_idx) {
    char* canonical = canonicalize_path(raw_path);

    // Already loaded? Return the existing entry, unless it's
    // still on the loading stack, which means a cycle.
    int existing = find_file_by_canonical(ml, canonical);
    if (existing >= 0) {
        if (on_loading_stack(ml, existing)) {
            diag_error(bag, SPAN_NONE,
                "circular import involving '%s'", canonical);
            free(canonical);
            return false;
        }
        if (out_idx) *out_idx = existing;
        free(canonical);
        return true;
    }

    // Read the file. Failure to open or read is reported
    // here (modload_load also has a "cannot find" message
    // for the root case; this one is for the recursive
    // case, where the file path came from a resolved using).
    char* content = read_file_content(canonical);
    if (!content) {
        diag_error(bag, SPAN_NONE, "cannot read '%s'", canonical);
        free(canonical);
        return false;
    }

    // Add the file's content to the SourceMap. The map
    // copies the content; the original buffer is freed
    // immediately below. After this point, tokens produced
    // from this file carry `fid` in their spans.
    uint32_t fid = (uint32_t)smap_add(&ml->smap, canonical, content);
    free(content);

    // Parse. The lexer is initialized over the *SourceMap's*
    // copy of the content (not the freed local buffer), so
    // the tokens' spans point into the map's memory.
    Lexer lex;
    lexer_init(&lex, smap_get(&ml->smap, fid)->content, fid);
    AstNode* prog = parse_program(&lex, &ml->smap, bag);
    // The AST_PROGRAM gets the file_id and a duplicated
    // canonical path. The path is duplicated because the
    // ParsedFile's canonical_path is separate: the AST's
    // copy is for diagnostics that want to name the file,
    // and the ParsedFile's copy is for the loader's dedup.
    // (The AST's copy is freed by ast_free when the
    // AST_PROGRAM is freed; the ParsedFile's copy is freed
    // by modload_free.)
    prog->as.program.file_id        = fid;
    prog->as.program.canonical_path = dup_string(canonical);

    // Register the ParsedFile entry. Growth: doubling from
    // 8, same pattern as everywhere else.
    if (ml->file_count >= ml->file_capacity) {
        ml->file_capacity = ml->file_capacity ? ml->file_capacity * 2 : 8;
        ml->files = (ParsedFile*)xrealloc(ml->files,
            sizeof(ParsedFile) * (size_t)ml->file_capacity);
    }
    int idx = ml->file_count++;
    ml->files[idx].file_id        = fid;
    ml->files[idx].canonical_path = canonical;   // ownership moves
    ml->files[idx].program        = prog;
    ml->files[idx].is_root        = is_root;

    // Push onto the loading stack, recursively load imports,
    // then pop. The pop is a direct decrement; the entry
    // itself is left in the array (it will be overwritten
    // by the next push).
    push_loading(ml, idx);
    load_imports(ml, idx, bag);
    ml->loading_count--;

    if (out_idx) *out_idx = idx;
    return !diag_has_errors(bag);
}

// Load a root file and its transitive imports.
//
// Two attempts:
//   1. The path as given. If the file exists, load it
//      directly. This is the common case: the CLI was
//      invoked as `manu run path/to/file.manu` and the
//      path is valid relative to the current directory.
//   2. Else resolve against the include directories.
//      This handles the case where the CLI was invoked
//      with a module-relative name (`manu run file` and
//      `file.manu` is in an include directory) or where
//      the extension was omitted.
//
// The two attempts use different code paths: (1) calls
// load_file directly with the given path; (2) resolves
// via resolve_against_includes and then calls load_file.
// Both paths converge at load_file, which does the
// canonicalization and dedup.
bool modload_load(ModuleLoader* ml, const char* path, DiagnosticBag* bag) {
    // The CLI typically passes a path like "main.manu" or
    // "subdir/main.manu". If it exists as given, use it. This
    // is the common case and avoids include-path scanning.
    if (file_exists(path)) {
        int idx;
        return load_file(ml, path, true, bag, &idx);
    }

    // Otherwise treat it as a module-relative name and search
    // the include directories.
    char* resolved = resolve_against_includes(ml, path);
    if (!resolved) {
        diag_error(bag, SPAN_NONE, "cannot find '%s'", path);
        return false;
    }
    int idx;
    bool ok = load_file(ml, resolved, true, bag, &idx);
    free(resolved);
    return ok;
}

// ------------------------------------------------------------
// Merge for the current single-namespace sema
// ------------------------------------------------------------

// Flatten every loaded file's declarations into one
// AST_PROGRAM.
//
// Ownership: the returned program takes ownership of every
// declaration pointer from every file. Each file's decl
// list is emptied (count set to 0) but the array itself
// (items) is not freed; it will be freed when modload_free
// calls ast_free on the AST_PROGRAM (which then frees the
// empty list's items buffer).
//
// File order in the merged program matches load order:
// the root file's decls first, then its imports', then
// their imports', etc. That ordering is not load-bearing
// for correctness (sema's passes are order-independent for
// type/function definitions), but it makes diagnostics
// that iterate the merged program list in order.
//
// The `is_root` flag on each ParsedFile is not consulted
// here; it is preserved for callers that want to know which
// file was the CLI entry point (currently nothing does).
AstNode* modload_merge(ModuleLoader* ml) {
    AstNode* merged = ast_program();
    for (int i = 0; i < ml->file_count; i++) {
        AstNode* f  = ml->files[i].program;
        AstList* fl = &f->as.program.decls;

        // Transfer every declaration, including AST_USING, to the
        // merged program. Sema walks the merged list to build the
        // per-file module table.
        //
        // Note that the transfer is a raw pointer push, not a
        // deep copy. The declarations retain their original
        // spans (and therefore their original file_ids), which
        // is what allows sema and codegen to distinguish
        // "declared in the root" from "declared in an import".
        for (int j = 0; j < fl->count; j++)
            ast_list_push(&merged->as.program.decls, fl->items[j]);

        // Ownership of the items has moved to `merged`; the file
        // AST no longer owns them.
        //
        // The `items` array is left allocated (with stale
        // pointers past the reset count). ast_free will free
        // it when the file's AST_PROGRAM is freed, and it
        // will not walk past count == 0, so the stale
        // pointers are harmless.
        fl->count = 0;
    }
    return merged;
}