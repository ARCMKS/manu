## `docs/arch/files/05-ast.md`

# `include/ast.h`, `src/ast.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

The AST is the parser's output and the input to every stage from
sema through codegen. `ast.h` declares the node kinds, the shared
payload structs, the `AstNode` struct, and the constructor and
destructor API. `ast.c` implements allocation, growth of the node
list, the destructor, and the constructors.

**What it solves.** One uniform node type that every stage can walk
without knowing its shape. A single allocation and free path.

**What it does not solve.** Type checking (sema), code generation
(codegen), type resolution (types.c), monomorphization
(monomorph.c). The AST is a data structure, not a pass.

**Who depends on it.** Every front-end and middle-end file:
`parser.c` builds it, `sema*.c` analyzes it, `codegen.c` consumes
it, `monomorph.c` deep-copies it, `resolve.c` and `types.c` read
its declarations. `ast.c` and `ast.h` are dependencies of nearly
every translation unit.

---

## §2 — The one-node design

Three properties shape the whole file.

**One node type, not a class hierarchy.** Every node is an
`AstNode`, distinguished by a `kind` field. The kind is a plain
enum, not a vtable. Dispatch is a `switch` at every consumer. This
trades type safety for uniform allocation, uniform free, and the
ability to walk a tree without knowing its shape.

**Every node carries its own span and (after sema) its own type.**
`Span span` is set at construction. `Type* type` is NULL until
sema fills it. This is what lets codegen read a type off any
expression node without keeping its own environment.

**Per-kind payloads live in one union.** `as.program`, `as.func`,
`as.binary`, and so on. A node reads and writes only the union
member its kind designates; the C language does not enforce this,
so the discipline is documented per kind rather than checked.

The struct is roughly 512 bytes regardless of kind because the
union is sized by its largest member (the `func` payload, which
has `Token type_params[8]`, `Param params[8]`, and several
pointers). Memory is traded for simplicity. A tagged-union-of-
smaller-structs design would save space but add an indirection
at every field access.

---

## §3 — Public interface

### §3.1 `AstKind`

```c
typedef enum {
    AST_PROGRAM,
    AST_FUNC, AST_CONST, AST_USING,
    AST_STRUCT_DEF, AST_ENUM_DEF,
    AST_STRUCT_FIELD, AST_ENUM_VARIANT, AST_TYPE_EXPR_NODE,
    AST_EXTERN_LIB,

    AST_BLOCK, AST_LET, AST_ASSIGN, AST_ASSIGN_OP, AST_IF, AST_WHILE,
    AST_FOR_RANGE, AST_FOR_IN,
    AST_BREAK, AST_CONTINUE, AST_RETURN, AST_EXPR_STMT,

    AST_INT, AST_FLOAT, AST_BOOL, AST_STRING, AST_CSTRING, AST_VAR,
    AST_BINARY, AST_UNARY, AST_CALL, AST_CALL_INDIRECT,
    AST_DEREF, AST_CAST, AST_TRY,
    AST_ARRAY_LIT, AST_INDEX,
    AST_STRUCT_LIT, AST_STRUCT_INIT, AST_FIELD,
    AST_VARIANT_CTOR,
    AST_ALLOC_ARENA, AST_ALLOC_RC, AST_ADDR_OF_FIELD, AST_SIZEOF,

    AST_MATCH, AST_MATCH_ARM,
    AST_PAT_WILDCARD, AST_PAT_BIND, AST_PAT_INT, AST_PAT_BOOL,
    AST_PAT_STRING, AST_PAT_VARIANT,
} AstKind;
```

Forty-eight kinds, grouped by layer: program-level declarations,
statements, expressions, patterns. The order within a group is
not semantically meaningful.

### §3.2 `AstList`

```c
typedef struct {
    AstNode** items;
    int       count;
    int       capacity;
} AstList;

void ast_list_init(AstList* l);
void ast_list_push(AstList* l, AstNode* n);
void ast_list_prepend_all(AstList* dst, AstList* src);
void ast_list_free(AstList* l);
```

The one container type in the AST. Every list of children — program
decls, block statements, call arguments, match arms, struct fields,
enum variants, array elements, struct initializers, pattern
subpatterns — is an `AstList`. There are no other container types.

