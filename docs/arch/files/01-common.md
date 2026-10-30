
## `docs/arch/files/01-common.md`

# `include/common.h`, `src/common.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

`common.h` is the smallest header in the project and the one
everything else depends on. It defines three things: fixed-width
type aliases, the `Span` type, and three allocation helpers.

`common.c` implements only the allocation helpers. There is no `.c`
file for the type aliases or `Span`.

**What it does not solve.** No error handling, no memory ownership,
no collections. `common` is a floor, not a framework.

**Who depends on it.** Every other file. `common.h` includes
`<stdint.h>`, `<stdbool.h>`, and `<stddef.h>`; that is its entire
dependency set.

---

## §2 — Public interface

### §2.1 Type aliases

| Alias | Underlying |
|---|---|
| `I64`, `I32`, `I16` | `int64_t`, `int32_t`, `int16_t` |
| `U64`, `U32`, `U16`, `U8` | `uint64_t`, `uint32_t`, `uint16_t`, `uint8_t` |
| `F64`, `F32` | `double`, `float` |

The widths are baked into the VM's stack layout and the FFI's
marshalling. Changing one is not a local edit.

There is no `I8` alias; signed 8-bit is not used in the compiler
(the lexer produces `int8_t` values via C's own types). `U8` is
used for byte pointers, `int8_t` is used explicitly when needed.

### §2.2 `Span`

```c
typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t file_id;
} Span;

#define SPAN_NONE ((Span){ 0u, 0u, 0u })

static inline bool span_is_none(Span s);
static inline uint32_t span_len(Span s);
```

A half-open byte range `[start, end)` into a file identified by
`file_id`. Twelve bytes, value semantics.

**`file_id == 0` is the "no file" sentinel.** `SourceMap` reserves
slot 0 so no legitimate file receives id 0. `span_is_none` tests
`file_id == 0`; that is the whole check.

**`span_len`** is `end - start`. It assumes `end >= start`, which
every construction site maintains. A malformed span with
`end < start` would underflow; nothing defends against this
because nothing constructs such a span.

### §2.3 Allocation helpers

```c
void* xmalloc(size_t n);
void* xcalloc(size_t n, size_t sz);
void* xrealloc(void* p, size_t n);
```

The contract: **if any returns, the allocation succeeded.** On
failure, `fprintf(stderr, "prism: out of memory\n")` and
`exit(1)`. There is no error-return path; callers may assume
non-NULL on return.

Zero-size requests are translated to a one-byte request
(`n ? n : 1`), avoiding the implementation-defined behavior of
`malloc(0)`.

---

## §3 — Implementation notes

`common.c` is roughly forty lines. `xmalloc`, `xcalloc`, and
`xrealloc` each wrap the corresponding standard-library function
with the size-normalization and the abort. Nothing else lives
there.

The header's inline functions (`span_is_none`, `span_len`) are
`static inline`, so a caller that includes `common.h` and uses
`span_len` gets the subtraction at the call site, not an
out-of-line call.

**Why abort instead of return NULL.** A compiler that cannot
allocate 64 bytes for a diagnostic cannot meaningfully report
that failure. Continuing in a degraded state produces worse
outcomes (silent truncation, cascading errors) than aborting.
Every compile-time allocation is therefore treated as either
successful or fatal. This is a policy choice, not a technical
one.

**Where the helpers are not used.** The VM's runtime allocations
(`OP_ALLOC_RC`, `OP_NEW_RC_ARRAY`, `OP_NEW_RC_ENUM`, the string
materialization in `materialize_consts`) use bare `malloc` and
produce runtime diagnostics on failure (`RT_ERR("rc heap out of
memory…")`). At compile time, a failure is the compiler's own
problem; at runtime, a failure is the program's problem, and the
program should see a diagnostic, not a `SIGABRT`.

**The `#endif` comment.** The last two lines of `common.h` sit
after the `#endif`, so they're outside the include guard. They
are comments only and are harmless; a reader who notices them
should not be alarmed.

---

## §4 — Invariants

- `SPAN_NONE.file_id == 0`, and `span_is_none(SPAN_NONE)` is true.
- For every `Span` produced by the lexer or parser, `end >= start`
  and `end <= file->length`. Maintained by construction in
  `lexer.c`'s `span_from` and every call site that assembles a
  span manually.
- The width aliases are used consistently: `U64` in VM state and
  bytecode operands, `uint32_t` in spans and line tables, `I64`
  in AST literal payloads and `FuncSig` slot numbers.
- `xmalloc(0)`, `xcalloc(0, N)`, and `xrealloc(p, 0)` all return
  a non-NULL, single-byte allocation. Callers may treat the return
  as always valid.

---

## §5 — Known limitations

**`Span` size depends on 32-bit fields.** A file larger than
4 GiB would overflow `start` or `end`. `smap_add` casts `strlen`'s
`size_t` return to `uint32_t` without checking; a file that
exceeds 4 GiB would silently wrap its `length`, and every span
into it would be wrong. Not a practical limit for the source
sizes the compiler handles today.

**No `BytePtr` alias.** Byte operations use `U8*` inline. This
is deliberate: a distinct `BytePtr` type would drift into
unrelated code. The compiler only manipulates raw bytes in
well-defined places (`arena_alloc`, `const_alloc`, string
materialization).

**No `xstrdup`.** The codebase has three private `dup_string`
implementations (`module.c`, `sema.c`, `types.c`). A shared
`xstrdup` in `common.c` would consolidate them. Not added
because each private version is only a few lines.

**`span_is_none` and `span_len` are inline, not macros.**
Callers cannot take their address; a function that wanted a
function-pointer to `span_len` would need a separate out-of-line
definition. No caller does.

**Abort-on-OOM is not testable in-process.** A test suite cannot
exercise the failure path without forking. No test does.

**`SPAN_NONE` is a compound literal, not a constant.** A caller
that wants to store it in a static initializer must write the
fields out (`{ 0u, 0u, 0u }`) rather than using the macro. C11
allows compound literals in static initializers, but the syntax
is awkward; the codebase mostly uses `SPAN_NONE` at runtime.

---

## §6 — Extension points

- **A `span_fmt` helper.** Every consumer that needs
  `file:line:col` calls `smap_decode` and formats the result.
  A `span_fmt` helper would centralize that, but it would need a
  `SourceMap*` argument, coupling `common.h` to `source_map.h`.
  Left out on purpose.
- **`xstrdup`.** See §5. A short addition; would need to be
  declared in `common.h` and implemented in `common.c`.
- **A `Span` print helper for diagnostics.** Some call sites
  format spans inline; a helper would be small.
- **Widening the span offsets to 64-bit.** Would need changes in
  every place that constructs a span. Not planned.


