// ============================================================
// sema.h — Semantic analysis entry point.
//
// A single exported function. Everything else in sema.c —
// FnCtx, FuncSig, Local, the resolution tables, the per-node
// handlers — is an implementation detail and is not visible
// outside this translation unit.
//
// The ConstTable is caller-owned. It is the only piece of state
// that survives between sema and codegen: sema registers
// constants into it, and codegen reads them back when a const
// name appears in an expression. Everything else sema computes
// (resolved types, local slots, escape information) lives on the
// AST nodes and the callers of sema_analyze do not need to
// know about it.
//
// Two kinds of state, and why they are split:
//
//   SemaCtx  — per-compilation. Function table, type table,
//              module table, constant table, monomorphizer
//              handle. Survives across all function bodies.
//              Exposed here because codegen needs to read the
//              same function table sema filled in (see the
//              `codegen_of_sema` mapping in codegen.c).
//
//   FnLocal  — per-function. Locals, scopes, current signature.
//              Created fresh inside analyze_func_body and
//              discarded when it returns. Not exposed because
//              nothing outside sema needs to see it. Splitting
//              it out is what makes recursive body analysis
//              (a template instantiated from inside another
//              body) safe without save/restore.
// ============================================================
#ifndef PRISM_SEMA_H
#define PRISM_SEMA_H

#include "ast.h"
#include "source_map.h"
#include "diagnostics.h"
#include "constant.h"
#include "prism_ffi.h"


// ---- Configuration limits ----
//
// All fixed caps. None of these are enforced at a lower layer;
// sema is the layer that turns "table is full" into a
// diagnostic. Exceeding any of them is a user-visible
// compilation error, not a silent truncation.
//
//   MAX_LOCALS     — locals per function (including parameters).
//                    Slots are indices into the VM's frame, so
//                    this must stay under whatever the VM's
//                    frame layout can address.
//   MAX_SCOPES     — nesting depth of { } blocks within one
//                    function. Each block pushes one scope.
//   MAX_FUNCS      — total functions across all files, plus
//                    every monomorphized function instantiation.
//                    Instantiated functions are added to the
//                    same table as user functions.
//   MAX_INST_CACHE — number of distinct (template, args)
//                    instantiations kept in the global cache.
//                    Cache is global (not per-SemaCtx) because
//                    there is one sema_analyze per compilation.
//   MAX_MONO       — cap on `mono_funcs`, which is currently
//                    unused; see the comment on SemaCtx.
#define MAX_LOCALS   256
#define MAX_SCOPES   256
#define MAX_FUNCS     64
#define MAX_INST_CACHE 256
// Enum variant counts are bounded by the fixed-size
// coverage array used by check_exhaustive (128 bits), and
// variant payload counts by the fixed-size tmp_slots array
// on AST_PAT_VARIANT (16 slots). Both bounds are enforced
// at registration time so that later passes can rely on
// them without re-checking.
#define MAX_ENUM_VARIANTS    128
#define MAX_VARIANT_PAYLOADS  16

// ============================================================
// State
// ============================================================

// One local binding in a function's frame.
//
// A Local is created by `declare` (from a `let`, a parameter,
// a for-loop variable, or a pattern binding) or by
// `declare_anon` (from a synthetic slot: a pattern temporary,
// a for-range end value, the cached scrutinee of a match).
// The distinction matters for diagnostics: anonymous slots
// have a zero-valued `name` token, and any lookup by name
// will miss them because smap_tokens_equal will never match
// against a zero span.
//
// Field notes:
//   name      — the binding's identifier, or a zero Token for
//               anonymous slots. Zero Token means span.file_id
//               == 0, which is what smap_tokens_equal uses to
//               reject a match.
//   type      — the resolved type of the binding. Always
//               non-NULL for a declared local; sema either
//               resolves the annotation or infers from the
//               initializer, and only creates the Local once
//               it has a type.
//   slot      — the index of this binding in the function's
//               frame. This is the same value that OP_LOAD_LOCAL
//               / OP_STORE_LOCAL carry. Slots are allocated in
//               declaration order; the parameter slots come
//               first because analyze_func_body declares them
//               before it walks the body.
//   holds_rc  — true iff this binding owns an RC reference
//               that must be dropped when the binding goes
//               out of scope. Set from the initializer's
//               ownership classification; see the AST_LET
//               case in analyze_stmt.
//   is_mut    — true iff the binding may be assigned. Set
//               from the AST for `let mut`; parameters get
//               it from the Param's own is_mut flag. Used
//               both for the "cannot assign to immutable
//               binding" check and by is_mutable_lvalue.
typedef struct {
    Token name;
    Type* type;
    int   slot;
    bool  holds_rc;
    bool  is_mut;
} Local;

