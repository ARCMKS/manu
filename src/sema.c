// It's a beast but in practical terms, it's composed of things that now deserve to be in individual files
// 1. Module namespace:         lines ~200 - ~450
// 2. FFI construction:         lines ~450 - ~750
// 3. Type registration:        lines ~750 - ~1400
// 4. Monomorphizer bridge:     lines ~1400 - ~1750
// 5. Body analysis:            lines ~1750 - ~5000

// ============================================================
// sema.c — Semantic analysis for Prism.
//
// Passes, in order:
//   Pass 0   Register nominal type names. Generic declarations
//            (with type parameters) are registered as templates
//            in the monomorphizer instead of the concrete table.
//   Pass 0b  Resolve struct fields and enum variant payloads.
//   Pass A   Function signatures and compile-time constants.
//            Functions with type parameters go to the template
//            table; the rest go to the function tabint mod_file_id = resolve_module_in_file(sc,le.
//   Pass B   Function bodies.
//
// Instantiation is on-demand: whenever a TEX_GENERIC type
// expression or an explicit `<...>` call is encountered, the
// monomorphizer deep-copies the template with substitution and
// the copy goes through Pass 0b/A/B like any other declaration.
// A global cache ensures one concrete instantiation per
// (template, type-args) pair.
//
// State is split between SemaCtx (per-compilation, stable
// across all bodies) and FnLocal (per-function, created and
// discarded by analyze_func_body). This makes nested calls
// reentrant-safe without save/restore.
//
// Reading order for this file, top to bottom:
//   1. Forward declarations.
//   2. Identifier / lookup helpers (find_func, module table).
//   3. build_module_table and validate_selective_imports —
//      run before Pass A so the resolver in types.c can find
//      imports when it resolves qualified names.
//   4. Instantiation cache and the two instantiate entry
//      points (sema_instantiate_generic as a callback from
//      types.c, and sema_instantiate_function called from
//      AST_CALL).
//   5. Local scope management.
//   6. analyze_expr — the largest function. It annotates each
//      expression node with a Type* on the node's `type`
//      field and (where relevant) sets resolution indices
//      such as func_idx / builtin_id / ffi_idx.
//   7. analyze_pattern and check_exhaustive.
//   8. analyze_stmt — the second largest. Annotates statements
//      and recursively descends into nested statements.
//   9. Pass 0 / 0b helpers (register_struct_or_enum,
//      resolve_struct_fields, resolve_enum_variants).
//  10. FFI table construction (build_library_table,
//      library_table_find, ffi_kind_from_type,
//      is_option_ptr_type, is_ffi_variadic_ok).
//  11. sema_analyze — the orchestrator.
// ============================================================
#include "../include/sema.h"
#include "../include/types.h"
#include "../include/constant.h"
#include "../include/diagnostics.h"
#include "../include/monomorph.h"
#include "../include/prism_ffi.h"
#include "../include/source_map.h"
#include "../include/resolve.h"
#include "../include/sema_internal.h"
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ------------------------------------------------------------
// Forward declarations
//
// These break the recursion cycle between the top-level
// analysis functions (analyze_expr calls analyze_stmt via
// nested constructs; analyze_stmt calls analyze_expr for
// conditions and initializers). They also let the compiler
// order the definitions naturally rather than forcing callees
// before callers.
// ------------------------------------------------------------
static Type* analyze_expr(SemaCtx* sc, FnLocal* fl, AstNode* n,
                          DiagnosticBag* bag);
static void  analyze_stmt(SemaCtx* sc, FnLocal* fl, AstNode* n,
                          DiagnosticBag* bag);

static void  analyze_pattern(SemaCtx* sc, FnLocal* fl, AstNode* p,
                             Type* T, DiagnosticBag* bag);
static void  check_exhaustive(Type* T, AstList* arms, Span where,
                              const SourceMap* smap, DiagnosticBag* bag);

// ============================================================
// Identifier helpers
// ============================================================

// Copy the source text of a token into a caller-supplied
// buffer, NUL-terminated and truncated to `cap`. Used by
// every diagnostic that wants to name an identifier; the
// alternative would be to spell out smap_span_text at each
// call site, which is noisier.
//
// No length is returned. Callers that need to know about
// truncation should call smap_span_text directly.
static void name_text(const SourceMap* smap, Token t, char* buf, size_t cap) {
    smap_span_text(smap, t.span, buf, cap);
}



// ============================================================
// Function lookup
// ============================================================

// Find a user function by name in the *same file* as the
// calling file. Skips instantiated signatures (their names
// are mangled and not user-visible), and skips functions in
// other files (cross-file reachability requires an import).
//
// The two filters are checked in order: `is_instantiated`
// first (cheap boolean), then `file_id` (cheap integer), then
// the byte-compare via smap_tokens_equal. The ordering is a
// hot-path optimization; the function is called once per
// call-expression.
static int find_func(const SemaCtx* sc, Token name, uint32_t file_id) {
    for (int i = 0; i < sc->func_count; i++) {
        if (sc->funcs[i].is_instantiated) continue;
        if (sc->funcs[i].file_id != file_id) continue;
        if (smap_tokens_equal(sc->smap, sc->funcs[i].name, name)) return i;
    }
    return -1;
}





// ============================================================
// Local scope management
// ============================================================

// Declare a named local. Returns the slot index (>= 0) or -1
// if the local table is full.
//
// Slots are assigned in declaration order and are stable for
// the lifetime of the frame. The `slot` field on the Local
// record is redundant with the array index but is stored
// anyway so call sites can read it from the record without
// also tracking the index.
//
// Note that this does NOT check for shadowing. Prism allows
// a local to shadow an outer local; lookup (see `resolve`)
// scans from the end, so the innermost binding wins. That
// is intentional.
static int declare(FnLocal* fl, Token name, Type* type, bool holds_rc, bool is_mut) {
    if (fl->local_count >= MAX_LOCALS) return -1;
    int slot = fl->local_count;
    fl->locals[slot].name     = name;
    fl->locals[slot].type     = type;
    fl->locals[slot].slot     = slot;
    fl->locals[slot].holds_rc = holds_rc;
    fl->locals[slot].is_mut   = is_mut;
    fl->local_count++;
    return slot;
}

// Declare an anonymous local. Used for compiler-generated
// temporaries: pattern binds' temporary payload slots, the
// cached end value of a for-range, the cached scrutinee of
// a match, the cached array/length/index for a for-in.
//
// The name is a zero-valued Token (file_id == 0), which
// means name lookup can never match it. is_mut is set to
// true so compiler-generated writes (e.g. incrementing the
// for-in index) do not trip the mutability check — those
// writes happen from codegen, not from source, but the same
// check is what would reject them if any pass looked.
static int declare_anon(FnLocal* fl, Type* type) {
    if (fl->local_count >= MAX_LOCALS) return -1;
    int slot = fl->local_count;
    Token anon = {0};
    fl->locals[slot].name     = anon;
    fl->locals[slot].type     = type;
    fl->locals[slot].slot     = slot;
    fl->locals[slot].holds_rc = false;
    fl->locals[slot].is_mut   = true;
    fl->local_count++;
    return slot;
}

// Resolve a name to a local slot index, or -1 if not found.
// Scans from the end backwards so that an inner shadowing
// binding is preferred over an outer one. Linear scan; the
// local count per function is small.
static int resolve(FnLocal* fl, const SourceMap* smap, Token name) {
    for (int i = fl->local_count - 1; i >= 0; i--)
        if (smap_tokens_equal(smap, fl->locals[i].name, name)) return i;
    return -1;
}

// Whether an lvalue expression may be written to.
//
// This is a *shallow* check: it walks down through field
// accesses, indexings, and derefs to find the base binding,
// then checks whether that binding (or a ref it passes
// through) is mutable. It does not check whether the field
// or index is writable, and it does not check the
// intermediate derefs' mutability beyond the base. Those
// checks happen at the specific assignment site (see the
// AST_ASSIGN cases in analyze_stmt).
//
// Recursive structure:
//   AST_VAR    — base case. The binding must be mutable, or
//                its type must be ref<mut T> (writes through
//                a mutable reference are allowed even when
//                the reference itself is bound immutably).
//   AST_FIELD  — recurse into the target.
//   AST_INDEX  — recurse into the target.
//   AST_DEREF  — a deref is mutable iff its operand is a
//                mutable reference. Note this is a *different*
//                check from the base case: the deref rule
//                applies to `*x = ...` where x is a ref.
//   default    — not an lvalue; return false.
static bool is_mutable_lvalue(SemaCtx* sc, FnLocal* fl, AstNode* n) {
    if (!n) return false;
    switch (n->kind) {
        case AST_VAR: {
            int i = resolve(fl, sc->smap, n->as.var.name);
            if (i < 0) return false;
            if (fl->locals[i].is_mut) return true;
            Type* t = fl->locals[i].type;
            if (!t) return false;
            // Writable-through: ref<mut T>, *T, and rc<T>.
            // A plain ref<T> is read-only.
            return (t->kind == TY_REF && t->is_mut)
                || t->kind == TY_RAWPTR
                || t->kind == TY_RC;
        }
        case AST_FIELD:
            return is_mutable_lvalue(sc, fl, n->as.field.target);
        case AST_INDEX:
            return is_mutable_lvalue(sc, fl, n->as.index.target);
        case AST_DEREF: {
            Type* t = n->as.deref.operand->type;
            if (!t) return false;
            return (t->kind == TY_REF && t->is_mut)
                || t->kind == TY_RAWPTR
                || t->kind == TY_RC;
        }
        default:
            return false;
    }
}

// Find the root binding name of an lvalue, for diagnostics.
// Used by analyze_stmt to name the binding in "cannot assign
// to X" messages, when X is a field or index of a binding.
//
// Walks through the same shapes as is_mutable_lvalue until it
// hits an AST_VAR, whose token it returns. Returns a
// zero-valued Token if the walk fails (which cannot happen
// for a well-formed lvalue, but is defensive).
static Token lvalue_root_name(AstNode* n) {
    while (n) {
        switch (n->kind) {
            case AST_VAR:   return n->as.var.name;
            case AST_FIELD: n = n->as.field.target; continue;
            case AST_INDEX: n = n->as.index.target; continue;
            case AST_DEREF: n = n->as.deref.operand; continue;
            default:        return (Token){0};
        }
    }
    return (Token){0};
}

// Enter a scope. Records the current local_count so that
// exit_scope can restore it. Returns false (with a
// diagnostic) if the scope stack is full; the caller is
// expected to skip the body in that case.
//
// Note that entering a scope does NOT allocate any locals.
// It just records a mark; declarations after the mark
// allocate slots normally, and exit_scope rolls them back.
static bool enter_scope(FnLocal* fl, DiagnosticBag* bag, Span where) {
    if (fl->scope_depth >= MAX_SCOPES) {
        diag_error(bag, where,
                   "too many nested scopes (max %d)", MAX_SCOPES);
        return false;
    }
    fl->scope_first[fl->scope_depth] = fl->local_count;
    fl->scope_depth++;
    return true;
}

// Exit a scope, restoring local_count to the value it had
// when the scope was entered. Does not free anything; the
// Local records past the restored count are dead but still
// allocated (they will be overwritten by future
// declarations).
//
// The `if (scope_depth > 0)` guard is defensive: entering a
// scope that failed (returned false) followed by an exit
// would underflow. Callers are expected to only exit scopes
// they entered, but the guard costs nothing.
static void exit_scope(FnLocal* fl) {
    if (fl->scope_depth > 0) {
        fl->scope_depth--;
        fl->local_count = fl->scope_first[fl->scope_depth];
    }
}

// True iff `n` is a bare numeric literal that has not yet been
// given a type. Such a node can take on a context-supplied type
// without conflicting with an already-resolved type. Used by the
// bidirectional literal-adaptation pattern: when two operands
// (or a range bound and its sibling, or an array element and its
// element type) meet, the non-literal side is analyzed first and
// the literal side is pre-typed to match.
//
// AST_INT and AST_FLOAT are the two shapes that can adapt. A
// negated literal (-5) is AST_UNARY and is *not* considered
// adaptive here; the caller that needs to handle negation uses
// pre_type_literal (which walks through the unary).
static bool is_adaptive_literal(const AstNode* n) {
    if (!n) return false;
    return (n->kind == AST_INT || n->kind == AST_FLOAT) && !n->type;
}

// True iff `t` is a float type that a float literal can adopt.
// Both f32 and f64 accept a bare literal; the VM stores f32
// values in the low 32 bits of a slot, and codegen emits the
// right width based on the literal's resolved type.
static bool is_float_target(Type* t) {
    return t && (t->kind == TY_F64 || t->kind == TY_F32);
}


// Human-readable form of a binary operator, for diagnostics.
static const char* binop_name(BinOp op) {
    switch (op) {
        case BIN_ADD: return "+";
        case BIN_SUB: return "-";
        case BIN_MUL: return "*";
        case BIN_DIV: return "/";
        case BIN_EQ:  return "==";
        case BIN_NE:  return "!=";
        case BIN_LT:  return "<";
        case BIN_LE:  return "<=";
        case BIN_GT:  return ">";
        case BIN_GE:  return ">=";
        case BIN_AND: return "&&";
        case BIN_OR:  return "||";
    }
    return "?";
}

static const char* unop_name(UnOp op) {
    switch (op) {
        case UN_NEG: return "-";
        case UN_NOT: return "!";
    }
    return "?";
}

// The type produced by `op` applied to two operands of type
// `t`, or NULL if the operation is not defined for `t`.
//
// Single source of truth for the whole language's binary
// operator rules. Both AST_BINARY and AST_ASSIGN_OP go
// through this.
static Type* binary_result_type(BinOp op, Type* t) {
    if (!t) return NULL;
    switch (op) {
        case BIN_ADD:
            if (is_integer_type(t))   return t;
            if (is_float_type(t))     return t;
            if (t->kind == TY_STRING) return ty_string();
            return NULL;
        case BIN_SUB:
        case BIN_MUL:
        case BIN_DIV:
            if (is_integer_type(t))   return t;
            if (is_float_type(t))     return t;
            return NULL;
        case BIN_EQ:
        case BIN_NE:
            return is_equatable_type(t) ? ty_bool() : NULL;
        case BIN_LT:
        case BIN_LE:
        case BIN_GT:
        case BIN_GE:
            return is_ordered_type(t) ? ty_bool() : NULL;
        case BIN_AND:
        case BIN_OR:
            return (t->kind == TY_BOOL) ? ty_bool() : NULL;
    }
    return NULL;
}

// Same for unary operators.
static Type* unary_result_type(UnOp op, Type* t) {
    if (!t) return NULL;
    switch (op) {
        case UN_NEG:
            if (is_signed_int(t)) return t;
            if (is_float_type(t)) return t;
            return NULL;
        case UN_NOT:
            return (t->kind == TY_BOOL) ? ty_bool() : NULL;
    }
    return NULL;
}


// Coerce the value expression stored at `*slot` to the target
// type, allowing safe implicit widening. Called at every site
// that checks "does this value fit this target?".
//
// Behavior:
//   - Same type: no-op, returns true.
//   - `got` widens to `target` and no runtime conversion is
//     needed (i8 → i64): no-op, returns true.
//   - `got` widens to `target` and a runtime conversion is
//     needed (i32 → f64, f32 → f64): replaces *slot with an
//     AST_CAST wrapping the original, returns true.
//   - No safe relation: reports "cannot implicitly convert"
//     and returns false.
//
// `span` is where the error points. `slot` is the caller's
// location that holds the expression; if a cast is inserted,
// the new node is written back through the slot so subsequent
// passes see it.
static bool coerce_to_target(AstNode** slot, Type* target, Type* got,
                             Span span, DiagnosticBag* bag) {
    if (!target || !got) return false;
    if (type_eq(target, got)) return true;

    if (!type_widens_to(got, target)) {
        diag_error(bag, span,
            "cannot implicitly convert %s to %s; "
            "use an explicit cast",
            type_name(got), type_name(target));
        return false;
    }

    // Widening is legal. Insert a runtime conversion only when
    // the representation actually changes.
    bool needs_conv =
        (is_integer_type(got) && is_float_type(target)) ||
        (is_float_type(got) && is_float_type(target) &&
         got->kind != target->kind);

    if (needs_conv) {
        AstNode* orig = *slot;
        AstNode* cast = ast_cast(orig,
            type_expr_resolved(target, orig->span),
            orig->span);
        cast->as.cast.target_type = target;
        cast->type = target;
        *slot = cast;
    }
    return true;
}

// Does `value_type` fit a field declared as `field_type`?
//
// In addition to plain equality, this accepts the nested
// extern struct case: a field declared `Inner` (an extern
// struct by value) is initialized by `Inner { ... }`, which
// produces `*Inner`. The pointer's pointee is the same
// extern struct TypeDef, so the value is the field's storage
// represented as an address; codegen copies the payload in.
static bool type_matches_field(Type* field_type, Type* value_type) {
    if (!field_type || !value_type) return false;
    if (type_eq(field_type, value_type)) return true;

    // Safe widening (i8 → i64, u32 → f64, f32 → f64, ...).
    if (type_widens_to(value_type, field_type)) return true;

    // Nested extern struct: pointer to the same extern struct.
    if (field_type->kind == TY_EXTERN_STRUCT
        && value_type->kind == TY_RAWPTR
        && value_type->inner
        && value_type->inner->kind == TY_EXTERN_STRUCT
        && value_type->inner->tdef == field_type->tdef)
        return true;

    return false;
}

// Same acceptance rule as type_matches_field, but if the
// match came from a safe widening that needs a runtime
// conversion (int → float, f32 → f64), the value at `*slot`
// is wrapped in an AST_CAST. `type_matches_field` remains the
// pure predicate; this function is the one the struct literal
// analyzer calls, because it needs the wrap.
//
// Returns true if the value fits (with or without a wrap),
// false otherwise. No diagnostic is reported here; the caller
// reports "field 'X' expects A, got B".
static bool coerce_to_field(AstNode** slot, Type* field_type,
                            Type* value_type) {
    if (!type_matches_field(field_type, value_type)) return false;
    if (type_eq(field_type, value_type)) return true;

    // Nested extern struct: no cast, codegen inlines the
    // payload.
    if (field_type->kind == TY_EXTERN_STRUCT) return true;

    // Safe widening. Insert a runtime conversion only when
    // the representation changes.
    bool needs_conv =
        (is_integer_type(value_type) && is_float_type(field_type)) ||
        (is_float_type(value_type) && is_float_type(field_type) &&
         value_type->kind != field_type->kind);

    if (needs_conv) {
        AstNode* orig = *slot;
        AstNode* cast = ast_cast(orig,
            type_expr_resolved(field_type, orig->span),
            orig->span);
        cast->as.cast.target_type = field_type;
        cast->type = field_type;
        *slot = cast;
    }
    return true;
}

// Propagate a target type into a literal expression. Called before
// analyze_expr in contexts where the expected type is known
// (annotations, parameter slots, struct fields). Handles bare integer
// and float literals, and their negation. Does nothing if the literal
// is out of range for the target, or if the expression is not a
// literal — the caller's own checks will handle those cases.
//
// Why this exists:
//   Without it, an integer literal would always be typed as
//   i64 (see the AST_INT case in analyze_expr). That would
//   make `let x: u8 = 5;` a type error ("i64 is not u8"),
//   and would reject `f(5)` where f takes u8. The fix is to
//   pre-set the literal's type before it is analyzed, so
//   that the literal adapts to its context.
//
// What it does NOT do:
//   - It does not touch expressions that are not literals or
//     negations of literals. A constant-folded expression
//     that ends up being an integer literal is not recognized
//     here.
//   - It does not error on out-of-range literals; it just
//     does not set the type, leaving analyze_expr to fall
//     back to i64 and (for annotated bindings) the enclosing
//     type check to catch the mismatch.
//   - It does not handle bool literals (they have a fixed
//     type) or string literals (same).
//
// The `!operand->type` check in the unary case is important:
// if the operand has already been typed (e.g. by an earlier
// pre_type_literal call), we do not overwrite it. That
// happens when the same expression is analyzed twice, which
// is possible if an outer pass has already visited it.
static void pre_type_literal(AstNode* e, Type* target) {
    if (!e || !target) return;

    if (e->kind == AST_INT && is_integer_type(target)) {
        // Set the target type unconditionally, even when the
        // value does not fit. The range check is performed by
        // analyze_expr's AST_INT case, which reports a focused
        // "literal N does not fit in T" and (see that case)
        // leaves the type set so the enclosing context does
        // not additionally report "expected T, got i64".
            e->type = target;
        return;
    }
    if (e->kind == AST_FLOAT && is_float_target(target)) {
        // Float literals are always f64 by default; only
        // reset them when the target is explicitly f64
        // (which is the same type, so this is a no-op in
        // practice today, but would matter if f32 were
        // ever added).
        e->type = target;
        return;
    }
    if (e->kind == AST_UNARY && e->as.unary.op == UN_NEG) {
        // Negated literal: `-5`. The AST represents this as
        // UN_NEG(int_lit), and the literal's value is
        // positive. To check fit, negate the literal's
        // value and check against the target.
        AstNode* operand = e->as.unary.operand;
        if (!operand || operand->type) return;
        if (operand->kind == AST_INT && is_integer_type(target)) {
            // Same reasoning as the AST_INT case above. The
            // AST_INT check will fire on the positive value;
            // the negation itself is checked by the AST_UNARY
            // case in analyze_expr.
            operand->type = target;
            return;
        }
        if (operand->kind == AST_FLOAT && is_float_target(target)) {
            operand->type = target;
            return;
        }
    }
    // Array literal in an array-typed context:
    // `let x: [i32] = [1, 2, 3]`. Each element is
    // pre-typed to the element type. Recursion handles
    // nested arrays naturally (`[[i32]]` recurses twice).
    if (e->kind == AST_ARRAY_LIT && target->kind == TY_ARRAY) {
        Type* elem = target->inner;
        if (!elem) return;
        for (int i = 0; i < e->as.array_lit.elements.count; i++)
            pre_type_literal(e->as.array_lit.elements.items[i], elem);
        return;
    }

    // `rc e` in an `rc<T>` context: propagate T into e.
    // This is what makes `let x: rc<[i32]> = rc [1, 2, 3]`
    // type-check.
    if (e->kind == AST_ALLOC_RC && target->kind == TY_RC) {
        pre_type_literal(e->as.alloc_rc.value, target->inner);
        return;
    }

    // `&e` / `&mut e` in a `ref<T>` context: propagate T
    // into e. The mutability bit lives on the AST node, not
    // on the target type, so we only propagate the inner
    // type here; sema's own check compares the full ref
    // types after the inner value is analyzed.
    if (e->kind == AST_ALLOC_ARENA && target->kind == TY_REF) {
        pre_type_literal(e->as.alloc_arena.value, target->inner);
        return;
    }
    // Struct literal in a struct-typed context:
    // `let p: Pair<i32> = Pair { ... }`. The annotation
    // provides the concrete TypeDef; store it on the node so
    // the struct-literal analyzer can use it after name
    // lookup fails to find a concrete type.
    if (e->kind == AST_STRUCT_LIT && target->kind == TY_STRUCT
        && target->tdef) {
        e->as.struct_lit.expected_type = target;
        return;
    }
    
}

