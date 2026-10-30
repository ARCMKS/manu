## `docs/arch/overview.md`

# The Prism Compiler — Overview

*Current as of commit `<hash>`, <date>.*

This is the pipeline tour. It says what each stage of the compiler
consumes, what it produces, and where its state lives. It is the
document to read before `runtime.md` (which is about execution) or
any of the `arch/files/*.md` per-file references.

The goal of this document is to give you a mental model that
survives contact with the source. When you open `sema.c` and see
five files, or see `codegen_program` take a `SemaCtx*` it doesn't
seem to use, this document should let you answer "why" without
reading more code than necessary.

If you want a minimal reading order for the source, §7 is the
short version.

---

## §1 — The shape of the compiler

Prism is compiled ahead-of-time. The compiler is a single process
that reads one or more `.manu` files, produces a `Chunk` (the
bytecode plus its side tables), and hands it to a VM that executes
in the same process.

Three things about this shape matter:

1. **The pipeline is linear with a stage between each pair.** Every
   stage either succeeds or reports errors; if there are errors,
   the driver stops the pipeline before the next stage runs. No
   stage has to defend against malformed input from the previous
   one. This is why, for example, `codegen.c` never checks whether
   a type is NULL before emitting instructions that depend on it
   — sema guarantees non-NULL, and the pipeline guarantees sema
   ran.

2. **The compiler's state is owned by the driver, not the stages.**
   `main.c` allocates a `CompileUnit` (a struct bundling the
   `SourceMap`, `ConstTable`, `TypeDefTable`, `FFITable`,
   `SemaCtx`, and `Chunk`) and passes pieces of it to each stage.
   No stage allocates another stage's tables. This is what makes
   the pipeline linear: each stage reads what the previous wrote,
   writes its own output, and returns.

3. **The `Chunk` is the artifact.** Everything before `codegen`
   produces data structures internal to the compiler; everything
   after consumes the `Chunk`. The `Chunk` is self-contained:
   it holds the code, the debug tables, the constant pool, the
   function table, and the runtime type table. Nothing else needs
   to be passed to the VM.

The `SourceMap` is an exception in the sense that it's shared
across the whole pipeline: the lexer stamps spans with file IDs
from it, sema and codegen read identifier bytes from it via
`smap_span_text`, the diagnostic renderer decodes spans with it,
and the VM's error renderer reads it for source excerpts. This
is deliberate; the alternative would be for each stage to carry
its own copy of the source text, which would be wasteful and
would let different stages disagree about what a span means.

---

## §2 — The pipeline

```
                     .manu files (one or more)
                             │
                             ▼
        ┌────────────────────────────────────────────┐
        │  SourceMap.smap_add                        │
        │    reads each file, assigns a file_id ≥ 1  │
        └────────────────────────────────────────────┘
                             │
                             ▼
        ┌────────────────────────────────────────────┐
        │  Lexer.lexer_init + lexer_next             │
        │    produces Token{kind, Span}              │
        └────────────────────────────────────────────┘
                             │
                             ▼
        ┌────────────────────────────────────────────┐
        │  Parser.parse_program                      │
        │    produces one AST_PROGRAM per file       │
        └────────────────────────────────────────────┘
                             │
                             ▼
        ┌────────────────────────────────────────────┐
        │  modload_merge                             │
        │    concatenates all files' decls into one  │
        │    AST_PROGRAM (spans preserve file_id)    │
        └────────────────────────────────────────────┘
                             │
                             ▼
        ┌────────────────────────────────────────────┐
        │  prelude_parse + ast_list_prepend_all      │
        │    parses Result/Option and prepends them  │
        └────────────────────────────────────────────┘
                             │
                             ▼
        ┌────────────────────────────────────────────┐
        │  sema_analyze (5 passes)                   │
        │    resolves types, assigns slots, checks   │
        │    escape rules, instantiates generics     │
        └────────────────────────────────────────────┘
                             │
                             ▼
        ┌────────────────────────────────────────────┐
        │  codegen_program                           │
        │    emits bytecode into a Chunk             │
        └────────────────────────────────────────────┘
                             │
                             ▼
        ┌────────────────────────────────────────────┐
        │  validate_bytecode                         │
        │    structural check (no execution)         │
        └────────────────────────────────────────────┘
                             │
                             ▼
        ┌────────────────────────────────────────────┐
        │  vm_init + vm_run                          │
        │    execute; return 0 on HALT, 1 on error   │
        └────────────────────────────────────────────┘
```

