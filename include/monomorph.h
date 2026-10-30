// monomorph_h
// ============================================================
// monomorph.h — Generic template registry and AST deep-copy
// with type substitution.
//
// What this file does, at a high level:
//
//   Prism handles generics by *monomorphization*: rather than
//   compiling a generic function or type once and dispatching
//   on type arguments at runtime, each distinct instantiation
//   gets its own copy of the AST with the type parameters
//   substituted by concrete types. The copy is then compiled
//   like any ordinary, non-generic declaration.
//
//   The two halves of that process are:
//     1. Registry: templates discovered during sema are
//        recorded in a TemplateList (see monomorph.c). A
//        template is just the original AST node plus the
//        list of its type-parameter names.
//     2. Substitution: given a template and a concrete
//        argument list, produce a fresh AST node with every
//        reference to a parameter name replaced by the
//        corresponding Type*. That is mono_copy_node and
//        mono_copy_type_expr.
//
// Nothing here resolves, checks, or evaluates types. The
// caller (sema) is expected to have already resolved the
// concrete Type* arguments (via type_expr_resolve) and to be
// holding valid Types that live for the process lifetime.
//
// Ownership and lifetime:
//   - The TemplateList is owned by the caller that created it
//     via mono_init; freed by mono_free. The list stores
//     *borrowed* AstNode* pointers: the templates' ASTs live
//     in the program's main AST tree and are owned by it.
//     mono_free must not (and does not) free them.
//   - A monomorphized copy returned by mono_copy_node is a
//     fresh, self-owned AST subtree. The caller is responsible
//     for eventually freeing it (via ast_free), either directly
//     or by attaching it to a parent that will be freed.
//   - Subst is a borrowed view: the caller owns the params and
//     args arrays for the duration of the copy call. No copy
//     of Subst is made and no ownership is transferred.
// ============================================================
#ifndef PRISM_MONOMORPH_H
#define PRISM_MONOMORPH_H

#include "ast.h"
#include "types.h"
#include "source_map.h"
#include "diagnostics.h"

// One generic template: the AST of the declaration, the names of
// its type parameters, and its defining file.
//
// A GenericTemplate is what mono_register_template extracts
// from a declaration that had `type_param_count > 0`. The
// `decl` pointer is borrowed from the program AST; the
// params[] array is a flat copy of the declaration's own
// type_params[] tokens (see the extractor in monomorph.c).
//
// Field-by-field:
//   decl        — AST_FUNC, AST_STRUCT_DEF, or AST_ENUM_DEF.
//                 Never NULL for a registered template; the
//                 registration switch only records those
//                 three kinds.
//   name        — the declaration's name token, used for
//                 name-based lookup in mono_find_template.
//   params[]    — up to 8 type-parameter name Tokens, in
//                 declaration order. `param_count` is how many
//                 are valid. The cap of 8 matches the fixed
//                 arrays in AST_FUNC / AST_STRUCT_DEF /
//                 AST_ENUM_DEF; the parser enforces it.
//   file_id     — the file the declaration lives in. Set from
//                 the declaration's span; used for diagnostics
//                 that want to point back at the original
//                 template.
//   is_builtin  — copied from the declaration's `is_builtin`
//                 flag. Prism has no builtin generic templates
//                 today, so this is always false; the field
//                 exists so that if a future prelude registers
//                 a generic like `Array<T>` it can be marked.
typedef struct {
    AstNode* decl;              // AST_FUNC, AST_STRUCT_DEF, or AST_ENUM_DEF
    Token    name;              // the declaration's name
    Token    params[8];
    int      param_count;
    uint32_t file_id;
    bool     is_builtin;
} GenericTemplate;

// Substitution map: for each parameter, the concrete Type*.
// Used while deep-copying a template's AST.
//
// Invariant: params[i] and args[i] are matched pairs; the
// copy functions look for a type expression whose name equals
// params[i] and replace it with a resolved TypeExpr wrapping
// args[i]. `count` is the number of valid entries. The two
// arrays are borrowed; the caller must keep them alive for the
// duration of any copy call.
//
// The map is a flat list, not a hash: with at most 8 entries
// per template, a linear scan is faster than any hashing
// scheme and avoids a second allocation.
typedef struct {
    Token* params;
    Type** args;
    int    count;
} Subst;

// Allocate an empty template list and store it through
// `*generics_out`. The output parameter is `void**` because
// the concrete TemplateList type is private to monomorph.c;
// callers (sema) treat the pointer as an opaque handle.
void mono_init(void** generics_out);