// ============================================================
// Expression analysis
//
// analyze_expr is the largest function in the file. Its
// contract, applied uniformly to every node kind:
//
//   - On success, returns the node's resolved Type* and
//     writes that same Type* onto n->type. The two are always
//     equal; callers may use either.
//   - On failure, returns NULL and has reported at least one
//     diagnostic into the bag. Some branches return a "safe"
//     fallback type (usually i64) *and* report an error; the
//     distinction between "returned NULL" and "returned a
//     type but reported an error" is not load-bearing for
//     callers, who should always check the bag when they
//     care about correctness.
//   - Side effects on the node: n->type, plus any
//     resolution field the node kind uses (local_index,
//     const_id, holds_rc, builtin_id, func_idx, ffi_idx,
//     tag, offset, sdef_index, tdef_index, unit_tag, tier,
//     and the various slot fields). All of these are set
//     here or by a helper called from here; nothing in
//     later passes writes them.
//
// The function recurses through child expressions in
// evaluation order (left, then right for binary; operand
// first for unary; receiver first, then args for calls).
// That ordering matters for diagnostics: the first error
// reported is the leftmost in source order, which is what a
// user expects.
// ============================================================
static Type* analyze_expr(SemaCtx* sc, FnLocal* fl, AstNode* n,
                          DiagnosticBag* bag) {
    if (!n) return NULL;

    switch (n->kind) {
        // ----- Literals -----
        //
        // Bare literals default to i64 / f64 / bool / string /
        // *u8. The i64 default for integers is what
        // pre_type_literal overrides when the surrounding
        // context supplies a target type. If pre_type_literal
        // did not set a type, we set it here, then range-check
        // it.
        //
        // The range check on AST_INT is the only place the
        // compiler rejects an out-of-range literal *on its
        // own*, without a surrounding context. If the literal
        // was pre-typed to a small type (u8, say) and the
        // value does not fit, this reports. If it was not
        // pre-typed, it becomes i64 and any value fits, so
        // no error.
        case AST_INT:
            if (!n->type) n->type = ty_i64();
            if (is_integer_type(n->type)
                && !uint_value_fits(n->as.int_lit.value, n->type)) {
                diag_error(bag, n->span,
                    "literal %llu does not fit in %s",
                    (unsigned long long)n->as.int_lit.value,
                    type_name(n->type));
            }
            return n->type;

        case AST_FLOAT:
            if (!n->type) n->type = ty_f64();
            return n->type;

        case AST_BOOL:
            n->type = ty_bool();
            return n->type;

        case AST_STRING:
            n->type = ty_string();
            return n->type;

        // C-string literals are typed as *u8: a raw pointer to
        // bytes. This is what makes them FFI-compatible
        // (ffi_kind_from_type maps TY_RAWPTR to FFI_PTR).
        case AST_CSTRING:
            n->type = ty_rawptr(ty_u8());
            return n->type;

        // ----- Variable reference -----
        //
        // Resolution order: local first, then const, then
        // error. The local wins on a name collision, which is
        // what lets a local shadow a const.
        //
        // On a local hit, the node records the slot
        // (local_index), clears const_id, and copies the
        // binding's type and holds_rc flag. On a const hit,
        // the reverse: const_id set, local_index cleared,
        // type from the const entry, holds_rc false (consts
        // are not owned by the expression that reads them).
        case AST_VAR: {
            int i = resolve(fl, sc->smap, n->as.var.name);
            if (i >= 0) {
                n->local_index     = fl->locals[i].slot;
                n->const_id        = -1;
                n->as.var.func_idx = -1;
                n->type            = fl->locals[i].type;
                n->holds_rc        = fl->locals[i].holds_rc;
                return n->type;
            }

            int c = consts_resolve(sc->consts, sc->smap, n->as.var.name);
            if (c >= 0) {
                const ConstEntry* e = consts_get(sc->consts, c);
                n->local_index     = -1;
                n->const_id        = c;
                n->as.var.func_idx = -1;
                n->type            = e->type;
                n->holds_rc        = false;
                return n->type;
            }

            // A user function used as a value: `add` in an
            // expression position. `find_func` skips
            // instantiated generics, so this only fires for a
            // concrete user function.
            int fidx = find_func(sc, n->as.var.name, n->span.file_id);
            if (fidx >= 0) {
                FuncSig* sig = &sc->funcs[fidx];
                if (sig->is_extern) {
                    diag_error(bag, n->span,
                        "extern function '%s' cannot be used as a "
                        "value; externs are not in the codegen "
                        "function table", sig->name_buf);
                    return NULL;
                }
                if (!sig->return_type) {
                    diag_error(bag, n->span,
                        "cannot take the value of function '%s' "
                        "before its return type is known; add a "
                        "return type annotation or use it later",
                        sig->name_buf);
                    return NULL;
                }
                Type* fn_t = ty_fn(sig->param_types, sig->param_count,
                                   sig->return_type);
                n->local_index     = -1;
                n->const_id        = -1;
                n->as.var.func_idx = fidx;
                n->type            = fn_t;
                n->holds_rc        = false;
                return n->type;
            }

            // A generic function cannot be used as a value;
            // it has no single concrete instantiation. Catch
            // this specifically so the diagnostic names the
            // real problem rather than "undefined variable".
            {
                GenericTemplate* tmpl = mono_find_template(
                    sc->structs->generics, sc->smap, n->as.var.name);
                if (tmpl && tmpl->decl
                    && tmpl->decl->kind == AST_FUNC) {
                    char nbuf[64];
                    smap_span_text(sc->smap, n->as.var.name.span,
                                   nbuf, sizeof nbuf);
                    diag_error(bag, n->span,
                        "generic function '%s' must be instantiated "
                        "with explicit type arguments before being "
                        "used as a value", nbuf);
                    return NULL;
                }
            }
            

            char buf[128];
            name_text(sc->smap, n->as.var.name, buf, sizeof buf);
            diag_error(bag, n->span, "undefined variable '%s'", buf);
            return NULL;
        }

        // ----- Binary operators -----
        //
        // The general shape: evaluate both operands, then
        // dispatch on their types.
        //
        // The first block is a small literal-adaptation
        // optimization. When one operand is an untyped
        // numeric literal and the other is not, the
        // non-literal side is analyzed first and the literal
        // is pre-typed to that result. This is what makes
        // `x + 1` work when x is u8: the literal 1 adapts to
        // u8 rather than forcing the whole expression to be
        // i64 (which would then be a type mismatch). The same
        // trick is what allows `1 + x` to work, via the
        // symmetric branch.
        //
        // If both operands are literals, or neither is, they
        // are analyzed in source order and the resulting
        // types are compared by the dispatch below.
        //
        // Dispatch order and rules:
        //   1. string op string -> ADD yields string; comparisons
        //      yield bool; everything else is an error.
        //   2. string op non-string (either direction) -> error.
        //   3. rawptr op rawptr -> EQ/NE yield bool; others
        //      error.
        //   4. Otherwise both operands must have the same type
        //      (or one must have failed to resolve). If they
        //      differ, error and default to i64.
        //   5. If the unified type is f64 and the op is
        //      arithmetic, result is f64; if it is a
        //      comparison, result is bool.
        //   6. Otherwise the result is the unified type for
        //      arithmetic, bool for comparison.
        case AST_BINARY: {
            AstNode* L = n->as.binary.left;
            AstNode* R = n->as.binary.right;

            // Short-circuit logical operators: both operands
            // must be bool, and no literal adaptation applies
            // (bools are not adaptive). Evaluate in order.
            if (n->as.binary.op == BIN_AND || n->as.binary.op == BIN_OR) {
                Type* lt = analyze_expr(sc, fl, L, bag);
                Type* rt = analyze_expr(sc, fl, R, bag);
                if (lt && lt->kind != TY_BOOL)
                    diag_error(bag, L->span,
                        "expected bool, found %s", type_name(lt));
                if (rt && rt->kind != TY_BOOL)
                    diag_error(bag, R->span,
                        "expected bool, found %s", type_name(rt));
                n->type = ty_bool();
                return n->type;
            }

            // Literal adaptation. If one side is an untyped
            // numeric literal and the other is not, analyze
            // the non-literal first and pre-type the literal
            // to its type. This is what makes `x + 1` work
            // when x is u8 or f32.
            bool L_lit = is_adaptive_literal(L);
            bool R_lit = is_adaptive_literal(R);

            Type* lt;
            Type* rt;
            if (L_lit && !R_lit) {
                rt = analyze_expr(sc, fl, R, bag);
                pre_type_literal(L, rt);
                lt = analyze_expr(sc, fl, L, bag);
            } else if (R_lit && !L_lit) {
                lt = analyze_expr(sc, fl, L, bag);
                pre_type_literal(R, lt);
                rt = analyze_expr(sc, fl, R, bag);
            } else {
                lt = analyze_expr(sc, fl, L, bag);
                rt = analyze_expr(sc, fl, R, bag);
            }

            // Unify operand types. Both must be present and
            // equal, or one is missing (an already-reported
            // error) in which case the other is used.
            Type* unified = NULL;
            if (lt && rt) {
                if (type_eq(lt, rt)) {
                    unified = lt;
                } else {
                    // The string-vs-non-string case gets a more
                    // specific message; everything else uses
                    // the general one.
                    if ((lt->kind == TY_STRING) != (rt->kind == TY_STRING)) {
                        diag_error(bag, n->span,
                            "cannot combine a string with a non-string "
                            "operand: %s and %s",
                            type_name(lt), type_name(rt));
                    } else {
                        diag_error(bag, n->span,
                            "operands have different types: %s and %s",
                            type_name(lt), type_name(rt));
                    }
                    n->type = ty_i64();
                    return n->type;
                }
            } else if (lt) {
                unified = lt;
            } else if (rt) {
                unified = rt;
            } else {
                n->type = ty_i64();
                return n->type;
            }

            // Operator validity and result type come from the
            // single predicate.
            Type* result = binary_result_type(n->as.binary.op, unified);
            if (!result) {
                diag_error(bag, n->span,
                    "operator '%s' is not defined for %s",
                    binop_name(n->as.binary.op), type_name(unified));
                n->type = ty_i64();
                return n->type;
            }
            n->type = result;
            return n->type;
        }

        // ----- Unary operators -----
        //
        // Two operators: UN_NEG (arithmetic negation) and
        // UN_NOT (logical not). They have different type
        // requirements:
        //   UN_NOT: operand must be bool; result is bool.
        //   UN_NEG: operand must be a signed int or f64;
        //           result is the operand's type.
        //
        // Unsigned integer negation is rejected: the
        // language does not define wrapping negation for
        // unsigned values. (Users who want it can cast to a
        // signed type first.)
        case AST_UNARY: {
            Type* ot = analyze_expr(sc, fl, n->as.unary.operand, bag);
            if (!ot) {
                n->type = ty_i64();
                return n->type;
            }
            Type* result = unary_result_type(n->as.unary.op, ot);
            if (!result) {
                // Specific message for the unsigned-negation
                // case, which is the common mistake.
                if (n->as.unary.op == UN_NEG && is_unsigned_int(ot)) {
                    diag_error(bag, n->as.unary.operand->span,
                        "cannot negate unsigned type %s", type_name(ot));
                } else {
                    diag_error(bag, n->as.unary.operand->span,
                        "operator '%s' is not defined for %s",
                        unop_name(n->as.unary.op), type_name(ot));
                }
                n->type = ty_i64();
                return n->type;
            }
            n->type = result;
            return n->type;
        }

        // ----- Function call -----
        //
        // This is the second-largest branch in the function.
        // It handles, in order:
        //   1. Explicit generic calls: `f<T>(...)`.
        //   2. Builtins (a local table of names).
        //   3. User function calls.
        //   4. Extern function calls.
        //
        // The resolution order matters: builtins are checked
        // by name before the user-function table, so a
        // builtin name cannot be shadowed by a user function.
        // If that ever needs to change, the builtin lookup
        // would move after the user-function lookup.
        case AST_CALL: {
            Token nm = n->as.call.name;
            char nbuf[64];
            name_text(sc->smap, nm, nbuf, sizeof nbuf);

            // Function-value call: `fp(args)` where `fp` is a
            // local of type `fn(...)`. Checked before the
            // builtin table and the user-function table so a
            // local shadows anything that shares its name.
            int li = resolve(fl, sc->smap, nm);
            if (li >= 0
                && fl->locals[li].type
                && fl->locals[li].type->kind == TY_FN) {
                Type* fn_t = fl->locals[li].type;

                if (n->as.call.instantiation) {
                    diag_error(bag, n->span,
                        "cannot provide type arguments to a "
                        "function value");
                    type_expr_free(n->as.call.instantiation);
                    n->as.call.instantiation = NULL;
                }

                if (n->as.call.args.count != fn_t->fn_param_count)
                    diag_error(bag, n->span,
                        "function value expects %d argument(s), got %d",
                        fn_t->fn_param_count, n->as.call.args.count);

                for (int i = 0;
                     i < n->as.call.args.count
                         && i < fn_t->fn_param_count; i++) {
                    AstNode* a = n->as.call.args.items[i];
                    pre_type_literal(a, fn_t->fn_params[i]);
                    Type* at = analyze_expr(sc, fl, a, bag);
                    if (at && fn_t->fn_params[i]) {
                        coerce_to_target(&n->as.call.args.items[i],
                                         fn_t->fn_params[i],
                                         at, a->span, bag);
                    }
                }
 
                // Build the callee expression: a fresh
                // AST_VAR pointing at the local's slot.
                AstNode* callee = ast_var(nm);
                callee->local_index     = fl->locals[li].slot;
                callee->type            = fn_t;
                callee->holds_rc        = false;

                // Save the args list before the union reuse.
                AstList saved_args = n->as.call.args;

                n->kind = AST_CALL_INDIRECT;
                n->as.call_indirect.callee = callee;
                n->as.call_indirect.args   = saved_args;
                n->type = fn_t->fn_ret;
                return n->type;
            }

            // -- Explicit generic instantiation: `f<T>(...)` --
            //
            // The AST carries the type arguments in
            // `instantiation` when the source wrote
            // `f<T>`. When present, this call is to a
            // monomorphized copy, and the resolution is
            // delegated to sema_instantiate_function.
            //
            // Note that the *result* FuncSig (which was
            // produced by instantiating) may itself have
            // type parameters that were substituted; it is
            // a concrete function. So argument checking
            // below uses the instantiated signature, not
            // the template.
            if (n->as.call.instantiation) {
                int fidx = sema_instantiate_function(sc, nm,
                    n->as.call.instantiation,bag);
                if (fidx < 0) { n->type = ty_i64(); return n->type; }
                FuncSig* sig = &sc->funcs[fidx];

                if (n->as.call.args.count != sig->param_count)
                    diag_error(bag, n->span,
                        "function '%s' expects %d argument(s), got %d",
                        nbuf, sig->param_count, n->as.call.args.count);

                // Argument checking. The `&& i < sig->param_count`
                // in the loop bound prevents an out-of-bounds
                // read when the arity check above failed
                // (extra args have no matching parameter).
                for (int i = 0; i < n->as.call.args.count
                                && i < sig->param_count; i++) {
                    AstNode* a = n->as.call.args.items[i];
                    pre_type_literal(a, sig->param_types[i]);
                    Type* at = analyze_expr(sc, fl, a, bag);
                    if (at && sig->param_types[i]) {
                        coerce_to_target(&n->as.call.args.items[i],
                                         sig->param_types[i],
                                         at, a->span, bag);
                    }
                }
                n->as.call.func_idx   = fidx;
                n->as.call.builtin_id = -1;
                n->as.call.ffi_idx    = -1;
                n->type = sig->return_type
                        ? sig->return_type : ty_i64();
                return n->type;
            }

            // -- Builtins --
            //
            // Local table. This is the *actual* builtin
            // table; the one in sema.h (BUILTINS[]) is
            // dead code. The two are meant to agree but
            // are maintained separately. `ret_kind` here
            // is an integer code, not a BuiltinRet:
            //   0 = void
            //   1 = i64
            //   2 = string
            //   3 = *u8
            //   4 = *void
            //   5 = same *T as the first argument (for
            //       raw_offset)
            //   6 = bool
            // Codegen's dispatch on builtin_id uses the
            // same numbering (with two more entries: 12
            // = ffi_alloc, 13 = ffi_free).
            static const struct {
                const char* name;
                int min_args, max_args;
                int ret_kind;
            } B[] = {
                { "print",        0, -1, 0 },
                { "len",          1,  1, 1 },
                { "format",       0, -1, 2 },
                { "ffi_cstring",  1,  1, 3 },
                { "ffi_error",    0,  0, 2 },
                { "ffi_errno",    0,  0, 1 },
                { "ffi_string",   1,  1, 2 },
                { "raw_null",     0,  0, 4 },
                { "raw_is_null",  1,  1, 6 },
                { "raw_offset",   2,  2, 5 },
                { "raw_load_u8",  2,  2, 1 },
                { "raw_store_u8", 3,  3, 0 },
                { "ffi_alloc",    1,  1, 4 },   // 4 = *void
                { "ffi_free",     1,  1, 0 },   // 0 = void
                { "args",         0,  0, 7 },   
                { "read_line",    0,  0, 8 },   
                { "read_all",     0,  0, 9 },   
            };
            int nbuilt = (int)(sizeof B / sizeof B[0]);
            for (int b = 0; b < nbuilt; b++) {
                if (strcmp(nbuf, B[b].name) != 0) continue;
                int nargs = n->as.call.args.count;
                if (B[b].min_args >= 0 && nargs < B[b].min_args)
                    diag_error(bag, n->span,
                        "'%s' requires at least %d argument(s), got %d",
                        nbuf, B[b].min_args, nargs);
                if (B[b].max_args >= 0 && nargs > B[b].max_args)
                    diag_error(bag, n->span,
                        "'%s' accepts at most %d argument(s), got %d",
                        nbuf, B[b].max_args, nargs);

                // Every argument is analyzed (so nested
                // expressions get their types), but the
                // results are not checked against any
                // expected type. Builtins are polymorphic
                // over their arguments; the codegen
                // handles the type dispatch.
                for (int i = 0; i < nargs; i++)
                    analyze_expr(sc, fl, n->as.call.args.items[i], bag);

                n->as.call.builtin_id = b;
                n->as.call.func_idx   = -1;
                n->as.call.ffi_idx    = -1;
                // Return-type dispatch on the code.
                switch (B[b].ret_kind) {
                    case 0: n->type = ty_void();              break;
                    case 1: n->type = ty_i64();               break;
                    case 2: n->type = ty_string();            break;
                    case 3: n->type = ty_rawptr(ty_u8());     break;
                    case 4: n->type = ty_rawptr(ty_void());   break;
                    case 5:
                        // `raw_offset(p, n)` returns the
                        // same type as `p`. The check for
                        // TY_RAWPTR is defensive; a
                        // non-pointer first arg is a
                        // codegen-time error.
                        if (nargs > 0 && n->as.call.args.items[0]->type
                            && n->as.call.args.items[0]->type->kind == TY_RAWPTR)
                            n->type = n->as.call.args.items[0]->type;
                        else
                            n->type = ty_rawptr(ty_void());
                        break;
                    case 6: n->type = ty_bool();              break;
                    case 7:
                        // args() : rc<[string]>
                        n->type = ty_rc(ty_array(ty_string()));
                        break;
                    case 8: {
                        // read_line() : Option<string>.
                        // Resolve via the prelude's Option
                        // template; see instantiate_option_of.
                        Type* opt = instantiate_option_of(sc,
                            ty_string(), n->span, bag);
                        n->type = opt ? opt : ty_string();
                        break;
                    }
                    case 9:
                        n->type = ty_string();
                        break;
                    default: n->type = ty_i64(); break;
                }
                return n->type;
            }

            // -- User function --
            //
            // Resolution order:
            //   1. File-local declarations (find_func).
            //   2. Selective imports (resolve_name on
            //      the target module's file_id).
            //
            // The two-step order is what makes a local
            // shadow a selective import of the same name.
            // That matches the language rule that a
            // declaration in the current file is visible
            // before anything imported.
            ResolvedDecl rd = resolve_name(sc, n->span.file_id, nm);
            int idx = -1;
            if (rd.kind == RD_FUNC) {
                idx = rd.index;
            }
            if (idx >= 0 && !sc->funcs[idx].is_pub) {
                // The resolver returned the function; check
                // visibility only when it came from another
                // file.
                if (sc->funcs[idx].file_id != n->span.file_id) {
                    const char* def_path = "<unknown>";
                    SourceFile* sf = smap_get(sc->smap,
                        sc->funcs[idx].file_id);
                    if (sf) def_path = sf->path;
                    diag_error(bag, n->span,
                        "'%s' is private; declared in %s "
                        "and not marked 'pub'",
                        nbuf, def_path);
                    idx = -1;
                }
            }
            if (idx < 0) {
                diag_error(bag, n->span, "unknown function '%s'", nbuf);
                for (int i = 0; i < n->as.call.args.count; i++)
                    analyze_expr(sc, fl, n->as.call.args.items[i], bag);
                return NULL;
            }

            FuncSig* sig = &sc->funcs[idx];

            // -- Extern function --
            //
            // Externs have a different arg-checking rule:
            // a variadic extern accepts extra args, and
            // those extra args must be FFI-variadic-ok
            // types (integers and pointers only).
            if (sig->is_extern) {
                int n_args = n->as.call.args.count;
                if (sig->is_variadic) {
                    if (n_args < sig->param_count)
                        diag_error(bag, n->span,
                            "extern '%s' expects at least %d argument(s), got %d",
                            nbuf, sig->param_count, n_args);
                } else if (n_args != sig->param_count) {
                    diag_error(bag, n->span,
                        "extern '%s' expects %d argument(s), got %d",
                        nbuf, sig->param_count, n_args);
                }
                for (int i = 0; i < n_args; i++) {
                    AstNode* a = n->as.call.args.items[i];
                    if (i < sig->param_count && sig->param_types[i])
                        pre_type_literal(a, sig->param_types[i]);
                    Type* at = analyze_expr(sc, fl, a, bag);
                    if (i < sig->param_count) {
                        // Fixed-arg slot: normal type check,
                        // with implicit widening.
                        if (at && sig->param_types[i]) {
                            coerce_to_target(&n->as.call.args.items[i],
                                             sig->param_types[i],
                                             at, a->span, bag);
                        }
                    } else {
                        // Variadic tail. Restricted to
                        // types that can survive the C
                        // ABI unchanged.
                        bool ok = is_ffi_variadic_ok(at);
                        if (!ok)
                            diag_error(bag, a->span,
                                "variadic argument %d has type %s; only "
                                "integers and pointers are allowed in "
                                "variadic position",
                                i + 1, at ? type_name(at) : "?");
                    }
                }
                n->as.call.func_idx   = idx;
                n->as.call.ffi_idx    = sig->ffi_idx;
                n->as.call.builtin_id = -1;
                n->type = sig->return_type ? sig->return_type : ty_void();
                return n->type;
            }

            // -- Regular user function --
            n->as.call.func_idx   = idx;
            n->as.call.ffi_idx    = -1;
            n->as.call.builtin_id = -1;

            // Arity check. On failure, analyze args for
            // errors and return the function's declared
            // return type (or i64 if none yet).
            if (n->as.call.args.count != sig->param_count) {
                diag_error(bag, n->span,
                    "function '%s' expects %d argument%s, got %d",
                    nbuf, sig->param_count,
                    sig->param_count == 1 ? "" : "s",
                    n->as.call.args.count);
                for (int i = 0; i < n->as.call.args.count; i++)
                    analyze_expr(sc, fl, n->as.call.args.items[i], bag);
                n->type = sig->return_type ? sig->return_type : ty_i64();
                return n->type;
            }
            
            // Per-argument type check, with implicit widening.
            // The arity check above has already established
            // args.count == sig->param_count, so indexing
            // sig->param_types[i] is in bounds for the whole
            // loop.
            for (int i = 0; i < n->as.call.args.count; i++) {
                AstNode* a = n->as.call.args.items[i];
                if (sig->param_types[i])
                    pre_type_literal(a, sig->param_types[i]);
                
                Type* at = analyze_expr(sc, fl, a, bag);
                if (at && sig->param_types[i]) {
                    coerce_to_target(&n->as.call.args.items[i],
                                     sig->param_types[i],
                                     at, a->span, bag);
                }
            }

            // Return-type inference fallback. A function
            // with no return annotation that was never
            // observed to return a value has return_type
            // == NULL at this point. If the call site is
            // reached before any such observation, we
            // cannot type the result; the user is told to
            // annotate or to move the call later.
            //
            // This is the classic "mutually recursive
            // inference" limitation: forward references
            // to inferred functions cannot be typed.
            if (sig->return_type == NULL) {
                diag_error(bag, n->span,
                    "cannot infer return type of '%s' at this call site; "
                    "add a return type annotation or move this call after "
                    "'%s''s definition", nbuf, nbuf);
                n->type = ty_i64();
                return n->type;
            }
            n->type = sig->return_type;
            return n->type;
        }

        case AST_CALL_INDIRECT:
            // Only reachable if sema is re-run on an
            // already-analyzed node (the rewrite in AST_CALL
            // produces these; the parser never does).
            return n->type;
        
        // ----- Allocation / borrow -----
        //
        // `&e` / `&mut e`: produces ref<T> where T is e's
        // type. The arena tier is recorded on the node so
        // codegen knows the value lives in the arena and
        // does not need an RC refcount. refs cannot contain
        // refs (transitively) by the language rules, so a
        // type_contains_ref(inner) check is enforced here.
        case AST_ALLOC_ARENA: {
            AstNode* inner = n->as.alloc_arena.value;

            // Analyze the inner expression first. For an
            // extern struct field access this sets owner,
            // offset, and declared_type on the AST_FIELD node.
            Type* inner_t = analyze_expr(sc, fl, inner, bag);
            if (!inner_t) inner_t = ty_i64();

            // `&p.x` where p is a *ExternStruct: rewrite to a
            // direct address operation. The result is a raw
            // pointer to the field's storage, not a ref<T>.
            // Raw pointers are always writable; the is_mut
            // flag on the source form is accepted and ignored.
            if (inner->kind == AST_FIELD
                && inner->as.field.owner
                && inner->as.field.owner->kind == TY_EXTERN_STRUCT) {
                Type* declared = inner->as.field.declared_type;
                n->kind = AST_ADDR_OF_FIELD;
                n->as.addr_of_field.field = inner;
                n->type = ty_rawptr(declared ? declared : inner_t);
                n->tier = TIER_NONE;
                return n->type;
            }

            // Regular arena case, unchanged.
            if (type_contains_ref(inner_t)) {
                diag_error(bag, n->span,
                    "cannot take a reference to a reference-typed value");
                inner_t = ty_i64();
            }
            n->tier = TIER_ARENA;
            n->type = ty_ref(inner_t, n->as.alloc_arena.is_mut);
            return n->type;
        }

        // `rc e`: produces rc<T> where T is e's type. RC
        // values go on the RC heap (malloc + RcHeader).
        // Like the arena case, refs are rejected inside an
        // RC wrapper: an RC value can outlive the scope
        // that owns a referenced local, which would dangle.
        //
        // Additional rule: when T is a compound type
        // (array, struct, enum), the operand must be a
        // *literal* form (array literal, struct literal,
        // variant constructor, or unit variant). rc of a
        // variable is rejected: `rc x` would need to copy
        // the aggregate's payload, which is not implemented
        // today. Users are told to write `rc <literal>`.
        case AST_ALLOC_RC: {
            AstNode* inner_ast = n->as.alloc_rc.value;
            Type* inner = analyze_expr(sc, fl, inner_ast, bag);
            if (!inner) inner = ty_i64();

            // If the operand is already an RC-managed value,
            // wrapping it in `rc` produces a nested RC — which
            // is never what the user wants, and which breaks
            // the `?` rule and every place that expects a
            // single-level RC handle. The common cause is
            // `rc Option.None` / `rc Result.Ok(...)`: prelude
            // types are already RC-managed.
            if (inner->kind == TY_RC) {
                diag_error(bag, inner_ast->span,
                    "value of type %s is already RC-managed; "
                    "`rc` is not needed here",
                    type_name(inner));
                inner = ty_i64();
            }

            if (type_contains_ref(inner)) {
                diag_error(bag, n->span,
                    "rc<T> cannot contain a reference");
                inner = ty_i64();
            }

            bool is_compound = (inner->kind == TY_ARRAY
                             || inner->kind == TY_STRUCT
                             || inner->kind == TY_ENUM);
            if (is_compound) {
                // The set of literal forms. Note that the
                // AST_FIELD case with unit_tag >= 0 is the
                // unit-variant construction form
                // (`Enum.Variant` with no payload), not a
                // field access.
                // 
                // "Literal form" means a syntactic constructor that builds
                // a compound value directly. AST_VARIANT_CTOR also covers
                // method calls and module calls; those are not constructor
                // literals and are excluded.
                bool is_variant_ctor =
                    inner_ast->kind == AST_VARIANT_CTOR
                    && inner_ast->as.variant_ctor.receiver_expr == NULL
                    && inner_ast->as.variant_ctor.mod_func_idx < 0
                    && inner_ast->as.variant_ctor.mod_ffi_idx < 0;
                bool is_unit_variant =
                    inner_ast->kind == AST_FIELD
                    && inner_ast->as.field.unit_tag >= 0;
                bool is_literal =
                    (inner_ast->kind == AST_ARRAY_LIT
                     || inner_ast->kind == AST_STRUCT_LIT
                     || is_variant_ctor
                     || is_unit_variant);
                if (!is_literal) {
                    diag_error(bag, inner_ast->span,
                        "rc on a %s variable is not yet supported; "
                        "use `rc <literal>` directly",
                        type_name(inner));
                    inner = ty_i64();
                }
            }

            n->tier = TIER_RC;
            n->type = ty_rc(inner);
            return n->type;
        }

        case AST_ADDR_OF_FIELD:
            // Already resolved by the AST_ALLOC_ARENA rewrite
            // above. Return the cached type.
            return n->type;
        

        // `*e`: dereference. The operand must be a reference,
        // rc, or raw pointer; the result is its inner type.
        // Note that rc<T> is dereferenceable, unlike in some
        // languages where RC handles hide the payload.
        case AST_DEREF: {
            Type* ot = analyze_expr(sc, fl, n->as.deref.operand, bag);
            if (!ot) return NULL;
            switch (ot->kind) {
                case TY_REF:
                case TY_RC:
                case TY_RAWPTR:
                    if (ot->inner
                        && ot->inner->kind == TY_EXTERN_STRUCT) {
                        TypeDef* td = ot->inner->tdef;
                        if (td && td->is_opaque) {
                            diag_error(bag, n->span,
                                "cannot dereference *%s; opaque "
                                "types have no value representation",
                                td->name_buf);
                        } else {
                            diag_error(bag, n->span,
                                "cannot dereference *%s; extern "
                                "structs have no value representation",
                                ot->inner->tdef->name_buf);
                        }
                        return NULL;
                    }
                    n->type = ot->inner;
                    return n->type;
                default:
                    diag_error(bag, n->span,
                        "cannot dereference a value of type %s",
                        type_name(ot));
                    return NULL;
            }
        }

        // ----- Array literal -----
        //
        // Element type is taken from the first element; every
        // other element must match. An empty `[]` is
        // rejected (no way to infer T). The resulting type
        // is `[T]`.
        //
        // Note that element 0 is used as the reference type,
        // not "the first element that resolved". If element
        // 0 fails to resolve, its error is reported and the
        // fallback is i64 for the whole array; subsequent
        // elements are then checked against i64, producing
        // possibly-spurious diagnostics. A smarter version
        // would pick the first resolved element; not done
        // today.
        case AST_ARRAY_LIT: {
            int n_elems = n->as.array_lit.elements.count;
            if (n_elems == 0) {
                diag_error(bag, n->span,
                    "empty array literals are not yet supported");
                n->type = ty_array(ty_i64());
                return n->type;
            }
            Type* elem = NULL;
            for (int i = 0; i < n_elems; i++) {
                AstNode* e = n->as.array_lit.elements.items[i];
                Type* et = analyze_expr(sc, fl, e, bag);
                if (i == 0) {
                    elem = et ? et : ty_i64();
                } else if (et && elem && !type_eq(elem, et)) {
                    diag_error(bag, e->span,
                        "array element %d has type %s, expected %s",
                        i + 1, type_name(et), type_name(elem));
                }
            }
            n->type = ty_array(elem ? elem : ty_i64());
            return n->type;
        }

        // ----- Array / string indexing -----
        //
        // Indexable containers:
        //   - TY_STRING: index yields u8.
        //   - TY_ARRAY: index yields the element type.
        //   - TY_RC of TY_ARRAY: same, transparently.
        // Index must be an integer.
        //
        // The RC unwrap is done here rather than in the
        // container-type check, so that `s[i]` works on both
        // `[u8]` and `rc<[u8]>` without a special case in
        // the user's source.
        case AST_INDEX: {
            Type* tt = analyze_expr(sc, fl, n->as.index.target, bag);
            Type* it = analyze_expr(sc, fl, n->as.index.index, bag);
            if (it && !is_integer_type(it))
                diag_error(bag, n->as.index.index->span,
                    "index must be an integer, found %s", type_name(it));

            if (!tt) { n->type = ty_i64(); return n->type; }
            if (tt->kind == TY_STRING) {
                n->type = ty_u8();
                return n->type;
            }

            Type* arr = NULL;
            if (tt->kind == TY_ARRAY) {
                arr = tt;
            } else if (tt->kind == TY_RC && tt->inner
                       && tt->inner->kind == TY_ARRAY) {
                arr = tt->inner;
            }
            if (!arr) {
                diag_error(bag, n->as.index.target->span,
                    "cannot index a value of type %s", type_name(tt));
                n->type = ty_i64();
                return n->type;
            }
            n->type = arr->inner;
            return n->type;
        }

        // ----- Struct literal -----
        //
        // `Name { field: v, ... }` with an optional
        // `Name<T> { ... }` instantiation form.
        //
        // Resolution order for the struct TypeDef:
        //   1. If an instantiation is present, resolve it
        //      and use its TypeDef (unwrapping rc<> if the
        //      generic is RC-managed).
        //   2. Else find the type by name in the current
        //      file.
        //   3. Else look for a selective import with that
        //      local name and resolve it in the target file.
        //
        // After the TypeDef is found, the initializer list is
        // checked: every named field must exist, no field may
        // be initialized twice, and every field of the struct
        // must be initialized. Missing fields and unknown
        // fields are both errors.
        //
        // Field offsets on the AST are filled in from the
        // TypeDef's flat field array, and codegen uses them
        // directly when emitting OP_STORE_INDIRECT.
        case AST_STRUCT_LIT: {
            TypeDef* sd = NULL;
            // Path 1: explicit `<...>`.
            if (n->as.struct_lit.instantiation) {
                Type* t = type_expr_resolve(n->as.struct_lit.instantiation,
                                            sc->smap, sc->structs,
                                            &sc->modtable, n->span.file_id,
                                            sc, bag);
                // RC-managed generics appear as rc<T>;
                // unwrap one level to inspect the underlying
                // TY_STRUCT.
                if (t && t->kind == TY_RC) t = t->inner;
                if (t && t->kind == TY_STRUCT) sd = t->tdef;
                else if (t)
                    diag_error(bag, n->span,
                        "type arguments resolve to %s, not a struct",
                        type_name(t));
            }
            // Path 2: file-local name.
            if (!sd) {
                ResolvedDecl rd = resolve_name(sc, n->span.file_id,
                                               n->as.struct_lit.type_name);
                sd = resolved_tdef(sc, rd);

                // If the name resolved to a template (a
                // generic type), use the contextual hint from
                // the surrounding annotation. That is how
                // `let p: Pair<i32> = Pair { ... }` finds the
                // instantiated TypeDef.
                if (!sd && n->as.struct_lit.expected_type
                    && n->as.struct_lit.expected_type->tdef) {
                    sd = n->as.struct_lit.expected_type->tdef;
                }
            }
            if (!sd) {
                char buf[64];
                smap_span_text(sc->smap, n->as.struct_lit.type_name.span,
                               buf, sizeof buf);
                diag_error(bag, n->span, "unknown struct '%s'", buf);
                n->type = ty_i64();
                return n->type;
            }
            // Extern struct literal. Produces a pointer to
            // freshly arena-allocated C-layout memory; the
            // literal's type is `*T`, not `T`. Extern structs
            // still have no value representation — the
            // literal returns an address.
            if (sd->kind == TDEF_EXTERN_STRUCT) {
                if (sd->is_opaque) {
                    diag_error(bag, n->span,
                        "'%s' is opaque and cannot be constructed",
                        sd->name_buf);
                    n->type = ty_i64();
                    return n->type;
                }

                AstList* il = &n->as.struct_lit.inits;
                bool seen[128] = {false};
                if (sd->field_count > 128)
                    diag_error(bag, n->span, "too many fields (max 128)");

                for (int i = 0; i < il->count; i++) {
                    AstNode* init = il->items[i];
                    int idx = -1;
                    for (int k = 0; k < sd->field_count; k++) {
                        if (smap_tokens_equal(sc->smap, sd->fields[k].name,
                                              init->as.struct_init.name)) {
                            idx = k; break;
                        }
                    }
                    if (idx < 0) {
                        char buf[64];
                        smap_span_text(sc->smap,
                            init->as.struct_init.name.span,
                            buf, sizeof buf);
                        diag_error(bag, init->span,
                            "extern struct '%s' has no field '%s'",
                            sd->name_buf, buf);
                        continue;
                    }
                    if (idx < 128 && seen[idx]) {
                        diag_error(bag, init->span,
                            "duplicate initializer for field");
                        continue;
                    }
                    if (idx < 128) seen[idx] = true;

                    init->as.struct_init.offset     = sd->fields[idx].offset;
                    init->as.struct_init.field_type = sd->fields[idx].type;

                    AstNode* vnode = init->as.struct_init.value;
                    Type* ftype = sd->fields[idx].type;
                    pre_type_literal(vnode, ftype);
                    Type* vt = analyze_expr(sc, fl, vnode, bag);
                    if (vt && !coerce_to_field(&init->as.struct_init.value,
                                               ftype, vt)) {
                        char fn_buf[64];
                        smap_span_text(sc->smap,
                            sd->fields[idx].name.span,
                            fn_buf, sizeof fn_buf);
                        diag_error(bag, vnode->span,
                            "field '%s' expects %s, got %s",
                            fn_buf, type_name(ftype), type_name(vt));
                    }
                }
                for (int k = 0; k < sd->field_count && k < 128; k++) {
                    if (!seen[k]) {
                        char fn_buf[64];
                        smap_span_text(sc->smap,
                            sd->fields[k].name.span,
                            fn_buf, sizeof fn_buf);
                        diag_error(bag, n->span,
                            "missing initializer for field '%s'", fn_buf);
                    }
                }

                // Declare an anonymous local for the pointer.
                // It lives for the function's duration; each
                // extern struct literal in the same function
                // gets its own slot, which is bounded by
                // MAX_LOCALS.
                Type* et = ty_extern_struct(sd);
                int slot = declare_anon(fl, ty_rawptr(et));
                if (slot < 0)
                    diag_error(bag, n->span, "too many locals");
                n->as.struct_lit.hidden_slot = slot;
                n->as.struct_lit.sdef_index  = (int)(sd - sc->structs->items);

                n->type = ty_rawptr(et);
                return n->type;
            }
            if (sd->kind != TDEF_STRUCT) {
                // Existing enum case, unchanged.
                diag_error(bag, n->span,
                    "'%s' is an enum; use `%s.Variant(...)`",
                    sd->name_buf, sd->name_buf);
                n->type = ty_i64();
                return n->type;
            }
            n->as.struct_lit.sdef_index = (int)(sd - sc->structs->items);

            AstList* il = &n->as.struct_lit.inits;
            // `seen[k]` is true iff field k has already been
            // initialized by an earlier entry. The 128-element
            // array is a static cap on field count for the
            // duplicate-detection array. Structs with >128
            // fields are rejected with a diagnostic.
            bool seen[128] = {false};
            if (sd->field_count > 128)
                diag_error(bag, n->span, "too many fields (max 128)");

            for (int i = 0; i < il->count; i++) {
                AstNode* init = il->items[i];
                // Find the field by name. Linear scan of the
                // TypeDef's field array.
                int idx = -1;
                for (int k = 0; k < sd->field_count; k++) {
                    if (smap_tokens_equal(sc->smap, sd->fields[k].name,
                                          init->as.struct_init.name)) {
                        idx = k; break;
                    }
                }
                if (idx < 0) {
                    char buf[64];
                    smap_span_text(sc->smap, init->as.struct_init.name.span,
                                   buf, sizeof buf);
                    diag_error(bag, init->span,
                        "struct '%s' has no field '%s'",
                        sd->name_buf, buf);
                    continue;
                }
                if (idx < 128 && seen[idx]) {
                    diag_error(bag, init->span,
                        "duplicate initializer for field");
                    continue;
                }
                if (idx < 128) seen[idx] = true;
                // The AST node carries the resolved offset so
                // codegen can emit a direct store. The
                // re-computation at codegen time (see
                // AST_STRUCT_LIT in codegen.c) matches this,
                // so the two must stay in sync.
                init->as.struct_init.offset = sd->fields[idx].offset;
                init->as.struct_init.field_type = sd->fields[idx].type;

                AstNode* vnode = init->as.struct_init.value;
                Type* ftype = sd->fields[idx].type;
                pre_type_literal(vnode, ftype);
                Type* vt = analyze_expr(sc, fl, vnode, bag);
                if (vt && !coerce_to_field(&init->as.struct_init.value,
                                           ftype, vt)) {
                    char fn_buf[64];
                    smap_span_text(sc->smap,
                        sd->fields[idx].name.span,
                        fn_buf, sizeof fn_buf);
                    diag_error(bag, vnode->span,
                        "field '%s' expects %s, got %s",
                        fn_buf, type_name(ftype), type_name(vt));
                }
            }
            // Every field must be initialized (no default
            // values today). The `&& k < 128` guard mirrors
            // the `seen` cap above.
            for (int k = 0; k < sd->field_count && k < 128; k++) {
                if (!seen[k]) {
                    char fn_buf[64];
                    smap_span_text(sc->smap, sd->fields[k].name.span,
                                   fn_buf, sizeof fn_buf);
                    diag_error(bag, n->span,
                        "missing initializer for field '%s'", fn_buf);
                }
            }
            n->type = ty_struct(sd);
            return n->type;
        }

        // ----- Field access, or unit-variant construction -----
        //
        // This node kind does double duty. Two cases:
        //
        //   1. `Type.Variant` where Type is an enum and Variant
        //      has no payload. The parser produced
        //      AST_FIELD with `target` being AST_VAR(Type) and
        //      `field` being the variant name. If Type
        //      resolves to a TypeDef that is an enum, this
        //      node is a unit-variant *construction*, not a
        //      field access. The node's unit_tag is set to
        //      the variant's tag and codegen emits a
        //      zero-payload enum constructor.
        //
        //   2. Everything else: a normal field access. The
        //      target's type must be a struct (possibly
        //      wrapped in ref/rc/rawptr); the field is
        //      resolved by name and its offset recorded.
        //
        // The distinguishing test at the top is heuristic:
        // it checks whether the target is an unresolved
        // identifier whose name happens to be a TypeDef in
        // scope. If yes, the unit-variant path runs. This is
        // why the enum-with-no-payload case only fires when
        // the identifier is *not* a local or const: a local
        // named `Option` would shadow the type name, and the
        // field access path would run instead.
        case AST_FIELD: {
            AstNode* target = n->as.field.target;

            // Test for the unit-variant case: unresolved
            // identifier that names an enum type.


            if (target->kind == AST_VAR
                && resolve(fl, sc->smap, target->as.var.name) < 0
                && consts_resolve(sc->consts, sc->smap,
                                  target->as.var.name) < 0) {
                // Try to interpret `Name.Variant` as enum
                // unit-variant construction. Resolution has
                // three outcomes:
                //   RD_TYPE / RD_BUILTIN_TYPE — a concrete
                //     TypeDef; if it is an enum, proceed.
                //   RD_TEMPLATE — a generic type such as
                //     Option or Result. The type arguments
                //     must come from context; fall back to
                //     the enclosing function's return type.
                //   RD_NONE — not a type; fall through to
                //     ordinary field access.
                ResolvedDecl rd = resolve_name(sc, n->span.file_id,
                                               target->as.var.name);
                TypeDef* td = NULL;
                if (rd.kind == RD_TYPE || rd.kind == RD_BUILTIN_TYPE)
                    td = resolved_tdef(sc, rd);

                // Contextual inference: if td is still NULL,
                // and the enclosing function has an annotated
                // return type that is an RC-managed enum
                // whose mangled name has the target's name as
                // a base prefix, use it.
                if (!td && fl->current_sig
                    && fl->current_sig->return_annotated
                    && fl->current_sig->return_type) {
                    Type* rt = fl->current_sig->return_type;
                    if (rt->kind == TY_RC && rt->inner
                        && rt->inner->kind == TY_ENUM) {
                        TypeDef* cand = rt->inner->tdef;
                        char tn_buf[64];
                        smap_span_text(sc->smap,
                            target->as.var.name.span,
                            tn_buf, sizeof tn_buf);
                        size_t tn_len = strlen(tn_buf);
                        if (strncmp(cand->name_buf, tn_buf, tn_len) == 0
                            && (cand->name_buf[tn_len] == '#'
                                || cand->name_buf[tn_len] == '\0'))
                            td = cand;
                    }
                }

                if (td && td->kind == TDEF_ENUM) {
                    int tag = -1;
                    for (int i = 0; i < td->variant_count; i++) {
                        if (smap_tokens_equal(sc->smap, td->variants[i].name,
                                              n->as.field.field)) {
                            tag = i; break;
                        }
                    }
                    if (tag < 0) {
                        char vn[64];
                        smap_span_text(sc->smap, n->as.field.field.span,
                                       vn, sizeof vn);
                        diag_error(bag, n->span,
                            "enum '%s' has no variant '%s'",
                            td->name_buf, vn);
                        n->type = ty_i64();
                        return n->type;
                    }
                    // If the variant has payloads, this
                    // syntax is wrong; the user must use
                    // the parentheses form. Report and
                    // continue with the error type.
                    if (td->variants[tag].payload_count != 0) {
                        diag_error(bag, n->span,
                            "variant '%s.%s' takes %d payload(s); "
                            "use `%s.%s(...)`",
                            td->name_buf, td->variants[tag].name_buf,
                            td->variants[tag].payload_count,
                            td->name_buf, td->variants[tag].name_buf);
                        n->type = ty_i64();
                        return n->type;
                    }
                    n->as.field.unit_tag = tag;
                    n->type = ty_enum(td);
                    return n->type;
                }
            }

            // Normal field access path.
            Type* tt = analyze_expr(sc, fl, target, bag);
            if (!tt) { n->type = ty_i64(); return n->type; }

            // Unwrap one level of indirection: `p.x` where p
            // is a ref / rc / rawptr to a struct is allowed.
            // The pointer-like wrappers are peeled here; the
            // resulting type must be a struct.
            Type* st = tt;
            if (st->kind == TY_REF
                || st->kind == TY_RC
                || st->kind == TY_RAWPTR)
                st = st->inner;

            // NEW: extern struct field access is Phase 2 work.
            // Report a specific message here so the user knows
            // the compiler recognized the type and is not just
            // confused about struct-ness.
            if (st->kind == TY_EXTERN_STRUCT) {
                TypeDef* td = st->tdef;
                if (td->is_opaque) {
                    diag_error(bag, n->span,
                        "cannot access field of opaque type '%s'; "
                        "its layout is not known to Prism, use the "
                        "C API to manipulate its contents",
                        td->name_buf);
                    n->type = ty_i64();
                    return n->type;
                }
                
                int idx = -1;
                for (int k = 0; k < td->field_count; k++) {
                    if (smap_tokens_equal(sc->smap, td->fields[k].name,
                                          n->as.field.field)) {
                        idx = k; break;
                    }
                }
                if (idx < 0) {
                    char buf[64];
                    smap_span_text(sc->smap, n->as.field.field.span,
                                   buf, sizeof buf);
                    diag_error(bag, n->span,
                        "extern struct '%s' has no field '%s'",
                        td->name_buf, buf);
                    n->type = ty_i64();
                    return n->type;
                }
                Type* declared = td->fields[idx].type;
                n->as.field.owner         = st;
                n->as.field.offset        = td->fields[idx].offset;
                n->as.field.declared_type = declared;

                // Nested-by-value extern struct field: the value
                // of `p.inner` is the field's address, typed
                // *Inner. Leaf fields (integers, floats, pointers)
                // load the field's storage, typed as the declared
                // type.
                if (declared->kind == TY_EXTERN_STRUCT) {
                    n->type = ty_rawptr(declared);
                } else {
                    n->type = declared;
                }
                return n->type;
            }
            
            if (st->kind != TY_STRUCT) {
                diag_error(bag, target->span,
                    "cannot access field on type %s", type_name(tt));
                n->type = ty_i64();
                return n->type;
            }
            

            TypeDef* sd = st->tdef;
            int idx = -1;
            for (int k = 0; k < sd->field_count; k++) {
                if (smap_tokens_equal(sc->smap, sd->fields[k].name,
                                      n->as.field.field)) {
                    idx = k; break;
                }
            }
            if (idx < 0) {
                char buf[64];
                smap_span_text(sc->smap, n->as.field.field.span,
                               buf, sizeof buf);
                diag_error(bag, n->span,
                    "struct '%s' has no field '%s'", sd->name_buf, buf);
                n->type = ty_i64();
                return n->type;
            }
            // Record the resolved offset for codegen. Note
            // that the offset is the field's byte offset
            // within the struct's payload, not within the
            // enclosing pointer. Codegen for a ref / rc
            // target may emit an extra indirect load before
            // using this offset; see AST_FIELD in codegen.c.
            n->as.field.owner         = st;
            n->as.field.offset        = sd->fields[idx].offset;
            n->as.field.declared_type = sd->fields[idx].type;
            n->type = sd->fields[idx].type;
            return n->type;
        }

        // ----- Variant construction, module call, or method call -----
        //
        // This is the most overloaded expression node in the
        // AST. Three distinct shapes share the same node kind:
        //
        //   1. Method call: `recv.method(args)`. The parser
        //      produces AST_VARIANT_CTOR with receiver_expr
        //      set. Semantics: look up the method on the
        //      receiver's type, check its args, use its
        //      return type.
        //
        //   2. Module-qualified call: `Mod.func(args)` where
        //      Mod is a module alias. Semantics: look up
        //      func in the module's file, check pub, check
        //      args, use its return type.
        //
        //   3. Enum variant construction:
        //      `Enum.Variant(args)`. Semantics: find the
        //      enum, find the variant, check payload count
        //      and types, result is the enum's type
        //      (possibly RC-wrapped).
        //
        // The dispatch is by structure:
        //   - receiver_expr set             -> method
        //   - type_name resolves as module  -> module call
        //   - type_name resolves as enum    -> variant
        //   - neither                       -> error
        //
        // There is a fallback path for case 3 when the type
        // name is not resolvable: if the enclosing function
        // has an annotated return type that is an RC-managed
        // enum whose name starts with the variant's type
        // name, that enum is used. This is what allows the
        // prelude's `Ok(...)` / `Err(...)` / `Some(...)` /
        // `None` to be used without an explicit `Result.` /
        // `Option.` prefix: the return type annotation of
        // the enclosing function supplies the missing enum.
        case AST_VARIANT_CTOR: {
            // ---- Method call ----
            // If the parser saw `a.b(args)` where a is a bare
            // identifier that is not a local, it can't tell
            // at parse time whether a is a receiver or a
            // type name. This block re-tests that: if the
            // parser set no receiver, but the type_name is
            // a local, we synthesize a receiver.
            if (n->as.variant_ctor.receiver_expr == NULL) {
                int li = resolve(fl, sc->smap, n->as.variant_ctor.type_name);
                if (li >= 0)
                    n->as.variant_ctor.receiver_expr =
                        ast_var(n->as.variant_ctor.type_name);
            }
            if (n->as.variant_ctor.receiver_expr != NULL) {
                AstNode* recv = n->as.variant_ctor.receiver_expr;
                Type* rt = analyze_expr(sc, fl, recv, bag);
                // Unwrap ref / rc / rawptr to get the
                // underlying type the method is defined
                // on.
                Type* base = rt;
                if (base && (base->kind == TY_RC || base->kind == TY_REF
                             || base->kind == TY_RAWPTR))
                    base = base->inner;
                if (!base || !base->tdef) {
                    // Types that carry methods are exactly the
                    // ones with a nominal identity: user
                    // structs, user enums, extern structs, and
                    // the builtins. An unnamed type (raw
                    // pointer to nothing, a bare array, a ref
                    // to a non-nominal type) has no methods.
                    diag_error(bag, n->span,
                        "cannot call method on value of type %s",
                        rt ? type_name(rt) : "?");
                    n->type = ty_i64();
                    return n->type;
                }
                // Find the method: a FuncSig with a
                // matching method_owner_tdef and name.
                int midx = -1;
                for (int i = 0; i < sc->func_count; i++) {
                    FuncSig* s = &sc->funcs[i];
                    if (!s->method_owner_tdef) continue;
                    if (s->method_owner_tdef != base->tdef) continue;
                    if (!smap_tokens_equal(sc->smap, s->name,
                                           n->as.variant_ctor.variant_name))
                        continue;
                    midx = i;
                    break;
                }
                if (midx < 0) {
                    char nm[64];
                    smap_span_text(sc->smap,
                        n->as.variant_ctor.variant_name.span,
                        nm, sizeof nm);
                    diag_error(bag, n->span,
                        "no method '%s' on type %s",
                        nm, type_name(base));
                    n->type = ty_i64();
                    return n->type;
                }
                FuncSig* sig = &sc->funcs[midx];
                // Method ABI: the receiver is passed as
                // parameter 0 (this is what makes the
                // method call look like a free-function
                // call at the bytecode level). So the
                // user-visible arg count is
                // sig->param_count - 1.
                int extra    = n->as.variant_ctor.args.count;
                int expected = sig->param_count - 1;
                if (extra != expected)
                    diag_error(bag, n->span,
                        "method expects %d arg(s), got %d",
                        expected, extra);
                // Type-check the non-receiver args, skipping
                // the first param slot (receiver). Implicit
                // widening is allowed here, same as any other
                // call site.
                for (int i = 0; i < extra && i + 1 < sig->param_count; i++) {
                    AstNode* a = n->as.variant_ctor.args.items[i];
                    if (sig->param_types[i + 1])
                        pre_type_literal(a, sig->param_types[i + 1]);
                    Type* at = analyze_expr(sc, fl, a, bag);
                    if (at && sig->param_types[i + 1]) {
                        coerce_to_target(&n->as.variant_ctor.args.items[i],
                                         sig->param_types[i + 1],
                                         at, a->span, bag);
                    }
                }
                n->as.variant_ctor.mod_func_idx = midx;
                n->as.variant_ctor.mod_ffi_idx  = -1;
                n->as.variant_ctor.tdef_index   = -1;
                n->as.variant_ctor.tag          = -1;
                n->type = sig->return_type ? sig->return_type : ty_void();
                return n->type;
            }

            // ---- Module call or variant construction ----
            char tn_buf[64];
            smap_span_text(sc->smap, n->as.variant_ctor.type_name.span,
                           tn_buf, sizeof tn_buf);

            // Optional explicit `<...>` on the type name.
            // Resolution errors here are reported but the
            // result is only consulted below in the enum
            // path (the module path ignores it).
            Type* explicit_ty = NULL;
            if (n->as.variant_ctor.instantiation) {
                explicit_ty = type_expr_resolve(
                    n->as.variant_ctor.instantiation, sc->smap, sc->structs,
                    &sc->modtable, n->span.file_id, sc, bag);
            }

            // Module call: `mod.fn(args)` or `mod.fn<T>(args)`.
            // The resolver answers both halves in one call: it
            // identifies `mod` in the current file's imports,
            // then finds `fn` in that module's scope. For the
            // generic case it returns the template index, which
            // is what instantiation needs.
            ResolvedDecl rd = resolve_qualified(sc, n->span.file_id,
                n->as.variant_ctor.type_name,
                n->as.variant_ctor.variant_name);
            if (rd.kind != RD_NONE) {
                // Handle each resolution kind.
                int fidx = -1;
                if (rd.kind == RD_FUNC) {
                    fidx = rd.index;
                } else if (rd.kind == RD_TEMPLATE) {
                    if (!n->as.variant_ctor.instantiation) {
                        diag_error(bag, n->span,
                            "generic function requires explicit "
                            "type arguments: `%s.%s<T>(...)`",
                            tn_buf,
                            /* variant_name as text */ "");
                        n->type = ty_i64();
                        return n->type;
                    }
                    fidx = sema_instantiate_function_by_template(
                        sc, rd.index,
                        n->as.variant_ctor.variant_name,
                        n->as.variant_ctor.instantiation,
                        bag);
                }
                // (RD_TYPE in this position is a compile error:
                //  a type name cannot be called.)
                
                if (fidx < 0) {
                    // Instantiation failed; error already reported.
                    n->type = ty_i64();
                    return n->type;
                }
                // ... rest of the module-call path (pub check,
                //     argument checking, return type) uses `fidx`
                char vn[64];
                smap_span_text(sc->smap,
                    n->as.variant_ctor.variant_name.span,
                    vn, sizeof vn);
                if (!sc->funcs[fidx].is_pub) {
                    const char* def_path = "<unknown>";
                    SourceFile* sf = smap_get(sc->smap,
                        sc->funcs[fidx].file_id);
                    if (sf) def_path = sf->path;
                    diag_error(bag, n->span,
                        "'%s.%s' is private; declared in %s "
                        "and not marked 'pub'",
                        tn_buf, vn, def_path);
                    n->type = ty_i64();
                    return n->type;
                }

                // Arity and type check. Same rules as
                // the free-function call, except that
                // the resolution has already happened.
                FuncSig* sig = &sc->funcs[fidx];
                int n_args = n->as.variant_ctor.args.count;
                if (sig->is_extern) {
                    if (sig->is_variadic) {
                        if (n_args < sig->param_count)
                            diag_error(bag, n->span,
                                "extern '%s.%s' expects at least %d "
                                "argument(s), got %d",
                                tn_buf, vn, sig->param_count, n_args);
                    } else if (n_args != sig->param_count) {
                        diag_error(bag, n->span,
                            "extern '%s.%s' expects %d argument(s), got %d",
                            tn_buf, vn, sig->param_count, n_args);
                    }
                } else if (n_args != sig->param_count) {
                    diag_error(bag, n->span,
                        "function '%s.%s' expects %d argument(s), got %d",
                        tn_buf, vn, sig->param_count, n_args);
                }
                for (int i = 0; i < n_args; i++) {
                    AstNode* a = n->as.variant_ctor.args.items[i];
                    if (i < sig->param_count && sig->param_types[i])
                        pre_type_literal(a, sig->param_types[i]);
                    Type* at = analyze_expr(sc, fl, a, bag);
                    
                    if (i < sig->param_count && at && sig->param_types[i]) {
                        coerce_to_target(&n->as.variant_ctor.args.items[i],
                                         sig->param_types[i],
                                         at, a->span, bag);
                    }
                }
                // The mod_* fields distinguish a module
                // call from a variant construction for
                // codegen. For a module call, mod_func_idx
                // (or mod_ffi_idx for an extern) is set
                // and tag is -1.
                n->as.variant_ctor.mod_func_idx =
                    sig->is_extern ? -1 : fidx;
                n->as.variant_ctor.mod_ffi_idx =
                    sig->is_extern ? sig->ffi_idx : -1;
                n->type = sig->return_type ? sig->return_type : ty_i64();
                return n->type;
            }

            // ---- Enum variant construction path ----
            // The type_name did not resolve as a module, so
            // it should be an enum. Try, in order:
            //   1. The explicit `<...>` type, if present.
            //   2. A type named type_name in the current file.
            //   3. A selective import of type_name.
            //   4. The enclosing function's return type, if it
            //      is an RC-managed enum whose name starts
            //      with type_name (this is the "bare
            //      constructor" case for prelude types).
            TypeDef* td = NULL;
            Type* explicit_enum = explicit_ty;
            if (explicit_enum && explicit_enum->kind == TY_RC)
                explicit_enum = explicit_enum->inner;
            if (explicit_enum && explicit_enum->kind == TY_ENUM) {
                td = explicit_enum->tdef;
            } else {
                rd = resolve_name(sc, n->span.file_id,
                                  n->as.variant_ctor.type_name);
                td = resolved_tdef(sc, rd);

                // Inference for the common case: a generic
                // enum with one type parameter, called with
                // one payload argument. `Option.Some(5)` binds
                // T to the argument's type.
                if (!td && rd.kind == RD_TEMPLATE
                    && n->as.variant_ctor.args.count == 1) {
                    GenericTemplate* tmpl = mono_template_at(
                        sc->structs->generics, rd.index);
                    if (tmpl && tmpl->param_count == 1) {
                        AstNode* a = n->as.variant_ctor.args.items[0];
                        Type* at = analyze_expr(sc, fl, a, bag);
                        if (at) {
                            // Build `Template<at>` and instantiate.
                            TypeExpr* arg_te =
                                type_expr_resolved(at, n->span);
                            TypeExpr* g = type_expr_generic(
                                n->as.variant_ctor.type_name, n->span);
                            type_expr_generic_add_arg(g, arg_te);
                            Type* inst = type_expr_resolve(g, sc->smap,
                                sc->structs, &sc->modtable,
                                n->span.file_id, sc, bag);
                            type_expr_free(g);
                            if (inst) {
                                Type* inst_base = inst;
                                if (inst_base->kind == TY_RC)
                                    inst_base = inst_base->inner;
                                if (inst_base->kind == TY_ENUM)
                                    td = inst_base->tdef;
                            }
                        }
                    }
                }
            }

            if (!td) {
                // Fallback: infer the enum from the enclosing function's
                // annotated return type. For `fn f(): Option<i64>`, the
                // return type was resolved to rc<Option#i64>; the base
                // name (before '#') matches the variant's type name.
                //
                // The strncmp+term check ensures that e.g. a
                // return type of `rc<OptSomething>` does not
                // match a variant type name of `Opt` (the
                // next char must be '#' or NUL, i.e. the
                // return type name is exactly the variant
                // type name followed by the mangled args).
                if (fl->current_sig
                    && fl->current_sig->return_annotated
                    && fl->current_sig->return_type) {
                    Type* rt = fl->current_sig->return_type;
                    if (rt->kind == TY_RC && rt->inner
                        && rt->inner->kind == TY_ENUM) {
                            TypeDef* cand = rt->inner->tdef;
                            size_t tn_len = strlen(tn_buf);
                            // The fallback targets instantiated
                            // RC-managed enums (Result, Option, and
                            // user types built on them). Their
                            // mangled names have the form
                            // "Base#arg1#arg2...". Requiring the
                            // character right after the prefix to
                            // be '#' (not NUL) restricts the match
                            // to genuinely instantiated types,
                            // avoiding false positives on a
                            // non-generic type whose name happens
                            // to share a prefix.
                            if (strncmp(cand->name_buf, tn_buf, tn_len) == 0
                                && cand->name_buf[tn_len] == '#')
                                td = cand;
                    }
                }
            }

            if (!td) {
                // If the name resolved to a template, the
                // failure is inference, not lookup: the type
                // exists but its arguments cannot be inferred
                // from the arguments alone.
                //
                // The variable `rd` was declared in the
                // enclosing scope when the resolver ran; it
                // holds RD_TEMPLATE in this case.
                if (rd.kind == RD_TEMPLATE) {
                    diag_error(bag, n->span,
                        "cannot infer type arguments for '%s' "
                        "at this call site; annotate the binding, "
                        "the enclosing function's return type, or "
                        "use explicit `<...>`",
                        tn_buf);
                } else {
                    diag_error(bag, n->span,
                        "unknown type or module '%s'", tn_buf);
                }
                n->type = ty_i64();
                return n->type;
            }
            if (td->kind != TDEF_ENUM) {
                diag_error(bag, n->span,
                    "'%s' is a struct; use `%s { field: value, ... }`",
                    tn_buf, tn_buf);
                n->type = ty_i64();
                return n->type;
            }

            // Find the variant by name.
            int tag = -1;
            for (int i = 0; i < td->variant_count; i++) {
                if (smap_tokens_equal(sc->smap, td->variants[i].name,
                                      n->as.variant_ctor.variant_name)) {
                    tag = i; break;
                }
            }
            if (tag < 0) {
                char vn_buf[64];
                smap_span_text(sc->smap,
                    n->as.variant_ctor.variant_name.span,
                    vn_buf, sizeof vn_buf);
                diag_error(bag, n->span,
                    "enum '%s' has no variant '%s'",
                    tn_buf, vn_buf);
                n->type = ty_i64();
                return n->type;
            }

            EnumVariantDef* vd = &td->variants[tag];
            int n_args = n->as.variant_ctor.args.count;
            if (n_args != vd->payload_count)
                diag_error(bag, n->span,
                    "variant '%s.%s' takes %d payload(s), got %d",
                    tn_buf, vd->name_buf, vd->payload_count, n_args);

            // Payload type check, skipping excess args on
            // arity-mismatch. Note the pre_type_literal
            // call here — it is what lets `Some(5)` work
            // when Some expects u8.
            for (int i = 0; i < n_args && i < vd->payload_count; i++) {
                AstNode* a = n->as.variant_ctor.args.items[i];
                pre_type_literal(a, vd->payloads[i]);
                Type* at = analyze_expr(sc, fl, a, bag);
                if (at && vd->payloads[i]) {
                    coerce_to_target(&n->as.variant_ctor.args.items[i],
                                     vd->payloads[i],
                                     at, a->span, bag);
                }
            }

            n->as.variant_ctor.tag          = tag;
            n->as.variant_ctor.tdef_index   = (int)(td - sc->structs->items);
            n->as.variant_ctor.mod_func_idx = -1;
            n->as.variant_ctor.mod_ffi_idx  = -1;
            Type* base = ty_enum(td);
            n->type = ty_enum(td);
            return n->type;
        }

        // ----- Cast -----
        //
        // `expr as Type`. The set of legal casts is
        // deliberately narrow: numeric types and bool, in
        // any combination. Notably, casts between numeric
        // types and pointers are rejected, and casts
        // involving compound types are rejected. This is
        // intentional: pointer casts would be a footgun
        // without the escape analysis the language does not
        // have.
        //
        // The macro IS_NUM_OR_BOOL is undef'd after use to
        // avoid leaking into unrelated code later in the
        // switch; the file uses this pattern elsewhere
        // (see SRC_IS_INT_* in vm.c and KW in lexer.c).
        case AST_CAST: {
            Type* from = analyze_expr(sc, fl, n->as.cast.operand, bag);
            Type* to   = type_expr_resolve(n->as.cast.target_type_expr,
                                           sc->smap, sc->structs,
                                           &sc->modtable, n->span.file_id,
                                           sc, bag);
            n->as.cast.target_type = to;

            if (!from || !to) {
                n->type = to ? to : (from ? from : ty_i64());
                return n->type;
            }

            if (!cast_valid(from, to)) {
                diag_error(bag, n->span,
                    "cannot cast from %s to %s",
                    type_name(from), type_name(to));
            }
            n->type = to;
            return n->type;
        }

        case AST_SIZEOF: {
            Type* t = type_expr_resolve(n->as.sizeof_expr.te,
                                        sc->smap, sc->structs,
                                        &sc->modtable, n->span.file_id,
                                        sc, bag);
            if (!t) {
                n->type = ty_u64();
                return n->type;
            }
            if (t->kind == TY_VOID) {
                diag_error(bag, n->span, "sizeof(void) is not defined");
                n->type = ty_u64();
                return n->type;
            }
            if (t->kind == TY_EXTERN_STRUCT && t->tdef
                && t->tdef->is_opaque) {
                diag_error(bag, n->span,
                    "sizeof of opaque type '%s' is not defined; "
                    "use sizeof(*%s) for the pointer size",
                    t->tdef->name_buf, t->tdef->name_buf);
                n->type = ty_u64();
                return n->type;
            }
            
            n->as.sizeof_expr.resolved = t;
            n->as.sizeof_expr.size     = (I64)t->size;
            n->type = ty_u64();
            return n->type;
        }
        

        // ----- Result/Option propagation -----
        //
        // `expr?`: if expr evaluates to a "failure" variant
        // (tag != 0 in Result/Option), return from the
        // enclosing function with that failure value; else
        // yield the "success" payload (tag 0's payload).
        //
        // Constraints:
        //   - The enclosing function must have an annotated
        //     return type that resolves to an RC-managed
        //     enum (Result or Option). Without an annotation
        //     there is no way to check that the `?` result
        //     can be returned from here.
        //   - The operand must also be an RC-managed enum.
        //   - If both are Result (name starts with
        //     "Result"), their error payload types must
        //     match. This is what makes `?` type-safe
        //     across nested Results.
        //
        // The result type is the first variant's first
        // payload type (i.e. Ok's payload or Some's payload).
        // If the first variant has no payload (should not
        // happen for a well-formed Result/Option), the
        // result is void.
        //
        // Note: the check for "is this Result" uses
        // strncmp on the TypeDef's mangled name_buf. That is
        // how the compiler identifies the prelude's types
        // without a hard dependency on their file ids.
        case AST_TRY: {
            if (!fl->current_sig || !fl->current_sig->return_annotated) {
                diag_error(bag, n->span,
                    "`?` requires an annotated return type on the "
                    "enclosing function");
                n->type = ty_i64();
                return n->type;
            }
            Type* rt = fl->current_sig->return_type;
            if (!rt || rt->kind != TY_RC || !rt->inner
                || rt->inner->kind != TY_ENUM
                || !rt->inner->tdef->is_rc_managed) {
                diag_error(bag, n->span,
                    "`?` requires the enclosing function to return "
                    "Result<_, _> or Option<_>");
                n->type = ty_i64();
                return n->type;
            }

            Type* ot = analyze_expr(sc, fl, n->as.try_.operand, bag);
            if (!ot || ot->kind != TY_RC || !ot->inner
                || ot->inner->kind != TY_ENUM
                || !ot->inner->tdef->is_rc_managed) {
                diag_error(bag, n->span,
                    "`?` operand must be Result<_, _> or Option<_>, "
                    "found %s", ot ? type_name(ot) : "?");
                n->type = ty_i64();
                return n->type;
            }

            TypeDef* outer_td = rt->inner->tdef;
            TypeDef* inner_td = ot->inner->tdef;
            // "Both are Result" is decided by the name
            // prefix. This works because the mangled name
            // for a Result instantiation starts with the
            // literal "Result" (see mono_mangle in
            // monomorph.c).
            bool both_result = (strncmp(outer_td->name_buf, "Result", 6) == 0
                             && strncmp(inner_td->name_buf, "Result", 6) == 0);
            if (both_result
                && outer_td->variant_count >= 2
                && inner_td->variant_count >= 2
                && outer_td->variants[1].payload_count >= 1
                && inner_td->variants[1].payload_count >= 1) {
                // Compare the error payload types (variant 1's
                // first payload). Both are expected to be
                // type-equal; a mismatch means the `?` would
                // return a failure of the wrong type.
                Type* oe = outer_td->variants[1].payloads[0];
                Type* ie = inner_td->variants[1].payloads[0];
                if (!type_eq(oe, ie))
                    diag_error(bag, n->span,
                        "`?` cannot propagate error type %s "
                        "into a function returning %s",
                        type_name(ie), type_name(oe));
            }

            // The result type of `expr?` is the first
            // variant's first payload, or void if there is
            // none.
            Type* payload = ty_void();
            if (inner_td->variant_count >= 1
                && inner_td->variants[0].payload_count >= 1)
                payload = inner_td->variants[0].payloads[0];
            n->type = payload;
            return n->type;
        }

        default:
            // Every expression-producing AST kind should have
            // a case above; anything that reaches here is an
            // internal bug (likely a new node kind added
            // without a corresponding case).
            diag_error(bag, n->span,
                "internal: unexpected expression node");
            return NULL;
    }
}

