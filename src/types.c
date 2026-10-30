// ============================================================
// types.c — Compile-time types, type-expression resolution,
// and runtime type-descriptor interning.
//
// Reading order for this file:
//   1. Primitive singletons and their accessors. These are
//      the only Type objects in the compiler that are not
//      heap-allocated; pointer equality is meaningful for
//      them.
//   2. Compound type constructors. Each is a thin wrapper
//      over alloc_compound.
//   3. type_name — the renderer.
//   4. type_eq / type_contains_ref / integer predicates /
//      int_value_fits — the predicate layer.
//   5. TypeExpr constructors and type_expr_free.
//   6. Nominal type table (TypeDefTable) — init/free/find/add.
//   7. Type expression resolution — resolve_inner, the big
//      switch that turns a TypeExpr into a Type*.
//   8. Runtime type descriptors — desc_kind_of,
//      desc_matches, typetable_intern, typetable_get.
//
// The file has no global mutable state that outlives a single
// call except the primitive singletons (which are read-only
// after initialization) and the render ring in type_name.
// ============================================================
#include "../include/types.h"
#include "../include/source_map.h"
#include "../include/resolve.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ------------------------------------------------------------
// Primitive TypeDefs
//
// Each primitive has a nominal identity so that methods can
// attach to it. The TypeDef's `kind` is TDEF_BUILTIN, which
// distinguishes it from user-declared structs/enums and from
// extern types. `name_buf` holds the printable name; the
// `name` Token is left zero-valued because builtins have no
// source location.
//
// These are file-static: they live for the process lifetime
// and are never freed. `typedef_table_find` consults them
// before the user table, so a user cannot shadow `string`
// with their own `type string = ...` — the duplicate check
// in `register_struct_or_enum` catches that.
// ------------------------------------------------------------
static TypeDef TDEF_VOID   = { .kind = TDEF_BUILTIN, .name_buf = "void",   .is_pub = true, .struct_size = 0 };
static TypeDef TDEF_I8     = { .kind = TDEF_BUILTIN, .name_buf = "i8",     .is_pub = true, .struct_size = 1 };
static TypeDef TDEF_I16    = { .kind = TDEF_BUILTIN, .name_buf = "i16",    .is_pub = true, .struct_size = 2 };
static TypeDef TDEF_I32    = { .kind = TDEF_BUILTIN, .name_buf = "i32",    .is_pub = true, .struct_size = 4 };
static TypeDef TDEF_I64    = { .kind = TDEF_BUILTIN, .name_buf = "i64",    .is_pub = true, .struct_size = 8 };
static TypeDef TDEF_U8     = { .kind = TDEF_BUILTIN, .name_buf = "u8",     .is_pub = true, .struct_size = 1 };
static TypeDef TDEF_U16    = { .kind = TDEF_BUILTIN, .name_buf = "u16",    .is_pub = true, .struct_size = 2 };
static TypeDef TDEF_U32    = { .kind = TDEF_BUILTIN, .name_buf = "u32",    .is_pub = true, .struct_size = 4 };
static TypeDef TDEF_U64    = { .kind = TDEF_BUILTIN, .name_buf = "u64",    .is_pub = true, .struct_size = 8 };
static TypeDef TDEF_F32    = { .kind = TDEF_BUILTIN, .name_buf = "f32",    .is_pub = true, .struct_size = 4 };
static TypeDef TDEF_F64    = { .kind = TDEF_BUILTIN, .name_buf = "f64",    .is_pub = true, .struct_size = 8 };
static TypeDef TDEF_BOOL   = { .kind = TDEF_BUILTIN, .name_buf = "bool",   .is_pub = true, .struct_size = 8 };
static TypeDef TDEF_STRING = { .kind = TDEF_BUILTIN, .name_buf = "string", .is_pub = true, .struct_size = 8 };


TypeDef* builtin_tdef_by_name(const char* name) {
    if (strcmp(name, "void")   == 0) return &TDEF_VOID;
    if (strcmp(name, "i8")     == 0) return &TDEF_I8;
    if (strcmp(name, "i16")    == 0) return &TDEF_I16;
    if (strcmp(name, "i32")    == 0) return &TDEF_I32;
    if (strcmp(name, "i64")    == 0) return &TDEF_I64;
    if (strcmp(name, "u8")     == 0) return &TDEF_U8;
    if (strcmp(name, "u16")    == 0) return &TDEF_U16;
    if (strcmp(name, "u32")    == 0) return &TDEF_U32;
    if (strcmp(name, "u64")    == 0) return &TDEF_U64;
    if (strcmp(name, "f32")    == 0) return &TDEF_F32;
    if (strcmp(name, "f64")    == 0) return &TDEF_F64;
    if (strcmp(name, "bool")   == 0) return &TDEF_BOOL;
    if (strcmp(name, "string") == 0) return &TDEF_STRING;
    return NULL;
}

static Type T_VOID   = { TY_VOID,   0, NULL, false, &TDEF_VOID,   NULL, 0, NULL };
static Type T_I8     = { TY_I8,     1, NULL, false, &TDEF_I8,     NULL, 0, NULL };
static Type T_I16    = { TY_I16,    2, NULL, false, &TDEF_I16,    NULL, 0, NULL };
static Type T_I32    = { TY_I32,    4, NULL, false, &TDEF_I32,    NULL, 0, NULL };
static Type T_I64    = { TY_I64,    8, NULL, false, &TDEF_I64,    NULL, 0, NULL };
static Type T_U8     = { TY_U8,     1, NULL, false, &TDEF_U8,     NULL, 0, NULL };
static Type T_U16    = { TY_U16,    2, NULL, false, &TDEF_U16,    NULL, 0, NULL };
static Type T_U32    = { TY_U32,    4, NULL, false, &TDEF_U32,    NULL, 0, NULL };
static Type T_U64    = { TY_U64,    8, NULL, false, &TDEF_U64,    NULL, 0, NULL };
static Type T_F32    = { TY_F32,    4, NULL, false, &TDEF_F32,    NULL, 0, NULL };
static Type T_F64    = { TY_F64,    8, NULL, false, &TDEF_F64,    NULL, 0, NULL };
static Type T_BOOL   = { TY_BOOL,   8, NULL, false, &TDEF_BOOL,   NULL, 0, NULL };
static Type T_STRING = { TY_STRING, 8, NULL, false, &TDEF_STRING, NULL, 0, NULL };

// Accessors. Each returns the address of the corresponding
// static; the returned pointer must not be mutated (the
// singletons are shared across the whole compilation).
Type* ty_void(void)   { return &T_VOID;   }
Type* ty_i8(void)     { return &T_I8;     }
Type* ty_i16(void)    { return &T_I16;    }
Type* ty_i32(void)    { return &T_I32;    }
Type* ty_i64(void)    { return &T_I64;    }
Type* ty_u8(void)     { return &T_U8;     }
Type* ty_u16(void)    { return &T_U16;    }
Type* ty_u32(void)    { return &T_U32;    }
Type* ty_u64(void)    { return &T_U64;    }
Type* ty_f32(void)    { return &T_F32;    }
Type* ty_f64(void)    { return &T_F64;    }
Type* ty_bool(void)   { return &T_BOOL;   }
Type* ty_string(void) { return &T_STRING; }

// Allocate and initialize a compound Type. Every compound
// constructor below funnels through here, so the only
// difference between them is which fields they set after the
// allocation. xcalloc zeroes; the caller sets `inner`/`tdef`
// and `is_mut` as needed.
static Type* alloc_compound(TypeKind k, int size) {
    Type* t = (Type*)xcalloc(1, sizeof *t);
    t->kind = k;
    t->size = size;
    return t;
}

