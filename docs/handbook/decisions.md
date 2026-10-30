## `docs/handbook/decisions.md`

# Design Decisions

*Current as of commit `<hash>`, <date>.*

This document records why the compiler and the language are the
way they are. For each decision: context, the choice made,
alternatives considered, the reasoning, and what would change the
choice.

A reader who wants to understand *what* the code does should read
`arch/`. This document is for the question "why is it this way and
not some other way."

The entries are grouped by subsystem. Within each group they are
roughly chronological.

---

## §1 — Memory model

### D1. Two memory tiers, not one

**Context.** Prism needs to manage values whose lifetime is
bounded by the enclosing scope (locals, transient arrays) and
values that outlive their scope (returned aggregates, shared
data).

**Decision.** Two tiers with distinct source syntax: `&e` produces
`ref<T>` and allocates in a lexical arena; `rc e` produces `rc<T>`
and allocates on a reference-counted heap.

**Alternatives considered.** A single garbage-collected heap. A
single manual `malloc`/`free`. A single region-based system.

**Rationale.** The two workloads have different costs. Scope-bound
allocation is O(1) to allocate and O(1) to free (one pointer
reset at scope exit), and needs no per-value bookkeeping. RC is
more expensive per operation but handles sharing and escape.
Making the choice explicit in the source syntax forces the
programmer to think about the lifetime, which is what the escape
rules then enforce.

**Cost.** Two syntaxes for "allocate a value." The programmer must
know which to use. The `rc` literal restriction (§D7) exists
because the general case is harder than it looks.

**Revision condition.** If a tracing GC were ever added, the
arena would become a nursery and the RC heap would become the
mature generation. The two-tier split makes that transition
mechanical; a single heap would not.

### D2. Three escape rules, not a marking pass

**Context.** A `ref<T>` pointing at an arena value is only valid
until the enclosing scope exits. A `ref<T>` returned from a
function would dangle.

**Decision.** Forbid the situations that would produce a dangling
reference: no nested references, no `ref<T>` return type, no `*T`
return type, no direct return of `[T]`, `struct`, or `enum`. All
four rules are checked at type-resolution time and at return
sites.

**Alternatives considered.** A marking pass that walks the AST
and computes which values escape. A flow-sensitive analysis. A
borrow checker with lifetimes.

**Rationale.** The three rules close every path by which an arena
pointer could outlive its scope. A marking pass would be more
permissive (some refs could be returned safely) but would require
a fixpoint and would be much harder to explain in a diagnostic.
The rules are conservative: they reject some programs that would
be safe. In practice, the safe programs are ones where the value
should be on the RC heap anyway, so the rules push the programmer
toward the correct design.

**Cost.** The programmer cannot return a `ref<T>` even when the
referent is on the RC heap and would outlive the function. The
workaround is to return the RC value itself.

**Revision condition.** A future borrow checker (à la Rust) would
subsume these rules and could be more permissive. The three rules
are a special case of "the reference doesn't outlive its
referent", simplified to be decidable in one pass.

### D3. `*void` unifies with any pointer

**Context.** The FFI needs a universal pointer type. C's `void*`
is the closest analog.

**Decision.** `type_eq(*void, *T)` is true for any `T`. The
unification is asymmetric to the other pointer type comparisons;
it's the only place where two structurally different types are
considered equal.

**Alternatives considered.** A distinct `opaque*` type with an
explicit cast to any other pointer. A `*Any` supertype.

**Rationale.** The FFI hands back `*void` from functions like
`malloc`. Requiring an explicit cast at every use would be noisy
for no safety gain; the FFI already requires the programmer to
know the actual type. The unification keeps `*void as *T` a
no-op.

**Cost.** A `*void` can be silently passed where a `*T` is
expected, even if the programmer meant something else. This is
the same loss of safety C has with `void*`.

**Revision condition.** A future generic `Any` type would allow
the cast to be explicit without the special case.