// ============================================================
// Pattern analysis
//
// Patterns are the left-hand sides of match arms. The `T`
// argument is the scrutinee's type; every pattern is checked
// against it and, for binding patterns, a local slot is
// allocated.
//
// Per-kind behavior:
//   WILDCARD  — always matches; nothing to check.
//   BIND      — introduces a new local of type T. The slot
//               is written to pat_bind.slot.
//   INT       — T must be an integer type and the literal
//               must fit in T.
//   BOOL      — T must be bool.
//   STRING    — T must be string.
//   VARIANT   — T must be an (possibly RC-wrapped) enum.
//               The variant name is resolved against the
//               enum; the payload count must match the
//               subpattern count; each subpattern is
//               checked against the corresponding payload
//               type. For each payload slot, an anonymous
//               temporary local is declared (its slot is
//               written to tmp_slots[i]); the subpattern
//               then binds or matches against that local.
//
// The temporary locals are what make destructuring work at
// runtime: codegen loads each payload into its temp slot,
// then runs the subpattern test against that slot (which is
// either another variant test, a literal compare, or a bind
// store).
//
// Note that the AST_PAT_VARIANT case ignores the pattern's
// `type_name` on the success path: the type is already known
// from the scrutinee. The type_name is only consulted to
// catch the case where the pattern names a type that does
// not match the scrutinee, and to allow the fallback to a
// different-but-compatible TypeDef (e.g. the pattern names
// `Option` and the scrutinee is `Option<i64>`'s instantiation
// — the base-name prefix match accepts it).
// ============================================================
static void analyze_pattern(SemaCtx* sc, FnLocal* fl, AstNode* p,
                            Type* T, DiagnosticBag* bag) {

// Bare variant rewriting. If the scrutinee is an enum (or
// rc<enum>) and the pattern is a bare identifier whose
// name matches one of the enum's variants, treat it as a
// nullary variant pattern rather than a bind. The parser
// cannot make this decision (it does not know the
// scrutinee's type); sema can, because it has T.
//
// The rewrite produces an AST_PAT_VARIANT with a zero
// type_name, which the AST_PAT_VARIANT case below treats
// as "use the scrutinee's enum".
if (p->kind == AST_PAT_BIND) {
    Type* scrut = T;
    if (scrut && scrut->kind == TY_RC) scrut = scrut->inner;
    if (scrut && scrut->kind == TY_ENUM) {
        TypeDef* td = scrut->tdef;
        for (int v = 0; v < td->variant_count; v++) {
            if (smap_tokens_equal(sc->smap, td->variants[v].name,
                                  p->as.pat_bind.name)) {
                Token saved = p->as.pat_bind.name;
                p->kind = AST_PAT_VARIANT;
                p->as.pat_variant.type_name     = (Token){0};
                p->as.pat_variant.variant_name  = saved;
                ast_list_init(&p->as.pat_variant.subpatterns);
                p->as.pat_variant.tag           = -1;
                p->as.pat_variant.sub_count     = 0;
                p->as.pat_variant.instantiation = NULL;
                // tmp_slots is left as the xcalloc'd zeros;
                // the variant-payload loop below only writes
                // the first sub_count entries.
                break;
            }
        }
    }
    }
    switch (p->kind) {
        case AST_PAT_WILDCARD:
            break;

        case AST_PAT_BIND:
            // Introduce a new local. The type is the
            // scrutinee's; holds_rc is false because the
            // binding is a new name for the value, not a
            // new reference. is_mut is false: pattern
            // bindings are immutable today. (This is a
            // design choice; a `mut x` pattern form could
            // be added, and would set this to true.)
            p->as.pat_bind.slot = declare(fl, p->as.pat_bind.name,
                                          T, false, false);
            if (p->as.pat_bind.slot < 0)
                diag_error(bag, p->span, "too many locals");
            break;

        case AST_PAT_INT:
            if (!is_integer_type(T))
                diag_error(bag, p->span,
                    "integer pattern doesn't match type %s", type_name(T));
            else if (!int_value_fits(p->as.pat_int.value, T))
                diag_error(bag, p->span,
                    "pattern value out of range for %s", type_name(T));
            break;

        case AST_PAT_BOOL:
            if (T->kind != TY_BOOL)
                diag_error(bag, p->span,
                    "bool pattern doesn't match type %s", type_name(T));
            break;

        case AST_PAT_STRING:
            if (T->kind != TY_STRING)
                diag_error(bag, p->span,
                    "string pattern doesn't match type %s", type_name(T));
            break;

        case AST_PAT_VARIANT: {
            // Unwrap one level of RC. All RC-managed enums
            // (Result, Option) appear as rc<enum> at use
            // sites; the pattern sees the underlying enum.
            Type* T2 = T;
            if (T2->kind == TY_RC) T2 = T2->inner;

            if (T2->kind != TY_ENUM) {
                diag_error(bag, p->span,
                    "variant pattern doesn't match type %s", type_name(T));
                return;
            }
            TypeDef* td = T2->tdef;

            // If the pattern carried explicit `<...>`, resolve
            // it and confirm it names the same enum. This
            // check catches a mismatch like
            // `Some<i64>(x)` matching an `Option<u8>`
            // scrutinee.
            if (p->as.pat_variant.instantiation) {
                Type* inst_ty = type_expr_resolve(
                    p->as.pat_variant.instantiation,
                    sc->smap, sc->structs, &sc->modtable,
                    p->span.file_id, sc, bag);
                Type* inst_enum = inst_ty;
                if (inst_enum && inst_enum->kind == TY_RC)
                    inst_enum = inst_enum->inner;
                if (inst_enum && inst_enum->kind == TY_ENUM
                    && inst_enum->tdef != td) {
                    diag_error(bag, p->span,
                        "variant pattern type does not match scrutinee");
                    return;
                }
            }

            // Type-name resolution. A zero type_name means
            // the pattern was written in the bare form
            // (`Some(x)`); the scrutinee's type supplies the
            // enum. A non-zero type_name must match the
            // scrutinee's enum, either exactly or by
            // base-name prefix (for the generic case).
            // Qualified-name check. If the pattern wrote
            // `Some.Enum` (with a real type name), verify that
            // the name actually matches the scrutinee's enum.
            // A zero type_name is the bare form and skips
            // this check.
            if (p->as.pat_variant.type_name.span.file_id != 0) {
                char pn_buf[64];
                smap_span_text(sc->smap, p->as.pat_variant.type_name.span,
                               pn_buf, sizeof pn_buf);
                size_t pn_len = strlen(pn_buf);
                const char* base = td->name_buf;
                bool base_match = (strncmp(base, pn_buf, pn_len) == 0
                                   && (base[pn_len] == '#'
                                       || base[pn_len] == '\0'));
                if (!base_match) {
                    ResolvedDecl rd = resolve_name(sc, p->span.file_id,
                                                   p->as.pat_variant.type_name);
                    if (rd.kind != RD_TYPE && rd.kind != RD_BUILTIN_TYPE) {
                        diag_error(bag, p->span,
                            "unknown type in variant pattern");
                        return;
                    }
                    TypeDef* pt = resolved_tdef(sc, rd);
                    if (pt != td) {
                        diag_error(bag, p->span,
                            "variant pattern names type '%s' but "
                            "scrutinee is '%s'",
                            pt ? pt->name_buf : "?",
                            td->name_buf);
                        return;
                    }
                }
            }

            // Find the variant by name.
            int tag = -1;
            for (int i = 0; i < td->variant_count; i++) {
                if (smap_tokens_equal(sc->smap, td->variants[i].name,
                                      p->as.pat_variant.variant_name)) {
                    tag = i; break;
                }
            }
            if (tag < 0) {
                char vn[64];
                smap_span_text(sc->smap,
                    p->as.pat_variant.variant_name.span, vn, sizeof vn);
                diag_error(bag, p->span,
                    "enum '%s' has no variant '%s'", td->name_buf, vn);
                return;
            }
            p->as.pat_variant.tag = tag;

            // Subpattern count must equal the variant's
            // payload count.
            int want = td->variants[tag].payload_count;
            int got  = p->as.pat_variant.subpatterns.count;
            if (want != got) {
                diag_error(bag, p->span,
                    "variant '%s' takes %d payload(s), pattern has %d",
                    td->variants[tag].name_buf, want, got);
                return;
            }

            // For each subpattern, declare a temporary
            // local to hold the payload, then recurse. The
            // temporary is what codegen will load the
            // payload into before running the subpattern's
            // test.
            for (int i = 0; i < got; i++) {
                int tmp = declare_anon(fl, td->variants[tag].payloads[i]);
                if (tmp < 0) {
                    diag_error(bag, p->span, "too many locals");
                    break;
                }
                p->as.pat_variant.tmp_slots[i] = tmp;
                analyze_pattern(sc, fl,
                                p->as.pat_variant.subpatterns.items[i],
                                td->variants[tag].payloads[i], bag);
            }
            p->as.pat_variant.sub_count = got;
            break;
        }

        default:
            diag_error(bag, p->span,
                "internal: unexpected pattern node");
            break;
    }
}

