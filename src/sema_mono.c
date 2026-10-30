// ============================================================
// sema_mono.c — Generic instantiation and the instantiation
// cache.
//
// When a TEX_GENERIC is resolved (types.c calls back into
// sema via the TypeDefTable's instantiate_generic callback) or
// an explicit call like `f<i32>(...)` is seen in a body, this
// file does the work: deep-copy the template's AST with
// substitution, register the copy as a concrete TypeDef or
// FuncSig, and cache the result so the same instantiation is
// never done twice.
//
// Cache invalidation is by convention, not by key: the cache
// is reset at the top of every sema_analyze. A compilation
// has exactly one sema_analyze.
//
// This file is the only place that calls analyze_func_body on
// an instantiated copy; the analysis itself lives in sema.c.
// That is why analyze_func_body's declaration is in
// sema_internal.h.
// ============================================================
#include "../include/sema_internal.h"
#include "../include/types.h"
#include "../include/ast.h"
#include "../include/monomorph.h"
#include "../include/source_map.h"
#include "../include/diagnostics.h"
#include "../include/common.h"
#include "../include/resolve.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---------- Instantiation cache ----------

// ============================================================
// Instantiation cache
//
// Global (file-static) so it survives across the recursive
// calls that happen during a single sema_analyze. Reset at
// the start of each sema_analyze so consecutive compilations
// do not see each other's instantiations.
//
// The cache maps (template, arg types) -> the concrete
// TypeDef or the function-table index of the instantiation.
// Both results can be present for the same cache entry if
// the same generic name is used both as a type (struct/
// enum) and as a function — though in practice a template is
// either a type template or a function template, never both.
//
// One of `type_result` / `func_result` is set per entry:
//   type_result != NULL   — the entry is a type instantiation.
//   func_result >= 0      — the entry is a function
//                           instantiation, and the value is
//                           the index into SemaCtx.funcs[].
//
// Entries with neither set are placeholders created by
// cache_insert before the instantiation completed; on a
// lookup they are treated as misses for the result being
// requested (see the two callers).
// ============================================================
typedef struct {
    GenericTemplate* tmpl;
    Type**           args;
    int              arg_count;
    TypeDef*         type_result;
    int              func_result;   // sema index into sc->funcs[]
} InstEntry;

typedef struct {
    InstEntry items[MAX_INST_CACHE];
    int       count;
} InstCache;

static InstCache g_cache;

// Free every entry's argument array and zero the cache. Note
// that the cache itself is zeroed by memset, so the count is
// reset; the entries are then reused from index 0.
//
// The GenericTemplate* and TypeDef* pointers in the entries
// are borrowed (they live in the program AST and the type
// table respectively) and are not freed here.
void cache_reset(void) {
    for (int i = 0; i < g_cache.count; i++)
        free(g_cache.items[i].args);
    memset(&g_cache, 0, sizeof g_cache);
}

// Look up a cache entry by (template, args). Returns the
// entry or NULL. The args are compared with type_eq, not
// pointer equality, because two calls can produce
// structurally identical Type* objects (see the note on
// compound type constructors in types.c).
static InstEntry* cache_lookup(GenericTemplate* tmpl, Type** args, int n) {
    for (int i = 0; i < g_cache.count; i++) {
        InstEntry* e = &g_cache.items[i];
        if (e->tmpl != tmpl || e->arg_count != n) continue;
        bool same = true;
        for (int j = 0; j < n; j++)
            if (!type_eq(e->args[j], args[j])) { same = false; break; }
        if (same) return e;
    }
    return NULL;
}

