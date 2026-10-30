// ============================================================
// resolve.c — Centralized global name resolution.
//
// Reading order:
//   1. Per-file lookups: find a declaration declared in a
//      specific file.
//   2. Selective-import fallthrough.
//   3. Public entry points.
//
// The resolver never allocates. Every lookup is a linear
// scan over a table; the tables are small (tens to low
// hundreds of entries) and the number of lookups per
// compilation is bounded by the source's size.
//
// The `sc` parameter is const because the resolver only
// reads; callers that need to mutate tables do so
// themselves.
// ============================================================
#include "../include/resolve.h"
#include "../include/sema.h"
#include "../include/types.h"
#include "../include/monomorph.h"
#include "../include/source_map.h"
#include "../include/constant.h"

// ============================================================
// Module table lookups
//
// The FileNamespace stores a file's imports (module aliases)
// and selective imports (`using a.b.{ x }`). These three
// lookups are linear scans over small arrays; they are the
// only way anything reads a FileNamespace, so they live here
// with the resolver that uses them.
// ============================================================

// Look up a FileNamespace by file_id. Returns NULL if the
// file has no namespace entry (which is normal for a file
// with no imports). Linear scan; the number of files in a
// typical compilation is small.
FileNamespace* modtable_find(const ModuleTable* mt, uint32_t file_id) {
    for (int i = 0; i < mt->file_count; i++)
        if (mt->files[i].file_id == file_id)
            return (FileNamespace*)&mt->files[i];
    return NULL;
}

// Resolve a module alias to a file_id: given the local
// name of a module (e.g. `c` in `using a.b as c`), find the
// imported file's id. Returns -1 if no such import exists.
int fnamespace_resolve(const FileNamespace* fn,
                       const SourceMap* smap, Token name) {
    for (int i = 0; i < fn->import_count; i++)
        if (smap_tokens_equal(smap, fn->imports[i].local_name, name))
            return fn->imports[i].module_file_id;
    return -1;
}

// Find a selective import by its local name. Returns a
// borrowed pointer into the namespace, or NULL. Used by the
// resolver in types.c (for `a.b.Type` where `b` is
// selective) and by sema itself (for the "is this a
// selective import?" check in analyze_expr's AST_VARIANT_CTOR
// and AST_FIELD cases).
const SelectiveImport* fnamespace_find_selective(
    const FileNamespace* fn, const SourceMap* smap, Token name) {
    for (int i = 0; i < fn->selective_count; i++)
        if (smap_tokens_equal(smap, fn->selective[i].local_name, name))
            return &fn->selective[i];
    return NULL;
}

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

// ------------------------------------------------------------
// Per-file lookups
//
// These find a declaration by name within a single file. They
// are the building blocks of the public functions; each is a
// straight linear scan filtered by file_id and pub.
//
// Precedence within a file, in order: functions, types,
// templates, constants. The order is arbitrary but fixed;
// changing it would change which declaration shadows which
// for the (rare) case of a name reused across kinds.
// ------------------------------------------------------------

static ResolvedDecl find_func_in_scope(const SemaCtx* sc,
                                       uint32_t file_id,
                                       Token name) {
    for (int i = 0; i < sc->func_count; i++) {
        if (sc->funcs[i].is_instantiated) continue;
        if (sc->funcs[i].file_id != file_id) continue;
        if (smap_tokens_equal(sc->smap, sc->funcs[i].name, name)) {
            ResolvedDecl r = { RD_FUNC, i , NULL};
            return r;
        }
    }
    return rd_none();
}

static ResolvedDecl find_type_in_scope(const SemaCtx* sc,
                                       uint32_t file_id,
                                       Token name) {
    for (int i = 0; i < sc->structs->count; i++) {
        const TypeDef* td = &sc->structs->items[i];
        if (td->is_instantiated) continue;
        if (td->name.span.file_id != file_id) continue;
        if (smap_tokens_equal(sc->smap, td->name, name)) {
            ResolvedDecl r = { RD_TYPE, i ,NULL};
            return r;
        }
    }
    return rd_none();
}

static ResolvedDecl find_template_in_scope(const SemaCtx* sc,
                                           uint32_t file_id,
                                           Token name) {
    int n = mono_template_count(sc->structs->generics);
    for (int i = 0; i < n; i++) {
        GenericTemplate* t = mono_template_at(sc->structs->generics, i);
        if (t->file_id != file_id) continue;
        if (smap_tokens_equal(sc->smap, t->name, name)) {
            ResolvedDecl r = { RD_TEMPLATE, i ,NULL};
            return r;
        }
    }
    return rd_none();
}

static ResolvedDecl find_const_in_scope(const SemaCtx* sc,
                                        uint32_t file_id,
                                        Token name) {
    int idx = consts_resolve(sc->consts, sc->smap, name);
    if (idx < 0) return rd_none();
    const ConstEntry* ce = consts_get(sc->consts, idx);
    if (!ce) return rd_none();
    if (ce->name.span.file_id != file_id) return rd_none();
    ResolvedDecl r = { RD_CONST, idx ,NULL};
    return r;
}

