// ============================================================
// sema_types.c — Pass 0 and Pass 0b: nominal type
// registration and layout.
//
// Pass 0 (register_struct_or_enum) walks the program's
// top-level declarations and adds a TypeDef for each struct
// and enum. Generic declarations go to the monomorphizer as
// templates instead of getting a concrete TypeDef.
//
// Pass 0b resolves the content of every registered type:
// field types for structs, variant payloads for enums, and
// C-ABI layout for extern structs. It runs after Pass 0 so
// that a field can refer to a type declared later in the
// same program.
//
// The two C-ABI helpers (c_size_of, c_align_of) are private;
// they are the only functions in this file that are not
// entry points.
// ============================================================
#include "../include/sema_internal.h"
#include "../include/types.h"
#include "../include/ast.h"
#include "../include/source_map.h"
#include "../include/diagnostics.h"
#include "../include/monomorph.h"
#include "../include/common.h"
#include <string.h>
#include <stdlib.h>

// ---------- C-ABI helpers (private) ----------

// C-ABI size of a value of type `t`. Only called on types
// that passed the extern-struct field validator, which
// restricts to the leaf set plus nested extern structs.
static int c_size_of(const Type* t) {
    switch (t->kind) {
        case TY_I8:  case TY_U8:  return 1;
        case TY_I16: case TY_U16: return 2;
        case TY_I32: case TY_U32: case TY_F32: return 4;
        case TY_I64: case TY_U64: case TY_F64: return 8;
        case TY_RAWPTR: return 8;
        case TY_EXTERN_STRUCT: return t->tdef->struct_size;
        default: return 8;   // unreachable; validator prevents it
    }
}

// C-ABI alignment of a value of type `t`. Same restriction as
// c_size_of.
static int c_align_of(const Type* t) {
    switch (t->kind) {
        case TY_I8:  case TY_U8:  return 1;
        case TY_I16: case TY_U16: return 2;
        case TY_I32: case TY_U32: case TY_F32: return 4;
        case TY_I64: case TY_U64: case TY_F64: return 8;
        case TY_RAWPTR: return 8;
        case TY_EXTERN_STRUCT: return t->tdef->struct_align;
        default: return 1;
    }
}

// ---------- Public entry points ----------

// ============================================================
// Pass 0 registration of a nominal type
//
// For each top-level `type X = struct {...}` or `type X =
// enum {...}`:
//   - If the declaration has type parameters, register it as
//     a generic template. It is NOT added to the concrete
//     type table; it will be added when first instantiated.
//   - Otherwise, check for a duplicate name and add it to
//     the type table with is_pub copied from the AST and
//     is_rc_managed copied from the AST's is_builtin flag
//     (which the prelude sets).
//
// The TypeDef's kind and initial size are set here, but
// fields / variants are filled in Pass 0b (after this pass
// has registered every type's name, so a field can refer to
// a later-defined type).
//
// The `sdef_index` / `tdef_index` field on the AST node is
// set to the type's position in the table, so later passes
// can find the TypeDef from the AST in O(1).
// ============================================================
void register_struct_or_enum(AstNode* d, SemaCtx* sc,
                                    DiagnosticBag* bag) {
    Token name;
    int   tpc;
    bool  is_extern = false;
    bool  is_opaque = false;
    bool  is_union  = false;
    if (d->kind == AST_STRUCT_DEF) {
        name      = d->as.struct_def.name;
        tpc       = d->as.struct_def.type_param_count;
        is_extern = d->as.struct_def.is_extern;
        is_opaque = d->as.struct_def.is_opaque;
        is_union  = d->as.struct_def.is_union;
    } else {
        name = d->as.enum_def.name;
        tpc  = d->as.enum_def.type_param_count;
    }

    if (is_extern && tpc > 0) {
        diag_error(bag, d->span,
            "extern types cannot have type parameters");
        return;
    }

    if (tpc > 0) {
        mono_register_template(sc->structs->generics, d, sc->smap);
        return;
    }

    if (typedef_table_find(sc->structs, sc->smap, name)) {
        char buf[64];
        smap_span_text(sc->smap, name.span, buf, sizeof buf);
        diag_error(bag, d->span, "duplicate type '%s'", buf);
        return;
    }

    TypeDef* td = typedef_table_add(sc->structs, name, sc->smap);
    td->is_pub    = d->is_pub;
    td->is_opaque = is_opaque;  
    td->is_union  = is_union;
    
    if (d->kind == AST_STRUCT_DEF) {
        if (is_extern) {
            td->kind = TDEF_EXTERN_STRUCT;
            td->is_rc_managed = false;
            td->struct_size  = 0;
            td->struct_align = 1;
            // Non-opaque extern structs get their layout in
            // Pass 0b; opaque ones never do.
        } else {
            td->kind = TDEF_STRUCT;
            td->is_rc_managed = d->is_builtin;
            td->struct_size  = 8;
            td->struct_align = 8;
        }
        d->as.struct_def.sdef_index = (int)(td - sc->structs->items);
    } else {
        td->kind = TDEF_ENUM;
        td->is_rc_managed = d->is_builtin;
        d->as.enum_def.tdef_index = (int)(td - sc->structs->items);
    }
}

