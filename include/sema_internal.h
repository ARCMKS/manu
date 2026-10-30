// ============================================================
// sema_internal.h — internal interface for the sema_*.c family.
//
// After Phase 3, sema analysis is split into several .c files:
//
//   sema.c          — bodies, expressions, statements, the
//                     orchestrator (sema_analyze).
//   sema_modules.c  — module table construction and validation.
//   sema_ffi.c      — FFI library and function table construction.
//   sema_types.c    — Pass 0 and 0b: nominal type registration
//                     and C-ABI layout.
//   sema_mono.c     — generic instantiation and its cache.
//
// Each file's externally-visible entry points are declared
// here. Nothing outside the sema stage includes this header;
// the public interface is sema.h.
//
// The header is intentionally small. Helpers that are used
// only within one .c file stay static there and do not appear
// here. Only cross-file entry points are declared.
// ============================================================
#ifndef PRISM_SEMA_INTERNAL_H
#define PRISM_SEMA_INTERNAL_H

#include "sema.h"

// --- sema_modules.c ---

// Walk a program's top-level declarations and populate the
// ModuleTable with the imports and selective imports declared
// by each file's `using` statements.
//
// The ModuleTable is caller-owned; the function appends to it.
// Called once from sema_analyze, before Pass A.
void build_module_table(ModuleTable* mt, AstNode* program,
                        DiagnosticBag* bag);

// Verify each selective import against the target module.
// Called from sema_analyze after Pass A, when the function
// table is populated. Uses the resolver to look up the
// source name in the module's file.
void validate_selective_imports(AstNode* program, SemaCtx* sc,
                                DiagnosticBag* bag);


// --- sema_ffi.c ---
//
// FFI construction is a two-part concern:
//
//   1. The library table. Build once from the program's
//      `extern lib` declarations, then consult by alias.
//      The table lives on the caller's stack as a
//      LibraryTable struct, and this file owns its layout.
//
//   2. Type translation. Given a Prism type, decide whether
//      it is FFI-compatible and if so, produce the matching
//      FFIType. Used by Pass A when registering extern
//      functions and by body analysis when checking that a
//      variadic argument is permitted.
//
// Two special cases:
//
//   is_option_ptr_type — recognizes Option<*T>, which crosses
//                        the boundary as a nullable C pointer.
//
//   is_ffi_variadic_ok — the set of types allowed in a
//                        variadic position (integers and
//                        pointers; not floats).

// A library declared with `extern lib "path" [as alias]`.
// The alias is what `from` clauses name; the path is passed
// to the OS loader. Entries are filled by build_library_table;
// the fields are read by library_table_find and, later, by
// the FFITable initialization in sema_analyze.
typedef struct {
    char alias[64];
    char path[256];
} LibEntry;

// The set of libraries declared by one compilation. Sized
// inline (16 entries) matching FFITable.libs[16]; overflow is
// a diagnostic, not silent truncation.
typedef struct {
    LibEntry entries[16];
    int      count;
} LibraryTable;

// Populate `lt` from the program's `extern lib` declarations.
// Resets `lt->count` to 0 on entry. Truncation of alias or
// path (longer than the inline buffers) is silent.
void build_library_table(LibraryTable* lt, AstNode* program,
                         const SourceMap* smap, DiagnosticBag* bag);

// Find a library by alias. Returns an index into `lt->entries`
// or -1 if the alias is unknown. The alias Token's text is
// extracted via `smap`.
int library_table_find(const LibraryTable* lt,
                       const SourceMap* smap, Token alias);

// Translate a Prism type to an FFIType. Returns true on
// success (writing to *out) and false if the type is not
// FFI-compatible.
bool ffi_kind_from_type(const Type* t, FFIType* out);

// True iff `t` is Option<*T>: an RC-managed enum whose
// mangled name begins with "Option#" and whose first
// variant's single payload is a raw pointer. This is how
// nullable C pointers are modeled across the boundary.
bool is_option_ptr_type(const Type* t);

// True iff `t` is a legal variadic argument. Integers, bool,
// and raw pointers qualify; floats and compound types do not.
bool is_ffi_variadic_ok(const Type* t);




// --- sema_types.c ---
//
// Pass 0 and Pass 0b of the sema pipeline. Pass 0 registers
// every nominal type name into the TypeDefTable; Pass 0b
// resolves field types, variant payloads, and C-ABI layout.
//
// All four functions are called from sema_analyze's Pass 0 /
// Pass 0b loops. Nothing after Pass 0b calls them, so no body
// analysis depends on this file.
//
// The C-ABI helpers (c_size_of, c_align_of) are private to
// this file; only resolve_extern_structs uses them.

