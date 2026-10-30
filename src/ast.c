// ============================================================
// ast.c — AST construction and destruction.
//
// Every node is allocated by alloc_node, which xcallocs so all
// fields start zeroed. Fields that need -1 as their "not set"
// value are written explicitly. Freeing walks every owned
// child pointer and every owned TypeExpr.
//
// Two structural facts about this file:
//
//   1. There is exactly one allocation site for AST nodes
//      (alloc_node). That makes it easy to reason about node
//      lifetime: a node is created here, is owned by exactly
//      one parent or list, and is released by ast_free.
//
//   2. There is exactly one destruction site (ast_free). Every
//      AST node is freed by the same switch. If a constructor
//      stores a heap pointer in a union member, the matching
//      ast_free case must release it; the two halves of this
//      file are paired by hand, so adding a new owned field to
//      any payload means editing both ends.
// ============================================================
#include "../include/ast.h"
#include <stdlib.h>
#include <string.h>

// ============================================================
// Allocation
// ============================================================

// Allocate and initialize one AST node. Called by every
// public constructor in this file.
//
// Why xcalloc: the union has many members with different field
// counts and offsets. Zeroing the whole node means a constructor
// only has to write the fields that must not be zero (typically
// -1 sentinels). Anything left untouched is 0/NULL/false, which
// is the intended "unset" value for pointers and counts, and is
// overridden explicitly for the integer fields that want -1.
//
// Fields set here rather than in individual constructors:
//   - kind and span: always set from arguments.
//   - is_pub = false: only some top-level constructors override.
//   - local_index = -1, const_id = -1: the two resolution
//     fields default to "unresolved" for every kind. Sema
//     overwrites the ones that are meaningful; the ones that
//     are not meaningful stay -1 forever, which is fine.
//   - tier = TIER_NONE: no allocation is implied by a bare
//     node. Sema promotes this on nodes that produce heap
//     values.
//
// is_builtin and holds_rc are left at xcalloc's 0 (false).
// They are only ever set true by sema, and a false default is
// correct.
static AstNode* alloc_node(AstKind k, Span span) {
    AstNode* n = (AstNode*)xcalloc(1, sizeof *n);
    n->kind        = k;
    n->span        = span;
    n->is_pub      = false;
    n->local_index = -1;
    n->const_id    = -1;
    n->tier        = TIER_NONE;
    return n;
}

// Public wrapper for alloc_node. The two exist so that callers
// outside ast.c (tests, one-off tools) can obtain a node without
// being able to reach the private static. No semantic
// difference.
AstNode* ast_alloc_node(AstKind k, Span span) {
    return alloc_node(k, span);
}

// ============================================================
// AstList
// ============================================================

// Zero-initialize a list. items is NULL, count and capacity 0.
// A list with capacity 0 is valid and means "not yet
// allocated"; the first push will allocate.
void ast_list_init(AstList* l) {
    l->items    = NULL;
    l->count    = 0;
    l->capacity = 0;
}

// Append one node. Ownership of `n` transfers to the list; the
// caller must not free it separately.
//
// Growth policy: start at 8, double when full. Same shape as
// diag_report's growth (see diagnostic.c); the choice is
// uniform across the compiler so the realloc pattern is easy
// to spot. Doubling keeps the amortized cost per push O(1).
//
// Note that `n` is stored by pointer, not copied. Two different
// lists must not hold the same node; doing so would double-free
// on ast_list_free.
void ast_list_push(AstList* l, AstNode* n) {
    if (l->count >= l->capacity) {
        l->capacity = l->capacity ? l->capacity * 2 : 8;
        l->items = (AstNode**)xrealloc(l->items,
            sizeof(AstNode*) * (size_t)l->capacity);
    }
    l->items[l->count++] = n;
}

// Splice every element of `src` in front of every element of
// `dst`, preserving order in both halves. Ownership of the src
// elements moves to dst; src is left empty (but still owns its
// items buffer, which the caller must eventually free).
//
// Used by the parser for the "prepend an already-built list to
// the front of the block we are building" pattern, which shows
// up when a construct (e.g. a desugaring step) has to insert
// statements before existing ones.
//
// Algorithm, step by step:
//   1. If src is empty, do nothing (avoids a pointless realloc
//      check and, more importantly, the loop below would be a
//      no-op anyway).
//   2. Ensure dst has room for dst->count + src->count. Note
//      that the growth target is new_count * 2 (not the old
//      capacity * 2), because the required size is already
//      known; doubling it leaves slack for future pushes.
//   3. Shift dst's existing elements to the right by src->count
//      positions. The loop runs *backwards* from the last
//      element to the first so that each element is copied to
//      a slot that has already been vacated (or was never
//      occupied). A forward loop would overwrite elements
//      before they were moved.
//   4. Copy src's elements into the newly freed slots [0,
//      src->count).
//   5. Update dst->count and clear src->count.
//
// The two loops in steps 3 and 4 are the classic "make a hole,
// then fill it" idiom; do not merge them into one pass, because
// the src and dst ranges may overlap in the underlying array
// when dst == src (which the parser never does today, but the
// reverse shift is what makes it correct if it ever did).
void ast_list_prepend_all(AstList* dst, AstList* src) {
    if (src->count == 0) return;
    int new_count = dst->count + src->count;
    if (new_count > dst->capacity) {
        dst->capacity = new_count * 2;
        dst->items = (AstNode**)xrealloc(dst->items,
            sizeof(AstNode*) * (size_t)dst->capacity);
    }
    for (int i = dst->count - 1; i >= 0; i--)
        dst->items[i + src->count] = dst->items[i];
    for (int i = 0; i < src->count; i++)
        dst->items[i] = src->items[i];
    dst->count = new_count;
    src->count = 0;   // ownership transferred
}

