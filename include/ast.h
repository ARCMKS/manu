// ============================================================
// ast.h — Abstract Syntax Tree.
//
// Every node carries a Span (source location), a resolved
// Type* (set by sema), and per-kind payloads. `is_pub` is set
// by the parser when the declaration was prefixed with `pub`
// and is meaningful only for top-level `fn`, `const`, and
// `type` nodes.
//
// Generic declarations carry `type_params` and
// `type_param_count` in their payloads. When a declaration has
// type parameters, sema registers it with the monomorphizer
// instead of resolving it concretely; use sites with explicit
// `<...>` arguments trigger instantiation.
//
// Design notes worth knowing before reading the node struct:
//
//   - Nodes are allocated with xcalloc and never reallocated.
//     Everything is a pointer; the tree is acyclic for all
//     node kinds except no kind (there are no back-pointers in
//     the AST itself; the only cross-link is `type`, which
//     points into the type table, not back into the AST).
//
//   - The union `as` is read according to `kind`. There is no
//     tag validation at runtime; misreading a node is a bug,
//     not a diagnostic. Union size is dominated by the biggest
//     variant, which today is `func` (8 Param + 8 Token + a
//     handful of pointers); the struct is therefore a fixed
//     ~512-byte allocation per node regardless of kind. That
//     is a deliberate tradeoff of memory for simplicity.
//
//   - Sentinel -1 is used consistently for "not yet assigned"
//     integer slots: local_index, const_id, func_idx, ffi_idx,
//     sdef_index, tdef_index, tag, offset, slot numbers, etc.
//     Any constructor that should set -1 does so explicitly;
//     the xcalloc'd 0 is never relied on to mean "unset".
//
//   - `type` is filled in by sema, not by the parser or by the
//     constructors. Reading `n->type` before sema has run is
//     always wrong.
// ============================================================
#ifndef PRISM_AST_H
#define PRISM_AST_H

#include "common.h"
#include "lexer.h"
#include "types.h"

// ------------------------------------------------------------
// Node kinds
//
// Grouped roughly by layer:
//   - Program and top-level declarations (PROGRAM..EXTERN_LIB)
//   - Statements (BLOCK..EXPR_STMT)
//   - Expressions (INT..ALLOC_RC)
//   - Patterns and match (MATCH..PAT_VARIANT)
//
// The order within each group is not semantically meaningful.
// If you add a kind, add a matching case in ast_free's switch
// and (usually) a constructor at the bottom of this header.
// ------------------------------------------------------------
typedef enum {
    // Top-level. Each AST_PROGRAM holds a list of these.
    AST_PROGRAM,       // root: { decls, file_id, canonical_path }
    AST_FUNC,          // fn declaration (or method, or extern fn)
    AST_CONST,         // top-level `const name = expr;`
    AST_USING,         // `using ...` import / alias declaration
    AST_STRUCT_DEF,    // `struct Name { ... }`
    AST_ENUM_DEF,      // `enum Name { ... }`
    AST_STRUCT_FIELD,  // one field inside a struct def
    AST_ENUM_VARIANT,  // one variant inside an enum def
    AST_TYPE_EXPR_NODE,// wrapper that lets a TypeExpr sit in an AstList
    AST_EXTERN_LIB,    // `extern "libname" [as alias]`

    // Statements. All of these appear inside AST_BLOCK.stmts.
    AST_BLOCK,         // `{ ... }` — the only compound stmt
    AST_LET,           // `let [mut] name [: T] = init;`
    AST_ASSIGN,        // `target = value;`
    AST_ASSIGN_OP,
    AST_IF,            // `if cond { .. } [else { .. }]`
    AST_WHILE,         // `while cond { .. }`
    AST_FOR_RANGE,     // `for i in a..b { .. }` (desugared by sema)
    AST_FOR_IN,        // `for x in iterable { .. }`
    AST_BREAK,         // `break;`
    AST_CONTINUE,      // `continue;`
    AST_RETURN,        // `return [expr];`
    AST_EXPR_STMT,     // an expression used as a statement

    // Expressions.
    AST_INT,           // integer literal
    AST_FLOAT,         // float literal
    AST_BOOL,          // true / false
    AST_STRING,        // "..." Prism string (RC-managed at runtime)
    AST_CSTRING,       // c"..." C string (FFI payload)
    AST_VAR,           // identifier reference (local or const)
    AST_BINARY,        // left op right
    AST_UNARY,         // -x or !x
    AST_CALL,          // fn call, method call, or builtin call
    AST_CALL_INDIRECT, // fn value call
    AST_DEREF,         // *ptr  (with an optional field offset)
    AST_CAST,          // `expr as Type`
    AST_TRY,           // `expr?` (Result/Option propagation)
    AST_ARRAY_LIT,     // `[a, b, c]`
    AST_INDEX,         // `arr[i]`
    AST_STRUCT_LIT,    // `Name { field: v, ... }`
    AST_STRUCT_INIT,   // one `field: v` inside a struct literal
    AST_FIELD,         // `expr.name` — field access OR variant tag
    AST_VARIANT_CTOR,  // `Enum.Variant` / `Enum.Variant(args)` / method call
    AST_ALLOC_ARENA,   // `arena expr` — bump-arena allocation
    AST_ALLOC_RC,      // `rc expr` — RC-heap allocation
    AST_ADDR_OF_FIELD, // sems-rewritten `&p.x` on an extern field
    AST_SIZEOF,        // sizeof(T), soft-keyword form

    // Pattern matching.
    AST_MATCH,         // `match scrutinee { arm... }`
    AST_MATCH_ARM,     // one `pattern => body` arm
    AST_PAT_WILDCARD,  // `_`
    AST_PAT_BIND,      // `name` binding in a pattern
    AST_PAT_INT,       // integer literal pattern
    AST_PAT_BOOL,      // true / false pattern
    AST_PAT_STRING,    // string literal pattern
    AST_PAT_VARIANT,   // `Enum.Variant(...)` pattern
} AstKind;