// ============================================================
// Exhaustiveness
//
// After all arms have been analyzed, check that they cover
// every possible value of the scrutinee's type. Coverage is
// tracked shallowly: a wildcard or bind pattern covers
// everything, and each variant/bool literal marks one slot.
//
// The check is *sound but incomplete*: it accepts a wildcard
// arm as "covers everything" even if the wildcard appears
// after an arm that would have shadowed it (unreachable
// arms are not diagnosed). It also does not look inside
// nested patterns — a match whose arms are all variant
// patterns covering the top level but whose subpatterns are
// not exhaustive is accepted, because codegen runs the test
// top-down and a non-exhaustive subpattern just fails to
// match that arm and falls through to the next.
//
// So the guarantee is: if this function reports no error,
// the *top-level* discrimination is exhaustive. Runtime
// non-exhaustiveness can still occur if all arms'
// top-level tests pass but a subpattern fails; that is
// caught by the OP_TRAP at the end of the match in codegen.
// ============================================================
static void check_exhaustive(Type* T, AstList* arms, Span where,
                             const SourceMap* smap, DiagnosticBag* bag) {
    (void)smap;
    // Unwrap rc<> for the same reason as in analyze_pattern.
    if (T && T->kind == TY_RC) T = T->inner;

    bool has_wild = false;
    // 128 is the same hard cap on variants that appears in
    // analyze_expr's struct field check. A bool uses indices
    // 0 and 1.
    bool covered[128] = {false};

    for (int i = 0; i < arms->count; i++) {
        AstNode* pat = arms->items[i]->as.match_arm.pattern;
        switch (pat->kind) {
            case AST_PAT_WILDCARD:
            case AST_PAT_BIND:
                has_wild = true;
                break;
            case AST_PAT_VARIANT:
                if (pat->as.pat_variant.tag >= 0
                    && pat->as.pat_variant.tag < 128)
                    covered[pat->as.pat_variant.tag] = true;
                break;
            case AST_PAT_BOOL:
                covered[pat->as.pat_bool.value ? 1 : 0] = true;
                break;
            default: break;
        }
    }
    // A wildcard arm is unconditionally sufficient.
    if (has_wild) return;

    if (T->kind == TY_ENUM) {
        TypeDef* td = T->tdef;
        for (int i = 0; i < td->variant_count; i++) {
            if (!covered[i]) {
                diag_error(bag, where,
                    "match is not exhaustive: variant '%s' not covered",
                    td->variants[i].name_buf);
                return;
            }
        }
    } else if (T->kind == TY_BOOL) {
        if (!covered[0] || !covered[1])
            diag_error(bag, where,
                "match on bool is not exhaustive: missing '%s'",
                !covered[0] ? "false" : "true");
    } else {
        // Any non-enum, non-bool scrutinee is only exhaustive
        // if there's a wildcard (which we already checked for)
        // or if it's a pattern kind that implicitly covers
        // everything. Reaching here means the user is matching
        // on a type that the coverage analysis does not
        // understand (e.g. an integer type with literal
        // arms); the diagnostic tells them to add a `_`.
        diag_error(bag, where,
            "match on %s is not exhaustive; add a '_' arm", type_name(T));
    }
}