// Free every element recursively, free the items buffer, and
// reset the list to its zero state. Order matters: elements are
// freed before the array that holds their pointers, and the
// array is freed before ast_list_init zeroes the fields (so
// the free is not lost).
//
// ast_list_init at the end makes the list safe to reuse or to
// free again, which matters because some callers
// (ast_list_free inside ast_free) can run on a list that was
// already reset by a constructor.
void ast_list_free(AstList* l) {
    for (int i = 0; i < l->count; i++) ast_free(l->items[i]);
    free(l->items);
    ast_list_init(l);
}

// ============================================================
// Destruction
// ============================================================

// Recursively free one node. The switch dispatches on kind; for
// each kind, the case frees exactly the fields that the matching
// constructor (or sema) may have allocated.
//
// Two rules for maintaining this function:
//   - Every union member that holds an AstNode*, AstList, or
//     TypeExpr* must be freed here for the kind(s) that use it.
//     The cases below are grouped by which fields are owned,
//     not by kind number.
//   - Leaf kinds (literals, var, break, continue, patterns
//     without children) fall through to `free(n)` with no case
//     body. Adding a new leaf kind means adding it to the group
//     list; forgetting to does not leak, but it will make the
//     compiler warn about an unhandled enum value in some
//     configurations.
//
// Fields that are never owned and therefore never freed here:
//   - `type`: points into the global type table, owned by the
//     type system, freed elsewhere (or not at all — types are
//     interned).
//   - `span`: borrowed.
//   - Tokens: contain spans (borrowed) and a kind; no heap.
//   - The resolution scalars (local_index, const_id, etc.):
//     plain integers.
void ast_free(AstNode* n) {
    if (!n) return;

    switch (n->kind) {
        case AST_PROGRAM:
            // decls owns every top-level declaration. canonical_path
            // is a heap string set by the loader; free it if present
            // (free(NULL) is a no-op, so no guard needed).
            ast_list_free(&n->as.program.decls);
            free(n->as.program.canonical_path);
            break;

        case AST_FUNC:
            // Only the TypeExprs are owned. `body` is an AST_BLOCK
            // (or NULL for externs), recursively freed. `name`,
            // `method_owner`, `type_params[]`, and `lib_alias` are
            // Tokens (spans only, no heap). `return_type` and each
            // param's `type` point into the type table and are not
            // freed here.
            //
            // type_params[] is an inline array of Token, so there is
            // no loop for it; only the params need a per-element
            // free, and only for their type_expr fields (the Param
            // struct itself is inline in the parent).
            for (int i = 0; i < n->as.func.param_count; i++)
                type_expr_free(n->as.func.params[i].type_expr);
            type_expr_free(n->as.func.return_type_expr);
            ast_free(n->as.func.body);
            break;

        case AST_CONST:
            type_expr_free(n->as.const_decl.type_expr);
            // initializer is a full expression tree.
            ast_free(n->as.const_decl.initializer);
            break;

        case AST_USING: {
            // Three things may be owned depending on kind:
            //   - path: always allocated by ast_using_module and
            //     ast_using_selective. NULL for USING_ALIAS.
            //   - names: allocated lazily by
            //     ast_using_selective_add. NULL unless the
            //     selective form added at least one name.
            //   - alias_target: only owned in the USING_ALIAS
            //     case; the other two kinds leave it NULL.
            AstUsing* u = &n->as.using_decl;
            free(u->path);
            free(u->names);
            if (u->kind == USING_ALIAS) type_expr_free(u->alias_target);
            break;
        }

        case AST_STRUCT_DEF:
            // Only the fields list is owned. The type_params[]
            // array is inline Token storage.
            ast_list_free(&n->as.struct_def.fields);
            break;

        case AST_ENUM_DEF:
            ast_list_free(&n->as.enum_def.variants);
            break;

        case AST_STRUCT_FIELD:
            type_expr_free(n->as.struct_field.type_expr);
            break;

        case AST_ENUM_VARIANT:
            // freeing the payload here left the ones pointing at it in dangling state
            ast_list_free(&n->as.enum_variant.payload_exprs);
            //free(n->as.enum_variant.payloads);
            break;

        case AST_TYPE_EXPR_NODE:
            type_expr_free(n->as.type_expr_node.te);
            break;

        case AST_BLOCK:
            ast_list_free(&n->as.block.stmts);
            break;

        case AST_LET:
            type_expr_free(n->as.let.type_expr);
            ast_free(n->as.let.init);
            break;

        case AST_ASSIGN:
            ast_free(n->as.assign.target);
            ast_free(n->as.assign.value);
            break;
        case AST_ASSIGN_OP:
            ast_free(n->as.assign_op.target);
            ast_free(n->as.assign_op.value);
            break;
        case AST_IF:
            // else_ may be NULL (no else clause). ast_free(NULL)
            // is a no-op, so no explicit guard.
            ast_free(n->as.if_.cond);
            ast_free(n->as.if_.then_);
            ast_free(n->as.if_.else_);
            break;

        case AST_WHILE:
            ast_free(n->as.while_.cond);
            ast_free(n->as.while_.body);
            break;

        case AST_FOR_RANGE:
            ast_free(n->as.for_range.start);
            ast_free(n->as.for_range.end);
            ast_free(n->as.for_range.body);
            break;

        case AST_FOR_IN:
            ast_free(n->as.for_in.iterable);
            ast_free(n->as.for_in.body);
            break;

        case AST_BREAK:
        case AST_CONTINUE:
            // No owned children; the node itself is freed below.
            break;

        case AST_RETURN:
            // value may be NULL for a bare `return;`.
            ast_free(n->as.ret.value);
            break;

        case AST_EXPR_STMT:
            ast_free(n->as.expr_stmt.expr);
            break;

        case AST_INT:
        case AST_FLOAT:
        case AST_BOOL:
        case AST_STRING:
        case AST_VAR:
        case AST_PAT_WILDCARD:
        case AST_PAT_INT:
        case AST_PAT_BOOL:
        case AST_PAT_STRING:
        case AST_CSTRING:
        case AST_PAT_BIND:
            // All leaves. AST_STRING / AST_CSTRING / AST_PAT_STRING
            // carry a Token (span only); AST_PAT_BIND carries a
            // Token and a slot; nothing here owns heap.
            break;

        case AST_BINARY:
            ast_free(n->as.binary.left);
            ast_free(n->as.binary.right);
            break;

        case AST_UNARY:
            ast_free(n->as.unary.operand);
            break;

        case AST_CALL:
            // Args are a list of expressions; instantiation is an
            // optional TypeExpr for `f<T>`.
            ast_list_free(&n->as.call.args);
            type_expr_free(n->as.call.instantiation);
            break;

        case AST_CALL_INDIRECT:
            ast_free(n->as.call_indirect.callee);
            ast_list_free(&n->as.call_indirect.args);
            break;
            

        case AST_DEREF:
            ast_free(n->as.deref.operand);
            break;

        case AST_CAST:
            ast_free(n->as.cast.operand);
            type_expr_free(n->as.cast.target_type_expr);
            break;
        case AST_TRY:
            ast_free(n->as.try_.operand);
            break;
            

        case AST_ARRAY_LIT:
            ast_list_free(&n->as.array_lit.elements);
            break;

        case AST_INDEX:
            ast_free(n->as.index.target);
            ast_free(n->as.index.index);
            break;

        case AST_STRUCT_LIT:
            ast_list_free(&n->as.struct_lit.inits);
            type_expr_free(n->as.struct_lit.instantiation);
            break;

        case AST_STRUCT_INIT:
            ast_free(n->as.struct_init.value);
            break;

        case AST_FIELD:
            ast_free(n->as.field.target);
            break;

        case AST_VARIANT_CTOR:
            // Four owned members:
            //   args           — list of argument expressions
            //   instantiation  — optional `<T>` TypeExpr
            //   receiver_expr  — for methods only; NULL otherwise
            // mod_func_idx and mod_ffi_idx are indices, not
            // owned pointers.
            ast_list_free(&n->as.variant_ctor.args);
            type_expr_free(n->as.variant_ctor.instantiation);
            ast_free(n->as.variant_ctor.receiver_expr);
            break;

        case AST_ALLOC_ARENA:
            ast_free(n->as.alloc_arena.value);
            break;

        case AST_ALLOC_RC:
            ast_free(n->as.alloc_rc.value);
            break;
        case AST_SIZEOF:
            type_expr_free(n->as.sizeof_expr.te);
            break;
            
        // the child is the field access node; the address
        // operation itself owns no separate allocation.
        case AST_ADDR_OF_FIELD:
            ast_free(n->as.addr_of_field.field);
            break;
            

        case AST_MATCH:
            ast_free(n->as.match.scrutinee);
            ast_list_free(&n->as.match.arms);
            break;

        case AST_MATCH_ARM:
            ast_free(n->as.match_arm.pattern);
            ast_free(n->as.match_arm.body);
            break;

        case AST_PAT_VARIANT:
            ast_list_free(&n->as.pat_variant.subpatterns);
            type_expr_free(n->as.pat_variant.instantiation);
            break;
        case AST_EXTERN_LIB:
            // The payload reuses the using_decl slot but only
            // allocates `path` (a one-element Token array). The
            // other AstUsing fields are zeroed. So free only
            // `path` here, not `names` or `alias_target`.
            free(n->as.using_decl.path);
            break;
    }

    // Finally free the node itself. Note that this runs whether
    // or not a case above matched: the switch is only for the
    // owned children, not for the node.
    free(n);
}