Between each stage, the driver checks `diag_has_errors(&bag)` and
stops if any error was reported. This is the reason a stage can
assume its input is well-formed. It's also the reason a stage
does not need to produce valid output on error — nothing downstream
will run.

The `Chunk` does not exist until `codegen` runs. The `TypeDefTable`
and `FFITable` exist before `sema` (they're caller-allocated and
zero-initialized) but are only populated during sema. The
`ConstTable` is populated by sema, read by codegen, and never
touched by the VM (constants are inlined at their use sites; see
§2.6).

### §2.1 Loading

**Input:** file paths (from `-I` flags and the root file name).

**Output:** for each loaded file, a `ParsedFile` in the
`ModuleLoader`, and one new entry in the `SourceMap`.

**Where:** `module.c`'s `modload_load`, which recurses through
`using` declarations.

The loader reads each file into a temporary buffer, hands the
buffer to `smap_add` (which makes its own copy), and frees the
buffer. The `SourceMap` owns the content from then on. Every
`Span` produced by the lexer refers to a byte range in that copy.

Two things the loader does that are easy to miss:

- **Cycle detection.** A file currently being processed (on the
  `loading` stack) that's referenced again is a cycle and is
  rejected. A file that's fully loaded and referenced again is
  a diamond and is returned as-is.
- **Path resolution.** A dotted path like `std.io` is tried as
  `std.io.manu`, then `std/io.manu`, in each include directory in
  order. The longest prefix that resolves is the module; trailing
  components name symbols inside it (see `resolve_module_path`).

After this stage, every file has a `file_id` and every file's
content is in the `SourceMap`.

### §2.2 Lexing and parsing

**Input:** a `SourceMap`, a file_id.

**Output:** for each file, an `AST_PROGRAM`.

**Where:** `lexer.c` produces tokens; `parser.c` consumes them.

The lexer is zero-allocation: a `Token` is a 24-byte value
(`TokenKind kind; Span span;`) produced on the stack. There is no
token buffer, no token pool. The source buffer is the token store;
a token's "text" is its span.

The parser is recursive descent for statements and declarations,
with a Pratt-style precedence chain for expressions (nine levels,
from `or_expr` at the loosest to `primary` at the tightest). It
uses one token of lookahead with two exceptions:

- `peek_generic_call` copies the lexer state and scans up to 64
  tokens to disambiguate `<` between less-than and generic
  opener.
- `restrict_struct_lit` is a parser flag that suppresses struct
  literal parsing in positions where a `{` could also start a
  block. It's set around `match` scrutinees and `for` sources.

The parser never resolves names. A name is a `Token` (a span).
Deciding whether it's a local, a constant, a function, or a type
is sema's job. This is what lets the parser be tested on
syntactically valid but semantically meaningless programs.

### §2.3 Merging

**Input:** the `ModuleLoader`'s list of `ParsedFile`s.

**Output:** a single `AST_PROGRAM` whose `decls` list contains
every file's top-level declarations in load order.

**Where:** `module.c`'s `modload_merge`.

The merge is a pointer copy, not a deep copy. Each file's decls
list is emptied (its `count` set to 0) and its pointers are moved
into the merged list. The `Span` on each declaration still carries
its original `file_id`, so sema can tell which file a declaration
came from.

This is why the merge is a concatenation and not a rewrite: file
identity is preserved through the merge. If the merge had stripped
file IDs, sema would have to reconstruct per-file scoping from
some other signal.

### §2.4 The prelude

**Input:** the `SourceMap`.

**Output:** an `AST_PROGRAM` containing `Result<T, E>` and
`Option<T>`, prepended to the user program's decls.

**Where:** `prelude.c`'s `prelude_parse`.

The prelude is written in Prism. It's a constant string in
`prelude.c` (`PRELUDE_SOURCE`), parsed by the same lexer and
parser as user code. This is deliberate: the prelude exercises
the same front end, so any change to the front end is validated
against it on every compile.