// For each non-generic struct and enum registered in Pass 0,
// resolve its field types and variant payload types and
// compute its layout (offset of each field, size of the
// struct).
//
// This runs after Pass 0 so that a field can refer to a type
// defined later in the same file (or in an imported file),
// and after build_module_table so that module-qualified
// field types resolve.
//
// Layout rules (identical to the instantiated-struct path in
// sema_instantiate_generic):
//   byte  0: tag / reserved slot
//   byte  8: first field
//   byte 16: second field
//   ...
// Every field is 8 bytes (one VM value slot).
//
// Enum layout is not computed here; enums are always 8
// bytes at runtime (a pointer to a payload block, or a
// small value whose tag lives in the low bits for some
// encodings; the current code uses a pointer to an
// allocated block, with tag and payload inline).
// ============================================================
void resolve_struct_fields(TypeDef* td, AstNode* d, SemaCtx* sc,
                                  DiagnosticBag* bag) {
    AstList* fl = &d->as.struct_def.fields;
    for (int j = 0; j < fl->count; j++) {
        AstNode* fld = fl->items[j];
        Type* ft = type_expr_resolve(fld->as.struct_field.type_expr,
                                     sc->smap, sc->structs,
                                     &sc->modtable, d->span.file_id,
                                     sc, bag);
        if (!ft) continue;
        if (ft->kind == TY_EXTERN_STRUCT) {
            diag_error(bag, fld->span,
                "regular struct fields cannot hold extern struct "
                "values; use *%s to store a pointer",
                type_name(ft));
            continue;
        }
        
        // No ref fields: a struct value can outlive the
        // scope that owns the referenced value, which
        // would leave a dangling reference.
        if (ft->kind == TY_REF) {
            diag_error(bag, fld->span,
                "struct fields cannot be of type ref<T>; "
                "references are scope-bound and would dangle "
                "if the struct outlived the scope that owns "
                "the referenced value");
            continue;
        }
        fld->as.struct_field.type = ft;
        // Offset = tag slot + 8 bytes per already-added field.
        fld->as.struct_field.offset = 8 + td->field_count * 8;

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
}



// Resolve an enum's variants: name, payload types, tag.
//
// Duplicate variant names are rejected. Payload types are
// resolved against the substituted environment; a payload
// that fails to resolve becomes i64 (the error was already
// reported by type_expr_resolve). ref payloads are rejected
// for the same reason as ref fields.
//
// Tag assignment: 0, 1, 2, ... in declaration order. That
// ordering is the runtime contract for the `?` operator
// (which assumes Ok/Some have tag 0 and Err/None have tag 1)
// and for the prelude's Result/Option.
void resolve_enum_variants(TypeDef* td, AstNode* d, SemaCtx* sc,
                                  DiagnosticBag* bag) {
    AstList* vl = &d->as.enum_def.variants;
    for (int j = 0; j < vl->count; j++) {
        AstNode* v = vl->items[j];
        Token vn = v->as.enum_variant.name;

        // Hard cap on variant count. The coverage array in
        // check_exhaustive is `bool covered[128]`, indexed by
        // variant tag; a 129th variant would silently be
        // ignored by the exhaustiveness check. Rejecting it
        // here keeps that array provably sufficient.
        //
        // `break`, not `continue`: once the cap is hit, every
        // later variant would also exceed it, and reporting
        // one error per variant is noise. The remaining
        // variants are dropped; the enum is already invalid.
        if (td->variant_count >= MAX_ENUM_VARIANTS) {
            diag_error(bag, v->span,
                "enum '%s' has more than %d variants (max %d)",
                td->name_buf, MAX_ENUM_VARIANTS, MAX_ENUM_VARIANTS);
            break;
        }
        

        // Duplicate variant name check within this enum.
        bool dup = false;
        for (int k = 0; k < td->variant_count; k++) {
            if (smap_tokens_equal(sc->smap, td->variants[k].name, vn)) {
                char buf[64];
                smap_span_text(sc->smap, vn.span, buf, sizeof buf);
                diag_error(bag, v->span, "duplicate variant '%s'", buf);
                dup = true; break;
            }
        }
        if (dup) continue;

        if (td->variant_count >= td->variant_capacity) {
            td->variant_capacity = td->variant_capacity
                                 ? td->variant_capacity * 2 : 4;
            td->variants = (EnumVariantDef*)xrealloc(td->variants,
                sizeof(EnumVariantDef) * (size_t)td->variant_capacity);
        }
        
        EnumVariantDef* vd = &td->variants[td->variant_count++];
        memset(vd, 0, sizeof *vd);
        vd->name = vn;
        smap_span_text(sc->smap, vn.span, vd->name_buf, sizeof vd->name_buf);

        // payload resolution
        int pc = v->as.enum_variant.payload_exprs.count;
        
        // Cap payload count at 16, matching the AST's fixed-size
        // tmp_slots[16] in pat_variant. A variant with more than 16
        // payloads cannot be destructured by the pattern compiler,
        // so it is rejected at declaration time rather than being
        // allowed to overrun the AST node later.
        if (pc > MAX_VARIANT_PAYLOADS) {
            diag_error(bag, v->span,
                "variant '%s' has %d payload(s); max is 16",
                vd->name_buf, pc);
            pc = MAX_VARIANT_PAYLOADS;   // clamp so the allocation and the loop below agree
        }
        
        vd->payload_count    = pc;
        vd->payload_capacity = pc;
        vd->payloads = (Type**)xcalloc((size_t)(pc ? pc : 1), sizeof(Type*));
        
        for (int p = 0; p < pc; p++) {
            AstNode* pe = v->as.enum_variant.payload_exprs.items[p];
            Type* pt = type_expr_resolve(pe->as.type_expr_node.te,
                                         sc->smap, sc->structs,
                                         &sc->modtable, d->span.file_id,
                                         sc, bag);
            if (!pt) pt = ty_i64();
            if (pt->kind == TY_EXTERN_STRUCT) {
                diag_error(bag, pe->span,
                    "enum payloads cannot hold extern struct "
                    "values; use *%s",
                    type_name(pt));
                pt = ty_i64();
            }
            
            if (pt->kind == TY_REF) {
                diag_error(bag, pe->span,
                    "enum payloads cannot be of type ref<T>; "
                    "references are scope-bound and would dangle "
                    "if the enum outlived the scope that owns "
                    "the referenced value");
                pt = ty_i64();
            }
            vd->payloads[p] = pt;
        }
        // Mirror the resolved data back onto the AST node.
        // (Both the AST node and the TypeDef now point at
        // the same payloads array; the AST copy is not
        // freed during Pass 0, so no dangling.)
        v->as.enum_variant.payloads = vd->payloads;
        v->as.enum_variant.tag      = td->variant_count - 1;
    }
}


// ============================================================
// Pass 0b: fields and variants

// Resolve every extern struct's fields and compute their C-ABI
// layout.
//
// Two passes are required because the layout of an extern
// struct depends on the sizes and alignments of the extern
// structs it contains by value, and those may be declared
// later in the source. Iterating to a fixpoint handles any
// declaration order without needing a topological sort.
//
// Pass 1: resolve each field's TypeExpr to a Type* and reject
//         anything that is not C-compatible.
// Pass 2: lay out each struct whose extern-struct fields are
//         already sized. Iterate until no progress is possible;
//         any struct still unsized at that point is part of a
//         cycle (or refers to an extern struct that is).
//
// Both diagnostics and the layout write go through the AST's
// field nodes (offset) and the TypeDef's flat fields array
// (name/type/offset). That matches the layout convention used
// by `resolve_struct_fields` for regular structs.
void resolve_extern_structs(AstNode* program, SemaCtx* sc,
                                   DiagnosticBag* bag) {
    // Collect the extern struct declarations and their TypeDefs.
    int n = 0;
    for (int i = 0; i < program->as.program.decls.count; i++) {
        AstNode* d = program->as.program.decls.items[i];
        if (d->kind == AST_STRUCT_DEF
            && d->as.struct_def.is_extern
            && !d->as.struct_def.is_opaque
            && d->as.struct_def.sdef_index >= 0)
            n++;
    }
    if (n == 0) return;

    AstNode** nodes = (AstNode**)xmalloc(sizeof(AstNode*) * (size_t)n);
    TypeDef** tdefs = (TypeDef**)xmalloc(sizeof(TypeDef*) * (size_t)n);
    int k = 0;
    for (int i = 0; i < program->as.program.decls.count; i++) {
        AstNode* d = program->as.program.decls.items[i];
        if (d->kind == AST_STRUCT_DEF
            && d->as.struct_def.is_extern
            && !d->as.struct_def.is_opaque
            && d->as.struct_def.sdef_index >= 0) {
            nodes[k] = d;
            tdefs[k] = &sc->structs->items[d->as.struct_def.sdef_index];
            k++;
        }
    }

    // ---- Pass 1: field types ----
    for (int i = 0; i < n; i++) {
        AstNode* d = nodes[i];
        TypeDef* td = tdefs[i];
        AstList* fl = &d->as.struct_def.fields;

        if (fl->count == 0) {
            diag_error(bag, d->span,
                "extern struct '%s' has no fields", td->name_buf);
            // Mark laid out so the fixpoint loop does not
            // re-report. 1 rather than 0 so the "size 0 means
            // not yet laid out" sentinel holds.
            td->struct_size = 1;
            td->struct_align = 1;
            continue;
        }

        for (int j = 0; j < fl->count; j++) {
            AstNode* fld = fl->items[j];
            Type* ft = type_expr_resolve(
                fld->as.struct_field.type_expr,
                sc->smap, sc->structs, &sc->modtable,
                d->span.file_id, sc, bag);
            if (!ft) {
                // Resolution failed and reported its own
                // error. Use i64 so the layout can continue;
                // this struct is already invalid.
                ft = ty_i64();
            }
            if (ft->kind == TY_REF) {
                diag_error(bag, fld->span,
                    "extern struct fields cannot be of type ref<T>");
                ft = ty_i64();
            } else if (ft->kind == TY_RC || ft->kind == TY_STRING) {
                diag_error(bag, fld->span,
                    "extern struct fields cannot be RC-managed "
                    "(string, rc<T>, Result, Option); C memory "
                    "has no reference-count header");
                ft = ty_i64();
            } else if (ft->kind == TY_STRUCT || ft->kind == TY_ENUM
                       || ft->kind == TY_ARRAY) {
                diag_error(bag, fld->span,
                    "extern struct fields must be C-compatible "
                    "(integers, floats, pointers, or nested "
                    "extern structs); found %s", type_name(ft));
                ft = ty_i64();
            } else if (ft->kind == TY_EXTERN_STRUCT && ft->tdef
                && ft->tdef->is_opaque) {
                // opaque types have no size, so they cannot be
                // stored by value in a struct whose own size
                // must be computable
                diag_error(bag, fld->span,
                    "extern struct fields cannot have opaque type "
                    "'%s'; only pointers to opaque types are allowed",
                    ft->tdef->name_buf);
                ft = ty_i64();
            }
            fld->as.struct_field.type = ft;
        }
    }

    // ---- Pass 2: layout to fixpoint ----
    bool progress = true;
    while (progress) {
        progress = false;
        for (int i = 0; i < n; i++) {
            TypeDef* td = tdefs[i];
            if (td->struct_size > 0) continue;   // already laid out

            AstNode* d = nodes[i];
            AstList* fl = &d->as.struct_def.fields;

            // Ready when every nested extern struct field has
            // a size. A forward reference to another extern
            // struct is fine as long as that one laid out
            // first; the fixpoint loop tries again next pass.
            bool ready = true;
            for (int j = 0; j < fl->count; j++) {
                Type* ft = fl->items[j]->as.struct_field.type;
                if (ft->kind == TY_EXTERN_STRUCT
                    && ft->tdef->struct_size == 0) {
                    ready = false;
                    break;
                }
                
            }
            if (!ready) continue;

            //  extern union
            if (td->is_union) {
                // Every field at offset 0. Size = max of
                // field sizes; alignment = max of field
                // alignments. Total rounded up to the
                // union's own alignment, matching the C rule
                // that sizeof(union) is a multiple of its
                // alignment.
                int max_size  = 0;
                int max_align = 1;
                for (int j = 0; j < fl->count; j++) {
                    AstNode* fld = fl->items[j];
                    Type* ft = fld->as.struct_field.type;
                    int size  = c_size_of(ft);
                    int align = c_align_of(ft);
                    fld->as.struct_field.offset = 0;

                    if (td->field_count >= td->field_capacity) {
                        td->field_capacity = td->field_capacity
                                           ? td->field_capacity * 2 : 4;
                        td->fields = (StructField*)xrealloc(td->fields,
                            sizeof(StructField) * (size_t)td->field_capacity);
                    }
                    td->fields[td->field_count].name   = fld->as.struct_field.name;
                    td->fields[td->field_count].type   = ft;
                    td->fields[td->field_count].offset = 0;
                    td->field_count++;

                    if (size  > max_size)  max_size  = size;
                    if (align > max_align) max_align = align;
                }
                int total = (max_size + max_align - 1) & ~(max_align - 1);
                td->struct_size  = total;
                td->struct_align = max_align;
            } else {
                // Existing struct layout, unchanged.
                int offset    = 0;
                int max_align = 1;
                for (int j = 0; j < fl->count; j++) {
                    AstNode* fld = fl->items[j];
                    Type* ft = fld->as.struct_field.type;
                    int align = c_align_of(ft);
                    int size  = c_size_of(ft);
                    offset = (offset + align - 1) & ~(align - 1);
                    fld->as.struct_field.offset = offset;

                    if (td->field_count >= td->field_capacity) {
                        td->field_capacity = td->field_capacity
                                           ? td->field_capacity * 2 : 4;
                        td->fields = (StructField*)xrealloc(td->fields,
                            sizeof(StructField) * (size_t)td->field_capacity);
                    }
                    td->fields[td->field_count].name   = fld->as.struct_field.name;
                    td->fields[td->field_count].type   = ft;
                    td->fields[td->field_count].offset = offset;
                    td->field_count++;

                    offset += size;
                    if (align > max_align) max_align = align;
                }
                offset = (offset + max_align - 1) & ~(max_align - 1);
                td->struct_size  = offset;
                td->struct_align = max_align;
            }
            progress = true;
        }
    }

    // Any struct still at size 0 is a cycle or refers to one.
    for (int i = 0; i < n; i++) {
        if (tdefs[i]->struct_size == 0) {
            diag_error(bag, nodes[i]->span,
                "extern struct '%s' has a cyclic or unresolved "
                "layout", tdefs[i]->name_buf);
        }
    }

    free(nodes);
    free(tdefs);
}

bool sema_tdef_has_rc_content(const TypeDef* td) {
    if (!td) return false;
    if (td->kind == TDEF_EXTERN_STRUCT) return false;
    if (td->kind == TDEF_BUILTIN) return false;

    if (td->kind == TDEF_STRUCT) {
        for (int i = 0; i < td->field_count; i++)
            if (type_contains_rc(td->fields[i].type)) return true;
        return false;
    }

    if (td->kind == TDEF_ENUM) {
        for (int v = 0; v < td->variant_count; v++) {
            const EnumVariantDef* vd = &td->variants[v];
            for (int p = 0; p < vd->payload_count; p++)
                if (type_contains_rc(vd->payloads[p])) return true;
        }
        return false;
    }

    return false;
}

void sema_resolve_rc_managing(TypeDefTable* structs) {
    if (!structs) return;
    bool progress = true;
    while (progress) {
        progress = false;
        for (int i = 0; i < structs->count; i++) {
            TypeDef* td = &structs->items[i];
            if (td->is_rc_managed) continue;
            if (sema_tdef_has_rc_content(td)) {
                td->is_rc_managed = true;
                progress = true;
            }
        }
    }
}