// ============================================================
// Statement analysis
//
// analyze_stmt walks a statement node and, in the course of
// doing so, recurses into its nested statements. It is the
// second-largest function in the file. The same contract as
// analyze_expr applies, but statements do not return a type;
// instead they annotate their nodes (local slots, resolved
// types, etc.) as a side effect.
//
// The switch handles every statement kind, including the
// "pass-through" node kinds (AST_CONST, AST_STRUCT_DEF, ...)
// which are top-level-only and are ignored here if they
// appear inside a body (which they should not).
// ============================================================
static void analyze_stmt(SemaCtx* sc, FnLocal* fl, AstNode* n,
                         DiagnosticBag* bag) {
    if (!n) return;

    switch (n->kind) {
        // ----- Compound statement -----
        // Push a scope, walk each child statement, then pop.
        // The push/pop is what gives block-local variables
        // their block lifetime: a `let` inside the block
        // becomes invisible after exit_scope restores
        // local_count.
        case AST_BLOCK:
            if (enter_scope(fl, bag, n->span)) {
                for (int i = 0; i < n->as.block.stmts.count; i++)
                    analyze_stmt(sc, fl, n->as.block.stmts.items[i], bag);
                exit_scope(fl);
            }
            break;

        // ----- `let` -----
        //
        // Sequence:
        //   1. Resolve the annotation, if any. `at` is the
        //      annotated type, or NULL for inference.
        //   2. If annotated, pre-type the initializer's
        //      literal (this is what lets `let x: u8 = 5`
        //      work).
        //   3. Analyze the initializer.
        //   4. If the initializer is void, that is an error:
        //      you cannot bind the absence of a value.
        //   5. Decide the binding's type: annotated if
        //      present, else inferred from the initializer.
        //   6. Check annotation vs inferred.
        //   7. Decide whether the binding holds an RC
        //      reference. Two cases:
        //        - If the initializer is a local var
        //          reference, the binding inherits that
        //          local's holds_rc flag (it is a *move*
        //          in RC terms; the source local is not
        //          marked as dropped, so a subsequent
        //          assignment would over-count — see the
        //          note below).
        //        - Otherwise the binding owns whatever the
        //          initializer produced, so holds_rc is
        //          true whenever the type is RC-managed.
        //   8. Declare the local.
        //
        // NOTE: the "inherit the source's holds_rc" rule is
        // subtle. Consider:
        //     let a = make_string();   // a holds_rc = true
        //     let b = a;               // b holds_rc = a.holds_rc = true
        //     // at end of block, BOTH a and b are dropped.
        // The end-of-block drop loop in codegen (see cg_block)
        // iterates over lets and drops each one whose
        // holds_rc is true. That means this code correctly
        // emits ONE RC_INC for b (see AST_LET in codegen)
        // and the refcount ends up at 1 after the initial
        // creation, so dropping both a and b is correct.
        // The AST_LET codegen block's `RC_INC` handles the
        // shared ownership.
        case AST_LET: {
            Type* at = NULL;
            if (n->as.let.type_expr)
                at = type_expr_resolve(n->as.let.type_expr, sc->smap,
                                       sc->structs, &sc->modtable,
                                       n->span.file_id, sc, bag);
            // extern structs have no value representation,
            // so a let binding to one is always wrong. Report
            // here — before analyzing the initializer — so the
            // primary error is the binding, not a downstream
            // type mismatch. Still analyze the initializer so
            // any independent error in it is visible, but
            // suppress the annotation-vs-init check by
            // declaring the local as i64.
            if (at && at->kind == TY_EXTERN_STRUCT) {
                diag_error(bag, n->span,
                    "cannot bind an extern struct value; extern "
                    "structs are C memory with no Prism value "
                    "representation. Use `*%s` to work with a "
                    "pointer.",
                    type_name(at));
                if (n->as.let.init)
                    analyze_expr(sc, fl, n->as.let.init, bag);
                n->as.let.type = at;
                n->type        = at;
                int s = declare(fl, n->as.let.name, ty_i64(),
                                false, n->as.let.is_mut);
                if (s >= 0) n->local_index = s;
                break;
            }
            
            AstNode* init = n->as.let.init;
            
            if (at) pre_type_literal(init, at);

            Type* it = analyze_expr(sc, fl, init, bag);

            //it's not wrong but redundant
            //if (init->type) {
            //    coerce_to_target(&n->as.let.init, at, it,
            //                     init->span, bag);
            //}

            if (it && it->kind == TY_VOID) {
                diag_error(bag, init->span,
                    "cannot bind a void value to a variable");
                it = ty_i64();
            }

            Type* bt = it ? it : ty_i64();

            if (at) {
                // Annotation present: the binding's type is
                // the annotation. If the initializer's type is
                // the same or safely widens, a cast is
                // inserted when the widening crosses the
                // int↔float domain; otherwise an error is
                // reported at the initializer's span.
                if (it) {
                    coerce_to_target(&n->as.let.init, at, it,
                                     init->span, bag);
                }
                bt = at;
            }

            n->as.let.type = bt;
            n->type = bt;
            // becomes dead code for the annotated case 
            // but is still reachable when the annotation is absent 
            // and the initializer somehow produces a bare extern struct 
            // kept as a defensive path
            if (bt->kind == TY_EXTERN_STRUCT) {
                diag_error(bag, n->span,
                    "cannot bind an extern struct value; use "
                    "*%s to work with a pointer to C memory",
                    type_name(bt));
                bt = ty_i64();
                n->as.let.type = bt;
                n->type = bt;
            }

            // holds_rc classification, described above.
            // Every RC-typed local owns its reference. Binding an
            // RC-typed view INC's the value; binding a fresh value
            // claims the fresh block. Scope exit emits a DEC either
            // way. No special case per initializer shape; the shape
            // only affects whether codegen INC's at bind time.
            n->holds_rc = bt && (bt->kind == TY_RC || bt->kind == TY_STRING);

            int s = declare(fl, n->as.let.name, bt, holds_rc,
                            n->as.let.is_mut);
            if (s < 0) {
                diag_error(bag, n->span, "too many locals");
                break;
            }
            n->local_index = s;
            break;
        }

        // ----- Assignment -----
        //
        // Five target shapes:
        //   AST_VAR   — a local or a const. Const is
        //               rejected; local is checked for
        //               mutability and type.
        //   AST_DEREF — a deref. Must be through a mutable
        //               reference (ref<mut T> or rawptr).
        //   AST_INDEX — a container index. The target
        //               expression must be a mutable
        //               lvalue (see is_mutable_lvalue);
        //               strings are rejected (immutable);
        //               arrays are allowed.
        //   AST_FIELD — a struct field. Same lvalue
        //               mutability check.
        //   anything else — not an lvalue.
        //
        // The AST_VAR case also computes the *holds_rc*
        // interaction: because the target is a local, we
        // know its slot, its existing RC ownership, and we
        // can copy those into the assignment node so codegen
        // knows to drop the old value before storing. See
        // AST_ASSIGN in codegen.c.
        case AST_ASSIGN: {
            AstNode* tgt = n->as.assign.target;
            AstNode* val = n->as.assign.value;

            // -- Target is a local or const --
            if (tgt->kind == AST_VAR) {
                int i = resolve(fl, sc->smap, tgt->as.var.name);
                if (i >= 0) {
                    tgt->local_index = fl->locals[i].slot;
                    tgt->const_id    = -1;
                    tgt->type        = fl->locals[i].type;
                    tgt->holds_rc    = fl->locals[i].holds_rc;
                    n->local_index   = fl->locals[i].slot;
                    n->holds_rc      = fl->locals[i].holds_rc;

                    // Mutability check on the target.
                    if (!fl->locals[i].is_mut) {
                        char buf[128];
                        name_text(sc->smap, tgt->as.var.name, buf, sizeof buf);
                        diag_error(bag, tgt->span,
                            "cannot assign to immutable binding '%s'; "
                            "declare with 'let mut'", buf);
                    }

                    Type* want = fl->locals[i].type;
                    pre_type_literal(val, want);
                    Type* got  = analyze_expr(sc, fl, val, bag);
                    if (want && got) {
                        coerce_to_target(&n->as.assign.value,
                                         want, got, n->span, bag);
                    }
                    break;
                }
                // Not a local. Try const: `const MAX = 5; MAX = 6;`
                // is an error with a specific message.
                int c = consts_resolve(sc->consts, sc->smap,
                                       tgt->as.var.name);
                if (c >= 0) {
                    char buf[128];
                    name_text(sc->smap, tgt->as.var.name, buf, sizeof buf);
                    diag_error(bag, tgt->span,
                        "cannot assign to constant '%s'", buf);
                    analyze_expr(sc, fl, val, bag);
                    break;
                }
                // Undefined identifier. Report and continue.
                char buf[128];
                name_text(sc->smap, tgt->as.var.name, buf, sizeof buf);
                diag_error(bag, tgt->span,
                    "assignment to undefined variable '%s'", buf);
                analyze_expr(sc, fl, val, bag);
                break;
            }

            // -- Target is a deref: `*p = v` --
            if (tgt->kind == AST_DEREF) {
                // Resolve the operand's type.
                Type* pt = analyze_expr(sc, fl, tgt->as.deref.operand, bag);
                tgt->type = pt ? pt->inner : NULL;

                if (pt) {
                    if (pt->kind == TY_REF && !pt->is_mut)
                        diag_error(bag, tgt->span,
                            "cannot assign through an immutable reference; "
                            "use `&mut x` and `ref<mut T>`");
                    else if (pt->kind != TY_REF
                             && pt->kind != TY_RAWPTR
                             && pt->kind != TY_RC)
                        diag_error(bag, tgt->span,
                            "cannot assign through a value of type %s",
                            type_name(pt));
                }

                Type* want = pt ? pt->inner : NULL;
                pre_type_literal(val, want);
                Type* got  = analyze_expr(sc, fl, val, bag);
                if (want && got) {
                    coerce_to_target(&n->as.assign.value,
                                     want, got, n->span, bag);
                }
                break;
            }

            // -- Target is an index: `a[i] = v` --
            if (tgt->kind == AST_INDEX) {
                Type* tt = analyze_expr(sc, fl, tgt->as.index.target, bag);
                Type* it = analyze_expr(sc, fl, tgt->as.index.index, bag);

                // Lvalue mutability: walk to the root binding.
                if (!is_mutable_lvalue(sc, fl, tgt->as.index.target)) {
                    Token root = lvalue_root_name(tgt->as.index.target);
                    char buf[128] = "?";
                    if (root.span.file_id != 0)
                        name_text(sc->smap, root, buf, sizeof buf);
                    diag_error(bag, tgt->as.index.target->span,
                        "cannot assign to index of immutable binding '%s'",
                        buf);
                }

                if (it && !is_integer_type(it))
                    diag_error(bag, tgt->as.index.index->span,
                        "array index must be an integer, found %s",
                        type_name(it));

                // Strings are not mutable.
                if (tt && tt->kind == TY_STRING) {
                    diag_error(bag, tgt->as.index.target->span,
                        "strings are immutable; cannot assign to s[i]");
                    analyze_expr(sc, fl, val, bag);
                    break;
                }

                // Compute the element type the value must
                // match.
                Type* want = NULL;
                if (tt) {
                    if (tt->kind == TY_ARRAY) {
                        want = tt->inner;
                    } else if (tt->kind == TY_RC && tt->inner
                               && tt->inner->kind == TY_ARRAY) {
                        want = tt->inner->inner;
                    } else {
                        diag_error(bag, tgt->as.index.target->span,
                            "cannot index-assign into type %s",
                            type_name(tt));
                    }
                }

                // Mirror the AST_ASSIGN_OP handler: the target node
                // carries its resolved element type so codegen can pick
                // the right descriptor for RC handling.
                tgt->type = want;
                Type* got;
                pre_type_literal(val, want);
                got = analyze_expr(sc, fl, val, bag);
                if (want && got) {
                    coerce_to_target(&n->as.assign.value,
                                     want, got, n->span, bag);
                }
                break;
            }

            // -- Target is a field: `s.f = v` --
            if (tgt->kind == AST_FIELD) {
                if (!is_mutable_lvalue(sc, fl, tgt->as.field.target)) {
                    Token root = lvalue_root_name(tgt->as.field.target);
                    char buf[128] = "?";
                    if (root.span.file_id != 0)
                        name_text(sc->smap, root, buf, sizeof buf);
                    diag_error(bag, tgt->as.field.target->span,
                        "cannot assign to field of immutable binding '%s'",
                        buf);
                }

                // Re-analyzing the target resolves owner,
                // offset, and declared_type.
                Type* want = analyze_expr(sc, fl, tgt, bag);

                // Assigning to a nested-by-value extern struct
                // field is rejected: `p.inner` is a computed
                // address, not a storage cell of its own; the
                // user must assign to individual leaf fields.
                if (tgt->as.field.owner
                    && tgt->as.field.owner->kind == TY_EXTERN_STRUCT
                    && tgt->as.field.declared_type
                    && tgt->as.field.declared_type->kind == TY_EXTERN_STRUCT) {
                    diag_error(bag, tgt->span,
                        "cannot assign to a nested extern struct "
                        "field; assign to its individual fields");
                    analyze_expr(sc, fl, val, bag);
                    break;
                }

                pre_type_literal(val, want);
                Type* got  = analyze_expr(sc, fl, val, bag);
                if (want && got) {
                    coerce_to_target(&n->as.assign.value,
                                     want, got, n->span, bag);
                }
                break;
            }

            // Anything else is not a valid lvalue.
            diag_error(bag, tgt->span, "invalid assignment target");
            analyze_expr(sc, fl, val, bag);
            break;
        }

        case AST_ASSIGN_OP: {
            AstNode* tgt = n->as.assign_op.target;
            AstNode* val = n->as.assign_op.value;
            BinOp    op  = n->as.assign_op.op;

            // Determine the target's type and mutability.
            // Reuses the same target-shape analysis as
            // AST_ASSIGN; the only difference is what comes
            // after the type is known.
            Type* want = NULL;

            if (tgt->kind == AST_VAR) {
                int i = resolve(fl, sc->smap, tgt->as.var.name);
                if (i >= 0) {
                    tgt->local_index = fl->locals[i].slot;
                    tgt->const_id    = -1;
                    tgt->type        = fl->locals[i].type;
                    tgt->holds_rc    = fl->locals[i].holds_rc;
                    n->local_index   = fl->locals[i].slot;
                    n->holds_rc      = fl->locals[i].holds_rc;
                    if (!fl->locals[i].is_mut) {
                        char buf[128];
                        name_text(sc->smap, tgt->as.var.name, buf, sizeof buf);
                        diag_error(bag, tgt->span,
                            "cannot assign to immutable binding '%s'; "
                            "declare with 'let mut'", buf);
                    }
                    want = fl->locals[i].type;
                } else {
                    int c = consts_resolve(sc->consts, sc->smap,
                                           tgt->as.var.name);
                    if (c >= 0) {
                        char buf[128];
                        name_text(sc->smap, tgt->as.var.name, buf, sizeof buf);
                        diag_error(bag, tgt->span,
                            "cannot assign to constant '%s'", buf);
                    } else {
                        char buf[128];
                        name_text(sc->smap, tgt->as.var.name, buf, sizeof buf);
                        diag_error(bag, tgt->span,
                            "assignment to undefined variable '%s'", buf);
                    }
                    analyze_expr(sc, fl, val, bag);
                    n->type = ty_void();
                    break;
                }
            } else if (tgt->kind == AST_DEREF) {
                Type* pt = analyze_expr(sc, fl, tgt->as.deref.operand, bag);
                tgt->type = pt ? pt->inner : NULL;
                if (pt) {
                    if (pt->kind == TY_REF && !pt->is_mut)
                        diag_error(bag, tgt->span,
                            "cannot assign through an immutable reference");
                    else if (pt->kind != TY_REF
                             && pt->kind != TY_RAWPTR
                             && pt->kind != TY_RC)
                        diag_error(bag, tgt->span,
                            "cannot assign through a value of type %s",
                            type_name(pt));
                }
                want = pt ? pt->inner : NULL;
            } else if (tgt->kind == AST_INDEX) {
                Type* tt = analyze_expr(sc, fl, tgt->as.index.target, bag);
                Type* it = analyze_expr(sc, fl, tgt->as.index.index, bag);
                if (!is_mutable_lvalue(sc, fl, tgt->as.index.target)) {
                    Token root = lvalue_root_name(tgt->as.index.target);
                    char buf[128] = "?";
                    if (root.span.file_id != 0)
                        name_text(sc->smap, root, buf, sizeof buf);
                    diag_error(bag, tgt->as.index.target->span,
                        "cannot assign to index of immutable binding '%s'",
                        buf);
                }
                if (it && !is_integer_type(it))
                    diag_error(bag, tgt->as.index.index->span,
                        "index must be an integer, found %s", type_name(it));
                if (tt) {
                    if (tt->kind == TY_STRING) {
                        diag_error(bag, tgt->as.index.target->span,
                            "strings are immutable; cannot assign to s[i]");
                    } else if (tt->kind == TY_ARRAY) {
                        want = tt->inner;
                    } else if (tt->kind == TY_RC && tt->inner
                               && tt->inner->kind == TY_ARRAY) {
                        want = tt->inner->inner;
                    } else {
                        diag_error(bag, tgt->as.index.target->span,
                            "cannot index-assign into type %s", type_name(tt));
                    }
                }
                tgt->type = want;
            } else if (tgt->kind == AST_FIELD) {
                if (!is_mutable_lvalue(sc, fl, tgt->as.field.target)) {
                    Token root = lvalue_root_name(tgt->as.field.target);
                    char buf[128] = "?";
                    if (root.span.file_id != 0)
                        name_text(sc->smap, root, buf, sizeof buf);
                    diag_error(bag, tgt->as.field.target->span,
                        "cannot assign to field of immutable binding '%s'",
                        buf);
                }
                want = analyze_expr(sc, fl, tgt, bag);
            } else {
                diag_error(bag, tgt->span, "invalid assignment target");
                analyze_expr(sc, fl, val, bag);
                n->type = ty_void();
                break;
            }

            // Value side. The right-hand operand may widen to
            // the target's type; a cast is inserted if the
            // widening crosses int↔float. After coercion the
            // operator check below sees the widened type.
            if (want) pre_type_literal(val, want);
            Type* got = analyze_expr(sc, fl, val, bag);
            if (want && got && !type_eq(want, got)) {
                if (type_widens_to(got, want)) {
                    coerce_to_target(&n->as.assign_op.value,
                                     want, got, n->span, bag);
                } else {
                    diag_error(bag, n->span,
                        "cannot apply %s to %s and %s",
                        op == BIN_ADD ? "+=" : op == BIN_SUB ? "-="
                      : op == BIN_MUL ? "*=" : "/=",
                        type_name(want), type_name(got));
                }
            }

            // Operator validity. Same rules as the binary form.
            if (want && !binary_result_type(op, want)) {
                diag_error(bag, n->span,
                    "operator '%s=' is not defined for %s",
                    binop_name(op), type_name(want));
            }

            // RC-managed target: allocate a temp for the old
            // value so codegen can drop it after computing the
            // new one. Only `+=` on string qualifies today.
            if (want
                && (want->kind == TY_RC || want->kind == TY_STRING)) {
                int tmp = declare_anon(fl, want);
                if (tmp < 0)
                    diag_error(bag, n->span, "too many locals");
                n->as.assign_op.temp_slot = tmp;
            }

            n->type = ty_void();
            break;
        }
        

        // ----- `if` -----
        //
        // Condition must be bool. Both branches are analyzed
        // independently; there is no "both branches must have
        // the same type" rule because `if` is a statement,
        // not an expression, in the current language.
        case AST_IF: {
            Type* ct = analyze_expr(sc, fl, n->as.if_.cond, bag);
            if (ct && ct->kind != TY_BOOL)
                diag_error(bag, n->as.if_.cond->span,
                    "condition must be bool, found %s", type_name(ct));
            analyze_stmt(sc, fl, n->as.if_.then_, bag);
            analyze_stmt(sc, fl, n->as.if_.else_, bag);
            break;
        }

        // ----- `while` -----
        //
        // Same condition check as `if`. loop_depth is
        // incremented around the body so break/continue are
        // allowed inside.
        case AST_WHILE: {
            Type* ct = analyze_expr(sc, fl, n->as.while_.cond, bag);
            if (ct && ct->kind != TY_BOOL)
                diag_error(bag, n->as.while_.cond->span,
                    "condition must be bool, found %s", type_name(ct));
            fl->loop_depth++;
            analyze_stmt(sc, fl, n->as.while_.body, bag);
            fl->loop_depth--;
            break;
        }

        // ----- `for i in a..b` -----
        //
        // The two bounds must be integers of the same type.
        // The loop variable's type is the bounds' type
        // (default i64 if the bounds did not resolve).
        //
        // Two slots are declared in the loop's scope:
        //   var_slot — the loop variable
        //   end_slot — an anonymous cache of the end
        //              expression, so it is evaluated once
        //              (the language does not say whether
        //              the end is re-evaluated; the current
        //              implementation caches, matching Go's
        //              `for i := 0; i < n; i++` where n is
        //              evaluated once).
        //
        // Note the analysis of the body: it walks the
        // block's statements *directly*, without calling
        // analyze_stmt(AST_BLOCK). That is because the
        // enclosing AST_FOR_RANGE already opened a scope;
        // recursing into a nested AST_BLOCK would push a
        // second scope, which is not wrong (it would just
        // tighten the lifetime of the body's own locals) but
        // is not what the body is.
        case AST_FOR_RANGE: {
            AstNode* start_node = n->as.for_range.start;
            AstNode* end_node   = n->as.for_range.end;

            // Bidirectional literal adaptation. If one bound is
            // an untyped numeric literal and the other is not,
            // analyze the non-literal first and pre-type the
            // literal to its type. Without this, `for i in 0..a`
            // fails when `a` is anything other than i64: `0`
            // would take the default i64 and the two bounds
            // would be reported as mismatched.
            //
            // Same shape as the corresponding logic in
            // AST_BINARY and (via pre_type_literal) AST_LET.
            bool start_lit = is_adaptive_literal(start_node);
            bool end_lit   = is_adaptive_literal(end_node);

            Type* st;
            Type* et;
            if (start_lit && !end_lit) {
                et = analyze_expr(sc, fl, end_node, bag);
                pre_type_literal(start_node, et);
                st = analyze_expr(sc, fl, start_node, bag);
            } else if (end_lit && !start_lit) {
                st = analyze_expr(sc, fl, start_node, bag);
                pre_type_literal(end_node, st);
                et = analyze_expr(sc, fl, end_node, bag);
            } else {
                st = analyze_expr(sc, fl, start_node, bag);
                et = analyze_expr(sc, fl, end_node, bag);
            }

            if (st && !is_integer_type(st))
                diag_error(bag, start_node->span,
                    "range start must be an integer, found %s",
                    type_name(st));
            if (et && !is_integer_type(et))
                diag_error(bag, end_node->span,
                    "range end must be an integer, found %s",
                    type_name(et));
            if (st && et && !type_eq(st, et))
                diag_error(bag, n->span,
                    "range start and end have different types: %s and %s",
                    type_name(st), type_name(et));

            Type* ct = is_integer_type(st) ? st : ty_i64();

            if (!enter_scope(fl, bag, n->span)) break;
                        int var_slot = declare(fl, n->as.for_range.var, ct, false, true);
            int end_slot = declare_anon(fl, ct);
            if (var_slot < 0 || end_slot < 0)
                diag_error(bag, n->span, "too many locals");

            n->as.for_range.var_slot = var_slot;
            n->as.for_range.end_slot = end_slot;

            fl->loop_depth++;
            AstNode* body = n->as.for_range.body;
            for (int i = 0; i < body->as.block.stmts.count; i++)
                analyze_stmt(sc, fl, body->as.block.stmts.items[i], bag);
            fl->loop_depth--;

            exit_scope(fl);
            break;
        }

        // ----- `for x in iterable` -----
        //
        // The iterable may be an array, rc<array>, or
        // string. Four slots are declared:
        //   arr_slot — the iterable value, cached
        //   len_slot — the array's length, cached
        //   idx_slot — the loop index
        //   var_slot — the current element
        //
        // As with for-range, the body's block statements are
        // walked directly, not via analyze_stmt(AST_BLOCK);
        // see the comment on the for-range case.
        case AST_FOR_IN: {
            Type* it = analyze_expr(sc, fl, n->as.for_in.iterable, bag);

            Type* arr_t  = NULL;
            Type* elem_t = NULL;
            if (it) {
                if (it->kind == TY_ARRAY) {
                    arr_t = it; elem_t = it->inner;
                } else if (it->kind == TY_RC && it->inner
                           && it->inner->kind == TY_ARRAY) {
                    arr_t = it->inner; elem_t = it->inner->inner;
                } else if (it->kind == TY_STRING) {
                    // Iterating a string yields u8 bytes.
                    arr_t = ty_string(); elem_t = ty_u8();
                } else {
                    diag_error(bag, n->as.for_in.iterable->span,
                        "cannot iterate over a value of type %s",
                        type_name(it));
                }
            }
            // Fallback types so the rest of the analysis
            // can proceed even after an error.
            if (!arr_t)  arr_t  = ty_array(ty_i64());
            if (!elem_t) elem_t = ty_i64();

            if (!enter_scope(fl, bag, n->span)) break;
            int arr_slot = declare_anon(fl, arr_t);
            int len_slot = declare_anon(fl, ty_i64());
            int idx_slot = declare_anon(fl, ty_i64());
                        int var_slot = declare(fl, n->as.for_in.var, elem_t, false, true);
            if (arr_slot < 0 || len_slot < 0 || idx_slot < 0 || var_slot < 0)
                diag_error(bag, n->span, "too many locals");

            n->as.for_in.arr_slot = arr_slot;
            n->as.for_in.len_slot = len_slot;
            n->as.for_in.idx_slot = idx_slot;
            n->as.for_in.var_slot = var_slot;

            fl->loop_depth++;
            AstNode* body = n->as.for_in.body;
            for (int i = 0; i < body->as.block.stmts.count; i++)
                analyze_stmt(sc, fl, body->as.block.stmts.items[i], bag);
            fl->loop_depth--;

            exit_scope(fl);
            break;
        }

        // ----- `break` / `continue` -----
        //
        // Both require an enclosing loop. The check is on
        // loop_depth, which the loop cases above increment.
        case AST_BREAK:
            if (fl->loop_depth == 0)
                diag_error(bag, n->span, "'break' outside of a loop");
            break;

        case AST_CONTINUE:
            if (fl->loop_depth == 0)
                diag_error(bag, n->span, "'continue' outside of a loop");
            break;

        // ----- `match` -----
        //
        // Two scopes are involved:
        //   - An outer scope for the cached scrutinee.
        //   - An inner scope per arm, for the arm's
        //     pattern bindings.
        //
        // The scrutinee is evaluated once and stored in an
        // anonymous local (hidden_slot). Each arm re-reads
        // it from that slot rather than re-evaluating.
        //
        // The order of operations is:
        //   1. Analyze the scrutinee.
        //   2. Enter the match scope, declare the hidden
        //      slot.
        //   3. For each arm, enter a fresh arm scope,
        //      analyze the pattern against the scrutinee's
        //      type, analyze the body, exit the arm scope.
        //   4. Check exhaustiveness.
        //   5. Exit the match scope.
        //
        // The exhaustiveness check runs after all arms are
        // analyzed, so it sees their resolved tags.
        case AST_MATCH: {
            Type* S = analyze_expr(sc, fl, n->as.match.scrutinee, bag);
            if (!S) S = ty_i64();
            if (S->kind == TY_VOID) {
                diag_error(bag, n->span, "cannot match on a void value");
                break;
            }

            if (!enter_scope(fl, bag, n->span)) break;
            int hidden = declare_anon(fl, S);
            if (hidden < 0) diag_error(bag, n->span, "too many locals");
            n->as.match.hidden_slot = hidden;

            for (int i = 0; i < n->as.match.arms.count; i++) {
                AstNode* arm = n->as.match.arms.items[i];
                if (!enter_scope(fl, bag, arm->span)) continue;
                analyze_pattern(sc, fl, arm->as.match_arm.pattern, S, bag);
                analyze_stmt(sc, fl, arm->as.match_arm.body, bag);
                exit_scope(fl);
            }

            check_exhaustive(S, &n->as.match.arms, n->span,
                             sc->smap, bag);
            exit_scope(fl);
            n->type = ty_void();
            break;
        }

        // ----- `return` -----
        //
        // The most involved check in the file. Multiple
        // rules interact:
        //   1. A bare `return` (no value) is only legal in
        //      a void function; a function with a declared
        //      non-void return type rejects it.
        //   2. The returned value's type must match the
        //      function's return type.
        //   3. Certain types cannot be returned:
        //        - ref<T>: would dangle.
        //        - rawptr: rejected in v1.
        //        - [T]: array values are inline and could
        //          not be copied out.
        //        - struct / enum: same reason, use rc<...>.
        //   4. RC-managed values must be *owned* by the
        //      return site, not borrowed from a local or
        //      a field. This prevents returning a
        //      reference to a value that will be dropped
        //      when the function returns.
        //
        // The "borrowed check" (rule 4) is not a full
        // escape analysis. It is a simple pattern:
        //   - AST_VAR with holds_rc == false: borrowed.
        //   - AST_FIELD / AST_INDEX / AST_DEREF: borrowed.
        //   - anything else: considered owned.
        // This is why `return some_local.string_field;`
        // is an error even in cases where the local could
        // outlive the return; the language is
        // conservative here.
        //
        // The return-type unification for *inferred*
        // functions is where the "previous return
        // establishes the type" behavior lives: the first
        // value-return sets sig->return_type, subsequent
        // ones must match.
        case AST_RETURN: {
            FuncSig* sig = fl->current_sig;
            if (!sig) break;

            // Bare `return;`.
            if (!n->as.ret.value) {
                if (sig->return_type && sig->return_type->kind != TY_VOID)
                    diag_error(bag, n->span,
                        "cannot return without a value; function returns %s",
                        type_name(sig->return_type));
                break;
            }

            AstNode* rv = n->as.ret.value;
            if (sig->return_annotated && sig->return_type)
                pre_type_literal(rv, sig->return_type);
                

            Type* got = analyze_expr(sc, fl, rv, bag);
            if (!got) break;

            // Rules 3a: returnable-by-type checks.
            if (got->kind == TY_REF) {
                diag_error(bag, rv->span,
                    "function cannot return a reference; "
                    "use rc<T> for values that outlive this function");
                break;
            }
            if (got->kind == TY_RAWPTR) {
                diag_error(bag, rv->span,
                    "raw pointers cannot be returned from functions in v1");
                break;
            }
            if (got->kind == TY_ARRAY) {
                diag_error(bag, rv->span,
                    "function cannot return an array value; "
                    "use rc<[...]> for arrays that outlive this function");
                break;
            }
            if (got->kind == TY_STRUCT) {
                diag_error(bag, rv->span,
                    "function cannot return a struct value; "
                    "use rc<StructName> for structs that outlive this function");
                break;
            }
            if (got->kind == TY_ENUM) {
                diag_error(bag, rv->span,
                    "function cannot return an enum value; "
                    "use rc<EnumName> for enums that outlive this function");
                break;
            }

            // Rule 2: type agreement. Three cases:
            //   - Annotated: must match the annotation.
            //   - Unannotated, no previous return: this
            //     return sets the type.
            //   - Unannotated, previous return was void:
            //     inconsistent (a bare `return` earlier,
            //     or an inference that already committed
            //     to void).
            //   - Unannotated, previous return was T: must
            //     match T.
            if (sig->return_annotated) {
                // Annotated: the return type is fixed, so
                // widening is allowed against it. A cast is
                // inserted if the widening crosses int↔float.
                if (sig->return_type && got) {
                    coerce_to_target(&n->as.ret.value,
                                     sig->return_type, got,
                                     n->span, bag);
                }
            } else if (sig->return_type == NULL) {
                // First value-return establishes the type.
                // No coercion: nothing to coerce to.
                sig->return_type = got;
            } else if (sig->return_type->kind == TY_VOID) {
                diag_error(bag, n->span,
                    "function is void but this return has type %s",
                    type_name(got));
            } else if (got && sig->return_type) {
                // Subsequent return: the previous return set
                // the type. Widen into it, same as the
                // annotated case.
                coerce_to_target(&n->as.ret.value,
                                 sig->return_type, got,
                                 n->span, bag);
            }

            // Rule 4: borrowed RC values cannot be
            // returned. See the function header for the
            // pattern used.
            if (got->kind == TY_RC || got->kind == TY_STRING) {
                bool borrowed = false;
                if (rv->kind == AST_VAR) {
                    borrowed = !rv->holds_rc;
                } else if (rv->kind == AST_FIELD) {
                    // `AST_FIELD` covers two distinct
                    // constructions:
                    //   - real field access (`p.x`), which is
                    //     a borrow from its target;
                    //   - unit-variant construction
                    //     (`Option.None`, `Color.Red`), which
                    //     produces a fresh value.
                    // `unit_tag >= 0` marks the second case.
                    borrowed = (rv->as.field.unit_tag < 0);
                } else if (rv->kind == AST_INDEX
                           || rv->kind == AST_DEREF) {
                    borrowed = true;
                }
                if (borrowed)
                    diag_error(bag, rv->span,
                        "cannot return a borrowed value; "
                        "return a fresh expression "
                        "(concatenate, allocate, or call a function "
                        "that produces a new value)");
            }
            break;
        }

        // ----- Expression statement -----
        //
        // Nothing special: analyze the expression for its
        // side effects on the node (types, resolutions).
        // Codegen decides whether to POP or RC_DEC the
        // result.
        case AST_EXPR_STMT:
            analyze_expr(sc, fl, n->as.expr_stmt.expr, bag);
            break;

        // Non-runtime top-level nodes; ignored by analyze_stmt.
        // These can appear in the program's decls list but
        // never inside a body; if they somehow reach here
        // (a parser or loader bug), they are silently
        // skipped rather than treated as errors.
        case AST_CONST:
        case AST_STRUCT_DEF:
        case AST_ENUM_DEF:
        case AST_STRUCT_FIELD:
        case AST_ENUM_VARIANT:
        case AST_TYPE_EXPR_NODE:
        case AST_MATCH_ARM:
        case AST_STRUCT_INIT:
        case AST_USING:
        case AST_EXTERN_LIB:
        case AST_PROGRAM:
            break;

        default:
            diag_error(bag, n->span,
                "internal: unexpected statement node");
            break;
    }
}