// One function signature.
//
// All functions — user, extern, monomorphized, method — share
// this struct. The flags and optional fields distinguish the
// cases:
//
//   is_instantiated   — true for a monomorphized copy. Used
//                       by find_func to skip instantiated
//                       entries during name lookup (a user
//                       writing `foo` should not find
//                       `foo#i64`).
//   is_extern         — true for `extern fn` declarations.
//                       Externs have no body and no
//                       accessible local frame; call sites
//                       go through OP_FFI_CALL.
//   is_variadic       — true for an extern with a trailing
//                       `...`. Only externs can be variadic.
//   ffi_idx           — index into the FFITable's funcs
//                       array. Set only when is_extern; -1
//                       otherwise.
//   method_owner_tdef — for methods, the TypeDef of the
//                       owner type. NULL for free functions.
//                       Used by the method-call resolution
//                       path in analyze_expr's
//                       AST_VARIANT_CTOR case.
//   decl              — the AST_FUNC that produced this
//                       signature. Borrowed. For an
//                       instantiated function this points
//                       at the monomorphized copy, which
//                       is owned by the FuncSig (see the
//                       comment on sema_instantiate_function).
//   name_buf          — 128-byte copy of the function name,
//                       mangled for instantiations. Used by
//                       diagnostics that want the name
//                       without a SourceMap lookup.
//
// Parameter types and the return type are stored as arrays
// / pointers, not as Param structs, so the signature is
// decoupled from the AST once Pass A completes.
typedef struct {
    Token name;
    int   param_count;
    Type* param_types[8];
    Type* return_type;
    bool  return_annotated;
    uint32_t file_id;
    bool     is_pub;
    bool     is_instantiated;
    bool     is_extern;         // NEW
    bool     is_variadic;       // NEW
    int      ffi_idx;           // NEW: index into FFITable, or -1
    TypeDef* method_owner_tdef; // NULL unless a method
    AstNode* decl;
    char     name_buf[128];
} FuncSig;

// NOTE:
// Builtin function table
//
// Sema's builtin definitions are local to analyze_expr in
// sema.c (the `B[]` array inside the AST_CALL case). The
// return codes are integer tags (0..13) that codegen's
// AST_CALL dispatch also uses; the two must be updated
// together when a builtin is added or removed.
//
// There is deliberately no shared header-level table: the
// only consumer of the table is analyze_expr, and keeping
// it local avoids the drift that would arise from two
// separately-maintained copies.



// Per-compilation state.
//
// Fields:
//   funcs / func_count — the function table. Populated in
//                        Pass A (and by instantiation on
//                        demand). find_func walks this.
//   smap               — borrowed; the same SourceMap passed
//                        to sema_analyze.
//   consts             — borrowed; caller-owned. Sema appends,
//                        codegen reads.
//   structs            — borrowed; caller-owned. Holds the
//                        TypeDefTable plus a `generics` handle
//                        that sema initializes (see below).
//   modtable           — owned by sema. Filled during
//                        sema_analyze by build_module_table and
//                        consumed by the resolver in types.c.
//                        Freed at the end of sema_analyze.
//
// Note: `structs->generics` and `structs->instantiate_generic`
// are set by sema_analyze before any type resolution runs, so
// that type_expr_resolve (in types.c) can call back into sema
// when it hits a TEX_GENERIC. That is the mechanism by which
// the monomorphizer is threaded through the otherwise
// generic-agnostic types.c.
typedef struct {
    // Per-compilation. Stable across all function bodies.
    FuncSig          funcs[MAX_FUNCS];
    int              func_count;
    const SourceMap* smap;
    ConstTable*      consts;
    TypeDefTable*    structs;
    ModuleTable      modtable;

    // The file_id of the compiler prelude, or 0 if no prelude
    // is loaded. The prelude is the implicit module every file
    // imports; the resolver uses this to make its declarations
    // (Option, Result) visible in every scope without a
    // `using` clause.
    uint32_t         prelude_file_id;
    
} SemaCtx;

