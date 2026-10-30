## `docs/handbook/roadmap.md`

# Roadmap

*Current as of commit `<hash>`, <date>.*

This document lists what is planned for the project, in rough
order. A roadmap is a prediction, not a commitment. Priorities
shift; new work appears; some of what is here will never be done.

The order below is the order I would take the work in if I were
maintaining this project. Each entry is a concrete step with a
rough size estimate.

For a list of what is *not* planned, see `handbook/limitations.md`
§8. For the design reasoning behind the current state, see
`handbook/decisions.md`.

---

## §1 — Near-term (single-session work)

These are small, self-contained, and worth doing soon. None
changes the language; all improve the compiler's robustness or
developer experience.

### 1.1 Fix the `ModuleTable` inner-array leak

`sema_analyze` frees `sc->modtable.files` but not each
`FileNamespace`'s `imports[]` and `selective[]` arrays. The code
has a comment acknowledging this ("repeatedly interacted badly
with heap state"). Investigate the earlier failure, then either
free the arrays or document why they cannot be freed.

**Size:** 20 lines plus investigation.

### 1.2 Free the `TemplateList` in `typedef_table_free`

`mono_init` allocates a `TemplateList` and installs it on the
`TypeDefTable`. Nothing frees it. The `TypeDefTable` API could
either own the handle (freeing it in `typedef_table_free`) or
call a `mono_free` hook. The current design leaves it to the
caller, and no caller does.

**Size:** 30 lines.

### 1.3 Structural RC-scope pairing

Bundle the "emit `ENTER_SCOPE` + push `RcScope`" and "emit
`EXIT_SCOPE` + pop `RcScope`" pairs into single helper
functions. Removes the possibility of the `AST_FOR_IN` class of
bug.

**Size:** 50 lines in `codegen.c`.

### 1.4 Fix `unmangle_name` for nested generics

`unmangle_name("Box#Option#i64")` produces `"Box<Option, i64>"`
instead of `"Box<Option<i64>>"`. Recursive renderer needed.

**Size:** 30 lines in `types.c`.

### 1.5 Add `smap_span_equals` NULL-map guard

`smap_span_equals` does not check for a NULL map. Add the guard
for symmetry with `smap_decode` and `smap_span_text`.

**Size:** 2 lines.

### 1.6 Clean up dead vestiges

- Remove the `Tier` field from `AstNode`, or add a comment
  documenting its vestigial status in the header.
- Remove or document `Parser.panic`.
- Remove or document the unused `TOK_ARROW`.
- Add a comment to `common.h`'s trailing post-`#endif` block.

**Size:** Cosmetic; ~30 lines of edits across several files.

### 1.7 Fix the `Makefile`'s `clean` target on POSIX

Split the `clean` recipe so it works on both Windows (`cmd /c`)
and POSIX (`rm -f`), or use a portable construct.

**Size:** 5 lines.

### 1.8 Remove `const_arena` overflow at init; compute at compile

`materialize_consts` aborts with `exit(1)` if the const arena
would exceed 64 KiB. The compiler could compute the total at
codegen time and reject at compile time with a diagnostic.

**Size:** 40 lines across `codegen.c` and `vm.c`.

---

## §2 — Medium-term (one-week work)

These are features or refactors that touch multiple files and
would benefit from a deliberate design pass.

### 2.1 `USING_ALIAS` (`using Name = Type`)

The syntax parses and is stored as an `AST_USING` with kind
`USING_ALIAS`, but nothing consumes it. Wire it up:

- Add alias entries to the `FileNamespace` (or a separate table).
- Make `type_expr_resolve` consult the aliases when resolving a
  `TEX_NAME`.
- Add a test.

**Size:** ~100 lines across `types.c`, `resolve.c`, and
`sema.c`.

### 2.2 Selective imports of constants

A `using lib.{ X }` where `X` is a constant has no effect. The
resolver does not consult the constant table for imported names.
Fix: extend the `SelectiveImport` record and the resolver's
`find_selective` to consult constants.

**Size:** ~80 lines.

### 2.3 File-scoped type namespaces

Currently duplicate type names across files are rejected. A
per-file namespace would let two files each declare a `Point`.
Requires:

- A `file_id` field on `TypeDef`.
- A `typedef_table_find_in_file` variant that does not require
  the caller to specify a file (which the current one does).
- Threading the current file through every type lookup site.

**Size:** ~200 lines across `types.c`, `sema.c`, `resolve.c`.

### 2.4 Compound `Type*` interning

Make identity comparison on compound `Type*` well-defined by
interning. A hashtable keyed on `(kind, inner, is_mut, tdef)`,
allocated per compilation and freed at shutdown.

**Size:** ~150 lines in `types.c` plus a per-compilation arena
in the driver.

### 2.5 `is_rc_managed` transparency

Move the RC wrapping of `Result`/`Option` from each use site
into `type_expr_resolve`, so every consumer sees `rc<Enum>`
uniformly. Removes the three-site unwrap pattern and prevents a
future fourth site from being forgotten.

**Size:** ~100 lines in `types.c` plus updates to the three
unwrapping sites.

### 2.6 Better exhaustiveness checking

Current: top-level only; matches on integers and strings
require a wildcard. Better: track which integer values are
covered, warn when a match is incomplete.

**Size:** ~200 lines in `sema.c`.

### 2.7 A `dump_ast` debug printer

A function that walks the AST and prints a tree. Useful for
debugging parser and sema bugs. Not exposed in the CLI, just a
debug helper.

**Size:** ~300 lines.

### 2.8 `language/errors.md` — the diagnostics catalog

Enumerate every diagnostic the compiler can emit, with the
trigger and an example. Also gives the test suite authoritative
needles.

**Size:** ~3000 words; a documentation task.

### 2.9 A `SemaOptions` struct

The seven-argument `sema_analyze` signature is at its limit. A
`SemaOptions` struct would consolidate it and give a place for
future flags (e.g. `--no-prelude` for tests).

**Size:** ~50 lines plus call-site updates.

---

## §3 — Long-term (month-scale)

These are features that would meaningfully change the language
or its tooling.

### 3.1 Traits

The single most-requested feature that does not exist. Would
require: new syntax, new type-system machinery, a dispatch
mechanism (static or dynamic), codegen support, and a stdlib
that uses them.

The design is open. A `trait` keyword with method signatures and
`impl` blocks is the Rust model; a structural-conformance model
is another option. The choice affects everything downstream.

**Size:** months.

### 3.2 Standard library

Prism has no stdlib. The prelude defines `Result` and `Option`;
everything else the programmer writes themselves or calls via
the FFI.

A first stdlib would be `std.mem`, `std.str`, `std.io`, and
`std.math`, written in Prism over the FFI primitives. Building
it would surface many language limitations (traits, better
generics, `USING_ALIAS`).

**Size:** months.

### 3.3 Full Hindley-Milner inference

Current inference is bidirectional and partial: locals from
initializers, return types from value-returns. Full HM would
infer more, at the cost of more complex diagnostics and a
substantially different type-checking algorithm.

**Size:** a compiler rewrite of sema.

### 3.4 In-language test blocks

`test "name" { ... }` compiled into a hidden function; an
external runner calls them. Would make tests part of the source
language rather than separate C files.

**Size:** weeks.

### 3.5 REPL

Interactive read-eval-print. Would need incremental parsing (a
statement at a time), a persistent `SemaCtx` across evaluations,
and a way to handle top-level errors without aborting the
session.

**Size:** weeks.

### 3.6 Optimization

Peephole, constant folding, dead-code elimination. The current
codegen emits straightforward bytecode; no optimizer runs.
A peephole pass over the `Chunk` before `validate_bytecode`
would be the smallest first step.

**Size:** weeks for a basic peephole; months for a full
optimizer.

### 3.7 FFI callbacks

Passing a Prism function to a C function. Would need a
`FFI_FNPTR` type, a way to marshal a Prism codegen index into a
C function pointer, and a calling convention that lets the C
side call back into the VM.

**Size:** weeks.

### 3.8 Cross-compilation

The VM is the only target. No other backend exists. Adding a
native backend (x86_64, ARM64) would be a project of its own.

**Size:** a separate project.

---

## §4 — The order I would take

If I were picking up the project today and had a month, this is
the sequence.

**Week 1 — Housekeeping.** §1.1 through §1.8. Each is small;
together they remove a class of latent bugs and improve the
developer experience.

**Week 2 — Structural fixes.** §2.1 (`USING_ALIAS`), §2.5
(`is_rc_managed` transparency), §2.4 (compound `Type*`
interning). The first closes a language feature that parses but
does nothing; the second and third reduce the surface area of
convention-only invariants.

**Week 3 — Diagnostics and testing.** §2.8 (`language/errors.md`)
and §2.6 (better exhaustiveness). The first is user-facing; the
second is a real feature.

**Week 4 — Design.** §3.1 (traits) or §3.2 (stdlib). Pick one
and spend the week on design, not implementation. The two are
related: a stdlib built over traits would be much better than a
stdlib built over duplication.

---

## §5 — What I would not do

**Not yet:** a REPL, optimization, cross-compilation. Each is
substantial and none is on the critical path until the stdlib
exists.

**Not ever (probably):** a garbage collector, exceptions, a
package manager. Each is a design choice that conflicts with
the language's current shape (arena + RC, explicit errors,
single-file compilation). If one is needed, the shape of the
project changes.

**Not without a use case:** most features in §3. A feature that
no program needs is complexity without value. The stdlib would
surface the requirements; features should follow from real
programs.
