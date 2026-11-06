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