The prelude's declarations are marked `is_builtin = true`, which
sema reads (in `register_struct_or_enum`) to set
`TypeDef.is_rc_managed`. This is what makes `Result` and `Option`
RC-managed throughout the compiler: their `TypeDef`s have the flag
set, and every place that constructs a value of those types
consults the flag to decide between `OP_NEW_ENUM` and
`OP_NEW_RC_ENUM`.

The prelude is registered in the `SourceMap` under the synthetic
path `<prelude>`. It has its own `file_id`, distinct from every
user file.

### §2.5 Sema

**Input:** the merged program, the `SourceMap`, the caller's
`ConstTable`, `TypeDefTable`, `FFITable`.

**Output:** the same program, with every expression node annotated
with its resolved `Type*`; every resolution field (`local_index`,
`const_id`, `func_idx`, `ffi_idx`, `builtin_id`, `tag`, `offset`,
`sdef_index`, `tdef_index`, `unit_tag`, `hidden_slot`, the
various loop and pattern slots) filled in; every constant
registered in the `ConstTable`; every type registered in the
`TypeDefTable`; every extern function registered in the `FFITable`;
every generic instantiation produced and analyzed.

**Where:** `sema.c`, `sema_modules.c`, `sema_ffi.c`, `sema_types.c`,
`sema_mono.c`, `resolve.c`.

Sema is split into five files. The split is by responsibility, not
by pass:

| File | Responsibility |
|---|---|
| `sema.c` | The orchestrator and the body analysis. |
| `sema_modules.c` | Module table construction (`using` declarations). |
| `sema_ffi.c` | FFI table construction, `Option<*T>` recognition, FFI type translation. |
| `sema_types.c` | Nominal type registration, C-ABI layout for extern structs. |
| `sema_mono.c` | Generic instantiation and the instantiation cache. |
| `resolve.c` | Centralized name resolution — `ResolvedDecl`, `resolve_name`, `resolve_qualified`, etc. |

The sema pipeline is five passes, run in this order:

**Pass 0** — register nominal type names. Every `type X = struct`
or `type X = enum` in the merged program gets a `TypeDef`, except
generic declarations, which become templates in the monomorphizer.

**Pass 0b** — resolve struct fields and enum variant payloads.
Runs after Pass 0 so a field can reference a type declared later
in the same file. Extern structs get their C-ABI layout computed
here, in a separate fixpoint loop (`resolve_extern_structs`) so
that forward references between extern structs work.

**Pass A** — function signatures and constants. Every function
gets a `FuncSig`; every `const` gets an entry in the `ConstTable`.
Functions with type parameters go to the template list instead
of getting a concrete `FuncSig`.

**Selective import validation** — after Pass A, because it
requires the function table to be populated. Checks that every
`using lib.{ x }` names a real symbol in `lib` and, for
functions, that the symbol is `pub`.

**Pass B** — function bodies. Each function gets a fresh
`FnLocal` (locals, scope stack, current signature). Every
expression is annotated with its type, every local gets a slot,
and the escape rules are enforced at return sites and allocation
sites.

The order matters. Pass A before Pass B is what allows a function
body to call a function declared later in the source: the callee's
signature is in the function table when the call site is analyzed.
Pass 0 before Pass 0b is what allows a struct field to reference
a type declared later in the same file.

The split between `SemaCtx` (per-compilation) and `FnLocal`
(per-function) is what makes nested body analysis reentrant. When
a generic function is instantiated from inside another function's
body, `sema_instantiate_function` creates a fresh `FnLocal` for
the copy and analyzes it; the outer function's `FnLocal` is
untouched. See `handbook/decisions.md` for the history.

### §2.6 Codegen

**Input:** the annotated program, the `SourceMap`, the `ConstTable`,
the `TypeDefTable`, the `FFITable`, the `SemaCtx`, and an empty
`Chunk`.

**Output:** the `Chunk`, populated with bytecode, debug tables,
constant pool, function table, and runtime type table.

**Where:** `codegen.c`.

`codegen_program` does four things:

1. **Assign codegen function indices.** A `codegen_of_sema[]`
   mapping is built by iterating `sc->funcs[]` and skipping
   externs and templates. Concrete user functions get sequential
   codegen indices starting at 0.
