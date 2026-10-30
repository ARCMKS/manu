// ============================================================
// types.h — Compile-time types and runtime descriptors.
//
// Two representations:
//
//   Type     The compiler's view. Recursive, carries sizes and
//            a pointer to the nominal TypeDef that names it.
//
//   TypeDesc The runtime's flat view. A kind tag, a nested
//            descriptor index, a size, and — for compound
//            kinds — a count. Codegen interns Types into
//            TypeDescs; the index travels in bytecode operands.
//
// Generics are handled above this layer. A generic declaration
// is stored as a template in monomorph.c; when instantiated,
// the template's AST is deep-copied with type substitution and
// the copy becomes an ordinary concrete TypeDef. Nothing in
// this file knows about type variables.
//
// Why two representations:
//   - The compiler needs recursion. `rc<Option<i64>>` is a
//     tree of Type nodes and it is convenient to walk it.
//   - The VM needs flat, indexable, comparable descriptors.
//     A bytecode operand is a 32-bit index into a TypeDesc
//     array; equality between two runtime types is integer
//     equality between their interned indices.
// `typetable_intern` (below) is the bridge: it walks a Type
// tree, emits any missing TypeDescs, and returns the index.
//
// Allocation / lifetime:
//   - Primitive Types (i8, i64, bool, string, void) are static
//     singletons in types.c and are never freed.
//   - Compound Types (ref, rc, rawptr, array, struct, enum)
//     are heap-allocated once and live for the life of the
//     process. They are effectively interned by construction
//     (see ty_struct/ty_enum below); there is no type_free.
//   - TypeDefs live in a TypeDefTable owned by the compiler
//     front end and are freed at shutdown.
//   - TypeDescs live in a TypeTable owned by codegen and are
//     freed at shutdown.
// ============================================================
#ifndef PRISM_TYPES_H
#define PRISM_TYPES_H

#include "common.h"
#include "lexer.h"
#include "diagnostics.h"

// ============================================================
// Compile-time types
// ============================================================

// Kind tag for Type. Grouped:
//   - VOID: the unit type. Size 0.
//   - signed ints i8..i64: sizes 1, 2, 4, 8.
//   - unsigned ints u8..u64: sizes 1, 2, 4, 8.
//   - floats f32, f64: sizes 4, 8.
//   - BOOL: size 8 (the VM has no sub-word bool; see the
//     Type static initializers in types.c).
//   - STRING: size 8 (a pointer to the RC-managed string
//     header / payload).
//   - REF, RC, RAWPTR, ARRAY: size 8 (pointer-shaped).
//     These are the only kinds that use `inner`.
//   - STRUCT, ENUM: sizes come from the TypeDef; they use
//     `tdef` and never `inner`.
//
// `is_mut` is only meaningful on TY_REF. Every other kind
// leaves it false.
typedef enum {
    TY_VOID,
    TY_I8, TY_I16, TY_I32, TY_I64,
    TY_U8, TY_U16, TY_U32, TY_U64,
    TY_F32, TY_F64,
    TY_BOOL,
    TY_STRING,
    TY_REF,
    TY_RC,
    TY_RAWPTR,
    TY_ARRAY,
    TY_STRUCT,
    TY_EXTERN_STRUCT,
    TY_FN,
    TY_ENUM,
} TypeKind;

// What kind of TypeDef this is. Structs and enums share one
// TypeDef struct with a `kind` discriminant rather than
// separate structs; the two have very different payloads
// (fields vs variants) but are always registered and looked
// up together, so a single table and a single struct is the
// simpler shape.
typedef enum { 
    TDEF_STRUCT, 
    TDEF_EXTERN_STRUCT,
    TDEF_ENUM,
    TDEF_BUILTIN,   // Primitive with a nominal identity
} TypeDefKind;

typedef struct Type Type;
typedef struct TypeDef TypeDef;

// One field of a struct definition.
//   name   — the identifier the user wrote. Kept as a Token
//            (not a string) so diagnostics can point at it.
//   type   — resolved by sema. NULL before then.
//   offset — byte offset within the struct's runtime layout,
//            computed by sema. Valid only after sema has run
//            for the enclosing struct_def.
//
// Layout rule (as of today): fields are laid out in
// declaration order, each at the next offset that satisfies
// its alignment. The exact algorithm lives in sema; types.c
// only stores the result.
typedef struct {
    Token name;
    Type* type;
    int   offset;
} StructField;

