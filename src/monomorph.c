// monomorph_c
// ============================================================
// monomorph.c — Generic template registry and AST deep-copy
// with type substitution.
//
// This file is the runtime half of Prism's monomorphization
// strategy. It provides:
//
//   1. A growable list of templates, hidden behind a void*
//      handle (TemplateList) so the rest of the compiler does
//      not need to know its shape.
//
//   2. Deep-copy functions that walk an AST subtree (or a
//      TypeExpr) and produce a fresh copy in which every
//      reference to a type-parameter name has been replaced by
//      a resolved Type*. This is what turns a generic
//      declaration into a concrete one.
//
//   3. A name-mangling helper that turns (base name, argument
//      types) into a unique string, used as the key for
//      looking up an already-instantiated type or function.
//
// Reading notes:
//   - The copy functions are the largest part of the file.
//     They are a single switch over AstKind with one case per
//     node kind, plus a recursive switch over TypeExprKind in
//     mono_copy_type_expr. If you add a new AST node kind, you
//     must add a case here (or accept that the new kind will
//     be shallow-copied, which is almost certainly a bug).
//   - The template list stores borrowed pointers; see the
//     header for the ownership contract.
//   - Nothing here reports diagnostics. A caller that wants
//     to complain about a bad template does so before calling
//     into this file.
// ============================================================
#include "../include/monomorph.h"
#include "../include/ast.h"
#include "../include/source_map.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ============================================================
// Template storage
//
// The public API uses `void*` for the generics handle so the
// concrete list type stays private. Internally, the handle is
// always a `TemplateList*`; every public function that takes
// `void* generics` casts it back on entry. The cast is a
// deliberate encapsulation trick, not a sign of polymorphism.
// ============================================================
typedef struct {
    GenericTemplate* items;
    int              count;
    int              capacity;
} TemplateList;

// Create an empty template list and store it through
// `*generics_out`. xcalloc means the initial list is empty
// (count == 0, items == NULL) and will allocate on the first
// registration.
//
// `generics_out` must be a valid pointer; the function does
// not check for NULL, and passing NULL is a caller bug.
void mono_init(void** generics_out) {
    TemplateList* tl = (TemplateList*)xcalloc(1, sizeof *tl);
    *generics_out = tl;
}

// Free the template list. The AstNodes inside are borrowed
// (see the header) and are NOT freed here; only the array of
// GenericTemplate records and the list struct itself are
// released.
//
// NULL is accepted as a no-op so that a cleanup path that
// was never initialized does not need a separate guard.
void mono_free(void* generics) {
    if (!generics) return;
    TemplateList* tl = (TemplateList*)generics;
    free(tl->items);
    free(tl);
}

// Register a generic declaration as a template.
//
// Precondition: `decl->kind` is AST_FUNC, AST_STRUCT_DEF, or
// AST_ENUM_DEF, and its type_param_count > 0. The kind switch
// below silently ignores anything else, leaving a
// zero-initialized record in the list (name and params all
// zero-valued). The current call site in sema only registers
// declarations that satisfy the precondition; a future caller
// that does not would produce a template with `param_count ==
// 0` that never matches any substitution.
//
// `smap` is cast to void: this function does not currently
// use it. The parameter is kept for API symmetry and future
// use (e.g. copying name text).
//
// Growth: doubling from 8, matching the pattern used
// throughout the compiler (diagnostics, AST lists, type
// tables, etc.).
//
// The record's fields are populated by extracting from the
// declaration's union member that corresponds to its kind.
// All three kinds use the same layout for the type-parameter
// data (`type_params[]` and `type_param_count`), just under
// different union members; the switch is the only place that
// distinction appears.
void mono_register_template(void* generics, AstNode* decl,
                            const SourceMap* smap) {
    (void)smap;
    TemplateList* tl = (TemplateList*)generics;
    if (tl->count >= tl->capacity) {
        tl->capacity = tl->capacity ? tl->capacity * 2 : 8;
        tl->items = (GenericTemplate*)xrealloc(tl->items,
            sizeof(GenericTemplate) * (size_t)tl->capacity);
    }
    GenericTemplate* t = &tl->items[tl->count++];
    // Zero the record first so any field the switch does not
    // set (e.g. is_builtin for a caller that left it false)
    // is a definite value rather than garbage. Then fill in
    // the common fields.
    memset(t, 0, sizeof *t);
    t->decl = decl;
    t->file_id = decl->span.file_id;
    t->is_builtin = decl->is_builtin;

    switch (decl->kind) {
        case AST_FUNC:
            // Function template: name is the function's name,
            // and the type-parameter list is `type_params[]`
            // under the `func` union member.
            t->name = decl->as.func.name;
            t->param_count = decl->as.func.type_param_count;
            for (int i = 0; i < t->param_count; i++)
                t->params[i] = decl->as.func.type_params[i];
            break;
        case AST_STRUCT_DEF:
            // Struct template: same shape, different member.
            t->name = decl->as.struct_def.name;
            t->param_count = decl->as.struct_def.type_param_count;
            for (int i = 0; i < t->param_count; i++)
                t->params[i] = decl->as.struct_def.type_params[i];
            break;
        case AST_ENUM_DEF:
            // Enum template: same shape, different member.
            t->name = decl->as.enum_def.name;
            t->param_count = decl->as.enum_def.type_param_count;
            for (int i = 0; i < t->param_count; i++)
                t->params[i] = decl->as.enum_def.type_params[i];
            break;
        default: break;
        // Anything else: `name` and `params` remain zero-valued
        // (the memset above). See the precondition note.
    }
}