**Growth policy.** `ast_list_push` doubles from an initial capacity
of 8. `ast_list_free` recursively frees every item via `ast_free`,
then frees the array and resets.

**Ownership.** An `AstList` owns its items. When a node is freed,
every list in its payload is freed with `ast_list_free`.

**`ast_list_prepend_all`** splices `src`'s items in front of
`dst`'s, preserving order, and clears `src->count`. Used by the
driver to prepend the prelude's declarations to the user program.
The two-loop shape (shift `dst` right, then copy `src` left) is the
classic "make a hole, then fill it" idiom; the shift runs backwards
so no element is overwritten.

### §3.3 Shared payload structs

```c
typedef struct {
    Token        name;
    TypeExpr*    type_expr;
    struct Type* type;
    bool         is_mut;
} Param;

typedef struct {
    Token local;
    Token source;
} UsingName;

typedef enum {
    USING_MODULE, USING_SELECTIVE, USING_ALIAS,
} UsingKind;

typedef struct {
    UsingKind kind;
    Token* path; int path_count;
    Token module_alias; bool has_module_alias;
    UsingName* names; int name_count; int name_capacity;
    Token alias_local; TypeExpr* alias_target;
    int resolved_file_id;
    int resolved_path_count;
} AstUsing;
```

**`Param`.** One function parameter. `type` is NULL until sema
resolves it; `type_expr` is NULL if the source had no annotation.
`is_mut` reflects the source's `mut` keyword on the parameter.

**`UsingName`.** One name inside a selective `using`. `source` is
the name as written in the target module; `local` is the name in
the importing file (equal to `source` when no alias).

**`AstUsing`.** The payload for `AST_USING`. Three variants share
one struct, distinguished by `kind`. Fields not applicable to the
current kind are zeroed by the constructors; a reader must consult
`kind` before touching any other field.

- `USING_MODULE` — `path`, `module_alias`, `has_module_alias`,
  plus the loader-set `resolved_*` fields.
- `USING_SELECTIVE` — `path`, `names`, plus the loader-set
  `resolved_*` fields.
- `USING_ALIAS` — `alias_local`, `alias_target`.

The `resolved_file_id` and `resolved_path_count` fields are set by
the module loader after resolving the path. Before the loader runs,
`resolved_file_id` is 0 and `resolved_path_count` is either
`path_count` (module form) or 0 (selective form). The distinction
exists so sema can tell whether the loader has run; see
`arch/files/20-module.md`.

### §3.4 `AstNode`

```c
struct AstNode {
    AstKind      kind;
    Span         span;
    struct Type* type;
    bool         is_pub;
    bool         is_builtin;

    int  local_index;
    int  const_id;
    bool holds_rc;
    Tier tier;

    union { ... } as;
};
```

**Common fields.**

- `kind` — the discriminator. Set at construction and never changed.
- `span` — the source location. May be `SPAN_NONE` for synthesized
  nodes.
- `type` — the resolved `Type*`, filled by sema. NULL until then.
- `is_pub` — meaningful for `AST_FUNC`, `AST_CONST`,
  `AST_STRUCT_DEF`, `AST_ENUM_DEF`, `AST_USING`. Set by the parser
  when the declaration was prefixed with `pub`.
- `is_builtin` — set on prelude declarations. Sema reads this to
  set `TypeDef.is_rc_managed`.
- `local_index` — set by sema on `AST_VAR`, `AST_LET`,
  `AST_PAT_BIND`, and assignment targets. `-1` means "not a local".
- `const_id` — set by sema on `AST_VAR` when the name resolves to a
  constant. `-1` means "not a constant". At most one of
  `local_index` and `const_id` is set after successful sema.
- `holds_rc` — set on `AST_LET` and `AST_VAR` when the value is
  `rc<T>` or `string`. Used by codegen to decide whether to emit
  RC drops.
- `tier` — set by sema on `AST_ALLOC_ARENA` (`TIER_ARENA`) and
  `AST_ALLOC_RC` (`TIER_RC`). **Written but never read.** The
  codegen distinguishes tiers by node kind, not by `tier`. A
  vestige of an earlier design.

### §3.5 The union, by kind group

**Top-level payloads.**

- `as.program` — `AstList decls`, `uint32_t file_id`, `char*
  canonical_path`. The loader sets the last two after parsing.