// Compound type constructors.
//
// Each: allocate a Type of the appropriate kind, set inner (and
// is_mut for ref) or tdef, return it. Sizes are fixed by the
// kind because all compound types are pointer-shaped (8 bytes)
// in the VM's value representation — even arrays, which are
// passed around by reference to their heap storage.
//
// None of these are interned; two calls with equal arguments
// give two distinct pointers. type_eq handles that by
// comparing structurally.
Type* ty_ref(Type* inner, bool is_mut) {
    Type* t = alloc_compound(TY_REF, 8);
    t->inner = inner; t->is_mut = is_mut; return t;
}
Type* ty_rc(Type* inner) {
    Type* t = alloc_compound(TY_RC, 8); t->inner = inner; return t;
}
Type* ty_rawptr(Type* inner) {
    Type* t = alloc_compound(TY_RAWPTR, 8); t->inner = inner; return t;
}
Type* ty_array(Type* inner) {
    Type* t = alloc_compound(TY_ARRAY, 8); t->inner = inner; return t;
}
// Struct and enum types carry the TypeDef's size (computed by
// sema during layout), not the 8-byte pointer size the other
// compound kinds use. The tdef pointer is borrowed; the
// TypeDef lives in the TypeDefTable.
Type* ty_struct(TypeDef* tdef) {
    Type* base = alloc_compound(TY_STRUCT, tdef->struct_size);
    base->tdef = tdef;
    if (tdef->is_rc_managed) return ty_rc(base);
    return base;
}
Type* ty_extern_struct(TypeDef* tdef) {
    Type* t = alloc_compound(TY_EXTERN_STRUCT, tdef->struct_size);
    t->tdef = tdef; return t;
}
Type* ty_fn(Type** params, int n, Type* ret) {
    Type* t = alloc_compound(TY_FN, 8);
    if (n > 0) {
        t->fn_params = (Type**)xmalloc(sizeof(Type*) * (size_t)n);
        for (int i = 0; i < n; i++) t->fn_params[i] = params[i];
    }
    t->fn_param_count = n;
    t->fn_ret = ret;
    return t;
}
Type* ty_enum(TypeDef* tdef) {
    Type* base = alloc_compound(TY_ENUM, 8);
    base->tdef = tdef;
    if (tdef->is_rc_managed) return ty_rc(base);
    return base;
}

// ============================================================
// Rendering
// ============================================================

// Render a mangled type name as source-level syntax:
// "Option#i64" -> "Option<i64>", "Pair#i32#i32" ->
// "Pair<i32, i32>". Names without '#' are returned
// unchanged. The transformation is purely textual and safe
// because '#' cannot appear in a user-written identifier.
static const char* unmangle_name(const char* mangled, char* buf, size_t cap) {
    const char* hash = strchr(mangled, '#');
    if (!hash) return mangled;

    size_t base_len = (size_t)(hash - mangled);
    if (base_len + 1 >= cap) return mangled;
    memcpy(buf, mangled, base_len);
    size_t pos = base_len;

    if (pos + 1 < cap) buf[pos++] = '<';
    const char* p = hash;
    int first = 1;
    while (*p == '#' && pos + 1 < cap) {
        if (!first) {
            if (pos + 2 < cap) {
                buf[pos++] = ',';
                buf[pos++] = ' ';
            }
        }
        first = 0;
        p++;
        const char* end = strchr(p, '#');
        size_t seg_len = end ? (size_t)(end - p) : strlen(p);
        if (pos + seg_len + 1 >= cap) break;
        memcpy(buf + pos, p, seg_len);
        pos += seg_len;
        p = end ? end : p + seg_len;
    }
    if (pos + 1 < cap) buf[pos++] = '>';
    buf[pos] = '\0';
    return buf;
}

// Render a type into a static ring of buffers.
//
// Why the ring: type_name recurses for compound kinds
// (`ref<X>` renders X with a recursive call, and the same for
// rc/rawptr/array). If there were a single static buffer, the
// inner call would overwrite the outer call's buffer before
// the outer call had a chance to snprintf into it. The ring
// gives each nesting level its own slot; 8 levels is enough
// for anything a human writes.
//
// Failure to consider: the ring is process-global, not
// reentrant across threads, and a call site that holds a
// type_name result across another type_name call will see the
// first result clobbered if the second call happens to use
// the same slot. Callers that need a durable string must copy
// it.
//
// The `slot` counter rotates 0..7 and wraps. Every recursive
// call advances it, so the two halves of `ref<rc<i64>>` (the
// outer string and the inner string) get different slots.
//
// Struct and enum return `tdef->name_buf` directly, without
// going through the ring: they are already stored as a
// NUL-terminated string in the TypeDef and do not need
// formatting. This is also why a diagnostic that captures a
// type_name result for a compound struct type is stable
// (name_buf is not shared/rotated), while one for a ref is
// not.
const char* type_name(const Type* t) {
    static char ring[8][128];
    static int  slot = 0;
    if (!t) return "<error>";
    switch (t->kind) {
        case TY_VOID:   return "void";
        case TY_I8:     return "i8";
        case TY_I16:    return "i16";
        case TY_I32:    return "i32";
        case TY_I64:    return "i64";
        case TY_U8:     return "u8";
        case TY_U16:    return "u16";
        case TY_U32:    return "u32";
        case TY_U64:    return "u64";
        case TY_F32:    return "f32";
        case TY_F64:    return "f64";
        case TY_BOOL:   return "bool";
        case TY_STRING: return "string";
        case TY_REF: {
            // Advance the ring slot before recursing so the
            // inner call gets a different slot from this one.
            // Do not reorder these two lines.
            char* b = ring[slot]; slot = (slot + 1) % 8;
            snprintf(b, 128, t->is_mut ? "ref<mut %s>" : "ref<%s>",
                     type_name(t->inner));
            return b;
        }
        case TY_RC: {
            // An RC-managed named aggregate (Result, Option, or a
            // user struct/enum with RC-typed fields) is spelled
            // without the rc<> wrapper: the wrapper is a
            // compiler-internal representation, not source syntax
            // the user writes.
            if (t->inner
                && t->inner->tdef
                && t->inner->tdef->is_rc_managed) {
                return type_name(t->inner);
            }
            char* b = ring[slot]; slot = (slot + 1) % 8;
            snprintf(b, 128, "rc<%s>", type_name(t->inner));
            return b;
        }
        case TY_RAWPTR: {
            char* b = ring[slot]; slot = (slot + 1) % 8;
            snprintf(b, 128, "*%s", type_name(t->inner));
            return b;
        }
        case TY_ARRAY: {
            char* b = ring[slot]; slot = (slot + 1) % 8;
            snprintf(b, 128, "[%s]", type_name(t->inner));
            return b;
        }
        case TY_STRUCT:
        case TY_EXTERN_STRUCT:
        case TY_ENUM:{
            // name_buf is fixed-size 128 in the TypeDef; see
            // the header. Returning it directly avoids the
            // ring entirely, at the cost that two consecutive
            // type_name calls on the *same* struct type return
            // the same pointer (which is fine, since it points
            // at stable memory).
            char* b = ring[slot]; slot = (slot + 1) % 8;
            return unmangle_name(t->tdef->name_buf, b, 128);
            }
        case TY_FN: {
            char* b = ring[slot]; slot = (slot + 1) % 8;
            int pos = snprintf(b, 128, "fn(");
            for (int i = 0; i < t->fn_param_count && pos < 127; i++) {
                if (i > 0 && pos < 127)
                    pos += snprintf(b + pos, (size_t)(128 - pos), ", ");
                if (pos < 127)
                    pos += snprintf(b + pos, (size_t)(128 - pos), "%s",
                                    type_name(t->fn_params[i]));
            }
            if (pos < 127)
                snprintf(b + pos, (size_t)(128 - pos), ") : %s",
                         type_name(t->fn_ret));
            return b;
        }
    }
    return "?";
}

// ============================================================
// Equality and predicates
// ============================================================

