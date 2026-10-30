// ============================================================
// diagnostics.h — DiagnosticBag API.
//
// The compiler collects every error and warning it can find in a
// single DiagnosticBag rather than bailing out on the first one.
// This is what lets sema report multiple type errors from one
// compilation pass, and it is why the bag tracks error_count and
// warning_count separately: at the end of compilation, main() can
// ask diag_has_errors() to decide the process exit code without
// walking the items array.
//
// The bag owns a heap-allocated array of Diagnostic records that
// grows geometrically (see diag_report in diagnostics.c). Records
// store a fixed-size message string, not a pointer, so the bag is
// self-contained and does not need to outlive its formatting
// arguments.
//
// Level semantics:
//   DIAG_NOTE    — context for a nearby error; never counted
//                  against error_count, does not fail the build.
//   DIAG_WARNING — non-fatal; does not fail the build by itself.
//   DIAG_ERROR   — fatal; any error in the bag means the build
//                  should not proceed to codegen.
// ============================================================
#ifndef PRISM_DIAGNOSTICS_H
#define PRISM_DIAGNOSTICS_H

#include "common.h"
#include "source_map.h"
#include <stdarg.h>

typedef enum { DIAG_NOTE, DIAG_WARNING, DIAG_ERROR } DiagnosticLevel;

// One collected message. The `span` is the source range the
// diagnostic points at; it may be SPAN_NONE if the issue is not
// tied to a specific location (e.g. a command-line problem).
//
// `message` is a fixed 256-byte buffer, not a pointer. That is a
// deliberate simplification: the bag never has to free individual
// messages, and copying the bag is a plain byte copy. It does cap
// a single diagnostic at 255 characters plus NUL; longer messages
// are silently truncated by vsnprintf in diag_report. If that ever
// bites, this struct is the place to change.
typedef struct {
    DiagnosticLevel level;
    Span span;
    char            message[256];
} Diagnostic;

// Dynamic array of diagnostics plus a pair of running counters.
// Invariants:
//   - 0 <= count <= capacity.
//   - items is either NULL (count == capacity == 0) or points to
//     `capacity` Diagnostic records, of which the first `count`
//     are initialized.
//   - error_count   == number of items[i].level == DIAG_ERROR.
//   - warning_count == number of items[i].level == DIAG_WARNING.
// These counters are maintained by diag_report; nothing else
// should write to them.
typedef struct {
    Diagnostic* items;
    int         count;
    int         capacity;
    int         error_count;
    int         warning_count;
} DiagnosticBag;

// Zero-initialize a bag. Safe to call on an uninitialized stack
// bag, and safe to call on a bag that has been diag_free'd.
void diag_init(DiagnosticBag* bag);
// Release the items array and reset the bag to its initial state.
// The bag pointer itself remains valid; it is just empty again.
void diag_free(DiagnosticBag* bag);
// Reset counts to zero without freeing the items buffer. Cheaper
// than diag_free + diag_init when the caller expects to report
// more diagnostics into the same bag (e.g. between compilation
// units) and wants to keep the already-allocated capacity.
void diag_clear(DiagnosticBag* bag);

// Append a formatted diagnostic. `fmt` is a printf-style format
// string; the varargs are consumed immediately (the message is
// copied into the record), so nothing needs to stay alive after
// the call. Grows the items array if needed and increments the
// matching counter based on `level`.
void diag_report(DiagnosticBag* bag, DiagnosticLevel level,
                 Span span, const char* fmt, ...);

// True iff at least one DIAG_ERROR has been reported into the bag.
// This is the check main() uses to decide whether to run codegen.
bool diag_has_errors(const DiagnosticBag* bag);

// Print every diagnostic in the bag to stderr, using smap to turn
// each span into file:line:col and a source excerpt with a caret
// underline. If smap is NULL, only the level and message are
// printed. Never modifies the bag.
void diag_flush(const DiagnosticBag* bag, const SourceMap* smap);

// Convenience macros. They exist so call sites read as
//   diag_error(&bag, tok->span, "expected '%s'", "}");
// rather than spelling out DIAG_ERROR and the function name.
// Variadic so the format string and args are forwarded unchanged.
#define diag_error(bag, span, ...)   diag_report((bag), DIAG_ERROR,   (span), __VA_ARGS__)
#define diag_warning(bag, span, ...) diag_report((bag), DIAG_WARNING, (span), __VA_ARGS__)
#define diag_note(bag, span, ...)    diag_report((bag), DIAG_NOTE,    (span), __VA_ARGS__)


#endif