// Free the template list. The AstNodes inside are NOT freed:
// they are borrowed from the program AST (see the file header).
// Passing NULL is a no-op.
void mono_free(void* generics);

// Register a template from a declaration that has type parameters.
//
// Contract: `decl->kind` must be AST_FUNC, AST_STRUCT_DEF, or
// AST_ENUM_DEF, and its `type_param_count` must be > 0. Other
// kinds are silently ignored (the switch in the implementation
// has a `default: break;`), which means a caller that
// mistakenly registers a non-generic declaration gets no
// template and no diagnostic. The current call site in sema
// only registers declarations whose `type_param_count > 0`, so
// the contract is honored in practice.
//
// The `smap` parameter is currently unused (the implementation
// casts it to void). It is part of the signature for symmetry
// with the other token-comparing functions in this header,
// and so that if registration ever needs to compare or copy
// identifier text it can do so without a signature change.
void mono_register_template(void* generics, AstNode* decl,
                            const SourceMap* smap);

// Look up a template by base name. Returns NULL if no
// registered template's name matches `base`.
//
// Matching is by byte-equality of the token text via
// smap_tokens_equal. Two templates with the same name in
// different files would both match; the function returns the
// first one found. Today the language does not permit
// duplicate template names across files, so this is not a
// practical ambiguity, but there is no per-file filter here
// (unlike typedef_table_find_in_file). If that ever matters,
// a file-id-restricted variant is the obvious addition.
GenericTemplate* mono_find_template(void* generics, const SourceMap* smap,
                                    Token base);

GenericTemplate* mono_find_template_in_file(void* generics,
                                            const SourceMap* smap,
                                            Token base,
                                            uint32_t file_id);

// Deep copy with substitution. The substitution values are the
// concrete Types resolved from `type_args`. Returns a fresh AST
// node (or TypeExpr) that has no remaining references to the
// template's parameters.
//
// Both functions are recursive and return NULL for a NULL
// input. The returned tree is fully owned by the caller, with
// the same ownership rules as if the parser had built it: an
// AstNode owns its children, an AstList owns its elements,
// and so on. Free with ast_free / type_expr_free as
// appropriate.
//
// Fields marked "sema-owned" on the original (resolved Type*
// pointers, table indices, slot numbers, offsets) are reset to
// their unset sentinels in the copy. Sema is expected to run
// again on the copy, exactly as if it were a freshly parsed
// declaration. That is the whole point: an instantiation is a
// new concrete declaration, not a partially-analyzed one.
//
// TypeExpr substitution:
//   - A TEX_NAME whose token matches one of sub->params is
//     replaced by a TEX_RESOLVED wrapping the corresponding
//     sub->args entry. That is the substitution.
//   - Any other TEX_NAME is copied as-is.
//   - Compound TypeExprs (ref, rc, rawptr, array, generic,
//     qualified, resolved) are deep-copied recursively.
AstNode*  mono_copy_node(AstNode* n, const Subst* sub, const SourceMap* smap);
TypeExpr* mono_copy_type_expr(TypeExpr* te, const Subst* sub,
                              const SourceMap* smap);

// Build a mangled name like "Option#i64" into `out`. Truncates
// safely.
//
// Format: base name, then for each argument `#` followed by the
// argument's `type_name` rendering. Example:
//     mono_mangle(buf, N, "Result", {i64, string}, 2)
//     -> "Result#i64#string"
//
// Truncation: if the formatted text would exceed `cap`, it is
// cut off at the last byte that fits. There is no error return
// and no way for the caller to know truncation happened; the
// output is always NUL-terminated (unless cap == 0, in which
// case out is left untouched). Callers that need to detect
// truncation should pre-size the buffer generously and compare
// against the base + argument count, or (better) call with a
// large stack buffer.
//
// Two arguments with the same mangled rendering collide. For
// example, `Option<ref<i64>>` and `Option<ref<i64>>` obviously
// collide (same types), but so would any pair whose
// type_name() output is identical. Since type_name uses a ring
// buffer (see types.c), two *different* deeply nested types
// could in principle render the same string if the ring wraps
// mid-render. This is a real but unlikely hazard; see the
// Follow-ups in monomorph.c.
void mono_mangle(char* out, size_t cap, const char* base,
                 Type** args, int arg_count);

// Iterate registered templates. Returned pointers are
// borrowed from the internal list; they are invalidated by
// any subsequent registration. Used by sema to find a
// prelude template (Option) by name.
int              mono_template_count(void* generics);
GenericTemplate* mono_template_at(void* generics, int i);

#endif