// Insert a new cache entry, copying the args array. Returns
// NULL if the cache is full; callers must handle that (the
// instantiations still happen, but the caching is skipped).
//
// Note: the copy is a shallow copy of Type* pointers. The
// types themselves live for the process lifetime, so no
// ownership is transferred.
//
// `func_result` is initialized to -1 to mean "no function
// instantiation yet" (0 would be a valid index). type_result
// starts NULL.
static InstEntry* cache_insert(GenericTemplate* tmpl, Type** args, int n) {
    if (g_cache.count >= MAX_INST_CACHE) return NULL;
    InstEntry* e = &g_cache.items[g_cache.count++];
    memset(e, 0, sizeof *e);
    e->tmpl = tmpl;
    e->arg_count = n;
    e->args = (Type**)xmalloc(sizeof(Type*) * (size_t)n);
    for (int i = 0; i < n; i++) e->args[i] = args[i];
    e->func_result = -1;
    return e;
}


// ---------- Type instantiation ----------

// ============================================================
// Type instantiation (callback invoked from type_expr_resolve)
//
// This function is installed on the TypeDefTable as the
// instantiate_generic callback (see sema_analyze). Types.c
// calls it whenever it encounters a TEX_GENERIC during
// resolution. The `void* generics` parameter is the
// TemplateList handle from monomorph.c; the `void* ctx` is
// the SemaCtx.
//
// Flow:
//   1. Resolve the argument TypeExprs to concrete Types. If
//      any argument fails to resolve, bail out with NULL.
//   2. Look up the template by name. If not found, report
//      "unknown generic type".
//   3. Check the argument count matches the template's
//      parameter count.
//   4. Check the cache. If a type_result is present, return
//      it (wrapped in ty_rc if the TypeDef is RC-managed).
//   5. Deep-copy the template's AST with the substitution.
//   6. Register the copy in the struct table with a mangled
//      name, mark it is_instantiated and (if the template
//      was builtin) is_rc_managed.
//   7. Walk the copy's fields or variants, resolving each
//      one against the substituted types and filling the
//      TypeDef's layout.
//   8. Free the AST copy (the TypeDef holds everything it
//      needs).
//   9. Cache the result and return the (possibly RC-wrapped)
//      type.
//
// Error handling: every failure path reports into `bag` and
// returns NULL. The AST copy is freed on the internal-error
// path only; on the normal path it is freed at step 8.
//
// The `is_rc_managed` flag on the TypeDef is copied from
// the template's `is_builtin`. This is how the prelude's
// Result and Option (marked is_builtin) become RC-managed
// at runtime. A user-defined generic type is not RC-managed
// by default; if the user wants RC, they must write
// `rc<TheirGeneric<T>>` at the use site, which the resolver
// will recognize.
Type* sema_instantiate_generic(void* generics, TypeExpr* te,
                                      const SourceMap* smap, void* ctx,
                                      uint32_t file_id,
                                      DiagnosticBag* bag) {
    SemaCtx* sc = (SemaCtx*)ctx;
    if (!sc) return NULL;

    // --- Step 1: resolve type arguments ---
    // The 8-arg cap mirrors the parser's cap on TEX_GENERIC
    // argument lists (see type_expr_generic_add_arg in
    // types.c, which grows without bound, but the AST
    // payload for a function or struct caps at 8). This is
    // a hard limit; more than 8 type arguments is an error.
    Type* arg_types[8];
    int   n = te->arg_count;
    if (n > 8) {
        diag_error(bag, te->span, "too many type arguments (max 8)");
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        arg_types[i] = type_expr_resolve(te->args[i], smap, sc->structs,
                                         &sc->modtable, file_id, sc, bag);
        if (!arg_types[i]) return NULL;
    }

    // --- Step 2: find the template ---
    GenericTemplate* tmpl = mono_find_template(generics, smap, te->name);
    if(!tmpl) {
        tmpl = mono_find_template_in_file(generics, smap, te->name, file_id);
    }
    if (!tmpl) {
        char buf[64];
        smap_span_text(smap, te->name.span, buf, sizeof buf);
        diag_error(bag, te->span, "unknown generic type '%s'", buf);
        return NULL;
    }
    // --- Step 3: arity check ---
    if (tmpl->param_count != n) {
        char buf[64];
        smap_span_text(smap, te->name.span, buf, sizeof buf);
        diag_error(bag, te->span,
            "generic '%s' expects %d type argument(s), got %d",
            buf, tmpl->param_count, n);
        return NULL;
    }

    // --- Step 4: cache lookup ---
    // Only a *completed* type instantiation is returned from
    // the cache. A placeholder entry (created by
    // cache_insert but not yet filled) is treated as a miss,
    // so the instantiation runs again. There is no cycle in
    // practice (a generic type cannot refer to itself before
    // being defined), so the placeholder path is not
    // exercised today; it exists as a defensive shape.
    InstEntry* e = cache_lookup(tmpl, arg_types, n);
    if (e && e->type_result) {
        TypeDef* td = e->type_result;
        Type* base = (td->kind == TDEF_STRUCT) ? ty_struct(td) : ty_enum(td);
        return td->is_rc_managed ? ty_rc(base) : base;
    }

    // --- Step 5: deep-copy with substitution ---
    Subst sub;
    sub.params = tmpl->params;
    sub.args   = arg_types;
    sub.count  = n;

    AstNode* copy = mono_copy_node(tmpl->decl, &sub, smap);

    // Mangled name is what the TypeDef will be registered
    // under; it is also what the runtime descriptor will
    // render (see typetable_intern's struct name handling).
    char base_buf[64];
    smap_span_text(smap, te->name.span, base_buf, sizeof base_buf);
    char mangled[128];
    mono_mangle(mangled, sizeof mangled, base_buf, arg_types, n);

    // --- Step 6: register the TypeDef ---
    // The copy is registered under the mangled name, not the
    // original. That is what keeps user lookups of `Option`
    // from finding `Option#i64` (see the is_instantiated
    // filter in typedef_table_find).
    TypeDef* td;
    if (copy->kind == AST_STRUCT_DEF) {
        td = typedef_table_add(sc->structs, copy->as.struct_def.name, smap);
        snprintf(td->name_buf, sizeof td->name_buf, "%s", mangled);
        td->kind = TDEF_STRUCT;
        // Instantiated types are visible everywhere; the
        // original may have been pub or not, but the
        // instantiation is treated as pub regardless. This
        // matters because the prelude's types are not
        // declared `pub`, yet they must be usable from any
        // file.
        td->is_pub = true;
        td->is_instantiated = true;
        // is_rc_managed is copied from the template's
        // is_builtin flag: prelude-defined generics become
        // RC-managed, user-defined ones do not.
        //td->is_rc_managed = tmpl->is_builtin;
        // Struct sizes start at 8 (the tag slot); fields add
        // 8 bytes each below.
        td->struct_size = 8;
        copy->as.struct_def.sdef_index = (int)(td - sc->structs->items);
    } else if (copy->kind == AST_ENUM_DEF) {
        td = typedef_table_add(sc->structs, copy->as.enum_def.name, smap);
        snprintf(td->name_buf, sizeof td->name_buf, "%s", mangled);
        td->kind = TDEF_ENUM;
        td->is_pub = true;
        td->is_instantiated = true;
        //td->is_rc_managed = tmpl->is_builtin;
        copy->as.enum_def.tdef_index = (int)(td - sc->structs->items);
    } else {
        // Should be unreachable: mono_register_template is
        // only called for AST_STRUCT_DEF and AST_ENUM_DEF
        // (see register_struct_or_enum). Defensive.
        diag_error(bag, te->span,
            "internal: generic template is not a type declaration");
        ast_free(copy);
        return NULL;
    }

    // Cache the result before the field resolution below,
    // because field resolution can recurse (a field's type
    // might itself be a generic instantiation). The
    // placeholder is filled in now, so a recursive lookup
    // for the same type finds a completed entry rather than
    // re-instantiating. This is technically a partial
    // entry (fields are not resolved yet), but the field
    // resolution does not read the cache for its own type,
    // so the ordering is fine.
    if (!e) e = cache_insert(tmpl, arg_types, n);
    if (e) e->type_result = td;

    // --- Step 7: resolve fields / variants and compute layout ---
    if (copy->kind == AST_STRUCT_DEF) {
        // Struct field layout:
        //   byte  0: tag / discriminator slot (unused for
        //            a plain struct, reserved so that
        //            struct and enum layouts share a prefix)
        //   byte  8: first field
        //   byte 16: second field, ...
        // Every field is 8 bytes because that is the VM's
        // value size. The layout is chosen by this loop; a
        // matching reader is in codegen (see the field
        // offset emission in AST_FIELD).
        AstList* fl = &copy->as.struct_def.fields;
        for (int j = 0; j < fl->count; j++) {
            AstNode* fld = fl->items[j];
            Type* ft = type_expr_resolve(fld->as.struct_field.type_expr,
                                         smap, sc->structs, &sc->modtable,
                                         file_id, sc, bag);
            if (!ft) continue;
            // ref fields in a struct are rejected for the
            // same reason as elsewhere: a struct may
            // outlive the scope that owns the referenced
            // value, leaving a dangling reference.
            if (ft->kind == TY_REF) {
                diag_error(bag, fld->span,
                    "struct fields cannot be of type ref<T>; "
                    "references are scope-bound and would dangle");
                continue;
            }
            fld->as.struct_field.type = ft;
            // Layout: 8 bytes per field, starting at offset 8.
            // td->field_count is the number of fields already
            // added; the new field is at slot field_count.
            fld->as.struct_field.offset = 8 + td->field_count * 8;

            // Grow the TypeDef's flat field array if needed.
            if (td->field_count >= td->field_capacity) {
                td->field_capacity = td->field_capacity
                                   ? td->field_capacity * 2 : 4;
                td->fields = (StructField*)xrealloc(td->fields,
                    sizeof(StructField) * (size_t)td->field_capacity);
            }
            td->fields[td->field_count].name = fld->as.struct_field.name;
            td->fields[td->field_count].type = ft;
            td->fields[td->field_count].offset = fld->as.struct_field.offset;
            td->field_count++;
            td->struct_size += 8;
        }
    } else {
        // Enum variant layout. Each variant is a name plus
        // a payload Type* array (allocated fresh per
        // variant). The tag is the variant's index in
        // declaration order (0, 1, 2, ...).
        AstList* vl = &copy->as.enum_def.variants;
        for (int j = 0; j < vl->count; j++) {
            AstNode* v = vl->items[j];

            // Same variant-count cap as resolve_enum_variants.
            // (An instantiated generic enum has the same
            // coverage-array bound as a non-generic one.)
            if (td->variant_count >= MAX_ENUM_VARIANTS) {
                diag_error(bag, v->span,
                    "enum '%s' has more than %d variants (max %d)",
                    td->name_buf, MAX_ENUM_VARIANTS, MAX_ENUM_VARIANTS);
                break;
            }
            
            if (td->variant_count >= td->variant_capacity) {
                td->variant_capacity = td->variant_capacity
                                     ? td->variant_capacity * 2 : 4;
                td->variants = (EnumVariantDef*)xrealloc(td->variants,
                    sizeof(EnumVariantDef) * (size_t)td->variant_capacity);
            }
            EnumVariantDef* vd = &td->variants[td->variant_count++];
            memset(vd, 0, sizeof *vd);
            vd->name = v->as.enum_variant.name;
            smap_span_text(smap, vd->name.span,
                           vd->name_buf, sizeof vd->name_buf);

            // Payload type resolution. Each payload expression
            // is an AST_TYPE_EXPR_NODE wrapper; the inner
            // TypeExpr is resolved against the substituted
            // environment (so a payload of type `T` becomes
            // the concrete argument type).
            //
            // The `pc ? pc : 1` in the xcalloc size is the
            // "never call xcalloc(0, ...)" convention: a
            // variant with no payload still gets a
            // 1-element array, which is never read but keeps
            // the pointer non-NULL. That way a future reader
            // can distinguish "no payloads" (count == 0) from
            // "not yet allocated" (which would be NULL) by
            // looking at the pointer, if it wants to.
            int pc = v->as.enum_variant.payload_exprs.count;
            if (pc > MAX_VARIANT_PAYLOADS) {
                diag_error(bag, v->span,
                    "variant '%.*s' has %d payload(s); max is 16",
                    (int)sizeof(vd->name_buf), vd->name_buf, pc);
                pc = MAX_VARIANT_PAYLOADS;
            }
            
            vd->payload_count    = pc;
            vd->payload_capacity = pc;
            vd->payloads = (Type**)xcalloc((size_t)(pc ? pc : 1),
                                           sizeof(Type*));
            for (int p = 0; p < pc; p++) {
                AstNode* pe = v->as.enum_variant.payload_exprs.items[p];
                Type* pt = type_expr_resolve(pe->as.type_expr_node.te,
                                             smap, sc->structs,
                                             &sc->modtable, file_id, sc, bag);
                // On resolution failure, substitute i64 so the
                // rest of the layout code can continue. The
                // error is already reported by type_expr_resolve.
                if (!pt) pt = ty_i64();
                if (pt->kind == TY_REF) {
                    diag_error(bag, pe->span,
                        "enum payloads cannot be of type ref<T>");
                    pt = ty_i64();
                }
                vd->payloads[p] = pt;
            }
            // The AST node's payloads field is updated to
            // point at the TypeDef's array. That is a
            // temporary binding; the AST copy is freed at
            // the end of this function, so the pointer does
            // not outlive this block.
            v->as.enum_variant.payloads = vd->payloads;
            v->as.enum_variant.tag      = td->variant_count - 1;
        }
    }

    // --- Step 7b: compute RC content on the instantiation ---
    //
    // The template's is_builtin flag (true for prelude types)
    // forces RC management regardless of content. Otherwise, an
    // instantiation whose fields hold RC-typed values is itself
    // RC-managed. Compute after fields are resolved so the
    // field types are concrete.
    if (tmpl->is_builtin) {
        td->is_rc_managed = true;
    } else if (sema_tdef_has_rc_content(td)) {
        td->is_rc_managed = true;
    }

    // --- Step 8: free the AST copy ---
    // The TypeDef now holds everything the AST copy carried
    // that matters: field names, types, offsets, variant
    // names, payloads, tags. The AST is discarded.
    ast_free(copy);
    // Return the type, wrapped in ty_rc if the TypeDef is
    // RC-managed. This wrapping is what makes
    // `Result<i64>` and `Option<T>` appear as TY_RC at every
    // use site, which is why so many call sites in this file
    // unwrap one level before inspecting the TY_ENUM inside.
    return (td->kind == TDEF_STRUCT) ? ty_struct(td) : ty_enum(td);
}