// ============================================================
// Constructors
//
// Convention across the whole section:
//   - alloc_node sets kind, span, is_pub=false, local_index=-1,
//     const_id=-1, tier=TIER_NONE.
//   - Each constructor fills only the fields whose correct
//     "unset" value differs from xcalloc's 0, and any fields
//     that the parser expects to be meaningful immediately.
//   - Fields that only sema will fill (resolved types, offsets,
//     slot numbers, tags, table indices) are set to their -1
//     sentinel here so sema can check "have I already done
//     this?" without a separate bool.
// ============================================================

// Root. `decls` is initialized empty (the parser pushes into
// it). file_id and canonical_path are set by the loader, not
// here; the loader is responsible for keeping canonical_path
// alive.
AstNode* ast_program(void) {
    AstNode* n = alloc_node(AST_PROGRAM, SPAN_NONE);
    ast_list_init(&n->as.program.decls);
    n->as.program.file_id        = 0;
    n->as.program.canonical_path = NULL;
    return n;
}

// Function node. `body` may be NULL (extern decls, or a
// declaration-only fn during parsing). The Token fields left
// zeroed here are filled by the parser once the surrounding
// syntax has been parsed:
//   - method_owner, type_params[], type_param_count, params[],
//     param_count, return_type_expr, is_extern, is_variadic,
//     lib_alias
// The sema-filled fields (func_idx, ffi_idx, return_type) are
// set to -1 / NULL here.
AstNode* ast_func(Token name, AstNode* body) {
    AstNode* n = alloc_node(AST_FUNC, name.span);
    n->as.func.name             = name;
    n->as.func.type_param_count = 0;
    n->as.func.param_count      = 0;
    n->as.func.return_type_expr = NULL;
    n->as.func.return_type      = NULL;
    n->as.func.body             = body;
    n->as.func.func_idx         = -1;
    n->as.func.is_extern        = false;
    n->as.func.is_variadic      = false;
    n->as.func.lib_alias        = (Token){0};
    n->as.func.ffi_idx          = -1;
    n->as.func.method_owner     = (Token){0};
    return n;
}