2. **Find `main`.** By name. If absent, report an error and bail.
3. **Emit the prologue.** `CALL main 0; HALT`.
4. **Emit each function's body.** For each function in
   `sc->funcs[]` that has a body (non-extern, non-template), the
   body is emitted via `cg_stmt`, then a fallback `PUSH 0; RET`.

The prologue is at offsets 0–3. Every function body follows.

The `codegen_of_sema[]` mapping exists because sema's function
table includes externs and generic instantiations, which don't
get emitted as standalone function bodies. A `CALL` operand is a
codegen index; `codegen_of_sema` converts a sema index to one.
This mapping is the `FuncSig.codegen_idx` invariant at the
codegen end (see `invariants.md`).

Constant inlining: a reference to a `const` is not a runtime
load. The `const`'s value is inlined into the instruction stream
(`PUSH <value>` for i64/bool, `LOAD_CONST <idx>` for f64/string).
This is why a `const` costs nothing at runtime, and why string
constants aren't supported yet (a `CONST_STRING` in a `const`
would need to be materialized as an arena block, which codegen
doesn't do for constants).

RC insertion: every `let` with an RC value registers itself in
the current RC scope; the drops are emitted at scope exit and on
every early exit path. See `runtime.md` §8.

### §2.7 Validation

**Input:** the `Chunk`.

**Output:** `0` on success, `-1` on the first structural error.

**Where:** `disasm.c`'s `validate_bytecode`.

The validator checks:

- Every opcode is a valid `OpCode`.
- Every instruction's operand words are inside the code array.
- Every jump target lands inside the code.
- Every `CALL` target is a valid function-table index.
- Every `LOAD_CONST` index is inside the pool.
- Every parameterized opcode's descriptor operand is a valid
  `TypeDesc` index.
- Every `BINARY`/`UNARY`/`MEM_*` sub-operand is in range.

It does not check stack balance, type consistency, or anything
that requires execution. It's a structural check, not a semantic
one. A codegen bug that produces bytecode with the wrong operands
but valid encoding will pass the validator and produce wrong
results at runtime.

### §2.8 Execution

**Input:** the `Chunk`, a `VMConfig` that references the chunk's
parts.

**Output:** `0` on normal halt, `1` on runtime error.

**Where:** `vm.c`'s `vm_run`.

The VM is described in detail in `runtime.md`. Two things here:

- **`vm_init` is not free.** It allocates the lexical arena
  (1 MiB) and materializes the constant pool's strings into the
  const arena. The materialization is O(pool size), so a large
  program has a bigger startup cost.
- **`vm_run` is a single loop.** Every opcode's implementation is
  a case in the loop's switch. Errors are reported via the
  `RT_ERR` macro, which formats a message, records the pc and
  function index, and returns 1 from `vm_run`.

After `vm_run` returns, the driver either prints the return value
(`vm_top`) or, on error, calls `render_runtime_error` which uses
the recorded `error_pc` and `error_func_idx` to print a source
excerpt and a call trace.

---

## §3 — The file map

```
include/                    public headers
  common.h                  type aliases, Span, allocators
  diagnostics.h             DiagnosticBag, Diagnostic
  source_map.h              SourceMap, SourceFile, SpanDecoded
  lexer.h                   Token, TokenKind, Lexer
  ast.h                     AstNode, AstKind, constructors
  types.h                   Type, TypeExpr, TypeDef, TypeDesc,
                            TypeDefTable, ModuleTable, TypeTable
  parser.h                  parse_program
  monomorph.h               GenericTemplate, Subst, mono_copy_*
  sema.h                    SemaCtx, FnLocal, sema_analyze
  sema_internal.h           cross-file declarations within sema/
  resolve.h                 ResolvedDecl, resolve_name, etc.
  constant.h                ConstantPool, ConstTable
  chunk.h                   Chunk, FuncEntry, FuncTable
  codegen.h                 codegen_program
  vm.h                      OpCode, VM, VMConfig
  disasm.h                  disassemble, validate_bytecode
  module.h                  ModuleLoader, ParsedFile
  prelude.h                 prelude_parse
  prism_ffi.h               FFILib, FFIFunc, FFITable
  test.h                    TestCase, TestGroup, assertion macros

src/                        implementation
  common.c                  xmalloc / xcalloc / xrealloc
  diagnostic.c              DiagnosticBag implementation + renderer
  source_map.c              SourceMap implementation + decoder
  lexer.c                   scanner
  ast.c                     constructors + destructor
  types.c                   type constructors, predicates,
                            type_expr_resolve, typetable_intern
  parser.c                  recursive descent + Pratt
  monomorph.c               template storage + AST deep copy
  sema.c                    sema orchestrator + body analysis
  sema_modules.c            module table construction
  sema_ffi.c                FFI table construction
  sema_types.c              nominal type registration, C ABI
  sema_mono.c               generic instantiation + cache
  resolve.c                 name resolution
  constant.c                ConstantPool + ConstTable
  chunk.c                   Chunk implementation
  codegen.c                 AST → bytecode
  vm.c                      dispatch loop + natives + FFI
  disasm.c                  listing + validator
  module.c                  multi-file loader
  prelude.c                 Result / Option declarations
  main.c                    CLI driver

tests/                      test suite
  test.c                    framework implementation, helpers
  test_main.c               entry point
  t_*.c                     test groups (10 files)
```