// The full "declare in this file" lookup, in precedence order.
static ResolvedDecl find_in_scope(const SemaCtx* sc,
                                  uint32_t file_id,
                                  Token name) {
    ResolvedDecl r;
    r = find_func_in_scope(sc, file_id, name);
    if (r.kind != RD_NONE) return r;
    r = find_type_in_scope(sc, file_id, name);
    if (r.kind != RD_NONE) return r;
    r = find_template_in_scope(sc, file_id, name);
    if (r.kind != RD_NONE) return r;
    r = find_const_in_scope(sc, file_id, name);
    if (r.kind != RD_NONE) return r;
    return rd_none();
}

// ------------------------------------------------------------
// Selective imports
//
// A selective import gives a local name for a symbol declared
// in another module: `using a.b.{ x as y }` lets the importing
// file write `y` and reach `x` in a/b.manu.
//
// `module_file_id` is the file the loader resolved the module
// path to; the resolver looks up the source name in that file.
// ------------------------------------------------------------
static ResolvedDecl find_selective(const SemaCtx* sc,
                                   uint32_t file_id,
                                   Token name) {
    const FileNamespace* fns = modtable_find(&sc->modtable, file_id);
    if (!fns) return rd_none();
    const SelectiveImport* si =
        fnamespace_find_selective(fns, sc->smap, name);
    if (!si) return rd_none();
    return find_in_scope(sc, (uint32_t)si->module_file_id, si->source_name);
}

// ------------------------------------------------------------
// Public entry points
// ------------------------------------------------------------
ResolvedDecl resolve_name(const SemaCtx* sc, uint32_t file_id, Token name) {
    // 1. Declared in this file.
    ResolvedDecl r = find_in_scope(sc, file_id, name);
    if (r.kind != RD_NONE) return r;

    // 2. Selective import of this name.
    r = find_selective(sc, file_id, name);
    if (r.kind != RD_NONE) return r;

    // 3. Builtin type.
    char buf[64];
    smap_span_text(sc->smap, name.span, buf, sizeof buf);
    TypeDef* bt = builtin_tdef_by_name(buf);
    if (bt) {
        ResolvedDecl rb = { RD_BUILTIN_TYPE, -1, bt };
        return rb;
    }

    // 4. Prelude. Every file implicitly imports the prelude's
    //    declarations; this is the language's rule that
    //    Option and Result are always available. The lookup
    //    is identical to the "declared in this file" case,
    //    just against the prelude's file_id. Skipped when
    //    the caller *is* the prelude, and when no prelude
    //    is loaded.
    if (sc->prelude_file_id != 0 && file_id != sc->prelude_file_id) {
        r = find_in_scope(sc, sc->prelude_file_id, name);
        if (r.kind != RD_NONE) return r;
    }

    return rd_none();
}

ResolvedDecl resolve_qualified(const SemaCtx* sc, uint32_t file_id,
                               Token alias, Token name) {
    // `alias` must name a module imported into `file_id`.
    // Module aliases come from `using X as alias` or from
    // `using X` (where the alias is X's last path component).
    const FileNamespace* fns = modtable_find(&sc->modtable, file_id);
    if (!fns) return rd_none();

    int target_file = fnamespace_resolve(fns, sc->smap, alias);
    if (target_file < 0) return rd_none();

    // The name is looked up in the target module. Only
    // top-level declarations are visible; a name declared in
    // some inner scope of the target module is not.
    return find_in_scope(sc, (uint32_t)target_file, name);
}

ResolvedDecl resolve_template(const SemaCtx* sc, uint32_t file_id, Token name) {
    // 1. Template declared in this file.
    ResolvedDecl r = find_template_in_scope(sc, file_id, name);
    if (r.kind != RD_NONE) return r;

    // 2. Template brought in by a selective import.
    const FileNamespace* fns = modtable_find(&sc->modtable, file_id);
    if (fns) {
        const SelectiveImport* si =
            fnamespace_find_selective(fns, sc->smap, name);
        if (si) {
            ResolvedDecl r2 = find_template_in_scope(
                sc, (uint32_t)si->module_file_id, si->source_name);
            if (r2.kind != RD_NONE) return r2;
        }
    }

    // 3. Prelude.
    if (sc->prelude_file_id != 0 && file_id != sc->prelude_file_id) {
        r = find_template_in_scope(sc, sc->prelude_file_id, name);
        if (r.kind != RD_NONE) return r;
    }

    return rd_none();
}

ResolvedDecl resolve_in_module(const SemaCtx* sc, uint32_t file_id,
                               Token name) {
    return find_in_scope(sc, file_id, name);
}
