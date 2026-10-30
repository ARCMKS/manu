// ============================================================
// sema_modules.c — Module table construction and validation.
//
// Two responsibilities:
//
//   build_module_table      Populates the ModuleTable with
//                           the imports and selective imports
//                           declared by each file's `using`
//                           statements. Runs before Pass A;
//                           the resolver and sema both read
//                           the table.
//
//   validate_selective_imports
//                           After Pass A, checks that each
//                           selective import names a
//                           declaration that actually exists
//                           in the target module and, for
//                           functions, that it is `pub`.
//
// The three helpers in this file (modtable_get_or_add and the
// two fnamespace_add_* functions) are private; only the two
// entry points are visible to sema.c. The read-only module
// table accessors (modtable_find, fnamespace_resolve,
// fnamespace_find_selective) live in resolve.c.
// ============================================================
#include "../include/sema_internal.h"
#include "../include/resolve.h"
#include "../include/source_map.h"
#include "../include/diagnostics.h"
#include "../include/types.h"
#include "../include/ast.h"
#include "../include/common.h"
#include <string.h>

// ---------- Private helpers ----------

// Get-or-create a FileNamespace for `file_id`.
//
// The cast away of const on the returned pointer mirrors
// modtable_find; the map is conceptually non-const from the
// perspective of callers that are filling it in. The
// alternative (returning `FileNamespace*` from a non-const
// parameter, or splitting into find and find_mut) is more
// code for no practical benefit.
//
// The freshly-created entry is memset to zero, so the
// imports[] and selective[] arrays start empty (NULL/0/0).
// Growth is doubling from 8, matching the compiler's
// standard growth policy.
static FileNamespace* modtable_get_or_add(ModuleTable* mt, uint32_t file_id) {
    FileNamespace* fn = modtable_find(mt, file_id);
    if (fn) return fn;
    if (mt->file_count >= mt->file_capacity) {
        mt->file_capacity = mt->file_capacity ? mt->file_capacity * 2 : 8;
        mt->files = (FileNamespace*)xrealloc(mt->files,
            sizeof(FileNamespace) * (size_t)mt->file_capacity);
    }
    fn = &mt->files[mt->file_count++];
    memset(fn, 0, sizeof *fn);
    fn->file_id = file_id;
    return fn;
}

// Record one `using a.b as alias` (module import). The
// local_name is what the importing file writes to refer to
// the module; module_file_id is the id of the file the
// import resolved to.
static void fnamespace_add(FileNamespace* fn, Token local_name,
                           int module_file_id) {
    if (fn->import_count >= fn->import_capacity) {
        fn->import_capacity = fn->import_capacity
                            ? fn->import_capacity * 2 : 4;
        fn->imports = (FileImport*)xrealloc(fn->imports,
            sizeof(FileImport) * (size_t)fn->import_capacity);
    }
    fn->imports[fn->import_count].local_name     = local_name;
    fn->imports[fn->import_count].module_file_id = module_file_id;
    fn->import_count++;
}

// Record one selective import (`using a.b.{ x as y }`). The
// `local` token is what the importing file writes; `source`
// is the name in the target module. For a non-aliased `x`,
// the two are the same token value.
static void fnamespace_add_selective(FileNamespace* fn, Token local,
                                     Token source, int module_file_id) {
    if (fn->selective_count >= fn->selective_capacity) {
        fn->selective_capacity = fn->selective_capacity
                               ? fn->selective_capacity * 2 : 4;
        fn->selective = (SelectiveImport*)xrealloc(fn->selective,
            sizeof(SelectiveImport) * (size_t)fn->selective_capacity);
    }
    fn->selective[fn->selective_count].local_name     = local;
    fn->selective[fn->selective_count].source_name    = source;
    fn->selective[fn->selective_count].module_file_id = module_file_id;
    fn->selective_count++;
}