Each file has a corresponding `arch/files/NN-name.md`. See
`docs/README.md` for the index.

---

## §4 — The three representations of a type

The same type appears in three forms as it flows through the
compiler. Understanding the boundaries between them is the single
biggest thing a new reader has to get used to.

### §4.1 `TypeExpr` — what the user wrote

```c
typedef struct TypeExpr {
    TypeExprKind kind;
    Span         span;
    Token        name;
    bool         is_mut;
    struct TypeExpr* inner;
    Token* path;
    int    path_count;
    struct TypeExpr** args;
    int               arg_count;
    int               arg_capacity;
    Type* resolved;
    struct TypeExpr** params;
    int               param_count;
    int               param_capacity;
    struct TypeExpr*  ret;
} TypeExpr;
```

A syntactic form. `[i64]`, `rc<P>`, `Option<i64>`, `foo.Bar`,
`*u8`, `fn(i64, i64) : i64` — all `TypeExpr`s. The parser produces
them; `type_expr_resolve` in `types.c` consumes them.

Why it's separate from `Type`: the parser sees `Option<i64>` as a
name followed by a generic argument list. It doesn't know whether
`Option` is a type, a function, or a variable — that's resolution's
job. And sema sometimes needs the syntactic form for diagnostics:
"the type arguments resolve to `foo#i64`, not a struct" needs the
original `foo` name to be spelled in the message.

`TypeExpr` is a tree. `type_expr_free` recursively frees it. The
`resolved` field is a cache: `type_expr_resolve` can be called
more than once on the same tree, and the resolved pointer is
reused.

### §4.2 `Type` — what sema resolved

```c
struct Type {
    TypeKind kind;
    int      size;
    Type*    inner;
    bool     is_mut;
    TypeDef* tdef;
    Type**   fn_params;
    int      fn_param_count;
    Type*    fn_ret;
};
```

A recursive, structured view. `rc<Option<i64>>` is a tree of
`Type` nodes: outer is `TY_RC`, its `inner` is `TY_ENUM`, whose
`tdef` points at the `Option#i64` `TypeDef`.

Primitives (`i64`, `bool`, `string`, `void`, `f32`, `f64`, the
integer types) are singletons in `types.c`. `ty_i64()` returns
the same pointer every time. Pointer equality is a valid
comparison for primitives.

Compound types are allocated fresh by each call to `ty_ref`,
`ty_rc`, `ty_rawptr`, `ty_array`, `ty_struct`, `ty_enum`,
`ty_extern_struct`, `ty_fn`. Two calls with the same arguments
produce two distinct pointers. `type_eq` does structural
comparison for these.

`Type*` is never freed. Every compound `Type` is leaked at
process exit. For a compiler that runs one compilation per
process this is bounded and acceptable; a REPL or language server
would need to change this.

### §4.3 `TypeDesc` — what the VM sees

```c
typedef struct {
    TypeDescKind kind;
    uint32_t     aux;
    uint32_t     size;
    uint32_t     count;
} TypeDesc;
```