// Binary operators. Ordering is not semantically meaningful;
// the parser maps TokenKind to one of these in ast_binary's
// call sites. There is no BIN_AND / BIN_OR; Prism has no
// short-circuit boolean operators at the AST level today (see
// Follow-ups at the bottom of ast.c).
typedef enum {
    BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV,
    BIN_EQ,  BIN_NE,  BIN_LT,  BIN_LE, BIN_GT, BIN_GE,
    BIN_AND, BIN_OR,
} BinOp;

// Unary operators. Only two exist: arithmetic negation and
// logical not.
typedef enum { UN_NEG, UN_NOT } UnOp;

// Memory tier of a value, recorded on the node that produces
// it (or on the node that refers to it). Used by codegen to
// decide how to emit drops; sema sets it.
//
//   TIER_NONE  — no allocation, or an allocation that is not
//                owned here (a borrowed reference, a literal,
//                a scalar).
//   TIER_ARENA — allocated in the bump arena. Freed en masse
//                when the arena is reset; individual drops are
//                no-ops.
//   TIER_RC    — allocated on the RC heap. Refcount-managed;
//                dropping is a real operation.
typedef enum { TIER_NONE, TIER_ARENA, TIER_RC } Tier;

// ------------------------------------------------------------
// Growable lists of AST pointers
//
// Used anywhere a node needs a variable-length sequence of
// children: program decls, block stmts, call args, struct
// fields, enum variants, array elements, match arms, etc.
//
// Ownership: an AstList owns its elements. Freeing the list
// recursively frees each element (which frees that element's
// children). There is no aliasing: a single AST node appears
// in exactly one list.
// ------------------------------------------------------------
struct AstNode;
typedef struct AstNode AstNode;

typedef struct {
    AstNode** items;
    int       count;
    int       capacity;
} AstList;

// Zero-initialize. Safe on uninitialized stack storage.
void ast_list_init(AstList* l);
// Append; grows geometrically (8, 16, 32, ...). Ownership of
// `n` transfers to the list.
void ast_list_push(AstList* l, AstNode* n);
// Move every element of `src` to the front of `dst`, preserving
// order, then empty `src`. Ownership transfers. After return,
// src->count == 0 and src->items is still allocated (the caller
// must ast_list_free it to release the buffer). See ast.c for
// the two-loop shift.
void ast_list_prepend_all(AstList* dst, AstList* src);
// Free every element (recursively) plus the items buffer, then
// reset the list to its zero state.
void ast_list_free(AstList* l);

// ------------------------------------------------------------
// Shared payload structs
// ------------------------------------------------------------

