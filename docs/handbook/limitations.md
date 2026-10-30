## `docs/handbook/limitations.md`

# Known Limitations

*Current as of commit `<hash>`, <date>.*

This document catalogs every limitation the project is aware of:
correctness bugs (currently none that we know of), fragile areas
where invariants are by convention, resource leaks, fixed caps,
untested paths, platform notes, and features that are deliberately
not supported.

A limitation is not a TODO. Some of these will be fixed; some
never will be. The document exists so a reader knows what to
expect, and so a future change can check whether it opens or
closes a limitation.

For design *reasoning* behind a limitation, see
`handbook/decisions.md`. For the fixes applied in the recent
work stream, see `handbook/changelog.md`.

---

## §1 — Correctness bugs

**None known at the current commit.**

The recent work stream fixed five:

- `AST_VARIANT_CTOR`'s enum-payload path used the wrong `ResolvedDecl`
  variable and reported `unknown type` for every non-generic enum
  payload constructor.
- `OP_DUP2` read the same slot twice, producing garbage for
  compound assignment through an index.
- `AST_ASSIGN_OP` for `AST_FIELD` evaluated its RHS twice and
  dropped the wrong reference.
- `AST_LET` called `coerce_to_target` twice, producing a doubled
  cast.
- `AST_FIELD` for a unit variant emitted `OP_NEW_ENUM` even for
  RC-managed enums, corrupting the heap on scope exit.

Details in `handbook/changelog.md`. All have regression tests
in the current suite.

**What "correctness bug" means here.** A bug that produces wrong
results on valid input, or a crash, or a heap corruption. Not:
a wrong diagnostic message, a suboptimal error message, a
performance issue, or a feature gap. Those are in later sections.

---

## §2 — Fragile areas (invariants by convention)

These are invariants the code relies on but does not enforce
structurally. A change that violates one would produce wrong
behavior silently; the test suite is the only defense.

See `arch/invariants.md` for the full enforcement map. The
fragile ones are summarized here.

### §2.1 One value per expression

The single most load-bearing invariant in codegen. Every
`cg_expr` call must leave exactly one value on the data stack.
No instruction checks this.

**Where it could break.** A new `AstKind` whose `cg_expr` case
emits a branch that doesn't push on one path. A new opcode whose
case in `vm_run` pushes two values or zero.

**Defense.** The test suite exercises most expression kinds. A
new kind needs its own tests.

### §2.2 RC-scope stack parallelism

`cg->rc_scope_count == cg->scope_depth` must hold at every
statement boundary. Every `OP_ENTER_SCOPE`-emitting construct
must call `cg_push_rc_scope`; every `OP_EXIT_SCOPE`-emitting
construct must call `cg_emit_scope_drops` and decrement.

**Where it could break.** A new statement kind that enters or
exits a scope without the corresponding calls. The `AST_FOR_IN`
case was missing its pushes at one point; the bug was silent until
a targeted test caught it.

**Defense.** The `for_in_body_rc` test in `t_memory.c`. A new
scope-introducing statement should have an analogous test.

**Structural fix.** Bundle the "emit ENTER_SCOPE + push RcScope"
and "emit EXIT_SCOPE + pop RcScope" pairs into single helper
functions. Not done.

### §2.3 RC ownership and the "fresh value" rule

A function returning `rc<T>` must return a fresh allocation. A
borrowed value (a local, a field read, an array element, a
dereferenced pointer) cannot be returned. Sema's `AST_RETURN`
checks this via a syntactic pattern.

**Where it could break.** A new expression kind that produces an
RC value which is neither clearly fresh nor clearly borrowed. The
current classifier has a fixed list; a new kind would fall
through to "owned" by default.

**Defense.** The `t_rc.c` / `t_memory.c` rejection tests. A new
kind that can produce RC values should be added to
`expr_produces_fresh_rc` and tested.

### §2.4 Codegen/sema index agreement

