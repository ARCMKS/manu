## `docs/arch/files/03-source-map.md`

# `include/source_map.h`, `src/source_map.c`

*Current as of commit `<hash>`, <date>.*

## §1 — Purpose

The `SourceMap` is the single authority on what a `Span` means.
Nothing else in the compiler walks backward through source to find
a line start, computes a column, or reads identifier bytes out of a
file. If you need to turn a `Span` into a line/column pair, an
excerpt, or a token's text, you call `smap_decode`,
`smap_span_text`, or `smap_span_equals`.

**What it solves.** File registration with stable `file_id`s;
span-to-line/column decoding; the line-start table that makes
decoding cheap; byte-range extraction for name comparison.

**What it does not solve.** It does not read files (the module
loader does that). It does not parse. It does not track scopes.

**Who depends on it.** Every stage of the compiler. The
`SourceMap` is passed through the pipeline and outlives every
stage except the `Chunk`-to-VM handoff (where the `Chunk` still
references spans that the map decodes for error rendering).

---

## §2 — Data layout

```c
typedef struct {
    const char* path;         // borrowed; caller's string must outlive the map
    char*       content;      // owned by the map
    uint32_t    length;       // bytes, not including NUL terminator
    uint32_t*   line_starts;  // offset of the first byte of each line
    uint32_t    line_count;
} SourceFile;

typedef struct {
    SourceFile* files;
    int         count;
    int         capacity;
} SourceMap;

typedef struct {
    uint32_t    line;
    uint32_t    column;
    uint32_t    line_start;
    uint32_t    line_end;
    const char* file_path;
    const char* file_content;
} SpanDecoded;
```

**`SourceFile`.** One per registered file. `path` is borrowed;
`content` is owned; `length` excludes the trailing NUL that
`content` carries for convenience. `line_starts` is a precomputed
array of the byte offset of each line's first character.

**`SourceMap`.** A growable array of `SourceFile`. Index 0 is
reserved as a placeholder; real files start at index 1.

**`SpanDecoded`.** The result of `smap_decode`. All fields are
1-based for line and column, byte offsets for the rest.
`file_path` and `file_content` borrow from the `SourceFile`.

---

## §3 — Public interface

```c
void smap_init(SourceMap* sm);
void smap_free(SourceMap* sm);

int  smap_add(SourceMap* sm, const char* path, const char* content);

SourceFile* smap_get(const SourceMap* sm, uint32_t file_id);
const char* smap_file_content(const SourceMap* sm, uint32_t file_id);

bool smap_decode(const SourceMap* sm, Span span, SpanDecoded* out);

int  smap_span_text(const SourceMap* sm, Span span, char* out, size_t cap);

bool smap_span_equals(const SourceMap* sm, Span span, const char* str);
bool smap_tokens_equal(const SourceMap* sm, Token a, Token b);
```

**`smap_init`.** Initializes an uninitialized map to the empty
state. Does not allocate.

**`smap_free`.** Frees every owned content and line-starts buffer
and the `files` array, then re-initializes. The slot-0 placeholder
is skipped because its `content` and `line_starts` are `NULL`.

**`smap_add`.** Registers a file and returns its 1-based
`file_id`. The `content` buffer is copied into heap memory owned
by the map; the caller may free their buffer immediately. The
`path` string is **not copied**; the caller must keep it alive
for as long as the map is used.

**`smap_get`.** Returns a pointer to the `SourceFile` for a valid
`file_id`, or `NULL` if the id is 0 or out of range.

**`smap_file_content`.** A convenience wrapper that returns the
file's `content` or `NULL`.

**`smap_decode`.** The central function. Fills a `SpanDecoded`
for the given span. Returns false if the map is NULL, the output
pointer is NULL, the span is `SPAN_NONE`, or the `file_id` is
invalid. On success, returns true with every field populated.

**`smap_span_text`.** Copies up to `cap - 1` bytes of the span's
text into `out`, NUL-terminates, and returns the number of bytes
written (excluding the NUL). Returns 0 on a NULL or empty buffer,
a NULL map, or a `SPAN_NONE` span. The `out` buffer is
unconditionally NUL-terminated even on failure.

**`smap_span_equals`.** Byte-for-byte comparison of a span
against a C string. Returns true only if the lengths match and
the bytes match.

**`smap_tokens_equal`.** Byte-for-byte comparison of two tokens'
source text, ignoring the token kind. Cross-file: two tokens in
different files compare equal if their bytes are equal.

---

## §4 — Implementation notes

### §4.1 The reserved slot 0

`file_id == 0` is reserved as the "no file" sentinel, which is
what allows `SPAN_NONE = {0, 0, 0}` to be unconditionally
invalid. The reservation is established by `smap_add` calling
`ensure_slot_zero` on its first invocation. That helper allocates
the files array and writes a placeholder into index 0:

```c
sm->files[0].path = "<none>";
sm->count = 1;
```

After the first `smap_add`, `sm->count` is at least 2 and index
1 holds the first real file. Slot 0's `content` and `line_starts`
are never allocated.

### §4.2 The line-start table

`smap_add` scans the source twice: once to count newlines (to
size the array) and once to fill it. `line_starts[0]` is always
0. Each subsequent `\n` at offset `i` produces an entry at
`i + 1`. `line_count` is `1 + (number of newline bytes)`.

A file with no trailing newline still reports at least one line.
A file that ends with `\n` gets an extra, empty final line: the
byte after the last newline is the start of a line with zero
bytes before EOF.

### §4.3 `smap_decode`

The offset is clamped to the file's length before the search, so
a span whose `start` lies past the end decodes to the last
position rather than failing. The search is a standard binary
search over `line_starts`: after the loop, `lo` is the largest
index with `line_starts[lo] <= off`.

The `column` is `off - line_starts[lo] + 1` — a 1-based byte
offset from the start of the line. The `line_end` is the offset
of the `\n` that terminated the line (which is
`line_starts[lo + 1] - 1`), or the file length for the last line.

Binary search cost is `O(log L)` for a file with `L` lines.

### §4.4 `smap_span_text`

The `out` buffer is unconditionally NUL-terminated even in the
failure paths. Span endpoints are clamped to the file length;
`e` is clamped to be at least `s`. The copy truncates to
`cap - 1` bytes.

The return value is the number of bytes actually copied, not the
length the span would have had if the buffer were larger.

### §4.5 `smap_tokens_equal`

Two paths: same file (a simpler bounds check) and different file
(reads from each file's content). This is the cross-file variant
of the `tok_eq` helper that also appears in `sema.c`,
`codegen.c`, `monomorph.c`, `constant.c`, and `types.c`. The
six implementations are private to their translation units;
consolidating them would be a cleanup, but has not been done.

---

## §5 — Invariants

- `file_id == 0` is never a valid file. Every `smap_get(0)`
  returns `NULL`.
- `smap_add` returns values `≥ 1`; the returned id is stable for
  the map's lifetime.
- For any span produced by the lexer, `end >= start` and
  `span.end <= file->length`.
- The `path` pointer in a `SourceFile` is borrowed; the map never
  frees it.
- The `content` pointer is owned by the map and is freed by
  `smap_free`.
- For a file with `L` lines, `line_starts` has `L` entries,
  `line_starts[0] == 0`, and `line_starts[i] > line_starts[i-1]`
  for every `i > 0`.
- `smap_decode` never reads bytes outside `[0, file->length]`.
- `smap_span_text` always writes a NUL terminator.
- A `SpanDecoded` returned with true has every field initialized.

---

## §6 — Known limitations

**Content is copied.** `smap_add` duplicates the caller's buffer.
The tradeoff is that the map is self-contained, at the cost of
doubling peak memory for one file at a time. For a compiler that
reads every source file at startup, the tradeoff is correct.

**Path strings are not copied.** The map cannot safely outlive
the buffers whose paths it holds. In practice the loader keeps
paths in `ParsedFile` structs and frees both paths and the map
together.

**`smap_span_equals` has no NULL-map guard.** `smap_decode` and
`smap_span_text` both have `if (!sm …)` guards; `smap_span_equals`
does not. A NULL map with a valid `span.file_id` would
dereference NULL. No current caller passes a NULL map.

**`smap_get` casts away const.** The signature takes
`const SourceMap*` and returns `SourceFile*`. The caller is
trusted to treat the result as read-only.

**`smap_tokens_equal`'s cross-file path does not check bounds.**
The same-file path checks `span.start + n > f->length`; the
cross-file path does not. A span with `start` past EOF in one
of the files could read past the heap copy. No caller produces
such a span.

**Line number and column are `uint32_t`.** Same 4 GiB per-file
bound as `Span`.

**No support for multi-line spans in `SpanDecoded`.** Only the
line containing `span.start` is reported. A future diagnostic
that wanted to point at a multi-line construct would need
additional fields.

**`smap_add` truncates the file length to `uint32_t`.** No check
against 4 GiB.

---

## §7 — Extension points

- **An `smap_add_owned` variant.** Takes ownership of a
  caller-provided buffer instead of copying it. Would halve peak
  memory during loading.
- **A `smap_span_line` shortcut.** Returns only the line number,
  skipping column and path lookups. Cheaper for line-based output
  (stack traces, for example).
- **Line-start caching per file.** The binary search is cheap; a
  one-entry cursor would make it cheaper. Not needed yet.
- **Registering a file from a `FILE*`.** Would centralize IO.
  Currently every caller does its own read.
- **`smap_spans_equal(sm, a, b)`.** Would consolidate the six
  private `tok_eq` implementations.