AstNode* ast_const(Token name, AstNode* initializer) {
    AstNode* n = alloc_node(AST_CONST, name.span);
    n->as.const_decl.name        = name;
    n->as.const_decl.initializer = initializer;
    n->as.const_decl.type_expr   = NULL;
    return n;
}

// Empty block. `stmts` is initialized so the parser can push
// without checking. The span argument is the `{`..`}` range.
AstNode* ast_block(Span span) {
    AstNode* n = alloc_node(AST_BLOCK, span);
    ast_list_init(&n->as.block.stmts);
    return n;
}

// `let`. is_mut defaults to false (immutable). The parser flips
// it if `mut` was present. type_expr is NULL until the parser
// attaches a `: T` annotation; type is filled by sema.
AstNode* ast_let(Token name, AstNode* init) {
    AstNode* n = alloc_node(AST_LET, name.span);
    n->as.let.name      = name;
    n->as.let.type_expr = NULL;
    n->as.let.type      = NULL;
    n->as.let.init      = init;
    n->as.let.is_mut    = false;    // default immutability
    return n;
}

// `target = value`. Span is taken from the target (the LHS is
// the "anchor" of an assignment). If target is NULL — which
// should not happen from the parser, but the guard is cheap —
// the span is SPAN_NONE.
AstNode* ast_assign(AstNode* target, AstNode* value) {
    AstNode* n = alloc_node(AST_ASSIGN, target ? target->span : SPAN_NONE);
    n->as.assign.target = target;
    n->as.assign.value  = value;
    return n;
}

AstNode* ast_assign_op(BinOp op, AstNode* target, AstNode* value) {
    AstNode* n = alloc_node(AST_ASSIGN_OP,
                            target ? target->span : SPAN_NONE);
    n->as.assign_op.op        = op;
    n->as.assign_op.target    = target;
    n->as.assign_op.value     = value;
    n->as.assign_op.temp_slot = -1;
    return n;
}

// `if`. Span comes from the condition; else_ may be NULL.
AstNode* ast_if(AstNode* cond, AstNode* then_, AstNode* else_) {
    AstNode* n = alloc_node(AST_IF, cond ? cond->span : SPAN_NONE);
    n->as.if_.cond   = cond;
    n->as.if_.then_  = then_;
    n->as.if_.else_  = else_;
    return n;
}

// `while`. Span from the condition.
AstNode* ast_while(AstNode* cond, AstNode* body) {
    AstNode* n = alloc_node(AST_WHILE, cond ? cond->span : SPAN_NONE);
    n->as.while_.cond = cond;
    n->as.while_.body = body;
    return n;
}

