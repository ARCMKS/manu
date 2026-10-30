// ============================================================
// codegen.c — AST to bytecode.
//
// Layout of a compiled program:
//
//   0:  CALL <main> 0
//   2:  HALT
//   3:  <body of fn #0>
//       <body of fn #1>
//       ...
//
// The prologue runs `main` and halts on return. Every function
// body ends with a fallback `PUSH 0; RET` so that a body without
// an explicit return still exits cleanly.
//
// Forward calls: a CALL instruction's target is emitted as a
// placeholder and recorded in `cg->patches`. After all bodies are
// laid out, each patch's target is filled in with the callee's
// final PC. This is what lets `a()` call `b()` defined later.
//
// Constant inlining: a variable reference whose `const_id` is
// non-negative is replaced by the constant's value. There is no
// runtime load. This is why `const` costs nothing.
//
// Print: three modes, all compile-time. The compiler inspects
// the argument list and emits the appropriate sequence of
// LOAD_CONST, PRINT <type>, and PRINT_NL. The VM's print handler
// never sees the format string.
//
// Stack discipline: every expression leaves exactly one value on
// the stack. Void expressions leave 0. This invariant lets
// codegen for statements be uniform: POP for expression
// statements, STORE/LOAD for assignments, no special-casing.
//
// Reading order for this file:
//   1. Compiler state (Cg, LoopFrame, PatchList).
//   2. Emission primitives (emit, emit_placeholder, patch_to_here).
//   3. Forward declarations of the recursion cycle.
//   4. desc_of — the Type* -> descriptor-index shim.
//   5. op_binary_code — AST BinOp -> VM OpBinary.
//   6. Print helpers (cg_print_value, cg_emit_literal_chunk,
//      string_has_placeholder, resolve_escapes,
//      cg_print_format_body, cg_builtin_print, cg_builtin_format).
//   7. Pattern codegen (cg_pattern_test).
//   8. Expression codegen (cg_expr).
//   9. Statement codegen (cg_block, cg_stmt).
//  10. Entry point (codegen_program).
//
// Codegen does not report errors on the "source program is
// malformed" axis (sema already did that). Its diagnostics are
// all "internal: ..." style, indicating a codegen bug or a
// backend limit (e.g. too many nested loops).
// ============================================================
#include "../include/codegen.h"
#include "../include/vm.h"
#include "../include/constant.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// ============================================================
// Compiler state
// ============================================================

// Hard caps. Exceeding either is a codegen error with a
// diagnostic, not a silent truncation.
//
//   MAX_LOOP_DEPTH     — nesting depth of loops in one
//                        function. Each loop pushes a
//                        LoopFrame.
//   MAX_BREAKS_PER_LOOP — number of break/continue
//                        statements in one loop. Each one
//                        records a jump patch in the
//                        frame.
#define MAX_LOOP_DEPTH 32
#define MAX_BREAKS_PER_LOOP 64

// One active loop. Recorded on entry, popped on exit, and
// consulted by break/continue to know where to jump and how
// many scopes to unwind.
//
// Field-by-field:
//   entry_scope_depth    — cg->scope_depth at the moment the
//                          loop started (before the loop
//                          scope was pushed). A break unwinds
//                          cg->scope_depth down to this value
//                          before jumping, by emitting
//                          OP_EXIT_SCOPE the right number of
//                          times.
//   continue_scope_depth — cg->scope_depth at the moment a
//                          continue should land (after the
//                          loop's own scope but before the
//                          body scope). A continue unwinds
//                          down to this value.
//   continue_patches[]   — one slot per continue statement,
//                          each patched to the loop's
//                          continue target.
//   continue_count       — number of valid entries in
//                          continue_patches[].
//   break_patches[]      — one slot per break statement,
//                          each patched to the loop's natural
//                          exit target.
//   break_count          — number of valid entries in
//                          break_patches[].
//
// Note that for while-loops the two scope depths are the same
// (both = the depth at entry); for for-range and for-in loops
// they differ because the loop introduces its own scope with
// the loop variable / cached values.
typedef struct {
    int entry_scope_depth;               // for break: unwind to this
    int continue_scope_depth;            // for continue: unwind to this
    int continue_patches[MAX_BREAKS_PER_LOOP];
    int continue_count;                 // PC to jump to for continue
    int break_patches[MAX_BREAKS_PER_LOOP];
    int break_count;
} LoopFrame;

// A growable list of chunk offsets to patch to a single target.
// Used by pattern tests: each conditional jump that should go to
// the "this arm failed" label appends its slot here.
//
// Grown by doubling (initial capacity 16). The xrealloc family
// is the compiler's standard allocation; a leak here would
// only happen if a patchlist is created and never freed, which
// would be a bug at the call site.
typedef struct {
    int* slots;
    int  count;
    int  capacity;
} PatchList;

// Zero a patchlist. Safe on uninitialized stack storage.
static void patchlist_init(PatchList* p) {
    p->slots    = NULL;
    p->count    = 0;
    p->capacity = 0;
}

// Free the backing array and reset. The list is safe to reuse
// (or re-init) after this call.
static void patchlist_free(PatchList* p) {
    free(p->slots);
    p->slots    = NULL;
    p->count    = 0;
    p->capacity = 0;
}

// Append a slot. Grows by doubling from 16.
static void patchlist_push(PatchList* p, int slot) {
    if (p->count >= p->capacity) {
        p->capacity = p->capacity ? p->capacity * 2 : 16;
        p->slots = (int*)xrealloc(p->slots,
            sizeof(int) * (size_t)p->capacity);
    }
    p->slots[p->count++] = slot;
}

typedef struct {
    int   slot;
    Type* type;
} RcLocal;

typedef struct {
    RcLocal items[MAX_LOCALS];
    int     count;
} RcScope;

// The codegen state for one compilation.
//
// Field-by-field:
//   chunk             — the target Chunk. All emission goes
//                       through emit / emit_placeholder /
//                       chunk_add_func on this pointer.
//   bag               — for backend-limit diagnostics.
//   smap              — for resolving source spans to
//                       line/col (see set_span) and for
//                       comparing identifiers.
//   consts            — the compile-time constant table,
//                       read for inlining.
//   structs           — the type table, read for field
//                       offsets and RC-managed flags.
//   current_line /    — updated by set_span before each
//   current_column      emitted instruction. The debug
//                       location recorded in the Chunk's
//                       parallel arrays.
//   scope_depth       — number of currently-active lexical
//                       scopes. Incremented by cg_block and
//                       by for-range/for-in bodies;
//                       decremented symmetrically. Used by
//                       break/continue to compute how many
//                       EXIT_SCOPEs to emit.
//   loops[] /         — the loop stack. Push on loop entry,
//   loop_count          pop on exit.
//   sema              — the SemaCtx from sema, needed to
//                       read funcs[] and the sema
//                       signatures.
//   codegen_of_sema[] — mapping from sema func index to
//                       codegen func index. Filled in
//                       codegen_program before any body is
//                       emitted.
//   in_string_build   — a refcount, not a boolean: nested
//                       BEGIN_STRING / END_STRING sequences
//                       are legal (e.g. a string built by a
//                       format() inside another format()).
//                       Currently the value is only
//                       incremented/decremented, never read;
//                       it exists as a hook for future
//                       emission decisions.
typedef struct {
    Chunk*            chunk;
    DiagnosticBag*    bag;
    const SourceMap*  smap;
    const ConstTable* consts;
    const TypeDefTable* structs;
    uint32_t          current_line;
    uint32_t          current_column;

    int               scope_depth;        //compile-time scope depth
    LoopFrame         loops[MAX_LOOP_DEPTH];
    int               loop_count;

    // RC-local tracking. One entry per active lexical scope;
    // pushed by cg_push_rc_scope and popped by the caller
    // after it emits the scope's drops. Used by cg_block,
    // for-range, for-in, and match.
    RcScope           rc_scopes[MAX_SCOPES];
    int               rc_scope_count;

    const SemaCtx* sema;
    int codegen_of_sema[MAX_FUNCS];

    int in_string_build ;
} Cg;


// Update the current debug location from the AST node's span.
// Called immediately before emitting each instruction that
// has a meaningful source correspondence.
//
// Failure to decode (a SPAN_NONE, or a span outside any
// known file) sets the location to 0/0, which the
// disassembler and the VM's error renderer both treat as
// "no location available".
//
// Note that this is a global side effect: the next emit
// uses whatever set_span last wrote. Codegen therefore
// calls set_span before each logical instruction, not
// once per function.
static void set_span(Cg* cg, const AstNode* n) {
    SpanDecoded d;
    if (smap_decode(cg->smap, n->span, &d)) {
        cg->current_line   = d.line;
        cg->current_column = d.column;
    } else {
        cg->current_line   = 0;
        cg->current_column = 0;
    }
}



// ============================================================
// Identifier comparison
//
// Two identifiers are equal iff their byte spans match in the
// same file. Bytes come from the SourceMap.
// ============================================================
// Name equality across files. Two identifiers are equal iff their
// byte sequences match, regardless of which file they were lexed
// from. This is what allows `helper` in a caller to match
// `helper` in the callee's defining file — the file_ids differ,
// the bytes don't.
//
// (The comment above this block does not correspond to any
// function immediately below; it is a leftover note. The
// actual cross-file comparison function is
// smap_tokens_equal in source_map.c.)

// Emit one instruction word with the current debug location.
// Every emitted word (opcode or operand) gets the same
// (line, col) pair; that is what the VM wants when it
// renders a runtime error.
static void emit(Cg* cg, U64 w) {
    chunk_emit(cg->chunk, w, cg->current_line, cg->current_column);
}

// Emit an opcode followed by a zero-valued placeholder
// operand, and return the slot index of the placeholder.
// The caller patches it later with patch_to_here.
//
// Used for jumps (JMP, JMP_IF_FALSE) whose target is not
// yet known. Also used for the after-body JMP in a match arm
// (jumping to the end of the match).
//
// The opcode and placeholder are emitted as two separate
// words; the returned slot is the second one. Because
// chunk_emit returns the index it just wrote, the second
// call's return value is the placeholder's slot.
static int emit_placeholder(Cg* cg, OpCode op) {
    emit(cg, (U64)op);
    return chunk_emit(cg->chunk, 0,
                      cg->current_line, cg->current_column);
}

// Patch a previously-emitted placeholder to point at the
// *current* end of the chunk. Called after emitting the code
// the jump should land on.
//
// This is the counterpart of emit_placeholder: the placeholder
// holds 0 until patched; patch_to_here overwrites it with the
// PC that immediately follows the last-emitted instruction.
static void patch_to_here(Cg* cg, int slot) {
    chunk_patch(cg->chunk, slot, (U64)cg->chunk->count);
}


// ============================================================
// Forward declarations
//
// These break the recursion cycle between expression codegen
// (cg_expr), statement codegen (cg_stmt), and the print helpers
// (which call cg_expr for their arguments).
// ============================================================
static void cg_expr(Cg* cg, AstNode* n);
static void cg_stmt(Cg* cg, AstNode* n);
static void cg_pattern_test(Cg* cg, AstNode* pat, int cur_slot,
                            PatchList* fails);

// Intern a Type* into the chunk's TypeTable and return the
// descriptor index. That index is what parameterized
// opcodes (OP_PRINT, OP_BINARY, OP_INDEX_LOAD, ...) carry
// as their operand.
//
// NULL types are a codegen bug (sema should always set
// `type` on every expression); the warning is defensive. The
// interner maps NULL to a default (TD_I64), so the resulting
// descriptor is at least usable.
static uint32_t desc_of(Cg* cg, Type* t) {
    if (!t) {
        diag_warning(cg->bag, SPAN_NONE,
                   "internal: codegen saw a NULL type");
    }
    return typetable_intern(&cg->chunk->types, t);
}

// Map a value's Type* to the memory access width used by
// LOAD_INDIRECT / STORE_INDIRECT. For any type that occupies
// a full 8-byte VM slot (structs, enums, strings), MEM_U64 is
// the correct width and matches the pre-Phase-2 behavior.
static MemKind memkind_of(Type* t) {
    if (!t) return MEM_U64;
    switch (t->kind) {
        case TY_I8:  return MEM_I8;
        case TY_I16: return MEM_I16;
        case TY_I32: return MEM_I32;
        case TY_I64: return MEM_I64;
        case TY_U8:  return MEM_U8;
        case TY_U16: return MEM_U16;
        case TY_U32: return MEM_U32;
        case TY_U64: return MEM_U64;
        case TY_F32: return MEM_F32;
        case TY_F64: return MEM_F64;
        case TY_RAWPTR: return MEM_PTR;
        default: return MEM_U64;
    }
}

// Map an AST BinOp to the VM's OpBinary enum.
//
// The two enums have the same members in a different order
// (the VM's is ADD, SUB, MUL, DIV, EQ, NE, LT, LE, GT, GE;
// the AST's is the same, so this is a straight switch). The
// default case (OPB_ADD) is unreachable for a valid BinOp;
// it exists only so the compiler does not warn about
// missing returns.
static U32 op_binary_code(BinOp op) {
    switch (op) {
        case BIN_ADD: return OPB_ADD;
        case BIN_SUB: return OPB_SUB;
        case BIN_MUL: return OPB_MUL;
        case BIN_DIV: return OPB_DIV;
        case BIN_EQ:  return OPB_EQ;
        case BIN_NE:  return OPB_NE;
        case BIN_LT:  return OPB_LT;
        case BIN_LE:  return OPB_LE;
        case BIN_GT:  return OPB_GT;
        case BIN_GE:  return OPB_GE;
        case BIN_AND: return OPB_ADD;
        case BIN_OR:  return OPB_OR;
    }
    return OPB_ADD;
}


static void cg_push_rc_scope(Cg* cg) {
    cg->rc_scopes[cg->rc_scope_count].count = 0;
    cg->rc_scope_count++;
}

static void cg_register_rc_local(Cg* cg, int slot, Type* type) {
    if (cg->rc_scope_count == 0) return;
    RcScope* s = &cg->rc_scopes[cg->rc_scope_count - 1];
    if (s->count < MAX_LOCALS) {
        s->items[s->count].slot = slot;
        s->items[s->count].type = type;
        s->count++;
    }
}

// Drop every RC local in one scope. Does not pop the scope.
static void cg_emit_scope_drops(Cg* cg, RcScope* s, Span where) {
    for (int i = 0; i < s->count; i++) {
        set_span(cg, (AstNode*)NULL);   // caller sets span before
        (void)where;
        emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)s->items[i].slot);
        emit(cg, OP_RC_DEC);
        emit(cg, (U64)desc_of(cg, s->items[i].type));
    }
}

// Emit drops for every active RC local except the one at
// `exclude_slot` (or all when exclude_slot is -1). Used by
// AST_RETURN and AST_TRY's failure path, where the value being
// returned may itself be a local whose reference is being
// transferred to the caller rather than dropped here.
static void cg_emit_all_rc_drops_except(Cg* cg, int exclude_slot) {
    for (int s = cg->rc_scope_count - 1; s >= 0; s--) {
        RcScope* sc = &cg->rc_scopes[s];
        for (int i = 0; i < sc->count; i++) {
            if (sc->items[i].slot == exclude_slot) continue;
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)sc->items[i].slot);
            emit(cg, OP_RC_DEC);
            emit(cg, (U64)desc_of(cg, sc->items[i].type));
        }
    }
}

// Drop every active RC local. Used by callers that do not
// transfer any local's ownership outward.
static void cg_emit_all_rc_drops(Cg* cg) {
    cg_emit_all_rc_drops_except(cg, -1);
}