// Structural equality between two Types.
//
// Fast paths and rules, in order:
//   1. Pointer equality: same Type object, true. This is the
//      common case for primitives (singletons) and for any
//      caller that reuses the same Type pointer.
//   2. NULL on either side: only equal if both are NULL,
//      which is not what the pointer-equality branch above
//      caught, so return false.
//   3. Different kinds: false. This is why `ref<i64>` and
//      `rc<i64>` are never equal even though both wrap i64.
//   4. Same kind, compare by kind:
//      - TY_REF: is_mut must match, then recurse into inner.
//        So `ref<i64>` and `ref<mut i64>` are distinct types.
//      - TY_RC, TY_ARRAY: recurse into inner.
//      - TY_RAWPTR: `*void` on either side means "matches
//        anything pointer-shaped" (the language's `void*`).
//        Otherwise recurse into inner. This is the only
//        non-reflexive branch: `*void == *i64` is true even
//        though the two descriptors differ.
//      - TY_STRUCT, TY_ENUM: nominal equality. Two struct
//        types are equal iff their tdef pointers are the
//        same. This is what makes two structurally identical
//        but nominally distinct structs unequal.
//      - Everything else (all the scalars): true, because
//        step 3 already matched the kinds and the scalars
//        have no further structure to compare.
bool type_eq(const Type* a, const Type* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TY_REF:
            return a->is_mut == b->is_mut && type_eq(a->inner, b->inner);
        case TY_RC:
        case TY_ARRAY:
            return type_eq(a->inner, b->inner);
        case TY_RAWPTR:
            // `*void` is the type of the `null` literal and the
            // universal pointer type (like C's `void*`). It unifies
            // with any other pointer type.
            //
            // Note the asymmetry with the rest of the function:
            // this branch returns true even when the inner types
            // differ, provided one of them is void. That is
            // intentional — the language wants `*void` to be a
            // supertype of every raw pointer.
            if (a->inner && a->inner->kind == TY_VOID) return true;
            if (b->inner && b->inner->kind == TY_VOID) return true;
            return type_eq(a->inner, b->inner);
        case TY_STRUCT:
        case TY_EXTERN_STRUCT:
        case TY_ENUM:
            return a->tdef == b->tdef;
        case TY_FN:
            if (a->fn_param_count != b->fn_param_count) return false;
            for (int i = 0; i < a->fn_param_count; i++)
                if (!type_eq(a->fn_params[i], b->fn_params[i]))
                    return false;
            return type_eq(a->fn_ret, b->fn_ret);
            
        default:
            return true;
    }
}

// Does a type contain a TY_REF anywhere reachable through
// inner/rc/rawptr/array?
//
// This is used by the resolver to reject reference-to-reference
// in a handful of contexts (`ref<ref<T>>`, `rc<ref<T>>`,
// `*ref<T>`, `[ref<T>]`). A struct field of ref type does NOT
// make the struct "contain a ref" by this predicate; the
// predicate is a shallow recursion over the Type shape, not a
// transitive walk through TypeDefs.
//
// Note the asymmetry between the outer and inner handling of
// TY_REF: the outer TY_REF returns true immediately (it *is*
// a ref), while TY_RC/RAWPTR/ARRAY recurse into their inner
// because containing a ref is transitive through those
// wrappers.
bool type_contains_ref(const Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_REF:    return true;
        case TY_RC:     return type_contains_ref(t->inner);
        case TY_RAWPTR: return type_contains_ref(t->inner);
        case TY_ARRAY:  return type_contains_ref(t->inner);
        case TY_EXTERN_STRUCT: return false;
        default:        return false;
    }
}

bool type_contains_rc(const Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_RC:
        case TY_STRING:
            return true;
        case TY_ARRAY:
            return type_contains_rc(t->inner);
        case TY_STRUCT:
        case TY_ENUM:
            return t->tdef && t->tdef->is_rc_managed;
        default:
            return false;
    }
}

// Integer predicates. `is_integer_type` is the union of the
// eight integer kinds; the signed/unsigned variants partition
// it. All three return false for NULL so callers can pass an
// unresolved type (e.g. from a failed type_expr_resolve)
// without a separate NULL check.
bool is_integer_type(const Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_I8: case TY_I16: case TY_I32: case TY_I64:
        case TY_U8: case TY_U16: case TY_U32: case TY_U64:
            return true;
        default: return false;
    }
}
bool is_signed_int(const Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_I8: case TY_I16: case TY_I32: case TY_I64: return true;
        default: return false;
    }
}
bool is_unsigned_int(const Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_U8: case TY_U16: case TY_U32: case TY_U64: return true;
        default: return false;
    }
}

bool is_float_type(const Type* t) {
    if (!t) return false;
    return t->kind == TY_F32 || t->kind == TY_F64;
}

bool is_scalar_type(const Type* t) {
    if (!t) return false;
    return is_integer_type(t)
        || is_float_type(t)
        || t->kind == TY_BOOL;
}

bool is_equatable_type(const Type* t) {
    if (!t) return false;
    return is_scalar_type(t)
        || t->kind == TY_STRING
        || t->kind == TY_RAWPTR;
}

bool is_ordered_type(const Type* t) {
    if (!t) return false;
    if (is_integer_type(t)) return true;
    if (is_float_type(t)) return true;
    if (t->kind == TY_STRING) return true;
    return false;
}

bool cast_valid(const Type* from, const Type* to) {
    if (!from || !to) return false;

    // Scalar to scalar: ints, floats, bool, in any direction.
    if (is_scalar_type(from) && is_scalar_type(to)) return true;

    // Pointer to pointer.
    if (from->kind == TY_RAWPTR && to->kind == TY_RAWPTR) return true;

    // Pointer to 64-bit integer and back.
    if (from->kind == TY_RAWPTR
        && (to->kind == TY_I64 || to->kind == TY_U64)) return true;
    if (to->kind == TY_RAWPTR
        && (from->kind == TY_I64 || from->kind == TY_U64)) return true;

    return false;
}

// Range check for an integer literal (or any I64 value) against
// an integer target type. Every case encodes the min/max of the
// target explicitly, rather than using `1 << bits` arithmetic,
// because the sign-edness differs and because the constants
// need to be exactly right for the diagnostic message "value
// does not fit in i32" to be trustworthy.
//
// Notes:
//   - The bounds are written as LL-suffixed literals where the
//     value exceeds INT_MAX (2147483647). The two that do
//     (-2147483648 and 4294967295) are the ones that would
//     otherwise be promoted to the wrong type and compared
//     incorrectly.
//   - I64 accepts any I64 input; the check is trivially true.
//   - U64 accepts any non-negative I64. A negative literal
//     cannot fit in U64 even though a bit-pattern cast could
//     preserve it; this matches the language's rule that
//     `-1` is not a u64.
//   - Non-integer targets return false. That means `f64` and
//     `bool` are rejected here; callers that want to allow
//     integer literals to fit into e.g. f64 must special-case
//     it themselves.
bool int_value_fits(I64 v, Type* target) {
    if (!target) return false;
    switch (target->kind) {
        case TY_I8:  return v >= -128 && v <= 127;
        case TY_I16: return v >= -32768 && v <= 32767;
        case TY_I32: return v >= -2147483648LL && v <= 2147483647LL;
        case TY_I64: return true;
        case TY_U8:  return v >= 0 && v <= 255;
        case TY_U16: return v >= 0 && v <= 65535;
        case TY_U32: return v >= 0 && v <= 4294967295LL;
        case TY_U64: return v >= 0;
        default:     return false;
    }
}

bool uint_value_fits(U64 v, Type* target) {
    if (!target) return false;
    switch (target->kind) {
        case TY_I8:  return v <= 127ULL;
        case TY_I16: return v <= 32767ULL;
        case TY_I32: return v <= 2147483647ULL;
        case TY_I64: return v <= 9223372036854775807ULL;
        case TY_U8:  return v <= 255ULL;
        case TY_U16: return v <= 65535ULL;
        case TY_U32: return v <= 4294967295ULL;
        case TY_U64: return true;
        default:     return false;
    }
}
// ============================================================
// TypeExpr constructors
//
// Every constructor xcallocs (so all fields, including the
// unused ones, are zeroed) and then sets kind, span, and the
// payload fields specific to that kind. Fields not relevant
// to the kind are left zeroed; type_expr_free below handles
// every kind uniformly and does not need them set.
// ============================================================