// ---------- Function instantiation ----------

// ============================================================
// Function instantiation
//
// The function counterpart of sema_instantiate_generic.
// Called from the AST_CALL case in analyze_expr when a call
// has an explicit type-argument list (`f<i64>(...)`).
//
// Flow:
//   1. Resolve the type arguments.
//   2. Look up the template.
//   3. Arity check.
//   4. Cache lookup: if the function instantiation already
//      exists, return its funcs[] index.
//   5. Deep-copy the template's AST with substitution.
//   6. Register a FuncSig for the copy with the mangled
//      name and is_instantiated = true.
//   7. Resolve the copy's parameter and return types.
//   8. Analyze the copy's body (which is where nested
//      instantiations happen).
//   9. Cache the index and return it.
//
// Unlike the type instantiation, the AST copy is NOT freed:
// it is stored as `sig->decl` and the body is analyzed
// against it. Ownership effectively transfers to the
// FuncSig. There is no explicit free for these copies; the
// current code leaks them at process exit (see the
// follow-ups).
//
// The returned index is the position in sc->funcs[]. Callers
// use it to read the signature (param types, return type) and
// to record func_idx on the AST_CALL node.
// ============================================================
// Instantiate a template by index. This is the primitive; it
// does not look up the template, only instantiates it. Callers
// that have already resolved a name to a template index (the
// resolver's job) call this directly.
int sema_instantiate_function_by_template(SemaCtx* sc,
                                                 int tmpl_idx,
                                                 Token base,
                                                 TypeExpr* te,
                                                 DiagnosticBag* bag) {
    // Step 1: resolve the type arguments.
    Type* arg_types[8];
    int   n = te->arg_count;
    if (n > 8) {
        diag_error(bag, te->span, "too many type arguments (max 8)");
        return -1;
    }
    for (int i = 0; i < n; i++) {
        arg_types[i] = type_expr_resolve(te->args[i], sc->smap, sc->structs,
                                         &sc->modtable, te->span.file_id,
                                         sc, bag);
        if (!arg_types[i]) return -1;
    }

    // Step 2: fetch the template directly (no name search).
    GenericTemplate* tmpl = mono_template_at(sc->structs->generics,
                                             tmpl_idx);
    if (!tmpl) {
        diag_error(bag, te->span,
            "internal: template index %d out of range", tmpl_idx);
        return -1;
    }
    if (tmpl->param_count != n) {
        char buf[64];
        smap_span_text(sc->smap, base.span, buf, sizeof buf);
        diag_error(bag, te->span,
            "generic '%s' expects %d type argument(s), got %d",
            buf, tmpl->param_count, n);
        return -1;
    }

    // --- Step 4: cache lookup ---
    InstEntry* e = cache_lookup(tmpl, arg_types, n);
    if (e && e->func_result >= 0) return e->func_result;

    // --- Step 5: deep-copy with substitution ---
    Subst sub;
    sub.params = tmpl->params;
    sub.args   = arg_types;
    sub.count  = n;

    AstNode* copy = mono_copy_node(tmpl->decl, &sub, sc->smap);

    // Capacity check must happen after the copy so we can
    // free it cleanly on the failure path.
    if (sc->func_count >= MAX_FUNCS) {
        diag_error(bag, te->span, "too many functions (max %d)", MAX_FUNCS);
        ast_free(copy);
        return -1;
    }

    // Mangled name for diagnostics and (indirectly) for the
    // chunk's func table.
    char base_buf[64];
    smap_span_text(sc->smap, base.span, base_buf, sizeof base_buf);
    char mangled[128];
    mono_mangle(mangled, sizeof mangled, base_buf, arg_types, n);

    // --- Step 6: register the FuncSig ---
    int sidx = sc->func_count++;
    FuncSig* sig = &sc->funcs[sidx];
    memset(sig, 0, sizeof *sig);
    sig->name            = base;
    // The file_id is the template's, not the caller's: the
    // instantiated function's "source file" for diagnostics
    // is where the template was declared.
    sig->file_id         = tmpl->file_id;
    sig->is_instantiated = true;
    sig->is_pub          = true;
    sig->is_extern       = false;
    sig->is_variadic     = false;
    sig->ffi_idx         = -1;
    sig->decl            = copy;    // ownership transfers
    snprintf(sig->name_buf, sizeof sig->name_buf, "%s", mangled);

    // --- Step 7: resolve parameter and return types ---
    for (int i = 0; i < copy->as.func.param_count; i++) {
        Param* p = &copy->as.func.params[i];
        // Unannotated parameters default to i64, same rule as
        // user functions in Pass A.
        Type* pt = p->type_expr
            ? type_expr_resolve(p->type_expr, sc->smap, sc->structs,
                                &sc->modtable, te->span.file_id, sc, bag)
            : ty_i64();
        if (pt && pt->kind == TY_EXTERN_STRUCT) {
            diag_error(bag, p->name.span,
                "parameter cannot be an extern struct "
                "value; use *%s", type_name(pt));
            pt = ty_i64();
        }
        
        p->type = pt;
        sig->param_types[i] = pt;
    }
    sig->param_count = copy->as.func.param_count;

    if (copy->as.func.return_type_expr) {
        sig->return_type = type_expr_resolve(
            copy->as.func.return_type_expr, sc->smap, sc->structs,
            &sc->modtable, te->span.file_id, sc, bag);
        sig->return_annotated = true;
    } else {
        sig->return_annotated = false;
        sig->return_type = ty_void();
    }

    // The AST copy's func_idx is filled in so that
    // post-sema passes (codegen) can map back from the AST
    // to this FuncSig.
    copy->as.func.func_idx = sidx;

    // Cache before body analysis: the body might itself
    // instantiate the same function with the same arguments
    // (recursive generic), and we want the cache to be
    // populated so the recursive lookup returns quickly.
    // The body walk below would otherwise recurse infinitely.
    if (!e) e = cache_insert(tmpl, arg_types, n);
    if (e) e->func_result = sidx;

    // --- Step 8: analyze the body ---
    analyze_func_body(sc, sig, copy, bag);

    return sidx;
}

