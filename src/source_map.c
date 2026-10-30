// ============================================================
// source_map.c — SourceMap implementation.
//
// Responsibilities:
//   - Own the byte copies of every source file and their
//     precomputed line-start tables.
//   - Provide id-based lookup (smap_get) and the single span ->
//     (line, column, excerpt) conversion (smap_decode).
//   - Provide a few small helpers for comparing spans to strings
//     and to each other without materializing a heap string.
//
// The line-start table is what makes smap_decode cheap: it lets
// the decoder binary-search for the line containing an offset
// rather than scanning from the start of the file on every
// diagnostic.
// ============================================================
#include "../include/source_map.h"
#include <string.h>
#include <stdlib.h>

// Zero-initialize a map. Deliberately does not allocate the
// sentinel slot; that happens lazily in ensure_slot_zero so an
// unused map costs no heap.
void smap_init(SourceMap* sm) { sm->files = NULL; sm->count = 0; sm->capacity = 0; }

// Free every owned buffer and reset the map.
//
// The loop starts at i = 1 because slot 0 is the synthetic
// sentinel: it has content == NULL and line_starts == NULL and
// must not be freed. If count is 0 (map was never used) the loop
// body never runs, free(NULL) is harmless, and the map is reset
// to its initial state by smap_init.
void smap_free(SourceMap* sm) {
    for (int i = 1; i < sm->count; i++) {
        free(sm->files[i].content);
        free(sm->files[i].line_starts);
    }
    free(sm->files);
    smap_init(sm);
}

// Lazily install the sentinel at index 0 the first time a file is
// added. The 4-slot initial capacity is a small heuristic: single-
// file compilations are the common case, but a prelude plus a main
// file plus one or two imports fit without a realloc.
//
// The sentinel's path is "<none>" so that if it is ever printed
// (it should not be, because smap_decode rejects file_id 0) the
// reader sees something meaningful. count is set to 1 so the next
// smap_add writes to slot 1 and returns 1, preserving the
// "file ids are 1-based" contract in the header.
static void ensure_slot_zero(SourceMap* sm) {
    if (sm->count > 0) return;
    sm->capacity = 4;
    sm->files = (SourceFile*)xcalloc((size_t)sm->capacity, sizeof(SourceFile));
    sm->files[0].path = "<none>";
    sm->count = 1;
}

// Copy a file into the map and return its 1-based id.
//
// Invariants established here (relied on by smap_decode and every
// caller of smap_get):
//   - f->content is a heap copy of `content` (or of the empty
//     string if content is NULL), NUL-terminated at index
//     f->length. The NUL is not counted in f->length.
//   - f->line_starts has exactly f->line_count entries, is sorted
//     ascending, and starts with 0.
//   - f->line_count equals (number of '\n' in content) + 1. The
//     file-ending-with-newline case therefore produces a final
//     empty line, which matches the editor model and is what
//     diag_flush's caret math assumes.
//   - f->path is the caller's pointer if non-NULL, otherwise the
//     literal "<anonymous>". The map borrows it; it must outlive
//     the map. Callers that pass a transient string will get a
//     dangling path in diagnostics; today every call site passes
//     a command-line argument or a literal, so this is safe.
//
// Two loops over the content are used rather than one: the first
// counts newlines so the line_starts array can be sized exactly,
// the second fills it. A single-pass version would need a growing
// array, which is not worth the complexity for source files of
// realistic size.
int smap_add(SourceMap* sm, const char* path, const char* content) {
    ensure_slot_zero(sm);
    if (sm->count >= sm->capacity) {
        sm->capacity = sm->capacity ? sm->capacity * 2 : 4;
        sm->files = (SourceFile*)xrealloc(sm->files,
                                          sizeof(SourceFile) * (size_t)sm->capacity);
    }
    size_t len = content ? strlen(content) : 0;

    SourceFile* f = &sm->files[sm->count];
    f->path   = path ? path : "<anonymous>";
    f->length = (uint32_t)len;
    char* copy = (char*)xmalloc(len + 1);
    if (len) memcpy(copy, content, len);
    copy[len] = '\0';
    f->content = copy;

    // Count lines. Always at least one line, even for an empty
    // file: line 1 is the empty line covering [0, 0).
    uint32_t lines = 1;
    for (size_t i = 0; i < len; i++) if (content[i] == '\n') lines++;
    f->line_count = lines;
    f->line_starts = (uint32_t*)xmalloc(sizeof(uint32_t) * lines);
    f->line_starts[0] = 0;
    uint32_t li = 1;
    // The `li < lines` guard is redundant given the count loop
    // above, but keeps the write bounded if the two loops ever
    // diverge during a future edit.
    for (size_t i = 0; i < len && li < lines; i++) {
        if (content[i] == '\n') f->line_starts[li++] = (uint32_t)(i + 1);
    }
    return sm->count++;
}

// Look up a file by id. The cast away of const is safe because the
// map's array is not conceptually const; the const qualifier on
// the parameter is a caller-side promise not to mutate through
// *this call*, and the two callers below (smap_file_content and
// smap_decode) also treat the result as read-only. Preserved
// as-is; if you want to remove the cast you'd have to change the
// return type to const SourceFile*, which would ripple into every
// caller. Not proposing that now.
SourceFile* smap_get(const SourceMap* sm, uint32_t file_id) {
    if (file_id == 0 || file_id >= (uint32_t)sm->count) return NULL;
    return (SourceFile*)&sm->files[file_id];
}

const char* smap_file_content(const SourceMap* sm, uint32_t file_id) {
    SourceFile* f = smap_get(sm, file_id);
    return f ? f->content : NULL;
}