- `as.func` — name, method_owner, `type_params[8]`,
  `type_param_count`, `params[8]`, `param_count`, `return_type_expr`,
  `return_type`, `body`, `func_idx`, `is_extern`, `lib_alias`,
  `is_variadic`, `ffi_idx`.
- `as.const_decl` — name, initializer, optional `type_expr`.
- `as.using_decl` — the `AstUsing` payload.
- `as.struct_def` — name, `type_params[8]`, `type_param_count`,
  `fields` (an `AstList` of `AST_STRUCT_FIELD`), `sdef_index`,
  `is_extern`, `is_opaque`, `is_union`.
- `as.enum_def` — name, `type_params[8]`, `type_param_count`,
  `variants` (an `AstList` of `AST_ENUM_VARIANT`), `tdef_index`.
- `as.struct_field` — name, `type_expr`, `type`, `offset`.
- `as.enum_variant` — name, `payload_exprs`, `payloads`,
  `payload_count`, `tag`.
- `as.type_expr_node` — a single `TypeExpr*`. Wrapper used inside
  enum variant payloads and pattern subpatterns.
- `as.extern_lib` is not a separate union member; the payload
  reuses `as.using_decl` with `kind` left at 0 (which equals
  `USING_MODULE`). A reader should not interpret the `kind` field
  on an `AST_EXTERN_LIB` node.

**Statement payloads.**

- `as.block` — `AstList stmts`.
- `as.let` — name, `type_expr`, `type`, `init`, `is_mut`.
- `as.assign` — `target`, `value`.
- `as.assign_op` — `op`, `target`, `value`, `temp_slot`.
- `as.if_` — `cond`, `then_`, `else_`.
- `as.while_` — `cond`, `body`.
- `as.for_range` — `var`, `start`, `end`, `body`, `var_slot`,
  `end_slot`.
- `as.for_in` — `var`, `iterable`, `body`, `var_slot`, `arr_slot`,
  `len_slot`, `idx_slot`.
- `as.ret` — `value` (NULL for a bare `return`).
- `as.expr_stmt` — `expr`.

**Expression payloads.**

- `as.int_lit` — `I64 value`.
- `as.float_lit` — `double value`.
- `as.bool_lit` — `bool value`.
- `as.string_lit` — `Token content`. Used by both `AST_STRING` and
  `AST_CSTRING`; the token span covers the text between the quotes
  (for `AST_CSTRING`, `start + 2` skips the `c"` prefix during
  escape resolution).
- `as.var` — `Token name`, `int func_idx`. The `func_idx` is
  `-1` for a normal variable reference; sema sets it to a function
  index if the name resolves to a function used as a value.
- `as.binary` — `op`, `left`, `right`.
- `as.unary` — `op`, `operand`.
- `as.call` — name, `instantiation`, `args`, `builtin_id`,
  `func_idx`, `ffi_idx`. Exactly one of the three indices is set
  after sema.
- `as.call_indirect` — `callee`, `args`. Produced by sema when an
  `AST_CALL`'s name resolves to a local of `TY_FN` type.
- `as.deref` — `operand`, `offset`.
- `as.cast` — `operand`, `target_type_expr`, `target_type`.
- `as.try_` — `operand`.
- `as.array_lit` — `AstList elements`.
- `as.index` — `target`, `index`.
- `as.struct_lit` — `type_name`, `instantiation`, `inits`,
  `sdef_index`, `hidden_slot`, `expected_type`.
- `as.struct_init` — name, `value`, `offset`, `field_type`.
- `as.field` — `target`, `field`, `owner`, `offset`, `unit_tag`,
  `declared_type`. `unit_tag == -1` means a field access;
  `unit_tag >= 0` means a unit-variant construction.
- `as.variant_ctor` — `type_name`, `instantiation`,
  `variant_name`, `args`, `tdef_index`, `tag`, `mod_func_idx`,
  `mod_ffi_idx`, `receiver_expr`. Three shapes unify: enum variant
  construction (when `mod_*` are -1), module function call (when
  `mod_func_idx` or `mod_ffi_idx` is set), method call (when
  `receiver_expr` is non-NULL).
- `as.alloc_arena` — `value`, `is_mut`.
- `as.alloc_rc` — `value`.
- `as.addr_of_field` — `field` (an `AST_FIELD` node). Produced by
  sema when `&p.x` is applied to an extern struct field.
