# Prism

Prism is a small statically-typed programming language with reference-counted
memory management, parametric generics (monomorphized at compile time), and
C FFI support. **Manu** is the name of the toolchain that compiles and runs it:
a single-pass compiler from `.manu` sources to a self-contained bytecode
`Chunk`, plus a 64-bit stack VM that executes it.

> **Project status: early-stage. APIs, syntax, the bytecode format, and the
> runtime ABI are unstable and will change without notice. This is a research
> and learning project — it is not ready for production use, and breaking
> changes are expected between commits.**

---

## What's in the box

- A lexer, recursive-descent parser, semantic analyzer, and code generator
  (`src/`).
- A bytecode `Chunk` format that carries code, a constant pool, debug tables,
  a function table, and a runtime type table — everything the VM needs to
  execute, with no side channels.
- A stack-based VM with a small register set (`pc`, `sp`, `fp`, `ap`, `amp`)
  and three stacks (data, call, scope).
- Monomorphized generics, a module system, and a C FFI surface
  (`include/prism_ffi.h`).
- A diagnostics pipeline that accumulates errors in a `DiagnosticBag` and
  renders them once, with source excerpts.

---

## Building

Requirements:

- A C99 (or later) compiler — `gcc` and `clang` are both supported.
- POSIX system headers. The toolchain is developed and tested on Linux and
  macOS.
- No external libraries or runtime dependencies.

```sh
git clone https://github.com/ARCMKS/manu.git
cd manu
make

This produces a single `manu` binary in the repository root.

**Platform caveats.**

- **Windows:** build under WSL (or another POSIX-compatible environment). The
  toolchain relies on POSIX file I/O and is not currently tested natively on
  Windows.
- **Cross-compilation:** not supported out of the box. The `Makefile` assumes
  host-native compilation; override `CC`, `CFLAGS`, and `LDFLAGS` as needed.

Test infrastructure is being stabilized. The regression corpus lives under
`tests/`, and the test runner is not exposed through the CLI at this time. Run
`make test` once the runner is re-enabled; until then, see the repository
history and the documents under `docs/` for the current verification story.

---

## Running a program

```sh
./manu run examples/hello.manu
```

The `run` subcommand compiles a source file to a `Chunk` and immediately
executes it in the same process. Use `./manu check <file>` to typecheck and
validate without running. `./manu help` lists the available subcommands.

---

## A five-line example

```prism
fn add(a: i64, b: i64) -> i64 {
    return a + b;
}

fn main() {
    let result: i64 = add(2, 3);
    print("2 + 3 = {result}");
}
```

Save the snippet as `examples/add.manu` and run:

```sh
./manu run examples/add.manu
```

String formatting is resolved at compile time; there is no runtime format
parser.

---

## Documentation

- **Tutorial (start here):** [`docs/language/tutorial.md`](docs/language/tutorial.md)
- **Language reference:** grammar, worked examples, and edge cases under
  `docs/language/`.
- **Architecture and design notes:** the pipeline, the VM, the invariant map,
  and the deliberate trade-offs under `docs/arch/` and `docs/handbook/`.

The architecture docs, in particular `docs/arch/invariants.md`, are the best
entry point if you want to understand *why* the implementation is shaped the
way it is, and which invariants are enforced structurally versus by
convention.

---

## Status, contributions, and roadmap

This project is currently a single-author research and learning effort. The
roadmap and known limitations live in `docs/handbook/`. Expect rough edges;
issues and design discussion are welcome, but the API and language surface
should be treated as in flux until the project reaches a stable milestone.

There is no license file yet; add one before distributing the project or
depending on it.