// Look up a template by name.
//
// Linear scan over the list, comparing the stored name token
// against `base` using smap_tokens_equal (byte-for-byte
// comparison of the two tokens' source text). Returns the
// first match, or NULL if none.
//
// There is no file filter: two templates with the same name
// in different files would both match, and the first one
// registered wins. Today the language forbids duplicate
// template names across files, so this is not a live bug;
// flagging for whoever adds per-file scoping.
GenericTemplate* mono_find_template(void* generics, const SourceMap* smap,
                                    Token base) {
    TemplateList* tl = (TemplateList*)generics;
    for (int i = 0; i < tl->count; i++)
        if (smap_tokens_equal(smap, tl->items[i].name, base)) return &tl->items[i];
    return NULL;
}


GenericTemplate* mono_find_template_in_file(void* generics,
                                            const SourceMap* smap,
                                            Token base,
                                            uint32_t file_id) {
    TemplateList* tl = (TemplateList*)generics;
    for (int i = 0; i < tl->count; i++) {
        if (tl->items[i].file_id != file_id) continue;
        if (smap_tokens_equal(smap, tl->items[i].name, base))
            return &tl->items[i];
    }
    return NULL;
}

// ============================================================
// Type expression deep copy with substitution
//
// The one and only place where the substitution is actually
// performed: a TEX_NAME whose identifier matches a parameter
// in `sub` is replaced by a TEX_RESOLVED wrapping the concrete
// Type* from the matching `sub->args` entry. Everything else
// is a structural deep copy.
//
// The function is total for every TypeExprKind that the
// parser produces (all eight kinds in the enum). A NULL input
// returns NULL so that callers can pass optional sub-fields
// (e.g. `return_type_expr` on a function that had no
// annotation) without a separate guard.
// ============================================================
TypeExpr* mono_copy_type_expr(TypeExpr* te, const Subst* sub,
                              const SourceMap* smap) {
    if (!te) return NULL;

    // TEX_NAME is the only kind that can be substituted. It is
    // handled outside the switch so the substitution logic is
    // easy to find and so the switch below can treat
    // TEX_NAME as the "already done" case without a `break`.
    //
    // The loop is a linear scan over the (at most 8) subst
    // entries. For each entry, if the parameter name matches,
    // return a TEX_RESOLVED wrapping the corresponding Type*.
    // This is the *only* substitution point in the entire
    // monomorphization pass.
    //
    // If no parameter matches, the identifier is not a type
    // variable; it is copied as-is, so a concrete type name
    // written in the template body (e.g. `i64`) survives
    // instantiation unchanged.
    if (te->kind == TEX_NAME) {
        for (int i = 0; i < sub->count; i++) {
            if (smap_tokens_equal(smap, te->name, sub->params[i]))
                return type_expr_resolved(sub->args[i], te->span);
        }
        return type_expr_name(te->name);
    }

    switch (te->kind) {
        // Already-resolved TypeExpr: copy the resolved Type*
        // pointer (it is process-lifetime) and the span.
        case TEX_RESOLVED:
            return type_expr_resolved(te->resolved, te->span);

        // The four unary wrappers. Each recurses into the
        // inner TypeExpr; the ref case additionally carries
        // the is_mut flag.
        case TEX_REF:
            return type_expr_ref(mono_copy_type_expr(te->inner, sub, smap),
                                 te->is_mut, te->span);
        case TEX_RC:
            return type_expr_rc(mono_copy_type_expr(te->inner, sub, smap),
                                te->span);
        case TEX_RAWPTR:
            return type_expr_rawptr(mono_copy_type_expr(te->inner, sub, smap),
                                    te->span);
        case TEX_ARRAY:
            return type_expr_array(mono_copy_type_expr(te->inner, sub, smap),
                                   te->span);

        case TEX_FN: {
            TypeExpr* fn = type_expr_fn(te->span);
            for (int i = 0; i < te->param_count; i++)
                type_expr_fn_add_param(fn,
                    mono_copy_type_expr(te->params[i], sub, smap));
            type_expr_fn_set_return(fn,
                mono_copy_type_expr(te->ret, sub, smap));
            return fn;
        }
            

        // Generic type application: `Base<A, B, ...>`. The
        // base name is copied as-is; each argument is
        // recursively copied (which is where substitution
        // happens for a type variable used as a type
        // argument, e.g. `Vec<T>` inside a template).
        case TEX_GENERIC: {
            TypeExpr* g = type_expr_generic(te->name, te->span);
            for (int i = 0; i < te->arg_count; i++)
                type_expr_generic_add_arg(g,
                    mono_copy_type_expr(te->args[i], sub, smap));
            return g;
        }

        // Qualified path: `a.b.c`. The path is a flat array of
        // Tokens with no substitution points (only the
        // *final* name of a qualified path could ever be a
        // type parameter, and by the language rules that is
        // not legal). So the copy is a flat Token copy.
        //
        // The local buffer caps at 8 components, matching the
        // parser's hard cap for type paths. If a future
        // version ever produces a longer path, this silently
        // truncates; noted for the follow-ups.
        case TEX_QUALIFIED: {
            // Same cap as the parser: if a path could be
            // produced with PRISM_MAX_PATH_COMPONENTS
            // components, the copy must not silently
            // truncate it. See PRISM_MAX_PATH_COMPONENTS
            // in common.h.
            Token path[PRISM_MAX_PATH_COMPONENTS];
            int   n = te->path_count < PRISM_MAX_PATH_COMPONENTS ? te->path_count : PRISM_MAX_PATH_COMPONENTS;
            for (int i = 0; i < n; i++) path[i] = te->path[i];
            return type_expr_qualified(path, n, te->span);
        }

        // TEX_NAME was handled above. This case exists only so
        // the compiler does not warn about a missing enum
        // case; it is unreachable.
        case TEX_NAME:
            break;   // handled above
    }
    return NULL;
}