// `Name`. Span is borrowed from the token.
TypeExpr* type_expr_name(Token name) {
    TypeExpr* te = (TypeExpr*)xcalloc(1, sizeof *te);
    te->kind = TEX_NAME; te->span = name.span; te->name = name;
    return te;
}
// `ref<T>` / `ref<mut T>`. `inner` is owned by this TypeExpr.
TypeExpr* type_expr_ref(TypeExpr* inner, bool is_mut, Span span) {
    TypeExpr* te = (TypeExpr*)xcalloc(1, sizeof *te);
    te->kind = TEX_REF; te->span = span; te->inner = inner; te->is_mut = is_mut;
    return te;
}
// `rc<T>`. inner owned.
TypeExpr* type_expr_rc(TypeExpr* inner, Span span) {
    TypeExpr* te = (TypeExpr*)xcalloc(1, sizeof *te);
    te->kind = TEX_RC; te->span = span; te->inner = inner;
    return te;
}
// `*T`. inner owned.
TypeExpr* type_expr_rawptr(TypeExpr* inner, Span span) {
    TypeExpr* te = (TypeExpr*)xcalloc(1, sizeof *te);
    te->kind = TEX_RAWPTR; te->span = span; te->inner = inner;
    return te;
}
// `[T]`. element owned.
TypeExpr* type_expr_array(TypeExpr* element, Span span) {
    TypeExpr* te = (TypeExpr*)xcalloc(1, sizeof *te);
    te->kind = TEX_ARRAY; te->span = span; te->inner = element;
    return te;
}

TypeExpr* type_expr_fn(Span span) {
    TypeExpr* te = (TypeExpr*)xcalloc(1, sizeof *te);
    te->kind = TEX_FN;
    te->span = span;
    return te;
}

void type_expr_fn_add_param(TypeExpr* te, TypeExpr* param) {
    if (te->param_count >= te->param_capacity) {
        te->param_capacity = te->param_capacity
                           ? te->param_capacity * 2 : 4;
        te->params = (TypeExpr**)xrealloc(te->params,
            sizeof(TypeExpr*) * (size_t)te->param_capacity);
    }
    te->params[te->param_count++] = param;
}

void type_expr_fn_set_return(TypeExpr* te, TypeExpr* ret) {
    te->ret = ret;
}

// `a.b.c`. `path` is copied into a fresh heap array so the
// caller's buffer (typically a stack array) may go out of
// scope. `n` must be > 0.
TypeExpr* type_expr_qualified(Token* path, int n, Span span) {
    TypeExpr* te = (TypeExpr*)xcalloc(1, sizeof *te);
    te->kind = TEX_QUALIFIED;
    te->span = span;
    te->path = (Token*)xmalloc(sizeof(Token) * (size_t)n);
    memcpy(te->path, path, sizeof(Token) * (size_t)n);
    te->path_count = n;
    return te;
}
// `Base<...>` with the base token and an empty args list. Add
// arguments with type_expr_generic_add_arg.
TypeExpr* type_expr_generic(Token base, Span span) {
    TypeExpr* te = (TypeExpr*)xcalloc(1, sizeof *te);
    te->kind = TEX_GENERIC;
    te->span = span;
    te->name = base;
    return te;
}
// Append a type argument. Growth: start at 4, double when
// full — same pattern as every other growable array in the
// compiler. Ownership of `arg` transfers to `te`.
void type_expr_generic_add_arg(TypeExpr* te, TypeExpr* arg) {
    if (te->arg_count >= te->arg_capacity) {
        te->arg_capacity = te->arg_capacity ? te->arg_capacity * 2 : 4;
        te->args = (TypeExpr**)xrealloc(te->args,
            sizeof(TypeExpr*) * (size_t)te->arg_capacity);
    }
    te->args[te->arg_count++] = arg;
}
// `TEX_RESOLVED` — a TypeExpr that already names a concrete
// Type. Used by the monomorphizer when substituting types into
// a copied AST: instead of inserting a syntactic form that
// would have to be re-resolved (and might not even be
// expressible in the source language), it inserts a resolved
// Type directly. The `resolved` pointer is borrowed; Types
// live for the process lifetime.
TypeExpr* type_expr_resolved(Type* t, Span span) {
    TypeExpr* te = (TypeExpr*)xcalloc(1, sizeof *te);
    te->kind = TEX_RESOLVED;
    te->span = span;
    te->resolved = t;
    return te;
}

// Recursively free a TypeExpr.
//
// The three potentially-owned fields are:
//   inner    — for TEX_REF / RC / RAWPTR / ARRAY; NULL
//              otherwise, so the recursive call is a no-op.
//   args[]   — for TEX_GENERIC; each element freed, then the
//              array itself.
//   path[]   — for TEX_QUALIFIED; a flat Token array, freed
//              with a single free (Tokens own no heap).
//
// Everything else is either inline (span, name, is_mut,
// resolved) or unused for this kind. `resolved` is NOT freed:
// Types live for the process lifetime.
void type_expr_free(TypeExpr* te) {
    if (!te) return;
    type_expr_free(te->inner);
    for (int i = 0; i < te->arg_count; i++)
        type_expr_free(te->args[i]);
    free(te->args);
    free(te->path);
    //  TEX_FN
    for (int i = 0; i < te->param_count; i++)
        type_expr_free(te->params[i]);
    free(te->params);
    type_expr_free(te->ret);
    free(te);
}

// ============================================================
// Nominal type table
// ============================================================

// Zero a TypeDefTable. The generics pointer and callback are
// owned by sema (which installed them); init deliberately
// does not touch them beyond zeroing, because init is also
// called from typedef_table_free, and free's caller may have
// already dropped sema's references.
void typedef_table_init(TypeDefTable* tt) {
    tt->items    = NULL;
    tt->count    = 0;
    tt->capacity = 0;
    tt->generics = NULL;
    tt->instantiate_generic = NULL;
}

// Free every owned sub-allocation and reset the table.
//
// For each TypeDef:
//   - Struct: free the `fields` array (a flat array of
//     StructField). Each field's `type` points into the
//     global type universe and is not freed.
//   - Enum: for each variant, free its `payloads` array
//     (Type**); then free the `variants` array itself. Each
//     payload Type* points into the global type universe
//     and is not freed.
//
// Note that the loop runs `count` times, so every entry is
// freed including instantiated ones. There is no "owned by
// mono" special case; instantiated TypeDefs live in the same
// table and are freed the same way.
void typedef_table_free(TypeDefTable* tt) {
    for (int i = 0; i < tt->count; i++) {
        TypeDef* td = &tt->items[i];
        free(td->fields);
        for (int v = 0; v < td->variant_count; v++)
            free(td->variants[v].payloads);
        free(td->variants);
    }
    free(tt->items);
    typedef_table_init(tt);
}



// Find a TypeDef by name across the whole table.
//
// Two filters are applied, in this order:
//   1. `is_instantiated` entries are skipped. This is what
//      keeps a monomorphized `Option#i64` from being found
//      when the user writes `Option`. Instantiated entries
//      are registered in the same table (so mono can look
//      them up by their mangled name), but they must not
//      participate in user-name lookup.
//   2. Names are compared with smap_tokens_equal, which does
//      a byte-for-byte comparison of the two tokens' source
//      text. That is why the function needs the SourceMap: it
//      does not store the identifier bytes on the TypeDef
//      (well, it does — name_buf — but the comparison here
//      is on Tokens, likely for consistency with the rest of
//      the codebase and to avoid truncation at 127 bytes).
//
// Linear scan. The table is small (typically tens to low
// hundreds of entries) so this is fine; if it grows, a hash
// keyed on the mangled name would be the obvious next step.