// If `n` is a bare reference to an owning local — an AST_VAR
// whose resolution points at an RC-owning slot — return that
// slot. Otherwise return -1. Used to compute the exclude slot
// for ownership-transferring exits.
static int cg_owning_local_slot(const AstNode* n) {
    if (n && n->kind == AST_VAR
        && n->local_index >= 0
        && n->holds_rc) {
        return n->local_index;
    }
    return -1;
}

// Drop the N innermost scopes' RC locals. Does NOT modify
// the compile-time scope stack (the runtime EXIT_SCOPEs are
// emitted by the caller). Used by break/continue.
static void cg_emit_n_scope_drops(Cg* cg, int n) {
    for (int i = 0; i < n; i++) {
        int idx = cg->rc_scope_count - 1 - i;
        if (idx < 0) return;
        RcScope* sc = &cg->rc_scopes[idx];
        for (int j = 0; j < sc->count; j++) {
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)sc->items[j].slot);
            emit(cg, OP_RC_DEC);
            emit(cg, (U64)desc_of(cg, sc->items[j].type));
        }
    }
}

// ============================================================
// Print helpers
//
// Print is the most complicated builtin because it has three
// modes:
//
//   1. Empty: `print()`. Emits a single newline.
//   2. Format: `print("x = {}", x)`. The first argument is a
//      string literal containing {} placeholders; each {} is
//      replaced (in source order) by the corresponding
//      subsequent argument.
//   3. Multi-arg: `print(a, b, c)`. Emits each argument
//      separated by a single space, then a newline.
//
// The format-vs-multi-arg decision is made at compile time by
// scanning the first argument's string literal for a `{}`.
// The VM never sees the format string; it just executes a
// sequence of LOAD_CONST and PRINT instructions.
//
// All three modes share a BEGIN_STRING / END_STRING pair that
// constructs the full line as a fresh RC string, which is
// then passed to the NATIVE_STDOUT_WRITE native via OP_CCALL.
// This is what makes print a single syscall (or fwrite call)
// per line rather than one per fragment.
// ============================================================

// Emit OP_PRINT <type> for the value on top of the stack.
// Assumes the value is already there.
//
// `value_expr` is used only for its span (via set_span) and
// its type (via desc_of). The value itself is already on the
// VM stack; this function does not push it.
static void cg_print_value(Cg* cg, AstNode* value_expr) {
    set_span(cg, value_expr);
    Type* t = value_expr->type;

    // Auto-unwrap one level of RC for display. Printing an
    // `Option<T>` or `Result<T, E>` should show the variant,
    // not the RC handle; printing an `rc<i64>` should show
    // the integer. The value is already on the stack, so
    // only a deref step is needed.
    if (t && t->kind == TY_RC && t->inner) {
        Type* inner = t->inner;
        bool compound = (inner->kind == TY_STRUCT
                      || inner->kind == TY_ENUM
                      || inner->kind == TY_ARRAY
                      || inner->kind == TY_STRING);
        if (!compound) {
            // Scalar payload: read the value out of the
            // RC block. The stack currently holds the RC
            // handle (a pointer to the payload); the
            // payload is a single 8-byte slot.
            emit(cg, OP_LOAD_INDIRECT);
            emit(cg, 0);
            emit(cg, (U64)memkind_of(inner));
        }
        // Compound payload: the RC handle already points
        // at the payload. Nothing to emit.
        emit(cg, OP_PUSH); emit(cg, (U64)desc_of(cg, inner));
    } else {
        emit(cg, OP_PUSH); emit(cg, (U64)desc_of(cg, t));
    }
    emit(cg, OP_CCALL); emit(cg, NATIVE_PRINT_VALUE);
}

// Emit code that prints a string constant of `len` bytes with no
// trailing newline.
//
// The constant is added to the chunk's pool, and the
// LOAD_CONST + PRINT pair pushes it and prints it. The type
// descriptor is ty_string, which tells the VM's print
// handler to treat the raw value as a pointer to an
// RC-shaped block with [u64 length][bytes...].
static void cg_print_string_const(Cg* cg, const char* bytes, uint32_t len) {
    int idx = pool_add_string(&cg->chunk->pool, bytes, len);
    emit(cg, OP_LOAD_CONST); emit(cg, (U64)idx);
    emit(cg, OP_PUSH);       emit(cg, (U64)desc_of(cg, ty_string()));
    emit(cg, OP_CCALL);      emit(cg, NATIVE_PRINT_VALUE);
}

// Emit the literal chunk [from, to) of a format string, resolving
// escapes and collapsing {{ and }}. Empty chunks emit nothing.
//
// `src` is the raw source of the whole string literal
// (including quotes, which the caller has already skipped
// by passing from > start). The output is assembled into a
// stack buffer (1024 bytes) and emitted as a single pool
// string.
//
// Escape handling:
//   \n -> newline; \t -> tab; \r -> CR;
//   \\ -> backslash; \" -> quote.
//   Any other backslash-X is passed through as X (the
//   backslash is dropped).
//
// Brace collapsing:
//   `{{` -> `{`, `}}` -> `}`. These are the standard
//   format-string escapes for literal braces.
//
// A chunk that ends up empty (because the source was all
// `{{` or all whitespace that got consumed by an escape)
// emits nothing.
//
// Truncation: if the assembled chunk exceeds 1023 bytes, it
// is truncated silently. In practice chunks are short
// (format strings are rarely more than a few hundred
// characters), so this is not a live hazard.
static void cg_emit_literal_chunk(Cg* cg, AstNode* n,
                                  const char* src, uint32_t from, uint32_t to) {
    char buf[1024];
    int  blen = 0;
    for (uint32_t i = from; i < to && blen < 1023; i++) {
        char c = src[i];
        if (c == '\\' && i + 1 < to) {
            // Escape sequence.
            char nx = src[++i];
            switch (nx) {
                case 'n':  buf[blen++] = '\n'; break;
                case 't':  buf[blen++] = '\t'; break;
                case 'r':  buf[blen++] = '\r'; break;
                case '\\': buf[blen++] = '\\'; break;
                case '"':  buf[blen++] = '"';  break;
                default:   buf[blen++] = nx;   break;
            }
        } else if (c == '{' && i + 1 < to && src[i + 1] == '{') {
            // `{{` -> literal `{`.
            buf[blen++] = '{'; i++;
        } else if (c == '}' && i + 1 < to && src[i + 1] == '}') {
            // `}}` -> literal `}`.
            buf[blen++] = '}'; i++;
        } else {
            buf[blen++] = c;
        }
    }
    if (blen == 0) return;
    set_span(cg, n);
    cg_print_string_const(cg, buf, (uint32_t)blen);
}

// Does a string literal contain at least one non-escaped "{}"?
//
// This is the compile-time test that decides whether the
// print call is in "format mode" (uses {} placeholders) or
// "multi-arg mode" (space-separated args).
//
// The scan skips `{{` and `}}` (they are literal braces,
// not placeholders) and returns true at the first bare `{}`.
// A string with no `{}` returns false.
//
// The scan walks the *raw source* between the quotes, not a
// decoded version: escapes are not resolved here, so a
// `"\{"` (if such an escape existed) would be seen as
// backslash-brace and the brace would not count as a
// placeholder. That is intentional; the format-string
// grammar is the raw source's grammar.
static bool string_has_placeholder(Cg* cg, AstNode* lit) {
    Token raw = lit->as.string_lit.content;
    const char* src = smap_file_content(cg->smap, raw.span.file_id);
    if (!src) return false;
    uint32_t cs = raw.span.start + 1;   // skip opening quote
    uint32_t ce = raw.span.end   - 1;   // stop before closing quote
    for (uint32_t i = cs; i + 1 < ce; ) {
        // Skip literal braces. Note that this is a
        // two-byte skip so `{{{}}}` is handled: the first
        // `{{` is a literal, then `{}` is a placeholder,
        // then the final `}}` is a literal.
        if (src[i] == '{' && src[i + 1] == '{') { i += 2; continue; }
        if (src[i] == '}' && src[i + 1] == '}') { i += 2; continue; }
        if (src[i] == '{' && src[i + 1] == '}') return true;
        i++;
    }
    return false;
}

// Resolve backslash escapes in `src[from..to)` into `out`.
// Returns the number of bytes written. Does not collapse
// `{{` / `}}` — that is a format-string concern, handled
// separately by cg_emit_literal_chunk.
//
// This is a lower-level escape resolver than the one inside
// cg_emit_literal_chunk; the two share the same escape
// table but differ in whether they also handle brace
// collapsing. Used for plain (non-format) string literals
// and for cstrings, where braces have no special meaning.
static uint32_t resolve_escapes(const char* src, uint32_t from, uint32_t to,
                                char* out, uint32_t cap) {
    uint32_t blen = 0;
    for (uint32_t i = from; i < to && blen < cap; i++) {
        char c = src[i];
        if (c == '\\' && i + 1 < to) {
            char nx = src[++i];
            switch (nx) {
                case 'n':  out[blen++] = '\n'; break;
                case 't':  out[blen++] = '\t'; break;
                case 'r':  out[blen++] = '\r'; break;
                case '\\': out[blen++] = '\\'; break;
                case '"':  out[blen++] = '"';  break;
                default:   out[blen++] = nx;   break;
            }
        } else {
            out[blen++] = c;
        }
    }
    return blen;
}

// Format mode: split the first-argument string into chunks and
// interleave arguments at each {}.
//
// The algorithm walks the raw source between the quotes,
// alternating between "literal chunks" (accumulated and
// emitted as a single pool string) and "placeholders" (each
// consuming one argument from the argument list).
//
// Argument indexing: `arg_i` starts at 1 (skipping the format
// string itself, which is args[0]). Each placeholder
// increments it. If a placeholder occurs when no args are
// left, an error is reported and the function returns early;
// if the loop ends with unused args, that is also an error.
//
// The error messages count placeholders and arguments for the
// user; note the `nargs - 1` because nargs includes the
// format string.
//
// Escapes and `{{` / `}}` are handled inside
// cg_emit_literal_chunk; the scan here only detects the
// placeholder boundaries.
static void cg_print_format_body(Cg* cg, AstNode* n) {
    Token raw = n->as.call.args.items[0]->as.string_lit.content;
    const char* src = smap_file_content(cg->smap, raw.span.file_id);
    uint32_t cs = raw.span.start + 1;
    uint32_t ce = raw.span.end   - 1;

    int nargs  = n->as.call.args.count;
    int arg_i  = 1;
    int places = 0;

    uint32_t i = cs;
    uint32_t chunk_start = cs;

    while (i < ce) {
        // Skip over `{{` and `}}` so they are not
        // mistaken for placeholders.
        if (i + 1 < ce && src[i] == '{' && src[i + 1] == '{') { i += 2; continue; }
        if (i + 1 < ce && src[i] == '}' && src[i + 1] == '}') { i += 2; continue; }

        // Bare `{}`: emit the accumulated literal chunk,
        // then the next argument.
        if (i + 1 < ce && src[i] == '{' && src[i + 1] == '}') {
            cg_emit_literal_chunk(cg, n, src, chunk_start, i);
            places++;

            if (arg_i >= nargs) {
                diag_error(cg->bag, n->span,
                    "print: format string has %d placeholder(s) but "
                    "only %d argument(s)", places, nargs - 1);
                return;
            }

            AstNode* a = n->as.call.args.items[arg_i++];
            cg_expr(cg, a);  // forward-declared below
            cg_print_value(cg, a);

            i += 2;
            chunk_start = i;
            continue;
        }
        i++;
    }

    // Emit the final literal chunk (after the last
    // placeholder, or the whole string if there were none).
    cg_emit_literal_chunk(cg, n, src, chunk_start, ce);

    // Unused arguments.
    if (arg_i != nargs)
        diag_error(cg->bag, n->span,
            "print: format string has %d placeholder(s) but %d argument(s)",
            places, nargs - 1);
}

// Emit a single newline. Used by print (in all modes) to
// terminate the line.
static void cg_emit_newline(Cg* cg) {
    int idx = pool_add_string(&cg->chunk->pool, "\n", 1);
    emit(cg, OP_LOAD_CONST); emit(cg, (U64)idx);
    emit(cg, OP_PUSH);       emit(cg, (U64)desc_of(cg, ty_string()));
    emit(cg, OP_CCALL);      emit(cg, NATIVE_PRINT_VALUE);
}

// Multi-argument / empty print.
//
// The dispatcher: if the first arg is a string literal with
// at least one `{}`, use format mode. Otherwise emit each
// argument separated by a single space.
//
// The `nargs == 0` early return handles the empty-print
// case: the caller (cg_builtin_print) emits the newline.
static void cg_emit_render_body(Cg* cg, AstNode* n) {
    int nargs = n->as.call.args.count;
    if (nargs == 0) return;

    AstNode* first = n->as.call.args.items[0];
    if (first->kind == AST_STRING && string_has_placeholder(cg, first)) {
        cg_print_format_body(cg, n);    // see below
    } else {
        for (int i = 0; i < nargs; i++) {
            if (i > 0) {
                set_span(cg, n);
                cg_print_string_const(cg, " ", 1);
            }
            AstNode* a = n->as.call.args.items[i];
            cg_expr(cg, a);
            cg_print_value(cg, a);
        }
    }
}

// `format(...)` builtin: same body as print, but returns the
// constructed string instead of writing it to stdout.
//
// The BEGIN_STRING / END_STRING pair pushes the string onto
// the stack (which print then consumes via CCALL). For
// format, the string is the result: END_STRING leaves it on
// the stack, and the caller's expression machinery treats it
// as the return value.
//
// `in_string_build` is a refcount, incremented around the
// nested render. It is currently never read; the increment
// is a hook for future emission decisions (e.g. if a nested
// print inside a format ever needs to know it is being
// captured rather than written).
static void cg_builtin_format(Cg* cg, AstNode* n) {
    set_span(cg, n);
    emit(cg, OP_CCALL); emit(cg, NATIVE_STR_BEGIN);

    cg->in_string_build++;
    cg_emit_render_body(cg, n);
    cg->in_string_build--;

    set_span(cg, n);
    emit(cg, OP_CCALL); emit(cg, NATIVE_STR_END);   // pushes the string
}

// `print(...)` builtin: construct the line, then write it.
//
// Sequence:
//   BEGIN_STRING           — start capturing output
//   <render body>          — emit args / literals
//   newline                — emit trailing \n
//   END_STRING             — push the whole line as a string
//   CCALL stdout_write     — write it
//   PUSH 0                 — return value is void
//
// The final PUSH 0 is what preserves the "every expression
// leaves one value" invariant. Without it, a print used as a
// statement would leave the stack unbalanced.
static void cg_builtin_print(Cg* cg, AstNode* n) {
    set_span(cg, n);
    emit(cg, OP_CCALL); emit(cg, NATIVE_STR_BEGIN);

    cg->in_string_build++;
    cg_emit_render_body(cg, n);
    set_span(cg, n);
    cg_emit_newline(cg);
    cg->in_string_build--;

    set_span(cg, n);
    emit(cg, OP_CCALL); emit(cg, NATIVE_STR_END);          // string with newline on stack
    emit(cg, OP_CCALL); emit(cg, NATIVE_STDOUT_WRITE);
    emit(cg, OP_PUSH);  emit(cg, 0);                        // void result
}

