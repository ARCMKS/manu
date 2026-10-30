// ============================================================
// diagnostic.c — DiagnosticBag implementation and stderr renderer.
//
// Two responsibilities live here:
//   1. The bag itself: append, clear, free, and count.
//   2. diag_flush(), which renders a bag to a terminal, decoding
//      each span through the SourceMap and printing a caret
//      underline under the offending source range.
//
// Nothing here reports errors to the user itself; it only stores
// and prints what other passes have reported. The file is
// deliberately free of any dependency on the lexer/parser so it
// can be used from any layer.
// ============================================================
#include "../include/diagnostics.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

// Zero a bag. Written as a memset rather than field-by-field so
// that adding a field to DiagnosticBag (e.g. a "max errors"
// threshold) does not silently leave it uninitialized here. Safe
// on a stack bag whose contents are garbage.
void diag_init(DiagnosticBag* bag) { memset(bag, 0, sizeof *bag); }

// Free the heap buffer and reset the bag to its zero state. Note
// the reset goes through diag_init, so the bag is usable again
// immediately after this call without a separate init.
void diag_free(DiagnosticBag* bag) { free(bag->items); diag_init(bag); }

// Reset the counters but keep the items buffer allocated. Used
// between compilation units / passes when the caller wants to
// discard messages but avoid a realloc the next time one arrives.
// The stale Diagnostic records remain in the buffer, but since
// count is 0 they are unreachable; the next diag_report will
// overwrite them in order.
void diag_clear(DiagnosticBag* bag) {
    bag->count = bag->error_count = bag->warning_count = 0;
}

// Append one formatted diagnostic to the bag.
//
// Growth policy: start at 8, double when full. Doubling (rather
// than +1 or +fixed) keeps the amortized cost per append O(1) and
// keeps the total realloc count logarithmic even when sema emits
// hundreds of errors from a single bad file.
//
// The message is formatted directly into the record's fixed
// buffer via vsnprintf, which writes at most sizeof(message) bytes
// including the NUL and returns the length that *would* have been
// written. We ignore the return value: if the message was longer
// than the buffer it is silently truncated. That is a deliberate
// tradeoff — see the note on Diagnostic.message in the header.
//
// Level handling: only ERROR and WARNING bump a counter. NOTE is
// intentionally not counted anywhere, so a bag full of notes still
// reports "no errors".
void diag_report(DiagnosticBag* bag, DiagnosticLevel level,
                 Span span, const char* fmt, ...) {
    if (bag->count >= bag->capacity) {
        bag->capacity = bag->capacity ? bag->capacity * 2 : 8;
        bag->items = (Diagnostic*)xrealloc(
            bag->items, sizeof(Diagnostic) * (size_t)bag->capacity);
    }
    Diagnostic* d = &bag->items[bag->count++];
    d->level = level;
    d->span  = span;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->message, sizeof d->message, fmt, ap);
    va_end(ap);
    if (level == DIAG_ERROR)   bag->error_count++;
    if (level == DIAG_WARNING) bag->warning_count++;
}

// The only question callers should ever ask of a bag before
// proceeding to codegen. Uses error_count rather than scanning
// items, which is why that counter exists.
bool diag_has_errors(const DiagnosticBag* bag) { return bag->error_count > 0; }

// Map a level to its display name. The trailing "?" is defensive:
// if the enum ever grows a new level and this switch is not
// updated, we print "?" rather than falling off the end. Some
// compilers warn about the missing default when all enum cases are
// covered, so the return is placed after the switch rather than in
// a default: case.
static const char* level_str(DiagnosticLevel l) {
    switch (l) {
        case DIAG_NOTE:    return "note";
        case DIAG_WARNING: return "warning";
        case DIAG_ERROR:   return "error";
    }
    return "?";
}

// Render the entire bag to stderr.
//
// Output format per diagnostic:
//   <colored level>: <message>
//     --> file:line:col
//      |
//   NN | <source line>
//      |     ^^^
//
// Coloring is done with raw ANSI escapes. No detection of whether
// stderr is a TTY; on a pipe or file the escapes will appear
// literally. Preserved as-is; flagging below if you want to change
// it later.
//
// Span rendering walks the SourceMap to get the line's byte range
// and the column of the span start. Two guards are worth calling
// out:
//   - If len (span_len) is 0, it is promoted to 1 so that a
//     zero-width span still produces a single caret.
//   - The caret run is clamped to the remainder of the line, so a
//     span that runs past EOL (or past EOF) does not print past
//     the source text. The clamp can produce len == 0 again, and
//     the second "if (len == 0) len = 1" re-promotes so at least
//     one caret is always printed.
void diag_flush(const DiagnosticBag* bag, const SourceMap* smap) {
    for (int i = 0; i < bag->count; i++) {
        const Diagnostic* d = &bag->items[i];
        fprintf(stderr, "\x1b[31m%s\x1b[0m: %s\n", level_str(d->level), d->message);

        // No SourceMap, or a diagnostic with no location: print
        // just the message line and move on. This is the path taken
        // for driver-level errors reported before any source file
        // has been registered.
        if (!smap || span_is_none(d->span)) continue;

        // smap_decode fills `dec` with the file path, the byte
        // range of the containing line, the 1-based line/column of
        // the span start, and a pointer to the file's full source
        // text. If it fails (span outside the file, bad file_id),
        // we skip the excerpt rather than guessing.
        SpanDecoded dec;
        if (!smap_decode(smap, d->span, &dec)) continue;

        fprintf(stderr, "  \x1b[36m--> %s:%u:%u\x1b[0m\n",
                dec.file_path, dec.line, dec.column);
        fprintf(stderr, "   |\n");

        // Print the source line itself. Length is taken from the
        // decoded line range rather than strlen so that a file
        // without a trailing newline on its last line still works,
        // and so embedded NULs (which Prism does not currently
        // allow, but SourceMap does not forbid) do not truncate.
        fprintf(stderr, "%2u | %.*s\n",
                dec.line,
                (int)(dec.line_end - dec.line_start),
                dec.file_content + dec.line_start);

        // Indent the caret row to line up under the span start.
        // dec.column is 1-based, so the caret row needs column-1
        // spaces after the "   | " gutter. Leading spaces are not
        // tab-expanded: a source line containing a tab will show
        // the caret out of alignment. Preserved as-is.
        fprintf(stderr, "   | ");
        for (uint32_t c = 1; c < dec.column; c++) fputc(' ', stderr);

        // Caret run length: the span's own length, clamped to the
        // rest of the line. See the function header for the two
        // zero-length promotions.
        uint32_t len = span_len(d->span);
        if (len == 0) len = 1;
        uint32_t remaining = dec.line_end - (dec.line_start + dec.column - 1);
        if (len > remaining) len = remaining;
        if (len == 0) len = 1;
        // Each caret is wrapped in its own bold-yellow escape
        // sequence. That is wasteful (one escape pair per caret
        // instead of one pair around the run) but means the
        // terminal does not need to remember state across the
        // newline if output is interrupted. Preserved as-is.
        for (uint32_t c = 0; c < len; c++) fputs("\x1b[33;1m^\x1b[0m", stderr);
        fputc('\n', stderr);
    }
}