// ============================================================
// Pre-scan for value-return presence
//
// Recursive predicate: does this subtree contain a `return
// <expr>` (as opposed to a bare `return`)?
//
// Used by Pass A to decide whether a function with no return
// annotation should be inferred as void or left for inference
// from its body. A function with no value returns is
// immediately void; one with value returns has its return
// type inferred during Pass B from the first value return.
//
// Scope: it looks only at statement-level constructs. It does
// not descend into expressions (which cannot contain
// returns). It also does not descend into nested function
// definitions (there are none in the current language).
//
// The scan is shallow in another sense: it returns true as
// soon as it finds any value return, so a function whose
// return type is only inferred on one path still counts as
// "has value returns" and gets inference rather than being
// force-voided. That is the correct behavior: an
// unannotated function with a value return must get a
// non-void type.
// ============================================================
static bool has_value_return(AstNode* n) {
    if (!n) return false;
    switch (n->kind) {
        case AST_RETURN:
            return n->as.ret.value != NULL;
        case AST_BLOCK:
            for (int i = 0; i < n->as.block.stmts.count; i++)
                if (has_value_return(n->as.block.stmts.items[i]))
                    return true;
            return false;
        case AST_IF:
            // Both branches; a value return in either counts.
            return has_value_return(n->as.if_.then_) ||
                   has_value_return(n->as.if_.else_);
        case AST_WHILE:
            return has_value_return(n->as.while_.body);
        case AST_FOR_RANGE:
            return has_value_return(n->as.for_range.body);
        case AST_FOR_IN:
            return has_value_return(n->as.for_in.body);
        case AST_MATCH:
            // Every arm's body.
            for (int i = 0; i < n->as.match.arms.count; i++)
                if (has_value_return(
                        n->as.match.arms.items[i]->as.match_arm.body))
                    return true;
            return false;
        default:
            return false;
    }
}