TypeDef* typedef_table_find(TypeDefTable* tt, const SourceMap* smap,
                            Token name) {
    if (!tt) return NULL;

    // Builtin lookup. Builtin TypeDefs are static and have
    // no source Token, so `smap_tokens_equal` cannot be
    // used; the name is compared as text instead. The cost
    // is up to 13 `strcmp`s per lookup, well below the cost
    // of the loop that follows.
    char buf[64];
    smap_span_text(smap, name.span, buf, sizeof buf);
    TypeDef* bt = builtin_tdef_by_name(buf);
    if (bt) return bt;

    // User types: linear scan, skipping instantiated generics
    // (whose names are mangled and not visible to name lookup).
    for (int i = 0; i < tt->count; i++) {
        if (tt->items[i].is_instantiated) continue;
        if (smap_tokens_equal(smap, tt->items[i].name, name))
            return &tt->items[i];
    }
    return NULL;
}

// Same as typedef_table_find but restricted to entries whose
// name token lives in `file_id`.
//
// The `name.span.file_id` field of a Token is what records
// which file the token was lexed from. So this function finds
// "types defined in file X". Note that unlike the unrestricted
// version, this one does NOT skip is_instantiated entries —
// instantiated types are registered with the *same* name span
// as their template, so they would not collide with a
// user-written name anyway. More importantly, an instantiation
// is looked up by name in some code paths, and skipping them
// here would break that.
TypeDef* typedef_table_find_in_file(TypeDefTable* tt, const SourceMap* smap,
                                    Token name, uint32_t file_id) {
    if (!tt) return NULL;
    for (int i = 0; i < tt->count; i++) {
        if (tt->items[i].name.span.file_id != file_id) continue;
        if (smap_tokens_equal(smap, tt->items[i].name, name)) return &tt->items[i];
    }
    return NULL;
}

// Register a new TypeDef and return a pointer to it.
//
// Growth: doubling from 8, same pattern as everywhere else.
//
// The new entry is memset to 0 (so fields the caller forgets
// to set are NULL/0 rather than garbage), then `name` and
// `name_buf` are filled. `name_buf` is a 128-byte copy of the
// token's text via smap_span_text; truncation at 127 bytes is
// silent. The caller is responsible for setting `kind`,
// `is_pub`, fields/variants, etc.
//
// Because growth uses realloc, existing TypeDef pointers may
// move. Callers should not hold TypeDef* across a call to
// this function.
TypeDef* typedef_table_add(TypeDefTable* tt, Token name,
                           const SourceMap* smap) {
    if (tt->count >= tt->capacity) {
        tt->capacity = tt->capacity ? tt->capacity * 2 : 8;
        tt->items = (TypeDef*)xrealloc(
            tt->items, sizeof(TypeDef) * (size_t)tt->capacity);
    }
    TypeDef* td = &tt->items[tt->count++];
    memset(td, 0, sizeof *td);
    td->name = name;
    smap_span_text(smap, name.span, td->name_buf, sizeof td->name_buf);
    return td;
}

bool type_has_value_representation(const Type* t) {
    if (!t) return false;
    return t->kind != TY_EXTERN_STRUCT;
}

// Turn a resolved TypeDef into its corresponding Type*. The
// kind of the TypeDef determines which constructor runs. Used
// by every name-lookup path in resolve_inner so that adding a
// new TypeDefKind is one change, not three.
static Type* type_for_tdef(TypeDef* td) {
    switch (td->kind) {
        case TDEF_STRUCT:        return ty_struct(td);
        case TDEF_ENUM:          return ty_enum(td);
        case TDEF_EXTERN_STRUCT: return ty_extern_struct(td);
        case TDEF_BUILTIN:
            // Builtins are the primitives; their Type
            // singletons already exist and are returned by
            // the ty_*() accessors. Reaching here means the
            // resolver found a builtin by name; return the
            // matching singleton.
            if (strcmp(td->name_buf, "void")   == 0) return ty_void();
            if (strcmp(td->name_buf, "i8")     == 0) return ty_i8();
            if (strcmp(td->name_buf, "i16")    == 0) return ty_i16();
            if (strcmp(td->name_buf, "i32")    == 0) return ty_i32();
            if (strcmp(td->name_buf, "i64")    == 0) return ty_i64();
            if (strcmp(td->name_buf, "u8")     == 0) return ty_u8();
            if (strcmp(td->name_buf, "u16")    == 0) return ty_u16();
            if (strcmp(td->name_buf, "u32")    == 0) return ty_u32();
            if (strcmp(td->name_buf, "u64")    == 0) return ty_u64();
            if (strcmp(td->name_buf, "f32")    == 0) return ty_f32();
            if (strcmp(td->name_buf, "f64")    == 0) return ty_f64();
            if (strcmp(td->name_buf, "bool")   == 0) return ty_bool();
            if (strcmp(td->name_buf, "string") == 0) return ty_string();
            return NULL;
    }
    return NULL;
}

// ============================================================
// Type expression resolution
//
// The public entry (type_expr_resolve) forwards to the static
// recursive worker (resolve_inner). There is no additional
// setup; the split is only so the worker can call itself
// without a forward declaration inside its own body.
//
// The worker does not memoize. The same TypeExpr resolved
// twice will produce two distinct Type pointers for compound
// kinds (and the same pointer for primitives, which are
// singletons). type_eq handles the resulting non-identity
// via structural comparison, so callers must use type_eq
// rather than `==` when comparing resolved types.
// ============================================================
static Type* resolve_inner(TypeExpr* te, const SourceMap* smap,
                           TypeDefTable* types,
                           const ModuleTable* modtable,
                           uint32_t file_id,
                           void* ctx,
                           DiagnosticBag* bag);

Type* type_expr_resolve(TypeExpr* te, const SourceMap* smap,
                        TypeDefTable* types,
                        const ModuleTable* modtable,
                        uint32_t file_id,
                        void* ctx,
                        DiagnosticBag* bag) {
    return resolve_inner(te, smap, types, modtable, file_id, ctx, bag);
}