// ============================================================
// Build module table
//
// Walks the top-level AST and, for each non-alias `using`
// declaration, records the import into the appropriate
// FileNamespace. Runs before Pass A so that when function
// bodies (and type expressions) refer to a qualified name,
// the module table is already populated.
//
// Three cases for a module `using`:
//
//   resolved_path_count == path_count
//     The whole path resolved to a file. `using a.b.c`
//     imports module c (or the alias) as a module. Add to
//     `imports[]`.
//
//   resolved_path_count == path_count - 1
//     The path has one trailing symbol after the file
//     portion. `using a.b.c` where `a.b` is the file and `c`
//     is a symbol inside it. This is sugar for a selective
//     import of one name; add to `selective[]`.
//
//   resolved_path_count < path_count - 1
//     Two or more trailing symbols. Not supported; report
//     an error suggesting the selective-import syntax.
//
// `resolved_path_count` and `resolved_file_id` are set by
// the module loader (module.c) before sema runs. If the
// loader found no file, resolved_file_id is 0 (the sentinel
// for "no file"), and the case still runs but with a bogus
// id. That is a known limitation; see the follow-ups.
void build_module_table(ModuleTable* mt, AstNode* program,
                               DiagnosticBag* bag) {
    AstList* decls = &program->as.program.decls;
    for (int i = 0; i < decls->count; i++) {
        AstNode* d = decls->items[i];
        if (d->kind != AST_USING) continue;

        AstUsing* u = &d->as.using_decl;
        // Type aliases (`using Name = TypeExpr`) are not
        // module imports and do not go in the module table.
        if (u->kind == USING_ALIAS) continue;

        // The namespace entry is keyed by the file that
        // contains the `using`, not by the target file.
        uint32_t owner_file = d->span.file_id;
        FileNamespace* fn = modtable_get_or_add(mt, owner_file);

        if (u->kind == USING_MODULE) {
            int trailing = u->path_count - u->resolved_path_count;
            if (trailing == 0) {
                // Whole path named a file. Local name is
                // the alias if one was given, else the
                // last path component.
                Token local = u->has_module_alias
                            ? u->module_alias
                            : u->path[u->path_count - 1];
                fnamespace_add(fn, local, u->resolved_file_id);
            } else if (trailing == 1) {
                // One trailing symbol: `using a.b.c` where
                // a.b is the file and c is a symbol. Record
                // it as a selective import with local_name
                // = alias or c, source_name = c.
                Token sym = u->path[u->path_count - 1];
                Token local = u->has_module_alias ? u->module_alias : sym;
                fnamespace_add_selective(fn, local, sym,
                                         u->resolved_file_id);
            } else {
                // Deeper nesting is not supported; the
                // error message points at the selective
                // syntax as the workaround.
                diag_error(bag, d->span,
                    "nested symbol paths deeper than one level "
                    "are not supported; use `using a.b.c.{ Symbol }`");
            }
        } else if (u->kind == USING_SELECTIVE) {
            // Straightforward: every entry in names[] becomes
            // one selective import. The parser has already
            // resolved local_name (alias or source).
            for (int k = 0; k < u->name_count; k++)
                fnamespace_add_selective(fn,
                    u->names[k].local, u->names[k].source,
                    u->resolved_file_id);
        }
    }
}

// Validate selective imports against the function table
// (and, as a fallback, the type table).
//
// This is a separate pass because it runs *after* Pass A:
// the function table must be populated before we can check
// whether `using a.b.{ x }` actually named a real symbol in
// a.b. It is called from sema_analyze between Pass A and
// Pass B.
//
// Each selective import is checked in two ways:
//   - If the source name matches a function in the target
//     file, that function must be `pub`.
//   - Else if the source name matches a type in the target
//     file, the import is accepted (type visibility is
//     checked at use sites, not here).
//   - Else the import names nothing; report an error.
//
// Note that the diagnostic for a private import names the
// target module and the offending symbol, and points at the
// symbol's span in the *target* file, not in the importing
// file. That is deliberate: the error is about the target
// symbol being non-public, not about the import statement
// itself.
void validate_selective_imports(AstNode* program, SemaCtx* sc,
                                       DiagnosticBag* bag) {
    AstList* decls = &program->as.program.decls;
    for (int i = 0; i < decls->count; i++) {
        AstNode* d = decls->items[i];
        if (d->kind != AST_USING) continue;
        AstUsing* u = &d->as.using_decl;
        if (u->kind != USING_SELECTIVE) continue;

        // The module's last path component is used purely
        // for the diagnostic message ("module 'foo' has no
        // public item 'bar'").
        char mod_buf[64];
        smap_span_text(sc->smap, u->path[u->path_count - 1].span,
                       mod_buf, sizeof mod_buf);

        for (int k = 0; k < u->name_count; k++) {
            Token source = u->names[k].source;
            ResolvedDecl rd = resolve_in_module(sc,
                (uint32_t)u->resolved_file_id, source);

            if (rd.kind == RD_NONE) {
                char sn[64];
                smap_span_text(sc->smap, source.span, sn, sizeof sn);
                diag_error(bag, source.span,
                    "module '%s' has no item '%s'",
                    mod_buf, sn);
                continue;
            }

            // Types are accepted without a visibility check
            // here; their pub-ness is checked at each use
            // site. Functions are checked here because a
            // selective import is the only place that
            // discovers them.
            if (rd.kind == RD_TYPE || rd.kind == RD_BUILTIN_TYPE)
                continue;

            if (rd.kind == RD_FUNC) {
                if (!sc->funcs[rd.index].is_pub) {
                    const char* def_path = "<unknown>";
                    SourceFile* sf = smap_get(sc->smap,
                        sc->funcs[rd.index].file_id);
                    if (sf) def_path = sf->path;
                    char sn[64];
                    smap_span_text(sc->smap, source.span, sn, sizeof sn);
                    diag_error(bag, source.span,
                        "'%s.%s' is private; declared in %s "
                        "and not marked 'pub'",
                        mod_buf, sn, def_path);
                }
                continue;
            }

            // Templates and constants: accepted without a
            // visibility check for now, matching the current
            // behavior for types.
        }
    }
}