A flat, 16-byte, index-addressable view. `typetable_intern` in
`types.c` converts a `Type*` into a descriptor index; the index
is what parameterized opcodes carry as operands.

The descriptor carries only what the VM needs to execute a
parameterized opcode: the kind for dispatch, the size for
allocations, and `aux`/`count` for walking compound structures.

Why three forms instead of one:

- **The parser needs the syntactic form.** A `TypeExpr` preserves
  the qualified path, the generic argument list, the mutability
  flag. Those are semantic data the parser can't discard.
- **The compiler needs the recursive form.** Computing "does this
  type contain a `ref`" is a recursive walk. Diagnosing "expected
  `rc<Option<i64>>`, got `Option<i64>`" needs the recursive shape
  to render either side correctly.
- **The VM needs the flat form.** A bytecode operand is a 32-bit
  index. The VM has no way to walk a tree of `Type` nodes at
  runtime; it only has the descriptor array.

The three are bridged by two functions: `type_expr_resolve` (from
`TypeExpr` to `Type`) and `typetable_intern` (from `Type` to
`TypeDesc`). Everything else flows from these.

---

## §5 — The Rosetta Stone

Prism's codebase uses prefixes and short names throughout. Every
one of them is listed here. When you see one in code, come back
to this table.

### Type and constant prefixes

| Prefix | Meaning | Example |
|---|---|---|
| `I8`/`I16`/`I32`/`I64` | signed integer of that width | `I64 v;` |
| `U8`/`U16`/`U32`/`U64` | unsigned integer of that width | `U64 bits;` |
| `F32`/`F64` | IEEE float / double | `F64 x;` |
| `ty_` | constructor / accessor for a `Type*` | `ty_i64()`, `ty_ref(t)` |
| `TY_` | `TypeKind` enum value | `TY_REF`, `TY_EXTERN_STRUCT` |
| `TDEF_` | `TypeDefKind` enum value | `TDEF_STRUCT`, `TDEF_BUILTIN` |
| `TEX_` | `TypeExprKind` enum value | `TEX_NAME`, `TEX_FN` |
| `TD_` | `TypeDescKind` enum value | `TD_I64`, `TD_ENUM` |
| `T_` | static singleton (only in `types.c`) | `T_I64`, `T_STRING` |

Rule of thumb: `TY_` is what the compiler sees (`Type`), `TD_` is
what the VM sees (`TypeDesc`), `TEX_` is what the parser produces
before either.

### AST prefixes

| Prefix | Meaning | Example |
|---|---|---|
| `AST_` | `AstKind` enum value | `AST_FUNC`, `AST_TRY` |
| `BIN_` | `BinOp` enum value | `BIN_ADD`, `BIN_AND` |
| `UN_` | `UnOp` enum value | `UN_NEG`, `UN_NOT` |
| `ast_` | constructor for an AST node | `ast_int(...)`, `ast_try(...)` |
| `Tier` / `TIER_` | memory tier (written by sema, not read) | `TIER_ARENA`, `TIER_RC` |

### VM prefixes

| Prefix | Meaning | Example |
|---|---|---|
| `OP_` | opcode | `OP_PUSH`, `OP_FFI_CALL` |
| `OPB_` | binary sub-op | `OPB_ADD`, `OPB_AND` |
| `OPU_` | unary sub-op | `OPU_NEG`, `OPU_NOT` |
| `MEM_` | memory access width | `MEM_I32`, `MEM_PTR` |
| `NATIVE_` | native function slot | `NATIVE_STDOUT_WRITE` |
| `FFI_` | C type kind for marshalling | `FFI_I64`, `FFI_PTR` |
| `CONST_` | constant-pool entry kind | `CONST_I64`, `CONST_CSTRING` |
| `SINK_` | output destination kind | `SINK_STDOUT`, `SINK_STRING` |
| `vm_` | VM-side helper | `vm_init`, `vm_run` |
| `cg_` | codegen helper | `cg_expr`, `cg_stmt` |
| `RT_` | runtime error macro | `RT_ERR(...)` |

### Struct field abbreviations