### D4. Compound `Type*` values are never freed

**Context.** Every `ty_ref(t)`, `ty_rc(t)`, etc. allocates a
~48-byte `Type` struct. A compilation creates hundreds or
thousands.

**Decision.** Let them leak. Primitive types are static
singletons; compound types are heap allocations that are never
released.

**Alternatives considered.** Reference counting on `Type*`.
A per-compilation arena for types. Interning, with a hashtable
for deduplication.

**Rationale.** The compiler runs one compilation per process.
At process exit, all memory is reclaimed by the OS. The leak is
bounded by the number of distinct `Type*` the compiler creates,
which for a realistic program is a few hundred kilobytes.

**Cost.** A REPL or language server that runs many compilations
in one process would accumulate. A future long-running process
would need to change this.

**Revision condition.** Adding a `Type*` arena to the
per-compilation `CompileUnit` and freeing it at `cu_free` would
close the leak without an ownership model. Not done because the
current lifetime is fine.

---

## §2 — Type system

### D5. Three representations of a type

**Context.** A type appears as: the syntactic form the parser
produces (`[i64]`), the resolved structured form (`Type*`), and
the runtime descriptor (`TypeDesc`).

**Decision.** Three distinct structures, with two bridge
functions: `type_expr_resolve` (syntactic → structured) and
`typetable_intern` (structured → descriptor index).

**Alternatives considered.** One representation used everywhere.
Two (syntactic and structured). A canonical single-form system.

**Rationale.** Each representation serves a different consumer.

- The parser needs the syntactic form: the qualified path, the
  generic argument list, the mutability flag. Those are semantic
  data that the parser cannot discard.
- The compiler needs the recursive form: "does this type contain
  a `ref`" is a recursive walk; the diagnostic "expected
  `rc<Option<i64>>`, got `Option<i64>`" needs the recursive shape
  to render either side.
- The VM needs the flat form: a bytecode operand is a 32-bit
  index. The VM has no way to walk a tree of `Type*` at runtime.

A single representation would compromise at least one of these.
The three-way split pays a small translation cost in exchange for
each layer having the representation it wants.

**Cost.** A reader must know which representation they're looking
at. The `TEX_`, `TY_`, and `TD_` prefixes are the guide.

**Revision condition.** Nothing likely. The three
representations are what the three consumers need.

### D6. `is_rc_managed` lives on `TypeDef`, not `Type`

**Context.** `Result` and `Option` are declared in the prelude and
must be RC-managed at every use site. The flag that says "this
type is RC-managed" could live on the `Type` or the `TypeDef`.

**Decision.** On the `TypeDef`. `Type` values with `kind ==
TY_ENUM` carry a `tdef` pointer; the RC-wrapping is applied by
every use site that resolves the type, producing a `ty_rc(...)`
that wraps the `ty_enum(...)`.

**Alternatives considered.** A `Type`-level flag, checked by
`type_eq` and propagated through compound constructions. A
`TY_RC_ENUM` kind (distinct from `TY_ENUM`). A sema-only flag,
not visible at the type level.