// ============================================================
// Pattern test codegen
//
// cg_pattern_test emits the test for a single pattern against
// the value held in local `cur_slot`. If the test fails, it
// jumps to the label at the end of the arm (which starts the
// next arm). The failure jumps are recorded in `fails`; the
// caller patches them all to the same target.
//
// Per pattern kind:
//
//   WILDCARD    — always matches; nothing emitted.
//
//   BIND        — copies cur_slot to the binding's slot.
//                 (There is no "test" here; a bind is
//                 unconditional.) Note that the copy is a
//                 raw 64-bit move; the RC system is not
//                 involved, so both slots now reference the
//                 same block. The refcounting invariant is
//                 that the scrutinee owns the reference and
//                 the bind is a borrow, so no INC is needed.
//
//   INT         — load cur_slot, push the pattern literal,
//                 compare with EQ, and JMP_IF_FALSE on
//                 failure. The type descriptor passed to
//                 BINARY is ty_i64 because all integers are
//                 stored as 64-bit slots regardless of their
//                 declared width; the VM's BINARY truncates
//                 according to the descriptor, but for a
//                 comparison, no truncation is needed.
//
//   BOOL        — same shape as INT, with 0/1 as the literal.
//                 The descriptor is again i64 for the same
//                 reason (bool is stored as a full slot).
//
//   STRING      — resolve the pattern string literal's
//                 escapes, pool it, load it, compare with EQ.
//                 The descriptor is ty_string.
//
//   VARIANT     — the complex case:
//                 a. Load the scrutinee's payload pointer,
//                    read the tag at offset 0, compare with
//                    the pattern's tag, jump on failure.
//                 b. For each subpattern, load the payload
//                    at offset 8 + i*8 into a temporary
//                    slot, then recurse cg_pattern_test
//                    against that slot. The temporary slot
//                    was assigned by analyze_pattern.
//
//                 Note that step (a) does not check the
//                 scrutinee's *type* against the pattern's
//                 enum; sema already did. At runtime, any
//                 value that reaches here is a well-typed
//                 enum payload block, so a simple tag read
//                 is correct.
//
// The "load scrutinee" idiom:
//   OP_LOAD_LOCAL <cur_slot>       — push the local
//   OP_LOAD_INDIRECT 0             — load the tag word
// This works because every enum value is represented as a
// pointer to a payload block whose first 8 bytes are the
// tag. For an AST_FIELD unit-variant construction, the same
// pointer shape is used.
//
// Note that cg_pattern_test is called from within a match
// arm's pattern-test sequence, and it recurses for nested
// variant patterns. The `fails` list accumulates jumps
// across the whole arm, not just the current pattern level.
// ============================================================
static void cg_pattern_test(Cg* cg, AstNode* pat, int cur_slot,
                            PatchList* fails) {
    switch (pat->kind) {
        case AST_PAT_WILDCARD:
            // Always matches. No code emitted.
            break;

        case AST_PAT_BIND:
            // Unconditional: copy the scrutinee's slot to the
            // binding's slot.
            set_span(cg, pat);
            emit(cg, OP_LOAD_LOCAL);  emit(cg, (U64)cur_slot);
            emit(cg, OP_STORE_LOCAL); emit(cg, (U64)pat->as.pat_bind.slot);
            break;

        case AST_PAT_INT:
            // Equal-or-fail.
            set_span(cg, pat);
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)cur_slot);
            emit(cg, OP_PUSH);       emit(cg, (U64)pat->as.pat_int.value);
            emit(cg, OP_BINARY);     emit(cg, OPB_EQ);
            emit(cg, (U64)desc_of(cg, ty_i64()));
            {
                // On failure, jump to the arm's fail target.
                // The slot is recorded; the caller patches it.
                int slot = emit_placeholder(cg, OP_JMP_IF_FALSE);
                patchlist_push(fails, slot);
            }
            break;

        case AST_PAT_BOOL:
            // Same shape as INT. The comment on the ty_i64
            // descriptor applies: bool is stored as 0/1 in a
            // full slot.
            set_span(cg, pat);
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)cur_slot);
            emit(cg, OP_PUSH);
            emit(cg, pat->as.pat_bool.value ? 1 : 0);
            emit(cg, OP_BINARY);     emit(cg, OPB_EQ);
            // ty_i64() instead of ty_bool() since bool is already stored as 0/1 U64
            emit(cg, (U64)desc_of(cg, ty_i64()));
            {
                int slot = emit_placeholder(cg, OP_JMP_IF_FALSE);
                patchlist_push(fails, slot);
            }
            break;

        case AST_PAT_STRING: {
            // Resolve escapes, pool the result, compare.
            set_span(cg, pat);
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)cur_slot);
            Token raw = pat->as.pat_string.content;
            const char* src = smap_file_content(cg->smap, raw.span.file_id);
            // If the source is unavailable (should not
            // happen), skip the test entirely. That is
            // safer than emitting a half-formed test.
            if (!src) break;
            char buf[1024];
            uint32_t blen = resolve_escapes(src,
                                            raw.span.start + 1,
                                            raw.span.end - 1,
                                            buf, sizeof buf);
            int idx = pool_add_string(&cg->chunk->pool, buf, blen);
            emit(cg, OP_LOAD_CONST); emit(cg, (U64)idx);
            emit(cg, OP_BINARY);     emit(cg, OPB_EQ);
            emit(cg, (U64)desc_of(cg, ty_string()));
            {
                int slot = emit_placeholder(cg, OP_JMP_IF_FALSE);
                patchlist_push(fails, slot);
            }
            break;
        }

        case AST_PAT_VARIANT: {
            // Step 1: load the scrutinee's tag and compare.
            set_span(cg, pat);
            emit(cg, OP_LOAD_LOCAL);  emit(cg, (U64)cur_slot);
            emit(cg, OP_LOAD_INDIRECT); emit(cg, 0); emit(cg, MEM_U64);
            emit(cg, OP_PUSH);        emit(cg, (U64)pat->as.pat_variant.tag);
            emit(cg, OP_BINARY);      emit(cg, OPB_EQ);
            emit(cg, (U64)desc_of(cg, ty_i64()));
            {
                int slot = emit_placeholder(cg, OP_JMP_IF_FALSE);
                patchlist_push(fails, slot);
            }

            // Step 2: destructure each payload and recurse.
            // The `sub_count` was set by analyze_pattern to
            // the number of subpatterns (equal to the
            // variant's payload count).
            for (int i = 0; i < pat->as.pat_variant.sub_count; i++) {
                AstNode* sub = pat->as.pat_variant.subpatterns.items[i];
                int off = 8 + i * 8;   // payload i lives at offset 8 + i*8
                int tmp = pat->as.pat_variant.tmp_slots[i];

                // Load the i-th payload into its temporary
                // slot, then run the subpattern test
                // against that slot.
                set_span(cg, pat);
                emit(cg, OP_LOAD_LOCAL);    emit(cg, (U64)cur_slot);
                emit(cg, OP_LOAD_INDIRECT); emit(cg, (U64)off);
                emit(cg, MEM_U64);
                emit(cg, OP_STORE_LOCAL);   emit(cg, (U64)tmp);

                cg_pattern_test(cg, sub, tmp, fails);
            }
            break;
        }

        default:
            // Unknown pattern kind: a bug in sema (which
            // should have rejected the pattern) or a new
            // pattern kind added without codegen.
            diag_error(cg->bag, pat->span,
                "internal: unknown pattern kind");
    }
}