// `for i in start..end { body }`. All sema-owned slots default
// to -1. Span is taken from the loop variable token (`i`),
// which is the natural place to point a diagnostic about the
// loop.
AstNode* ast_for_range(Token var, AstNode* start, AstNode* end,
                       AstNode* body) {
    AstNode* n = alloc_node(AST_FOR_RANGE, var.span);
    n->as.for_range.var      = var;
    n->as.for_range.start    = start;
    n->as.for_range.end      = end;
    n->as.for_range.body     = body;
    n->as.for_range.var_slot = -1;
    n->as.for_range.end_slot = -1;
    return n;
}

// `for x in iterable { body }`. Four sema-owned slots, all -1.
AstNode* ast_for_in(Token var, AstNode* iterable, AstNode* body) {
    AstNode* n = alloc_node(AST_FOR_IN, var.span);
    n->as.for_in.var      = var;
    n->as.for_in.iterable = iterable;
    n->as.for_in.body     = body;
    n->as.for_in.var_slot = -1;
    n->as.for_in.arr_slot = -1;
    n->as.for_in.len_slot = -1;
    n->as.for_in.idx_slot = -1;
    return n;
}

// `break` / `continue`. No children; span is passed in by the
// parser because there is no token to derive it from
// (ast_break is called after the keyword has been consumed).
AstNode* ast_break(Span span)    { return alloc_node(AST_BREAK, span);    }
AstNode* ast_continue(Span span) { return alloc_node(AST_CONTINUE, span); }

AstNode* ast_return(Span span, AstNode* value) {
    AstNode* n = alloc_node(AST_RETURN, span);
    n->as.ret.value = value;
    return n;
}

// Wrap an expression as a statement. Span comes from the
// expression. The expression's value is discarded at runtime
// unless sema reclassifies this as a tail expression (not done
// today).
AstNode* ast_expr_stmt(AstNode* expr) {
    AstNode* n = alloc_node(AST_EXPR_STMT, expr ? expr->span : SPAN_NONE);
    n->as.expr_stmt.expr = expr;
    return n;
}

// Literals. Each stores the value directly in the union; no
// heap. AST_STRING / AST_CSTRING store the token covering the
// text *between the quotes*, not including them.
AstNode* ast_int(Span span, U64 value) {
    AstNode* n = alloc_node(AST_INT, span);
    n->as.int_lit.value = value;
    return n;
}

AstNode* ast_float(Span span, double value) {
    AstNode* n = alloc_node(AST_FLOAT, span);
    n->as.float_lit.value = value;
    return n;
}

AstNode* ast_bool(Span span, bool value) {
    AstNode* n = alloc_node(AST_BOOL, span);
    n->as.bool_lit.value = value;
    return n;
}

AstNode* ast_string(Span span, Token content) {
    AstNode* n = alloc_node(AST_STRING, span);
    n->as.string_lit.content = content;
    return n;
}

AstNode* ast_cstring(Span span, Token content) {
    AstNode* n = alloc_node(AST_CSTRING, span);
    n->as.string_lit.content = content;   // reuses the string_lit slot
    return n;
}

// Bare identifier reference. Resolution fields (local_index,
// const_id) are set to -1 by alloc_node; sema sets one of them
// when it figures out what the name means.
AstNode* ast_var(Token name) {
    AstNode* n = alloc_node(AST_VAR, name.span);
    n->as.var.name = name;
    n->as.var.func_idx = -1;
    return n;
}

// Binary and unary expression constructors. Both take the span
// from the caller because the operator token's span is the
// natural location for a diagnostic about the operation, and
// the caller has that token in hand when it parses.
AstNode* ast_binary(BinOp op, AstNode* l, AstNode* r, Span span) {
    AstNode* n = alloc_node(AST_BINARY, span);
    n->as.binary.op    = op;
    n->as.binary.left  = l;
    n->as.binary.right = r;
    return n;
}

AstNode* ast_unary(UnOp op, AstNode* operand, Span span) {
    AstNode* n = alloc_node(AST_UNARY, span);
    n->as.unary.op      = op;
    n->as.unary.operand = operand;
    return n;
}

// Empty call node. The parser pushes arguments and, for a
// generic call, attaches an instantiation TypeExpr. The three
// resolution indices are set to -1: exactly one will be set by
// sema if the call resolves.
AstNode* ast_call(Token name) {
    AstNode* n = alloc_node(AST_CALL, name.span);
    n->as.call.name          = name;
    n->as.call.instantiation = NULL;
    n->as.call.builtin_id    = -1;
    n->as.call.func_idx      = -1;
    n->as.call.ffi_idx       = -1;
    ast_list_init(&n->as.call.args);
    return n;
}

AstNode* ast_call_indirect(AstNode* callee, Span span) {
    AstNode* n = alloc_node(AST_CALL_INDIRECT, span);
    n->as.call_indirect.callee = callee;
    ast_list_init(&n->as.call_indirect.args);
    return n;
}