// One variant of an enum definition.
//   name, name_buf — redundant spellings. `name` is the Token
//                    (for diagnostics); `name_buf` is the
//                    copied text, capped at 64 bytes, used by
//                    the runtime TypeDesc interner and by
//                    rendering.
//   payloads       — array of resolved Type* for the variant's
//                    payload fields, in order. Owned by this
//                    struct (freed in typedef_table_free).
//                    NULL for a variant with no payload.
//   payload_count  — number of valid entries in `payloads`.
//   payload_capacity — allocation size of `payloads`. Not
//                    otherwise used; kept for symmetry with
//                    the other growable arrays.
typedef struct {
    Token name;
    char  name_buf[64];
    Type** payloads;
    int    payload_count;
    int    payload_capacity;
} EnumVariantDef;

// A named type (struct or enum). Instances live in a
// TypeDefTable and are looked up by name.
//
// Field notes:
//   kind            — TDEF_STRUCT or TDEF_ENUM.
//   name_buf        — 128-byte copy of `name`'s text. Both
//                     exist so the resolver can compare names
//                     without touching the SourceMap and so
//                     diagnostics can print the name without
//                     re-deriving it. Truncation at 127 bytes
//                     is silent.
//   name            — original Token, with span.
//   is_pub          — `pub` was written on this type. Only
//                     checked when the type is referenced
//                     from a different file (see
//                     resolve_inner's TEX_QUALIFIED case).
//   is_instantiated — true iff this TypeDef is a monomorphized
//                     copy of a generic. Such entries are
//                     skipped by name lookup
//                     (typedef_table_find / _in_file) so a
//                     user-written `Option` never accidentally
//                     resolves to `Option#i64`.
//
// Struct-only fields (fields/field_count/field_capacity/
// struct_size): valid iff kind == TDEF_STRUCT. `struct_size`
// is the total byte size, including padding for alignment.
//
// Enum-only fields (variants/variant_count/variant_capacity/
// is_rc_managed): valid iff kind == TDEF_ENUM. `is_rc_managed`
// marks enums whose payload contains RC-managed data, so
// codegen knows to emit refcount traffic on drops. RC-managed
// enums are the reason a `TY_ENUM` is sometimes wrapped in a
// `TY_RC` — see the note on `unwrap one level` in sema.
struct TypeDef {
    TypeDefKind kind;
    char        name_buf[128];
    Token       name;
    bool        is_pub;

    // A type that came from a generic template. It is registered
    // in the same table as user-written concrete types, but
    // name lookups skip it, so `Option` written by the user
    // never accidentally finds `Option#i64`.
    bool        is_instantiated;
    bool        is_opaque;
    bool        is_union;

    // Struct-only
    StructField* fields;
    int          field_count;
    int          field_capacity;
    int          struct_size;
    int          struct_align;  //C-ABI alignment

    // Enum-only
    EnumVariantDef* variants;
    int             variant_count;
    int             variant_capacity;
    bool            is_rc_managed;
};

// A resolved type.
//
// Kind determines which fields are meaningful:
//   - TY_VOID, TY_I*, TY_U*, TY_F*, TY_BOOL, TY_STRING: all
//     fields other than kind and size are unused.
//   - TY_REF: inner + is_mut.
//   - TY_RC, TY_RAWPTR, TY_ARRAY: inner.
//   - TY_STRUCT, TY_ENUM: tdef. `size` is copied from the
//     TypeDef at construction (see ty_struct/ty_enum).
//
// There is no owned heap in a Type that this file frees; Types
// are allocated once and live for the process lifetime. The
// `inner` pointer is borrowed: it points to another Type that
// also lives for the process lifetime.
struct Type {
    TypeKind kind;
    int      size;
    Type*    inner;
    bool     is_mut;
    TypeDef* tdef;

    // TY_FN only — the signature. `fn_params` is a heap array
    // owned by this Type; it is never freed (Types are
    // process-lifetime).
    Type**   fn_params;
    int      fn_param_count;
    Type*    fn_ret;
    
};