`FuncSig.codegen_idx` bridges sema's function table (which
includes externs and templates) and codegen's (which excludes
them). Every `CALL` operand must be a codegen index.

**Where it could break.** A new AST node that references a
function and stores the sema index directly. The
`codegen_of_sema[]` mapping must be applied.

**Defense.** The mapping is documented in `arch/files/17-codegen.md`
§11. The test suite exercises calls, method calls, and module
calls.

### §2.5 `is_rc_managed` implies `ty_rc` wrapping

Every use site of an RC-managed enum's type must see the type
wrapped in `ty_rc(...)`. Three code paths unwrap one level to
inspect the inner enum.

**Where it could break.** A fourth code path that inspects a
resolved type without unwrapping. The bug would be: a
pattern-matching or field-access site sees a `TY_RC` where it
expected a `TY_ENUM`, and reports "cannot match on this type" or
similar.

**Defense.** No systematic check. The three known sites are
documented in `arch/files/11-sema.md` §5.4.

### §2.6 Discriminated unions by kind

`AstNode.kind` selects which union member is live. Nothing
prevents reading the wrong member.

**Where it could break.** A new pass that dispatches on `kind`
and forgets a case (falls into the `default` and reads the wrong
member). Or a bug in a `switch` that reads `as.if_.cond` on a
binary node.

**Defense.** The dispatch-by-switch pattern makes it rare. The
`-Wswitch` compiler warning catches missing cases in exhaustive
switches.

### §2.7 `Span` well-formedness

Every span has `end >= start` and `end <= file->length`. Enforced
by construction in the lexer; not checked at decode time.

**Where it could break.** A synthesized span built by hand (the
parser builds some spans from token starts and ends). A bug in
the assembly would produce a malformed span.

**Defense.** The `smap_decode` clamping logic tolerates
out-of-range starts; `span_len` would underflow on `end < start`.

---

## §3 — Resource leaks

The compiler leaks memory in several places, all deliberately.
Each is bounded by the compilation's lifetime; none is a
per-iteration leak.

### §3.1 Compound `Type*`

Every `ty_ref`, `ty_rc`, `ty_rawptr`, `ty_array`, `ty_struct`,
`ty_enum`, `ty_extern_struct`, `ty_fn` allocates a `Type` struct
that is never freed. Roughly 48 bytes per call.

**Scale.** A realistic compilation creates hundreds to a few
thousand `Type*`. Peak leak: a few hundred kilobytes. Bounded by
the compilation's lifetime.

**Rationale.** See `decisions.md` §D4.

### §3.2 Instantiated function ASTs

`sema_instantiate_function_by_template` deep-copies the
template's AST and does not free it. The copy is stored as
`FuncSig.decl` and the body is analyzed against it; codegen
needs the annotations.

**Scale.** One AST per distinct function instantiation. For a
program with a few instantiations, a few kilobytes each.

**Rationale.** Deliberate; documented in
`arch/files/15-sema-mono.md` §3.3.

### §3.3 `ModuleTable` inner arrays

