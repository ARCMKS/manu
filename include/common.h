// ============================================================
// common.h — Shared types and small utilities.
//
// This header is the lowest layer of the compiler: it defines
// fixed-width integer/float aliases used everywhere else, the
// Span type that every lexer/parser/sema/codegen layer passes
// around for diagnostics, and the allocation wrappers that the
// rest of the codebase calls instead of malloc/calloc/realloc.
//
// Everything here is intentionally dependency-free apart from
// the three standard headers below. If you are reading the
// compiler top-to-bottom, this is where to start.
// ============================================================
#ifndef PRISM_COMMON_H
#define PRISM_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Maximum length of a dotted path (type path or module path)
// in components. Used by the parser to size its path buffers
// and by the monomorphizer when copying a qualified type
// expression. Keeping it in common.h ensures every user of
// the constant sees the same number.
#define PRISM_MAX_PATH_COMPONENTS 16

// Fixed-width type aliases. The rest of the codebase uses these
// rather than `int`, `long`, etc., so that struct layouts and
// (later) FFI marshalling are identical across platforms. F64/F32
// are aliases for the C double/float so callers don't have to
// remember which width a bare `double` is on the current platform.
typedef int64_t  I64;
typedef int32_t  I32;
typedef int16_t  I16;
typedef uint64_t U64;
typedef uint32_t U32;
typedef uint16_t U16;
typedef uint8_t  U8;
typedef double   F64;
typedef float    F32;

// ============================================================
// Span — a half-open byte range [start, end) in a source file.
//
// The file is identified by file_id, an index into a SourceMap.
// file_id == 0 is reserved for "no file"; spans with file_id 0
// are SPAN_NONE and must never be rendered.
//
// Invariants:
//   - start <= end for any non-none span.
//   - file_id indexes a SourceMap that is created before any
//     token is produced. File id 0 is never assigned to a real
//     file, so span_is_none() can be tested by checking file_id.
//
// Half-open rather than inclusive so that span_len() is the
// trivial subtraction below, and so adjacent spans that share a
// boundary (e.g. token A ends at 5, token B starts at 5) do not
// overlap. Diagnostics code relies on this when underlining.
// ============================================================
typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t file_id;
} Span;

// The canonical "no location" value. Any Span with file_id == 0
// is treated as absent by span_is_none() below; this macro is
// provided so callers don't spell out the zero triple everywhere.
// Do not render a Span that satisfies span_is_none().
#define SPAN_NONE ((Span){ 0u, 0u, 0u })

// True if the span carries no source location. Used by diagnostics
// to decide whether to attach a file:line:col to a message.
static inline bool span_is_none(Span s) {
    return s.file_id == 0;
}
// Length in bytes of the span. Only meaningful for a non-none span;
// calling this on SPAN_NONE yields 0 because start == end == 0.
static inline uint32_t span_len(Span s)     { return s.end - s.start; }

// ---- Allocation helpers (abort on OOM) ----
//
// These wrap malloc/calloc/realloc and terminate the process on
// failure. The compiler cannot meaningfully continue without
// memory: every later stage would have to invent a recovery path,
// and the language has no partial-compilation mode. Aborting with
// a message on stderr is the whole error strategy here. All
// callers of these may assume a non-NULL result on return.
void* xmalloc(size_t n);
void* xcalloc(size_t n, size_t sz);
void* xrealloc(void* p, size_t n);

#endif // PRISM_COMMON_H

// FFI is enabled when the build links against libffi. The Makefile
// defines PRISM_FFI_AVAILABLE when it finds ffi.h.
// (No code here; kept as a documentation marker.)
//
// Note: this comment sits after #endif so it is not covered by the
// include guard. That is harmless (it is only a comment) but means
// it will be re-read on every inclusion. Preserved as-is per the
// annotation rules.