// --- Primitive type accessors ---
// Each returns a pointer to a shared static singleton. Two
// calls to ty_i64() return the same pointer, so pointer
// equality (a == b) is a valid fast path for type_eq on
// primitives.
Type* ty_void(void);
Type* ty_i8(void);
Type* ty_i16(void);
Type* ty_i32(void);
Type* ty_i64(void);
Type* ty_u8(void);
Type* ty_u16(void);
Type* ty_u32(void);
Type* ty_u64(void);
Type* ty_f32(void);
Type* ty_f64(void);
Type* ty_bool(void);
Type* ty_string(void);

// --- Compound type constructors ---
// Each allocates a new Type. They are NOT interned: two
// separate calls with the same arguments produce two distinct
// Type pointers. That is fine for `type_eq` (which compares
// structurally) but means pointer equality is only a fast
// path for the primitives. If you need canonical types for
// compound kinds, do it at a higher layer (or add an interning
// cache here); the current design relies on structural
// comparison.
Type* ty_ref(Type* inner, bool is_mut);
Type* ty_rc(Type* inner);
Type* ty_rawptr(Type* inner);
Type* ty_array(Type* inner);
// Unlike the four above, ty_struct/ty_enum take a TypeDef and
// stamp its size onto the resulting Type. All Types for the
// same TypeDef therefore have equal size, but again they are
// distinct pointers; type_eq on two structs compares their
// tdef pointers, not the Type pointers.
Type* ty_struct(TypeDef* tdef);
Type* ty_extern_struct(TypeDef* tdef);
Type* ty_fn(Type** params, int param_count, Type* ret);
Type* ty_enum(TypeDef* tdef);

// --- Rendering and predicates ---

// Human-readable name of a type. Uses a small ring of static
// buffers so recursive calls (a ref inside a ref, etc.) do not
// overwrite each other's intermediate output. The ring is 8
// deep; formatting a type nested more than ~8 levels deep will
// clobber outer strings before the caller has a chance to copy
// them. Not a concern for realistic source.
//
// Returned string is never NULL. For an unknown TypeKind the
// function returns "?" rather than falling off the end.
const char* type_name(const Type* t);

// Structural equality. Returns true for identical pointers
// without further inspection (fast path). For TY_RAWPTR, a
// `*void` on either side unifies with the other (the language's
// universal pointer, matching C's void*). For TY_STRUCT and
// TY_ENUM, equality is TypeDef pointer identity — nominal, not
// structural.
bool        type_eq(const Type* a, const Type* b);

// True iff `t` is, or transitively contains, a TY_REF
// somewhere in its structure. Used by the resolver to reject
// `ref<ref<...>>`, `rc<ref<...>>`, etc. Note the search is
// through inner/rc/rawptr/array, not through struct fields;
// a struct containing a ref field does not itself "contain a
// ref" by this predicate.
bool        type_contains_ref(const Type* t);

// True iff the type, when dropped, needs to run a deep drop
// to release any RC-managed sub-values. Used by sema to
// compute RC management for user aggregates.
//
// Recursion:
//   - TY_RC, TY_STRING: true.
//   - TY_ARRAY: recurse into the element type.
//   - TY_STRUCT, TY_ENUM: consult the TypeDef's is_rc_managed
//     flag. The TypeDef's flag may be in flux during a
//     fixpoint computation; false is a sound "not yet known"
//     value.
//   - TY_REF, TY_RAWPTR: false. The referent's lifetime is
//     managed elsewhere; a reference is a view.
//   - Everything else: false.
bool type_contains_rc(const Type* t);

// True iff a value of this type has a runtime representation
// on the Prism stack. Extern structs do not: they live only
// behind pointers or as fields of other extern structs, and
// every construct that would materialize one is rejected in
// sema. Callers use this predicate at exactly the sites where
// a value is required (let, parameter, return, struct field,
// array element).
//
// A NULL type returns false; that matches the "error already
// reported" convention of the other type predicates.
bool type_has_value_representation(const Type* t);

// Integer predicates. All return false for NULL and for
// non-integer kinds. `is_signed_int` and `is_unsigned_int`
// are mutually exclusive and together cover exactly the eight
// integer kinds.
bool        is_integer_type(const Type* t);
bool        is_signed_int(const Type* t);
bool        is_unsigned_int(const Type* t);