// One function parameter. Used inside AST_FUNC.params.
//
// Invariants after sema:
//   - `type` is non-NULL for every parameter, including ones
//     whose source had no annotation (type inference fills it
//     in, or a diagnostic is reported if it cannot).
//   - `is_mut` is false unless the source wrote `mut name: T`.
//     The parser sets it from the source; sema enforces that
//     assigning to a non-mut parameter is an error.
//
// `type_expr` is the syntactic annotation, kept because the
// parser produces it and because diagnostics can point at it
// if sema later complains about the type. It is NULL when the
// parameter was written without an annotation.
typedef struct {
    Token        name;
    TypeExpr*    type_expr;    // NULL if unannotated
    struct Type* type;         // resolved by sema
    bool         is_mut;
} Param;

// One name inside a selective `using` declaration
// (`using a.b.{ x, y as z }`).
//
// `source` is the name as written in the target module
// (e.g. `y` in the example above). `local` is the name that
// the importing file will use to refer to it: `z` for the
// aliased entry, or `source` itself for the non-aliased ones
// (`x`). Storing both means the resolver does not have to
// re-derive the local name from the presence of `as`.
typedef struct {
    Token local;      // name introduced into the importing file
    Token source;     // name in the target module
} UsingName;

// What flavor of `using` a given AST_USING node is.
//
//   USING_MODULE    — `using a.b.c` or `using a.b.c as X`.
//                     Brings the whole module under `c` (or
//                     `X`).
//   USING_SELECTIVE — `using a.b.{ x, y as z }`. Imports only
//                     the listed symbols, possibly renamed.
//   USING_ALIAS     — `using Name = TypeExpr`. A type alias,
//                     not a module import. Has a `alias_target`
//                     instead of a `path`.
//
// The three kinds share one AST node kind (AST_USING) and one
// payload struct (AstUsing). Only a subset of the fields is
// meaningful per kind; see the comments on AstUsing below.
typedef enum {
    USING_MODULE,      // using a.b.c           or  using a.b.c as X
    USING_SELECTIVE,   // using a.b.{ x, y as z }
    USING_ALIAS,       // using Name = TypeExpr
} UsingKind;

// Payload for AST_USING. Field applicability by kind:
//
//   USING_MODULE:    path, path_count, module_alias,
//                    has_module_alias, resolved_file_id,
//                    resolved_path_count
//   USING_SELECTIVE: path, path_count, names, name_count,
//                    name_capacity, resolved_file_id,
//                    resolved_path_count
//   USING_ALIAS:     alias_local, alias_target
//
// Fields not applicable to a given kind are left zeroed by the
// constructor. Do not read them for the wrong kind.
//
// `path` is a heap array of Token (module path components, in
// source order). The constructor copies the caller's array, so
// the caller's buffer can be a stack array that goes out of
// scope. The tokens reference the source file, not copied text.
//
// `resolved_file_id` and `resolved_path_count` are outputs of
// the module loader: they record which file the path actually
// resolved to, and how many leading components of the path
// named that file. The remaining path components (if any) name
// a declaration inside that file. Before the loader runs, both
// are 0 / uninitialized; sema must not consult them.
typedef struct {
    UsingKind kind;

    // Module path, e.g. `a.b.c` is three tokens.
    Token* path;
    int    path_count;

    // Optional `as X` on a module import. has_module_alias
    // distinguishes "no alias was written" from "an alias token
    // that happens to be zero-valued", which matters because a
    // zero Token has file_id 0 and would otherwise look like
    // SPAN_NONE.
    Token  module_alias;
    bool   has_module_alias;

    // Selective imports. Grows by doubling starting at 4.
    UsingName* names;
    int        name_count;
    int        name_capacity;

    // Type alias form.
    Token     alias_local;
    TypeExpr* alias_target;

    // Set by loader; see comment above.
    int resolved_file_id;       // set by loader for MODULE/SELECTIVE
    int resolved_path_count;    // leading components that named a file
} AstUsing;

