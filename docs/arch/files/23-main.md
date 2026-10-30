## `docs/arch/files/23-main.md`
# `src/main.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

The command-line driver: argument parsing, pipeline orchestration,
runtime-error rendering.

**Commands.** `run`, `check`, `help`. A bare file path is
`run <file>`.

**Flags.** `--trace`/`-t`, `--quiet`/`-q`, `-I <dir>` (up to 16),
`--help`/`-h`.

**Exit codes.** 0 success; 1 compile or I/O error; 2 runtime
error; 3 bytecode validation failure; 64 usage error.

---

## §2 — Public interface

Only `main`. Everything else is static.

---

## §3 — Argument parsing

`parse_args` fills a `Cli` struct:

```c
typedef struct {
    Command     cmd;
    const char* file;
    const char* test_filter;
    bool        trace;
    bool        quiet;
    const char* include_paths[MAX_INCLUDES];
    int         include_count;
    const char* prog_argv[MAX_PROG_ARGS];
    int         prog_argc;
} Cli;
```

The parser is a single left-to-right walk. Flags and commands
are position-independent. The first non-flag argument is the
command; if it doesn't match `run`, `check`, or `help`, it's
treated as a file path for an implicit `run`.

Arguments after the file path are collected into `prog_argv` for
the program's `args()` builtin. Up to 64 are stored; overflow is
silent.

`--help` anywhere on the command line overrides the command; the
parser sets a `want_help` flag and the loop's tail assigns
`CMD_HELP`.

---

## §4 — The pipeline

```c
typedef struct {
    ModuleLoader  ml;
    DiagnosticBag bag;
    ConstTable    consts;
    TypeDefTable  structs;
    FFITable      ffi;
    SemaCtx       sc;
    AstNode*      program;
    Chunk         chunk;
} CompileUnit;
```

`cu_init` calls each sub-initializer. `cu_free` frees the chunk,
the merged program, the diagnostic bag, the type table, and the
loader.

`compile_multi`:

1. Add include paths: the root file's directory, then `.`, then
   CLI `-I` paths.
2. `modload_load`.
3. `modload_merge`.
4. `prelude_parse` + prepend.
5. `sema_analyze` (if no errors).
6. `codegen_program` (if no errors).
7. Return `!diag_has_errors(&cu->bag)`.

The driver checks errors between stages. Running sema on a
partially-parsed AST would produce cascading errors; running
codegen on a partially-analyzed AST would dereference unresolved
types.

---

## §5 — `cmd_run`

The full pipeline plus:

- `disassemble` (unless `--quiet`).
- `validate_bytecode`; on failure, exit 3.
- `vm_init` + `vm_run`.
- On runtime error, `render_runtime_error`.
- On success, print the return value and the arena high-water
  mark.

`VMConfig` is constructed inline from the chunk's parts. The VM
borrows from the `CompileUnit`, which stays alive until `cu_free`
after `vm_free`.

---

## §6 — `cmd_check`

Runs the same `compile_multi` pipeline. Renders the bag.
Does not validate or execute.

Prints `ok` on success (unless `--quiet`).

---

## §7 — Runtime-error rendering

`render_runtime_error` (static) prints:

- The error message from `vm->error`.
- A source excerpt at `vm->error_pc` (via `source_line_bounds`
  and the chunk's `lines`/`cols` arrays).
- A call trace from deepest to shallowest, walking
  `vm->call_stack[]`. For each frame, the "current ip" is the
  error pc for the top frame and the frame's `call_pc` for
  every other frame.

`source_line_bounds` walks the source from the start on every
call, so rendering N errors is O(N * source length). The
diagnostic path uses `smap_decode` instead; the runtime-error
path cannot, because the `SourceMap`'s `line_starts` are not
available in this function's scope (it takes raw source text).
`INFERRED`: a future change could pass the `SourceMap` through;
not currently done.

---

## §8 — Invariants

- Exit codes are one of the five documented values.
- Every return path from a `cmd_*` function frees the resources
  it acquired.
- The `ModuleLoader` owns the `SourceMap` and the `ParsedFile`
  programs; the `CompileUnit`'s `program` is the merged AST,
  whose decls are borrowed from the loader's programs.

---

## §9 — Known limitations

**No `repl` command.** The usage message does not mention one.
Older design docs mentioned a REPL; it does not exist.

**`source_line_bounds` is O(source) per call.** Only used for
runtime errors; a program that produces many runtime errors is
not realistic.

**No `--` separator for program arguments.** Everything after
the file path is a program argument; there is no way to pass
something that looks like a flag.

**`-I` overflow is silent after a diagnostic.** A caller passing
17 `-I` flags sees an error for the 17th but continues with the
first 16.

**No progress output.** A slow compilation produces no output
between start and finish.

**No parallel compilation.** Files are loaded and analyzed
serially.

**Debug output is unconditional in some paths.** `cmd_run`
prints the disassembly and return-value banner unless
`--quiet`; there is no separate "verbose" level.

**`cmd_check` prints `ok` without any summary.** A `--verbose`
mode would say how many declarations/types/functions were
processed.

---

## §10 — Extension points

- **A `repl` command.** Would need incremental parsing (a
  statement at a time) and a persistent `SemaCtx`.
- **A `--emit-bytecode <file>` flag.** Would let `check` write
  the chunk to disk for later execution.
- **Structured error output.** A `--json` flag would emit
  diagnostics in machine-readable form.
- **A `--time` flag.** Would report stage timings.
- **`--target` for cross-compilation.** The VM is the only
  target; no other backend exists.
- **A `--` separator.** Simple addition to `parse_args`.
- **A `--verbose` mode with per-stage summaries.** Would help
  debug slow compilations.
- **Passing the `SourceMap` to `render_runtime_error`.** Would
  let it use `smap_decode` instead of `source_line_bounds`.