// Floating-point types: f32 and f64.
bool is_float_type(const Type* t);

// Scalar types: every integer kind, both floats, and bool.
// These are the types that can participate in numeric
// arithmetic and can be cast to one another by value.
bool is_scalar_type(const Type* t);

// Equatable types: equality (`==`, `!=`) is defined. All
// scalars, plus string and raw pointers. Arrays, structs,
// enums, refs, and rc values are excluded — comparing two
// of them would require structural equality, which the
// language does not have.
bool is_equatable_type(const Type* t);

// Ordered types: ordering (`<`, `<=`, `>`, `>=`) is defined.
// All scalars except bool, plus string (lexicographic).
// Pointers are not ordered; integers are (via cast to u64).
bool is_ordered_type(const Type* t);

// Cast validity. Returns true iff `from` can be converted
// to `to` by an `as` expression:
//   - scalar to scalar (any direction)
//   - pointer to pointer
//   - pointer to i64/u64 and back
// Everything else is rejected.
bool cast_valid(const Type* from, const Type* to);

// Range check: does the I64 value `v` fit in `target` without
// changing sign or truncating bits? For a signed target the
// range is the symmetric two's-complement range; for an
// unsigned target the range is [0, 2^N - 1]. Returns false for
// a non-integer target and for NULL.
bool        int_value_fits(I64 v, Type* target);

// Range check for a non-negative literal. `v` is a U64 bit
// pattern; the target's positive range is consulted:
//   signed target   — v must be <= the target's positive max
//   unsigned target — v must be <= the target's full max
// Non-integer targets return false.
//
// The split from int_value_fits exists because a bare
// literal is always non-negative at the AST level
// (negation is a wrapping AST_UNARY), and because unsigned
// 64-bit values (0xFFFFFFFFFFFFFFFF) cannot be checked
// through an I64 parameter without losing the top bit.
bool uint_value_fits(U64 v, Type* target);

// ============================================================
// Type expressions
//
// A TypeExpr is the *syntactic* form of a type, produced by
// the parser and resolved by sema into a Type*. It is a
// separate tree from Type because the parser may need to
// preserve source-level information (a qualified path, an
// unresolved generic base, an annotation the resolver failed
// to make sense of) that the resolved Type does not carry.
//
// TypeExprs are owned by whoever creates them (typically an
// AST node) and are freed by type_expr_free, which recurses
// through `inner` and `args`.
// ============================================================

// Discriminant for TypeExpr.
//   TEX_NAME      — a bare identifier: `i64`, `Point`.
//   TEX_REF       — `ref<T>` or `ref<mut T>`.
//   TEX_RC        — `rc<T>`.
//   TEX_RAWPTR    — `*T`.
//   TEX_ARRAY     — `[T]`.
//   TEX_QUALIFIED — `mod.Type` (path length > 1).
//   TEX_GENERIC   — `Base<A, B, ...>`.
//   TEX_RESOLVED  — carries a Type* directly. Used by the
//                   monomorphizer to substitute a concrete
//                   type into a copied AST without going
//                   through name lookup.
typedef enum {
    TEX_NAME,
    TEX_REF,
    TEX_RC,
    TEX_RAWPTR,
    TEX_ARRAY,
    TEX_QUALIFIED,     // a.b.c
    TEX_GENERIC,       // Option<i64>
    TEX_RESOLVED,      // carries a Type* directly; used by mono
    TEX_FN,
} TypeExprKind;

typedef struct TypeExpr {
    TypeExprKind kind;
    Span         span;
    Token        name;          // TEX_NAME, TEX_GENERIC (base name)
    bool         is_mut;        // TEX_REF
    struct TypeExpr* inner;     // TEX_REF, TEX_RC, TEX_RAWPTR, TEX_ARRAY

    // TEX_QUALIFIED
    // A heap array of Tokens, one per path component. Owned by
    // this TypeExpr and freed by type_expr_free. The array is
    // copied from the caller's buffer, so a stack array at the
    // call site is fine.
    Token* path;
    int    path_count;

    // TEX_GENERIC
    // The type arguments. Each element is a TypeExpr (which may
    // itself be a generic, ref, etc.). Owned and freed by
    // type_expr_free. `arg_capacity` is the allocated size of
    // `args`; `arg_count` is the number in use. Growth is
    // doubling, starting at 4.
    struct TypeExpr** args;
    int               arg_count;
    int               arg_capacity;

    // TEX_RESOLVED
    // The type this expression stands for. Borrowed; points
    // into the global type universe.
    Type* resolved;

    // TEX_FN — the parameter types (anonymous; no names) and
    // the return type. `params` grows by doubling from 4;
    // `ret` is a single TypeExpr.
    struct TypeExpr** params;
    int               param_count;
    int               param_capacity;
    struct TypeExpr*  ret;
} TypeExpr;