// ------------------------------------------------------------
// The node
//
// All nodes have the same struct layout regardless of kind, so
// AstNode is a fixed-size allocation. Fields above the union
// are meaningful for every kind; the union holds the per-kind
// payload.
//
// Fields above the union:
//   kind         — discriminant; determines which union member
//                  is active.
//   span         — source range this node came from. Used by
//                  diagnostics. For synthesized nodes (e.g.
//                  AST_TYPE_EXPR_NODE wrapping a TypeExpr) the
//                  span is borrowed from the wrapped thing.
//   type         — the resolved type. Set by sema only. NULL
//                  until then.
//   is_pub       — `pub` prefix on a top-level decl. Set by the
//                  parser; only meaningful for AST_FUNC,
//                  AST_CONST, AST_STRUCT_DEF, AST_ENUM_DEF, and
//                  AST_USING. Ignored on everything else.
//   is_builtin   — set by sema on AST_CALL nodes that resolve
//                  to a builtin (raw_is_null, ffi helpers, etc.)
//                  instead of a user function. Codegen dispatches
//                  on this flag.
//   local_index  — for AST_VAR / AST_PAT_BIND / AST_LET, the
//                  slot in the enclosing function's local
//                  frame. -1 if not a local (e.g. an unresolved
//                  identifier is an error anyway; a const
//                  reference uses const_id instead).
//   const_id     — for AST_VAR that references a `const`, the
//                  index into the const table. -1 otherwise.
//   holds_rc     — true iff this expression's value owns an RC
//                  reference that must eventually be dropped.
//                  Set by sema; used by codegen to decide
//                  whether to emit a drop at a statement
//                  boundary.
//   tier         — memory tier of the value this node refers
//                  to. Set by sema alongside holds_rc.
// ------------------------------------------------------------
struct AstNode {
    AstKind      kind;
    Span         span;
    struct Type* type;
    bool         is_pub;
    bool         is_builtin;

    int  local_index;      // >= 0 iff this references a local
    int  const_id;         // >= 0 iff this references a const
    bool holds_rc;
    Tier tier;

    union {
        // ---- Top-level ----

        // Root of the compilation. One AST_PROGRAM per source
        // file; the loader chains them by file_id. `decls`
        // holds the file's top-level items in source order.
        // `canonical_path` is owned (heap) and is NULL until
        // the loader sets it.
        struct {
            AstList  decls;
            uint32_t file_id;
            char*    canonical_path;
        } program;

        // Function or method declaration, including externs.
        //
        // Field notes:
        //   method_owner  — if the function is a method, the
        //                   type-name token it is attached to
        //                   (e.g. `Point` in `fn Point.move`).
        //                   Zero-valued for free functions.
        //   type_params   — up to 8 generic parameters. There is
        //                   a hard cap of 8, enforced by the
        //                   parser; exceeding it is a parser
        //                   error, not a silent truncation.
        //   params        — up to 8 parameters, same hard cap.
        //   return_type_expr / return_type — syntactic and
        //                   resolved forms of the return type.
        //                   return_type_expr may be NULL if the
        //                   source omitted it (a `fn` with no
        //                   `-> T` returns unit).
        //   body          — AST_BLOCK, or NULL for an extern fn
        //                   (which has no body).
        //   func_idx      — index into the function table, set
        //                   by sema. -1 until then. For externs
        //                   this is still set (the extern is
        //                   registered as a function); ffi_idx
        //                   is the FFI-specific counterpart.
        //   is_extern     — true iff this `fn` was written with
        //                   the `extern` keyword. Codegen never
        //                   emits a body for these; the call
        //                   site goes through OP_FFI_CALL.
        //   lib_alias     — for externs, the library alias the
        //                   function belongs to (`extern "c"` =>
        //                   alias `c`). Zero-valued otherwise.
        //   is_variadic   — trailing `...` in the parameter
        //                   list; only legal on externs.
        //   ffi_idx       — index into the FFI table. -1 for
        //                   non-extern functions.
        struct {
            Token        name;
            Token        method_owner;
            Token        type_params[8];
            int          type_param_count;
            Param        params[8];
            int          param_count;
            TypeExpr*    return_type_expr;
            struct Type* return_type;
            AstNode*     body;
            int          func_idx;

            bool         is_extern;     // extern declaration
            Token        lib_alias;     // 
            bool         is_variadic;   // trailing '...'
            int          ffi_idx;       // >=0 iff is_extern, index into FFI_Table
        } func;

        // Top-level `const NAME = expr;`. `initializer` is any
        // expression; sema folds it to a constant and stores the
        // result in the const table (const_id is not used here,
        // it is used on AST_VAR references to the const).
        struct {
            Token    name;
            AstNode* initializer;
            TypeExpr* type_expr;  // optional `: Type` annotation
        } const_decl;

        // Entire `using` payload. See AstUsing above.
        AstUsing using_decl;

        // Struct definition. `fields` is a list of
        // AST_STRUCT_FIELD nodes in source order. `sdef_index`
        // is the index into the struct table, set by sema;
        // -1 until then.
        struct {
            Token   name;
            Token   type_params[8];
            int     type_param_count;
            AstList fields;
            int     sdef_index;
            // flags for ffi handling
            // `unions` is only available for extern types
            // prism has emun as sum type which already cover the use cases
            bool    is_extern;
            bool    is_opaque;
            bool    is_union;   
        } struct_def;