// ============================================================
// Expression codegen
//
// cg_expr emits code that leaves exactly one value on the
// VM stack (or zero, for a void expression — but every
// expression that reaches here is expected to leave one).
// The value is the expression's result.
//
// The contract in full:
//   - Every cg_expr call is preceded by set_span or does its
//     own set_span before emitting, so the instruction's
//     debug location is the expression's source location.
//   - The emitted code uses the `type` and resolution fields
//     that sema filled in. cg_expr never consults `type_expr`
//     or any syntactic field that would require re-resolving.
//   - Sub-expression evaluation is left-to-right. For binary
//     ops that is important because the codegen reads them
//     off the stack in that order.
//
// The shape per expression kind matches the codegen's
// "operate on values, not names" rule: by the time cg_expr
// runs, all names have been resolved to slots, indices, or
// table entries.
// ============================================================
static void cg_expr(Cg* cg, AstNode* n) {
    switch (n->kind) {
        // ----- Literals -----
        //
        // Integer literals: OP_PUSH <value>. The push is
        // unconditional; small values could be encoded as a
        // dedicated small-int opcode, but the current ISA
        // does not have one.
        case AST_INT:
            set_span(cg, n);
            emit(cg, OP_PUSH); emit(cg, (U64)n->as.int_lit.value);
            break;
        // Float literals: the bit pattern is stored in the
        // chunk's constant pool (as an F64), and OP_LOAD_CONST
        // pushes the raw 64-bit value. The VM reconstitutes
        // the double from the bits at load time.
        case AST_FLOAT: {
            set_span(cg, n);
            // Emit the literal at the width of its resolved
            // type. An `f64` literal (the default when no
            // context applied) goes through the constant pool
            // as a double; an `f32` literal is narrowed at
            // compile time and pushed as its 32-bit pattern
            // in the low half of the slot, which is what
            // MEM_F32 and the f32 case of OP_BINARY expect.
            if (n->type && n->type->kind == TY_F32) {
                float fv = (float)n->as.float_lit.value;
                uint32_t bits;
                memcpy(&bits, &fv, 4);
                emit(cg, OP_PUSH); emit(cg, (U64)bits);
            } else {
                int idx = pool_add_f64(&cg->chunk->pool,
                                       n->as.float_lit.value);
                emit(cg, OP_LOAD_CONST); emit(cg, (U64)idx);
            }
            break;
        }
        // Booleans are 0/1 in a full slot.
        case AST_BOOL:
            set_span(cg, n);
            emit(cg, OP_PUSH); emit(cg, n->as.bool_lit.value ? 1 : 0);
            break;
        // Prism strings: resolve escapes and push the pool
        // index. The pool entry is a CONST_STRING; the VM
        // materializes it into the const arena at VM init
        // (see materialize_consts in vm.c) with the
        // "never freed" sentinel refcount.
        //
        // The source bytes between the quotes are what get
        // copied; the quote characters themselves are
        // excluded by the +1 / -1 on start/end.
        case AST_STRING: {
            set_span(cg, n);
            Token raw = n->as.string_lit.content;
            const char* src = smap_file_content(cg->smap, raw.span.file_id);
            if (!src) break;

            char buf[1024];
            uint32_t blen = resolve_escapes(src,
                                            raw.span.start + 1,
                                            raw.span.end   - 1,
                                            buf, sizeof buf);
            int idx = pool_add_string(&cg->chunk->pool, buf, blen);
            emit(cg, OP_LOAD_CONST); emit(cg, (U64)idx);
            break;
        }

        // C strings (`c"..."`). Same as AST_STRING, except:
        //   - the leading `c` prefix is skipped (+2 instead
        //     of +1 on start),
        //   - the pool entry is a CONST_CSTRING, which the
        //     VM materializes as a NUL-terminated byte
        //     buffer (no RC header),
        //   - no final NUL is added by the pool; the VM
        //     adds it during materialization.
        case AST_CSTRING: {
            set_span(cg, n);
            Token raw = n->as.string_lit.content;
            const char* src = smap_file_content(cg->smap, raw.span.file_id);
            if (!src) break;
            char buf[1024];
            uint32_t blen = resolve_escapes(src,
                                            raw.span.start + 2,
                                            raw.span.end   - 1,
                                            buf, sizeof buf);
            int idx = pool_add_cstring(&cg->chunk->pool, buf, blen);
            emit(cg, OP_LOAD_CONST); emit(cg, (U64)idx);
            break;
        }
        // Array literal: evaluate each element (in source
        // order), then OP_NEW_ARRAY with the element count.
        // The VM pops N values, allocates an array block in
        // the arena, and pushes the block pointer.
        //
        // Note that the elements are pushed in source order
        // but popped in reverse by the VM's NEW_ARRAY; the
        // VM writes them into the block in the right order
        // by counting down from N. See the OP_NEW_ARRAY case
        // in vm.c.
        case AST_ARRAY_LIT: {
            for (int i = 0; i < n->as.array_lit.elements.count; i++)
                cg_expr(cg, n->as.array_lit.elements.items[i]);
            set_span(cg, n);
            emit(cg, OP_NEW_ARRAY);
            emit(cg, (U64)n->as.array_lit.elements.count);
            break;
        }

        // Array / string indexing: `target[index]`.
        // Evaluation order: target first, then index. Then
        // OP_INDEX_LOAD with the *container's* type
        // descriptor (unwrapped from rc<> if the target is
        // RC-managed).
        //
        // The container descriptor tells the VM whether to
        // do a byte read (TD_STRING) or a slot read
        // (TD_ARRAY). The two cases are distinguished by
        // the desc's kind at runtime.
        case AST_INDEX: {
            AstNode* tgt = n->as.index.target;
            cg_expr(cg, tgt);
            cg_expr(cg, n->as.index.index);
            set_span(cg, n);
            emit(cg, OP_INDEX_LOAD);

            // Unwrap rc<T> so the descriptor is the
            // underlying array/string, not the RC wrapper.
            // The VM's INDEX_LOAD does not know about RC;
            // it wants the container type.
            Type* ct = tgt->type;
            if (ct && ct->kind == TY_RC) ct = ct->inner;
            emit(cg, (U64)desc_of(cg, ct));
            break;
        }
        // Struct literal: `Name { f1: v1, f2: v2, ... }`.
        //
        // The codegen walks the *TypeDef's* field order,
        // not the source order of the initializer list. For
        // each field in the TypeDef, it finds the matching
        // initializer entry and emits its value expression.
        // That produces a canonical field order (the TypeDef
        // order) which is what the runtime layout expects.
        //
        // Fields are laid out at offsets 8, 16, 24, ...
        // (see resolve_struct_fields in sema.c). OP_NEW_ARRAY
        // builds the payload block with tag slot at offset 0
        // (unused for structs) and fields from offset 8.
        //
        // The inner loop is a linear search of the
        // initializer list for the field name. If not found,
        // it is a sema error and would have been caught;
        // the loop does not emit a value in that case, which
        // would leave the stack short. In practice this path
        // is unreachable (sema requires every field to be
        // initialized).
        case AST_STRUCT_LIT: {
            TypeDef* sd = &cg->structs->items[n->as.struct_lit.sdef_index];

            // Extern struct literal: allocate C-layout
            // memory in the arena, then store each field at
            // its offset with the field's natural width.
            // The result on the stack is the pointer.
            if (sd->kind == TDEF_EXTERN_STRUCT) {
                int slot = n->as.struct_lit.hidden_slot;

                // 1. Arena-allocate `sizeof(T)` zeroed bytes.
                set_span(cg, n);
                emit(cg, OP_PUSH); emit(cg, (U64)sd->struct_size);
                emit(cg, OP_CCALL); emit(cg, NATIVE_ARENA_ZERO);

                // 2. Save the pointer in the hidden slot.
                emit(cg, OP_STORE_LOCAL); emit(cg, (U64)slot);

                // 3. Store each field's value at its C offset.
                AstList* il = &n->as.struct_lit.inits;
                for (int i = 0; i < il->count; i++) {
                    AstNode* init = il->items[i];
                    Type* ftype = init->as.struct_init.field_type;

                    if (ftype && ftype->kind == TY_EXTERN_STRUCT) {
                        // Nested extern struct: the initializer
                        // is `*Inner` pointing at arena memory.
                        // Copy sizeof(Inner) bytes into the
                        // parent's field slot.
                        set_span(cg, init);
                        emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)slot);
                        emit(cg, OP_PUSH);
                        emit(cg, (U64)init->as.struct_init.offset);
                        emit(cg, OP_BINARY); emit(cg, OPB_ADD);
                        emit(cg, (U64)desc_of(cg, ty_i64()));
                        cg_expr(cg, init->as.struct_init.value);
                        emit(cg, OP_PUSH);
                        emit(cg, (U64)ftype->tdef->struct_size);
                        emit(cg, OP_CCALL); emit(cg, NATIVE_MEMCPY);
                        emit(cg, OP_POP);   // discard the void result
                        continue;
                    }

                    set_span(cg, init);
                    emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)slot);
                    cg_expr(cg, init->as.struct_init.value);
                    set_span(cg, init);
                    emit(cg, OP_STORE_INDIRECT);
                    emit(cg, (U64)init->as.struct_init.offset);
                    emit(cg, (U64)memkind_of(ftype));
                }

                // 4. Result: the pointer.
                set_span(cg, n);
                emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)slot);
                break;
            }

            // Regular Prism struct literal.
            //
            // The value's type decides the allocator. An RC-managed
            // struct (fields hold RC content, or the prelude marked it
            // is_builtin) is allocated on the RC heap; a plain struct
            // uses the arena.
            AstList* il = &n->as.struct_lit.inits;
            for (int i = 0; i < sd->field_count; i++) {
                for (int j = 0; j < il->count; j++) {
                    if (smap_tokens_equal(cg->smap, sd->fields[i].name,
                                          il->items[j]->as.struct_init.name)) {
                        cg_expr(cg, il->items[j]->as.struct_init.value);
                        break;
                    }
                }
            }
            set_span(cg, n);
            bool rc_managed = (n->type && n->type->kind == TY_RC);
            emit(cg, rc_managed ? OP_NEW_RC_ARRAY : OP_NEW_ARRAY);
            emit(cg, (U64)sd->field_count);
            break;
        }

        // Variant constructor / method call / module call.
        //
        // Four sub-cases, distinguished by the resolved
        // fields:
        //
        //   receiver_expr != NULL
        //     Method call. Push receiver, push args, then
        //     OP_CALL (or OP_FFI_CALL if extern). The
        //     receiver becomes arg 0 (this is the method
        //     ABI established in sema).
        //
        //   mod_ffi_idx >= 0
        //     Module-qualified extern call. Push args, then
        //     OP_FFI_CALL.
        //
        //   mod_func_idx >= 0
        //     Module-qualified user function call. Push args,
        //     then OP_CALL. The operand is the *codegen*
        //     index (mapped through codegen_of_sema).
        //
        //   else
        //     Enum variant construction. Push args, push the
        //     tag, then OP_NEW_ENUM (or OP_NEW_RC_ENUM for
        //     RC-managed enums). The tag is pushed last so
        //     the VM can read it first; see the OP_NEW_ENUM
        //     case in vm.c.
        //
        // The rc_managed test consults the TypeDef's
        // is_rc_managed flag; the prelude's Result/Option
        // have it set, user enums do not.
        case AST_VARIANT_CTOR: {
            if (n->as.variant_ctor.receiver_expr != NULL) {
                // Method call.
                cg_expr(cg, n->as.variant_ctor.receiver_expr);
                for (int i = 0; i < n->as.variant_ctor.args.count; i++)
                    cg_expr(cg, n->as.variant_ctor.args.items[i]);
                set_span(cg, n);
                if (n->as.variant_ctor.mod_ffi_idx >= 0) {
                    emit(cg, OP_FFI_CALL);
                    emit(cg, (U64)n->as.variant_ctor.mod_ffi_idx);
                    // argc includes the receiver (which is
                    // arg 0 from the callee's perspective).
                    emit(cg, (U64)(n->as.variant_ctor.args.count + 1));
                } else {
                    emit(cg, OP_CALL);
                    emit(cg, (U64)n->as.variant_ctor.mod_func_idx);
                    emit(cg, (U64)(n->as.variant_ctor.args.count + 1));
                }
                break;
            }

            // Module-qualified extern call.
            if (n->as.variant_ctor.mod_ffi_idx >= 0) {
                for (int i = 0; i < n->as.variant_ctor.args.count; i++)
                    cg_expr(cg, n->as.variant_ctor.args.items[i]);
                set_span(cg, n);
                emit(cg, OP_FFI_CALL);
                emit(cg, (U64)n->as.variant_ctor.mod_ffi_idx);
                emit(cg, (U64)n->as.variant_ctor.args.count);
                break;
            }
            // Module-qualified user function.
            if (n->as.variant_ctor.mod_func_idx >= 0) {
                for (int i = 0; i < n->as.variant_ctor.args.count; i++)
                    cg_expr(cg, n->as.variant_ctor.args.items[i]);
                set_span(cg, n);
                emit(cg, OP_CALL);
                // The mod_func_idx is a *sema* index; map to
                // codegen's own index.
                emit(cg, (U64)cg->codegen_of_sema[n->as.variant_ctor.mod_func_idx]);
                emit(cg, (U64)n->as.variant_ctor.args.count);
                break;
            }
            // Plain enum variant construction.
            for (int i = 0; i < n->as.variant_ctor.args.count; i++)
                cg_expr(cg, n->as.variant_ctor.args.items[i]);
            set_span(cg, n);
            // Tag is pushed last (on top of the payload
            // args), so the VM pops it first.
            emit(cg, OP_PUSH); emit(cg, (U64)n->as.variant_ctor.tag);
            // RC-managed vs arena: consult the TypeDef.
            // tdef_index is the sema index; the structs
            // table is the caller-owned TypeDefTable.
            bool rc_managed =
                n->as.variant_ctor.tdef_index >= 0
                && cg->structs->items[n->as.variant_ctor.tdef_index]
                       .is_rc_managed;
            emit(cg, rc_managed ? OP_NEW_RC_ENUM : OP_NEW_ENUM);
            emit(cg, (U64)n->as.variant_ctor.args.count);
            break;
        }
        
        // Top-level struct definitions have no runtime
        // representation; the AST node is only in the
        // decls list, never reached as an expression.
        // The case is here so the switch is exhaustive.
        case AST_STRUCT_DEF:
            break;

        // Field access, or unit-variant construction.
        //
        // Two cases:
        //
        //   unit_tag >= 0
        //     Unit-variant construction (`Color.Red`). Push
        //     the tag, then OP_NEW_ENUM with zero payloads.
        //     The result is a pointer to an 8-byte block
        //     containing the tag.
        //
        //   else
        //     Field access. Evaluate the target, then
        //     OP_LOAD_INDIRECT at the field's byte offset.
        //     If the target's type is ref<T>, an extra
        //     indirect load at offset 0 happens first: the
        //     ref is a pointer to a slot that holds a
        //     pointer to the struct, so loading the field
        //     requires peeling off the first level.
        case AST_FIELD:
            if (n->as.field.unit_tag >= 0) {
                set_span(cg, n);
                emit(cg, OP_PUSH); emit(cg, (U64)n->as.field.unit_tag);
                // RC-managed enums (Result, Option) are typed as
                // rc<Enum> at every use site; the allocator must match.
                // Sema has already set n->type to ty_rc(...) for these;
                // a bare enum stays ty_enum(...) and uses the arena.
                // Using OP_NEW_ENUM for an RC-managed value would put
                // the block in the arena while the scope-exit drop
                // treats it as RC heap memory, corrupting the heap.
                bool rc_managed = n->type && n->type->kind == TY_RC;
                emit(cg, rc_managed ? OP_NEW_RC_ENUM : OP_NEW_ENUM);
                emit(cg, 0);

            } else {
                cg_expr(cg, n->as.field.target);
                set_span(cg, n);

                // Extern struct field access.
                if (n->as.field.owner
                    && n->as.field.owner->kind == TY_EXTERN_STRUCT) {
                    Type* ft = n->as.field.declared_type;
                    if (ft && ft->kind == TY_EXTERN_STRUCT) {
                        // Nested by value: push the address.
                        emit(cg, OP_LOAD_ADDR_INDIRECT);
                        emit(cg, (U64)n->as.field.offset);
                    } else {
                        // Leaf: load with the field's width.
                        emit(cg, OP_LOAD_INDIRECT);
                        emit(cg, (U64)n->as.field.offset);
                        emit(cg, (U64)memkind_of(ft));
                    }
                    break;
                }

                // Regular Prism struct: unchanged, plus the
                // new mem_kind operand. ref<T> peel first.
                Type* tt = n->as.field.target->type;
                if (tt && tt->kind == TY_REF) {
                    emit(cg, OP_LOAD_INDIRECT);
                    emit(cg, 0);
                    emit(cg, MEM_U64);
                }

                emit(cg, OP_LOAD_INDIRECT);
                emit(cg, (U64)n->as.field.offset);
                emit(cg, MEM_U64);
            }
            break;
        // ----- Variable reference -----
        //
        // Resolution:
        //   const_id >= 0  → inline the value, no runtime load
        //   else           → LOAD_LOCAL by slot
        //
        // Constant inlining: for each ConstKind, emit a
        // PUSH or LOAD_CONST for the stored value. This is
        // the "const costs nothing" claim from the file
        // header — a const reference is literally the value
        // it names.
        //
        // For CONST_STRING and CONST_CSTRING the value is a
        // pool index; the pool entry was populated when the
        // const was registered. String constants are
        // currently rejected by sema, so those branches are
        // unreachable today; they exist for when string
        // constants are added.
        case AST_VAR: {
            set_span(cg, n);
            if (n->const_id >= 0) {
                const ConstEntry* e = consts_get(cg->consts, n->const_id);
                if (!e) {
                    diag_error(cg->bag, n->span,
                        "internal: const id out of range");
                    break;
                }
                switch (e->value.kind) {
                    case CONST_I64:
                        emit(cg, OP_PUSH); emit(cg, (U64)e->value.as.i64);
                        break;
                    case CONST_F64: {
                        // Note: OP_PUSH is the wrong opcode
                        // for a double — the operand is a
                        // U64, and F64 needs its bit pattern
                        // preserved. This branch currently
                        // writes the double's value into a
                        // U64 slot via C's implicit
                        // conversion, which corrupts the bit
                        // pattern. In practice this is
                        // unreachable because f64 constants
                        // are rejected by sema (const_from_literal
                        // accepts F64, but no code path
                        // currently produces one; see the
                        // follow-ups).                     
                        int idx = pool_add_f64(&cg->chunk->pool, e->value.as.f64);
                        emit(cg, OP_LOAD_CONST); emit(cg, (U64)idx);
                        break;
                    }
                    case CONST_BOOL:
                        emit(cg, OP_PUSH); emit(cg, e->value.as.b ? 1 : 0);
                        break;
                    case CONST_STRING: {
                        int idx = pool_add_string(&cg->chunk->pool,
                            e->value.as.str.bytes, e->value.as.str.length);
                        emit(cg, OP_LOAD_CONST); emit(cg, (U64)idx);
                        break;
                    case CONST_CSTRING: {
                        int idx = pool_add_cstring(&cg->chunk->pool,
                            e->value.as.str.bytes, e->value.as.str.length);
                        emit(cg, OP_LOAD_CONST); emit(cg, (U64)idx);
                        break;
                    }
                    }
                }
            } else if (n->as.var.func_idx >= 0) {
                int cg_idx = cg->codegen_of_sema[n->as.var.func_idx];
                if (cg_idx < 0) {
                    diag_error(cg->bag, n->span,
                        "internal: function value has no codegen index");
                    emit(cg, OP_PUSH); emit(cg, 0);
                    break;
                }
                emit(cg, OP_PUSH); emit(cg, (U64)cg_idx);
            } else if (n->local_index >= 0) {
                emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)n->local_index);
            } else {
                diag_error(cg->bag, n->span,
                    "internal: unresolved identifier in codegen; this "
                    "is a bug in sema, please report");
                emit(cg, OP_PUSH); emit(cg, 0);
            }
            break;
        }

        // Indirect call: `fp(a, b)`. The callee is an
        // expression of function type; its evaluation pushes
        // the codegen func index. Args are pushed on top, and
        // OP_CALL_INDIRECT does the rest.
        case AST_CALL_INDIRECT:
            cg_expr(cg, n->as.call_indirect.callee);
            for (int i = 0; i < n->as.call_indirect.args.count; i++)
                cg_expr(cg, n->as.call_indirect.args.items[i]);
            set_span(cg, n);
            emit(cg, OP_CALL_INDIRECT);
            emit(cg, (U64)n->as.call_indirect.args.count);
            break;
        
        // ----- Binary operators -----
        //
        // Left then right, then OP_BINARY <sub-op> <desc>.
        // The descriptor is taken from the LEFT operand's
        // type; for the current language this is always the
        // unified type of the operation (see analyze_expr's
        // AST_BINARY case), so it is correct as the type the
        // VM should operate on.
        //
        // Note the descriptor is the left operand's *declared*
        // type, not the expression's result type. For
        // comparisons the result is bool but the descriptor
        // is the operand's type; the VM uses the operand
        // type to know how to compare (signed vs unsigned
        // int, f64, string) and produces a 0/1 result.
        case AST_BINARY:
        if (n->as.binary.op == BIN_AND) {
            // a && b: if a is false, result is a; else
            // evaluate b.
            cg_expr(cg, n->as.binary.left);
            set_span(cg, n);
            emit(cg, OP_DUP);
            int jz = emit_placeholder(cg, OP_JMP_IF_FALSE);
            emit(cg, OP_POP);
            cg_expr(cg, n->as.binary.right);
            patch_to_here(cg, jz);
            break;
        }
        if (n->as.binary.op == BIN_OR) {
            // a || b: if a is true, result is a; else
            // evaluate b.
            cg_expr(cg, n->as.binary.left);
            set_span(cg, n);
            emit(cg, OP_DUP);
            int jt = emit_placeholder(cg, OP_JMP_IF_TRUE);
            emit(cg, OP_POP);
            cg_expr(cg, n->as.binary.right);
            patch_to_here(cg, jt);
            break;
        }
        
            cg_expr(cg, n->as.binary.left);
            cg_expr(cg, n->as.binary.right);
            set_span(cg, n);
            emit(cg, OP_BINARY);
            emit(cg, (U64)op_binary_code(n->as.binary.op));
            emit(cg, (U64)desc_of(cg, n->as.binary.left->type));   // all current binary ops are i64
            break;

        // ----- Unary operators -----
        //
        // Operand, then OP_UNARY <sub-op> <desc>. The
        // descriptor is the expression's own type (which
        // for a unary is the operand's type).
        case AST_UNARY:
            cg_expr(cg, n->as.unary.operand);
            set_span(cg, n);
            emit(cg, OP_UNARY);
            emit(cg, (U64)(n->as.unary.op == UN_NEG ? OPU_NEG : OPU_NOT));
            emit(cg, (U64)desc_of(cg, n->type));
            break;

        // ----- Function call -----
        //
        // Dispatch on builtin_id (0..13). Each builtin has
        // its own emission sequence; a few are simple
        // (one arg, one CCALL) and a few are involved
        // (print / format, handled above; raw_offset, which
        // scales by element size).
        //
        // User function call (builtin_id == -1): push args,
        // then OP_CALL or OP_FFI_CALL depending on
        // ffi_idx. The operand is codegen_of_sema[func_idx]
        // for a user function, and ffi_idx directly for an
        // extern.
        case AST_CALL: {
            // Builtin path.
            if (n->as.call.builtin_id == 0) {           // print
                cg_builtin_print(cg, n);
                break;
            }
            if (n->as.call.builtin_id == 1) {           // len
                AstNode* arg = n->as.call.args.items[0];
                cg_expr(cg, arg);
                set_span(cg, n);
                emit(cg, OP_LEN);
                // Container descriptor: unwrap rc<>.
                Type* at = arg->type;
                if (at && at->kind == TY_RC) at = at->inner;
                emit(cg, (U64)desc_of(cg, at));
                break;
            }
            if (n->as.call.builtin_id == 2) {           // format
                cg_builtin_format(cg, n);
                break;
            }
            // ffi_cstring: convert a Prism string to a
            // NUL-terminated C string in the arena.
            if (n->as.call.builtin_id == 3) {           // ffi_cstring
                AstNode* arg = n->as.call.args.items[0];
                cg_expr(cg, arg);
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_FFI_CSTRING);
                break;
            }
            // ffi_error / ffi_errno: read the VM's last
            // FFI error state. No arguments.
            if (n->as.call.builtin_id == 4) {           // ffi_error
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_FFI_ERROR);
                break;
            }
            if (n->as.call.builtin_id == 5) {           // ffi_errno
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_FFI_ERRNO);
                break;
            }
            // ffi_string: wrap a C string in an RC-managed
            // Prism string.
            if (n->as.call.builtin_id == 6) {           // ffi_string
                cg_expr(cg, n->as.call.args.items[0]);
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_FFI_STRING);
                break;
            }
            // raw_null: push 0.
            if (n->as.call.builtin_id == 7) {           // raw_null
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_RAW_NULL);
                break;
            }
            // raw_is_null: 1 if the pointer is NULL, else 0.
            if (n->as.call.builtin_id == 8) {           // raw_is_null
                cg_expr(cg, n->as.call.args.items[0]);
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_RAW_IS_NULL);
                break;
            }
            // raw_offset: scale the integer offset by the
            // element size of the pointer's target type, then
            // add to the pointer.
            //
            // The element size is computed at compile time
            // from the pointer's inner type. If the pointer
            // is *void or the inner type has size 0, the
            // size defaults to 1 (byte-wise arithmetic).
            //
            // Note the operand order: the native expects
            // [ptr, n, elem_size] on the stack, with
            // elem_size on top. This is why the PUSH for
            // elem_size comes after the two args.
            if (n->as.call.builtin_id == 9) {           // raw_offset
                AstNode* a0 = n->as.call.args.items[0];
                AstNode* a1 = n->as.call.args.items[1];
                cg_expr(cg, a0);
                cg_expr(cg, a1);
                U64 elem_size = 1;
                if (a0->type && a0->type->kind == TY_RAWPTR
                    && a0->type->inner
                    && a0->type->inner->kind != TY_VOID)
                    elem_size = (U64)a0->type->inner->size;
                if (elem_size == 0) elem_size = 1;
                set_span(cg, n);
                emit(cg, OP_PUSH); emit(cg, elem_size);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_RAW_OFFSET);
                break;
            }
            // raw_load_u8(ptr, offset) -> u8
            if (n->as.call.builtin_id == 10) {          // raw_load_u8
                cg_expr(cg, n->as.call.args.items[0]);
                cg_expr(cg, n->as.call.args.items[1]);
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_RAW_LOAD_U8);
                break;
            }
            // raw_store_u8(ptr, offset, value) -> void
            if (n->as.call.builtin_id == 11) {          // raw_store_u8
                cg_expr(cg, n->as.call.args.items[0]);
                cg_expr(cg, n->as.call.args.items[1]);
                cg_expr(cg, n->as.call.args.items[2]);
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_RAW_STORE_U8);
                break;
            }
            // ffi_alloc(size) -> *void. Zero-initialized
            // malloc; the caller is responsible for
            // freeing via ffi_free.
            if (n->as.call.builtin_id == 12) {          /* ffi_alloc */
                cg_expr(cg, n->as.call.args.items[0]);
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_FFI_ALLOC);
                break;
            }
            // ffi_free(ptr) -> void. NULL is a no-op.
            if (n->as.call.builtin_id == 13) {          /* ffi_free */
                cg_expr(cg, n->as.call.args.items[0]);
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_FFI_FREE);
                break;
            }
            if (n->as.call.builtin_id == 14) {          // args
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_ARGS);
                break;
            }
            if (n->as.call.builtin_id == 15) {          // read_line
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_READ_LINE);
                break;
            }
            if (n->as.call.builtin_id == 16) {          // read_all
                set_span(cg, n);
                emit(cg, OP_CCALL);
                emit(cg, NATIVE_READ_ALL);
                break;
            }
            
            
            
            // User function call. Args left-to-right.
            //
            // The call is dispatched to OP_FFI_CALL if the
            // callee is an extern (ffi_idx >= 0), else
            // OP_CALL. In both cases the last operand is
            // the argument count; the VM uses it to set up
            // the callee frame.
            //
            // The OP_CALL operand is the codegen index (not
            // sema's), which is why the codegen_of_sema
            // mapping exists.
            for (int i = 0; i < n->as.call.args.count; i++)
                cg_expr(cg, n->as.call.args.items[i]);
            set_span(cg, n);
            if (n->as.call.ffi_idx >= 0) {
                emit(cg, OP_FFI_CALL);
                emit(cg, (U64)n->as.call.ffi_idx);
                emit(cg, (U64)n->as.call.args.count);
            } else {
                emit(cg, OP_CALL);
                emit(cg, (U64)cg->codegen_of_sema[n->as.call.func_idx]);
                emit(cg, (U64)n->as.call.args.count);
            }
            break;
        }

        // ----- Dereference -----
        //
        // Evaluate the operand (which must be a pointer-like
        // value), then OP_LOAD_INDIRECT at the deref offset.
        // The offset is 0 for a plain `*p`; a nonzero offset
        // is set by a syntactic rewrite (see the AST_DEREF
        // case in the parser for when that happens).
        //
        // Note that rc<T> does NOT need an extra peel here:
        // an rc handle is already the block pointer, so a
        // single indirect load gets the payload.
        case AST_DEREF: {
            cg_expr(cg, n->as.deref.operand);
            set_span(cg, n);

            // For `*rc<T>` where T is a compound type
            // (struct, enum, array, string), the RC handle
            // already points at the payload. The payload IS
            // the compound value's runtime representation
            // (a `[len]...` or `[tag]...` block). Emitting
            // LOAD_INDIRECT would read the first slot of
            // that block — the length or tag — instead of
            // producing the compound value. So the deref is
            // the identity: the handle is left on the stack
            // unchanged and sema's `n->type` (the inner type)
            // describes it correctly.
            //
            // For `*rc<i64>` and similar scalars, the payload
            // is a single 8-byte slot, so LOAD_INDIRECT is
            // correct. The same is true for `*ref<...>` (a
            // slot holding the value or a pointer) and
            // `*rawptr`.
            Type* operand_type = n->as.deref.operand->type;
            if (operand_type && operand_type->kind == TY_RC
                && operand_type->inner) {
                Type* inner = operand_type->inner;
                if (inner->kind == TY_STRUCT
                    || inner->kind == TY_ENUM
                    || inner->kind == TY_ARRAY
                    || inner->kind == TY_STRING) {
                    break;   // identity; nothing emitted
                }
            }

            emit(cg, OP_LOAD_INDIRECT);
            emit(cg, (U64)n->as.deref.offset);
            emit(cg, (U64)memkind_of(n->type));
            break;
        }

        // ----- `&e` and `&mut e` -----
        //
        // If `e` is a local variable, `&e` is the address of the
        // local's stack slot — no allocation. Otherwise, `e` is
        // evaluated and its value is copied into the arena.
        //
        // The local-address case is what makes `&x` cheap:
        // the reference points directly at x's frame slot,
        // so writes through it are writes to x. This is the
        // only place a ref target is not a heap allocation.
        //
        // The general case (any non-local expression)
        // allocates an 8-byte arena cell and copies the
        // value in. The arena cell lives until the enclosing
        // scope exits (EXIT_SCOPE resets the arena mark).
        case AST_ALLOC_ARENA: {
            AstNode* inner = n->as.alloc_arena.value;
            if (inner->kind == AST_VAR && inner->const_id < 0) {
                set_span(cg, n);
                emit(cg, OP_LOAD_ADDR_LOCAL);
                emit(cg, (U64)inner->local_index);
            } else {
                cg_expr(cg, inner);
                set_span(cg, n);
                emit(cg, OP_ALLOC); emit(cg, 8);
            }
            break;
        }

        // `&p.x` on an extern struct field, rewritten by sema
        // from `AST_ALLOC_ARENA`. The child is the field
        // access node; its target is what gets evaluated (to
        // produce the base pointer), and the field's offset
        // is added to it.
        case AST_ADDR_OF_FIELD: {
            AstNode* field = n->as.addr_of_field.field;
            cg_expr(cg, field->as.field.target);
            set_span(cg, n);
            emit(cg, OP_LOAD_ADDR_INDIRECT);
            emit(cg, (U64)field->as.field.offset);
            break;
        }
        

        // ----- `rc e` -----
        //
        // Four cases depending on the inner expression's
        // kind:
        //
        //   AST_ARRAY_LIT   — OP_NEW_RC_ARRAY
        //   AST_STRUCT_LIT  — OP_NEW_RC_ARRAY (structs and
        //                     arrays share the RC layout)
        //   AST_VARIANT_CTOR — OP_NEW_RC_ENUM
        //   AST_FIELD with unit_tag >= 0 (unit variant) —
        //                     OP_NEW_RC_ENUM with 0 payloads
        //   else            — OP_ALLOC_RC 8 (single-value
        //                     RC cell)
        //
        // The first four cases are what sema calls "literal
        // forms" — the ones that `rc` accepts for compound
        // types. The last case handles `rc <expr>` for a
        // scalar expression; it allocates an 8-byte RC
        // cell holding the value.
        //
        // The layout of an RC compound is [RcHeader][payload
        // block]. The VM's OP_NEW_RC_* emit the header and
        // return the payload pointer (not the header).
        case AST_ALLOC_RC: {
            AstNode* inner = n->as.alloc_rc.value;
            if (inner->kind == AST_ARRAY_LIT) {
                // Array of values. Emit elements in
                // source order; VM pops them into the
                // block.
                for (int i = 0; i < inner->as.array_lit.elements.count; i++)
                    cg_expr(cg, inner->as.array_lit.elements.items[i]);
                set_span(cg, n);
                emit(cg, OP_NEW_RC_ARRAY);
                emit(cg, (U64)inner->as.array_lit.elements.count);
            } else if (inner->kind == AST_STRUCT_LIT) {
                            // Struct literal wrapped in
                            // rc. Walk the TypeDef's field
                            // order, emit each field's value
                            // in that order.
                            TypeDef* sd = &cg->structs->items[
                                inner->as.struct_lit.sdef_index];
                            AstList* il = &inner->as.struct_lit.inits;
                            for (int i = 0; i < sd->field_count; i++) {
                                for (int j = 0; j < il->count; j++) {
                                    if (smap_tokens_equal(cg->smap, sd->fields[i].name,
                                                       il->items[j]->as.struct_init.name)) {
                                        cg_expr(cg, il->items[j]->as.struct_init.value);
                                        break;
                                    }
                                }
                            }
                            set_span(cg, n);
                            // Structs use the array layout
                            // (both are [tag][payloads...]).
                            emit(cg, OP_NEW_RC_ARRAY);
                            emit(cg, (U64)sd->field_count);
            } else if (inner->kind == AST_VARIANT_CTOR) {
                // Sema guarantees this is a true enum variant
                // construction (receiver_expr NULL and no module call
                // index). The defensive check catches a sema regression
                // rather than emitting wrong bytecode.
                if (inner->as.variant_ctor.receiver_expr != NULL
                    || inner->as.variant_ctor.mod_func_idx >= 0
                    || inner->as.variant_ctor.mod_ffi_idx >= 0) {
                    diag_error(cg->bag, inner->span,
                        "internal: rc applied to a non-constructor call");
                    emit(cg, OP_PUSH); emit(cg, 0);
                    break;
                }
                for (int i = 0; i < inner->as.variant_ctor.args.count; i++) {
                    cg_expr(cg, inner->as.variant_ctor.args.items[i]);
                }
                set_span(cg, n);
                emit(cg, OP_PUSH);
                emit(cg, (U64)inner->as.variant_ctor.tag);
                emit(cg, OP_NEW_RC_ENUM);
                emit(cg, (U64)inner->as.variant_ctor.args.count);
            } else if(inner->kind == AST_FIELD
                && inner->as.field.unit_tag >=0){
                    // Unit variant wrapped in rc.
                    set_span(cg,n);
                    emit(cg, OP_PUSH);
                    emit(cg, (U64)inner->as.field.unit_tag);
                    emit(cg, OP_NEW_RC_ENUM);
                    emit(cg, 0);
            
            } else {
                // Scalar rc: allocate an 8-byte RC cell
                // holding the value.
                cg_expr(cg, inner);
                set_span(cg, n);
                emit(cg, OP_ALLOC_RC); emit(cg, 8);
            }
            break;
        }
        // `e as T`: emit operand, then OP_CONVERT with the
        // source and target descriptors. The VM implements
        // the numeric conversions; the source descriptor
        // tells it how to interpret the raw stack value,
        // and the target tells it what to produce.
        case AST_CAST: {
            cg_expr(cg, n->as.cast.operand);
            set_span(cg, n);

            // Pointer-involving casts are bit-preserving on
            // every target the VM supports. Emitting no
            // instruction at all is correct: the value on the
            // stack is already the bit pattern both sides
            // agree on. Sema guarantees that any cast
            // involving a pointer has the other side as a
            // pointer or a 64-bit integer.
            Type* from = n->as.cast.operand->type;
            Type* to   = n->as.cast.target_type;
            if ((from && from->kind == TY_RAWPTR)
                || (to && to->kind == TY_RAWPTR)) {
                break;
            }

            // Numeric conversion: OP_CONVERT with the source
            // and target descriptors.
            emit(cg, OP_CONVERT);
            emit(cg, (U64)desc_of(cg, from));
            emit(cg, (U64)desc_of(cg, to));
            break;
        }

        case AST_SIZEOF:
            set_span(cg, n);
            emit(cg, OP_PUSH); emit(cg, (U64)n->as.sizeof_expr.size);
            break;
            
        // `expr?` — try / early-return-on-failure.
        //
        // The emitted sequence is:
        //   <evaluate operand>       pushes the Result/Option
        //   TRY_JUMP_IF_OK <target>  if success, jump past the RET
        //   RET                      if failure, return the block
        //  <target>:
        //   <continue with the payload on the stack>
        //
        // The VM's TRY_JUMP_IF_OK checks the block's tag: tag
        // == 0 means success (jump), tag != 0 means failure
        // (fall through to RET, which returns the block as
        // the enclosing function's return value).
        //
        // On success the VM replaces the top-of-stack with
        // the payload (dropping the block wrapper) and
        // jumps. On failure the block is left on the stack
        // as the return value.
        //
        // Note that sema has already verified the operand's
        // type is Result/Option, so the runtime check inside
        // TRY_JUMP_IF_OK cannot fail on a well-formed
        // program. The runtime guards are for bytecode
        // correctness, not user errors.
        case AST_TRY: {
            // On the failure path, the block produced by the operand
            // is returned to the caller. If the operand is a bare
            // reference to an owning local, that local's reference is
            // being transferred; exclude it from the drops.
            int exclude = cg_owning_local_slot(n->as.try_.operand);
            cg_expr(cg, n->as.try_.operand);
            set_span(cg, n);
            int jmp_ok = emit_placeholder(cg, OP_TRY_JUMP_IF_OK);
            cg_emit_all_rc_drops_except(cg, exclude);
            emit(cg, OP_RET);
            patch_to_here(cg, jmp_ok);
            break;
        }
            

        default:
            // An AST kind that should never appear as an
            // expression. If it does, it is a codegen bug.
            diag_error(cg->bag, n->span,
                "codegen: unexpected expression node");
            break;
    }
}