// ============================================================
// Constant literal extraction
//
// Convert an AST expression into a Constant, or report an
// error. Only literals are accepted today: integers, floats,
// and booleans. String constants are explicitly rejected with
// a message that says so, rather than being silently
// unsupported.
//
// The output parameter is written only on success. On
// failure, a diagnostic has been reported and the caller is
// expected to skip the declaration.
//
// Note that this is the only place const-ness is decided.
// The parser accepts any expression as a const initializer;
// sema is the layer that narrows it to a literal. If a
// future revision wants compile-time constant folding, this
// is where the extension goes: a `const_fold(AstNode*) ->
// Constant` function called in place of the current switch.
// ============================================================
static bool const_from_literal(AstNode* expr, Constant* out,
                               DiagnosticBag* bag) {
    switch (expr->kind) {
        case AST_INT:
            out->kind = CONST_I64;
            out->as.i64 = (I64)expr->as.int_lit.value;
            return true;
        case AST_BOOL:
            out->kind = CONST_BOOL;
            out->as.b = expr->as.bool_lit.value;
            return true;
        case AST_FLOAT:
            out->kind = CONST_F64;
            out->as.f64 = expr->as.float_lit.value;
            return true;
        case AST_UNARY:
            // `-literal`. The parser produces
            // AST_UNARY(UN_NEG, AST_INT|AST_FLOAT); without
            // this case, `const X: i32 = -1` errors with
            // "constant initializer must be a literal".
            if (expr->as.unary.op == UN_NEG && expr->as.unary.operand) {
                AstNode* op = expr->as.unary.operand;
                if (op->kind == AST_INT) {
                    out->kind = CONST_I64;
                    out->as.i64 = -(I64)op->as.int_lit.value;
                    return true;
                }
                if (op->kind == AST_FLOAT) {
                    out->kind = CONST_F64;
                    out->as.f64 = -op->as.float_lit.value;
                    return true;
                }
            }
            break;
        case AST_STRING:
            diag_error(bag, expr->span,
                "string constants are not yet supported");
            return false;
        default:
            break;
    }
    diag_error(bag, expr->span,
        "constant initializer must be a literal "
        "(integer, float, or boolean)");
    return false;
}
// ============================================================
// Per-function body analysis
//
// Sets up a fresh FnLocal, declares the parameters as locals,
// walks the body, and finalizes the function's return type.
//
// The parameter declaration is what places parameters at
// slots 0..param_count-1. That ordering is the function ABI:
// the caller pushes arguments left-to-right, and the callee
// reads them from slots 0..param_count-1 (see the OP_CALL /
// OP_RET pair in vm.c).
//
// The final "inferred void" fallback is important: a function
// with no value returns and no annotation gets ty_void(), so
// calls to it type as void. Without this, the call-site
// return-type inference would fail with "cannot infer return
// type" for every such function.
//
// Note that this can only be called once per function. If it
// were called twice, locals would be re-declared and
// parameter slots would be wrong. The codebase never does
// this; the check exists only as a comment.
// ============================================================
void analyze_func_body(SemaCtx* sc, FuncSig* sig, AstNode* fn,
                              DiagnosticBag* bag) {
    FnLocal fl;
    memset(&fl, 0, sizeof fl);
    fl.current_sig  = sig;
    fl.current_file = fn->span.file_id;

    if (!enter_scope(&fl, bag, fn->span))
        return;

    for (int i = 0; i < fn->as.func.param_count; i++) {
        Param* p = &fn->as.func.params[i];
        // Parameters default to immutable (is_mut from the
        // Param's own flag). holds_rc is false: parameters
        // do not own their arguments; the caller does.
        // (This is a design decision; some languages copy
        // or move at the call boundary. Prism's model is
        // that parameters are borrowed.)
        if (declare(&fl, p->name, p->type, false, p->is_mut) < 0)
            diag_error(bag, p->name.span, "too many parameters/locals");
    }

    analyze_stmt(sc, &fl, fn->as.func.body, bag);

    // If the function was not annotated and no value return
    // established a type, it is void.
    if (!sig->return_annotated && sig->return_type == NULL)
        sig->return_type = ty_void();
    // Propagate the finalized return type back to the AST so
    // codegen sees the same value.
    fn->as.func.return_type = sig->return_type;

    exit_scope(&fl);
}


// Locate the prelude's file_id in the SourceMap. The prelude
// is registered by prelude_parse under the synthetic path
// "<prelude>". This helper finds it so the resolver can fall
// back to it for names not found in the caller's scope.
//
// Returns 0 if the prelude was not loaded. That is a valid
// state: a test harness or a REPL might run without a
// prelude. The resolver skips the prelude step when the
// field is 0, so no special-casing is needed at the call
// site.
static uint32_t find_prelude_file_id(const SourceMap* smap) {
    for (uint32_t i = 1; i < (uint32_t)smap->count; i++) {
        SourceFile* f = smap_get(smap, i);
        if (f && f->path && strcmp(f->path, "<prelude>") == 0)
            return i;
    }
    return 0;
}