        // Enum definition. `variants` is a list of
        // AST_ENUM_VARIANT nodes. `tdef_index` is the index
        // into the type table (enum defs live in the type
        // table, not a separate one); -1 until sema.
        struct {
            Token   name;
            Token   type_params[8];
            int     type_param_count;
            AstList variants;
            int     tdef_index;
        } enum_def;

        // One field in a struct definition. `offset` is the
        // byte offset within the struct's runtime layout,
        // computed by sema. 0 before sema (do not confuse with
        // "field at offset 0", which is also a valid resolved
        // value; check sdef_index on the parent struct if you
        // need to know whether sema has run).
        struct {
            Token        name;
            TypeExpr*    type_expr;
            struct Type* type;
            int          offset;
        } struct_field;

        // One variant in an enum definition.
        //
        // payload_exprs is a list of AST_TYPE_EXPR_NODE wrappers
        // around the variant's payload types, in source order.
        // payloads / payload_count are the resolved Type* array,
        // allocated and filled by sema. tag is the variant's
        // discriminant (0, 1, 2, ...) assigned by sema in
        // declaration order.
        //
        // Note: payloads is a raw array owned by the AST node
        // (freed in ast_free — see ast.c for the current status
        // of that free). payload_exprs and payloads cover the
        // same information in different forms; both are kept
        // because diagnostics want the syntactic form and
        // codegen wants the resolved form.
        struct {
            Token         name;
            AstList       payload_exprs;   // AST_TYPE_EXPR_NODE wrappers
            struct Type** payloads;        // resolved by sema
            int           payload_count;
            int           tag;
        } enum_variant;

        // Wrapper that lets a TypeExpr participate in an AstList
        // (which otherwise holds AstNode*). The TypeExpr is
        // owned by this node and freed in ast_free.
        struct { TypeExpr* te; } type_expr_node;

        // ---- Statements ----

        // Compound statement. `stmts` is an ordered list of
        // statement nodes. A block is itself a statement and
        // also the body of `fn`, `if`, `while`, `for`, etc.
        struct { AstList stmts; } block;

        // `let [mut] name [: T] = init;`.
        //   type_expr — annotation, may be NULL.
        //   type      — resolved type; always non-NULL after sema.
        //   init      — initializer expression; may be NULL for
        //               a declaration-only `let` (rare but legal).
        //   is_mut    — true iff `mut` appeared in the source.
        //               Default false (immutable binding).
        struct {
            Token        name;
            TypeExpr*    type_expr;
            struct Type* type;
            AstNode*     init;
            bool         is_mut;
        } let;

        // `target = value;`. The parser rejects anything but a
        // var, deref, field, or index as `target`; sema then
        // checks mutability.
        struct {
            AstNode* target;
            AstNode* value;
        } assign;

        struct {
            BinOp    op;
            AstNode* target;
            AstNode* value;
            int      temp_slot;   // >= 0 iff target type holds RC; -1 otherwise
        } assign_op;
        
        // `if cond { .. } [else { .. }]`. `else_` is NULL when
        // absent; when present it is either an AST_BLOCK (the
        // common case) or another AST_IF (for `else if` chains,
        // which the parser builds right-recursively).
        struct {
            AstNode* cond;
            AstNode* then_;
            AstNode* else_;
        } if_;

        // `while cond { .. }`.
        struct {
            AstNode* cond;
            AstNode* body;
        } while_;

        // `for var in start..end { .. }`. The three slots are
        // sema-assigned local slot numbers for the loop variable
        // and the two loop bounds. -1 until sema. `start` and
        // `end` are expressions evaluated once at loop entry.
        // `end` is exclusive; there is no inclusive `..=`.
        struct {
            Token    var;
            AstNode* start;
            AstNode* end;
            AstNode* body;
            int      var_slot;
            int      end_slot;
        } for_range;

        // `for var in iterable { .. }`. `iterable` is an
        // expression producing an array (or a type that yields
        // one). Four slot numbers are assigned by sema:
        //   var_slot — current element
        //   arr_slot — the iterable value, cached once
        //   len_slot — the array's length, cached once
        //   idx_slot — the loop index
        // -1 until sema.
        struct {
            Token    var;
            AstNode* iterable;
            AstNode* body;
            int      var_slot;
            int      arr_slot;
            int      len_slot;
            int      idx_slot;
        } for_in;