`sema_analyze` frees `sc->modtable.files` (the outer array) but
not each `FileNamespace`'s `imports[]` and `selective[]` arrays.
The code has a comment acknowledging this ("repeatedly
interacted badly with heap state").

**Scale.** Bounded by the number of files with imports times the
number of imports per file. Small.

**Rationale.** Unknown. The comment suggests an earlier attempt
to free them produced a bug. `INFERRED`: the free was probably
double-freeing or freeing from the wrong owner.

### §3.4 `TypeDefTable` in tests

`test_run` deliberately leaks the `TypeDefTable` per call. The
comment in `tests/test.c` acknowledges this.

**Scale.** Each `test_run` call leaks one `TypeDefTable`, sized
by the number of types in the test program. For hundreds of
tests, a few hundred kilobytes.

**Rationale.** Deliberate; the framework assumes short-lived
processes.

### §3.5 `TypeDefTable`'s `generics` handle

`mono_init` allocates a `TemplateList` and installs it on the
`TypeDefTable`. `typedef_table_free` does not free it (its
signature doesn't know whether the table owns the handle).

**Scale.** One list per compilation.

**Rationale.** The handle's lifetime is caller-owned by
convention; the caller is expected to call `mono_free`. In
practice no caller does, so it leaks.

---

## §4 — Fixed caps

Every cap produces a diagnostic when exceeded unless noted.

**Lexical / parser:**

- Parameters per function: 8.
- Type parameters per declaration: 8.
- Path components per `using` or type path: 16.
- Generic lookahead in `peek_generic_call`: 64 tokens.

**Types:**

- Enum variants: 128 (`MAX_ENUM_VARIANTS`).
- Variant payloads: 16 (`MAX_VARIANT_PAYLOADS`).
- Pattern subpatterns per variant pattern: 16
  (`AST_PAT_VARIANT.tmp_slots[16]`).

**Sema:**

- Locals per function: 256 (`MAX_LOCALS`).
- Scopes per function: 256 (`MAX_SCOPES`).
- Functions per compilation: 64 (`MAX_FUNCS`).
- Instantiation cache entries: 256 (`MAX_INST_CACHE`). Cache
  overflow is *silent* (see §5).

**Codegen:**

- Nested loops per function: 32 (`MAX_LOOP_DEPTH`).
- `break`/`continue` per loop: 64 (`MAX_BREAKS_PER_LOOP`).
- Format-string literal chunk buffer: 1024 bytes.

**VM:**

- Data stack: 4096 words.
- Call stack: 256 frames.
- Scope stack: 256 frames.
- Lexical arena: 1 MiB (`ARENA_MAX`).
- Const arena: 64 KiB (`CONST_ARENA_MAX`).
- Native slots: 32 (`MAX_NATIVES`); 21 registered.
- Sink stack: 64 (`SINK_STACK_MAX`).
- Sink inline buffer: 128 bytes (`SINK_INLINE_CAP`).

**FFI:**

- Libraries per compilation: 16.
- Extern functions per compilation: 128.
- Fixed args per function: 8.
- Total args per call: 16.

**Other:**

- `ConstTable` entries: 256 (`MAX_CONSTS`).
- CLI `-I` paths: 16.
- CLI program arguments: 64.
- Source files: unbounded, but each capped at 4 GiB by `Span`.
- Command-line length: bounded by the OS.

---

## §5 — Silent failures

These are cases where the code does not diagnose a limit; it
silently truncates, drops, or degrades.

- **Instantiation cache overflow.** When `g_cache.count ==
  MAX_INST_CACHE`, `cache_insert` returns NULL and the
  instantiation still runs but is not cached. A second call
  with the same arguments re-instantiates, producing a
  duplicate `TypeDef` with the same mangled name. Nominal
  comparison via `tdef` pointer would treat them as unequal.
  Bounded by the number of distinct instantiations.

- **`mono_mangle` truncation.** A mangled name longer than the
  output buffer is silently truncated. Two distinct
  instantiations could theoretically collide. The mangled name
  is display-only; identity is `TypeDef*`, so the collision is
  invisible.

- **`type_name` ring wrap.** More than 8 nested `type_name`
  calls in one expression cycle the static ring, overwriting
  earlier results. The compiler emits no warning.

- **Format string chunk truncation.** A format-string literal
  chunk longer than 1023 bytes is truncated without a
  diagnostic. Not observed in practice.

- **`ConstTable.entries[]` past `count`.** `consts_init` does not
  clear the entries array. A reader accessing `entries[i]` for
  `i >= count` reads stale data. The API is `count`-bounded.

- **`TypeDef.name_buf[128]` truncation.** A type name longer
  than 127 characters is truncated by `smap_span_text`. Not
  observed.

- **`EnumVariantDef.name_buf[64]` truncation.** Same.

- **`smap_add`'s `uint32_t` cast.** A file longer than 4 GiB
  would wrap its `length`. No check.

- **`-I` path overflow.** A 17th `-I` flag produces a
  diagnostic but is silently dropped; the other 16 are kept.

- **Program-argument overflow.** More than 64 arguments after
  the file path are silently dropped.

---

## §6 — Untested paths

Code paths that exist but are not exercised by the test suite.
Each is a place where a bug could hide.

- **FFI variadic on Windows.** The `t_ffi.c` test for variadic
  `snprintf` is guarded with `#ifndef _WIN32`. On Windows, the
  variadic FFI path is untested.

- **`USING_ALIAS` (`using Name = Type`).** Parses and is stored
  in the AST, but nothing consumes it. The path is dead code.

- **`AST_CALL_INDIRECT`.** Produced by sema when an `AST_CALL`'s
  name resolves to a local of `TY_FN` type. No test declares a
  `fn`-typed local or calls through it.

- **`AST_ADDR_OF_FIELD`.** Produced by sema for `&p.x` on an
  extern struct field. No test exercises extern structs with
  field addresses.

- **`AST_SIZEOF`.** No test uses `sizeof`.

- **Extern unions.** `extern type X = union { ... }` is parsed
  and laid out (fields at offset 0, size = max). No test
  exercises it.

- **Opaque extern types.** `extern type X = opaque` is
  recognized by the parser and sema. No test uses one.

- **`Result<*T, E>` from extern.** Sema rejects it (only
  `Option<*T>` is special-cased). No test verifies the
  rejection message.

- **The `Tier` field on `AstNode`.** Written by sema on
  allocation nodes; never read. The field exists and is set
  but has no observable effect.

- **`Parser.panic`.** Set by `expect`; read only in
  `function_declaration`'s recovery path. The broader use is
  vestigial.

- **Error-token paths in the lexer.** Unterminated strings
  produce `TOK_ERROR`; the parser filters them. A test would
  need a source with an unterminated string.

- **`NATIVE_READ_LINE` / `NATIVE_READ_ALL`.** Both read from
  `vm->in`; no test exercises them.

- **`NATIVE_ARGS`.** Reads `vm->argv`; no test exercises it.

- **`NATIVE_FFI_ALLOC` / `NATIVE_FFI_FREE`.** No test exercises
  them.

- **`AST_TRY` on the failure path with RC locals in scope.**
  The success path is tested; the failure path with drops is
  not.

- **`AST_FOR_RANGE`'s `break`/`continue` with RC locals in the
  loop scope.** The `for..range` loop's own rc-scope is pushed
  and popped; a break out of it emits drops. No test exercises
  this.

- **`?` in a function whose return type is `Option<_>` with a
  `Result<_>` operand.** Cross-type propagation is not
  supported; no test verifies the diagnostic.

- **Match with more than 128 variants.** Rejected at
  registration; no test.

- **`super` or relative imports.** Not supported.

---

## §7 — Platform notes

**Build system.** The `Makefile` uses Windows `cmd.exe` syntax
in `clean` (`@cmd /c "if exist manu.exe del /Q manu.exe"`). On a
POSIX shell, `make clean` fails.

**`test-%` target.** Uses `$*` in a recipe; works under GNU make
regardless of shell (the variable is expanded by make before the
shell sees it). The `FILTER` variable is the modern way.

**MinGW vs. MSVC.** The project builds under MinGW (gcc) with
libffi. An MSVC build is untested.

**Windows FFI symbol resolution.** `GetProcAddress` on
`ucrtbase.dll` fails for some symbols depending on the MinGW
distribution. A fallback tries `_<symbol>` for 32-bit cdecl
decoration. Variadic functions (`printf`, `sprintf`) may not be
exported at all; see `handbook/changelog.md`.

**Windows subprocess IO.** The test harness uses `tmpfile()`.
On Windows, `tmpfile` returns a file in the root directory or
`%TEMP%`, depending on the CRT. No test relies on the location.

**Endianness.** The VM stores `U64` values in the host's native
byte order. A program that manipulates bytes via `raw_load_u8`
would see host-endian ordering. The compiler and VM have not
been tested on a big-endian platform.

**Pointer width.** The VM assumes 64-bit pointers (`sizeof(void*)
== 8`). A 32-bit build would fail at compile time on the
`U64`-to-pointer casts.

---

## §8 — Deliberately not supported

Features that are not planned. Each is a design choice, not an
omission.

**Language features:**

- Traits or interfaces. See `decisions.md` §D20.
- Full Hindley-Milner inference. The current inference is
  bidirectional and partial.
- `&&` and `||` at the AST level (lexed, not parsed).
- String interpolation in literals other than `print`/`format`.
- Multi-line strings.
- Escaped quotes inside strings.
- Unicode identifiers.
- Octal, binary-exponent numeric literals.
- `switch`/`case`. `match` covers the use case.
- Exceptions and unwinding.
- `goto`.
- Global mutable state.
- Operator overloading.

**Runtime features:**

- Threads.
- Async/await.
- `dlclose` for FFI libraries.
- Signal handling.
- A garbage collector.

**FFI features:**

- Struct-by-value marshalling.
- Function pointer callbacks.
- Unions by value at the boundary.
- Variadic float arguments.
- `Result<*T, E>` returns.
- Calling convention attributes (`stdcall`, `fastcall`).
- Non-C ABIs.

**Tooling features:**

- A REPL.
- A debugger.
- A profiler.
- A package manager.
- A language server.
- Incremental compilation.
- Parallel compilation.
- Cross-compilation.
- Optimization passes (peephole, constant folding, DCE).

**Standard library:**

- There is no standard library. The prelude defines `Result` and
  `Option` and nothing else. The FFI primitives exist; a stdlib
  built on them is the natural next layer.

---

## §9 — Documentation gaps

Honest list of what the current doc set does not cover.

- **Diagrams.** The runtime model and the calling convention
  have no visual diagrams. Prose and worked traces substitute.
- **Per-opcode examples.** `arch/files/18-vm.md` describes each
  opcode's behavior but does not show a bytecode sequence that
  uses it.
- **A tutorial for the FFI.** `language/ffi.md` is written; the
  tutorial is not.
- **The diagnostics catalog.** `language/errors.md` is planned
  but not written.
- **A "how to add a native" recipe.** In `arch/recipes.md` when
  that lands.
- **Design rationale for `Result`/`Option`'s specific tags.**
  Documented as a fact (Ok is 0, Err is 1) but not as a
  decision. The decision is minor: it comes from the prelude's
  variant order.
- **The history of the older docs.** `docs/_old/` contains the
  pre-refresh doc set. Its relationship to the current set is
  not documented.

---

## §10 — What "fixing" a limitation would require

A short table mapping the most reachable limitations to their
fix.

| Limitation | Fix | Size |
|---|---|---|
| Instantiation cache overflow | Growable cache or hash table | ~100 lines in `sema_mono.c` |
| `ModuleTable` inner array leak | Free each namespace's arrays | ~20 lines in `sema_analyze` |
| Compound `Type*` leak | Per-compilation `Type*` arena | ~50 lines across `types.c` and `cmd_run` |
| `USING_ALIAS` no-op | Consult alias declarations in `resolve_name` | ~80 lines across `types.c` and `sema.c` |
| Global type namespace | `file_id` on `TypeDef` plus lookup threading | ~200 lines |
| `is_rc_managed` three-site unwrap | Transparent wrapping at resolution | ~100 lines |
| RC-scope pairing (structural) | Bundle `ENTER_SCOPE`+push and `EXIT_SCOPE`+pop | ~50 lines in `codegen.c` |
| Fix `unmangle_name` for nested generics | Recursive renderer | ~30 lines in `types.c` |
| Selective import of constants | `file_id` on `ConstEntry` | ~50 lines |
| `cmd_check` and `cmd_run` drift | Already fixed (both use `compile_multi`) | — |

The sizes are rough; each would be a single session's work.
None is on the critical path for the language's current uses.