// The recursive resolver. Reads a TypeExpr, returns the
// corresponding Type*, or NULL (with diagnostics) on failure.
//
// Per-kind behaviour:
//
// TEX_RESOLVED:
//   Trivial: return the stored Type*. Used by the
//   monomorphizer to splice in a concrete type without
//   re-resolving.
//
// TEX_GENERIC:
//   Delegate to `types->instantiate_generic`, the callback
//   sema installed. Without that callback (types == NULL or
//   the field is unset) it is an error: generic instantiation
//   is only available in contexts where sema has set up the
//   monomorphizer. The `ctx` parameter is passed through
//   unchanged; sema uses it to resolve the generic arguments
//   in the enclosing function's environment.
//
// TEX_NAME:
//   Four-step lookup:
//     1. Primitives — a hardcoded string compare against
//        the well-known names. This is why a user cannot
//        shadow `i64` with their own type: the primitive
//        check comes first.
//     2. Current file's local types — typedef_table_find_in_file
//        with `file_id`. This is the common case for a type
//        defined and used in the same source file.
//     3. Selective imports — walk the enclosing file's
//        FileNamespace.selective[] looking for an entry whose
//        local_name matches. If found, look up the source
//        name in the target file (recorded in the entry) and
//        use that. This is the only place `using a.b.{x}`
//        imports are resolved.
//     4. Failure: report "unknown type" and return NULL.
//   Note that module-qualified `using a.b as c` imports do
//   NOT contribute names here; those require the explicit
//   `c.TypeName` syntax (TEX_QUALIFIED).
//
// TEX_QUALIFIED:
//   Only `a.b` (two components) is supported today; longer
//   paths are an explicit error. The first component is a
//   module alias; the second is a type name inside that
//   module. Resolution:
//     1. Find the enclosing file's FileNamespace.
//     2. Look up `a` in its imports[] to get target_file.
//     3. typedef_table_find_in_file(types, b, target_file).
//     4. If found, require is_pub on the TypeDef and return
//        the corresponding struct or enum Type.
//   Three failure modes:
//     - `a` is not an imported module alias.
//     - `a` is a module but has no type `b`.
//     - `b` exists but is not `pub`.
//   The "declared in <file>" message in the not-pub case
//   comes from the TypeDef's own name span, which is a nice
//   side effect of storing a real Token rather than a bare
//   string.
//
// TEX_REF / TEX_RC / TEX_RAWPTR / TEX_ARRAY:
//   Resolve `inner`, then check type_contains_ref(inner).
//   Each kind has its own message ("a reference cannot
//   contain another reference", "rc<T> cannot contain a
//   reference", etc.) but the check is the same predicate.
//   The rule exists so that a ref/rc/rawptr cannot form a
//   cycle of references; see the design notes elsewhere.
//   On success, wrap `inner` in the appropriate compound
//   constructor.
//
// Type equality is never consulted here; the resolver always
// creates a new compound Type. That is why the same source
// type written twice yields two distinct Type* objects, and
// why callers must use type_eq rather than `==`.
static Type* resolve_inner(TypeExpr* te, const SourceMap* smap,
                           TypeDefTable* types,
                           const ModuleTable* modtable,
                           uint32_t file_id,
                           void* ctx,
                           DiagnosticBag* bag) {
    if (!te) return NULL;
    switch (te->kind) {
        case TEX_RESOLVED:
            return te->resolved;

        case TEX_GENERIC:
            // Sema installs instantiate_generic on the table
            // when it sets up the monomorphizer. If the
            // callback is missing we are in a context that
            // does not support generics (e.g. a standalone
            // tool that calls type_expr_resolve without sema's
            // setup), and the right thing is a clear error.
            if (!types || !types->instantiate_generic) {
                diag_error(bag, te->span,
                    "generic instantiation is not available in this context");
                return NULL;
            }
            return types->instantiate_generic(types->generics, te, smap,
                                              ctx, file_id, bag);


            case TEX_NAME: {
                char buf[64];
                smap_span_text(smap, te->name.span, buf, sizeof buf);
                // Primitives
                //
                // Hardcoded string comparison. These are the only
                // names that resolve even when `types` is NULL,
                // which is why the primitive check runs before any
                // table lookup.
                if (strcmp(buf, "i8")     == 0) return ty_i8();
                if (strcmp(buf, "i16")    == 0) return ty_i16();
                if (strcmp(buf, "i32")    == 0) return ty_i32();
                if (strcmp(buf, "i64")    == 0) return ty_i64();
                if (strcmp(buf, "u8")     == 0) return ty_u8();
                if (strcmp(buf, "u16")    == 0) return ty_u16();
                if (strcmp(buf, "u32")    == 0) return ty_u32();
                if (strcmp(buf, "u64")    == 0) return ty_u64();
                if (strcmp(buf, "f32")    == 0) return ty_f32();
                if (strcmp(buf, "f64")    == 0) return ty_f64();
                if (strcmp(buf, "bool")   == 0) return ty_bool();
                if (strcmp(buf, "string") == 0) return ty_string();
                if (strcmp(buf, "void")   == 0) return ty_void();
    
                // Everything else goes through the resolver. It
                // handles this-file declarations and selective
                // imports in one place; the caller does not need
                // to know which is which.
                SemaCtx* sc = (SemaCtx*)ctx;
                if (sc) {
                    ResolvedDecl rd = resolve_name(sc, file_id, te->name);
                    if (rd.kind == RD_TYPE) {
                        TypeDef* td = &sc->structs->items[rd.index];
                        return type_for_tdef(td);
                    }
                    if (rd.kind == RD_TEMPLATE) {
                        // A bare template name without arguments.
                        // This is an error in a type position
                        // (generics need `<...>`), but sema may
                        // not have caught it yet; report it here.
                        diag_error(bag, te->span,
                            "generic type '%s' requires type arguments",
                            buf);
                        return NULL;
                    }
                }
    
                diag_error(bag, te->span, "unknown type '%s'", buf);
                return NULL;
            }

            case TEX_QUALIFIED: {
                if (te->path_count != 2) {
                    diag_error(bag, te->span,
                        "type paths with more than two components "
                        "are not yet supported");
                    return NULL;
                }
                Token mod_token  = te->path[0];
                Token type_token = te->path[1];
    
                SemaCtx* sc = (SemaCtx*)ctx;
                if (!sc) {
                    diag_error(bag, te->span,
                        "internal: qualified type resolved without "
                        "a sema context");
                    return NULL;
                }
                ResolvedDecl rd = resolve_qualified(sc, file_id,
                                                    mod_token, type_token);
                if (rd.kind != RD_TYPE) {
                    char mbuf[64], tbuf[64];
                    smap_span_text(smap, mod_token.span, mbuf, sizeof mbuf);
                    smap_span_text(smap, type_token.span, tbuf, sizeof tbuf);
                    diag_error(bag, te->span,
                        "'%s.%s' is not a type", mbuf, tbuf);
                    return NULL;
                }
                TypeDef* td = &sc->structs->items[rd.index];
                if (!td->is_pub) {
                    const char* def_path = "<unknown>";
                    SourceFile* sf = smap_get(smap, td->name.span.file_id);
                    if (sf) def_path = sf->path;
                    char tbuf[64];
                    smap_span_text(smap, type_token.span, tbuf, sizeof tbuf);
                    diag_error(bag, te->span,
                        "type '%s' is private; declared in %s and "
                        "not marked 'pub'", tbuf, def_path);
                    return NULL;
                }
                return type_for_tdef(td);
            }

        // The four "wrapper" kinds. Each resolves its inner
        // type, rejects the resulting type if it transitively
        // contains a ref, then wraps. The messages differ by
        // kind because each context has a slightly different
        // reason for the restriction, but the predicate is the
        // same.
        case TEX_REF: {
            Type* inner = resolve_inner(te->inner, smap, types, modtable,
                                        file_id, ctx, bag);
            if (!inner) return NULL;

            if (inner->kind == TY_EXTERN_STRUCT) {
                diag_error(bag, te->span, 
                    "ref<T> cannot contain an extern struct: "
                    "use *T to refer to raw/external ffi memory"); 
                return NULL;
            }
            
            if (type_contains_ref(inner)) {
                diag_error(bag, te->span,
                    "a reference cannot contain another reference");
                return NULL;
            }
            return ty_ref(inner, te->is_mut);
        }
        case TEX_RC: {
            Type* inner = resolve_inner(te->inner, smap, types, modtable,
                                        file_id, ctx, bag);
            if (!inner) return NULL;
            if (inner->kind == TY_EXTERN_STRUCT) {
                diag_error(bag, te->span, 
                    "rc<T> cannot contain an extern struct: "
                    "C memory has no ref_count header "
                    "use *T to refer to raw/external ffi memory"); 
                return NULL;
            }           
            if (type_contains_ref(inner)) {
                diag_error(bag, te->span, "rc<T> cannot contain a reference");
                return NULL;
            }
            return ty_rc(inner);
        }
        case TEX_RAWPTR: {
            Type* inner = resolve_inner(te->inner, smap, types, modtable,
                                        file_id, ctx, bag);
            if (!inner) return NULL;
            if (type_contains_ref(inner)) {
                diag_error(bag, te->span,
                    "a raw pointer cannot point to a reference type");
                return NULL;
            }
            return ty_rawptr(inner);
        }
        case TEX_ARRAY: {
            Type* inner = resolve_inner(te->inner, smap, types, modtable,
                                        file_id, ctx, bag);
            if (!inner) return NULL;
            
            if (inner->kind == TY_EXTERN_STRUCT) {
                diag_error(bag, te->span, 
                    "array element type cannot contain an extern struct: "
                    "use [*T] for an array of pointers "); 
                return NULL;
            } 
            if (type_contains_ref(inner)) {
                diag_error(bag, te->span,
                    "array element type cannot be a reference");
                return NULL;
            }
            return ty_array(inner);
        }
        case TEX_FN: {
            // Function types are not generic (they carry no
            // type variables), so this branch does not
            // delegate to the monomorphizer. It just resolves
            // the parameter and return TypeExprs.
            int n = te->param_count;
            if (n > 16) {
                diag_error(bag, te->span,
                    "function type has too many parameters (max 16)");
                return NULL;
            }
            Type* params[16];
            for (int i = 0; i < n; i++) {
                params[i] = resolve_inner(te->params[i], smap, types,
                                          modtable, file_id, ctx, bag);
                if (!params[i]) return NULL;
            }
            Type* ret = resolve_inner(te->ret, smap, types,
                                      modtable, file_id, ctx, bag);
            if (!ret) return NULL;
            return ty_fn(params, n, ret);
        }
        
    }
    return NULL;
}