// ============================================================
// Statement codegen
// ============================================================

// Returns true if the expression leaves a fresh RC-managed value on
// the stack — one that owns a reference and must be dropped if the
// value is discarded. Borrowed views (locals, field reads, indexing)
// return false: their owners will drop them.
//
// The classification is:
//   - AST_CALL: fresh iff the callee returns RC/string. The
//     builtin filter is for `format` (builtin 2), `ffi_error`
//     (4), and `ffi_string` (6). User and extern calls are
//     presumed to return fresh values when their return type
//     is RC-managed (sema enforces the return rule that
//     "borrowed values cannot be returned").
//   - AST_VARIANT_CTOR: fresh iff it is a construction
//     (mod_func_idx / mod_ffi_idx not applicable, or tdef_index
//     set). A method call that returns an RC-managed value
//     also produces a fresh one, hence the mod_* checks.
//   - AST_FIELD with unit_tag >= 0: unit variant construction,
//     fresh.
//   - AST_ALLOC_RC: always fresh.
//   - AST_BINARY with ADD: string concatenation produces a
//     fresh string.
//   - AST_STRING: string literal. The const-arena sentinel
//     refcount makes RC_DEC a no-op, so dropping is
//     harmless. Marking it fresh is uniform.
//   - AST_TRY: the `?` result owns its payload.
//
// Everything else returns false, meaning the expression's
// value is either non-RC or borrowed. Statement codegen uses
// this to decide between OP_POP and OP_RC_DEC for an
// expression statement, and between "drop old value" and
// "just overwrite" for an assignment.
static bool expr_produces_fresh_rc(AstNode* n) {
    if (!n || !n->type) return false;
    Type* t = n->type;
    // Non-RC types never need an RC_DEC. Note that
    // TY_STRING is treated as RC-managed: strings have an
    // RC header even though their TypeKind is distinct.
    if (t->kind != TY_RC && t->kind != TY_STRING) return false;

    switch (n->kind) {
        /*case AST_CALL: {
            int b = n->as.call.builtin_id;
            // format, ffi_error, ffi_string produce fresh strings.
            if (b == 2 || b == 4 || b == 6 || b == 14 || b == 15 || b == 16) return true;
            // User / extern calls with declared RC/string returns
            // produce fresh values (sema enforces the return rule).
            if (n->as.call.func_idx >= 0 || n->as.call.ffi_idx >= 0)
                return true;
            return false;
            }*/
        case AST_CALL:
            // A call whose declared return type is RC/string always
            // produces a fresh value:
            //   - User functions: sema's AST_RETURN borrowed-value
            //     check rejects returning a borrowed reference, so any
            //     user function returning RC/string returns a fresh
            //     allocation.
            //   - Builtins: format, ffi_error, ffi_string, args,
            //     read_line, and read_all all allocate a fresh block
            //     on each call.
            //   - Externs: the FFI marshalling path allocates a fresh
            //     RC block for Option<*T> returns.
            //
            // The early return at the top of this function has already
            // filtered out every non-RC/string return type, so the
            // case is unconditional.
            return true;
        case AST_VARIANT_CTOR:
            // Result.Ok / Option.Some construct fresh RC blocks.
            return n->as.variant_ctor.tdef_index >= 0
                || n->as.variant_ctor.mod_func_idx >= 0
                || n->as.variant_ctor.mod_ffi_idx >= 0;
        case AST_FIELD:
            // Unit variant construction (Color.Red) is fresh;
            // field access is borrowed.
            return n->as.field.unit_tag >= 0;
        case AST_ALLOC_RC:
            return true;
        case AST_BINARY:
            // String concatenation allocates a fresh block.
            return n->as.binary.op == BIN_ADD;
        case AST_STRING:
            // Const-arena literal. RC_DEC is a no-op via the sentinel
            // refcount, so dropping is harmless and uniform.
            return true;
        case AST_TRY:
            // The `?` result owns the payload reference.
            return true;
        default:
            return false;
    }
}