// `*expr`. `offset` is 0 for a plain dereference; the field
// access through a pointer path (in the parser or sema) may
// set it later.
AstNode* ast_deref(AstNode* operand, Span span) {
    AstNode* n = alloc_node(AST_DEREF, span);
    n->as.deref.operand = operand;
    n->as.deref.offset  = 0;
    return n;
}

// `expr as Type`. target_type is left NULL for sema to fill.
AstNode* ast_cast(AstNode* operand, TypeExpr* target, Span span) {
    AstNode* n = alloc_node(AST_CAST, span);
    n->as.cast.operand          = operand;
    n->as.cast.target_type_expr = target;
    n->as.cast.target_type      = NULL;
    return n;
}

// `expr?`. No extra state beyond the operand.
AstNode* ast_try(AstNode* operand, Span span) {
    AstNode* n = alloc_node(AST_TRY, span);
    n->as.try_.operand = operand;
    return n;
}

// Empty array literal. The parser pushes element expressions.
// The element type is inferred by sema; the array type itself
// is stored on the node's `type` field, not in a payload.
AstNode* ast_array_lit(Span span) {
    AstNode* n = alloc_node(AST_ARRAY_LIT, span);
    ast_list_init(&n->as.array_lit.elements);
    return n;
}

// `target[index]`. Both children owned.
AstNode* ast_index(AstNode* target, AstNode* index, Span span) {
    AstNode* n = alloc_node(AST_INDEX, span);
    n->as.index.target = target;
    n->as.index.index  = index;
    return n;
}

// `arena expr` / `arena mut expr`. The is_mut flag is a
// language-level property of the resulting reference, not an
// allocator property.
AstNode* ast_alloc_arena(AstNode* value, Span span, bool is_mut) {
    AstNode* n = alloc_node(AST_ALLOC_ARENA, span);
    n->as.alloc_arena.value  = value;
    n->as.alloc_arena.is_mut = is_mut;
    return n;
}

// `rc expr`. No mutability flag; RC handles are always
// mutable through the handle.
AstNode* ast_alloc_rc(AstNode* value, Span span) {
    AstNode* n = alloc_node(AST_ALLOC_RC, span);
    n->as.alloc_rc.value = value;
    return n;
}

AstNode* ast_sizeof(Span span, TypeExpr* te) {
    AstNode* n = alloc_node(AST_SIZEOF, span);
    n->as.sizeof_expr.te       = te;
    n->as.sizeof_expr.resolved = NULL;
    n->as.sizeof_expr.size     = 0;
    return n;
}

// `expr.name`. unit_tag is set to -1 here to mean "field
// access"; if sema determines that the `.name` is actually a
// variant tag on the target's enum type, it overwrites
// unit_tag with the tag value (>= 0) and sets `offset` to the
// tag's byte offset.
AstNode* ast_field(AstNode* target, Token field) {
    AstNode* n = alloc_node(AST_FIELD, field.span);
    n->as.field.target   = target;
    n->as.field.field    = field;
    n->as.field.owner    = NULL;
    n->as.field.offset   = 0;
    n->as.field.unit_tag = -1;    // field access, not a variant
    n->as.field.declared_type = NULL;
    return n;
}

// Empty struct definition. The parser fills fields and
// type_params; sema sets sdef_index.
AstNode* ast_struct_def(Token name) {
    AstNode* n = alloc_node(AST_STRUCT_DEF, name.span);
    n->as.struct_def.name             = name;
    n->as.struct_def.type_param_count = 0;
    n->as.struct_def.sdef_index       = -1;
    n->as.struct_def.is_extern        = false;
    n->as.struct_def.is_opaque        = false;
    n->as.struct_def.is_union         = false;
    ast_list_init(&n->as.struct_def.fields);
    return n;
}

// One field in a struct definition. offset defaults to 0; sema
// overwrites it with the computed layout offset. Because 0 is
// also a valid resolved offset (the first field), do not use
// offset == 0 to mean "unresolved"; check the parent struct's
// sdef_index (which is -1 until sema) instead.
AstNode* ast_struct_field(Token name, TypeExpr* type_expr) {
    AstNode* n = alloc_node(AST_STRUCT_FIELD, name.span);
    n->as.struct_field.name      = name;
    n->as.struct_field.type_expr = type_expr;
    n->as.struct_field.type      = NULL;
    n->as.struct_field.offset    = 0;
    return n;
}

// Empty struct literal. sdef_index = -1 until sema. inits is
// initialized empty for the parser to push AST_STRUCT_INIT
// nodes into.
AstNode* ast_struct_lit(Token type_name) {
    AstNode* n = alloc_node(AST_STRUCT_LIT, type_name.span);
    n->as.struct_lit.type_name  = type_name;
    n->as.struct_lit.instantiation = NULL;
    n->as.struct_lit.sdef_index = -1;
    n->as.struct_lit.hidden_slot = -1;
    n->as.struct_lit.expected_type = NULL;
    ast_list_init(&n->as.struct_lit.inits);
    return n;
}

