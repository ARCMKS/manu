// ============================================================
// sema_ffi.c — FFI table construction and type translation.
//
// Two concerns:
//
//   Library table       A fixed-size array of declared
//                       libraries, keyed by alias. Built from
//                       the program's `extern lib` clauses
//                       and read by Pass A when resolving an
//                       `extern fn ... from <alias>`.
//
//   Type translation    Given a Prism type, decide FFI
//                       compatibility and produce the matching
//                       FFIType. Used by Pass A for parameter
//                       and return types, and by body analysis
//                       for variadic argument checks.
//
// The runtime side (FFILib, FFIFunc, the loaded handles) lives
// in vm.c. This file produces only the compile-time view.
// ============================================================
#include "../include/sema_internal.h"
#include "../include/types.h"
#include "../include/source_map.h"
#include "../include/diagnostics.h"
#include "../include/prism_ffi.h"
#include "../include/ast.h"
#include "../include/common.h"
#include <string.h>
#include <stdio.h>

// ---------- Private helpers ----------

// Derive a library's default alias from its path (basename
// up to the first '.'). Called only by build_library_table;
// remains static here.
//

// Derive a library's default alias from its path. Used when
// the source writes `extern lib "path"` without an `as`:
// the alias is the basename up to the first `.`.
//
// Examples:
//   "./libfoo.so"     -> "libfoo"
//   "/usr/lib/bar.a"  -> "bar"
//   "msvcrt.dll"      -> "msvcrt"
//
// Both `/` and `\\` are treated as path separators, so the
// same source works on Unix and Windows. The scan finds the
// last separator, then walks forward copying characters up
// to the first `.` (or the NUL).
//
// Truncation: if the basename (before the first `.`) is
// longer than cap-1, the alias is truncated silently. In
// practice alias names are short, so this is not a hazard.
static void derive_alias(const char* path, char* out, size_t cap) {
    // Find the last path separator. `base` ends up pointing
    // at the first character of the basename.
    const char* base = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    // Copy the basename up to the first '.'.
    size_t n = 0;
    while (base[n] && base[n] != '.' && n + 1 < cap) {
        out[n] = base[n];
        n++;
    }
    out[n] = '\0';
}

// ---------- Public entry points ----------

// Build the LibraryTable from the program's AST_EXTERN_LIB
// declarations.
//
// Path extraction: the token for the library path includes
// the surrounding quotes (the lexer emits the whole string
// literal including the `"..."`). This code strips them by
// reading the span [start+1, end-1). The `if (path_tok.span.end
// >= start+2)` guard catches the degenerate case of a
// 1-character span (which could only occur if the parser
// somehow produced a malformed library path token); in that
// case path_buf is empty.
//
// Alias: explicit if the source wrote `as alias`, otherwise
// derived from the path via derive_alias.
//
// Errors: overflow of the 16-entry table is reported and the
// offending declaration skipped. There is no other failure
// path; a path or alias longer than its buffer is truncated
// silently, which is the same policy as the AST constructors.
void build_library_table(LibraryTable* lt, AstNode* program,
                                const SourceMap* smap, DiagnosticBag* bag) {
    lt->count = 0;
    for (int i = 0; i < program->as.program.decls.count; i++) {
        AstNode* d = program->as.program.decls.items[i];
        if (d->kind != AST_EXTERN_LIB) continue;
        if (lt->count >= 16) {
            diag_error(bag, d->span,
                       "too many extern lib declarations (max 16)");
            continue;
        }
        Token path_tok  = d->as.using_decl.path[0];
        Token alias_tok = d->as.using_decl.module_alias;
        bool  has_alias = d->as.using_decl.has_module_alias;

        char path_buf[256];
        // The `>= start + 2` check requires the span to cover
        // at least the two quote characters. A malformed
        // token (start == end, or one byte) results in an
        // empty path rather than reading past the span.
        if (path_tok.span.end >= path_tok.span.start + 2) {
            Span clean = { path_tok.span.start + 1,
                           path_tok.span.end   - 1,
                           path_tok.span.file_id };
            smap_span_text(smap, clean, path_buf, sizeof path_buf);
        } else {
            path_buf[0] = '\0';
        }

        LibEntry* e = &lt->entries[lt->count];
        snprintf(e->path, sizeof e->path, "%s", path_buf);
        if (has_alias) {
            smap_span_text(smap, alias_tok.span, e->alias, sizeof e->alias);
        } else {
            derive_alias(path_buf, e->alias, sizeof e->alias);
        }
        lt->count++;
    }
}