// Decode a span into line/column/excerpt metadata.
//
// Failure modes (all return false, leaving *out untouched):
//   - sm or out is NULL.
//   - span is SPAN_NONE (file_id == 0).
//   - span.file_id is out of range.
//   - the file's line table is missing (should not happen for a
//     file added through smap_add, but is checked defensively).
//
// The clamp `if (off > f->length) off = f->length` handles spans
// that point one past EOF, which the lexer can produce for an
// unterminated token at the very end of input. Clamping rather
// than rejecting means the diagnostic lands at the last byte
// instead of being silently dropped; diag_flush's caret math
// relies on line_end >= line_start, which the clamp preserves.
//
// The binary search finds the largest `lo` with
// line_starts[lo] <= off. Because line_starts[0] == 0 and
// line_starts is sorted, `lo` is always a valid line index for
// any clamped offset.
//
// line_end: if there is a next line, it is the byte just before
// the next line's start, i.e. the '\n'. For the final line, it is
// f->length (the file may or may not end with a newline; if it
// does, the "final line" is the empty line after it, whose
// line_end equals its line_start equals f->length).
bool smap_decode(const SourceMap* sm, Span span, SpanDecoded* out) {
    if (!sm || !out || span_is_none(span)) return false;
    SourceFile* f = smap_get(sm, span.file_id);
    if (!f || !f->line_starts) return false;

    uint32_t off = span.start;
    if (off > f->length) off = f->length;

    uint32_t lo = 0, hi = f->line_count;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (f->line_starts[mid] <= off) lo = mid; else hi = mid;
    }

    out->line         = lo + 1;
    out->column       = off - f->line_starts[lo] + 1;
    out->line_start   = f->line_starts[lo];
    out->line_end     = (lo + 1 < f->line_count)
                      ? f->line_starts[lo + 1] - 1 : f->length;
    out->file_path    = f->path;
    out->file_content = f->content;
    return true;
}

// Copy the bytes of `span` into `out` as a NUL-terminated string.
//
// All clamping mirrors smap_decode: the start and end offsets are
// independently clamped to f->length, and end is additionally
// clamped up to start so a reversed span produces an empty string
// rather than a huge memcpy. This is the same "one-past-EOF is
// allowed, reversed spans are not" contract smap_decode uses.
//
// Truncation: if the span is longer than cap-1 bytes, only the
// first cap-1 bytes are copied. The NUL is always written, so
// callers may treat `out` as a string on every successful return.
// The return value is the number of source bytes copied, not
// including the NUL, so a caller that cares about truncation can
// compare it against span_len(span).
int smap_span_text(const SourceMap* sm, Span span, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!sm || span_is_none(span)) return 0;
    SourceFile* f = smap_get(sm, span.file_id);
    if (!f || !f->content) return 0;

    uint32_t s = span.start, e = span.end;
    if (s > f->length) s = f->length;
    if (e > f->length) e = f->length;
    if (e < s) e = s;
    uint32_t n = e - s;
    if (n >= cap) n = (uint32_t)cap - 1;
    memcpy(out, f->content + s, n);
    out[n] = '\0';
    return (int)n;
}

// Byte-for-byte compare a span against a C string.
//
// Length is checked first so the memcmp below is only reached when
// the sizes agree. The bounds check `span.start + n > f->length`
// guards against a span whose end runs past EOF (the lexer can
// produce these for unterminated tokens), which would otherwise
// make memcmp read past the file's heap copy.
//
// Note: this does NOT go through span_is_none. A SPAN_NONE has
// file_id 0, so smap_get returns NULL and the function returns
// false. Same result, one branch earlier. Fine either way.
bool smap_span_equals(const SourceMap* sm, Span span, const char* str) {
    int n = (int)strlen(str);
    if ((int)span_len(span) != n) return false;
    SourceFile* f = smap_get(sm, span.file_id);
    if (!f) return false;
    if (span.start + (uint32_t)n > f->length) return false;
    return memcmp(f->content + span.start, str, (size_t)n) == 0;
}

// Compare two tokens' source text, ignoring their kinds.
//
// Two cases:
//   1. Same file: compare the two byte ranges in that file.
//      Both start offsets are bounds-checked before the memcmp.
//   2. Different files: fetch each file and compare the two byte
//      ranges across files. This is the "cross-file comparison"
//      path mentioned in the comment; it is what makes an
//      identifier declared in file A compare equal to the same
//      identifier spelled in file B.
//
// The bounds checks are in both paths for the same reason as in
// smap_span_equals: the lexer can emit spans that end at or past
// EOF, and memcmp would read past the heap copy without the check.
//
// Note that the same-file path re-derives n twice (once for each
// token's span_len); that is not a bug, just a slightly verbose
// way of asserting the two lengths are equal before the memcmp.
bool smap_tokens_equal(const SourceMap* smap, Token a, Token b) {
    if (a.span.file_id != b.span.file_id) {
        // Cross-file comparison: compare bytes.
        uint32_t na = span_len(a.span);
        uint32_t nb = span_len(b.span);
        if (na != nb) return false;
        SourceFile* fa = smap_get(smap, a.span.file_id);
        SourceFile* fb = smap_get(smap, b.span.file_id);
        if (!fa || !fb) return false;
        return memcmp(fa->content + a.span.start,
                      fb->content + b.span.start,
                      (size_t)na) == 0;
    }
    uint32_t n = span_len(a.span);
    if (n != span_len(b.span)) return false;
    SourceFile* f = smap_get(smap, a.span.file_id);
    if (!f) return false;
    if (a.span.start + n > f->length) return false;
    if (b.span.start + n > f->length) return false;
    return memcmp(f->content + a.span.start,
                  f->content + b.span.start,
                  (size_t)n) == 0;
}