// The old name-based entry point. Resolves the name to a
// template via the resolver and delegates. Used by AST_CALL
// for unqualified generic calls (`f<T>(...)`).
int sema_instantiate_function(SemaCtx* sc, Token base,
                                     TypeExpr* te, DiagnosticBag* bag) {
    ResolvedDecl rd = resolve_template(sc, te->span.file_id, base);
    if (rd.kind != RD_TEMPLATE) {
        char buf[64];
        smap_span_text(sc->smap, base.span, buf, sizeof buf);
        diag_error(bag, te->span, "unknown generic function '%s'", buf);
        return -1;
    }
    return sema_instantiate_function_by_template(
        sc, rd.index, base, te, bag);
}



// Instantiate the prelude's `Option<T>` for a concrete
// payload type. Used by builtins whose return type is
// Option<something> (currently only `read_line`), so that
// users get a real `Option<string>` rather than a bespoke
// nullable-string type.
//
// Returns NULL if the prelude's Option template is not
// registered (which would mean the prelude did not load),
// or if instantiation fails. Both cases are already
// reported into the bag.
Type* instantiate_option_of(SemaCtx* sc, Type* payload,
                                   Span span, DiagnosticBag* bag) {
    if (!sc->structs || !sc->structs->instantiate_generic) return NULL;

    // Iterate the registered templates, looking for one
    // named "Option". Templates are stored in the mono
    // generics list; the helpers below expose them.
    int n = mono_template_count(sc->structs->generics);
    for (int i = 0; i < n; i++) {
        GenericTemplate* tmpl = mono_template_at(sc->structs->generics, i);
        if (!tmpl) continue;
        if (!smap_span_equals(sc->smap, tmpl->name.span, "Option")) continue;
        if (tmpl->param_count != 1) return NULL;

        // Construct `Option<payload>` as a TEX_GENERIC with
        // one resolved argument. The base token is the
        // template's own name token, so name matching inside
        // sema_instantiate_generic succeeds.
        TypeExpr* arg = type_expr_resolved(payload, span);
        TypeExpr* g   = type_expr_generic(tmpl->name, span);
        type_expr_generic_add_arg(g, arg);
        Type* result = sc->structs->instantiate_generic(
            sc->structs->generics, g, sc->smap, sc, span.file_id, bag);
        type_expr_free(g);   // frees `arg` as well
        return result;
    }
    return NULL;
}