// ============================================================
// Node deep copy with substitution
//
// Every AST node that can carry a TypeExpr or a subtree is
// handled. Nodes that cannot are copied verbatim.
//
// Shape of a case:
//   1. Copy the scalar fields that the copy needs to preserve
//      (names, tokens, is_mut flags, op codes, etc.).
//   2. For each AstNode* child, recurse via mono_copy_node.
//   3. For each TypeExpr* field, recurse via
//      mono_copy_type_expr (this is where substitution
//      happens).
//   4. For each AstList, ast_list_init the copy and push the
//      recursively-copied elements.
//   5. Reset sema-owned fields (resolved Type*, table indices,
//      slot numbers, offsets, tags) to their unset sentinels
//      (-1 / NULL / 0). Sema will re-run on the copy.
//
// The default case at the bottom covers AST_PROGRAM (which
// should never be passed here) and any leaf kind that has no
// payload to copy. Falling through to default is NOT the same
// as doing nothing: alloc_node has already run, so a
// non-handled kind produces a fresh node of the same kind
// with only the fields alloc_node sets. That is a bug if the
// kind has children (they would leak in the original and be
// NULL in the copy); noting in the follow-ups.
// ============================================================
AstNode* mono_copy_node(AstNode* n, const Subst* sub, const SourceMap* smap) {
    if (!n) return NULL;
    // Allocate the copy with the same kind and span. alloc_node
    // also sets is_pub=false, local_index=-1, const_id=-1,
    // tier=TIER_NONE; the switch below may overwrite some of
    // those.
    AstNode* c = ast_alloc_node(n->kind, n->span);
    // `is_pub` is a property of the declaration's visibility,
    // not of its analysis state; preserve it across the copy
    // so an instantiation of a `pub fn` is still `pub`.
    c->is_pub = n->is_pub;

    switch (n->kind) {
        // ---- Top-level declarations ----
        case AST_FUNC:
            // Copy the function name, owner (for methods), and
            // param list. Note that `type_param_count` is set
            // to 0 on the copy: an instantiated function is a
            // concrete, non-generic function, so it must not
            // itself carry type parameters.
            //
            // Each parameter's type_expr is recursively copied
            // with substitution. The parameter's resolved
            // `type` is reset to NULL; sema will re-resolve it
            // in the instantiation's context (where the
            // substituted types are now concrete).
            c->as.func.name = n->as.func.name;
            c->as.func.method_owner = n->as.func.method_owner;
            c->as.func.param_count = n->as.func.param_count;
            c->as.func.type_param_count = 0;   // instantiation has none
            for (int i = 0; i < n->as.func.param_count; i++) {
                c->as.func.params[i].name      = n->as.func.params[i].name;
                c->as.func.params[i].type_expr =
                    mono_copy_type_expr(n->as.func.params[i].type_expr,
                                        sub, smap);
                c->as.func.params[i].type      = NULL;
                c->as.func.params[i].is_mut    = n->as.func.params[i].is_mut;
            }
            // Return type: same treatment (copy the TypeExpr,
            // drop the resolved Type*).
            c->as.func.return_type_expr =
                mono_copy_type_expr(n->as.func.return_type_expr, sub, smap);
            c->as.func.return_type = NULL;
            // Body: recursive copy. An extern function has a
            // NULL body, which mono_copy_node handles.
            c->as.func.body = mono_copy_node(n->as.func.body, sub, smap);
            // Fresh function; sema will assign a new index.
            c->as.func.func_idx = -1;
            c->as.func.ffi_idx  = -1;
            // NOTE: is_extern, is_variadic, lib_alias, and
            // ffi_idx are NOT set here. They fall through from
            // alloc_node's zero-init for is_extern (false) and
            // is_variadic (false), and a zero Token for
            // lib_alias. ffi_idx is left at 0 rather than -1
            // by this path; if an extern function is ever
            // instantiated (it should not be, since externs
            // cannot be generic), sema must reset it. Noting.
            break;

        case AST_STRUCT_DEF:
            // Struct template instantiation. Type parameter
            // count drops to 0; fields are recursively copied
            // (each field carries a TypeExpr that may mention
            // the template's parameters, which is where
            // substitution occurs).
            c->as.struct_def.name = n->as.struct_def.name;
            c->as.struct_def.type_param_count = 0;
            c->as.struct_def.is_extern = n->as.struct_def.is_extern;
            c->as.struct_def.is_opaque = n->as.struct_def.is_opaque;
            ast_list_init(&c->as.struct_def.fields);
            for (int i = 0; i < n->as.struct_def.fields.count; i++)
                ast_list_push(&c->as.struct_def.fields,
                    mono_copy_node(n->as.struct_def.fields.items[i],
                                   sub, smap));
            c->as.struct_def.sdef_index = -1;
            break;

        case AST_ENUM_DEF:
            // Same shape as struct_def. Variants are copied
            // recursively; each variant's payload_exprs may
            // mention template parameters, and the substitution
            // happens inside mono_copy_type_expr for the
            // AST_TYPE_EXPR_NODE children.
            c->as.enum_def.name = n->as.enum_def.name;
            c->as.enum_def.type_param_count = 0;
            ast_list_init(&c->as.enum_def.variants);
            for (int i = 0; i < n->as.enum_def.variants.count; i++)
                ast_list_push(&c->as.enum_def.variants,
                    mono_copy_node(n->as.enum_def.variants.items[i],
                                   sub, smap));
            c->as.enum_def.tdef_index = -1;
            break;

        case AST_STRUCT_FIELD:
            // One field. The TypeExpr is copied (with
            // substitution); the resolved Type* and computed
            // offset are reset.
            c->as.struct_field.name = n->as.struct_field.name;
            c->as.struct_field.type_expr =
                mono_copy_type_expr(n->as.struct_field.type_expr, sub, smap);
            c->as.struct_field.type = NULL;
            c->as.struct_field.offset = 0;
            break;

        case AST_ENUM_VARIANT:
            // One enum variant. Note that `tag` IS preserved
            // (variants have a stable order, so the tag is the
            // same on the copy as on the template). The
            // resolved `payloads` array is reset to NULL /
            // count 0; sema will re-resolve and re-allocate
            // it. `payload_exprs` is the syntactic list, deep-
            // copied as usual.
            c->as.enum_variant.name = n->as.enum_variant.name;
            c->as.enum_variant.tag  = n->as.enum_variant.tag;
            c->as.enum_variant.payloads = NULL;
            c->as.enum_variant.payload_count = 0;
            ast_list_init(&c->as.enum_variant.payload_exprs);
            for (int i = 0; i < n->as.enum_variant.payload_exprs.count; i++)
                ast_list_push(&c->as.enum_variant.payload_exprs,
                    mono_copy_node(
                        n->as.enum_variant.payload_exprs.items[i],
                        sub, smap));
            break;

        case AST_TYPE_EXPR_NODE:
            // Wrapper that lets a TypeExpr sit in an AstList.
            // The TypeExpr is copied with substitution.
            c->as.type_expr_node.te =
                mono_copy_type_expr(n->as.type_expr_node.te, sub, smap);
            break;

        // ---- Statements ----
        case AST_BLOCK:
            // Compound statement. Each stmt is recursively
            // copied.
            ast_list_init(&c->as.block.stmts);
            for (int i = 0; i < n->as.block.stmts.count; i++)
                ast_list_push(&c->as.block.stmts,
                    mono_copy_node(n->as.block.stmts.items[i], sub, smap));
            break;

        case AST_LET:
            // `let name [: T] = init`. The name is preserved
            // verbatim (bindings are not substituted; only
            // type names are), the TypeExpr is copied, and
            // the init is recursively copied. The resolved
            // Type* on the let is reset; sema re-infers it.
            c->as.let.name = n->as.let.name;
            c->as.let.type_expr =
                mono_copy_type_expr(n->as.let.type_expr, sub, smap);
            c->as.let.type = NULL;
            c->as.let.init = mono_copy_node(n->as.let.init, sub, smap);
            c->as.let.is_mut = n->as.let.is_mut;
            break;

        case AST_ASSIGN:
            c->as.assign.target =
                mono_copy_node(n->as.assign.target, sub, smap);
            c->as.assign.value  =
                mono_copy_node(n->as.assign.value, sub, smap);
            break;

        case AST_ASSIGN_OP:
            c->as.assign_op.op        = n->as.assign_op.op;
            c->as.assign_op.target    =
                mono_copy_node(n->as.assign_op.target, sub, smap);
            c->as.assign_op.value     =
                mono_copy_node(n->as.assign_op.value, sub, smap);
            c->as.assign_op.temp_slot = -1;
            break;

        case AST_IF:
            c->as.if_.cond   = mono_copy_node(n->as.if_.cond, sub, smap);
            c->as.if_.then_  = mono_copy_node(n->as.if_.then_, sub, smap);
            c->as.if_.else_  = mono_copy_node(n->as.if_.else_, sub, smap);
            break;

        case AST_WHILE:
            c->as.while_.cond = mono_copy_node(n->as.while_.cond, sub, smap);
            c->as.while_.body = mono_copy_node(n->as.while_.body, sub, smap);
            break;

        case AST_FOR_RANGE:
            // Note: var_slot and end_slot are NOT copied. They
            // fall through as alloc_node's defaults (0, not
            // -1!). That is a latent bug: sema uses -1 to mean
            // "not yet assigned" and the copy presents 0 as a
            // valid-looking slot. See the follow-ups. Same
            // applies to the AST_FOR_IN case below.
            c->as.for_range.var   = n->as.for_range.var;
            c->as.for_range.start = mono_copy_node(n->as.for_range.start, sub, smap);
            c->as.for_range.end   = mono_copy_node(n->as.for_range.end, sub, smap);
            c->as.for_range.body  = mono_copy_node(n->as.for_range.body, sub, smap);
            c->as.for_range.var_slot = -1;
            c->as.for_range.end_slot = -1;
            break;

        case AST_FOR_IN:
            c->as.for_in.var      = n->as.for_in.var;
            c->as.for_in.iterable = mono_copy_node(n->as.for_in.iterable, sub, smap);
            c->as.for_in.body     = mono_copy_node(n->as.for_in.body, sub, smap);
            c->as.for_in.var_slot = -1;
            c->as.for_in.arr_slot = -1;
            c->as.for_in.len_slot = -1;
            c->as.for_in.idx_slot = -1;
            break;

        case AST_RETURN:
            c->as.ret.value = mono_copy_node(n->as.ret.value, sub, smap);
            break;

        case AST_EXPR_STMT:
            c->as.expr_stmt.expr =
                mono_copy_node(n->as.expr_stmt.expr, sub, smap);
            break;

        // ---- Expressions ----
        case AST_BINARY:
            c->as.binary.op    = n->as.binary.op;
            c->as.binary.left  = mono_copy_node(n->as.binary.left, sub, smap);
            c->as.binary.right = mono_copy_node(n->as.binary.right, sub, smap);
            break;

        case AST_UNARY:
            c->as.unary.op      = n->as.unary.op;
            c->as.unary.operand =
                mono_copy_node(n->as.unary.operand, sub, smap);
            break;

        case AST_CALL:
            // A call node. The name is preserved, the args
            // are recursively copied, and all resolution
            // fields (builtin_id, func_idx, ffi_idx) are
            // reset to -1. Note that `instantiation` is set
            // to NULL even if the original had one: an
            // instantiated generic function called inside
            // another template's body would have its
            // instantiation re-resolved by sema against the
            // substituted types. Preserving the original
            // TypeExpr would be wrong.
            //
            // (The ffi_idx field is set to 0 by alloc_node,
            // not -1. This is a latent bug of the same kind
            // as the for-slot issue. Noting.)
            c->as.call.name = n->as.call.name;
            c->as.call.instantiation = NULL;
            c->as.call.builtin_id    = -1;
            c->as.call.func_idx      = -1;
            c->as.call.ffi_idx       = -1;
            ast_list_init(&c->as.call.args);
            for (int i = 0; i < n->as.call.args.count; i++)
                ast_list_push(&c->as.call.args,
                    mono_copy_node(n->as.call.args.items[i], sub, smap));
                    
            break;

        case AST_VARIANT_CTOR:
            // `Enum.Variant(...)` or `receiver.method(...)`.
            // The receiver_expr is conditionally copied (NULL
            // for a non-method variant constructor). The
            // resolved fields (tdef_index, tag, mod_func_idx,
            // mod_ffi_idx) are reset to -1 so sema re-resolves
            // them in the new context.
            c->as.variant_ctor.type_name    = n->as.variant_ctor.type_name;
            c->as.variant_ctor.variant_name = n->as.variant_ctor.variant_name;
            c->as.variant_ctor.receiver_expr =
                n->as.variant_ctor.receiver_expr
                    ? mono_copy_node(n->as.variant_ctor.receiver_expr, sub, smap)
                    : NULL;
            c->as.variant_ctor.instantiation = NULL;
            c->as.variant_ctor.tdef_index   = -1;
            c->as.variant_ctor.tag          = -1;
            c->as.variant_ctor.mod_func_idx = -1;
            c->as.variant_ctor.mod_ffi_idx  = -1;
            ast_list_init(&c->as.variant_ctor.args);
            for (int i = 0; i < n->as.variant_ctor.args.count; i++)
                ast_list_push(&c->as.variant_ctor.args,
                    mono_copy_node(n->as.variant_ctor.args.items[i],
                                   sub, smap));
            break;

        case AST_STRUCT_LIT:
            // `Type { field: v, ... }`. The instantiation
            // TypeExpr (if any) is copied with substitution;
            // this is how a struct literal whose type was
            // `Point<T>` in the template becomes
            // `Point<i64>` after instantiation.
            c->as.struct_lit.type_name  = n->as.struct_lit.type_name;
            c->as.struct_lit.sdef_index = -1;
            c->as.struct_lit.hidden_slot = -1;

            c->as.struct_lit.instantiation = n->as.struct_lit.instantiation
                ? mono_copy_type_expr(n->as.struct_lit.instantiation, sub, smap)
                : NULL;
            
            ast_list_init(&c->as.struct_lit.inits);
            for (int i = 0; i < n->as.struct_lit.inits.count; i++)
                ast_list_push(&c->as.struct_lit.inits,
                    mono_copy_node(n->as.struct_lit.inits.items[i],
                                   sub, smap));
            break;

        case AST_STRUCT_INIT:
            // One `field: v` inside a struct literal. The
            // value is copied; offset is reset to 0 (a
            // resolved field offset would be wrong in the
            // instantiation's layout). Same latent issue as
            // for_range: alloc_node zeroed offset and the
            // canonical unset value is -1.
            c->as.struct_init.name = n->as.struct_init.name;
            c->as.struct_init.offset = -1;
            c->as.struct_init.field_type = NULL;
            c->as.struct_init.value =
                mono_copy_node(n->as.struct_init.value, sub, smap);
            break;

        case AST_FIELD:
            // `expr.name`. The target is recursively copied;
            // the resolved owner Type* and computed offset are
            // reset. unit_tag is preserved: the variant-tag
            // value is intrinsic to the field/variant name,
            // not to the instantiation's types.
            c->as.field.target   = mono_copy_node(n->as.field.target, sub, smap);
            c->as.field.field    = n->as.field.field;
            c->as.field.owner    = NULL;
            c->as.field.offset   = 0;
            c->as.field.unit_tag = n->as.field.unit_tag;
            break;

        case AST_ARRAY_LIT:
            ast_list_init(&c->as.array_lit.elements);
            for (int i = 0; i < n->as.array_lit.elements.count; i++)
                ast_list_push(&c->as.array_lit.elements,
                    mono_copy_node(n->as.array_lit.elements.items[i],
                                   sub, smap));
            break;

        case AST_INDEX:
            c->as.index.target = mono_copy_node(n->as.index.target, sub, smap);
            c->as.index.index  = mono_copy_node(n->as.index.index, sub, smap);
            break;

        case AST_DEREF:
            // offset is preserved: it is a syntactic
            // adjustment (field offset baked into the deref),
            // not a resolved value.
            c->as.deref.operand = mono_copy_node(n->as.deref.operand, sub, smap);
            c->as.deref.offset  = n->as.deref.offset;
            break;

        case AST_ALLOC_ARENA:
            c->as.alloc_arena.value  = mono_copy_node(n->as.alloc_arena.value, sub, smap);
            c->as.alloc_arena.is_mut = n->as.alloc_arena.is_mut;
            break;
        case AST_ADDR_OF_FIELD:
            // Defensive: only produced by sema, so never seen by
            // the monomorphizer in practice. Copy the child.
            c->as.addr_of_field.field =
                mono_copy_node(n->as.addr_of_field.field, sub, smap);
            break;
            

        case AST_ALLOC_RC:
            c->as.alloc_rc.value = mono_copy_node(n->as.alloc_rc.value, sub, smap);
            break;

        case AST_CAST:
            // `expr as Type`. The operand and the target
            // TypeExpr are both copied; the resolved
            // target_type is dropped.
            c->as.cast.operand = mono_copy_node(n->as.cast.operand, sub, smap);
            c->as.cast.target_type_expr =
                mono_copy_type_expr(n->as.cast.target_type_expr, sub, smap);
            c->as.cast.target_type = NULL;
            break;

        case AST_SIZEOF:
            c->as.sizeof_expr.te =
                mono_copy_type_expr(n->as.sizeof_expr.te, sub, smap);
            c->as.sizeof_expr.resolved = NULL;
            c->as.sizeof_expr.size     = 0;
            break;
            

        case AST_TRY:
            c->as.try_.operand =
                mono_copy_node(n->as.try_.operand, sub, smap);
            break;
            
        // ---- Match and patterns ----
        case AST_MATCH:
            // The hidden_slot is reset to -1 (sema assigns a
            // fresh one on the copy). The arms list is
            // deep-copied.
            c->as.match.scrutinee = mono_copy_node(n->as.match.scrutinee, sub, smap);
            c->as.match.hidden_slot = -1;
            ast_list_init(&c->as.match.arms);
            for (int i = 0; i < n->as.match.arms.count; i++)
                ast_list_push(&c->as.match.arms,
                    mono_copy_node(n->as.match.arms.items[i], sub, smap));
            break;

        case AST_MATCH_ARM:
            c->as.match_arm.pattern =
                mono_copy_node(n->as.match_arm.pattern, sub, smap);
            c->as.match_arm.body =
                mono_copy_node(n->as.match_arm.body, sub, smap);
            break;

        case AST_PAT_VARIANT:
            // Variant pattern. The instantiation TypeExpr (if
            // any) is copied with substitution. The tag is
            // reset (sema recomputes it), sub_count is reset
            // to 0 (sema recomputes it from subpatterns), and
            // tmp_slots is left as alloc_node zeroed it
            // (sema will fill the first sub_count entries).
            c->as.pat_variant.type_name    = n->as.pat_variant.type_name;
            c->as.pat_variant.variant_name = n->as.pat_variant.variant_name;
            c->as.pat_variant.tag          = -1;
            c->as.pat_variant.sub_count    = 0;

            c->as.pat_variant.instantiation = n->as.pat_variant.instantiation
                ? mono_copy_type_expr(n->as.pat_variant.instantiation, sub, smap)
                : NULL;
            
            ast_list_init(&c->as.pat_variant.subpatterns);
            for (int i = 0; i < n->as.pat_variant.subpatterns.count; i++)
                ast_list_push(&c->as.pat_variant.subpatterns,
                    mono_copy_node(n->as.pat_variant.subpatterns.items[i],
                                   sub, smap));
            break;

        case AST_PAT_BIND:
            // Binding pattern. The slot is reset to -1;
            // sema assigns a new one on the copy.
            c->as.pat_bind.name = n->as.pat_bind.name;
            c->as.pat_bind.slot = -1;
            break;

        // ---- Leaf nodes ----
        // These have no child pointers and no resolution
        // state that would be wrong on a copy. They are
        // copied field-for-field.
        case AST_PAT_INT:    c->as.pat_int.value    = n->as.pat_int.value;    break;
        case AST_PAT_BOOL:   c->as.pat_bool.value   = n->as.pat_bool.value;   break;
        case AST_PAT_STRING: c->as.pat_string.content = n->as.pat_string.content; break;
        
        case AST_PAT_WILDCARD: break;

        case AST_INT:    c->as.int_lit.value    = n->as.int_lit.value;    break;
        case AST_FLOAT:  c->as.float_lit.value  = n->as.float_lit.value;  break;
        case AST_BOOL:   c->as.bool_lit.value   = n->as.bool_lit.value;   break;
        case AST_STRING: c->as.string_lit.content = n->as.string_lit.content; break;
        // AST_VAR: the name is preserved. local_index and
        // const_id are left at their alloc_node defaults (-1),
        // so sema will re-resolve the variable in the copy's
        // environment. This is correct for a template copy: a
        // variable reference inside a generic function body
        // must be re-resolved against the instantiation's
        // local frame.
        case AST_VAR:    
            c->as.var.name = n->as.var.name; 
            c->as.var.func_idx = -1;
            break;
        case AST_BREAK:
        case AST_CONTINUE: break;

        case AST_CALL_INDIRECT:
            // Only produced by sema on an analyzed tree.
            // Templates never contain one, but copying is
            // harmless and keeps the switch exhaustive.
            c->as.call_indirect.callee =
                mono_copy_node(n->as.call_indirect.callee, sub, smap);
            ast_list_init(&c->as.call_indirect.args);
            for (int i = 0; i < n->as.call_indirect.args.count; i++)
                ast_list_push(&c->as.call_indirect.args,
                    mono_copy_node(n->as.call_indirect.args.items[i],
                                   sub, smap));
            break;
        

        // AST_CONST: name (Token) + initializer (AstNode*).
        // Shallow assignment shares the initializer subtree
        // with the original; deep-copy it.
        case AST_CONST:
            c->as.const_decl.name = n->as.const_decl.name;
            c->as.const_decl.type_expr =
                mono_copy_type_expr(n->as.const_decl.type_expr, sub, smap);
            c->as.const_decl.initializer =
                mono_copy_node(n->as.const_decl.initializer, sub, smap);
            break;

        // AST_USING: has three pointer fields (path, names,
        // alias_target) plus a group of scalars. Copy the
        // scalars verbatim and duplicate each pointer field.
        case AST_USING: {
            AstUsing* src = &n->as.using_decl;
            AstUsing* dst = &c->as.using_decl;
            *dst = *src;   // scalar fields, plus the pointer
                           // values (overwritten below)

            if (src->path && src->path_count > 0) {
                dst->path = (Token*)xmalloc(
                    sizeof(Token) * (size_t)src->path_count);
                memcpy(dst->path, src->path,
                       sizeof(Token) * (size_t)src->path_count);
            } else {
                dst->path = NULL;
                dst->path_count = 0;
            }

            if (src->names && src->name_count > 0) {
                dst->names = (UsingName*)xmalloc(
                    sizeof(UsingName) * (size_t)src->name_count);
                memcpy(dst->names, src->names,
                       sizeof(UsingName) * (size_t)src->name_count);
                dst->name_capacity = src->name_count;
            } else {
                dst->names = NULL;
                dst->name_count = 0;
                dst->name_capacity = 0;
            }

            dst->alias_target =
                mono_copy_type_expr(src->alias_target, sub, smap);
            break;
        }

        // AST_EXTERN_LIB: reuses the using_decl payload slot
        // for its path and alias. Only `path` is allocated.
        case AST_EXTERN_LIB: {
            AstUsing* src = &n->as.using_decl;
            AstUsing* dst = &c->as.using_decl;
            *dst = *src;

            if (src->path && src->path_count > 0) {
                dst->path = (Token*)xmalloc(
                    sizeof(Token) * (size_t)src->path_count);
                memcpy(dst->path, src->path,
                       sizeof(Token) * (size_t)src->path_count);
            } else {
                dst->path = NULL;
                dst->path_count = 0;
            }
            // AST_EXTERN_LIB does not set names or alias_target.
            break;
        }

        // AST_PROGRAM should never be copied by the
        // monomorphizer: templates are function, struct, or
        // enum declarations. This case is here so the
        // switch is exhaustive over the non-default kinds;
        // if it ever fires, the caller is using mono_copy_node
        // on the wrong input.
        case AST_PROGRAM:
            fprintf(stderr,
                "monomorph: internal error: mono_copy_node called "
                "on AST_PROGRAM\n");
            break;

        default:
            // Leaf kinds with no owned heap. Nothing to do;
            // alloc_node has already produced a valid copy.
            break;
    }
    return c;
}