// Block: emit ENTER_SCOPE, walk each statement, emit EXIT_SCOPE,
// and insert the RC drops for the block's lets.
//
// The block's job in codegen is threefold:
//   1. Bracket the scope with ENTER_SCOPE / EXIT_SCOPE so the
//      VM knows where to reset its scope stack (locals) and
//      arena mark.
//   2. Emit each statement.
//   3. Emit RC_DEC for every `let` whose holds_rc flag is set,
//      *before* the EXIT_SCOPE. This is what decrements the
//      refcounts on RC-managed locals as they go out of scope.
//
// Why the drops are emitted here and not as part of the
// EXIT_SCOPE opcode: because the VM does not track which
// locals hold RC references. The compiler knows (via
// holds_rc) and emits the drops explicitly. This is what
// makes drops explicit in the bytecode rather than implicit
// in the scope mechanism.
//
// The drop loop walks the block's *statements* looking for
// AST_LET nodes, not the locals table. That is correct
// because `cg_block` is called for a block, and the block's
// stmts are exactly the let declarations that were in scope
// for this block (the sema pass uses the same set — see the
// AST_BLOCK case in analyze_stmt).
//
// The order of drops is source order, not reverse. In
// practice this does not matter because every drop targets
// a distinct local.
static void cg_block(Cg* cg, AstNode* blk) {
    set_span(cg, blk);
    emit(cg, OP_ENTER_SCOPE);
    cg->scope_depth++;
    cg_push_rc_scope(cg);

    for (int i = 0; i < blk->as.block.stmts.count; i++)
        cg_stmt(cg, blk->as.block.stmts.items[i]);

    // Natural exit: drop the scope's RC locals, then pop.
    set_span(cg, blk);
    cg_emit_scope_drops(cg, &cg->rc_scopes[cg->rc_scope_count - 1], blk->span);
    cg->rc_scope_count--;

    emit(cg, OP_EXIT_SCOPE);
    cg->scope_depth--;
}