// Per-function state.
//
// Created on the stack inside analyze_func_body and discarded
// when that function returns. The reason this is separate
// from SemaCtx is reentrancy: an instantiation triggered
// inside a body walk calls analyze_func_body again, which
// creates its own FnLocal. Nothing needs to be saved across
// the nested call.
//
// Fields:
//   locals            — the frame. Slots are indices into
//                       this array, in declaration order.
//                       Parameters occupy slots 0..n-1, then
//                       locals are appended. Note that slot
//                       numbers are NOT reused after a scope
//                       exits; exit_scope restores local_count
//                       so subsequent declarations overwrite
//                       the same slot numbers, but the
//                       AST nodes that referenced the
//                       previous binding keep their old
//                       local_index values. That is fine
//                       because those nodes are out of
//                       scope by then.
//   scope_first[]     — for each active scope, the value of
//                       local_count at the moment the scope
//                       was entered. exit_scope restores
//                       local_count from this.
//   scope_depth       — number of active scopes; index of
//                       the next free slot in scope_first[].
//   current_sig       — the FuncSig being analyzed. Used by
//                       AST_RETURN and AST_TRY to check the
//                       enclosing function's return type.
//   current_file      — the file_id of the function's
//                       declaration. Used by the fallback
//                       variant resolution in
//                       AST_VARIANT_CTOR (when the type name
//                       is not findable, the function's own
//                       return type is consulted).
//   loop_depth        — number of enclosing loops. AST_BREAK
//                       and AST_CONTINUE check this is > 0.
typedef struct {
    // Per-function. Scoped to one analyze_func_body call.
    Local    locals[MAX_LOCALS];
    int      local_count;
    int      scope_first[MAX_SCOPES];
    int      scope_depth;
    FuncSig* current_sig;
    uint32_t current_file;
    int      loop_depth;
} FnLocal;

// ============================================================
// sema_analyze
//
// Runs over a fully parsed program. Semantics:
//
//   Pass A — Signatures and constants.
//     Every function gets a FuncSig: parameter types resolved
//     from their annotations (unannotated → i64), and a return
//     type resolved from its annotation or left NULL for
//     inference during body checking.
//
//     Every `const` is registered in `consts` after converting
//     its literal initializer into a Constant. Duplicate names
//     are rejected.
//
//   Pass B — Function bodies.
//     Every function is checked independently. During the body
//     walk, local variables are assigned slots, every expression
//     is annotated with its type, and the reference model's
//     rules are enforced:
//       - No nested references: a ref<T> cannot contain another
//         reference, nor appear inside an rc<> or *<>.
//       - No reference-typed returns: a function cannot return
//         ref<T>. rc<T> is the escape mechanism.
//       - Type mismatches in let, assign, call, return, and
//         binary/unary operands.
//
// The function is idempotent with respect to `consts`: callers
// pass an initialized (possibly empty) table, and sema appends.
//
// Returns true if no errors were reported. Warnings do not
// affect the return value; callers may inspect the bag.
//
// Additional passes beyond A and B, run in this order:
//   Pass 0   — register nominal type names (structs / enums).
//              Generic declarations go to the monomorphizer
//              instead of the concrete table.
//   Pass 0b  — resolve struct fields and enum variant
//              payloads against the now-populated type table.
//   (between passes A and B) validate selective imports
//              against the populated function table.
//
// `ffi` is filled in during Pass A for extern declarations.
// `sc` must be caller-allocated and is zeroed at the start of
// sema_analyze.
//
// Ownership: `program`, `smap`, `consts`, `structs`, `bag`,
// and `ffi` are all borrowed. Sema does not free any of them.
// The internal `sc->modtable` is owned by sema and freed at
// the end of the call.
bool sema_analyze(AstNode* program, const SourceMap* smap,
                  ConstTable* consts, TypeDefTable* structs,
                  DiagnosticBag* bag, FFITable* ffi,
                  SemaCtx* sc);
#endif // PRISM_SEMA_H