// TypeExpr constructors. Every constructor xcallocs and sets
// kind + span. Fields not meaningful for the kind are left
// zeroed; type_expr_free is safe to call on any kind.
TypeExpr* type_expr_name(Token name);
TypeExpr* type_expr_ref(TypeExpr* inner, bool is_mut, Span span);
TypeExpr* type_expr_rc(TypeExpr* inner, Span span);
TypeExpr* type_expr_rawptr(TypeExpr* inner, Span span);
TypeExpr* type_expr_array(TypeExpr* element, Span span);
TypeExpr* type_expr_qualified(Token* path, int n, Span span);
TypeExpr* type_expr_generic(Token base, Span span);
TypeExpr* type_expr_fn(Span span);
TypeExpr* type_expr_resolved(Type* t, Span span);
void      type_expr_fn_add_param(TypeExpr* te, TypeExpr* param);
void      type_expr_fn_set_return(TypeExpr* te, TypeExpr* ret);
// Append one argument to a TEX_GENERIC. Safe to call multiple
// times; grows the args array by doubling.
void      type_expr_generic_add_arg(TypeExpr* te, TypeExpr* arg);
// Recursively free a TypeExpr and everything it owns (inner,
// args, path). NULL is a no-op. Does NOT free the Type* in a
// TEX_RESOLVED; Types are process-lifetime.
void      type_expr_free(TypeExpr* te);

// ============================================================
// Nominal type table
//
// Owns struct and enum definitions. The `generics` pointer is
// opaque to this file; sema initializes it via mono_init and
// installs a callback that this file invokes when it hits a
// TEX_GENERIC node.
//
// The callback receives `ctx`, the enclosing FnCtx (opaque
// here), which sema uses to resolve the type arguments and
// instantiate. Threading `ctx` through every resolve call is
// the price of keeping the generic machinery out of types.c.
//
// Invariants:
//   - `items` is a heap array of `count` TypeDefs, capacity
//     `capacity`. Growth is doubling, starting at 8.
//   - Entries are append-only. A TypeDef pointer returned by
//     typedef_table_add remains valid until the table is
//     freed, because the array only grows, and realloc
//     preserves existing contents (though it may move them;
//     do not cache raw pointers across an add).
//   - Lookups by name skip entries with is_instantiated
//     == true.
// ============================================================
typedef struct {
    TypeDef* items;
    int      count;
    int      capacity;

    void* generics;

    Type* (*instantiate_generic)(void* generics, TypeExpr* te,
                                 const SourceMap* smap, void* ctx,
                                 uint32_t file_id,
                                 DiagnosticBag* bag);
} TypeDefTable;

void     typedef_table_init(TypeDefTable* tt);
// Frees every owned sub-allocation: fields of each struct,
// payloads of each enum variant, and the variants array. Then
// frees the items array and resets the table.
void     typedef_table_free(TypeDefTable* tt);

// Find a TypeDef by name across every non-instantiated entry.
// Used when the name is unqualified (the source file's own
// definitions and the prelude share one table).
TypeDef* typedef_table_find(TypeDefTable* tt, const SourceMap* smap,
                            Token name);

// Same as above but restricted to entries whose name token
// lives in `file_id`. Used to resolve a name in a specific
// module (for imports) and to look up a local definition
// before falling back to imports. `is_instantiated` entries
// are skipped here too.
TypeDef* typedef_table_find_in_file(TypeDefTable* tt, const SourceMap* smap,
                                    Token name, uint32_t file_id);