**Rationale.** `is_rc_managed` is a nominal property: `Result` is
RC-managed regardless of where it appears. A `Type`-level flag
would need to be propagated through every `ty_rc`/`ty_ref`/etc.
construction, and the propagation rules would be non-obvious (is
`ref<Result<i64>>` RC-managed? it points at an RC value, but
isn't itself one).

A `TY_RC_ENUM` kind would conflate "the type is a variant-
discriminated aggregate" with "the type's values are RC-managed".
Every pattern-matching site would need to check both kinds.

Putting the flag on the `TypeDef` and applying the RC wrapping at
every use site (in `type_expr_resolve` and the three sema
resolution sites that inspect the resolved type) matches the
nominal nature of the flag.

**Cost.** Three code paths need to unwrap one level of `TY_RC`
to see the underlying enum: `analyze_pattern`, `analyze_expr`'s
`AST_VARIANT_CTOR`, and `analyze_expr`'s `AST_FIELD` for unit
variants. A fourth such site would need to remember the unwrap.
This is the "three-use-site unwrapping problem" documented in
`arch/invariants.md` §10.2.

**Revision condition.** If `type_expr_resolve` returned
`ty_rc(T)` transparently (i.e., every `TY_ENUM` with
`is_rc_managed` appeared as a `TY_RC` wrapping a `TY_ENUM`, and
every use site that inspects the type knew to unwrap), the
three-site pattern would disappear. This is a larger change to
the type system; not yet implemented.

### D7. `rc` requires a literal form for compound types

**Context.** `rc e` allocates an RC block. For scalars, the block
holds a single 8-byte value. For compounds (struct, array,
enum), the block would hold the compound's payload.

**Decision.** For a compound type, `rc e` requires `e` to be a
literal form: an array literal, a struct literal, a variant
constructor, or a unit variant. `rc variable` is rejected.

**Alternatives considered.** Always copy the compound value into
a fresh RC block. Never allow `rc` of a compound (require the
programmer to use the constructor form). Allow `rc variable` only
when the variable is a temporary.

**Rationale.** Copying a compound value into an RC block requires
knowing the value's runtime layout. That's known for literals
(the codegen built the layout itself) but for a variable, the
value could have come from anywhere. Codegen would need a
generic "clone an aggregate into an RC block" opcode, which does
not exist.

The literal-form restriction avoids the copy: the array/struct/
enum constructor builds directly on the RC heap with the correct
layout. No copy needed.

**Cost.** `let a = [1, 2, 3]; let b = rc a;` is rejected. The
programmer writes `let b = rc [1, 2, 3];` instead, or shares `a`
via `&a`.

**Revision condition.** An `OP_CLONE_RC` opcode that took a
source block and a descriptor and copied the payload into a fresh
RC block would remove the restriction. The opcode is
straightforward; the reason not to add it is that no realistic
program needs `rc variable`.

### D8. Nominal equality for structs and enums

**Context.** `type_eq(a, b)` for two struct types compares `tdef`
pointers; for two compound types (ref, rc, array), it recurses
structurally.

**Decision.** Nominal equality for structs and enums.

**Alternatives considered.** Structural equality (compare field
names and types). Both (a `structural_eq` predicate alongside
`type_eq`).

**Rationale.** A struct with fields `{x: i64, y: i64}` and a
struct with fields `{y: i64, x: i64}` have the same fields in
different orders. Structurally they might be equal; but they are
different types with different constructors. Nominal equality
matches the language's declaration-based identity: a type is what
it's named, not what it contains.

**Cost.** Two structurally identical but nominally distinct
structs cannot be used interchangeably. This is the correct
behavior for a language with named types.

**Revision condition.** A future "structural refinement" feature
(a `Satisfies<T>` bound) would want structural comparison as a
predicate. Not currently planned.

### D9. No interning of compound `Type*`

**Context.** `ty_ref(ty_i64(), false)` called twice produces two
distinct `Type*`. `type_eq` compares them structurally.

**Decision.** Do not intern. Every constructor allocates fresh.

**Alternatives considered.** A global hashtable keyed on
`(kind, inner, is_mut, tdef)`. A per-compilation intern table.

**Rationale.** Interning would make identity comparison on
compound types valid, which several call sites would benefit
from (`InstCache`'s argument comparison, the escape-rule checks).
But interning requires an ownership model for the interned
`Type*` (they'd need to be freed when the table is), and the
call sites that benefit already use `type_eq`, which is correct
without interning.

The performance cost of `type_eq` on compound types is a
recursive comparison. For the depth of types in realistic
programs (2–4 levels), this is cheaper than a hashtable lookup.

**Cost.** `Type*` identity comparison on compound types is
meaningless. Every caller must use `type_eq`. The convention is
documented but not enforced.

**Revision condition.** If a profiling run shows `type_eq` as a
hotspot (unlikely for the current scale), interning becomes
worthwhile. Adding it would require a per-compilation intern
table and freeing it at shutdown.

### D10. `TEX_RESOLVED` for monomorphization

**Context.** When the monomorphizer substitutes a concrete type
into a copied AST, the substitution replaces a `TEX_NAME` with
... what? The concrete type is a `Type*`, not a syntactic form.

**Decision.** A `TEX_RESOLVED` node that carries the `Type*`
directly. `resolve_inner` returns it unchanged.

**Alternatives considered.** Construct a syntactic form for each
concrete type (build `[i64]` as `TEX_ARRAY(TEX_NAME("i64"))`).
Thread a separate "already resolved" side table.

**Rationale.** Constructing a syntactic form is wrong when the
concrete type is a compound type from an instantiation: there is
no source-level name for `Option#i64`. The `TEX_RESOLVED` node
sidesteps this by carrying the type directly.

A side table would require every `type_expr_resolve` call to
consult it, which would be slower and would need a lifecycle
parallel to the AST.

**Cost.** `TypeExpr` grows an extra field (`resolved`) and an
extra kind. The invariant "every `TypeExpr` eventually resolves
to a `Type*`" is now trivially true for `TEX_RESOLVED`.

**Revision condition.** Nothing likely; the mechanism is used by
every instantiation and works.

---

## §3 — Runtime

### D11. Three stacks, not one

**Context.** The VM has a data stack (values), a call stack
(frames), and a scope stack (arena marks + sp).

**Decision.** Three separate fixed-size arrays.

**Alternatives considered.** One stack with tagged entries. Push
arena marks onto the data stack.

**Rationale.** A single combined stack would need to know, for
each entry, whether it's a value, a call frame, or a scope frame.
That's a tagged representation with a runtime dispatch on the tag
at every push/pop.

The old design (before the split) pushed arena marks onto the
data stack. This corrupted locals whenever a scope contained both
values and allocations: an `EXIT_SCOPE` would interpret the
top-of-stack as an arena mark and reset the arena to a value that
was actually a local. The split is the fix.

Separate stacks make each push/pop a plain assignment with no
tag check.

**Cost.** `OP_RET` must update three things (data stack, call
frame, arena mark). This is acceptable: RET happens once per
call, not once per instruction.

**Revision condition.** Nothing likely. The separation is
load-bearing.

### D12. Parameterized opcodes, not type-specific opcodes

**Context.** Arithmetic operations need to work on signed
integers, unsigned integers, floats, and strings. Each has
different semantics.

**Decision.** `OP_BINARY sub-op desc` — the opcode name
identifies the operation category (ADD, SUB, etc.) and the
descriptor operand identifies the type. The VM dispatches on the
descriptor kind.

**Alternatives considered.** One opcode per (operation, type):
`OP_ADD_I64`, `OP_ADD_F64`, `OP_ADD_STRING`, and so on. Roughly
40 opcodes for the current type set.

**Rationale.** The per-type design means adding a new numeric
type (say `f128`) requires adding ~10 opcodes for the arithmetic
operators alone, plus new opcodes for comparisons, print, convert,
and every other parameterized operation. The parameterized design
means adding a new type is a new `TypeDescKind` and a new case in
each opcode's dispatch — no new opcodes.

The tradeoff is a two-level dispatch: the opcode switch finds
`OP_BINARY`, which then switches on the descriptor kind. For an
indirect-threaded interpreter, this is invisible.

**Cost.** A reader of the bytecode listing sees `BINARY ADD i64`
instead of `ADD_I64`. This is slightly less readable but
substantially more regular.

**Revision condition.** Nothing likely. The parameterization is
the design's main source of extensibility.

### D13. The calling convention: `fp = sp - argc`

**Context.** The caller pushes arguments left-to-right, then the
CALL instruction. The callee needs to address its arguments and
locals.

**Decision.** Set `fp` to `sp - argc` at CALL time. The callee's
slot 0 is the first argument, slot 1 is the second, and locals
start at slot `argc`.

**Alternatives considered.** Push arguments right-to-left (the C
convention). Pass a count and let the callee compute offsets.
Pass arguments in registers.

**Rationale.** Left-to-right is what the parser sees and what
codegen emits naturally. `fp = sp - argc` makes the first
argument slot 0, which is the simplest possible addressing.

The C convention (right-to-left) exists to make variadic
functions work: the first argument is always at a known offset
from the top. Prism's variadic FFI is handled at the FFI layer,
not at the VM level, so the VM does not need this.

**Cost.** A variadic function written in Prism would need the
compiler to emit code that reads arguments from a computed
offset. Prism has no variadic user functions, so this is not an
issue.

**Revision condition.** If user-defined variadic functions were
ever added, the calling convention would need rethinking.

### D14. `OP_RET` does three things

**Context.** A function's return needs to pop the return value,
release the callee's resources, and restore the caller's state.

**Decision.** `OP_RET` does all three: pop the value, reset
`ap = amp`, truncate `sp = fp`, push the value, restore the
caller's registers.

**Alternatives considered.** Split into `OP_RET` (pop and restore)
plus `OP_RELEASE_ARENA` and `OP_TRUNCATE_STACK` emitted before
the RET.

**Rationale.** The three actions are always performed together.
Splitting them would make codegen emit three instructions where
one suffices, and would require every return site to remember all
three. Combining them makes "return" a single atomic concept.

**Cost.** A callee that wanted to keep an arena allocation past
its return would need to copy it to the RC heap first. That's the
correct behavior anyway; the arena is scope-bound.

**Revision condition.** Nothing likely.

### D15. RC drops are emitted by codegen, not the VM

**Context.** A local of type `rc<T>` owns a reference. When the
local goes out of scope, the reference must be dropped. The
question is: who emits the drop?

**Decision.** Codegen. Sema marks each `let` with `holds_rc` and
codegen emits `OP_RC_DEC` on every path that exits the scope.

**Alternatives considered.** A `VariableIsRc` side table on the
`Chunk` that `OP_EXIT_SCOPE` consulted. A shadow stack of RC
locals maintained by the VM. A finalizer pass that dropped
everything at function return.

**Rationale.** The compiler knows statically which locals hold
RC references. Emitting the drops explicitly makes the bytecode
self-documenting (a reader can see where drops happen) and
avoids the runtime cost of a side table lookup.

A shadow stack would require the VM to track which slots hold RC
values at every STORE, which would be a runtime cost for no
benefit.

A finalizer pass would need to know the type of every slot at
function exit, which the VM does not have.

**Cost.** Codegen must emit the drops correctly on every exit
path. The RC-scope stack (§D16) is the mechanism. A new statement
kind that can exit a scope must remember to drop; this is the
"invariant by convention" documented as I42.

**Revision condition.** A bytecode format with type-tagged
locals would let the VM do it. Not on the roadmap.

### D16. RC-scope stack parallel to `scope_depth`

**Context.** Codegen needs to track which locals in which scope
hold RC references, so that scope exit and early exits can emit
the right drops.

**Decision.** An `RcScope rc_scopes[MAX_SCOPES]` array parallel
to the VM's `scope_depth`. Each `cg_push_rc_scope` pairs with a
compiler-side `enter_scope`; each `cg_emit_scope_drops` pairs
with an `exit_scope`.

**Alternatives considered.** A single flat list of RC locals with
a scope depth per entry. Recomputing the drops by walking the
AST at each exit point. Attaching the RC-local list to each
`AST_BLOCK` node.

**Rationale.** The scope stack is already maintained by
`cg->scope_depth`; the RC-scope stack follows it. The parallel
invariant (`cg->rc_scope_count == cg->scope_depth`) is easy to
verify and gives a single place to look when a drop is missing.

A flat list would need a per-entry scope depth and a search to
find the entries for a given scope.

Recomputing from the AST would require a walk at every exit site,
which is more code and more bug surface.

Attaching to `AST_BLOCK` wouldn't cover non-block scopes
(`for..range`, `for..in`, `match`).

**Cost.** Codegen must maintain the parallel stack. Every
`ENTER_SCOPE` emission needs a `cg_push_rc_scope`; every
`EXIT_SCOPE` needs a `cg_emit_scope_drops` and a pop. Forgetting
one produces a silent leak or a double-free. The invariant is
checked only by review; see I37.

**Revision condition.** Bundling "emit ENTER_SCOPE and push
RcScope" into a single helper function would make the pairing
structural. Not done; a small refactor.

---

## §4 — Language design

### D17. `let mut` instead of `var`

**Context.** A binding either can be reassigned or cannot.

**Decision.** Default immutable. `let mut x = 5;` for mutable.

**Alternatives considered.** `var x = 5;` for mutable, `let x =
5;` for immutable (à la Swift). `let x = 5;` mutable by default
(à la C).

**Rationale.** Immutable-by-default catches a large class of
bugs (accidental reassignment) and matches the language's overall
"explicit about what's mutable" philosophy (also seen in
`ref<mut T>` and the distinction between `ref<T>` and
`ref<mut T>`).

`var` for mutable and `let` for immutable reads well in Swift but
makes `let` rare, which inverts the common case: most bindings
are not reassigned.

**Cost.** A programmer coming from C writes `let x = 5; x = 10;`
and gets an error. The error message names the fix (`declare with
'let mut'`).

**Revision condition.** None likely.

### D18. `?` requires an annotated return type

**Context.** The `?` operator desugars to "if this Result is an
Err, return it from the enclosing function."

**Decision.** The enclosing function must have an annotated
return type that resolves to `Result<_, _>` or `Option<_>`.

**Alternatives considered.** Infer the return type from the first
`?` in the body. Require a per-function annotation only when `?`
is used. Allow `?` in any function whose inferred return type
matches.

**Rationale.** The outer type of a `?` cannot be inferred from
the operand alone. Given `let x = inner()?;`, the operand's type
says the failure type; the outer function's return type says how
to propagate it. Without the annotation, there's no way to know
whether the function returns `Result<i64, i64>` or
`Result<i64, string>`.

An inference-based scheme would need to know the return type
before analyzing the body, which is the same as requiring the
annotation.

**Cost.** A function using `?` must annotate its return type.
This is a small syntactic burden for a large clarity benefit.

**Revision condition.** Full HM inference would remove the
requirement but is not implemented and would be a much larger
change.

### D19. Match exhaustiveness at the top level only

**Context.** A `match` with a wildcard arm always compiles. A
match without one is exhaustive iff the arms cover every variant.

**Decision.** Check exhaustiveness at the top level. Do not check
subpatterns. Emit a defensive `OP_TRAP` after the last arm.

**Alternatives considered.** Full recursive exhaustiveness (would
need to check that every combination of subpatterns covers every
possible value). No check at all (require a wildcard).

**Rationale.** Full recursive exhaustiveness is
exponential in the depth of nested patterns; it's undecidable in
general for integer patterns. Top-level checking catches the
common mistakes (forgetting a variant) without the complexity.
The defensive trap handles the case where all top-level tests
pass but no subpattern matches: it produces a clean runtime error
naming the problem.

**Cost.** A match can be accepted at compile time and trap at
runtime. The diagnostic is `match: no arm matched`, which is
clear but not helpful (it doesn't say which value was unmatched).

**Revision condition.** A more thorough analysis could
track which values are covered for integer and string patterns,
warning when a non-wildcard match is incomplete. Not currently
implemented.

### D20. No traits

**Context.** Prism supports structs, enums, methods (via
`fn Type.method`), and generics. It does not support traits (à la
Rust) or interfaces.

**Decision.** No traits. Generic code is monomorphized and
type-checked per instantiation; there is no way to write a
generic function that requires a type parameter to have certain
methods.

**Alternatives considered.** A trait system with dynamic or
static dispatch. A "concept"-style constraint system.
Duck-typed generic constraints.

**Rationale.** Traits are a substantial addition: new syntax, new
type-system machinery, new dispatch codegen, new diagnostics. The
current design (no traits) is sufficient for the language's
current users (the prelude and the tests). Adding traits would
be a project of its own.

The workaround is: a generic function can call methods that are
defined on the specific types it's instantiated with, and a
missing method is a compile error at the instantiation site.

**Cost.** A programmer cannot write `fn max<T>(a: T, b: T) : T
where T: Ord`; they would need to write per-type versions or
inline the comparison. The stdlib (unbuilt) would need traits
or would need to duplicate code.

**Revision condition.** When the stdlib is built, traits (or
something equivalent) become necessary. The specific design
is open.

### D21. Every file's types share one global namespace

**Context.** Two files can each declare a type named `Point`.

**Decision.** Rejected. `typedef_table_find` searches all files
and reports a duplicate.

**Alternatives considered.** Per-file type namespaces with
explicit imports. Module-scoped types with `pub` visibility.
Global uniqueness only for public types.

**Rationale.** Per-file namespaces would require every type
lookup to know the current file, which the `TypeDefTable` API
does not currently support. The simpler design (global
uniqueness) is enough for small programs and forces the
programmer to think about naming.

**Cost.** A large program with many modules would need to
namespace its types manually (e.g., `Lib1_Point`,
`Lib2_Point`). This is a real limitation for the stdlib.

**Revision condition.** Adding a `file_id` field to `TypeDef`
and threading the current file through every lookup would close
this. It's a substantial change to the type system's API.

### D22. `main` is mandatory

**Context.** A program needs an entry point.

**Decision.** `fn main` must be declared. Missing it is a compile
error (`no 'main' function declared`).

**Alternatives considered.** Default to no execution (a library
program). Take the entry point as a command-line argument.
Support multiple entry points.

**Rationale.** Prism is a program language, not a library
language. Every compilation produces something to run. Requiring
`main` makes the intent explicit.

**Cost.** A file that's meant to be a library still needs a
`main` if compiled on its own. In practice, files are compiled
via the module loader with the driver file providing `main`.

**Revision condition.** A `--no-main` flag for library
compilation would let standalone library files compile. Not
currently implemented.

---

## §5 — Compiler architecture

### D23. `SemaCtx` / `FnLocal` split

**Context.** Sema's state includes both per-compilation data
(function table, type table, module table) and per-function data
(locals, scopes, current signature). Nested body analysis (a
generic instantiated from inside another body) needs the
per-function state to be isolated.

**Decision.** Two structs. `SemaCtx` holds per-compilation state
and is shared by reference. `FnLocal` holds per-function state
and is created fresh inside `analyze_func_body`, discarded on
return.

**Alternatives considered.** A single `FnCtx` with save/restore
around nested calls. A stack of per-function states.

**Rationale.** The save/restore approach was the earlier design
and produced subtle bugs: a nested call would clobber the outer
function's `current_sig` and slots. The split makes the isolation
structural: there is no shared field to clobber.

**Cost.** Two pointers passed to every analysis function. Minor.

**Revision condition.** Nothing likely. The split is the fix and
it works.

### D24. The AST is mutated in place

**Context.** Sema annotates every expression node with its type,
every name with a resolution, every local with a slot.

**Decision.** The annotations are written back into the parser's
`AstNode`s. There is no separate typed AST.

**Alternatives considered.** A separate typed AST produced by
sema. A side table from `AstNode*` to resolved data.

**Rationale.** A separate typed AST would double the memory and
require a duplicate set of node kinds. A side table would need
to be looked up at every access, which would be slower than
reading a field.

Mutating in place is simple: codegen reads the same nodes sema
wrote to.

**Cost.** The AST's invariant "every node is either fully
annotated or fully unannotated" is by convention. A partial
sema (an error partway through) leaves a partially annotated
AST; downstream passes are gated on `diag_has_errors`, so this
is fine in practice.

**Revision condition.** Nothing likely.

### D25. Five passes in sema

**Context.** Sema's work is not a single walk; it has
dependencies between declarations.

**Decision.** Five passes in fixed order: module table (before
Pass 0), nominal type registration (Pass 0), fields/variants
(Pass 0b), signatures/constants (Pass A), selective import
validation, bodies (Pass B).

**Alternatives considered.** A single pass with deferred work.
An on-demand pass that resolves names lazily.

**Rationale.** Each pass has a precondition that the previous
pass establishes.

- Pass 0 registers type names; Pass 0b's field resolutions need
  them.
- Pass 0b resolves fields; Pass A's signatures can reference
  struct types.
- Pass A registers functions; Pass B's call sites need their
  signatures.

A single pass would need to handle forward references explicitly,
which would be more code and more complex.

**Cost.** Five walks over the program. For a realistic program,
the walks are cheap. The alternative (one walk with a worklist)
would be harder to reason about.

**Revision condition.** If the number of passes ever grew past
~8, a worklist-based analysis would be worth considering. Not on
the roadmap.

### D26. Codegen walks `sc->funcs[]`, not `program->decls`

**Context.** Codegen needs to emit a body per function. The
functions are top-level declarations in `program->decls`, but
some are templates (no body to emit) and some are externs.

**Decision.** Codegen iterates `sc->funcs[]` (sema's function
table). The `program` argument to `codegen_program` is unused.

**Alternatives considered.** Iterate `program->decls`, filtering
by kind. Have sema produce a separate list of codegen-eligible
functions.

**Rationale.** Sema's function table already excludes externs
and templates (`find_func` skips them). Reusing it avoids a
second filter that must stay in sync with the first.

**Cost.** `codegen_program`'s signature includes `program` for
ABI stability; it's `(void)program;` in the body. Minor.

**Revision condition.** If the two filters ever diverged (say,
codegen started emitting something sema's table doesn't list),
the reuse would become a bug. The `codegen_of_sema[]` mapping is
the bridge; it's documented.

---

## §6 — Design decisions not made

These are open questions the project has deferred. They are not
"planned features"; they are choices that will need to be made if
the corresponding feature is ever built.

**Ownership model for RC.** Currently implicit: the value
returned from a function is owned by the caller; parameters are
borrowed. There is no explicit `move` or `borrow` syntax. A
future ownership model might add one.

**Traits.** See D20. Not designed.

**Error handling in the type system.** The current `?` operator
is `Result`/`Option`-specific. A general `Try` trait (à la
Rust) is not designed.

**Effect system.** No support for effects (async, IO, etc.).
Not designed.

**Const generics.** Type parameters must be types; a
`Vec<i64, 10>` form (with a compile-time constant as a parameter)
is not designed.

**Modules as first-class values.** Modules exist as import
sources; a module value (à la OCaml functors) is not designed.

**FFI callbacks.** Prism functions cannot be passed to C as
function pointers. Designed in `docs/ffi.md`'s "deliberately not
supported" section; the design (a `FFI_FNPTR` type plus a way to
marshal a codegen index into a C function pointer) is sketched
but not built.

**Reflection.** No runtime type information beyond what the
`TypeTable` provides for print and enum name rendering.
Additional reflection is not designed.