// ============================================================
// Entry point
//
// The whole of sema, orchestrated. Order of operations is
// load-bearing and is spelled out here. Each pass assumes the
// previous one completed:
//
//   1. Zero SemaCtx and wire in the borrowed pointers.
//   2. Build the module table (namespace bindings from
//      `using` declarations). Required before any type
//      resolution, because qualified names resolve through
//      it.
//   3. Reset the instantiation cache. It is file-static and
//      must not leak between compilations.
//   4. Initialize the monomorphizer and install the
//      instantiate_generic callback on the type table. This
//      is what makes TEX_GENERIC resolution work: types.c
//      calls back into sema.
//   5. Build the library table from `extern lib` decls and
//      copy it into the FFITable's libs[] array. This must
//      happen before Pass A, because Pass A resolves
//      `from alias` clauses against it.
//   6. Pass 0: register nominal type names.
//   7. Pass 0b: resolve fields and variants.
//   8. Pass A: function signatures, extern registration,
//      and constants.
//   9. Validate selective imports (needs a populated
//      function table, hence after Pass A).
//  10. Pass B: function bodies.
//  11. Free the module table.
//
// The final return value is `!diag_has_errors(bag)`. Warnings
// do not affect it.
//
// Cleanup: on the success and failure paths alike, this
// function frees sc->modtable.files and zeroes the file_count
// / file_capacity. Everything else (funcs, structs, consts,
// ffi) is caller-owned and outlives the call. Note that the
// instantiation cache is *not* freed here; it is freed at the
// top of the next sema_analyze via cache_reset. A single
// compilation that never runs another sema_analyze will leak
// the cache's argument arrays at process exit; bounded and
// harmless, but worth knowing.
// ============================================================
bool sema_analyze(AstNode* program, const SourceMap* smap,
                  ConstTable* consts, TypeDefTable* structs,
                  DiagnosticBag* bag, FFITable* ffi,
                  SemaCtx* sc) {
    // Step 1: zero and wire up. memset of the whole SemaCtx
    // is safe because every field is a scalar or a
    // fixed-size array; nothing inside owns heap.
    memset(sc, 0, sizeof *sc);
    sc->smap    = smap;
    sc->consts  = consts;
    sc->structs = structs;
    sc->prelude_file_id = find_prelude_file_id(smap);

    // Step 2: module table. Reads `using` declarations.
    build_module_table(&sc->modtable, program, bag);

    // Step 3: reset the global instantiation cache.
    cache_reset();

    // Step 4: monomorphizer hookup. `structs->generics` is
    // the opaque handle that types.c passes back to us in
    // the instantiate_generic callback. The callback is a
    // function pointer stored on the type table, so
    // resolving a TEX_GENERIC from types.c invokes
    // sema_instantiate_generic with `sc` as the ctx.
    //
    // Idempotency: if structs->generics is already non-NULL
    // (i.e. a previous sema_analyze on the same TypeDefTable
    // ran without freeing), we keep the existing handle
    // rather than leaking a second one. In normal usage
    // structs is fresh per compilation, so this is a
    // defensive path.
    if (!structs->generics)
        mono_init(&structs->generics);
    structs->instantiate_generic = sema_instantiate_generic;

    // Step 5: library table -> FFITable.libs[].
    //
    // The FFILib entries are copied into the caller-owned
    // FFITable. The VM will mutate them (loading the
    // library, storing the handle), but the compile-time
    // data (alias, path) is what sema writes here.
    //
    // The cap on `ffi->lib_count < 16` matches
    // LibraryTable.entries[16]; if the source had more
    // (which build_library_table already rejected), this
    // would drop the extras, but that path is not reachable
    // since both tables cap at 16.
    LibraryTable libtab;
    build_library_table(&libtab, program, smap, bag);

    for (int i = 0; i < libtab.count && ffi->lib_count < 16; i++) {
        FFILib* fl = &ffi->libs[ffi->lib_count++];
        memset(fl, 0, sizeof *fl);
        snprintf(fl->alias, sizeof fl->alias, "%s", libtab.entries[i].alias);
        snprintf(fl->path,  sizeof fl->path,  "%s", libtab.entries[i].path);
        fl->handle = NULL;
        fl->loaded = false;
    }

    // ---- Pass 0: register nominal types ----
    //
    // Only struct and enum definitions. Generics go to the
    // monomorphizer here rather than to the concrete table.
    // The order of decls is source order; a duplicate name
    // is caught in register_struct_or_enum.
    for (int i = 0; i < program->as.program.decls.count; i++) {
        AstNode* d = program->as.program.decls.items[i];
        if (d->kind == AST_STRUCT_DEF || d->kind == AST_ENUM_DEF)
            register_struct_or_enum(d, sc, bag);
    }

    // ---- Pass 0b: fields and variants ----
    //
    // Extern structs use their own two-pass resolver (all
    // field types first, then layout to a fixpoint) because
    // their C-ABI layout depends on nested extern structs
    // that may be declared later. Regular structs and enums
    // keep the existing single-pass resolver; they were
    // already order-independent because Pass 0 registered
    // every type name.
    resolve_extern_structs(program, sc, bag);

    for (int i = 0; i < program->as.program.decls.count; i++) {
        AstNode* d = program->as.program.decls.items[i];
        if (d->kind == AST_STRUCT_DEF
            && !d->as.struct_def.is_extern       // NEW
            && d->as.struct_def.sdef_index >= 0) {
            TypeDef* td = &sc->structs->items[d->as.struct_def.sdef_index];
            resolve_struct_fields(td, d, sc, bag);
        }
        if (d->kind == AST_ENUM_DEF && d->as.enum_def.tdef_index >= 0) {
            TypeDef* td = &sc->structs->items[d->as.enum_def.tdef_index];
            resolve_enum_variants(td, d, sc, bag);
        }
    }

    // Compute RC content propagation. A struct or enum whose
    // fields transitively hold RC-typed values becomes
    // RC-managed. Run after Pass 0b so every field and payload
    // type has been resolved.
    sema_resolve_rc_managing(structs);
    
    // ---- Pass A: signatures and constants ----
    //
    // A single walk over top-level decls. Order matters
    // within this pass:
    //
    //   a. Extern declarations are processed first (they
    //      appear in source order, but the branch is checked
    //      before the regular-function branch). This is what
    //      makes an extern function's FuncSig available to
    //      Pass B even if it is defined after its callers.
    //
    //   b. Regular functions: generic ones go to the
    //      monomorphizer; concrete ones get a FuncSig.
    //
    //   c. Constants: literals are converted and registered
    //      in the caller-owned ConstTable.
    //
    // Note that a *call* inside another function's body can
    // require instantiating a generic function, which
    // recursively calls back into sema. Those instantiations
    // append to sc->funcs (bounded by MAX_FUNCS) but are not
    // seen by the loop below, which only iterates the AST
    // decls list.
    for (int i = 0; i < program->as.program.decls.count; i++) {
        AstNode* d = program->as.program.decls.items[i];

        // -- Extern declarations --
        //
        // Full FFI type checking happens here: return type
        // and parameter types are resolved and translated
        // to FFIType codes, and the resulting FFIFunc is
        // registered in the FFITable. Any type that is not
        // FFI-compatible causes the function to be skipped
        // (with a diagnostic) rather than registered.
        if (d->kind == AST_FUNC && d->as.func.is_extern) {
            int lib_idx = library_table_find(&libtab, smap,
                                             d->as.func.lib_alias);
            if (lib_idx < 0) {
                char lbuf[64];
                smap_span_text(smap, d->as.func.lib_alias.span,
                               lbuf, sizeof lbuf);
                diag_error(bag, d->span,
                    "unknown library alias '%s'", lbuf);
                continue;
            }

            // Resolve the return type. The special case is
            // Option<*T>, which is not directly an FFI type
            // but is used to model nullable pointers. When
            // present, the FFI-level return type is FFI_PTR
            // and a flag is set so the VM wraps the result.
            Type* ret_ty = type_expr_resolve(
                d->as.func.return_type_expr, smap, structs, &sc->modtable,
                d->span.file_id, sc, bag);
            
            bool ret_is_optptr = is_option_ptr_type(ret_ty);
            FFIType ret_ffi = FFI_VOID;
            if (ret_is_optptr) {
                ret_ffi = FFI_PTR;
            } else if (ret_ty && ret_ty->kind == TY_EXTERN_STRUCT) {
                diag_error(bag, d->as.func.return_type_expr->span,
                    "extern functions cannot return an extern struct "
                    "by value; use `*%s`", type_name(ret_ty));
                continue;
            } else if (!ret_ty || !ffi_kind_from_type(ret_ty, &ret_ffi)) {
                if (ret_ty)
                    diag_error(bag, d->as.func.return_type_expr->span,
                        "type %s is not FFI-compatible", type_name(ret_ty));
                continue;
            }

            // Per-parameter resolution, with the same
            // Option<*T> special case. `ok` is the "did
            // any parameter fail?" flag; on failure we skip
            // the function entirely rather than registering
            // a partial FFIFunc.
            //
            // The arg_ty[] / arg_ffi[] / arg_is_optptr[]
            // arrays are sized 8, matching the parser's
            // parameter cap. The loop bound is the parser-
            // enforced param_count, so no overrun.
            bool ok = true;
            bool arg_is_optptr[8] = {false};
            FFIType arg_ffi[8];
            Type*   arg_ty[8];
            for (int j = 0; j < d->as.func.param_count; j++) {
                Param* p = &d->as.func.params[j];
                Type* pt = p->type_expr
                    ? type_expr_resolve(p->type_expr, smap, structs,
                                        &sc->modtable, d->span.file_id,
                                        sc, bag)
                    : ty_i64();
                arg_ty[j] = pt;
                p->type = pt;
                if (is_option_ptr_type(pt)) {
                    arg_is_optptr[j] = true;
                    arg_ffi[j] = FFI_PTR;
                    continue;
                }
                if (pt->kind == TY_EXTERN_STRUCT) {
                    diag_error(bag, p->name.span,
                        "extern functions cannot take an extern struct "
                        "by value; use `*%s`", type_name(pt));
                    ok = false;
                    continue;
                }
                
                if (!ffi_kind_from_type(pt, &arg_ffi[j])) {
                    diag_error(bag, p->name.span,
                        "parameter has type %s, which is not FFI-compatible",
                        type_name(pt));
                    ok = false;
                }
            }
            if (!ok) continue;

            // Register the FFIFunc. ff->symbol is the
            // function name text (what the OS loader will
            // look up); the rest of the fields are the
            // resolved types and flags.
            if (ffi->func_count >= 128) {
                diag_error(bag, d->span,
                    "too many extern functions (max 128)");
                continue;
            }
            int fidx = ffi->func_count++;
            FFIFunc* ff = &ffi->funcs[fidx];
            memset(ff, 0, sizeof *ff);
            smap_span_text(smap, d->as.func.name.span,
                           ff->symbol, sizeof ff->symbol);
            ff->lib_idx     = lib_idx;
            ff->is_variadic = d->as.func.is_variadic;
            ff->arg_count   = d->as.func.param_count;
            ff->ret_type    = ret_ffi;
            for (int j = 0; j < ff->arg_count; j++)
                ff->arg_types[j] = arg_ffi[j];
            ff->returns_option_ptr = ret_is_optptr;
            for (int j = 0; j < ff->arg_count; j++)
                ff->arg_is_option_ptr[j] = arg_is_optptr[j];

            // Record the FFI index on the AST node and set
            // up the FuncSig. Note that an extern function
            // gets BOTH an ffi_idx (for codegen to emit
            // OP_FFI_CALL) and a func_idx (for sema's
            // function table, so call sites can find it
            // during Pass B).
            d->as.func.ffi_idx  = fidx;
            d->as.func.func_idx = -1;   // set below

            d->as.func.return_type = ret_ty;

            if (sc->func_count >= MAX_FUNCS) {
                diag_error(bag, d->span,
                    "too many functions (max %d)", MAX_FUNCS);
                continue;
            }
            int sidx = sc->func_count++;
            FuncSig* sig = &sc->funcs[sidx];
            memset(sig, 0, sizeof *sig);
            sig->name             = d->as.func.name;
            sig->file_id          = d->span.file_id;
            sig->is_pub           = d->is_pub;
            sig->is_extern        = true;
            sig->is_variadic      = d->as.func.is_variadic;
            sig->ffi_idx          = fidx;
            sig->decl             = d;
            sig->param_count      = d->as.func.param_count;
            sig->return_type      = ret_ty;
            // Externs always have a resolved return type;
            // there is no inference.
            sig->return_annotated = true;
            for (int j = 0; j < sig->param_count; j++)
                sig->param_types[j] = arg_ty[j];
            smap_span_text(smap, d->as.func.name.span,
                           sig->name_buf, sizeof sig->name_buf);

            d->as.func.func_idx = sidx;
            continue;
        }

        // -- Regular functions --
        //
        // Generic: register as a template, do not add to
        // the function table. The instantiation will add
        // entries when a call site forces it.
        //
        // Non-generic: create a FuncSig. The parameters are
        // resolved here (so Pass B and cross-file callers
        // see them). The return type:
        //   - If annotated, resolve it.
        //   - If not annotated, this is where inference
        //     starts: has_value_return decides whether the
        //     function starts as void or as "unknown, fill
        //     in on first value return".
        //
        // Method detection: if method_owner is a real token
        // (file_id != 0), look up the owner TypeDef and
        // record it. Methods are not distinguished from
        // free functions in the funcs[] table; the
        // method_owner_tdef field is what codegen and the
        // method-call resolution path use to find them.
        if (d->kind == AST_FUNC && !d->as.func.is_extern) {
            if (d->as.func.type_param_count > 0) {
                mono_register_template(structs->generics, d, smap);
                continue;
            }
            if (sc->func_count >= MAX_FUNCS) {
                diag_error(bag, d->span,
                           "too many functions (max %d)", MAX_FUNCS);
                continue;
            }
            int idx = sc->func_count++;
            FuncSig* sig = &sc->funcs[idx];
            memset(sig, 0, sizeof *sig);
            sig->name        = d->as.func.name;
            sig->file_id     = d->span.file_id;
            sig->is_pub      = d->is_pub;
            sig->is_extern   = false;
            sig->is_variadic = false;
            sig->ffi_idx     = -1;
            sig->decl        = d;
            smap_span_text(smap, d->as.func.name.span,
                           sig->name_buf, sizeof sig->name_buf);

            for (int j = 0; j < d->as.func.param_count; j++) {
                Param* p = &d->as.func.params[j];
                // Unannotated parameter defaults to i64.
                // The convention is that a parameter without
                // a type is treated as i64 so that early
                // examples can omit annotations; a future
                // change to require annotations would
                // replace the else branch with a
                // diagnostic.
                sig->param_types[j] = p->type_expr
                    ? type_expr_resolve(p->type_expr, smap, structs,
                                        &sc->modtable, d->span.file_id,
                                        sc, bag)
                    : ty_i64();
                p->type = sig->param_types[j];
            }
            sig->param_count = d->as.func.param_count;

            if (d->as.func.return_type_expr) {
                sig->return_type = type_expr_resolve(
                    d->as.func.return_type_expr, smap, structs,
                    &sc->modtable, d->span.file_id, sc, bag);
                sig->return_annotated = true;

                if (sig->return_type
                    && sig->return_type->kind == TY_EXTERN_STRUCT) {
                    diag_error(bag, d->as.func.return_type_expr->span,
                        "function cannot return an extern struct "
                        "value; use *%s", type_name(sig->return_type));
                    sig->return_type = ty_void();
                }
                
                // ref<T> as a return type is rejected at the
                // signature level, so the diagnostic fires
                // once at the declaration rather than at
                // every return site. The actual "cannot
                // return a reference" check in
                // analyze_stmt's AST_RETURN case handles
                // the value-level rule.
                if (sig->return_type && sig->return_type->kind == TY_REF) {
                    diag_error(bag, d->as.func.return_type_expr->span,
                        "function cannot return a reference type; "
                        "use rc<T> for values that outlive this function");
                    sig->return_type = ty_void();
                }
            } else {
                // Inference seed: if the body has any value
                // return, leave return_type NULL so
                // Pass B fills it in. Otherwise, mark void
                // immediately (which also lets call sites
                // see the void return type before the body
                // is analyzed, avoiding some circular
                // inference).
                sig->return_annotated = false;
                sig->return_type = has_value_return(d->as.func.body)
                                 ? NULL : ty_void();
            }

            // Method owner resolution. A real token has a
            // non-zero file_id; a zero token means "not a
            // method" (see the Token zero-value convention
            // used throughout the compiler).
            if (d->as.func.method_owner.span.file_id != 0) {
                ResolvedDecl rd = resolve_name(sc, d->span.file_id,
                                               d->as.func.method_owner);
                TypeDef* owner = resolved_tdef(sc, rd);
                if (!owner) {
                    char buf[64];
                    smap_span_text(smap, d->as.func.method_owner.span,
                                   buf, sizeof buf);
                    diag_error(bag, d->span,
                        "unknown method owner type '%s'", buf);
                } else {
                    sig->method_owner_tdef = owner;
                }
            }

            // Duplicate-name check.
            //
            // Free functions: a name collides only within the
            // same file. Two files each declaring `helper` is
            // fine (they are separate modules and resolution
            // is per-file for free functions).
            //
            // Methods: a name collides on the same owner,
            // regardless of file. `Point.length` declared in
            // two files would be a genuine ambiguity at every
            // call site, and the second declaration would be
            // unreachable. Builtin methods fall in this case
            // too: `string.trim` belongs to the builtin
            // `string` TypeDef, and a user redeclaring it
            // shadows the stdlib version invisibly. Reject.
            for (int k = 0; k < idx; k++) {
                FuncSig* other = &sc->funcs[k];
                if (!smap_tokens_equal(smap, other->name,
                                       d->as.func.name))
                    continue;

                if (sig->method_owner_tdef) {
                    if (other->method_owner_tdef
                        != sig->method_owner_tdef)
                        continue;
                } else {
                    if (other->file_id != d->span.file_id) continue;
                    if (other->method_owner_tdef) continue;
                }

                char nbuf[64];
                smap_span_text(smap, d->as.func.name.span,
                               nbuf, sizeof nbuf);
                if (sig->method_owner_tdef) {
                    diag_error(bag, d->span,
                        "method '%s.%s' is already defined",
                        sig->method_owner_tdef->name_buf, nbuf);
                } else {
                    diag_error(bag, d->span,
                        "function '%s' is already defined in this file",
                        nbuf);
                }
                break;
            }
            

            d->as.func.func_idx    = idx;
            d->as.func.return_type = sig->return_type;
            continue;
        }

        // -- Constants --
        //
        // Only literal initializers are accepted (see
        // const_from_literal). The Constant value is
        // registered in the caller-owned table, and the
        // AST_CONST node gets a const_id so codegen can
        // find the entry.
        if (d->kind == AST_CONST) {
            AstNode* init = d->as.const_decl.initializer;
            Constant c;
            if (!const_from_literal(init, &c, bag)) continue;

            // Optional annotation. When present, the
            // annotation governs the const's type and its
            // literal is fit-checked against it.
            Type* annotated = NULL;
            if (d->as.const_decl.type_expr) {
                annotated = type_expr_resolve(
                    d->as.const_decl.type_expr, smap, structs,
                    &sc->modtable, d->span.file_id, sc, bag);
            }

            if (annotated && c.kind == CONST_I64) {
                if (!is_integer_type(annotated)) {
                    diag_error(bag, init->span,
                        "constant is an integer but annotated as %s",
                        type_name(annotated));
                } else {
                    // Determine the literal's U64 bit pattern
                    // for range checking. Positive literals
                    // store the bits directly; negated
                    // literals produce the two's-complement
                    // pattern of the negated I64.
                    U64 bits;
                    if (init->kind == AST_INT) {
                        bits = init->as.int_lit.value;
                    } else if (init->kind == AST_UNARY
                               && init->as.unary.op == UN_NEG
                               && init->as.unary.operand
                               && init->as.unary.operand->kind == AST_INT) {
                        bits = (U64)(-(I64)init->as.unary.operand
                                          ->as.int_lit.value);
                    } else {
                        bits = (U64)c.as.i64;
                    }
                    bool fits;
                    if (is_signed_int(annotated)) {
                        fits = int_value_fits((I64)bits, annotated);
                    } else {
                        fits = uint_value_fits(bits, annotated);
                    }
                    if (!fits) {
                        diag_error(bag, init->span,
                            "constant value does not fit in %s",
                            type_name(annotated));
                    }
                }
            } else if (annotated && c.kind == CONST_BOOL
                       && annotated->kind != TY_BOOL) {
                diag_error(bag, init->span,
                    "constant is bool but annotated as %s",
                    type_name(annotated));
            } else if (annotated && c.kind == CONST_F64
                       && annotated->kind != TY_F64) {
                diag_error(bag, init->span,
                    "constant is f64 but annotated as %s",
                    type_name(annotated));
            }

            Type* t;
            if (annotated)                 t = annotated;
            else if (c.kind == CONST_I64)  t = ty_i64();
            else if (c.kind == CONST_BOOL) t = ty_bool();
            else if (c.kind == CONST_F64)  t = ty_f64();
            else                           t = ty_i64();

            int idx = consts_declare(consts, d->as.const_decl.name,
                                     c, t, smap);
            if (idx == -1) {
                diag_error(bag, d->span,
                           "too many constants (max %d)", MAX_CONSTS);
                continue;
            }
            if (idx == -2) {
                char nbuf[64];
                smap_span_text(smap, d->as.const_decl.name.span,
                               nbuf, sizeof nbuf);
                diag_error(bag, d->span,
                           "duplicate constant '%s'", nbuf);
                continue;
            }
            d->const_id = idx;
            continue;
        }
    }

    // Step 9: selective-import validation. Runs between Pass A
    // and Pass B so that the function table is populated.
    validate_selective_imports(program, sc, bag);

    // ---- Pass B: bodies ----
    //
    // Every non-extern, non-generic function that was
    // successfully registered in Pass A gets its body
    // analyzed. The `func_idx < 0` check skips functions
    // that failed Pass A (e.g. an extern that had a bad
    // type); those have no FuncSig and no AST to analyze.
    //
    // Bodies are analyzed in source order. That matters for
    // the return-type inference of unannotated functions:
    // a function whose return type is inferred from its
    // body becomes visible to callers analyzed later in
    // the same pass. Callers analyzed earlier would see
    // return_type == NULL and report "cannot infer return
    // type at this call site".
    for (int i = 0; i < program->as.program.decls.count; i++) {
        AstNode* d = program->as.program.decls.items[i];
        if (d->kind != AST_FUNC) continue;
        if (d->as.func.is_extern) continue;
        if (d->as.func.type_param_count > 0) continue;
        if (d->as.func.func_idx < 0) continue;
        FuncSig* sig = &sc->funcs[d->as.func.func_idx];
        analyze_func_body(sc, sig, d, bag);
    }

    // Step 11: free the module table (owned by sema, not by
    // any caller).
    free(sc->modtable.files);
    sc->modtable.files = NULL;
    sc->modtable.file_count = 0;
    sc->modtable.file_capacity = 0;

    return !diag_has_errors(bag);
}