- `as.sizeof_expr` — `te`, `resolved`, `size`.

**Pattern payloads.**

- `as.match` — `scrutinee`, `arms`, `hidden_slot`.
- `as.match_arm` — `pattern`, `body`.
- `as.pat_bind` — name, `slot`.
- `as.pat_int` — `I64 value`.
- `as.pat_bool` — `bool value`.
- `as.pat_string` — `Token content`.
- `as.pat_variant` — `type_name`, `variant_name`, `subpatterns`,
  `tag`, `sub_count`, `tmp_slots[16]`, `instantiation`.

### §3.6 Lifecycle

```c
void ast_free(AstNode* n);
```

Recursively frees a node and everything it owns. NULL is a no-op.

Ownership rules, stated once:

- Every `AstNode*` field in a payload is owned by the containing
  node.
- Every `AstList` in a payload owns its items.
- Every `TypeExpr*` field in a payload is owned by the containing
  node.
- `Token` fields are borrowed by value; freeing a node does not
  touch the source buffer.

### §3.7 Constructors

Over forty constructors, one per kind (some kinds share a shape).
Each is named `ast_<kind>` (or a short name for multi-word kinds:
`ast_for_range`, `ast_alloc_arena`, `ast_pat_variant`).

The pattern is uniform:

- Allocate via `alloc_node(kind, span)`.
- Every field left at `xcalloc`'s 0 unless it needs a different
  "unset" sentinel.
- Set `local_index = -1`, `const_id = -1`, `tier = TIER_NONE`.
- Fill kind-specific payload fields.
- Return.

`ast_alloc_node` is the public wrapper for the private
`alloc_node`. Callers outside `ast.c` use it to obtain a bare
node and fill in the payload themselves. Used by `monomorph.c`
during AST deep-copy.

Two constructors have unusual copy semantics:

- `ast_using_module(path, n, alias, has_alias)` — copies the
  `path` array into fresh heap memory.
- `ast_using_selective(path, n)` — same path copy, but does not
  initialize the `names` array. The caller fills it via
  `ast_using_selective_add`.

---

## §4 — Implementation notes

### §4.1 The single allocation site

```c
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
```

Every AST node is created here. `xcalloc` zeroes the whole struct;
the four sentinel fields are written explicitly so a reader of the
constructor can see them. Every specific constructor calls
`alloc_node` internally.

The sentinel values matter:

- `local_index = -1` means "this node does not reference a local".
- `const_id = -1` means "this node does not reference a constant".
- `tier = TIER_NONE` is the default.

### §4.2 The single destruction site

`ast_free` is a switch over `n->kind`. For each kind, the case
frees exactly the fields the matching constructor (or sema) may
have allocated. Two rules for maintaining this function:

- Every union member that holds an `AstNode*`, `AstList`, or
  `TypeExpr*` must be freed for every kind that uses it.
- Leaf kinds (literals, `AST_VAR`, `AST_BREAK`, `AST_CONTINUE`,
  patterns without children) fall through with no case body.

The cases are grouped by which fields are owned:

- **Leaf kinds** — `AST_INT`, `AST_FLOAT`, `AST_BOOL`, `AST_STRING`,
  `AST_CSTRING`, `AST_VAR`, `AST_BREAK`, `AST_CONTINUE`,
  `AST_PAT_WILDCARD`, `AST_PAT_INT`, `AST_PAT_BOOL`,
  `AST_PAT_STRING`, `AST_PAT_BIND`.
- **Single-owner kinds** — `AST_LET` frees its `init` and
  `type_expr`; `AST_DEREF` frees its `operand`; and so on.
- **Multi-owner kinds** — `AST_FUNC` frees every parameter's
  `type_expr`, the return type's `type_expr`, and the body.
  `AST_BINARY` frees both operands. `AST_MATCH` frees the
  scrutinee and the arms list.

The `AST_ENUM_VARIANT` case is worth noting: it frees
`payload_exprs` (the syntactic list) but **deliberately does not**
free `payloads` (the resolved `Type*` array). The `payloads` array
is owned by the `TypeDef` that produced it; freeing it in
`ast_free` would leave the `TypeDef` pointing at freed memory.
This was a real bug at one point; see `handbook/changelog.md`.

### §4.3 Prepend-splice