        // `return [value];`. value is NULL for a bare return.
        struct { AstNode* value; } ret;

        // An expression statement. The expression's value is
        // discarded; sema is responsible for complaining if the
        // expression cannot be discarded (e.g. a must-use
        // Result — not enforced today).
        struct { AstNode* expr; } expr_stmt;

        // ---- Expressions ----

        // Literals. AST_STRING and AST_CSTRING both use the
        // `string_lit.content` slot (union member named after
        // the AST_STRING case; CSTRING reuses it, see ast.c).
        // `content` is the *inner* text of the literal (between
        // the quotes), with its own span, so diagnostics on a
        // string literal can point inside it.
        struct { U64    value; } int_lit;
        struct { double value; } float_lit;
        struct { bool   value; } bool_lit;
        struct { Token  content; } string_lit;

        // Identifier reference. Resolution fields on the parent
        // node decide what it means: local_index >= 0 means a
        // local, const_id >= 0 means a top-level const, both -1
        // means sema failed to resolve it (and reported an
        // error).
        struct { Token  name; int func_idx; } var;

        // Binary and unary expression nodes. Both operands are
        // owned.
        struct { BinOp op; AstNode* left; AstNode* right; } binary;
        struct { UnOp  op; AstNode* operand; } unary;

        // A call. Covers user functions, builtins, externs, and
        // method calls; the resolution fields disambiguate:
        //   builtin_id — >= 0 iff this is a builtin (set by sema)
        //   func_idx   — >= 0 iff this is a user function
        //   ffi_idx    — >= 0 iff this is an extern
        // Exactly one of the three is set on a successful call
        // after sema; -1 on the others.
        //   instantiation — explicit generic args (`f<i64>`), or
        //                   NULL for non-generic / inferred calls.
        struct {
            Token     name;
            TypeExpr* instantiation;   // `f<i64>` or NULL
            AstList   args;
            int       builtin_id;
            int       func_idx;
            int       ffi_idx;      // >= 0 iff this call targets an extern function
        } call;

        // Indirect call: the callee is an expression of
        // function type, not a name. Produced by sema when
        // an `AST_CALL` node's name resolves to a local of
        // function type; the parser never emits this kind.
        struct {
            AstNode* callee;
            AstList  args;
        } call_indirect;
        

        // `*expr`. `offset` is a byte offset added to the
        // pointer before the load; used to implement field
        // access through a pointer without a separate node
        // kind. 0 for a plain dereference.
        struct { AstNode* operand; int offset; } deref;

        // `expr as Type`. `target_type_expr` is the syntactic
        // target; `target_type` is the resolved Type* (set by
        // sema). Both are kept for the same reason
        // struct_field keeps both.
        struct {
            AstNode*     operand;
            TypeExpr*    target_type_expr;
            struct Type* target_type;
        } cast;

        // `expr?` — Result/Option propagation. Desugared by
        // codegen into a match-like check + early return.
        struct { AstNode* operand; } try_;

        // Array literal. Elements are expressions; the element
        // type is inferred by sema.
        struct { AstList elements; } array_lit;

        // `target[index]`. Both children are owned. `index`
        // must be an integer expression (checked by sema).
        struct {
            AstNode* target;
            AstNode* index;
        } index;

        // `Name { field: v, ... }`. `inits` is a list of
        // AST_STRUCT_INIT nodes; `sdef_index` is the resolved
        // struct table index (-1 until sema).
        struct {
            Token   type_name;
            TypeExpr* instantiation;
            AstList inits;
            int     sdef_index;
            int     hidden_slot;    //temp local for extern struct literals
            struct Type* expected_type; //contextual hint
        } struct_lit;

        // One `field: v` inside a struct literal. `offset` is
        // filled by sema (the field's byte offset in the struct
        // layout). -1 before sema.
        struct {
            Token    name;
            AstNode* value;
            int      offset;
            struct Type* field_type;    //resolved field type
        } struct_init;

        // `expr.name`. Also used for the bare variant-tag case
        // (`Enum.Variant` with no payload and no call): in that
        // case `unit_tag` is the variant's tag and `offset` is
        // the tag's byte offset in the runtime value.
        //   unit_tag == -1  =>  field access
        //   unit_tag >=  0  =>  variant tag
        // `owner` is the resolved struct/enum type; NULL until
        // sema.
        struct {
            AstNode*     target;
            Token        field;
            struct Type* owner;
            int          offset;
            int          unit_tag;   // -1 = field access; >= 0 = variant tag
            struct Type* declared_type;  // the field's declared type
        } field;