// Register a new TypeDef. `name` is copied by span into
// name_buf; the SourceMap is consulted only for that copy.
// Returns a pointer to the new entry, which is zeroed except
// for `name` and `name_buf`. Caller fills kind, is_pub,
// fields/variants, etc. Caller must also, at some later point,
// resolve any StructField.type / EnumVariantDef.payloads to
// real Type*.
TypeDef* typedef_table_add(TypeDefTable* tt, Token name,
                           const SourceMap* smap);

// Look up a builtin TypeDef by name. Returns the static
// TypeDef for `name` (e.g. "string", "i64") or NULL if the
// name is not a builtin. Used by the resolver to make
// builtins visible to name lookup, so that a method
// declaration like `fn string.trim(...)` finds the same
// TypeDef that a reference like `let s: string` finds.
TypeDef* builtin_tdef_by_name(const char* name);

// ============================================================
// Module namespace tracking
//
// One FileNamespace per source file. Populated by sema from
// AST_USING nodes; consulted for module-qualified names and
// selective imports. The module loader (a separate component)
// records each file's imports/selectives into this table so
// the resolver in types.c can find them without walking the
// AST.
// ============================================================

// One `using a.b as local_name` import. `module_file_id` is
// the file the path resolved to, set by the loader.
typedef struct {
    Token   local_name;
    int     module_file_id;
} FileImport;

// One name inside a `using a.b.{ x as y }` declaration.
// `local_name` is what the importing file writes; `source_name`
// is what the target module defines; `module_file_id` is the
// resolved target file. A non-aliased `x` has
// local_name == source_name.
typedef struct {
    Token local_name;      // name in the importing file
    Token source_name;     // name in the target module
    int   module_file_id;
} SelectiveImport;

// Per-file import namespace. Both arrays are growable
// (doubling, starting at 4 for imports and selectives alike).
typedef struct {
    uint32_t    file_id;
    FileImport* imports;
    int         import_count;
    int         import_capacity;
    SelectiveImport* selective;
    int              selective_count;
    int              selective_capacity;
} FileNamespace;

// All FileNamespaces, keyed by file_id. Linear scan on lookup
// (see resolve_inner below); the file count is small.
typedef struct {
    FileNamespace* files;
    int            file_count;
    int            file_capacity;
} ModuleTable;

// ============================================================
// Type expression resolution
//
// `types` may be NULL; without it, only primitives resolve.
// `modtable` may be NULL; without it, qualified names fail.
// `ctx` is the enclosing FnCtx, opaque here, required for
// generic instantiation.
//
// On success returns a non-NULL Type* that lives for the
// process lifetime. On failure returns NULL and reports at
// least one error into `bag`. The function may report multiple
// errors before returning NULL (e.g. an unknown type name plus
// a follow-up note); it does not stop at the first.
// ============================================================
Type* type_expr_resolve(TypeExpr* te, const SourceMap* smap,
                        TypeDefTable* types,
                        const ModuleTable* modtable,
                        uint32_t file_id,
                        void* ctx,
                        DiagnosticBag* bag);

// ============================================================
// Runtime type descriptors
//
// TypeDesc is the runtime's flat, index-addressable
// representation of a type. Codegen interns a Type into a
// TypeDesc via typetable_intern and gets back a uint32_t
// index; that index is what appears in bytecode operands (for
// e.g. OP_ALLOC, OP_PRINT) and in the runtime's type checks.
//
// Descriptor shape by kind:
//   - Scalars (VOID, I*, U*, F*, BOOL, STRING): only kind,
//     size.
//   - REF, RC, RAWPTR, ARRAY: kind, size, `aux` = interned
//     index of the inner TypeDesc.
//   - STRUCT: kind, size, `aux` = start index into
//     TypeTable.fields of the field descriptor indices;
//     `count` = number of fields.
//   - ENUM: kind, size, `aux` = start index into
//     TypeTable.enum_variants; `count` = number of variants.
//
// Two TypeDescs are considered equal iff their (kind, aux,
// size, count) tuples match; interning is what guarantees
// uniqueness, so an interned index is a canonical identity.
// ============================================================