// One statement.
//
// The contract: emit code that executes the statement and
// leaves the stack balanced (no extra values). Every
// statement kind either consumes the value it produced or
// leaves a store in its place.
//
// The stack-balance invariant is what lets statements be
// emitted in sequence without tracking stack depth at the
// cg_stmt level.
static void cg_stmt(Cg* cg, AstNode* n) {
    switch (n->kind) {
        case AST_BLOCK:
            cg_block(cg, n);
            break;

        // ----- `let` -----
        //
        // The initializer's value IS the local; it stays on the
        // stack. If the source of the initializer is another
        // RC-owning local, we increment the shared cell so both
        // locals account for a reference.
        //
        // The stack mechanics:
        //   <evaluate initializer>   — pushes the value
        //   (optional RC_INC)        — if we are sharing
        //   (implicit)               — the value stays on the
        //                              stack and becomes the
        //                              local's initial value
        //
        // Note that there is no explicit STORE_LOCAL: the
        // value is left on the stack, and subsequent reads
        // of the local (via OP_LOAD_LOCAL) find it at the
        // right slot because the local_count in the VM's
        // frame has grown to include it. This is the
        // "locals live on the stack" convention; see the
        // OP_CALL / OP_STORE_LOCAL cases in vm.c for how the
        // frame grows.
        case AST_LET: {
            AstNode* init = n->as.let.init;
            cg_expr(cg, init);
        
            // The new binding owns its reference. If the initializer
            // produced a fresh block (a call, a concat, an rc literal,
            // a variant construction, a string literal), that block's
            // refcount is already 1 for this binding; no INC. If the
            // initializer is a view (a local, a field read, an index,
            // a deref), INC so both the source and the new binding
            // hold a reference.
            if (n->holds_rc && !expr_produces_fresh_rc(init)) {
                set_span(cg, n);
                emit(cg, OP_DUP);
                emit(cg, OP_RC_INC);
            }
        
            if (n->holds_rc)
                cg_register_rc_local(cg, n->local_index, n->as.let.type);
            break;
        }

        // ----- Assignment -----
        //
        // Target is either a local variable or a dereference. In
        // both cases, the old RC reference (if any) is dropped
        // before storing the new value.
        //
        // Four target shapes:
        //   AST_VAR    — drop old if RC; then store.
        //   AST_DEREF  — no drop (we do not own the
        //                through-ref value); store indirect.
        //   AST_INDEX  — same idea as deref; store at index.
        //   AST_FIELD  — same idea as deref; store at offset.
        //
        // The RC drop for AST_VAR is the only place codegen
        // emits a drop before an assignment. That is because
        // for a local we know the previous value was owned
        // by the local (holds_rc on the local record); for
        // the other target shapes, ownership is elsewhere
        // (the ref / rc / array owns it).
        case AST_ASSIGN: {
            AstNode* tgt = n->as.assign.target;
            AstNode* val = n->as.assign.value;
        
            // RHS classification. A value is "borrowed" when its type
            // is RC/string and it was not produced by a fresh
            // expression. Storing a borrowed value into an owning
            // location must RC_INC so both locations hold a
            // reference.
            bool val_is_rc_typed =
                val->type && (val->type->kind == TY_RC
                              || val->type->kind == TY_STRING);
            bool val_borrowed = val_is_rc_typed
                && !expr_produces_fresh_rc(val);
        
            if (tgt->kind == AST_VAR) {
                // A local owns iff its holds_rc flag is set. A view
                // (holds_rc false) does not participate in refcounting
                // on either side.
                cg_expr(cg, val);
                if (tgt->holds_rc && val_borrowed) {
                    set_span(cg, n);
                    emit(cg, OP_DUP);
                    emit(cg, OP_RC_INC);
                }
                if (tgt->holds_rc) {
                    set_span(cg, n);
                    emit(cg, OP_LOAD_LOCAL);
                    emit(cg, (U64)tgt->local_index);
                    emit(cg, OP_RC_DEC);
                    emit(cg, (U64)desc_of(cg, tgt->type));
                }
                set_span(cg, n);
                emit(cg, OP_STORE_LOCAL);
                emit(cg, (U64)tgt->local_index);
                break;
            }
        
            // Non-local targets: the target's type decides whether it
            // owns an RC reference. If it does, drop the old value
            // first and INC a borrowed RHS.
            bool target_owns = tgt->type
                && (tgt->type->kind == TY_RC || tgt->type->kind == TY_STRING);
        
            if (tgt->kind == AST_DEREF) {
                cg_expr(cg, tgt->as.deref.operand);
                set_span(cg, n);
                if (target_owns) {
                    emit(cg, OP_DUP);
                    emit(cg, OP_LOAD_INDIRECT);
                    emit(cg, (U64)tgt->as.deref.offset);
                    emit(cg, (U64)memkind_of(tgt->type));
                    emit(cg, OP_RC_DEC);
                    emit(cg, (U64)desc_of(cg, tgt->type));
                }
                cg_expr(cg, val);
                if (target_owns && val_borrowed) {
                    set_span(cg, n);
                    emit(cg, OP_DUP);
                    emit(cg, OP_RC_INC);
                }
                set_span(cg, n);
                emit(cg, OP_STORE_INDIRECT);
                emit(cg, (U64)tgt->as.deref.offset);
                emit(cg, (U64)memkind_of(tgt->type));
                break;
            }
        
            if (tgt->kind == AST_INDEX) {
                AstNode* container = tgt->as.index.target;
                Type* ct = container->type;
                if (ct && ct->kind == TY_RC) ct = ct->inner;
        
                cg_expr(cg, container);
                cg_expr(cg, tgt->as.index.index);
                if (target_owns) {
                    set_span(cg, n);
                    emit(cg, OP_DUP2);
                    emit(cg, OP_INDEX_LOAD);
                    emit(cg, (U64)desc_of(cg, ct));
                    emit(cg, OP_RC_DEC);
                    emit(cg, (U64)desc_of(cg, tgt->type));
                }
                cg_expr(cg, val);
                if (target_owns && val_borrowed) {
                    set_span(cg, n);
                    emit(cg, OP_DUP);
                    emit(cg, OP_RC_INC);
                }
                set_span(cg, n);
                emit(cg, OP_INDEX_STORE);
                emit(cg, (U64)desc_of(cg, ct));
                break;
            }
        
            if (tgt->kind == AST_FIELD) {
                cg_expr(cg, tgt->as.field.target);
                set_span(cg, n);
                Type* tt = tgt->as.field.target->type;
                if (tt && tt->kind == TY_REF) {
                    emit(cg, OP_LOAD_INDIRECT);
                    emit(cg, 0);
                    emit(cg, MEM_U64);
                }
                U32 mk;
                if (tgt->as.field.owner
                    && tgt->as.field.owner->kind == TY_EXTERN_STRUCT) {
                    mk = memkind_of(tgt->as.field.declared_type);
                } else {
                    mk = MEM_U64;
                }
                if (target_owns) {
                    set_span(cg, n);
                    emit(cg, OP_DUP);
                    emit(cg, OP_LOAD_INDIRECT);
                    emit(cg, (U64)tgt->as.field.offset);
                    emit(cg, mk);
                    emit(cg, OP_RC_DEC);
                    emit(cg, (U64)desc_of(cg, tgt->type));
                }
                cg_expr(cg, val);
                if (target_owns && val_borrowed) {
                    set_span(cg, n);
                    emit(cg, OP_DUP);
                    emit(cg, OP_RC_INC);
                }
                set_span(cg, n);
                emit(cg, OP_STORE_INDIRECT);
                emit(cg, (U64)tgt->as.field.offset);
                emit(cg, mk);
                break;
            }
        
            diag_error(cg->bag, tgt->span,
                "codegen: invalid assignment target");
            break;
        }        

        
        case AST_ASSIGN_OP: {
            AstNode* tgt = n->as.assign_op.target;
            AstNode* val = n->as.assign_op.value;
            U32      opc = op_binary_code(n->as.assign_op.op);
            int      tmp = n->as.assign_op.temp_slot;
            bool     rc  = (tmp >= 0);

            if (tgt->kind == AST_VAR) {
                set_span(cg, n);
                emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)tgt->local_index);
                if (rc) {
                    // Save old to temp, then reload it so the
                    // stack holds [old] and the value side
                    // pushes [old, new]. BINARY then computes
                    // `old op new`, which is the correct order
                    // for -=, /=, and string concatenation.
                    emit(cg, OP_STORE_LOCAL); emit(cg, (U64)tmp);
                    emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)tmp);
                }
                cg_expr(cg, val);
                set_span(cg, n);
                emit(cg, OP_BINARY); emit(cg, (U64)opc);
                emit(cg, (U64)desc_of(cg, tgt->type));
                if (rc) {
                    // Drop the old value now that the new one
                    // is computed and on the stack.
                    emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)tmp);
                    emit(cg, OP_RC_DEC);
                    emit(cg, (U64)desc_of(cg, tgt->type));
                }
                emit(cg, OP_STORE_LOCAL); emit(cg, (U64)tgt->local_index);
                break;
            }

            if (tgt->kind == AST_DEREF) {
                cg_expr(cg, tgt->as.deref.operand);
                set_span(cg, n);
                emit(cg, OP_DUP);
                emit(cg, OP_LOAD_INDIRECT);
                emit(cg, (U64)tgt->as.deref.offset);
                emit(cg, (U64)memkind_of(tgt->type));
                if (rc) {
                    // Same save-and-reload pattern. `old` is on
                    // top after LOAD_INDIRECT; stash it and put
                    // it back before evaluating the new value.
                    emit(cg, OP_STORE_LOCAL); emit(cg, (U64)tmp);
                    emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)tmp);
                }
                cg_expr(cg, val);
                set_span(cg, n);
                emit(cg, OP_BINARY); emit(cg, (U64)opc);
                emit(cg, (U64)desc_of(cg, tgt->type));
                if (rc) {
                    emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)tmp);
                    emit(cg, OP_RC_DEC);
                    emit(cg, (U64)desc_of(cg, tgt->type));
                }
                emit(cg, OP_STORE_INDIRECT);
                emit(cg, (U64)tgt->as.deref.offset);
                emit(cg, (U64)memkind_of(tgt->type));
                break;
            }

            if (tgt->kind == AST_INDEX) {
                AstNode* container = tgt->as.index.target;
                cg_expr(cg, container);
                cg_expr(cg, tgt->as.index.index);
                set_span(cg, n);
                emit(cg, OP_DUP2);
                Type* ct = container->type;
                if (ct && ct->kind == TY_RC) ct = ct->inner;
                emit(cg, OP_INDEX_LOAD);
                emit(cg, (U64)desc_of(cg, ct));
                if (rc) {
                    emit(cg, OP_STORE_LOCAL); emit(cg, (U64)tmp);
                    emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)tmp);
                }
                cg_expr(cg, val);
                set_span(cg, n);
                emit(cg, OP_BINARY); emit(cg, (U64)opc);
                emit(cg, (U64)desc_of(cg, tgt->type));
                if (rc) {
                    emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)tmp);
                    emit(cg, OP_RC_DEC);
                    emit(cg, (U64)desc_of(cg, tgt->type));
                }
                emit(cg, OP_INDEX_STORE);
                emit(cg, (U64)desc_of(cg, ct));
                break;
            }

            if (tgt->kind == AST_FIELD) {
                cg_expr(cg, tgt->as.field.target);
                set_span(cg, n);
                Type* tt = tgt->as.field.target->type;
                if (tt && tt->kind == TY_REF) {
                    emit(cg, OP_LOAD_INDIRECT);
                    emit(cg, 0);
                    emit(cg, MEM_U64);
                }
                emit(cg, OP_DUP);
                U32 mk;
                if (tgt->as.field.owner
                    && tgt->as.field.owner->kind == TY_EXTERN_STRUCT) {
                    mk = memkind_of(tgt->as.field.declared_type);
                } else {
                    mk = MEM_U64;
                }
                emit(cg, OP_LOAD_INDIRECT);
                emit(cg, (U64)tgt->as.field.offset);
                emit(cg, mk);
                if (rc) {
                    emit(cg, OP_STORE_LOCAL); emit(cg, (U64)tmp);
                    emit(cg, OP_LOAD_LOCAL);  emit(cg, (U64)tmp);
                }
                cg_expr(cg, val);                       // once
                set_span(cg, n);
                emit(cg, OP_BINARY); emit(cg, (U64)opc);
                emit(cg, (U64)desc_of(cg, tgt->type));
                if (rc) {
                    emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)tmp);
                    emit(cg, OP_RC_DEC);
                    emit(cg, (U64)desc_of(cg, tgt->type));
                }
                emit(cg, OP_STORE_INDIRECT);
                emit(cg, (U64)tgt->as.field.offset);
                emit(cg, mk);
                break;
            }
            break;
        }
        

        // ----- Expression statement -----
        //
        // If the expression produces a fresh RC-managed value,
        // drop it now instead of just POPping it. Borrowed views
        // (locals, field reads, indexes) are unaffected: their
        // owners will drop them.
        //
        // The choice between OP_RC_DEC and OP_POP is what keeps
        // the refcount correct for `make_string();` (a
        // statement whose value is discarded) versus
        // `some_local;` (a no-op statement that should not
        // touch the refcount of the local).
        case AST_EXPR_STMT: {
            AstNode* expr = n->as.expr_stmt.expr;
            cg_expr(cg, expr);
            set_span(cg, n);
            if (expr_produces_fresh_rc(expr)) {
                emit(cg, OP_RC_DEC);
                emit(cg, (U64)desc_of(cg, expr->type));   // NEW
            } else {
                emit(cg, OP_POP);
            }
            break;
        }

        // ----- `if` -----
        //
        // Standard if/else with short-circuit:
        //   <cond>
        //   JMP_IF_FALSE else
        //   <then>
        //   JMP end
        //  else:
        //   <else>
        //  end:
        //
        // Without an else, the JMP_IF_FALSE target is "end".
        case AST_IF:
            cg_expr(cg, n->as.if_.cond);
            set_span(cg, n);
            {
                int jz = emit_placeholder(cg, OP_JMP_IF_FALSE);

                cg_stmt(cg, n->as.if_.then_);

                if (n->as.if_.else_) {
                    set_span(cg, n);
                    int jmp = emit_placeholder(cg, OP_JMP);
                    patch_to_here(cg, jz);
                    cg_stmt(cg, n->as.if_.else_);
                    patch_to_here(cg, jmp);
                } else {
                    patch_to_here(cg, jz);
                }
            }
            break;

        // ----- `while` -----
        //
        // Layout:
        //  loop_start:
        //   <cond>
        //   JMP_IF_FALSE end
        //   <body>
        //   [continue targets land here]
        //   JMP loop_start
        //  end:
        //   [break targets land here]
        //
        // The continue target is between the body and the
        // unconditional JMP back to loop_start. A continue in
        // a while skips the rest of the body and re-tests
        // the condition; that is why continue lands before
        // the JMP rather than after it.
        //
        // A LoopFrame is pushed to track the break/continue
        // patches. For a while, entry_scope_depth and
        // continue_scope_depth are both cg->scope_depth, so
        // both break and continue unwind to the same level.
        case AST_WHILE: {
            if (cg->loop_count >= MAX_LOOP_DEPTH) {
                diag_error(cg->bag, n->span,
                    "too many nested loops (max %d)", MAX_LOOP_DEPTH);
                break;
            }
            LoopFrame* lf = &cg->loops[cg->loop_count++];
            lf->entry_scope_depth    = cg->scope_depth;
            lf->continue_scope_depth = cg->scope_depth;
            lf->continue_count       = 0;
            lf->break_count          = 0;

            set_span(cg, n);
            int loop_start = cg->chunk->count;

            cg_expr(cg, n->as.while_.cond);
            set_span(cg, n);
            int jz = emit_placeholder(cg, OP_JMP_IF_FALSE);

            cg_stmt(cg, n->as.while_.body);

            // Continue target: patched to just before the JMP back
            // to loop_start. Continue in a while skips the rest of
            // the body and re-tests the condition.
            for (int i = 0; i < lf->continue_count; i++)
                patch_to_here(cg, lf->continue_patches[i]);

            set_span(cg, n);
            emit(cg, OP_JMP); emit(cg, (U64)loop_start);

            // Natural exit: fall through to here.
            patch_to_here(cg, jz);

            cg->loop_count--;

            // Break target: same location as the natural exit.
            for (int i = 0; i < lf->break_count; i++)
                patch_to_here(cg, lf->break_patches[i]);
            break;
        }

        // ----- `for i in a..b` -----
        //
        // Layout:
        //   ENTER_SCOPE                  ; loop scope (var + end)
        //   <start>                      ; push loop var init
        //   <end>                        ; push end value
        //  loop_start:
        //   LOAD_LOCAL var
        //   LOAD_LOCAL end
        //   BINARY LT i64
        //   JMP_IF_FALSE end_label
        //   ENTER_SCOPE                  ; body scope
        //   <body stmts>
        //   EXIT_SCOPE                   ; body scope
        //   [continue targets land here]
        //   LOAD_LOCAL var; PUSH 1; ADD; STORE_LOCAL var
        //   JMP loop_start
        //  end_label:
        //   EXIT_SCOPE                   ; loop scope
        //   [break targets land here]
        //
        // The `start` and `end` expressions are both
        // evaluated at loop entry and left on the stack as
        // the var and end slots. That is why the var and
        // end slots are the first two local slots of the
        // loop scope.
        //
        // Note the loop variable's value is initialized to
        // `start` and the end is cached at `end`. A `for i
        // in 0..n` with n a variable evaluates n once. That
        // is the language's semantics.
        //
        // The body scope is separate from the loop scope so
        // that a `let` inside the body gets a fresh slot
        // per iteration conceptually (though the slot number
        // is reused; there is no per-iteration allocation).
        case AST_FOR_RANGE: {
            if (cg->loop_count >= MAX_LOOP_DEPTH) {
                diag_error(cg->bag, n->span,
                    "too many nested loops (max %d)", MAX_LOOP_DEPTH);
                break;
            }
            set_span(cg, n);
            emit(cg, OP_ENTER_SCOPE);
            cg->scope_depth++;
            cg_push_rc_scope(cg);

            LoopFrame* lf = &cg->loops[cg->loop_count++];
            // break unwinds to the *outer* scope (before the
            // loop's own ENTER_SCOPE), hence scope_depth - 1.
            lf->entry_scope_depth    = cg->scope_depth - 1;
            // continue unwinds to the *loop* scope (after
            // the loop's ENTER_SCOPE but before the body's).
            lf->continue_scope_depth = cg->scope_depth;
            lf->continue_count       = 0;
            lf->break_count          = 0;

            // Initialization: loop var at top of the loop scope,
            // cached end value immediately above it.
            cg_expr(cg, n->as.for_range.start);
            cg_expr(cg, n->as.for_range.end);

            int loop_start = cg->chunk->count;

            // Condition: var < end.
            set_span(cg, n);
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)n->as.for_range.var_slot);
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)n->as.for_range.end_slot);
            emit(cg, OP_BINARY); emit(cg, OPB_LT);
            emit(cg, (U64)desc_of(cg, ty_i64()));
            int jz = emit_placeholder(cg, OP_JMP_IF_FALSE);

            // Body scope.
            set_span(cg, n);
            emit(cg, OP_ENTER_SCOPE);
            cg->scope_depth++;
            cg_push_rc_scope(cg);

            {
                AstNode* body = n->as.for_range.body;
                for (int i = 0; i < body->as.block.stmts.count; i++)
                    cg_stmt(cg, body->as.block.stmts.items[i]);
            }

            set_span(cg, n);
            cg_emit_scope_drops(cg, &cg->rc_scopes[cg->rc_scope_count - 1], n->span);
            cg->rc_scope_count--;
            emit(cg, OP_EXIT_SCOPE);
            cg->scope_depth--;

            // Continue target: after the body scope has been closed,
            // before the increment. A continue skips the rest of the
            // body but still runs the increment.
            for (int i = 0; i < lf->continue_count; i++)
                patch_to_here(cg, lf->continue_patches[i]);

            // Increment: var = var + 1.
            set_span(cg, n);
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)n->as.for_range.var_slot);
            emit(cg, OP_PUSH); emit(cg, 1);
            emit(cg, OP_BINARY); emit(cg, OPB_ADD);
            emit(cg, (U64)desc_of(cg, ty_i64()));
            emit(cg, OP_STORE_LOCAL); emit(cg, (U64)n->as.for_range.var_slot);

            set_span(cg, n);
            emit(cg, OP_JMP); emit(cg, (U64)loop_start);

            // Natural exit: fall through to here.
            patch_to_here(cg, jz);

            // Close the loop scope.
            set_span(cg, n);
            cg_emit_scope_drops(cg, &cg->rc_scopes[cg->rc_scope_count - 1], n->span);
            cg->rc_scope_count--;
            emit(cg, OP_EXIT_SCOPE);
            cg->scope_depth--;

            cg->loop_count--;

            // Break target: after the loop scope is closed.
            for (int i = 0; i < lf->break_count; i++)
                patch_to_here(cg, lf->break_patches[i]);
            break;
        }

        // ----- `for x in iterable` -----
        //
        // Layout (slots in the loop scope: arr, len, idx, var):
        //   ENTER_SCOPE
        //   <iterable>                   ; push arr
        //   LOAD_LOCAL arr; LEN <desc>   ; push len
        //   PUSH 0                       ; push idx
        //  loop_start:
        //   LOAD_LOCAL idx
        //   LOAD_LOCAL len
        //   BINARY LT i64
        //   JMP_IF_FALSE end_label
        //   ENTER_SCOPE                  ; body scope (var lives here)
        //   LOAD_LOCAL arr
        //   LOAD_LOCAL idx
        //   INDEX_LOAD <desc>            ; push arr[idx]
        //   <body stmts>                 ; var is at the top of
        //                                ; the body scope's frame
        //   EXIT_SCOPE
        //   [continue targets land here]
        //   LOAD_LOCAL idx; PUSH 1; ADD; STORE_LOCAL idx
        //   JMP loop_start
        //  end_label:
        //   EXIT_SCOPE
        //   [break targets land here]
        //
        // The body scope: the loop variable is the first local
        // declared in the body scope, so the INDEX_LOAD
        // result is left on the stack as the var's slot. That
        // is why the var_slot appears as the first element of
        // the body scope's frame.
        case AST_FOR_IN: {
            if (cg->loop_count >= MAX_LOOP_DEPTH) {
                diag_error(cg->bag, n->span,
                    "too many nested loops (max %d)", MAX_LOOP_DEPTH);
                break;
            }
            set_span(cg, n);
            emit(cg, OP_ENTER_SCOPE);
            cg->scope_depth++;
            cg_push_rc_scope(cg);

            LoopFrame* lf = &cg->loops[cg->loop_count++];
            lf->entry_scope_depth    = cg->scope_depth - 1;
            lf->continue_scope_depth = cg->scope_depth;
            lf->continue_count       = 0;
            lf->break_count          = 0;

            // The loop scope holds: arr, len, idx, and the loop var.
            // Sema has assigned slots in that order.

            // arr_slot = iterable.
            cg_expr(cg, n->as.for_in.iterable);

            // Determine the container's descriptor for LEN and INDEX.
            // Unwrap rc<>: the container is the underlying array
            // or string, not the RC wrapper.
            Type* itype = n->as.for_in.iterable->type;
            // If the iterable produced an RC value, its reference
            // lives in arr_slot for the loop's duration. Register
            // it so the loop-scope drop decrements it at exit.
            if (itype && itype->kind == TY_RC)
                        cg_register_rc_local(cg, n->as.for_in.arr_slot, itype);
            Type* arr_t = (itype && itype->kind == TY_RC)
                        ? itype->inner : itype;

            // len_slot = len(arr).
            set_span(cg, n);
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)n->as.for_in.arr_slot);
            emit(cg, OP_LEN);
            emit(cg, (U64)desc_of(cg, arr_t));

            // idx_slot = 0.
            set_span(cg, n);
            emit(cg, OP_PUSH); emit(cg, 0);

            int loop_start = cg->chunk->count;

            // Condition: idx < len.
            set_span(cg, n);
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)n->as.for_in.idx_slot);
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)n->as.for_in.len_slot);
            emit(cg, OP_BINARY); emit(cg, OPB_LT);
            emit(cg, (U64)desc_of(cg, ty_i64()));
            int jz = emit_placeholder(cg, OP_JMP_IF_FALSE);

            // Body scope: contains the loop variable binding.
            set_span(cg, n);
            emit(cg, OP_ENTER_SCOPE);
            cg->scope_depth++;
            cg_push_rc_scope(cg);

            // var_slot = arr[idx]. The value is pushed and
            // stays as the var_slot in the body scope's
            // frame; that is the convention for locals.
            set_span(cg, n);
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)n->as.for_in.arr_slot);
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)n->as.for_in.idx_slot);
            emit(cg, OP_INDEX_LOAD);
            emit(cg, (U64)desc_of(cg, arr_t));

            {
                AstNode* body = n->as.for_in.body;
                for (int i = 0; i < body->as.block.stmts.count; i++)
                    cg_stmt(cg, body->as.block.stmts.items[i]);
            }

            set_span(cg, n);
            cg_emit_scope_drops(cg, &cg->rc_scopes[cg->rc_scope_count - 1], n->span);
            cg->rc_scope_count--;
            emit(cg, OP_EXIT_SCOPE);
            cg->scope_depth--;

            // Continue target: after the body scope, before idx
            // increment. Continue skips the rest of the body but
            // still advances to the next element.
            for (int i = 0; i < lf->continue_count; i++)
                patch_to_here(cg, lf->continue_patches[i]);

            // Increment: idx = idx + 1.
            set_span(cg, n);
            emit(cg, OP_LOAD_LOCAL); emit(cg, (U64)n->as.for_in.idx_slot);
            emit(cg, OP_PUSH); emit(cg, 1);
            emit(cg, OP_BINARY); emit(cg, OPB_ADD);
            emit(cg, (U64)desc_of(cg, ty_i64()));
            emit(cg, OP_STORE_LOCAL); emit(cg, (U64)n->as.for_in.idx_slot);

            set_span(cg, n);
            emit(cg, OP_JMP); emit(cg, (U64)loop_start);

            // Natural exit.
            patch_to_here(cg, jz);

            // Close the loop scope.
            set_span(cg, n);
            cg_emit_scope_drops(cg, &cg->rc_scopes[cg->rc_scope_count - 1], n->span);
            cg->rc_scope_count--;
            emit(cg, OP_EXIT_SCOPE);
            cg->scope_depth--;

            cg->loop_count--;

            // Break target.
            for (int i = 0; i < lf->break_count; i++)
                patch_to_here(cg, lf->break_patches[i]);
            break;
        }

        // Top-level enum defs have no runtime rep. Skipped.
        case AST_ENUM_DEF:
            break;

        // ----- `match` -----
        //
        // Layout:
        //   ENTER_SCOPE
        //   <scrutinee>
        //   STORE_LOCAL hidden_slot       ; cache the value
        //
        //   per arm:
        //     <pattern tests, failures jump to arm_fail>
        //     <arm body>
        //     JMP end
        //   arm_fail:
        //     (falls through to next arm)
        //
        //   TRAP "match: no arm matched"  ; defensive
        //   end:
        //   EXIT_SCOPE
        //
        // Two arrays are declared statically: `end_jumps`
        // collects one JMP per arm (all patched to `end`),
        // and each arm has its own `fails` PatchList for its
        // pattern-test failures.
        //
        // The scrutinee is stored in the hidden slot once
        // and each arm reads it from there. This means the
        // scrutinee's side effects (if any) happen exactly
        // once, which is the standard semantics.
        //
        // The TRAP after the last arm is defensive: sema
        // requires matches to be exhaustive at the top
        // level, but a nested subpattern can still fail at
        // runtime (see the note in check_exhaustive). The
        // TRAP gives a clean error rather than falling off
        // the end of the switch.
        case AST_MATCH: {
            set_span(cg, n);
            emit(cg, OP_ENTER_SCOPE);
            cg->scope_depth++;
            cg_push_rc_scope(cg);

            cg_expr(cg, n->as.match.scrutinee);
            set_span(cg, n);
            emit(cg, OP_STORE_LOCAL);
            emit(cg, (U64)n->as.match.hidden_slot);

            PatchList end_jumps;
            patchlist_init(&end_jumps);

            for (int a = 0; a < n->as.match.arms.count; a++) {
                AstNode* arm = n->as.match.arms.items[a];

                PatchList fails;
                patchlist_init(&fails);

                cg_pattern_test(cg, arm->as.match_arm.pattern,
                                n->as.match.hidden_slot, &fails);

                cg_stmt(cg, arm->as.match_arm.body);

                int jmp_end = emit_placeholder(cg, OP_JMP);
                patchlist_push(&end_jumps, jmp_end);

                // All pattern-fail jumps in this arm land here —
                // immediately after the arm's body, which is the
                // start of the next arm.
                for (int i = 0; i < fails.count; i++)
                    patch_to_here(cg, fails.slots[i]);

                patchlist_free(&fails);
            }

            // Defensive trap: sema should have proven exhaustiveness.
            set_span(cg, n);
            {
                const char* msg = "match: no arm matched";
                int idx = pool_add_string(&cg->chunk->pool,
                                          msg, (uint32_t)strlen(msg));
                emit(cg, OP_TRAP); emit(cg, (U64)idx);
            }

            for (int i = 0; i < end_jumps.count; i++)
                patch_to_here(cg, end_jumps.slots[i]);

            patchlist_free(&end_jumps);

            set_span(cg, n);
            cg_emit_scope_drops(cg, &cg->rc_scopes[cg->rc_scope_count - 1], n->span);
            cg->rc_scope_count--;
            emit(cg, OP_EXIT_SCOPE);
            cg->scope_depth--;
            break;
        }
        // ----- `return` -----
        //
        // Every function leaves one value on the stack, void
        // included (as 0). RET pops it and hands it to the caller.
        //
        // A bare `return;` emits PUSH 0 then RET. A `return
        // expr;` evaluates the expression (which leaves its
        // value on the stack) and then RETs.
        case AST_RETURN: {
            // The returned value may be a bare reference to an owning
            // local. If so, its reference is being transferred to the
            // caller; exclude it from the drop pass or the block will
            // be freed before RET executes.
            int exclude = cg_owning_local_slot(n->as.ret.value);
            if (n->as.ret.value) cg_expr(cg, n->as.ret.value);
            else { set_span(cg, n); emit(cg, OP_PUSH); emit(cg, 0); }
            set_span(cg, n);
            cg_emit_all_rc_drops_except(cg, exclude);
            emit(cg, OP_RET);
            break;
        }

        // ----- `const` at top level: no runtime emission -----
        // Constants are inlined at use sites, so the
        // declaration itself emits nothing.
        case AST_CONST:
            break;

        // ----- `break` -----
        //
        // Emit EXIT_SCOPE for each scope between the current
        // depth and the loop's entry scope, then a JMP whose
        // target is patched when the loop exits.
        //
        // The EXIT_SCOPE count is what makes `break` unwind
        // through any nested blocks (e.g. a `break` inside an
        // `if` inside a loop must EXIT_SCOPE for the if's
        // scope before jumping).
        case AST_BREAK: {
                if (cg->loop_count == 0) {
                    // Sema should have caught this; defensive.
                    diag_error(cg->bag, n->span,
                        "codegen: 'break' outside loop");
                    break;
                }
                LoopFrame* lf = &cg->loops[cg->loop_count - 1];
    
                int to_exit = cg->scope_depth - lf->entry_scope_depth;
                // handle rc drops/decrement before exit
                set_span(cg,n);
                cg_emit_n_scope_drops(cg, to_exit);
                for (int i = 0; i < to_exit; i++) {
                    set_span(cg, n);
                    emit(cg, OP_EXIT_SCOPE);
                }
    
                set_span(cg, n);
                int slot = emit_placeholder(cg, OP_JMP);
                if (lf->break_count < MAX_BREAKS_PER_LOOP) {
                    lf->break_patches[lf->break_count++] = slot;
                } else {
                    diag_error(cg->bag, n->span,
                        "too many 'break's in one loop (max %d)",
                        MAX_BREAKS_PER_LOOP);
                }
                break;
            }
    
        // ----- `continue` -----
        //
        // Same shape as break, but unwinds to the loop's
        // continue_scope_depth (which is deeper than the
        // break target for for-loops) and jumps to the
        // continue target.
        //
        // Note the manual emit of a placeholder JMP: it uses
        // emit + chunk_emit directly instead of
        // emit_placeholder, because it needs the placeholder
        // slot for the continue patch list. The two-call
        // sequence is equivalent.
        case AST_CONTINUE: {
                if (cg->loop_count == 0) {
                    diag_error(cg->bag, n->span,
                        "codegen: 'continue' outside loop");
                    break;
                }
                LoopFrame* lf = &cg->loops[cg->loop_count - 1];
    
                int to_exit = cg->scope_depth - lf->continue_scope_depth;
                // rc drops handled 
                set_span(cg, n);
                cg_emit_n_scope_drops(cg, to_exit);
                for (int i = 0; i < to_exit; i++) {
                    set_span(cg, n);
                    emit(cg, OP_EXIT_SCOPE);
                }
    
                set_span(cg, n);
                emit(cg, OP_JMP);
                int slot = chunk_emit(cg->chunk, 0,
                                      cg->current_line, cg->current_column);
                if (lf->continue_count < MAX_BREAKS_PER_LOOP) {
                    lf->continue_patches[lf->continue_count++] = slot;
                } else {
                    diag_error(cg->bag, n->span,
                        "too many 'continue's in one loop (max %d)",
                        MAX_BREAKS_PER_LOOP);
                }
                break;
            }
        default:
            diag_error(cg->bag, n->span,
                "codegen: unexpected statement node");
            break;
    }
}