// One `field: value` inside a struct literal. offset = -1 until
// sema resolves the field.
AstNode* ast_struct_init(Token name, AstNode* value) {
    AstNode* n = alloc_node(AST_STRUCT_INIT, name.span);
    n->as.struct_init.name   = name;
    n->as.struct_init.value  = value;
    n->as.struct_init.offset = -1;
    n->as.struct_init.field_type = NULL;
    return n;
}

// Empty enum definition. variants is initialized empty; sema
// sets tdef_index.
AstNode* ast_enum_def(Token name) {
    AstNode* n = alloc_node(AST_ENUM_DEF, name.span);
    n->as.enum_def.name             = name;
    n->as.enum_def.type_param_count = 0;
    n->as.enum_def.tdef_index       = -1;
    ast_list_init(&n->as.enum_def.variants);
    return n;
}

// One enum variant. tag = -1 until sema assigns discriminants.
// payloads is a heap array filled by sema; it is NULL here and
// stays NULL until sema resolves the payload type expressions.
// payload_exprs is the syntactic form (list of AST_TYPE_EXPR_NODE).
AstNode* ast_enum_variant(Token name) {
    AstNode* n = alloc_node(AST_ENUM_VARIANT, name.span);
    n->as.enum_variant.name            = name;
    n->as.enum_variant.payloads        = NULL;
    n->as.enum_variant.payload_count   = 0;
    n->as.enum_variant.tag             = -1;
    ast_list_init(&n->as.enum_variant.payload_exprs);
    return n;
}

// Wrapper that lets a TypeExpr sit inside an AstList. The
// TypeExpr is owned by this node; ast_free frees it. Span is
// borrowed from the TypeExpr.
AstNode* ast_type_expr_node(TypeExpr* te) {
    AstNode* n = alloc_node(AST_TYPE_EXPR_NODE,
                            te ? te->span : SPAN_NONE);
    n->as.type_expr_node.te = te;
    return n;
}

// `Enum.Variant` / `Enum.Variant(args)` / `receiver.method(args)`.
// All three shapes share this node. The sema-set fields are all
// -1 here: tdef_index, tag, mod_func_idx, mod_ffi_idx. args is
// initialized empty; receiver_expr is NULL until the parser
// attaches one for a method call.
AstNode* ast_variant_ctor(Token type_name, Token variant_name) {
    AstNode* n = alloc_node(AST_VARIANT_CTOR, type_name.span);
    n->as.variant_ctor.type_name     = type_name;
    n->as.variant_ctor.instantiation = NULL;
    n->as.variant_ctor.variant_name  = variant_name;
    n->as.variant_ctor.tdef_index    = -1;
    n->as.variant_ctor.tag           = -1;
    n->as.variant_ctor.mod_func_idx  = -1;
    n->as.variant_ctor.mod_ffi_idx   = -1;
    n->as.variant_ctor.receiver_expr = NULL;
    ast_list_init(&n->as.variant_ctor.args);
    return n;
}

// `match scrutinee { arms }`. hidden_slot = -1 until sema
// assigns a local to cache the scrutinee.
AstNode* ast_match(AstNode* scrutinee, Span span) {
    AstNode* n = alloc_node(AST_MATCH, span);
    n->as.match.scrutinee   = scrutinee;
    n->as.match.hidden_slot = -1;
    ast_list_init(&n->as.match.arms);
    return n;
}

// One `pattern => body` arm. The parser constructs this before
// appending it to the enclosing match's arms list.
AstNode* ast_match_arm(AstNode* pattern, AstNode* body, Span span) {
    AstNode* n = alloc_node(AST_MATCH_ARM, span);
    n->as.match_arm.pattern = pattern;
    n->as.match_arm.body    = body;
    return n;
}

// Pattern constructors. Each mirrors the matching expression
// constructor (ast_int, ast_bool, ast_string) plus the wildcard
// and binding forms. All sema-owned slots are -1 by default
// (alloc_node sets local_index; pat_bind.slot and pat_variant
// fields are set explicitly).
AstNode* ast_pat_wildcard(Span span) {
    return alloc_node(AST_PAT_WILDCARD, span);
}

AstNode* ast_pat_bind(Token name) {
    AstNode* n = alloc_node(AST_PAT_BIND, name.span);
    n->as.pat_bind.name = name;
    n->as.pat_bind.slot = -1;
    return n;
}

AstNode* ast_pat_int(Span span, I64 value) {
    AstNode* n = alloc_node(AST_PAT_INT, span);
    n->as.pat_int.value = value;
    return n;
}

AstNode* ast_pat_bool(Span span, bool value) {
    AstNode* n = alloc_node(AST_PAT_BOOL, span);
    n->as.pat_bool.value = value;
    return n;
}

AstNode* ast_pat_string(Span span, Token content) {
    AstNode* n = alloc_node(AST_PAT_STRING, span);
    n->as.pat_string.content = content;
    return n;
}

