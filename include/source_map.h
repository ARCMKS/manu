// ============================================================
// source_map.h — Multi-file source registry and span decoder.
//
// The SourceMap is the single authority on what a Span means.
// Nothing else in the codebase walks backward through source to
// find a line start, or computes a column. If you need to turn a
// Span into a line/column/excerpt, call smap_decode.
//
// File ids are 1-based. File 0 is reserved as the "no file"
// sentinel so that SPAN_NONE is always invalid.
//
// Ownership summary:
//   - The map owns the heap copy of each file's content and the
//     line_starts array. It does NOT own the path string; the
//     caller must keep it alive for as long as the map is used
//     (typically the string is a command-line argument or a
//     string literal, both of which outlive compilation).
//   - SpanDecoded borrows pointers from the SourceFile; they are
//     valid until the file is removed from the map. The map never
//     removes files, so decoded pointers stay valid for the life
//     of the map.
//
// Callers should treat every SourceFile* returned by smap_get as
// read-only. The map's contents are considered frozen once a file
// has been added: lexer/parser/sema all read spans against the
// same line_starts table, and mutating a file after tokens point
// into it would silently corrupt every downstream diagnostic.
// ============================================================
#ifndef PRISM_SOURCE_MAP_H
#define PRISM_SOURCE_MAP_H

#include "common.h"
#include "lexer.h"

// One file's stored source plus a precomputed index of line starts.
//
// `content` is a heap copy of the caller's string, always NUL-
// terminated at index `length` even though `length` is the true
// byte length (the NUL is not counted). The NUL is a convenience
// for code that wants to treat content as a C string; it is not
// part of the file for span purposes.
//
// `line_starts` has `line_count` entries. Entry 0 is always 0;
// entry k (k >= 1) is the byte offset of the first character of
// line k+1, i.e. the byte just after the k-th '\n'. Lines are
// therefore counted 1..line_count in the public API, matching the
// 1-based line numbers produced by smap_decode.
//
// A file that ends with '\n' gets an extra, empty final line:
// the byte after the last '\n' is the start of a line with zero
// bytes before EOF. This matches the usual editor model and is
// the reason the line-count loop in smap_add counts newlines
// plus one.
typedef struct {
    const char* path;         // borrowed; the caller's string must outlive the map
    char*       content;      // owned by the map
    uint32_t    length;       // bytes, not including a NUL terminator
    uint32_t*   line_starts;  // offset of the first byte of each line
    uint32_t    line_count;
} SourceFile;

// Dynamic array of SourceFiles. Slot 0 is a synthetic placeholder
// whose `path` is "<none>"; it exists so that file_id 0 can be
// reserved as the "no file" sentinel without special-casing every
// lookup. Real files start at index 1, and smap_add returns that
// index. The synthetic slot has content == NULL, so any attempt to
// decode a span with file_id 0 fails cleanly in smap_get.
typedef struct {
    SourceFile* files;
    int         count;
    int         capacity;
} SourceMap;

// Result of decoding a Span. All fields are outputs of
// smap_decode; callers should not construct one by hand.
//
// Line and column are 1-based to match the numbers a user sees in
// an editor. Column is a byte offset within the line, not a
// character or grapheme offset; a UTF-8 source file with non-ASCII
// text on the offending line will report the byte column, which is
// what the caret renderer in diag_flush needs.
//
// line_start and line_end delimit the *source text* of the line
// (excluding the terminating '\n'). line_end points at the '\n'
// byte if the line is newline-terminated, or at f->length if it is
// the final, unterminated line.
//
// file_path and file_content borrow from the SourceFile and remain
// valid for the life of the SourceMap.
typedef struct {
    uint32_t    line;          // 1-based
    uint32_t    column;        // 1-based
    uint32_t    line_start;    // offset into file content
    uint32_t    line_end;      // offset of the newline (or file end)
    const char* file_path;     // borrowed from the SourceFile
    const char* file_content;  // borrowed from the SourceFile
} SpanDecoded;

// Zero-initialize a map. Safe on an uninitialized stack value.
// The sentinel slot is not allocated here; it is created lazily by
// the first smap_add so that a map that is never used costs
// nothing but the three zeroed fields.
void smap_init(SourceMap* sm);
// Release every owned content/line_starts buffer and the files
// array, then reset the map to its zeroed state. The sentinel
// slot is skipped by the free loop because its content is NULL
// and it was never separately allocated.
void smap_free(SourceMap* sm);

// `content` is copied. `path` is borrowed. Returns the file_id (>= 1).
// The returned id is what callers store in Span.file_id. Ids are
// assigned in insertion order and never reused for the life of
// the map.
int  smap_add(SourceMap* sm, const char* path, const char* content);

// Look up a file by id. Returns NULL for file_id 0, for an
// out-of-range id, and for a NULL map. The result is a borrowed
// pointer into the map's array; do not free it.
SourceFile* smap_get(const SourceMap* sm, uint32_t file_id);
// Convenience wrapper around smap_get that returns the file's
// content pointer, or NULL if the id is invalid.
const char* smap_file_content(const SourceMap* sm, uint32_t file_id);

// Decode a span into line/column/excerpt metadata. Returns false
// (and leaves *out untouched) if the map is NULL, out is NULL,
// the span is SPAN_NONE, the file_id is invalid, or the file has
// no line table. Returns true on success.
//
// This is the *only* function in the compiler that is allowed to
// compute a line/column from a byte offset. If you find yourself
// wanting to do the same arithmetic elsewhere, call this instead.
bool smap_decode(const SourceMap* sm, Span span, SpanDecoded* out);

// Copies the bytes of `span` into `out` as a NUL-terminated string.
// Truncates to `cap` bytes. Returns bytes written (excluding NUL).
//
// Truncation is silent: the return value is the number of bytes
// actually written, so callers that need to distinguish "short
// span" from "clipped span" can compare it against span_len.
// If cap is 0, returns 0 and does not touch out.
int  smap_span_text(const SourceMap* sm, Span span, char* out, size_t cap);

// Byte-for-byte comparison of a span against a C string.
// Returns false if the span is in an unknown file, extends past
// the file's end, or differs in length from `str`.
bool smap_span_equals(const SourceMap* sm, Span span, const char* str);
// Byte-for-byte comparison of two tokens' source text, ignoring
// the token *kind*. Two tokens are "equal" here iff they cover
// the same bytes, even in different files. Used by identifier
// comparison paths that want to avoid materializing a string.
//
// Parameter names are omitted in this declaration (they appear in
// the definition). Preserved as-is.
bool smap_tokens_equal(const SourceMap*, Token, Token );
#endif // PRISM_SOURCE_MAP_H