// ============================================================
// Mangled names
//
// Produce a string like "Option#i64#string" that uniquely
// identifies an instantiation. The result is used as a lookup
// key when checking whether an instantiation already exists;
// two different source-level type applications that mangle to
// the same string are considered the same instantiation.
//
// Format:
//   <base>
//   for each argument i:
//     '#' <type_name(args[i])>
//
// The '#' separator was chosen because it cannot appear in an
// identifier, so the mangled form never collides with a
// user-written name. That property is what makes mangled
// names safe to store alongside real names in the type table.
// ============================================================
void mono_mangle(char* out, size_t cap, const char* base,
                 Type** args, int arg_count) {
    size_t pos = 0;
    // snprintf returns the number of characters that *would*
    // have been written (excluding NUL), or negative on
    // encoding error. A negative return means the format is
    // somehow invalid; we bail out with an empty string.
    int n = snprintf(out, cap, "%s", base);
    if (n < 0) { out[0] = '\0'; return; }
    // If n >= cap, snprintf already truncated; `pos` is set to
    // the truncated length so the loop below will not write
    // anything (the `pos + 2 < cap` guard fails). That is the
    // intended behaviour: no partial argument suffix is added
    // when there is no room for it.
    pos = (size_t)n;
    for (int i = 0; i < arg_count && pos + 2 < cap; i++) {
        // type_name returns a static ring buffer string. Two
        // calls in the same statement could use different
        // slots; the ring is 8 deep. Since we call type_name
        // once per argument and immediately snprintf the
        // result into `out`, the ring is only a hazard if a
        // single argument's rendering needs more than ~7
        // nested type_name calls (which would clobber the
        // ring in a way that could produce a wrong-looking
        // mangled name). Noted for the follow-ups.
        const char* a = type_name(args[i]);
        // `pos + 2 < cap` guarantees room for '#' plus at
        // least one character of the argument, plus the NUL.
        // Any larger argument is truncated by snprintf.
        int k = snprintf(out + pos, cap - pos, "#%s", a);
        if (k < 0) break;
        pos += (size_t)k;
    }
    // `pos` is not used after the loop; the final NUL is
    // written by snprintf. No explicit truncation marker is
    // added, so callers cannot tell from the string alone
    // whether it was truncated. See the header for the
    // contract.
}

int mono_template_count(void* generics) {
    if (!generics) return 0;
    return ((TemplateList*)generics)->count;
}

GenericTemplate* mono_template_at(void* generics, int i) {
    if (!generics) return NULL;
    TemplateList* tl = (TemplateList*)generics;
    if (i < 0 || i >= tl->count) return NULL;
    return &tl->items[i];
}