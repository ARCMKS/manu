
## `docs/arch/files/02-diagnostics.md`

# `include/diagnostics.h`, `src/diagnostic.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

`diagnostics.h` declares the `DiagnosticBag`, the single channel
through which every compiler stage reports warnings and errors.
`diagnostic.c` implements the bag and the stderr renderer.

**What it solves.** A stage can report more than one error without
aborting on the first. Diagnostics from different stages accumulate
in source order. Rendering is centralized and consistent. The bag
can be inspected programmatically (tests grep `items[i].message`
for expected substrings).

**What it does not solve.** It is not a logging system. It has no
levels beyond the three severities, no categories, no machine-
readable output. It is not intended to be machine-parsed; the
rendered form is for humans.

**Who depends on it.** `common.h` (for `Span`) and `source_map.h`
(for the `SourceMap` used during rendering). Every compiler stage
that can report errors.

---

## §2 — Public interface

```c
typedef enum {
    DIAG_NOTE,
    DIAG_WARNING,
    DIAG_ERROR
} DiagnosticLevel;

typedef struct {
    DiagnosticLevel level;
    Span            span;
    char            message[256];
} Diagnostic;

typedef struct {
    Diagnostic* items;
    int         count;
    int         capacity;
    int         error_count;
    int         warning_count;
} DiagnosticBag;

void diag_init(DiagnosticBag* bag);
void diag_free(DiagnosticBag* bag);
void diag_clear(DiagnosticBag* bag);
void diag_report(DiagnosticBag* bag, DiagnosticLevel level,
                 Span span, const char* fmt, ...);
bool diag_has_errors(const DiagnosticBag* bag);
void diag_flush(const DiagnosticBag* bag, const SourceMap* smap);

#define diag_error(bag, span, ...)   diag_report((bag), DIAG_ERROR,   (span), __VA_ARGS__)
#define diag_warning(bag, span, ...) diag_report((bag), DIAG_WARNING, (span), __VA_ARGS__)
#define diag_note(bag, span, ...)    diag_report((bag), DIAG_NOTE,    (span), __VA_ARGS__)
```

**`Diagnostic`.** One report. `level` is one of three severities.
`span` locates the report or is `SPAN_NONE`. `message` is a fixed
256-byte buffer, always NUL-terminated (even on truncation, since
`vsnprintf` guarantees this).

**`DiagnosticBag`.** A growable array plus two counters. The
`error_count` and `warning_count` are maintained incrementally by
`diag_report`; notes are counted in `count` but not in either
severity counter.

**`diag_init`.** Initializes an uninitialized bag. Must be called
before any other function on the same bag.

**`diag_free`.** Frees the item array and re-initializes the bag.
The bag is usable after this call, but any previously-returned
`items` pointer is invalid.

**`diag_clear`.** Resets `count`, `error_count`, and
`warning_count` to zero, keeps the allocated capacity. Used by
stages that want to report a batch, render it, and start fresh
without reallocating.

**`diag_report`.** Appends a diagnostic and updates the counts.
Callers normally use the macros rather than calling this directly.

**`diag_has_errors`.** Returns `error_count > 0`. This is the
predicate every stage calls between phases. Warnings and notes
never affect the return value.

**`diag_flush`.** Renders every entry to stderr. See §3.

---

## §3 — Implementation notes

### §3.1 Growth

`diag_report` doubles the item array from an initial capacity of
8. A bag that reports two diagnostics allocates eight slots; a
bag that reports a hundred ends up at 128. There is no shrink.

For the driver's usage (one bag per compilation, freed at the
end), the growth policy is invisible.

### §3.2 Message formatting

`vsnprintf` writes into the fixed buffer. Its return value is
discarded, so truncation is silent. The message length limit is
255 characters plus NUL; the longest diagnostic in the compiler
is well under 128 characters, so this has never been hit.

### §3.3 Rendering

`diag_flush` walks the bag once. For each diagnostic:

1. Print `level: message` to stderr. The level string is one of
   `"note"`, `"warning"`, `"error"`, from the internal
   `level_str` helper.
2. If `smap` is NULL or the diagnostic's span is `SPAN_NONE`,
   continue to the next diagnostic without a source excerpt. This
   is the path for errors reported before a file has been
   registered (e.g. "cannot read file" from the module loader).