// ============================================================
// Runtime type descriptors
// ============================================================

// Zero a TypeTable. All three sub-arrays start NULL.
void typetable_init(TypeTable* tt) {
    tt->items = NULL; tt->count = 0; tt->capacity = 0;
    tt->fields = NULL; tt->field_count = 0; tt->field_capacity = 0;
    tt->enum_variants = NULL;
    tt->enum_variant_count = 0;
    tt->enum_variant_capacity = 0;
    tt->enum_payloads = NULL;
    tt->enum_payload_count = 0;
    tt->enum_payload_capacity = 0;
    
}

// Free all three sub-arrays and reset. Note there is nothing
// to free inside a TypeDesc or EnumVariantDesc; they are
// plain-old-data with no owned pointers. The Type* they
// *refer to* (via aux indices) live in the compiler's type
// universe, not here.
void typetable_free(TypeTable* tt) {
    free(tt->items);
    free(tt->fields);
    free(tt->enum_variants);
    free(tt->enum_payloads);
    typetable_init(tt);
}

// Map a TypeKind to a TypeDescKind. Total: every TypeKind
// has a descriptor counterpart. NULL and unknown kinds fall
// through to TD_I64 (see the note in typetable_intern about
// the NULL default).
static TypeDescKind desc_kind_of(const Type* t) {
    if (!t) return TD_I64;
    switch (t->kind) {
        case TY_VOID:   return TD_VOID;
        case TY_I8:     return TD_I8;
        case TY_I16:    return TD_I16;
        case TY_I32:    return TD_I32;
        case TY_I64:    return TD_I64;
        case TY_U8:     return TD_U8;
        case TY_U16:    return TD_U16;
        case TY_U32:    return TD_U32;
        case TY_U64:    return TD_U64;
        case TY_F32:    return TD_F32;
        case TY_F64:    return TD_F64;
        case TY_BOOL:   return TD_BOOL;
        case TY_STRING: return TD_STRING;
        case TY_REF:    return TD_REF;
        case TY_RC:     return TD_RC;
        case TY_RAWPTR: return TD_RAWPTR;
        case TY_ARRAY:  return TD_ARRAY;
        case TY_STRUCT: return TD_STRUCT;
        case TY_EXTERN_STRUCT: return TD_EXTERN_STRUCT;
        case TY_FN:    return TD_I64;   // function value is a u32 index
        case TY_ENUM:   return TD_ENUM;
    }
    return TD_I64;
}

// Does the descriptor `d` describe the type `t`?
//
// This is the equality predicate used by typetable_intern to
// find an existing descriptor before appending a new one. It
// is structural, not pointer-based: two distinct Type*
// objects with the same shape map to the same descriptor.
//
// Step by step:
//   1. Kind check first. Two types of different kinds never
//      share a descriptor.
//   2. If `t` is NULL, the descriptor is the default TD_I64
//      (see typetable_intern's NULL handling). The kind
//      check above already guaranteed d->kind == TD_I64,
//      so return true. This makes the very first intern of
//      NULL produce an intern of TD_I64 for any later NULL.
//   3. Per-kind comparison:
//      - REF / RC / RAWPTR / ARRAY: compare the interned
//        inner index (d->aux) against the interned inner of
//        t. Note this recursively interns t->inner — which
//        has the side effect of adding inner descriptors if
//        they do not exist yet. That is fine; interning is
//        idempotent.
//      - STRUCT: count must match, and then every field's
//        interned descriptor index must match the
//        corresponding entry in tt->fields starting at
//        d->aux.
//      - ENUM: count must match, and every variant name
//        must match strcmp-for-strcmp against the
//        corresponding entry in tt->enum_variants starting
//        at d->aux.
//      - Everything else (all scalars): kind match is
//        sufficient, return true.
//
// Note that for STRUCT and ENUM the comparison uses interned
// indices and strcmp, not pointer equality, so two
// structurally identical structs share one descriptor.
static bool desc_matches(TypeTable* tt, const TypeDesc* d, const Type* t) {
    if (d->kind != desc_kind_of(t)) return false;
    if (!t) return true;
    switch (t->kind) {
        case TY_REF:
        case TY_RC:
        case TY_RAWPTR:
        case TY_ARRAY: {
            uint32_t inner = typetable_intern(tt, t->inner);
            return d->aux == inner;
        }
        case TY_STRUCT:
        case TY_EXTERN_STRUCT: {
            if (d->count != (uint32_t)t->tdef->field_count) return false;
            for (int i = 0; i < t->tdef->field_count; i++) {
                uint32_t want = typetable_intern(tt, t->tdef->fields[i].type);
                if (tt->fields[d->aux + i] != want) return false;
            }
            return true;
        }
        case TY_ENUM: {
            if (d->count != (uint32_t)t->tdef->variant_count) return false;
            for (int i = 0; i < t->tdef->variant_count; i++) {
                const EnumVariantDesc* stored = &tt->enum_variants[d->aux + i];
                if (strcmp(stored->name_buf, t->tdef->variants[i].name_buf) != 0)
                    return false;
                // NEW: payload descriptors must match too.
                if (stored->payload_count
                    != (uint32_t)t->tdef->variants[i].payload_count)
                    return false;
                for (uint32_t p = 0; p < stored->payload_count; p++) {
                    uint32_t want = typetable_intern(
                        tt, t->tdef->variants[i].payloads[p]);
                    if (tt->enum_payloads[stored->payload_start + p] != want)
                        return false;
                }
            }
            return true;
        }
        default:
            return true;
    }
}