// Kind tag for TypeDesc. Mirrors TypeKind one-for-one, but is
// a distinct enum so the runtime does not depend on the
// compiler's TypeKind values (which could be reordered without
// breaking the wire format). TD_COUNT is the sentinel count;
// it is never a valid descriptor kind.
typedef enum {
    TD_VOID,
    TD_I8, TD_I16, TD_I32, TD_I64,
    TD_U8, TD_U16, TD_U32, TD_U64,
    TD_F32, TD_F64,
    TD_BOOL,
    TD_STRING,
    TD_REF,
    TD_RC,
    TD_RAWPTR,
    TD_ARRAY,
    TD_STRUCT,
    TD_EXTERN_STRUCT,
    TD_ENUM,
    TD_COUNT
} TypeDescKind;

// One runtime type descriptor.
//   kind  — see above.
//   aux   — meaning depends on kind. For REF/RC/RAWPTR/ARRAY
//           it is the interned index of the inner descriptor.
//           For STRUCT/ENUM it is the start index into the
//           corresponding side table. For scalars it is 0.
//   size  — byte size of a value of this type.
//   count — number of fields (structs) or variants (enums);
//           0 for everything else.
typedef struct {
    TypeDescKind kind;
    uint32_t     aux;
    uint32_t     size;
    uint32_t     count;
} TypeDesc;

// Runtime name of one enum variant. Only the name is stored;
// payload descriptors are not carried here today, so runtime
// introspection of enum payloads is limited to the variant
// tag plus the enum's overall type.
// carries the variant's payload descriptor
// indices, so that a runtime deep-drop can walk an RC-managed
// enum's payload. The name is kept for diagnostics and
// rendering; the payload_* fields are the new "structure"
// information.
typedef struct {
    char name_buf[64];
    uint32_t payload_start;   // index into TypeTable.enum_payloads
    uint32_t payload_count;
} EnumVariantDesc;

// The whole runtime type universe for one compilation.
//
//   items / count / capacity
//       — the descriptor array. Grows by doubling from 8.
//   fields / field_count / field_capacity
//       — flattened struct field descriptors. For each struct
//         descriptor, its `aux` indexes into this array and
//         its `count` consecutive entries are the field
//         descriptor indices. Grows by doubling from 16.
//   enum_variants / enum_variant_count / enum_variant_capacity
//       — flattened enum variant names. For each enum
//         descriptor, its `aux` indexes into this array and
//         its `count` consecutive entries are the variant
//         names. Grows by doubling from 16.
//
// The three sub-arrays are not freed or shrunk by anything
// except typetable_free.
typedef struct {
    TypeDesc*        items;
    int              count;
    int              capacity;

    uint32_t*        fields;
    int              field_count;
    int              field_capacity;

    EnumVariantDesc* enum_variants;
    int              enum_variant_count;
    int              enum_variant_capacity;

    uint32_t*        enum_payloads;         
    int              enum_payload_count;    
    int              enum_payload_capacity; 
} TypeTable;

void typetable_init(TypeTable* tt);
void typetable_free(TypeTable* tt);

// Intern a Type and return its descriptor index. If an
// equivalent descriptor already exists (comparison is
// structural, see desc_matches in types.c), returns the
// existing index; otherwise appends a new descriptor (plus
// any needed nested descriptors and struct/enum side entries)
// and returns the new index. Passing NULL yields the index of
// the default descriptor (TD_I64); see the note in types.c.
uint32_t typetable_intern(TypeTable* tt, const Type* t);

// Look up a descriptor by index. Returns NULL for a NULL table
// or an out-of-range index. The returned pointer is borrowed
// and remains valid until the table is freed or grown; do not
// hold across another intern call.
const TypeDesc* typetable_get(const TypeTable* tt, uint32_t idx);

// Is there a safe implicit widening from `from` to `to`?
// "Safe" means every value representable as `from` is
// representable exactly as `to`. The relation is:
//   - signed int → signed int with strictly more bits
//   - unsigned int → unsigned int with strictly more bits
//   - unsigned int → signed int with strictly more bits
//   - int with ≤ 24 bits → f32
//   - int with ≤ 53 bits → f64
//   - f32 → f64
// Nothing else. In particular, f64 → f32 is NOT in the set:
// it narrows and can lose precision.
bool type_widens_to(const Type* from, const Type* to);

#endif // PRISM_TYPES_H