| Field | Long form | Meaning |
|---|---|---|
| `fp` | frame pointer | base of the current function's arguments in `data_stack` |
| `sp` | stack pointer | next free slot in `data_stack` |
| `ap` | arena pointer | current bump cursor in `arena` |
| `amp` | arena mark (previous) | arena's `ap` at the enclosing scope's entry |
| `pc` | program counter | word index into `bytecode` |
| `cap` | capacity | allocated size of an array |
| `idx` | index | position in an array |
| `rd` | read length | return value of `fread` |
| `tok` | token | a `Token` value |
| `t` | token or type | context-dependent |
| `n` | count | number of elements |
| `b` | byte or bool | context-dependent |

### Function-name prefixes

| Prefix | Owner | Meaning |
|---|---|---|
| `smap_` | `source_map.c` | `SourceMap` operations |
| `lexer_` | `lexer.c` | lexer operations |
| `ast_` | `ast.c` | AST operations |
| `type_` / `ty_` | `types.c` | type operations / constructors |
| `typedef_` | `types.c` | `TypeDefTable` operations |
| `consts_` | `constant.c` | `ConstTable` (compile-time) |
| `pool_` | `constant.c` | `ConstantPool` (runtime) |
| `mono_` | `monomorph.c` | monomorphizer |
| `sema_` | `sema*.c` | semantic analysis |
| `codegen_` / `cg_` | `codegen.c` | bytecode emission |
| `vm_` | `vm.c` | VM lifecycle and execution |
| `disasm_` | `disasm.c` | disassembly |
| `modload_` | `module.c` | module loader |
| `diag_` | `diagnostic.c` | diagnostics |
| `prelude_` | `prelude.c` | compiler prelude |

### Growth policy

Every dynamic array in the compiler uses the same growth: **double,
starting from a small initial value.** You will see
`capacity ? capacity * 2 : N` in dozens of functions. The `xrealloc`
family (`xmalloc`, `xcalloc`, `xrealloc`) are the only allocation
primitives used; they abort on OOM. Every table in the compiler is
therefore a `{ items, count, capacity }` triple grown this way.

---

## §6 — Between the stages: what's checked

The pipeline is linear, but it isn't silent. Here's what
`main.c` checks between each pair of stages:

```
modload_load         → diag_has_errors? abort : continue
modload_merge        → (no check; the merge always succeeds)
prelude_parse        → (no check; the prelude is a constant)
prepend prelude      → (no check)
sema_analyze         → diag_has_errors? abort : continue
codegen_program      → diag_has_errors? abort : continue
validate_bytecode    → rc != 0? abort with exit 3 : continue
vm_init + vm_run     → rc != 0? report runtime error, exit 2 : exit 0
```

`modload_load` and `sema` and `codegen` can produce multiple
diagnostics before returning. The driver does not stop at the
first error; it renders the full bag with `diag_flush`. This is
why `sema` uses a `DiagnosticBag` rather than returning an error
code from each `analyze_*` function.

`cmd_check` runs the same pipeline but stops after codegen. It
doesn't validate or execute.

---

## §7 — Where to start reading

If you want to understand the compiler and have limited time,
read in this order:

1. **`include/vm.h`** — the opcode list, the `VM` struct, the
   `VMConfig`. Skim the docblock; the opcode enum is what you
   need.
2. **`src/vm.c`'s `vm_run`** — the dispatch loop. Read it one
   opcode at a time. This is the target the compiler produces
   code for; knowing it makes everything else easier.
3. **`src/codegen.c`'s `cg_expr` and `cg_stmt`** — how each AST
   node maps to opcodes. Compare against `vm_run`.
4. **`src/sema.c`'s `analyze_expr` and `analyze_stmt`** — how
   types and slots are resolved. This is the biggest file; read
   one case at a time.
5. **`src/lexer.c`** and **`src/parser.c`** — the front end.
   Small compared to the back end; the lexer is a single function
   dispatch loop and the parser is nine precedence levels plus a
   statement dispatcher.

Skip the plumbing (`common.c`, `source_map.c`, `diagnostic.c`,
`chunk.c`, `constant.c`) on the first pass. They do exactly what
their names suggest.

If you're going to modify the compiler, read `invariants.md`
after this document. It's the enforcement map that tells you
what assumptions each stage makes about its input.

If you're going to modify the VM, read `runtime.md`. It's the
companion to this document for the execution end.