// Register one nominal type: a struct or an enum. If the
// declaration has type parameters, it goes to the
// monomorphizer as a template; otherwise it gets a TypeDef.
// `sdef_index` (structs) or `tdef_index` (enums) is set on
// the AST node to point at the new entry.
//
// Extern types (with `is_extern` set) register with kind
// TDEF_EXTERN_STRUCT; opaque externs register with an empty
// layout and are never laid out.
void register_struct_or_enum(AstNode* d, SemaCtx* sc, DiagnosticBag* bag);

// Pass 0b: resolve a regular struct's field types and compute
// their Prism layout (8 bytes per field, starting at offset 8).
// Called for each non-extern struct whose sdef_index >= 0.
void resolve_struct_fields(TypeDef* td, AstNode* d, SemaCtx* sc,
                           DiagnosticBag* bag);

// Pass 0b: resolve an enum's variants' names, tags, and
// payload types. Called for each enum whose tdef_index >= 0.
void resolve_enum_variants(TypeDef* td, AstNode* d, SemaCtx* sc,
                           DiagnosticBag* bag);

// Pass 0b: resolve every extern struct's fields and compute
// C-ABI layout. Walks all extern structs in the program and
// iterates the layout to a fixpoint, so forward references
// between extern structs work. Called once, before Pass 0b's
// per-declaration loop.
void resolve_extern_structs(AstNode* program, SemaCtx* sc,
                            DiagnosticBag* bag);

// --- shared analysis entry point ---
//
// analyze_func_body is defined in sema.c and called from
// sema_mono.c during instantiation: an instantiated function's
// body is analyzed exactly like a user-written one, using the
// new FuncSig. Its definition stays in sema.c.
void analyze_func_body(SemaCtx* sc, FuncSig* sig, AstNode* fn,
                       DiagnosticBag* bag);


// --- sema_mono.c ---
//
// Generic instantiation. Two entry points are reachable from
// sema.c and types.c:
//
//   sema_instantiate_generic    Installed as a callback on
//                               TypeDefTable, called from
//                               types.c whenever a TEX_GENERIC
//                               is resolved.
//
//   sema_instantiate_function   Called from body analysis
//                               when an explicit `<...>` is
//                               seen on a call.
//
// Both consult a global instantiation cache keyed on
// (template, argument types). The cache is file-static and
// reset at the start of each sema_analyze.
//
// The two functions share a private helper,
// sema_instantiate_function_by_template, that takes a
// template index directly. It is exposed here so that
// sema.c's module-call path (which has already resolved
// the template via the resolver) can call it without a
// second name lookup.

// Reset the instantiation cache. Called once at the start of
// each sema_analyze so consecutive compilations do not see
// each other's instantiations. The cache is file-static in
// sema_mono.c; this entry point is the only way to clear it.
void cache_reset(void);

// Instantiate a generic type from a TEX_GENERIC. Installed
// as TypeDefTable.instantiate_generic; not called directly.
Type* sema_instantiate_generic(void* generics, TypeExpr* te,
                               const SourceMap* smap, void* ctx,
                               uint32_t file_id, DiagnosticBag* bag);

// Instantiate a generic function by name, scoped to the use
// site's file. Used by body analysis for `f<T>(...)`.
int sema_instantiate_function(SemaCtx* sc, Token base,
                              TypeExpr* te, DiagnosticBag* bag);

// Instantiate a generic function by template index. Used by
// sema.c's module-call path once the resolver has identified
// the template.
int sema_instantiate_function_by_template(SemaCtx* sc,
                                          int tmpl_idx,
                                          Token base,
                                          TypeExpr* te,
                                          DiagnosticBag* bag);

Type* instantiate_option_of(SemaCtx* sc, Type* payload,
                                   Span span, DiagnosticBag* bag);




// True iff the TypeDef's direct fields (for a struct) or
// variant payloads (for an enum) transitively contain RC
// content. Extern structs and builtin types always return
// false; their RC status is handled separately.
bool sema_tdef_has_rc_content(const TypeDef* td);

// Fixpoint: for every non-instantiated TypeDef in `structs`,
// compute has_rc_content and set `is_rc_managed` if the
// computation succeeds and `is_builtin` did not already set
// it. Called once after Pass 0b (fields and variants
// resolved).
void sema_resolve_rc_managing(TypeDefTable* structs);

#endif // PRISM_SEMA_INTERNAL_H