// `Enum.Variant(p1, ...)` pattern. subpatterns is initialized
// empty; the parser pushes child patterns. tag is -1 until
// sema; sub_count is kept in sync by sema (or by the parser
// if it chooses to; today it is redundant with
// subpatterns.count and both must agree).
//
// tmp_slots is not zeroed by ast_pat_variant beyond what
// xcalloc already did. Sema is expected to fill only the first
// sub_count entries; codegen must not read past that.
AstNode* ast_pat_variant(Token type_name, Token variant_name, Span span) {
    AstNode* n = alloc_node(AST_PAT_VARIANT, span);
    n->as.pat_variant.type_name    = type_name;
    n->as.pat_variant.variant_name = variant_name;
    n->as.pat_variant.tag          = -1;
    n->as.pat_variant.sub_count    = 0;
    n->as.pat_variant.instantiation = NULL;
    ast_list_init(&n->as.pat_variant.subpatterns);
    return n;
}

// ------------------------------------------------------------
// using declarations
// ------------------------------------------------------------

// `using a.b.c` or `using a.b.c as X`. `path` is a caller-owned
// array of n Tokens, copied here; the caller's buffer can go
// out of scope immediately.
//
// resolved_path_count is initialized to `n`, not 0. That is
// the "no resolution has happened yet, so assume the whole
// path named the module" default. The loader overwrites it if
// it discovers that only a prefix of the path named a file
// (e.g. `using a.b.c` where `a.b` is the file and `c` is a
// declaration inside it).
AstNode* ast_using_module(Token* path, int n, Token alias, bool has_alias) {
    AstNode* node = alloc_node(AST_USING, path[0].span);
    AstUsing* u = &node->as.using_decl;
    u->kind = USING_MODULE;
    u->path = (Token*)xmalloc(sizeof(Token) * (size_t)n);
    memcpy(u->path, path, sizeof(Token) * (size_t)n);
    u->path_count          = n;
    u->module_alias        = alias;
    u->has_module_alias    = has_alias;
    u->resolved_file_id    = 0;
    u->resolved_path_count = n; 
    return node;
}

// `using a.b.{ x, y as z }`. Same path copy as the module
// form. resolved_path_count is left at 0 here, unlike the
// module form's `n`: the selective form's path must resolve
// wholly to a module (there is no "prefix + declaration"
// interpretation), so the loader will set both resolution
// fields. Leaving it 0 signals "loader has not run" for this
// kind.
AstNode* ast_using_selective(Token* path, int n) {
    AstNode* node = alloc_node(AST_USING, path[0].span);
    AstUsing* u = &node->as.using_decl;
    u->kind = USING_SELECTIVE;
    u->path = (Token*)xmalloc(sizeof(Token) * (size_t)n);
    memcpy(u->path, path, sizeof(Token) * (size_t)n);
    u->path_count          = n;
    u->resolved_file_id    = 0;
    u->resolved_path_count = 0;
    return node;
}

// Append one selective-import name. `sym` is the name as
// written in the target module; `alias` is the local name, or
// ignored if has_alias is false (in which case the local name
// is the same as the source name).
//
// Growth: start at 4, double when full — same pattern as
// every other dynamic array in the compiler.
void ast_using_selective_add(AstNode* n, Token sym, Token alias,
                             bool has_alias) {
    AstUsing* u = &n->as.using_decl;
    if (u->name_count >= u->name_capacity) {
        u->name_capacity = u->name_capacity ? u->name_capacity * 2 : 4;
        u->names = (UsingName*)xrealloc(u->names,
            sizeof(UsingName) * (size_t)u->name_capacity);
    }
    u->names[u->name_count].source = sym;
    u->names[u->name_count].local  = has_alias ? alias : sym;
    u->name_count++;
}

// `using Name = TypeExpr`. No path, no names; the payload has
// only alias_local and alias_target. ast_free frees
// alias_target for this kind only.
AstNode* ast_using_alias(Token name, TypeExpr* target) {
    AstNode* node = alloc_node(AST_USING, name.span);
    AstUsing* u = &node->as.using_decl;
    u->kind         = USING_ALIAS;
    u->alias_local  = name;
    u->alias_target = target;
    return node;
}

// `extern "libname" [as alias]`. Reuses the AstUsing payload
// to hold a one-element `path` and an optional module_alias.
//
// Note that `kind` is left at xcalloc's 0, which happens to
// equal USING_MODULE. Do not interpret the kind field on an
// AST_EXTERN_LIB node as a UsingKind; it is not meaningful
// here. This is a known wart of the payload-reuse scheme.
//
// has_module_alias is derived from the alias token's file_id
// being non-zero: a real alias token has a real span, and a
// zero-valued Token has file_id 0. That is the same convention
// the rest of the compiler uses for SPAN_NONE / absent tokens.
AstNode* ast_extern_lib(Token path, Token alias) {
    AstNode* n = alloc_node(AST_EXTERN_LIB, path.span);
    n->as.using_decl.path = (Token*)xmalloc(sizeof(Token));
    n->as.using_decl.path[0] = path;
    n->as.using_decl.path_count = 1;
    n->as.using_decl.module_alias = alias;
    n->as.using_decl.has_module_alias = alias.span.file_id != 0;
    return n;
}