3. Call `smap_decode`. If decoding fails (out-of-range `file_id`),
   skip the excerpt and continue.
4. On a successful decode, print:
   ```
     --> <path>:<line>:<col>
      |
    N | <source line>
      | <spaces>^^^^
   ```

The line-number gutter uses `%2u`, so single-digit lines are
right-aligned with a leading space. The pipe column is three
characters: space, pipe, space.

### §3.4 Caret length

The caret run's length is derived from the span:

```c
uint32_t len = span_len(d->span);
if (len == 0) len = 1;
uint32_t remaining = dec.line_end - (dec.line_start + dec.column - 1);
if (len > remaining) len = remaining;
if (len == 0) len = 1;
```

Three adjustments:

- Zero-length spans (from ASI semicolons) draw one caret.
- The caret run is clamped to the remainder of the line, so a
  span past end-of-line doesn't overrun.
- If the clamp produced zero (a span whose start is exactly at
  the end of the line), the run is promoted to 1.

### §3.5 Color

`diag_flush` emits ANSI color escapes unconditionally, with no
`isatty` check. On a pipe or file, the escapes appear literally.
This is a known limitation; see §5.

---

## §4 — Invariants

- `count <= capacity`.
- `error_count + warning_count <= count`. Notes are counted in
  `count` but not in either severity counter.
- The `items` array is either `NULL` (never grown) or points at
  `capacity` contiguous `Diagnostic` values.
- `diag_free` leaves the bag in the initial state, with
  `items == NULL`, `count == 0`, `capacity == 0`.
- `message` is always NUL-terminated inside the 256-byte buffer.
- `diag_flush` prints each diagnostic exactly once, in the order
  it was reported.
- The output stream is always `stderr`. No caller can redirect
  it.

---

## §5 — Known limitations

**No error codes.** Diagnostics are plain text. There is no
`E0001`-style identifier to reference from documentation or a
caller.

**Fixed 256-byte message limit.** A longer message is silently
truncated. See §3.2.

**No severity filtering.** Every reported diagnostic is retained;
nothing is dropped based on level or context. A future `--quiet`
mode would filter at render time in `diag_flush`, not in
`diag_report`.

**No structured notes attached to errors.** An error and its
explanatory note are independent entries with no parent/child
relation. `diag_flush` prints them in report order, not under the
error they explain. No caller emits notes today.

**Caret-length underflow.** The expression
`dec.line_end - (dec.line_start + dec.column - 1)` is `uint32_t`
arithmetic. If a diagnostic were reported with a span whose start
lies past the last character of its line, the subtraction would
underflow to a large value and the caret run would extend across
the whole file. `smap_decode` cannot produce such a span
currently, but the invariant is not enforced structurally.

**No way to render to a buffer.** `diag_flush` writes to stderr
directly. Tests that want to inspect rendered output must capture
stderr at the process level (`2>&1`). No test currently does;
tests inspect `bag.items[i].message`.

**No deduplication.** Two identical diagnostics are printed twice.
A stage that reports the same error on two paths would produce
duplicate output.

**No notes as context.** A `DIAG_NOTE` prints with the same shape
as a warning or error. There is no way to render a note as an
indented sub-line under a previous diagnostic.

**Unconditional ANSI escapes.** See §3.5.

---

## §6 — Extension points

- **A `FILE*` out parameter on `diag_flush`.** Would let tests
  capture rendered output without process-level redirects. The
  current callers (`main.c`, `test.c`) would pass `stderr`
  unchanged.
- **Error codes.** A `uint16_t code` field on `Diagnostic`, with
  a table mapping codes to stable strings.
- **Multi-line spans.** `SpanDecoded` exposes only the line
  containing `span.start`. A diagnostic whose span crossed lines
  would need `SpanDecoded` to carry `end_line`/`end_column`, and
  `diag_flush` to print several source lines with a caret span
  across them.
- **Column correction for wide characters.** Columns are byte
  offsets. A future Unicode-aware column mode would live in
  `smap_decode`, not here.
- **Severity-based exit codes.** `diag_flush` has no opinion
  about process exit status; the driver decides.
- **A `diag_count(bag)` accessor.** Currently every caller reads
  `bag->count` or `bag->error_count` directly. Adding an accessor
  is trivial.