`ast_list_prepend_all` is the only list operation with a
non-trivial algorithm:

```c
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
    src->count = 0;
}
```

The growth target is `new_count * 2` rather than the old capacity
doubled, because the required size is already known. The reverse
shift is what makes the operation correct when the ranges overlap
in the underlying array; a forward shift would overwrite elements
before moving them.

Used once, by `main.c`'s `compile_multi`, to prepend the prelude.

### §4.4 Path-array copying

Both `ast_using_module` and `ast_using_selective` copy the caller's
`path` array into fresh heap memory:

```c
u->path = (Token*)xmalloc(sizeof(Token) * (size_t)n);
memcpy(u->path, path, sizeof(Token) * (size_t)n);
```

This is important because the parser builds path arrays on the
stack and discards them. The AST must own a copy.

### §4.5 The `ast_using_selective_add` growth

`ast_using_selective_add` appends a `UsingName` to a node created
by `ast_using_selective`. It grows the `names` array by doubling
from 4. It is the only mutating function in the AST constructors
besides `ast_list_push`; every other constructor is fully
initializing.

---

## §5 — Invariants

- Every AST node is allocated via `alloc_node`, which `xcalloc`s.
  Every node therefore has zeroed memory for fields not explicitly
  initialized by its constructor.
- Every constructor returns a node whose kind-specific sentinel
  fields are set to `-1` or `NULL`. A caller that forgets to set
  a field reads a sentinel, not garbage.
- `ast_free` on a tree frees every node reachable from the root,
  exactly once. No node is shared between two parents.
- Every `AstList` has `items == NULL` iff `capacity == 0`. The
  empty-list state is uniform.
- The `kind` field is set exactly once, at construction, and never
  changed.
- The AST is acyclic. No constructor produces a back-reference.

---

## §6 — Known limitations

**Fixed caps.**

- 8 parameters per function (`as.func.params[8]`).
- 8 type parameters per declaration (`type_params[8]`).
- 16 subpatterns per variant pattern (`as.pat_variant.tmp_slots[16]`).

Exceeding any cap is a parser diagnostic (the extra items are
discarded after being reported).

**Union member discipline is not enforced.** Nothing prevents a
caller from reading `as.if_.cond` on an `AST_BINARY` node. The
dispatch-by-`switch` pattern makes this rare, but it is not a
compile-time guarantee.

**No source-level annotation or comment attachment.** The AST
carries no comments, docstrings, or attributes beyond `is_pub`.

**`Tier` is written but not read.** See §3.4. A reader who sees
`tier` set by sema and never consumed may suspect a missing case;
the correct interpretation is that the field is vestigial.

**`AST_STRUCT_INIT` name collision.** `AST_STRUCT_INIT` is a field
initializer inside a struct literal, not a struct definition. The
name does not match the pattern of `AST_STRUCT_DEF` /
`AST_STRUCT_FIELD`.

**`AST_EXTERN_LIB` reuses the `using_decl` payload.** The node's
`kind` field is left at 0 (which equals `USING_MODULE`); reading
it as a `UsingKind` is not meaningful.

**No parent pointers.** Passes that need upward navigation would
have to maintain their own parent stack.

**`as.pat_variant.tmp_slots[16]` is written by sema and read by
codegen.** Both stages must agree on the meaning of the array.
The cap is enforced by the parser's pattern-count check.

**No visitor or walker.** Every stage writes its own recursive
walk.

---

## §7 — Extension points

- **A visitor or walker framework.** The `ast_free` switch is the
  canonical list of "the children of each node kind". A visitor
  would benefit from sharing that knowledge.
- **An `ast_clone` for non-generic nodes.** `monomorph.c` already
  contains a deep-copy function (`mono_copy_node`) but it is
  specialised to substitution. A non-substituting clone would be
  useful if a future pass needed to copy a subtree without
  instantiating it.
- **A `type` field on non-expression nodes.** Some passes would
  benefit from having a `Type*` on blocks and statements. The
  current design keeps `type` only on expressions.
- **Removing the `Tier` field.** A cleanup, not a feature.
- **Dynamic parameter and type-parameter caps.** Replacing the
  fixed arrays with growable lists would remove the caps but add
  an allocation per function and per struct.
- **An `ast_dump` pretty-printer.** There is none today.