// Intern a type and return its descriptor index.
//
// Two-phase algorithm:
//
//   Phase 1 (search): walk the existing descriptors and
//   return the first whose desc_matches describes `t`. Because
//   desc_matches is structural, this is a linear scan over the
//   descriptor array; the array is small.
//
//   Phase 2 (insert): no match. Append a new descriptor slot,
//   set its kind / size / count, then fill in kind-specific
//   payload:
//     - NULL type: `aux` stays 0, count stays 0. Slot is the
//       default TD_I64 descriptor. The early return skips
//       the rest of phase 2.
//     - Wrapper kinds: recursively intern `t->inner` and
//       store its index in `aux`.
//     - STRUCT: for each field, recursively intern the field
//       type and append the index to tt->fields; record the
//       start index in `aux` and the field count in `count`.
//       Nested field descriptors may cause further appends
//       to tt->items and tt->fields, but that is fine: the
//       start index `first` was captured before the loop.
//     - ENUM: for each variant, intern each payload type
//       (for the side effect of making descriptors exist),
//       then append the variant name to tt->enum_variants.
//       Record the start index in `aux` and the variant
//       count in `count`.
//
// Note the order of the `tt->count++` and the subsequent
// recursive interning. The new slot is recorded *before*
// recursing, so if interning an inner type triggers another
// intern that somehow refers back (it cannot today, but the
// pattern is defensive), the slot already exists. Also,
// because the slot's kind is set before recursion, a
// recursive call's phase-1 scan will see this slot; but its
// desc_matches will fail (aux is still 0, which will not
// match a non-default inner), so the recursive call appends
// a new slot for its own type. This is the intended
// behaviour: no aliasing, no cycles.
//
// The `idx` returned is stable for the lifetime of the
// TypeTable: descriptors are only appended, never moved by
// index, and never removed until typetable_free.
uint32_t typetable_intern(TypeTable* tt, const Type* t) {
    // Phase 1: linear search for an existing descriptor.
    for (int i = 0; i < tt->count; i++)
        if (desc_matches(tt, &tt->items[i], t)) return (uint32_t)i;

    // Phase 2: no match. Grow if needed and append a slot.
    if (tt->count >= tt->capacity) {
        tt->capacity = tt->capacity ? tt->capacity * 2 : 8;
        tt->items = (TypeDesc*)xrealloc(
            tt->items, sizeof(TypeDesc) * (size_t)tt->capacity);
    }

    // Capture the new slot's index, then increment count.
    // After this point, `idx` is the descriptor we are about
    // to fill in.
    int idx = tt->count;
    tt->items[idx].kind  = desc_kind_of(t);
    tt->items[idx].aux   = 0;
    tt->items[idx].size  = t ? (uint32_t)t->size : 0;
    tt->items[idx].count = 0;
    tt->count++;

    // NULL type: the slot is the default descriptor. Do not
    // recurse (there is no inner). The resulting index is
    // what any future NULL intern will find in phase 1.
    if (!t) return (uint32_t)idx;

    switch (t->kind) {
        case TY_REF:
        case TY_RC:
        case TY_RAWPTR:
        case TY_ARRAY:
            // Single inner: recurse, store the index.
            tt->items[idx].aux = typetable_intern(tt, t->inner);
            break;

            // Multi-field struct. `first` is the starting
            // position in tt->fields; we do not know how many
            // entries the loop will add until we run it, but
            // we do know it will be exactly field_count of
            // them (each iteration appends exactly one).
            //
            // The capacity growth inside the loop mirrors the
            // outer pattern and must stay in sync with the
            // layout expectation in desc_matches: entries
            // for one struct are contiguous starting at
            // `aux`.
            case TY_STRUCT:
            case TY_EXTERN_STRUCT: {
                uint32_t first = (uint32_t)tt->field_count;
                for (int i = 0; i < t->tdef->field_count; i++) {
                    uint32_t fd = typetable_intern(tt, t->tdef->fields[i].type);
                    if (tt->field_count >= tt->field_capacity) {
                        tt->field_capacity = tt->field_capacity
                                           ? tt->field_capacity * 2 : 16;
                        tt->fields = (uint32_t*)xrealloc(tt->fields,
                            sizeof(uint32_t) * (size_t)tt->field_capacity);
                    }
                    tt->fields[tt->field_count++] = fd;
                }
                tt->items[idx].aux   = first;
                tt->items[idx].count = (uint32_t)t->tdef->field_count;
                break;
            }

        case TY_ENUM: {
            uint32_t first = (uint32_t)tt->enum_variant_count;
        
            for (int i = 0; i < t->tdef->variant_count; i++) {
                const EnumVariantDef* vd = &t->tdef->variants[i];
                uint32_t pc = (uint32_t)vd->payload_count;
                if (pc > 16) pc = 16;   // defensive; sema caps at 16
        
                // Phase 1: intern each payload type into a local
                // buffer. This may recursively grow the items /
                // fields / enum_* arrays, so we do it before
                // taking any pointers into them.
                uint32_t local_pds[16];
                for (uint32_t p = 0; p < pc; p++)
                    local_pds[p] = typetable_intern(tt, vd->payloads[p]);
        
                // Phase 2: grow and populate the variant record.
                if (tt->enum_variant_count >= tt->enum_variant_capacity) {
                    tt->enum_variant_capacity = tt->enum_variant_capacity
                                              ? tt->enum_variant_capacity * 2 : 16;
                    tt->enum_variants = (EnumVariantDesc*)xrealloc(
                        tt->enum_variants,
                        sizeof(EnumVariantDesc) * (size_t)tt->enum_variant_capacity);
                }
                EnumVariantDesc* ev = &tt->enum_variants[tt->enum_variant_count++];
                strncpy(ev->name_buf, vd->name_buf, 63);
                ev->name_buf[63] = '\0';
                ev->payload_start = (uint32_t)tt->enum_payload_count;
                ev->payload_count = pc;
        
                // Phase 3: append payload descriptors to the flat
                // enum_payloads array. Only this array is realloc'd
                // in this phase, so `ev` (which points into
                // enum_variants) stays valid.
                for (uint32_t p = 0; p < pc; p++) {
                    if (tt->enum_payload_count >= tt->enum_payload_capacity) {
                        tt->enum_payload_capacity = tt->enum_payload_capacity
                                                  ? tt->enum_payload_capacity * 2 : 16;
                        tt->enum_payloads = (uint32_t*)xrealloc(
                            tt->enum_payloads,
                            sizeof(uint32_t) * (size_t)tt->enum_payload_capacity);
                    }
                    tt->enum_payloads[tt->enum_payload_count++] = local_pds[p];
                }
            }
            tt->items[idx].aux   = first;
            tt->items[idx].count = (uint32_t)t->tdef->variant_count;
            break;
        }

        default: break;
    }

    return (uint32_t)idx;
}

// Look up a descriptor by index. Bounds-checked; returns NULL
// for a NULL table or an out-of-range index. The returned
// pointer is borrowed and must not be freed or held across a
// call that might grow the table.
const TypeDesc* typetable_get(const TypeTable* tt, uint32_t idx) {
    if (!tt || idx >= (uint32_t)tt->count) return NULL;
    return &tt->items[idx];
}

static int type_size_bits(const Type* t) {
    switch (t->kind) {
        case TY_I8:  case TY_U8:  return 8;
        case TY_I16: case TY_U16: return 16;
        case TY_I32: case TY_U32: return 32;
        case TY_I64: case TY_U64: return 64;
        default: return 0;
    }
}

// Is there a safe implicit widening from `from` to `to`?
// "Safe" means every value representable as `from` is
// representable exactly as `to`. The set is closed and
// documented above the function; every relation here loses
// no information.
//
// This is not a general subtyping relation. It is used only
// at binding sites (let, assign, argument, return, field,
// element), never inside expressions. `a + b` still requires
// both operands to have the same type.


bool type_widens_to(const Type* from, const Type* to) {
    if (!from || !to) return false;
    if (type_eq((Type*)from, (Type*)to)) return true;
    
    // Integer → integer.
    if (is_integer_type((Type*)from) && is_integer_type((Type*)to)) {
        // Table of from -> set of valid targets.
        // Encoded as: how many bits does `from` need, and is
        // it signed? A signed-from can widen to a signed-to of
        // strictly more bits. An unsigned-from can widen to any
        // target whose bit count is strictly greater than the
        // source's, except it cannot exceed the target's signed
        // capacity without going through a signed type of the
        int fb = type_size_bits(from);
        int tb = type_size_bits(to);
        if (fb >= tb) return false;
        bool fs = is_signed_int((Type*)from);
        bool ts = is_signed_int((Type*)to);
        if (fs) return ts;   /* signed → signed only */
        return true;         /* unsigned → signed or unsigned */
    }

    if (is_integer_type((Type*)from) && is_float_type((Type*)to)) {
        int fb = type_size_bits(from);
        if (to->kind == TY_F32) return fb <= 24;
        if (to->kind == TY_F64) return fb <= 53;
        return false;
    }

    if (is_float_type((Type*)from) && is_float_type((Type*)to))
        return from->kind == TY_F32 && to->kind == TY_F64;

    return false;
}