// Look up a library by alias. Returns the index into
// LibraryTable.entries, or -1 if no match.
//
// The lookup is by string compare against the alias buffer;
// the caller passes the alias as a Token and this function
// extracts its text. That is why it needs the smap parameter
// (to read the token's source bytes).
int library_table_find(const LibraryTable* lt,
                              const SourceMap* smap, Token alias) {
    char buf[64];
    smap_span_text(smap, alias.span, buf, sizeof buf);
    for (int i = 0; i < lt->count; i++)
        if (strcmp(lt->entries[i].alias, buf) == 0) return i;
    return -1;
}

// Translate a Prism Type* to an FFIType code. Returns true on
// success (writing the code to *out) and false if the Prism
// type is not FFI-compatible.
//
// FFI-compatible types are exactly:
//   - void
//   - the eight integer types (i8..u64)
//   - f32, f64
//   - bool
//   - raw pointer (*T)
//
// Everything else — ref, rc, string, array, struct, enum — is
// rejected. This is the same set that is_ffi_variadic_ok
// accepts for variadic arguments, minus void (which cannot be
// a value).
bool ffi_kind_from_type(const Type* t, FFIType* out) {
    if (!t) return false;
    switch (t->kind) {
        case TY_VOID:   *out = FFI_VOID;   return true;
        case TY_I8:     *out = FFI_I8;     return true;
        case TY_I16:    *out = FFI_I16;    return true;
        case TY_I32:    *out = FFI_I32;    return true;
        case TY_I64:    *out = FFI_I64;    return true;
        case TY_U8:     *out = FFI_U8;     return true;
        case TY_U16:    *out = FFI_U16;    return true;
        case TY_U32:    *out = FFI_U32;    return true;
        case TY_U64:    *out = FFI_U64;    return true;
        case TY_F32:    *out = FFI_F32;    return true;
        case TY_F64:    *out = FFI_F64;    return true;
        case TY_BOOL:   *out = FFI_BOOL;   return true;
        case TY_RAWPTR: *out = FFI_PTR;    return true;
        default: return false;
    }
}

// Is `t` the type `Option<*T>` — i.e. an RC-wrapped,
// RC-managed enum named "Option#..." whose first variant has a
// single raw-pointer payload?
//
// This is the shape sema recognizes for nullable-pointer
// marshalling across the FFI boundary. The recognizer is
// string-based: it checks that the mangled name starts with
// the literal "Option#". A user could in principle define
// their own generic enum with a base name that mangles to
// that prefix; not currently a concern, but a known hazard.
//
// The check is deliberately narrow: variant count >= 1 (not
// == 2), payload count on variant 0 (Some) must be exactly
// 1, and that payload must be a raw pointer.
bool is_option_ptr_type(const Type* t) {
    if (!t || t->kind != TY_RC || !t->inner) return false;
    if (t->inner->kind != TY_ENUM) return false;
    TypeDef* td = t->inner->tdef;
    if (!td || !td->is_rc_managed) return false;
    if (strncmp(td->name_buf, "Option#", 7) != 0) return false;
    if (td->variant_count < 1) return false;
    if (td->variants[0].payload_count != 1) return false;
    return td->variants[0].payloads[0]
        && td->variants[0].payloads[0]->kind == TY_RAWPTR;
}

// Type check for a variadic argument. The FFI rules say that
// only types that survive the C variadic ABI unchanged may be
// passed: integers (all widths, signed or unsigned), bool
// (promoted to int by C, which libffi handles), and pointers.
//
// Floats are deliberately excluded: a vararg float must be
// passed as double via the C default-argument-promotion
// rules, and libffi does not apply that promotion for you.
// Prism's f32 is a distinct type, so allowing it here would
// produce silent ABI mismatches.
bool is_ffi_variadic_ok(const Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_I8: case TY_I16: case TY_I32: case TY_I64:
        case TY_U8: case TY_U16: case TY_U32: case TY_U64:
        case TY_BOOL:
        case TY_RAWPTR:
            return true;
        default:
            return false;
    }
}