## `docs/arch/files/21-prelude.md`

# `include/prelude.h`, `src/prelude.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

The compiler prelude is a small Prism source fragment compiled
with every user program. It defines the language's built-in
generic types — `Result` and `Option` — in Prism itself, rather
than special-casing them inside sema or codegen.

**What it solves.** Two things:

1. The prelude's types are ordinary Prism enums with
   `is_builtin = true` on their AST nodes, so sema's
   `register_struct_or_enum` sets `is_rc_managed` on their
   `TypeDef`s. This flag is what makes every use site wrap the
   type in `ty_rc(...)`.
2. Every change to the front end is validated against the
   prelude on every compile, since the same lexer/parser runs on
   it.

**What it does not solve.** Visibility (handled by sema's
`is_pub = true` on instantiations). The `?` operator's typing
(sema's `AST_TRY` case uses name prefixes to identify `Result`
and `Option`).

---

## §2 — Public interface

```c
AstNode* prelude_parse(SourceMap* smap, DiagnosticBag* bag);
```

**`prelude_parse`.** Registers the prelude source in the map under
the synthetic path `"<prelude>"`, parses it, and marks every
top-level declaration `is_builtin = true`.

Returns a fresh `AST_PROGRAM`. The caller prepends its
declarations to the user program and frees the wrapper.

**Side effect on `smap`.** Adds an entry with the given path and
source. If called twice on the same map, two entries with the
same path are added; the parser does not dedupe. The current call
site (`main.c`'s `compile_multi`) calls it once.

---

## §3 — Implementation notes

### §3.1 The source

```c
static const char* const PRELUDE_SOURCE =
    "type Result<T, E> = enum { Ok(T), Err(E) }\n"
    "type Option<T>    = enum { Some(T), None }\n";
```

Two declarations, one per line. The trailing newline is required
by ASI (the parser needs the line terminator to close each
declaration).

**Order matters.** Variant tags are assigned in source order.
`Ok` and `Some` have tag 0; `Err` and `None` have tag 1. This is
the runtime contract for `?`: the operator checks tag 0 for
success.

### §3.2 The path

`"<prelude>"` is a synthetic path. Angle brackets are the
convention for "not a real file"; nothing opens it. The loader's
`find_prelude_file_id` (in `sema.c`) matches this string to
locate the prelude's `file_id`.

### §3.3 The `is_builtin` marking

```c
for (int i = 0; i < prog->as.program.decls.count; i++)
    prog->as.program.decls.items[i]->is_builtin = true;
```

Every top-level declaration gets the flag. Sema's
`register_struct_or_enum` copies it to `TypeDef.is_rc_managed`.
Monomorph's `GenericTemplate.is_builtin` is also copied from it,
which is how instantiated generics know to set the flag.

---

## §4 — Invariants

- The prelude's source is a hard-coded constant. It cannot be
  loaded from a file.
- Every top-level declaration in the returned program has
  `is_builtin == true`.
- The returned program's spans all carry the prelude's
  `file_id` (returned by `smap_add` for `"<prelude>"`).

---

## §5 — Known limitations

**Hard-coded source.** The prelude cannot be extended without
editing `prelude.c` and rebuilding the compiler. A future design
might load it from a file in the install tree.

**Called once per compilation.** No caching across compilations.
For a test suite that runs hundreds of small programs, this
means the prelude is parsed hundreds of times. The cost is small
(~10 tokens) but nonzero.

**The `is_builtin` flag is on AST nodes, not on a name lookup.**
Any prelude declaration inherits it. A future prelude with a
non-builtin helper would incorrectly get `is_rc_managed`.

**Name-based recognition in sema.** The `?` operator's
`Result`/`Option` recognition uses `strncmp` on the mangled
name. Two templates whose names began with `Result` would be
misrecognized. Not reachable today.

---

## §6 — Extension points

- **Loading the prelude from a file.** Would let it be extended
  without a rebuild.
- **A richer prelude.** Adding generic helper types (e.g. a
  `Box<T>`) would follow the same pattern: declare in
  `PRELUDE_SOURCE`, mark `is_builtin`.
- **Prelude caching.** A shared `AST_PROGRAM` freed at process
  exit would eliminate the re-parse cost. Requires care with
  `ast_free` (the merged program does not own the prelude's
  decls after the prepend; ownership transfers).
- **Non-generic prelude declarations.** Currently none; the
  prelude is entirely generic types.