// ============================================================
// Program codegen
//
// The entry point. In order:
//
//   1. Build codegen_of_sema[], a mapping from sema's func
//      index to codegen's func index (they are the same
//      today, because both iterate the same sc->funcs[]
//      and skip the same functions, but the mapping is
//      explicit so a future change to codegen's enumeration
//      order does not break the CALL operands).
//
//   2. Find `main` among the sema functions. If absent,
//      report and bail — a program without main is not
//      runnable.
//
//   3. Emit the prologue: CALL main with argc 0, then HALT.
//      The CALL and HALT are the first two instructions.
//
//   4. For each codegen-eligible function (non-extern,
//      non-generic, has a body), record its entry_pc in
//      the chunk's func table and emit its body. After
//      each body, emit PUSH 0; RET as the fallback exit
//      (for functions that fall off the end without an
//      explicit return).
//
// The order of (3) before (4) matters: the prologue's
// CALL main must know main's codegen index, which is
// assigned in step (1). The prologue is emitted before
// any body, so the entry point is always at PC 0-1.
//
// Function bodies are emitted in the order they appear in
// sc->funcs[]. That order is the order sema assigned
// during Pass A, which is source order (with instantiations
// appended at the end as they occur during body analysis).
// Because CALL targets are emitted as placeholders and
// patched by the VM at runtime via the func table lookup
// (not by backpatching), the source order is not
// load-bearing.
// ============================================================
void codegen_program(AstNode* program, const SourceMap* smap,
                     Chunk* chunk, DiagnosticBag* bag,
                     const ConstTable* consts,
                     const TypeDefTable* structs,
                     const FFITable* ffi,
                     const SemaCtx* sc) {
    (void)program;  // silence the unused warnings, codegen now walks sc->funcs[] instead of program->decls
    Cg cg;
    memset(&cg, 0, sizeof cg);
    cg.chunk  = chunk;
    cg.bag    = bag;
    cg.smap   = smap;
    cg.consts = consts;
    cg.structs=structs;
    (void)ffi;          // reserved, sema already populated the table

    // ---- Collect functions, find main ----
    //
    // A "codegen-eligible" function is one that:
    //   - is not extern (externs have no body);
    //   - has a decl (some sema paths leave it NULL);
    //   - is not generic (templates are not emitted; their
    //     instantiations are).
    //
    // The first pass assigns codegen indices and locates
    // main. The second pass (below) emits bodies.
    int codegen_of_sema[MAX_FUNCS];
    for (int i = 0; i < MAX_FUNCS; i++) codegen_of_sema[i] = -1;

    int next_codegen_idx = 0;
    int main_sema_idx    = -1;
    int main_codegen_idx = -1;

    // populates codegen_of_sema
    for (int i = 0; i < sc->func_count; i++) {
        const FuncSig* sig = &sc->funcs[i];
        if (sig->is_extern) continue;
        if (sig->decl == NULL) continue;
        if (sig->decl->as.func.type_param_count > 0) continue;

        codegen_of_sema[i] = next_codegen_idx++;

        // main detection by name comparison.
        if (smap_span_equals(smap, sig->name.span, "main")) {
            main_sema_idx    = i;
            main_codegen_idx = codegen_of_sema[i];
        }
    }

    // No main: a hard error. This happens before any
    // emission, so the chunk is left empty.
    if (main_codegen_idx < 0) {
        diag_error(bag, SPAN_NONE, "no 'main' function declared");
        return;
    }

    // copy the populated codegen_of_sema into cg
    // alternatively, we can directly populate the table above in the loop itself
    // using cg->codegen_of_sema instead of populating bare table and copying
    memcpy(cg.codegen_of_sema, codegen_of_sema,
           sizeof cg.codegen_of_sema);    

    // ---- Prologue: CALL main, HALT ----
    //
    // The span is main's decl, so a runtime error at
    // instruction 0 or 1 would point at main's source
    // location. That is a nicety; the two instructions
    // almost never error.
    set_span(&cg, sc->funcs[main_sema_idx].decl);
    emit(&cg, OP_CALL);
    emit(&cg, (U64)main_codegen_idx);
    emit(&cg, 0);
    emit(&cg, OP_HALT);

    // ---- Emit every body; populate func table as we go ----
    //
    // chunk_add_func records the function's entry_pc under
    // its sema name; the VM looks up a callee by func_idx
    // (which is the codegen index) at OP_CALL time. The
    // name in the func table is used for the trace / error
    // renderer, not for dispatch.
    for (int i = 0; i < sc->func_count; i++) {
        const FuncSig* sig = &sc->funcs[i];
        if (sig->is_extern) continue;
        if (sig->decl == NULL) continue;
        if (sig->decl->as.func.type_param_count > 0) continue;

        U64 entry_pc = (U64)chunk->count;
        chunk_add_func(chunk, sig->name, sig->name_buf, entry_pc);
        cg_stmt(&cg, sig->decl->as.func.body);

        // Fallback exit for functions whose bodies fall off
        // the end without a `return`. The `PUSH 0; RET` pair
        // is the same sequence an explicit `return;` emits,
        // so a fall-through function returns void.
        //
        // This pair is emitted unconditionally, even for
        // functions whose body already ends in an explicit
        // return; the pair is unreachable in those cases and
        // just costs two words per function.
        set_span(&cg, sig->decl);
        emit(&cg, OP_PUSH); emit(&cg, 0);
        emit(&cg, OP_RET);
    }
    // Imbalance sanity check. If scope_depth or loop_count
    // is nonzero at the end, a nested construct leaked a
    // scope or loop frame. That would be a codegen bug;
    // the message goes to stderr (not the bag) because it
    // indicates a compiler bug, not a source error.
    if (cg.scope_depth != 0 || cg.loop_count != 0) {
        fprintf(stderr,
            "codegen: internal imbalance — scope_depth=%d loop_count=%d\n",
            cg.scope_depth, cg.loop_count);
    }
}