        // `Enum.Variant` / `Enum.Variant(args)` / method call
        // on a receiver. Three distinct cases unified in one
        // node kind:
        //   - enum variant construction: mod_func_idx == -1,
        //     tag >= 0, tdef_index set.
        //   - call of a function exported from a module: this
        //     is the "mod" case; mod_func_idx / mod_ffi_idx
        //     name the target, tag == -1.
        //   - method call: receiver_expr is non-NULL and
        //     mod_func_idx names the method.
        // `instantiation` carries explicit generic args.
        struct {
            Token     type_name;
            TypeExpr* instantiation;   // `Option<i64>` or NULL
            Token     variant_name;
            AstList   args;
            int       tdef_index;
            int       tag;
            int       mod_func_idx;    // -1 = enum variant; >= 0 = module call
            int       mod_ffi_idx;
            AstNode*  receiver_expr;    // NULL except for methods
        } variant_ctor;

        // `arena expr` — allocate in the bump arena. is_mut
        // records whether the result is treated as mutable (the
        // arena itself does not care; this affects the language-
        // level mutability check on the resulting reference).
        struct { AstNode* value; bool is_mut; } alloc_arena;
        // `rc expr` — allocate on the RC heap.
        struct { AstNode* value; }              alloc_rc;

        struct {
            TypeExpr*       te;         // the type operand
            struct Type*    resolved;
            I64             size;
        } sizeof_expr;

        // NEW: the address of a storage location in C memory.
        // Only produced by sema, when `&e` is applied to an
        // extern struct field access. The child is always an
        // AST_FIELD whose owner is a TY_EXTERN_STRUCT.
        struct { AstNode* field; }              addr_of_field;

        // ---- Pattern matching ----

        // `match scrutinee { arm... }`. hidden_slot is a sema-
        // assigned local slot used to cache the scrutinee so
        // each arm can re-read it without re-evaluating. -1
        // until sema.
        struct {
            AstNode* scrutinee;
            AstList  arms;
            int      hidden_slot;
        } match;

        // One arm. `pattern` is a pattern node (PAT_*), `body`
        // is an expression or block.
        struct {
            AstNode* pattern;
            AstNode* body;
        } match_arm;

        // Pattern nodes. `slot` is the sema-assigned local slot
        // for the bound name, -1 until sema. The other scalar
        // patterns carry the literal they match on.
        struct { Token name; int slot; } pat_bind;
        struct { I64   value; } pat_int;
        struct { bool  value; } pat_bool;
        struct { Token content; } pat_string;

        // `Enum.Variant(p1, p2, ...)` pattern.
        //   subpatterns — owned list of child patterns.
        //   tag         — variant discriminant, set by sema.
        //   sub_count   — number of subpatterns, redundant with
        //                 subpatterns.count but cached because
        //                 codegen reads it often.
        //   tmp_slots   — up to 16 scratch locals the codegen
        //                 uses to destructure the payload. Hard
        //                 cap; a variant with more than 16
        //                 payload fields is rejected by sema.
        //   instantiation — explicit generic args for the
        //                 variant's enum type, or NULL.
        struct {
            Token    type_name;
            Token    variant_name;
            AstList  subpatterns;
            int      tag;
            int      sub_count;
            int      tmp_slots[16];
            TypeExpr* instantiation;
        } pat_variant;
    } as;
};

// ------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------

// Recursively free a node and everything it owns: child nodes,
// owned lists, owned TypeExprs, and any owned raw buffers
// (e.g. AstUsing.path, AstUsing.names, program.canonical_path).
// NULL is a no-op. See ast.c for per-kind details and for one
// known "do not free" case in AST_ENUM_VARIANT.
void ast_free(AstNode* n);

// ------------------------------------------------------------
// Constructors
//
// Every constructor xcallocs the node and then fills in the
// fields that must not be left at zero (notably -1 sentinels).
// Fields not mentioned in a constructor's own comment are left
// zeroed and are either "not applicable for this kind" or are
// meant to be filled by sema later.
//
// Spans: constructors derive the node's span from a child node
// or a token, whichever is available. Where neither is
// available (e.g. ast_break), the caller passes the span.
// ------------------------------------------------------------
AstNode* ast_program(void);
AstNode* ast_func(Token name, AstNode* body);
AstNode* ast_const(Token name, AstNode* initializer);
AstNode* ast_block(Span span);
AstNode* ast_let(Token name, AstNode* init);
AstNode* ast_assign(AstNode* target, AstNode* value);
AstNode* ast_assign_op(BinOp op, AstNode* target, AstNode* value);  
AstNode* ast_if(AstNode* cond, AstNode* then_, AstNode* else_);
AstNode* ast_while(AstNode* cond, AstNode* body);
AstNode* ast_for_range(Token var, AstNode* start, AstNode* end, AstNode* body);
AstNode* ast_for_in(Token var, AstNode* iterable, AstNode* body);
AstNode* ast_break(Span span);
AstNode* ast_continue(Span span);
AstNode* ast_return(Span span, AstNode* value);
AstNode* ast_expr_stmt(AstNode* expr);
AstNode* ast_int(Span span, U64 value);
AstNode* ast_float(Span span, double value);
AstNode* ast_bool(Span span, bool value);
AstNode* ast_string(Span span, Token content);
AstNode* ast_cstring(Span span, Token content);
AstNode* ast_var(Token name);
AstNode* ast_binary(BinOp op, AstNode* l, AstNode* r, Span span);
AstNode* ast_unary(UnOp op, AstNode* operand, Span span);
AstNode* ast_call(Token name);
AstNode* ast_call_indirect(AstNode* callee, Span span);  
AstNode* ast_deref(AstNode* operand, Span span);
AstNode* ast_cast(AstNode* operand, TypeExpr* target, Span span);
AstNode* ast_try(AstNode* operand, Span span);
AstNode* ast_array_lit(Span span);
AstNode* ast_index(AstNode* target, AstNode* index, Span span);
AstNode* ast_alloc_arena(AstNode* value, Span span, bool is_mut);
AstNode* ast_alloc_rc(AstNode* value, Span span);
AstNode* ast_sizeof(Span span, TypeExpr* te);
// Public alias for the private alloc_node used by ast.c. Exists
// so callers that need a bare node (usually tests or one-off
// tools) can obtain one without going through a specific
// constructor. The node has the same defaults as any other
// alloc_node result; the caller is responsible for filling the
// union.
AstNode* ast_alloc_node(AstKind k, Span span);
AstNode* ast_field(AstNode* target, Token field);
AstNode* ast_struct_def(Token name);
AstNode* ast_struct_field(Token name, TypeExpr* type_expr);
AstNode* ast_struct_lit(Token type_name);
AstNode* ast_struct_init(Token name, AstNode* value);
AstNode* ast_enum_def(Token name);
AstNode* ast_enum_variant(Token name);
AstNode* ast_type_expr_node(TypeExpr* te);
AstNode* ast_variant_ctor(Token type_name, Token variant_name);
AstNode* ast_match(AstNode* scrutinee, Span span);
AstNode* ast_match_arm(AstNode* pattern, AstNode* body, Span span);
AstNode* ast_pat_wildcard(Span span);
AstNode* ast_pat_bind(Token name);
AstNode* ast_pat_int(Span span, I64 value);
AstNode* ast_pat_bool(Span span, bool value);
AstNode* ast_pat_string(Span span, Token content);
AstNode* ast_pat_variant(Token type_name, Token variant_name, Span span);

// `using` constructors. The path array passed in is copied; the
// caller's buffer may be a stack array that goes out of scope.
// `n` is the number of path components. For ast_using_module,
// `alias` and `has_alias` describe an optional `as X` suffix;
// `has_alias` distinguishes "no alias" from "alias token that
// happens to be zero-valued".
AstNode* ast_using_module(Token* path, int n, Token alias, bool has_alias);
AstNode* ast_using_selective(Token* path, int n);
// Append one selective-import name to a node created by
// ast_using_selective. `alias` is the local name; `has_alias`
// distinguishes `x as y` from plain `x`. Safe to call multiple
// times to build up the list.
void     ast_using_selective_add(AstNode* n, Token sym, Token alias,
                                 bool has_alias);
AstNode* ast_using_alias(Token name, TypeExpr* target);
// `extern "libpath" [as alias]`. Stores the path and alias in
// the AstUsing payload (kind is left zeroed, so reading `kind`
// on an AST_EXTERN_LIB node is not meaningful).
AstNode* ast_extern_lib(Token path, Token alias);

#endif // PRISM_AST_H