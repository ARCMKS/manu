// ============================================================
// vm.c — Virtual machine execution.
//
// The dispatch loop reads one U64 word per opcode. Operands
// follow inline for opcodes that take them; the dispatcher
// advances `pc` past each word as it reads it.
//
// Error handling: every case that can fail sets `vm->error`,
// copies the current instruction's span to `vm->error_span`,
// and returns 1. The caller (main.c) renders the span with
// source context.
//
// Memory:
//   Arena      heap-allocated once in vm_init. Allocations bump
//              `ap`; scope exit restores it from `amp`.
//   RC heap    malloc-backed. Header precedes payload.
//   Const      strings materialized into a dedicated arena at
//              init. Process-lifetime; never freed until VM is.
//
// Reading order for this file:
//   1. Dispatch-loop macros (PUSH / POP / READ / RT_ERR / NATIVE_ERR).
//   2. String comparison helpers (strings_equal, strings_compare).
//   3. Sink mechanics (sink_grow, vm_sink_write, vm_sink_putc)
//      and value rendering (vm_print_value).
//   4. Arena and const-arena allocators.
//   5. Const materialization (materialize_consts).
//   6. Natives (stdout/stderr write, ffi_cstring, ffi_string,
//      ffi_error, ffi_errno, raw_null/is_null/offset/load/store,
//      ffi_alloc, ffi_free).
//   7. FFI support (ffi_type_of_kind, ffi_resolve,
//      ffi_call_dispatch).
//   8. Lifecycle (vm_init, vm_free, vm_register_native, vm_top).
//   9. vm_run — the dispatch loop.
//
// The file is one translation unit on purpose: the dispatch
// loop's RT_ERR macro and the natives all mutate the same VM
// state, and keeping them together avoids a header split that
// would have to expose every helper.
// ============================================================
#include "../include/vm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// FFI includes. The two cases are mutually exclusive;
// PRISM_FFI_AVAILABLE is defined by the Makefile when
// libffi is present. When it is not defined, the FFI
// natives degrade to runtime errors ("FFI support not
// compiled in") and the FFI code below is excluded by
// preprocessor guards.
#ifdef PRISM_FFI_AVAILABLE
#  ifdef _WIN32
#    include <windows.h>
#  else
#    include <dlfcn.h>
#  endif
#endif

// ============================================================
// Macros for the dispatch loop
// ============================================================

// PUSH: store a value at the current sp, then increment.
// The cast is to U64; callers that have an I64 are
// expected to have already cast it, or rely on C's
// conversion.
//
// No bounds check. The dispatch loop relies on codegen
// having produced bytecode that does not overflow the
// stack; the validator does not check stack depth either
// (that would require a CFG analysis). In practice a
// well-formed program cannot overflow; a buggy one can
// segfault before the VM catches it.
#define PUSH(val)   (vm->data_stack[vm->sp++] = (U64)(val))
// POP: decrement sp, then read the value. Note the
// decrement happens first, so POP reads the last pushed
// slot. No bounds check; see PUSH.
#define POP()       (vm->data_stack[--vm->sp])
// READ: return the current word and advance pc. Used for
// every operand. No bounds check; the validator ensures
// operand words are inside the code array.
#define READ()      (vm->bytecode[vm->pc++])

// Every error path uses this. The `%s` in the format string
// cases is a placeholder for "" so the trailing comma in the
// caller is always valid; the compiler removes it.
//
// Effect:
//   - snprintf into vm->error (truncated to 256 bytes).
//   - vm->error_pc is set to vm->current_pc, which the
//     dispatch loop keeps up to date.
//   - vm->error_func_idx is set to the current frame's
//     func_idx (or -1 if there are no frames).
//   - returns 1 from the enclosing function.
//
// The "return 1" is why RT_ERR can only be used inside
// functions that return int (vm_run, ffi_call_dispatch,
// ffi_resolve). It cannot be used from a void-returning
// native; that is what NATIVE_ERR is for.
#define RT_ERR(...) do { \
    snprintf(vm->error, sizeof vm->error, __VA_ARGS__); \
    vm->error_pc = vm->current_pc; \
    vm->error_func_idx = (vm->call_depth > 0) \
        ? vm->call_stack[vm->call_depth - 1].func_idx : -1; \
    return 1; \
} while (0)


// Error reporting for native functions. Sets the error fields and
// the failure flag, then returns. OP_CCALL checks the flag after
// dispatch and propagates the error into vm_run.
//
// The difference from RT_ERR is the final action: natives
// return void, so NATIVE_ERR cannot `return 1`. Instead it
// sets `native_failed` and returns; the OP_CCALL case in
// vm_run checks the flag and returns 1 if it is set.
#define NATIVE_ERR(vm, ...) do { \
    snprintf((vm)->error, sizeof (vm)->error, __VA_ARGS__); \
    (vm)->error_pc = (vm)->current_pc; \
    (vm)->error_func_idx = ((vm)->call_depth > 0) \
        ? (vm)->call_stack[(vm)->call_depth - 1].func_idx : -1; \
    (vm)->native_failed = true; \
    return; \
} while (0)
// ============================================================
// String comparison
//
// Both strings are blocks of [u64 length][bytes...]. NULL is
// treated as an empty string for comparison purposes.
//
// This is the runtime layout of a prism string: the value
// on the data stack is a pointer to the *payload* (the
// bytes after the RcHeader), and the payload starts with an
// 8-byte length followed by the bytes. NULL is used to
// represent a "missing" string and compares equal to the
// empty string, which is what makes `if s == ""` work even
// when s is a null string pointer.
// ============================================================

// Byte-equality. Both pointers are to the payload; each
// begins with a little-endian u64 length (loaded via
// memcpy so the alignment is not assumed). Two strings are
// equal iff their lengths match and (for non-empty
// strings) their bytes match.
//
// The `alen == 0 ||` short-circuit avoids calling memcmp
// with a count of 0, which is technically defined but
// often flagged by sanitizers.
static bool strings_equal(U8* a, U8* b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    U64 alen, blen;
    memcpy(&alen, a, 8);
    memcpy(&blen, b, 8);
    if (alen != blen) return false;
    return alen == 0 || memcmp(a + 8, b + 8, alen) == 0;
}

// Lexicographic comparison. Returns negative, zero, or
// positive for a < b, a == b, a > b respectively. The
// convention is the same as memcmp's: the caller only cares
// about the sign.
//
// Implementation notes:
//   - NULL sorts before any non-NULL; two NULLs are equal.
//     This is a design choice, not a language rule.
//   - The first memcmp is over the shorter length. If it
//     finds a difference, that difference is the result.
//   - If the prefixes are equal, the shorter string sorts
//     before the longer one. That is the standard
//     lexicographic rule for ASCII strings where the
//     shorter is a prefix of the longer.
static int strings_compare(U8* a, U8* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    U64 alen, blen;
    memcpy(&alen, a, 8);
    memcpy(&blen, b, 8);
    U64 n = alen < blen ? alen : blen;
    int c = n == 0 ? 0 : memcmp(a + 8, b + 8, n);
    if (c != 0) return c;
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

// ============================================================
// Value rendering
//
// Used by OP_PRINT. Recurses through TD_ARRAY; scalars render
// inline. This is the only place in the VM that renders a value.
//
// The rendering is buffered through a Sink: bytes are
// appended to `vm->sink` rather than being written
// immediately to stdout. That is what makes
// BEGIN_STRING/END_STRING work (the sink is temporarily
// redirected to a string buffer) and what makes print a
// single CCALL rather than many small writes.
// ============================================================

// Grow a sink's buffer to fit at least `need` bytes.
//
// Two cases:
//   - The sink is still using its inline buffer. Allocate a
//     heap buffer and copy the current contents over. The
//     inline buffer becomes unused (but is not freed; it is
//     part of the Sink struct).
//   - The sink is already on the heap. Realloc.
//
// Growth policy: double until the capacity is at least
// `need`. Starting from SINK_INLINE_CAP means a string that
// just barely exceeds 128 bytes gets a 256-byte allocation,
// which is the usual tradeoff between memory and realloc
// count.
static void sink_grow(Sink* s, size_t need) {
    if (need <= s->cap) return;
    size_t new_cap = s->cap ? s->cap * 2 : SINK_INLINE_CAP;
    while (new_cap < need) new_cap *= 2;

    if (s->buf == s->inline_buf) {
        // First growth from inline: allocate fresh and copy.
        s->buf = (uint8_t*)xmalloc(new_cap);
        memcpy(s->buf, s->inline_buf, s->len);
    } else {
        // Already on the heap: realloc.
        s->buf = (uint8_t*)xrealloc(s->buf, new_cap);
    }
    s->cap = new_cap;
}

// Write `n` bytes to a sink. The behavior depends on
// s->kind:
//   - STDOUT / STDERR: fwrite to s->file.
//   - STRING: append to the buffer, growing as needed.
//
// Zero-length writes are a no-op; that avoids a pointless
// fwrite and lets callers pass through without a check.
static void vm_sink_write(VM* vm, const void* data, size_t n) {
    if (n == 0) return;
    Sink* s = &vm->sink;
    switch (s->kind) {
        case SINK_STDOUT:
        case SINK_STDERR:
            fwrite(data, 1, n, s->file);
            break;
        case SINK_STRING:
            if (s->len + n > s->cap) sink_grow(s, s->len + n);
            memcpy(s->buf + s->len, data, n);
            s->len += n;
            break;
    }
}

// Convenience: write one byte. Used by vm_print_value for
// brackets and commas.
static void vm_sink_putc(VM* vm, int c) {
    char ch = (char)c;
    vm_sink_write(vm, &ch, 1);
}

// C ABI alignment of a descriptor. Used only by the extern
// struct printer, which needs to know where each field
// begins. For primitives, alignment equals size (capped at
// 8). For compound types, alignment is the maximum
// alignment of any field.
//
// This mirrors c_align_of in sema_types.c, but operates on
// the runtime descriptor instead of the compiler's Type. The
// two must agree, or the printer reads the wrong offset.
static int desc_align(VM* vm, const TypeDesc* d) {
    if (!d) return 1;
    switch (d->kind) {
        case TD_VOID:   return 1;
        case TD_I8: case TD_U8:  return 1;
        case TD_I16: case TD_U16: return 2;
        case TD_I32: case TD_U32: case TD_F32: return 4;
        case TD_I64: case TD_U64: case TD_F64: return 8;
        case TD_BOOL:
        case TD_STRING:
        case TD_REF: case TD_RC: case TD_RAWPTR: case TD_ARRAY:
        case TD_STRUCT: case TD_ENUM:
            return 8;
        case TD_EXTERN_STRUCT: {
            int max = 1;
            for (uint32_t i = 0; i < d->count; i++) {
                uint32_t fi = vm->types->fields[d->aux + i];
                int a = desc_align(vm, typetable_get(vm->types, fi));
                if (a > max) max = a;
            }
            return max;
        }
        default: return 8;
    }
}

// Render one value of descriptor `d` to the current sink.
//
// The `raw` argument is the U64 slot as it appears on the
// data stack. Its interpretation depends on d->kind:
//   - Signed ints: cast to I64 and print with %lld.
//   - Unsigned ints: print with %llu.
//   - F64: the bits are the bit pattern of a double; memcpy
//     into a double and print with %g.
//   - Bool: print "true" or "false" (raw != 0).
//   - String: raw is a pointer to a [len][bytes] payload;
//     write the bytes. NULL renders as "(null)".
//   - REF / RC / RAWPTR: print the pointer value as an
//     unsigned integer. Not an address render (no 0x
//     prefix); the intent is to be readable, not to be a
//     debugger.
//   - ARRAY: raw is a [len][elems...] payload. Recurse per
//     element. The element descriptor comes from the type
//     table via d->aux. Elements are separated by ", ".
//   - STRUCT: same layout as an array (structs are laid
//     out as [tag][field0][field1]...). Fields are
//     separated by ", ". Field descriptors come from the
//     type table's `fields` side array: d->aux is the
//     start index, and there are d->count of them.
//   - ENUM: raw is a [tag][payloads...] payload. Print the
//     variant name (looked up from the type table's
//     enum_variants side array). Out-of-range tags render
//     as "<enum #N>". Note that payloads are not printed;
//     the enum rendering is name-only. That is a
//     deliberate simplification; the runtime does not
//     carry enough per-variant payload descriptors to
//     print them generically.
//   - default: "<unprintable>".
//
// The default case should never be reached for a
// well-formed descriptor; it exists for defensive safety.
static void vm_print_value(VM* vm, const TypeDesc* d, U64 raw) {
    switch (d->kind) {
        case TD_I8: case TD_I16: case TD_I32: case TD_I64: {
            char b[32];
            int n = snprintf(b, sizeof b, "%lld", (long long)(I64)raw);
            vm_sink_write(vm, b, (size_t)n);
            break;
        }
        case TD_U8: case TD_U16: case TD_U32: case TD_U64: {
            char b[32];
            int n = snprintf(b, sizeof b, "%llu", (unsigned long long)raw);
            vm_sink_write(vm, b, (size_t)n);
            break;
        }
        case TD_F32: {
            float v; memcpy(&v, &raw, 4);
            char b[32];
            int n = snprintf(b, sizeof b, "%g", v);
            vm_sink_write(vm, b, (size_t)n);
            break;
        }        
        case TD_F64: {
            double v; memcpy(&v, &raw, 8);
            char b[32];
            int n = snprintf(b, sizeof b, "%g", v);
            vm_sink_write(vm, b, (size_t)n);
            break;
        }
        case TD_BOOL:
            if (raw) vm_sink_write(vm, "true", 4);
            else     vm_sink_write(vm, "false", 5);
            break;
        case TD_STRING: {
            U8* p = (U8*)(uintptr_t)raw;
            if (!p) { vm_sink_write(vm, "(null)", 6); break; }
            U64 len; memcpy(&len, p, 8);
            vm_sink_write(vm, p + 8, (size_t)len);
            break;
        }
        case TD_REF: case TD_RC: {
            char b[32];
            int n = snprintf(b, sizeof b, "%llu", (unsigned long long)raw);
            vm_sink_write(vm, b, (size_t)n);
            break;
        }
        case TD_RAWPTR: {
            // If the pointee is a struct (regular or extern),
            // print the struct's contents rather than the raw
            // address. `print(p)` where p is `*Point` should
            // render `{x, y}`, matching the source-level view.
            const TypeDesc* inner = typetable_get(vm->types, d->aux);
            if (inner && (inner->kind == TD_STRUCT
                          || inner->kind == TD_EXTERN_STRUCT)) {
                vm_print_value(vm, inner, raw);
                break;
            }
            char b[32];
            int n = snprintf(b, sizeof b, "%llu", (unsigned long long)raw);
            vm_sink_write(vm, b, (size_t)n);
            break;
        }
        case TD_ARRAY: {
            U8* p = (U8*)(uintptr_t)raw;
            if (!p) { vm_sink_write(vm, "(null)", 6); break; }
            U64 len; memcpy(&len, p, 8);
            const TypeDesc* ed = typetable_get(vm->types, d->aux);
            if (!ed) { vm_sink_write(vm, "<bad-array-desc>", 16); break; }
            vm_sink_putc(vm, '[');
            for (U64 i = 0; i < len; i++) {
                if (i > 0) vm_sink_write(vm, ", ", 2);
                U64 v; memcpy(&v, p + 8 + i * 8, 8);
                vm_print_value(vm, ed, v);
            }
            vm_sink_putc(vm, ']');
            break;
        }
        case TD_STRUCT: {
            U8* p = (U8*)(uintptr_t)raw;
            if (!p) { vm_sink_write(vm, "(null)", 6); break; }
            U64 len; memcpy(&len, p, 8);
            vm_sink_putc(vm, '{');
            for (U64 i = 0; i < len; i++) {
                if (i > 0) vm_sink_write(vm, ", ", 2);
                // Field descriptors live in the type table's
                // flat `fields` array: d->aux is the start
                // index for this struct.
                uint32_t fd = vm->types->fields[d->aux + i];
                const TypeDesc* fdesc = typetable_get(vm->types, fd);
                U64 v; memcpy(&v, p + 8 + i * 8, 8);
                vm_print_value(vm, fdesc, v);
            }
            vm_sink_putc(vm, '}');
            break;
        }
        case TD_EXTERN_STRUCT: {
            U8* p = (U8*)(uintptr_t)raw;
            if (!p) { vm_sink_write(vm, "(null)", 6); break; }
            vm_sink_putc(vm, '{');
            int offset = 0;
            for (uint32_t i = 0; i < d->count; i++) {
                if (i > 0) vm_sink_write(vm, ", ", 2);
                uint32_t fi = vm->types->fields[d->aux + i];
                const TypeDesc* fd = typetable_get(vm->types, fi);
                int align = desc_align(vm, fd);
                offset = (offset + align - 1) & ~(align - 1);

                if (fd && fd->kind == TD_EXTERN_STRUCT) {
                    // Nested extern struct: the field's bytes
                    // are the struct itself, not a pointer.
                    // Recurse on the field's address so the
                    // nested printer sees a valid pointer.
                    vm_print_value(vm, fd,
                                   (U64)(uintptr_t)(p + offset));
                } else {
                    int bytes = fd ? (int)fd->size : 8;
                    if (bytes > 8) bytes = 8;
                    if (bytes < 1) bytes = 8;
                    U64 v = 0;
                    memcpy(&v, p + offset, (size_t)bytes);
                    if (fd) vm_print_value(vm, fd, v);
                }

                int advance = fd ? (int)fd->size : 8;
                if (advance < 1) advance = 8;
                offset += advance;
            }
            vm_sink_putc(vm, '}');
            break;
        }
        case TD_ENUM: {
            U8* p = (U8*)(uintptr_t)raw;
            if (!p) { vm_sink_write(vm, "(null)", 6); break; }
            U64 tag; memcpy(&tag, p, 8);
            if (tag >= d->count) {
                // Out-of-range tag: the payload's tag word
                // is not a valid variant. Render a
                // placeholder rather than crash.
                char b[32];
                int n = snprintf(b, sizeof b, "<enum #%llu>",
                                 (unsigned long long)tag);
                vm_sink_write(vm, b, (size_t)n);
                break;
            }
            const EnumVariantDesc* ev = &vm->types->enum_variants[d->aux + tag];
            vm_sink_write(vm, ev->name_buf, strlen(ev->name_buf));

            // Variant payloads. The descriptors live in the
            // TypeTable's flat enum_payloads side array, one
            // entry per payload, starting at payload_start.
            // Each payload occupies an 8-byte slot after the
            // tag. Recurse for compound payloads (structs,
            // nested enums), so `Some(Point{x, y})` renders as
            // `Some({x, y})`.
            if (ev->payload_count > 0) {
                vm_sink_putc(vm, '(');
                for (uint32_t i = 0; i < ev->payload_count; i++) {
                    if (i > 0) vm_sink_write(vm, ", ", 2);
                    uint32_t pd_idx = vm->types->enum_payloads[
                        ev->payload_start + i];
                    const TypeDesc* pd = typetable_get(vm->types, pd_idx);
                    U64 pv;
                    memcpy(&pv, p + 8 + i * 8, 8);
                    if (pd) vm_print_value(vm, pd, pv);
                }
                vm_sink_putc(vm, ')');
            }
            break;
        }

        default:
            vm_sink_write(vm, "<unprintable>", 12);
    }
}

// ============================================================
// Arena and const-arena allocators
// ============================================================

// Bump-allocate `size` bytes from the lexical arena, 8-byte
// aligned. Returns NULL on exhaustion.
//
// Alignment: the current ap is rounded up to the next
// multiple of 8 before the request is served. That is what
// makes storing doubles and pointers in the arena safe.
//
// The bounds check is against the arena's end (arena +
// ARENA_MAX). If the aligned offset plus size would exceed
// that, NULL is returned. The caller is expected to report
// "arena out of memory"; the VM does not have a way to
// grow the arena.
static U8* arena_alloc(VM* vm, U64 size) {
    uintptr_t p = ((uintptr_t)vm->ap + 7u) & ~(uintptr_t)7u;
    if (p + size > (uintptr_t)vm->arena + ARENA_MAX) return NULL;
    vm->ap = (U8*)(p + size);
    return (U8*)p;
}

// Bump-allocate `size` bytes from the const arena. Strings are
// materialized once at init and never freed.
//
// Same alignment and bounds logic as arena_alloc, but the
// const arena's size is whatever was computed at init (see
// materialize_consts), not a fixed ARENA_MAX.
static U8* const_alloc(VM* vm, U64 size) {
    uintptr_t p = ((uintptr_t)vm->cap + 7u) & ~(uintptr_t)7u;
    if (p + size > (uintptr_t)vm->const_arena + vm->const_arena_size)
        return NULL;
    vm->cap = (U8*)(p + size);
    return (U8*)p;
}

// ============================================================
// Const materialization
//
// Strings are stored in the pool as (bytes, length). At VM
// init, each is copied into the const arena as
// [u64 length][bytes…], and the pointer to that block is
// stored in `const_ptrs[idx]`. Non-string constants get a NULL
// entry. LOAD_CONST uses const_ptrs only for strings.
//
// The const arena is sized to hold exactly the pool's
// constants, with 8-byte alignment between entries. Two
// layout cases:
//
//   CONST_STRING  [RcHeader][u64 length][bytes]
//   CONST_CSTRING [bytes][NUL]
//
// The RcHeader on a CONST_STRING has ref_count set to the
// sentinel 0xFFFFFFFF, which makes RC_INC and RC_DEC on the
// payload a no-op. This is what allows a string literal to
// be dropped uniformly with a runtime-produced string
// without the RC system trying to free it.
//
// CSTRINGs have no header: they are the raw NUL-terminated
// bytes that a C function would accept. There is no RC
// machinery around them; a CSTRING is always borrowed (it
// lives in the const arena and is never freed).
// ============================================================
static void materialize_consts(VM* vm) {
    if (!vm->pool || vm->pool->count == 0) return;

    // Compute total const-arena size exactly. The two
    // contributions per entry:
    //   CONST_STRING:  RcHeader + 8-byte length + bytes
    //   CONST_CSTRING: bytes + 1 for the NUL
    // Every entry is then rounded up to 8 bytes, matching
    // the alignment that const_alloc will apply.
    size_t total = 0;
    for (int i=0; i<vm->pool->count; i++) {
        const Constant* c = &vm->pool->items[i];
        if (c->kind == CONST_STRING)
            total += sizeof(RcHeader) + 8 + c->as.str.length;
        if (c->kind == CONST_CSTRING)
            total += 1 + c->as.str.length; 
        total = (total + 7) & ~(size_t)7;
    }
    
    if (total == 0) return;
    if (total > CONST_ARENA_MAX) {
        // Too many/large constants. This is a hard error
        // because there is no way to grow the const arena
        // after init. exit(1) rather than a diagnostic
        // because at this point the VM cannot continue.
        fprintf(stderr, "prism: const arena overflow (%zu > %d)\n",
                total, CONST_ARENA_MAX);
        exit(1);
    }

    // Allocate the const arena and the per-pool-entry
    // pointer array. The arena is zeroed (xcalloc) so
    // padding bytes are deterministic; the pointer array
    // starts all-NULL.
    vm->const_arena = (U8*)xcalloc(total, 1);
    vm->cap = vm->const_arena;
    vm->const_ptrs = (U8**)xcalloc((size_t)vm->pool->count, sizeof(U8*));
    vm->const_arena_size  = total;

    for (int i = 0; i < vm->pool->count; i++) {
        const Constant* c = &vm->pool->items[i];
        if (c->kind == CONST_STRING) {
            U64 len = (U64)c->as.str.length;
            U8* block = const_alloc(vm, sizeof(RcHeader) + 8 + len);
            if (!block) {
                fprintf(stderr, "prism: const arena overflow\n");
                exit(1);
            }
            RcHeader* h = (RcHeader*)block;
            h->ref_count = 0xFFFFFFFF;  // SENTINEL, never freed, never counted
            h->padding = 0;
            U8* p = (U8*)(h+1);
            memcpy(p, &len, 8);
            memcpy(p + 8, c->as.str.bytes, (size_t)len);
            vm->const_ptrs[i] = p;
        } else if (c->kind == CONST_CSTRING) {         
            U64 len = (U64)c->as.str.length;
            U8* p = const_alloc(vm, len + 1);
            if (!p) { fprintf(stderr, "prism: const arena overflow\n"); exit(1); }
            memcpy(p, c->as.str.bytes, (size_t)len);
            p[len] = '\0';
            vm->const_ptrs[i] = p;
        }
    }
}

// Write a string to the VM's current stdout FILE*.
//
// Stack effect: pops one value (the string pointer). The
// string is a [len][bytes] payload; the length prefix is
// read first, then the bytes are written.
//
// NULL is treated as "nothing to write" (no error, no
// bytes). This is the reason `print(ffi_error())` does not
// crash when there is no FFI error yet.
static void native_stdout_write(VM* vm) {
    U8* p = (U8*)(uintptr_t)POP();
    if (!p) return;
    U64 n; memcpy(&n, p, 8);
    FILE* f = vm->out ? vm->out : stdout;
    fwrite(p + 8, 1, (size_t)n, f);
    fflush(f);
}
// Same as stdout_write but to stderr unconditionally. Note
// the `vm->out` field is not consulted; that is deliberate,
// because `out` is the VM's stdout-redirect hook and
// stderr should not be affected by it.
static void native_stderr_write(VM* vm) {
    U8* p = (U8*)(uintptr_t)POP();
    if (!p) return;
    U64 n; memcpy(&n, p, 8);
    fwrite(p + 8, 1, (size_t)n, stderr);
    fflush(stderr);
}

// Convert a prism string to a C string. The result is a
// NUL-terminated buffer in the *lexical arena*, so it lives
// until the enclosing scope exits. That is the right
// lifetime for a string handed to a C function that will
// copy or use it before returning.
//
// Stack effect: pops one (prism string), pushes one
// (C string pointer). NULL pops to NULL pushes; the empty
// case is a 1-byte allocation with a NUL.
//
// The arena allocation can fail (arena full); that is
// reported via NATIVE_ERR. The resulting pointer is
// *not* an RC-managed block; it has no header, so it
// cannot be passed to RC_INC / RC_DEC. It is a bare
// buffer.
static void native_ffi_cstring(VM* vm) {
    if (vm->sp == 0)
        NATIVE_ERR(vm, "ffi_cstring on empty stack%s", "");
    U8* s = (U8*)(uintptr_t)POP();
    if (!s) { PUSH(0); return; }
    U64 len; memcpy(&len, s, 8);
    U8* block = arena_alloc(vm, len + 1);
    if (!block)
        NATIVE_ERR(vm, "arena out of memory for cstring%s", "");
    memcpy(block, s + 8, (size_t)len);
    block[len] = '\0';
    PUSH((U64)(uintptr_t)block);
}

// Convert a C string to a prism string. The result is a
// fresh RC-managed string on the RC heap.
//
// Stack effect: pops one (C string pointer), pushes one
// (prism string). NULL pops to a zero-length string push
// (the runtime representation of the empty string is a
// valid RC block with length 0).
//
// The C string's length is found by walking to the first
// NUL. If the C string is not NUL-terminated, this walks
// off the end; that is a caller bug, not something the
// VM can defend against (there is no length to check).
static void native_ffi_string(VM* vm) {
    if (vm->sp == 0)
        NATIVE_ERR(vm, "ffi_string on empty stack%s", "");
    U8* src = (U8*)(uintptr_t)POP();
    U64 len = 0;
    if (src) while (src[len] != 0) len++;
    RcHeader* h = (RcHeader*)malloc(sizeof(RcHeader) + 8 + len);
    if (!h)
        NATIVE_ERR(vm, "rc heap out of memory for ffi_string%s", "");
    h->ref_count = 1;
    h->padding   = 0;
    U8* block = (U8*)(h + 1);
    memcpy(block, &len, 8);
    if (len > 0) memcpy(block + 8, src, (size_t)len);
    PUSH((U64)(uintptr_t)block);
}

// Build a string from the VM's `ffi_error` buffer (the last
// dlerror / dlopen / dlsym message). The buffer is a C
// string in the VM struct; this converts it to a prism
// string the same way ffi_string does.
//
// Stack effect: pushes one (prism string). No operands
// popped.
static void native_ffi_error(VM* vm) {
    U64 len = (U64)strlen(vm->ffi_error);
    RcHeader* h = (RcHeader*)malloc(sizeof(RcHeader) + 8 + len);
    if (!h)
        NATIVE_ERR(vm, "rc heap out of memory for ffi_error%s", "");
    h->ref_count = 1;
    h->padding   = 0;
    U8* block = (U8*)(h + 1);
    memcpy(block, &len, 8);
    memcpy(block + 8, vm->ffi_error, (size_t)len);
    PUSH((U64)(uintptr_t)block);
}

// Push the last captured errno. Set after every ffi call
// (see ffi_call_dispatch). No operands popped.
static void native_ffi_errno(VM* vm) {
    PUSH((U64)(I64)vm->ffi_errno);
}

// Push a null pointer (0).
static void native_raw_null(VM* vm) {
    PUSH(0);
}

// Test a pointer for null. Stack: pop pointer, push 0 or 1.
static void native_raw_is_null(VM* vm) {
    if (vm->sp == 0)
        NATIVE_ERR(vm, "raw_is_null on empty stack%s", "");
    U64 v = POP();
    PUSH(v == 0 ? 1 : 0);
}

// Compute `ptr + n * elem_size`. Three operands, pushed in
// this order: ptr, n, elem_size. elem_size is on top so the
// first POP retrieves it.
//
// The three-operand signature exists so that the caller
// (codegen) can compute the element size at compile time
// from the pointer's type. The VM does not know the type;
// it just multiplies.
//
// Overflow is not checked. A caller passing a huge n can
// produce a wrapped pointer; that is the caller's problem.
static void native_raw_offset(VM* vm) {
    if (vm->sp < 3)
        NATIVE_ERR(vm, "raw_offset needs 3 operands%s", "");
    U64 elem_size = POP();
    I64 n         = (I64)POP();
    U64 p         = POP();
    PUSH((U64)((I64)p + n * (I64)elem_size));
}

// Read one byte at `ptr + i`. Two operands: ptr, i (i on
// top). NULL ptr is an error.
//
// The byte is zero-extended to u64 on push; the caller's
// sema has declared the result as u8, but the stack slot is
// always 64 bits wide.
static void native_raw_load_u8(VM* vm) {
    if (vm->sp < 2)
        NATIVE_ERR(vm, "raw_load_u8 needs 2 operands%s", "");
    I64 i = (I64)POP();
    U8* p = (U8*)(uintptr_t)POP();
    if (!p)
        NATIVE_ERR(vm, "raw_load_u8 through null%s", "");
    PUSH((U64)p[i]);
}

// Write one byte at `ptr + i`. Three operands: ptr, i,
// value (value on top). NULL ptr is an error. Pushes 0 (the
// void result) to preserve the stack-balance invariant.
static void native_raw_store_u8(VM* vm) {
    if (vm->sp < 3)
        NATIVE_ERR(vm, "raw_store_u8 needs 3 operands%s", "");
    U64 v = POP();
    I64 i = (I64)POP();
    U8* p = (U8*)(uintptr_t)POP();
    if (!p)
        NATIVE_ERR(vm, "raw_store_u8 through null%s", "");
    p[i] = (U8)v;
    PUSH(0);
}

/* ffi_alloc(size) -> *void
 *
 * Pops a size and returns a pointer to a zero-filled block of that
 * many bytes from the C heap. Never returns NULL for size zero; a
 * zero-size request is rounded to one byte. The allocation is not
 * tracked by the RC system; it is released only by ffi_free. */
static void native_ffi_alloc(VM* vm) {
    if (vm->sp == 0)
        NATIVE_ERR(vm, "ffi_alloc on empty stack%s", "");
    U64 size = POP();
    if (size == 0) size = 1;
    void* p = calloc(1, (size_t)size);
    if (!p)
        NATIVE_ERR(vm, "ffi_alloc: out of memory (%llu bytes)",
                   (unsigned long long)size);
    PUSH((U64)(uintptr_t)p);
}

/* ffi_free(p) -> void
 *
 * Pops a pointer and frees it. NULL is a no-op so callers can free
 * unconditionally after a fallible path. Leaves one value on the
 * stack (0), preserving the expression invariant. */
static void native_ffi_free(VM* vm) {
    if (vm->sp == 0)
        NATIVE_ERR(vm, "ffi_free on empty stack%s", "");
    void* p = (void*)(uintptr_t)POP();
    if (p) free(p);
    PUSH(0);
}

// Build rc<[string]> from vm->argv. Each element is a fresh
// RC string with a copy of the argument's bytes. The array
// itself is a fresh RC block with refcount 1.
//
// With argc == 0, an empty array is still returned (length 0,
// no elements). This is not the same as NULL; the caller can
// iterate it safely.
static void native_args(VM* vm) {
    int argc = vm->argc;
    if (argc < 0) argc = 0;
    // An upper bound that matches the driver's array size.
    // If vm->argc is ever set wrong, the loop stops here
    // rather than reading past the end of cli.prog_argv.
    if (argc > 64) argc = 64;

    U64 payload_bytes = 8 + (U64)argc * 8;
    RcHeader* ah = (RcHeader*)malloc(sizeof(RcHeader) + payload_bytes);
    if (!ah) NATIVE_ERR(vm, "out of memory for args%s", "");
    ah->ref_count = 1;
    ah->padding   = 0;
    U8* apay = (U8*)(ah + 1);
    U64 len64 = (U64)argc;
    memcpy(apay, &len64, 8);

    for (int i = 0; i < argc; i++) {
        const char* arg = (vm->argv && vm->argv[i]) ? vm->argv[i] : "";
        U64 alen = (U64)strlen(arg);
        RcHeader* sh = (RcHeader*)malloc(sizeof(RcHeader) + 8 + alen);
        if (!sh) {
            // The array block is already allocated; free it
            // before reporting, so a failed args() does not
            // leak.
            free(ah);
            NATIVE_ERR(vm, "out of memory for args%s", "");
        }
        sh->ref_count = 1;
        sh->padding   = 0;
        U8* spay = (U8*)(sh + 1);
        memcpy(spay, &alen, 8);
        if (alen) memcpy(spay + 8, arg, alen);
        U64 sp = (U64)(uintptr_t)spay;
        memcpy(apay + 8 + (U64)i * 8, &sp, 8);
    }
    PUSH((U64)(uintptr_t)apay);
}

// Read one line from vm->in into a fresh RC string, wrapped
// in Option.
//
// Semantics:
//   - \n terminates a line; the trailing \n is stripped.
//   - \r\n is treated as \n (the \r is stripped if present
//     immediately before the \n).
//   - EOF with no bytes read returns Option.None.
//   - EOF after bytes read returns Option.Some(last_line);
//     the next call returns Option.None.
//   - An empty line ("\n") returns Option.Some("").
//
// The result is rc<Option#string>: an RC block with tag 0
// (Some) and a payload pointer to an RC string, or tag 1
// (None) with no payload.
static void native_read_line(VM* vm) {
    FILE* f = vm->in ? vm->in : stdin;

    size_t cap = 128;
    size_t n   = 0;
    char*  buf = (char*)xmalloc(cap);
    bool   got_any = false;
    int    c = EOF;

    while ((c = fgetc(f)) != EOF) {
        got_any = true;
        if (c == '\n') break;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = (char*)xrealloc(buf, cap);
        }
        buf[n++] = (char)c;
    }

    // Strip a CR that immediately preceded the LF.
    if (n > 0 && buf[n - 1] == '\r') n--;

    // Immediate EOF with no bytes: Option.None.
    if (n == 0 && !got_any) {
        free(buf);
        RcHeader* h = (RcHeader*)malloc(sizeof(RcHeader) + 8);
        if (!h) NATIVE_ERR(vm, "out of memory for read_line%s", "");
        h->ref_count = 1;
        h->padding   = 0;
        U8* p = (U8*)(h + 1);
        U64 tag = 1;
        memcpy(p, &tag, 8);
        PUSH((U64)(uintptr_t)p);
        return;
    }

    // Build the line as an RC string.
    RcHeader* sh = (RcHeader*)malloc(sizeof(RcHeader) + 8 + n);
    if (!sh) {
        free(buf);
        NATIVE_ERR(vm, "out of memory for read_line%s", "");
    }
    sh->ref_count = 1;
    sh->padding   = 0;
    U8* spay = (U8*)(sh + 1);
    U64 n64 = (U64)n;
    memcpy(spay, &n64, 8);
    if (n) memcpy(spay + 8, buf, n);
    free(buf);

    // Wrap in Option.Some(ptr): [tag=0][string_ptr].
    RcHeader* oh = (RcHeader*)malloc(sizeof(RcHeader) + 16);
    if (!oh) NATIVE_ERR(vm, "out of memory for read_line%s", "");
    oh->ref_count = 1;
    oh->padding   = 0;
    U8* opay = (U8*)(oh + 1);
    U64 tag = 0;
    memcpy(opay, &tag, 8);
    U64 sp = (U64)(uintptr_t)spay;
    memcpy(opay + 8, &sp, 8);
    PUSH((U64)(uintptr_t)opay);
}

// Read all of stdin to EOF as a single RC string. Returns
// the empty string if no bytes were available.
static void native_read_all(VM* vm) {
    FILE* f = vm->in ? vm->in : stdin;

    size_t cap = 4096;
    size_t n   = 0;
    char*  buf = (char*)xmalloc(cap);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            buf = (char*)xrealloc(buf, cap);
        }
        size_t r = fread(buf + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }

    RcHeader* sh = (RcHeader*)malloc(sizeof(RcHeader) + 8 + n);
    if (!sh) {
        free(buf);
        NATIVE_ERR(vm, "out of memory for read_all%s", "");
    }
    sh->ref_count = 1;
    sh->padding   = 0;
    U8* spay = (U8*)(sh + 1);
    U64 n64 = (U64)n;
    memcpy(spay, &n64, 8);
    if (n) memcpy(spay + 8, buf, n);
    free(buf);
    PUSH((U64)(uintptr_t)spay);
}

// Begin a string build. Pushes the current sink and installs
// a fresh string sink. Balanced by native_str_end. Nested
// builds are legal; each push/pop pair is one level.
static void native_str_begin(VM* vm) {
    if (vm->sink_depth >= SINK_STACK_MAX)
        NATIVE_ERR(vm, "sink stack overflow (max %d)", SINK_STACK_MAX);
    vm->sink_stack[vm->sink_depth++] = vm->sink;

    memset(&vm->sink, 0, sizeof vm->sink);
    vm->sink.kind = SINK_STRING;
    vm->sink.buf  = vm->sink.inline_buf;
    vm->sink.cap  = SINK_INLINE_CAP;
}

// Finish a string build. Materializes the accumulated bytes
// into a fresh RC string and pushes it, then restores the
// previous sink. The result is a normal Prism string.
static void native_str_end(VM* vm) {
    if (vm->sink_depth == 0 || vm->sink.kind != SINK_STRING)
        NATIVE_ERR(vm, "str_end without matching str_begin");

    U64 n = (U64)vm->sink.len;
    RcHeader* h = (RcHeader*)malloc(sizeof(RcHeader) + 8 + n);
    if (!h) {
        if (vm->sink.buf != vm->sink.inline_buf)
            free(vm->sink.buf);
        NATIVE_ERR(vm, "out of memory building string");
    }
    h->ref_count = 1;
    h->padding   = 0;
    U8* blk = (U8*)(h + 1);
    memcpy(blk, &n, 8);
    memcpy(blk + 8, vm->sink.buf, (size_t)n);
    if (vm->sink.buf != vm->sink.inline_buf)
        free(vm->sink.buf);

    vm->sink = vm->sink_stack[--vm->sink_depth];
    PUSH((U64)(uintptr_t)blk);
}

// Append one value to the current sink. The stack layout is
// [value][desc_idx] with desc_idx on top; the native pops the
// desc, then the value. The descriptor selects the renderer.
static void native_print_value(VM* vm) {
    if (vm->sp < 2)
        NATIVE_ERR(vm, "print_value needs 2 operands%s", "");
    U64 desc_idx = POP();
    U64 value    = POP();
    const TypeDesc* d = typetable_get(vm->types, (U32)desc_idx);
    if (!d) NATIVE_ERR(vm, "bad print desc %llu",
                       (unsigned long long)desc_idx);
    vm_print_value(vm, d, value);
}

// Arena-allocate `size` zeroed bytes and push the pointer.
// Used by extern struct literals. Zeroing matters because
// arena memory is reused across scope exits: `ap` is reset
// but the bytes below are not cleared. A fresh allocation
// must not see stale data from a previous scope.
static void native_arena_zero(VM* vm) {
    if (vm->sp == 0)
        NATIVE_ERR(vm, "arena_zero on empty stack%s", "");
    U64 size = POP();
    if (size == 0) size = 1;
    U8* p = arena_alloc(vm, size);
    if (!p)
        NATIVE_ERR(vm, "arena out of memory (%llu bytes)",
                   (unsigned long long)size);
    memset(p, 0, (size_t)size);
    PUSH((U64)(uintptr_t)p);
}

// memcpy(dst, src, n) -> void. Three operands: dst, src, n
// with n on top. Pushes 0 to preserve the stack-balance
// invariant.
static void native_memcpy(VM* vm) {
    if (vm->sp < 3) NATIVE_ERR(vm, "memcpy needs 3 operands%s", "");
    U64 n   = POP();
    U64 src = POP();
    U64 dst = POP();
    if (dst && src && n) memcpy((void*)(uintptr_t)dst,
                                (void*)(uintptr_t)src, (size_t)n);
    PUSH(0);
}
// ------------------------------------------------------------
// FFI support
// ------------------------------------------------------------
//
// When PRISM_FFI_AVAILABLE is defined (the Makefile set it),
// libffi is used for the call. Otherwise all FFI natives
// fail at runtime with a clear message; the compile path
// still accepts `extern fn` declarations but execution of
// any call to them will abort.
//
// The flow for one FFI call:
//   1. ffi_call_dispatch is entered from OP_FFI_CALL with a
//      function index and an argument count.
//   2. If the FFIFunc's cif is not ready, ffi_resolve loads
//      the library (if not already loaded) and resolves the
//      symbol, then prepares the cif.
//   3. Pop the arguments off the data stack. Option<*T>-
//      marked args are unwrapped from their RC enum to a raw
//      pointer (None -> NULL, Some -> payload).
//   4. ffi_call with the prepared cif.
//   5. Push the result. If the function returns an
//      Option<*T>, wrap it: NULL becomes Option.None (tag 1),
//      non-NULL becomes Option.Some(ptr) (tag 0). Otherwise
//      push the raw result, narrowing it per the declared
//      FFI return type.
#ifdef PRISM_FFI_AVAILABLE
// Map an FFIType to a libffi type descriptor. libffi uses
// static ffi_type objects (one per C type); this function
// returns the right one.
//
// The BOOL mapping to ffi_type_uint8 matches the runtime
// convention (bools are passed as one-byte values) and
// mirrors what most C ABIs do for `_Bool`.
//
// An unrecognized FFIType returns ffi_type_void; that is
// defensive and should not be reached for a well-formed
// extern declaration (sema would have rejected it).
static ffi_type* ffi_type_of_kind(FFIType t) {
    switch (t) {
        case FFI_VOID: return &ffi_type_void;
        case FFI_I8:   return &ffi_type_sint8;
        case FFI_I16:  return &ffi_type_sint16;
        case FFI_I32:  return &ffi_type_sint32;
        case FFI_I64:  return &ffi_type_sint64;
        case FFI_U8:   return &ffi_type_uint8;
        case FFI_U16:  return &ffi_type_uint16;
        case FFI_U32:  return &ffi_type_uint32;
        case FFI_U64:  return &ffi_type_uint64;
        case FFI_F32:  return &ffi_type_float;
        case FFI_F64:  return &ffi_type_double;
        case FFI_BOOL: return &ffi_type_uint8;
        case FFI_PTR:  return &ffi_type_pointer;
    }
    return &ffi_type_void;
}
#endif

// Resolve one FFIFunc: load its library (if not already
// loaded), find the symbol, and prepare the libffi cif.
//
// Returns 0 on success, or 1 (with RT_ERR having fired) on
// failure. On success, the FFIFunc's fn_ptr and cif_ready
// fields are filled; the next call can skip this step.
//
// Library loading: LoadLibraryA on Windows, dlopen with
// RTLD_LAZY | RTLD_GLOBAL on Unix. The RTLD_GLOBAL flag
// makes symbols from this library visible to later-loaded
// libraries, which matches the behavior of a normal
// process that links all libraries at startup. RTLD_LAZY
// defers symbol resolution until first use.
//
// The handle is cached on the FFILib; on a second call to
// the same library, the load is skipped. Handles are never
// closed (they live for the process lifetime).
//
// cif preparation: two forms. Non-variadic uses ffi_prep_cif
// with the fixed argument types. Variadic uses
// ffi_prep_cif_var with the fixed count and the (initial)
// total count. When a later variadic call has a different
// total count, ffi_call_dispatch rebuilds the cif (see
// below).
static int ffi_resolve(VM* vm, FFIFunc* f) {
#ifdef PRISM_FFI_AVAILABLE
    FFILib* lib = &((FFITable*)vm->ffi)->libs[f->lib_idx];
    if (!lib->loaded) {
#ifdef _WIN32
        lib->handle = LoadLibraryA(lib->path);
        if (!lib->handle)
            RT_ERR("cannot load '%s' (error %lu)",
                   lib->path, (unsigned long)GetLastError());
#else
        lib->handle = dlopen(lib->path, RTLD_LAZY | RTLD_GLOBAL);
        if (!lib->handle) {
            // Capture dlerror() into the VM state so
            // ffi_error() can surface it.
            const char* e = dlerror();
            snprintf(vm->ffi_error, sizeof vm->ffi_error, "%s",
                     e ? e : "unknown dlopen error");
            RT_ERR("cannot load '%s': %s", lib->path, vm->ffi_error);
        }
#endif
        lib->loaded = true;
    }
    // Symbol resolution. dlerror() is cleared before dlsym
    // so the subsequent dlerror() call (if dlsym returns
    // NULL) reports only the current failure, not a stale
    // one from a previous resolve.
    #ifdef _WIN32
        f->fn_ptr = (void*)GetProcAddress((HMODULE)lib->handle, f->symbol);
        if (!f->fn_ptr) {
            // On 32-bit Windows, the cdecl C ABI decorates symbol
            // names with a leading underscore. 64-bit Windows does
            // not. Trying the decorated form first-then-fallback
            // costs one GetProcAddress and makes the VM work on
            // both.
            char decorated[128];
            snprintf(decorated, sizeof decorated, "_%s", f->symbol);
            f->fn_ptr = (void*)GetProcAddress((HMODULE)lib->handle, decorated);
        }
        if (!f->fn_ptr)
            RT_ERR("symbol '%s' not found in '%s'", f->symbol, lib->path);
    #else
    dlerror();
    f->fn_ptr = dlsym(lib->handle, f->symbol);
    if (!f->fn_ptr) {
        const char* e = dlerror();
        snprintf(vm->ffi_error, sizeof vm->ffi_error, "%s",
                 e ? e : "unknown dlsym error");
        RT_ERR("symbol '%s' not found in '%s': %s",
               f->symbol, lib->path, vm->ffi_error);
    }
#endif

    // Fill the libffi type array for the fixed args. The
    // array is sized 16 (max args the VM accepts) and lives
    // on the FFIFunc, because libffi's cif stores the
    // pointer to it, not a copy.
    for (int i = 0; i < f->arg_count; i++)
        f->ffi_arg_types[i] = ffi_type_of_kind(f->arg_types[i]);
    
    ffi_status st;
    if (f->is_variadic)
        // Variadic: arg_count fixed args + 0 extras on the
        // first resolve. The extras will be added on the
        // first call if the call passes more.
        st = ffi_prep_cif_var(&f->cif, FFI_DEFAULT_ABI,
                            f->arg_count, f->arg_count,
                            ffi_type_of_kind(f->ret_type),
                            f->ffi_arg_types);
    else
        st = ffi_prep_cif(&f->cif, FFI_DEFAULT_ABI, f->arg_count,
                        ffi_type_of_kind(f->ret_type),
                        f->ffi_arg_types);
    if (st != FFI_OK) RT_ERR("libffi cif prep failed for '%s'", f->symbol);
    f->cif_ready    = true;
    f->last_varargc = f->arg_count;
    return 0;
#else
    // No FFI support compiled in.
    (void)vm; (void)f;
    RT_ERR("FFI support not compiled in");
#endif
}

// Perform one FFI call.
//
// Steps:
//   1. Resolve the cif on first use (ffi_resolve).
//   2. Bounds-check the function index and the arg count.
//   3. If variadic and the arg count differs from the last
//      prepared cif, rebuild the cif with the actual count.
//      The variadic tail is typed as sint64 (the C default
//      integer promotion for integers; pointers are the
//      same width, and floats would need explicit
//      promotion, which is why sema rejects f32 in
//      variadic position).
//   4. Pop `argc` args off the data stack into a slots
//      array, unwrapping Option<*T> for the marked
//      positions.
//   5. Clear errno, call ffi_call, capture the new errno.
//   6. Push the result, wrapping or narrowing per the
//      declared FFI return.
//
// Return value: 0 on success, 1 on error (RT_ERR fired).
static int ffi_call_dispatch(VM* vm, U32 fidx, U32 argc) {
    // Bounds checks FIRST: the two guards below must run
    // before we take a pointer into vm->ffi->funcs[], which
    // would otherwise be an out-of-bounds read on a bad
    // fidx. (Previously the pointer was taken first and the
    // check ran afterwards, which is a memory-safety bug.)
    if (!vm->ffi || fidx >= (U32)vm->ffi->func_count)
        RT_ERR("bad ffi function index %u", fidx);
    if (argc > 16)
        RT_ERR("ffi call with %u args exceeds 16", argc);
    
    FFIFunc* f = (FFIFunc*)&vm->ffi->funcs[fidx];
    if (!f->cif_ready) {
        int rc = ffi_resolve(vm, f);
        //fprintf(stderr, "[ffi] resolve ffi#%u '%s' -> rc=%d fn_ptr=%p\n",
        //        fidx, f->symbol, rc, f->fn_ptr);
        if (rc != 0) return 1;
    }
    
    // Defensive bounds check. This runs after resolve
    // because the resolve path also uses `f` (and would
    // have segfaulted if fidx were bad). The order means
    // an out-of-bounds fidx can be a memory violation on
    // the resolve path; that is the accepted risk.
    if (!vm->ffi || fidx >= (U32)vm->ffi->func_count)
        RT_ERR("bad ffi function index %u", fidx);
    if (argc > 16)
        RT_ERR("ffi call with %u args exceeds 16", argc);

    // Rebuild the cif for a variadic call whose argument
    // count has changed. The fixed args keep their types;
    // the extras are typed as sint64.
    if (f->is_variadic && (int)argc != f->last_varargc) {
        for (int i = 0; i < f->arg_count; i++)
            f->ffi_arg_types[i] = ffi_type_of_kind(f->arg_types[i]);
        for (int i = f->arg_count; i < (int)argc; i++)
            f->ffi_arg_types[i] = &ffi_type_sint64;

        ffi_status st = ffi_prep_cif_var(&f->cif, FFI_DEFAULT_ABI,
                                         f->arg_count, argc,
                                         ffi_type_of_kind(f->ret_type),
                                         f->ffi_arg_types);
        if (st != FFI_OK)
            RT_ERR("libffi vararg cif prep failed for '%s'", f->symbol);
        f->last_varargc = (int)argc;
    }

    // Pop args. They are on the stack in source order
    // (leftmost pushed first), so popping reverses them.
    // The `dst = argc - 1 - i` computation puts each
    // popped value into the correct slot in the final
    // order.
    //
    // For an Option<*T> parameter, the value on the
    // stack is an RC enum pointer. The unwrap reads the
    // tag word; tag 0 (Some) reads the payload as a
    // pointer; tag 1 (None) becomes NULL. The resulting
    // raw pointer is what the C function receives.
    uint64_t slots[16];
    void*    argv[16];
    for (U32 i = 0; i < argc; i++) {
        U32 dst = argc - 1 - i;
        U64 raw = POP();
        if ((int)dst < f->arg_count && f->arg_is_option_ptr[dst]) {
            // Unwrap Option<*T> → raw pointer. None → NULL.
            if (raw == 0) {
                slots[dst] = 0;
            } else {
                U8* p = (U8*)(uintptr_t)raw;
                U64 tag;
                memcpy(&tag, p, 8);
                if (tag == 0) {
                    U64 payload;
                    memcpy(&payload, p + 8, 8);
                    slots[dst] = payload;
                } else {
                    slots[dst] = 0;
                }
            }
        } else {
            slots[dst] = raw;
        }
        // libffi wants an array of pointers to the
        // argument values, not the values themselves.
        argv[dst] = &slots[dst];
    }

    uint64_t result = 0;
#ifdef PRISM_FFI_AVAILABLE
    // Reset errno before the call so a successful call
    // does not leave a stale errno from an earlier
    // operation. Capture the post-call errno immediately.
    errno = 0;
    if (!f->fn_ptr)
        RT_ERR("ffi '%s' has no resolved function pointer", f->symbol);
    /*if (argc > 0 && slots[0]) {
        U8* ap = (U8*)(uintptr_t)slots[0];
        fprintf(stderr, "[ffi] arg0=%p bytes=", (void*)ap);
        for (int k = 0; k < 12; k++) fprintf(stderr, "%02x ", ap[k]);
        fprintf(stderr, "\n");
        }*/ // debug print
    ffi_call(&f->cif, FFI_FN(f->fn_ptr), &result, argv);

    vm->ffi_errno = errno;

    // Result handling. Three cases:
    //
    //   returns_option_ptr: wrap. A NULL result becomes
    //   Option.None (tag 1) with a single-word payload;
    //   non-NULL becomes Option.Some(ptr) (tag 0) with
    //   the pointer as payload. Both are RC-managed
    //   (ref_count 1) so they flow through the same RC
    //   path as Option values produced in-language.
    //
    //   FFI_VOID: push 0. The result slot is unused.
    //
    //   Otherwise: narrow the raw result per the declared
    //   return type. The narrowing (int8_t, int16_t,
    //   etc.) is what puts the value in the correct
    //   numeric domain before it hits the stack. Pointers
    //   and 64-bit values pass through unchanged.
    if (f->returns_option_ptr) {
        RcHeader* h;
        U8* payload;
        if (result == 0) {
            // Option.None: [tag = 1]
            h = (RcHeader*)malloc(sizeof(RcHeader) + 8);
            if (!h) RT_ERR("rc heap out of memory for Option.None%s", "");
            h->ref_count = 1;
            h->padding   = 0;
            payload = (U8*)(h + 1);
            U64 tag = 1;
            memcpy(payload, &tag, 8);
        } else {
            // Option.Some(ptr): [tag = 0][payload]
            h = (RcHeader*)malloc(sizeof(RcHeader) + 16);
            if (!h) RT_ERR("rc heap out of memory for Option.Some%s", "");
            h->ref_count = 1;
            h->padding   = 0;
            payload = (U8*)(h + 1);
            U64 tag = 0;
            memcpy(payload, &tag, 8);
            memcpy(payload + 8, &result, 8);
        }
        PUSH((U64)(uintptr_t)payload);
    }
    else if (f->ret_type == FFI_VOID) PUSH(0);
    else if (f->ret_type == FFI_I8)  PUSH((U64)(I64)(int8_t)result);
    else if (f->ret_type == FFI_I16) PUSH((U64)(I64)(int16_t)result);
    else if (f->ret_type == FFI_I32) PUSH((U64)(I64)(int32_t)result);
    else if (f->ret_type == FFI_U8)  PUSH((U64)(uint8_t)result);
    else if (f->ret_type == FFI_U16) PUSH((U64)(uint16_t)result);
    else if (f->ret_type == FFI_U32) PUSH((U64)(uint32_t)result);
    else if (f->ret_type == FFI_BOOL) PUSH(result ? 1 : 0);
    else PUSH(result);
    return 0;
#else
    (void)argv; (void)result;
    RT_ERR("FFI support not compiled in");
#endif
}

// ============================================================
// Lifecycle
// ============================================================
void vm_init(VM* vm, const VMConfig* cfg) {
    // Zero the whole VM. Every scalar field starts at 0,
    // every pointer NULL. That is the desired initial state
    // for fields that are not otherwise set below.
    memset(vm, 0, sizeof *vm);

    // Allocate the lexical arena. Zeroed (xcalloc) so that
    // any padding bytes the VM stores are deterministic
    // (which matters for reproducibility if a program
    // prints raw bytes).
    //
    // The const arena is not allocated here; it is sized
    // and allocated by materialize_consts below, based on
    // the pool's contents. That is why const_arena starts
    // NULL even though the field exists.
    vm->arena       = (U8*)xcalloc(ARENA_MAX, 1);
    vm->const_arena = NULL;
    vm->ap  = vm->arena;
    vm->amp = vm->arena;
    vm->cap = NULL;

    // Output defaults: stdout, no trace, sink to stdout.
    vm->out   = stdout;
    vm->in    = stdin;
    vm->argv  = NULL;
    vm->argc  = 0;
    vm->trace = false;

    vm->sink.kind = SINK_STDOUT;
    vm->sink.file = vm->out;
    vm->sink.buf  = vm->sink.inline_buf;
    vm->sink.cap  = SINK_INLINE_CAP;
    vm->sink_depth = 0;

    // Register every native. Note that the first two are
    // registered twice (see the duplicate lines below); the
    // second write is a no-op but is a leftover.
    //
    // The two "write" natives could be merged (they differ
    // only in the target FILE*), but keeping them separate
    // lets the dispatch be a table lookup with no branching.
    vm_register_native(vm, NATIVE_STDOUT_WRITE, native_stdout_write);
    vm_register_native(vm, NATIVE_STDERR_WRITE, native_stderr_write);
    vm_register_native(vm, NATIVE_FFI_CSTRING,  native_ffi_cstring);
    vm_register_native(vm, NATIVE_FFI_STRING,   native_ffi_string);
    vm_register_native(vm, NATIVE_FFI_ERROR,    native_ffi_error);
    vm_register_native(vm, NATIVE_FFI_ERRNO,    native_ffi_errno);
    vm_register_native(vm, NATIVE_RAW_NULL,     native_raw_null);
    vm_register_native(vm, NATIVE_RAW_IS_NULL,  native_raw_is_null);
    vm_register_native(vm, NATIVE_RAW_OFFSET,   native_raw_offset);
    vm_register_native(vm, NATIVE_RAW_LOAD_U8,  native_raw_load_u8);
    vm_register_native(vm, NATIVE_RAW_STORE_U8, native_raw_store_u8);
    vm_register_native(vm, NATIVE_FFI_ALLOC,    native_ffi_alloc);
    vm_register_native(vm, NATIVE_FFI_FREE,     native_ffi_free);
    vm_register_native(vm, NATIVE_ARGS,         native_args);
    vm_register_native(vm, NATIVE_READ_LINE,    native_read_line);
    vm_register_native(vm, NATIVE_READ_ALL,     native_read_all);
    vm_register_native(vm, NATIVE_STR_BEGIN,   native_str_begin);
    vm_register_native(vm, NATIVE_STR_END,     native_str_end);
    vm_register_native(vm, NATIVE_PRINT_VALUE, native_print_value);
    vm_register_native(vm, NATIVE_ARENA_ZERO, native_arena_zero);
    vm_register_native(vm, NATIVE_MEMCPY, native_memcpy);
    
    // If a config was passed, wire in the program and
    // materialize the const arena. A NULL config is legal
    // (it leaves the VM in a "no program" state); the
    // dispatch loop would then fail on the first READ.
    if (cfg) {
        vm->bytecode     = cfg->bytecode;
        vm->bytecode_len = cfg->bytecode_len;
        vm->lines        = cfg->lines;
        vm->cols         = cfg->cols;
        vm->pool         = cfg->pool;
        vm->funcs        = cfg->funcs;
        vm->types        = cfg->types;
        vm->ffi          = cfg->ffi;
        vm->argc         = cfg->prog_argc;
        vm->argv         = cfg->prog_argv;
        materialize_consts(vm);
    }
}

// Release everything the VM owns and reset the pointer
// fields.
//
// The sequence:
//   1. const_ptrs (the per-pool-entry pointer array).
//   2. arena (lexical; the base; ap/amp are cursors into it).
//   3. const_arena (base; cap is a cursor into it).
//   4. The sink's heap buffer, if it grew past its inline.
//   5. Each sink on the stack, same check.
//
// The NativeFn table and the call/scope stacks do not
// contain heap allocations (they are fixed-size arrays),
// so nothing needs freeing there.
//
// Note: the VM does NOT free any RC blocks. The refcounting
// system is expected to have freed everything by the time
// the program returns; if a leak exists, it will surface as
// an un-freed block at process exit. There is no "final GC"
// here.
void vm_free(VM* vm) {
    free(vm->const_ptrs);
    free(vm->arena);
    free(vm->const_arena);
    vm->const_ptrs  = NULL;
    vm->arena       = NULL;
    vm->const_arena = NULL;
    vm->ap = vm->amp = vm->cap = NULL;

    if (vm->sink.buf && vm->sink.buf != vm->sink.inline_buf)
        free(vm->sink.buf);
    for (int i = 0; i < vm->sink_depth; i++) {
        Sink* s = &vm->sink_stack[i];
        if (s->buf && s->buf != s->inline_buf) free(s->buf);
    }
}

// Register a native. Out-of-range slots are silently
// ignored (the MAX_NATIVES check); a caller that registers
// an out-of-range slot has a bug that this function does
// not diagnose.
void vm_register_native(VM* vm, int slot, NativeFn fn) {
    if (slot >= 0 && slot < MAX_NATIVES) vm->natives[slot] = fn;
}

// Return the value on top of the data stack, or 0 if the
// stack is empty. Used by main.c to print "return value".
I64 vm_top(VM* vm) {
    return vm->sp ? (I64)vm->data_stack[vm->sp - 1] : 0;
}

// ============================================================
// Deep RC drop
//
// The VM's refcounting is per-block. A block that holds
// RC-managed sub-values (a struct's RC fields, an array's RC
// elements, an RC-managed enum's payloads) must, when its
// own refcount reaches zero, recursively drop those
// sub-values before freeing the block. That is what these
// two functions do.
//
// rc_drop_deep(vm, value, d):
//   Drop one value on the stack (or in a slot). `d`
//   describes the value's type; for RC-managed kinds, the
//   value is a pointer to a block whose header is at
//   value-8. NULL is a no-op.
//
// rc_drop_payload(vm, payload, d):
//   Walk the sub-values of a block's payload. `payload`
//   points at the block's payload (no header). `d` describes
//   the payload's layout (struct fields, array elements, or
//   enum variant payloads). Used only for struct / array /
//   enum descriptors, which are always reached as the inner
//   descriptor of an RC block.
//
// Ordering: the outer block's payload is walked BEFORE the
// outer block is freed, because the walk reads the payload's
// bytes (which would be freed along with the block).
// ============================================================
static void rc_drop_payload(VM* vm, U8* payload, const TypeDesc* d);

static void rc_drop_deep(VM* vm, U64 value, const TypeDesc* d) {
    if (!value) return;   // NULL is a no-op for every kind

    switch (d->kind) {
        // String: [RcHeader][len][bytes]. Decrement, free if 0.
        // The const-arena sentinel is skipped.
        case TD_STRING: {
            U8* payload = (U8*)(uintptr_t)value;
            RcHeader* h = ((RcHeader*)payload) - 1;
            if (h->ref_count == 0xFFFFFFFF) return;
            if (--h->ref_count == 0) free(h);
            break;
        }

        // RC-wrapped value: [RcHeader][inner payload].
        // Decrement; if 0, walk the inner payload for
        // RC-managed sub-values, then free.
        case TD_RC: {
            U8* payload = (U8*)(uintptr_t)value;
            RcHeader* h = ((RcHeader*)payload) - 1;
            if (h->ref_count == 0xFFFFFFFF) return;
            if (--h->ref_count == 0) {
                const TypeDesc* inner = typetable_get(vm->types, d->aux);
                if (inner) {
                    if (inner->kind == TD_STRING
                        || inner->kind == TD_RC) {
                        // The inner value is itself pointer-
                        // shaped: the payload's first 8 bytes
                        // are a pointer to another RC block.
                        U64 inner_val;
                        memcpy(&inner_val, payload, 8);
                        rc_drop_deep(vm, inner_val, inner);
                    } else if (inner->kind == TD_STRUCT
                            || inner->kind == TD_ARRAY
                            || inner->kind == TD_ENUM) {
                        rc_drop_payload(vm, payload, inner);
                    }
                    // Primitives, ref, rawptr: nothing to drop.
                }
                free(h);
            }
            break;
        }

        // Struct / array / enum in the arena: no header to
        // free (the arena reclaims on scope exit), but the
        // sub-values still need dropping. This case is reached
        // when an RC-managed container holds a non-RC
        // container whose elements are themselves RC.
        case TD_STRUCT:
        case TD_ARRAY:
        case TD_ENUM:
            rc_drop_payload(vm, (U8*)(uintptr_t)value, d);
            break;

        default:
            // Primitives, ref, rawptr: no-op.
            break;
    }
}

static void rc_drop_payload(VM* vm, U8* payload, const TypeDesc* d) {
    switch (d->kind) {
        case TD_STRUCT:
            // Fields start at offset 8 (offset 0 is the
            // unused tag / field-count slot, shared with
            // enums for a uniform block layout).
            for (uint32_t i = 0; i < d->count; i++) {
                uint32_t fidx = vm->types->fields[d->aux + i];
                const TypeDesc* fd = typetable_get(vm->types, fidx);
                if (!fd) continue;
                U64 field_val;
                memcpy(&field_val, payload + 8 + i * 8, 8);
                rc_drop_deep(vm, field_val, fd);
            }
            break;

        case TD_ARRAY: {
            // [u64 len][elem0][elem1]...
            U64 len; memcpy(&len, payload, 8);
            const TypeDesc* elem = typetable_get(vm->types, d->aux);
            if (!elem) return;
            for (U64 i = 0; i < len; i++) {
                U64 elem_val;
                memcpy(&elem_val, payload + 8 + i * 8, 8);
                rc_drop_deep(vm, elem_val, elem);
            }
            break;
        }

        case TD_ENUM: {
            // [u64 tag][payload0][payload1]...
            U64 tag; memcpy(&tag, payload, 8);
            if (tag >= d->count) return;   // out-of-range tag
            const EnumVariantDesc* ev =
                &vm->types->enum_variants[d->aux + tag];
            for (uint32_t p = 0; p < ev->payload_count; p++) {
                uint32_t pidx =
                    vm->types->enum_payloads[ev->payload_start + p];
                const TypeDesc* pd = typetable_get(vm->types, pidx);
                if (!pd) continue;
                U64 pval;
                memcpy(&pval, payload + 8 + p * 8, 8);
                rc_drop_deep(vm, pval, pd);
            }
            break;
        }

        default:
            // Not reachable from RC_REC/STRING walks; kept
            // for safety.
            break;
    }
}

static const char* trace_op_name(int op) {
    switch (op) {
        case OP_HALT: return "HALT";
        case OP_PUSH: return "PUSH";
        case OP_POP: return "POP";
        case OP_LOAD_LOCAL: return "LOAD_LOCAL";
        case OP_STORE_LOCAL: return "STORE_LOCAL";
        case OP_LOAD_ADDR_LOCAL: return "LOAD_ADDR_LOCAL";
        case OP_LOAD_INDIRECT: return "LOAD_INDIRECT";
        case OP_STORE_INDIRECT: return "STORE_INDIRECT";
        case OP_LOAD_ADDR_INDIRECT: return "LOAD_ADDR_INDIRECT";
        case OP_LOAD_CONST: return "LOAD_CONST";
        case OP_LEN: return "LEN";
        case OP_BINARY: return "BINARY";
        case OP_UNARY: return "UNARY";
        case OP_JMP: return "JMP";
        case OP_JMP_IF_FALSE: return "JMP_IF_FALSE";
        case OP_CALL: return "CALL";
        case OP_RET: return "RET";
        case OP_ENTER_SCOPE: return "ENTER_SCOPE";
        case OP_EXIT_SCOPE: return "EXIT_SCOPE";
        case OP_ALLOC: return "ALLOC";
        case OP_ALLOC_RC: return "ALLOC_RC";
        case OP_NEW_ARRAY: return "NEW_ARRAY";
        case OP_NEW_RC_ARRAY: return "NEW_RC_ARRAY";
        case OP_INDEX_LOAD: return "INDEX_LOAD";
        case OP_INDEX_STORE: return "INDEX_STORE";
        case OP_NEW_ENUM: return "NEW_ENUM";
        case OP_NEW_RC_ENUM: return "NEW_RC_ENUM";
        case OP_TRAP: return "TRAP";
        case OP_RC_INC: return "RC_INC";
        case OP_RC_DEC: return "RC_DEC";
        case OP_CONVERT: return "CONVERT";
        case OP_CCALL: return "CCALL";
        case OP_FFI_CALL: return "FFI_CALL";
        case OP_TRY_JUMP_IF_OK: return "TRY_JUMP_IF_OK";
        default: return "?";
    }
}

// ============================================================
// Execution
// ============================================================

// The dispatch loop.
//
// Shape of one iteration:
//   1. Save vm->current_pc = vm->pc. All error reporting
//      uses current_pc, not the post-read pc, so an error
//      points at the *start* of the failing instruction.
//   2. Read the opcode word from bytecode[pc++].
//   3. Dispatch on the opcode. Operand words are read via
//      READ() inline; each case consumes exactly the
//      operands its OPINFO row specifies.
//   4. Loop. There is no explicit "advance to next
//      instruction": the pc is already past the operands
//      when the case returns.
//
// Error handling: RT_ERR sets vm->error, error_pc, and
// error_func_idx, then returns 1 from vm_run. main.c's
// render_runtime_error then renders the message plus a
// source excerpt and call trace.
//
// The VM does not perform any of the structural checks
// that validate_bytecode does. It trusts the input: a
// well-formed chunk has valid opcodes, non-truncated
// instructions, in-range jump targets, and in-range
// descriptor indices. That is the layering: the validator
// checks, the VM executes.
int vm_run(VM* vm) {
    for (;;) {
        // Save the span of the instruction we're about to
        // execute. Errors report this span, not the advanced pc.
        vm->current_pc = vm->pc;
        
        OpCode op = (OpCode)READ();
        // Trace: one line per instruction, to stderr, when
        // the driver enabled it. Prints the pc, the opcode
        // number, and the current stack depth. Enough to
        // follow control flow without a debugger; not
        // enough to replace one.
        if (vm->trace) {
            fprintf(stderr, "[trace] %s pc=%04llu op=%2d sp=%u\n",
                    trace_op_name((int)op),(unsigned long long)vm->current_pc,
                    (int)op,
                    vm->sp);
            fflush(stderr);
        }
        

        switch (op) {
            // ----- Halt -----
            //
            // The program's exit point. OP_HALT is emitted
            // only in the prologue (after CALL main), and
            // reaching it means main returned normally.
            // Returns 0 to main.c, which prints the return
            // value from the top of stack.
            case OP_HALT:
                return 0;

            // ----- Stack manipulation -----
            //
            // PUSH: push one literal word. The operand is
            // the value (often an integer literal, but also
            // used for small constants like pattern tags).
            // Stack: -> v
            case OP_PUSH:
                PUSH(READ());
                break;
            // POP: discard one value. Stack: v ->
            //
            // Emitted for expression statements whose value
            // is not RC-managed (see the AST_EXPR_STMT case
            // in codegen; the RC_DEC variant is used for
            // fresh RC values). The guard against empty
            // stack is defensive; codegen should never
            // emit a POP with an empty stack.
            case OP_POP:
                if (vm->sp == 0) RT_ERR("pop on empty stack%s", "");
                (void)POP();
                break;

            case OP_DUP:{
                if (vm->sp == 0) RT_ERR("dup on empty stack%s", "");
                U64 v = vm->data_stack[vm->sp -1];
                PUSH(v);
                break;
            }
            case OP_DUP2:
                if (vm->sp < 2) RT_ERR("dup2 on shallow stack%s", "");
                U64 a = vm->data_stack[vm->sp - 2];   // pushes a
                U64 b = vm->data_stack[vm->sp - 1];   // pushes b
                PUSH(a);
                PUSH(b);
                break;
                

            // ----- Locals -----
            //
            // Locals are addressed as data_stack[fp + slot].
            // The frame pointer `fp` is set by OP_CALL to the
            // base of the callee's argument region, so slot 0
            // is the first parameter.
            //
            // LOAD_LOCAL: push data_stack[fp + slot].
            // The bounds check `fp + slot >= sp` catches two
            // failure modes:
            //   - reading past the top of the stack, which
            //     would read an uninitialized slot;
            //   - reading a local that the current frame
            //     has not yet declared (should not happen
            //     in well-formed codegen).
            case OP_LOAD_LOCAL: {
                U32 slot = (U32)READ();
                if (vm->fp + slot >= vm->sp)
                    RT_ERR("bad local load slot %u", slot);
                
                U64 val = vm->data_stack[vm->fp + slot];
                PUSH(val);
                break;
            }
            // STORE_LOCAL: pop a value and write it into
            // data_stack[fp + slot].
            //
            // The interesting behavior is the "extend frame"
            // loop. Because `let` codegen relies on the
            // initializer's value staying on the stack as
            // the local's slot, the frame's top-of-stack
            // grows naturally during a sequence of `let`s.
            // But an `assign` to a slot higher than the
            // current sp (which can happen with anonymous
            // locals or a pattern-bound slot whose
            // declaration is not a `let`) needs the frame
            // to be grown explicitly.
            //
            // The loop zero-fills the gap between sp and
            // the target slot. That makes the frame's
            // invariant (every slot < sp contains a
            // defined value) hold, at the cost of some
            // zero-initialized slots that are immediately
            // overwritten or never read.
            case OP_STORE_LOCAL: {
                U32 slot = (U32)READ();

                //fprintf(stderr, "[store] slot=%u fp=%u sp=%u\n",
                //        slot, vm->fp, vm->sp); fflush(stderr);
                
                U32 target = vm->fp + slot;
                if (target >= DATA_STACK_MAX)
                    RT_ERR("local store slot %u out of range", slot);

                /* POP on an empty stack reads data_stack[-1] (as an
                unsigned index, a huge value). Guard before we
                touch it. This cannot happen with correct codegen,
                but a codegen bug should produce a clean error
                rather than a segfault. */
                if (vm->sp == 0)
                    RT_ERR("store local on empty stack%s", "");
                // Grab the value first; the extension below pushes
                // placeholder zeros that would otherwise mask it.
            
                U64 v = POP();

                // If the target slot is at or above the current
                // stack top, grow the stack to include it. This
                // lets codegen write to anonymous locals (match
                // scrutinee, pattern temporaries, bind slots)
                // without a separate reservation step.
                while (vm->sp <= target) {
                    if (vm->sp >= DATA_STACK_MAX)
                        RT_ERR("stack overflow extending locals%s", "");
                    vm->data_stack[vm->sp++] = 0;
                }
                vm->data_stack[target] = v;
                break;
            }
            // LOAD_ADDR_LOCAL: push the *address* of a local
            // slot as a pointer. This is what makes `&x`
            // (where x is a local) cheap: the reference is a
            // raw pointer into the data stack, not a heap
            // allocation.
            //
            // The lifetime of this pointer is the enclosing
            // frame; the language's escape rules ensure it
            // does not outlive the frame.
            case OP_LOAD_ADDR_LOCAL: {
                U32 slot = (U32)READ();
                if (vm->fp + slot >= vm->sp)
                    RT_ERR("bad address-of-local slot %u", slot);
                PUSH((U64)(uintptr_t)&vm->data_stack[vm->fp + slot]);
                break;
            }

            // ----- Indirect access -----
            //
            // LOAD_INDIRECT: pop an address, push 8 bytes at
            // address + offset. NULL is rejected (there is no
            // defined behavior for a null load).
            //
            // The offset is a *byte* offset, and the load is
            // always 8 bytes. That is why struct fields are
            // 8-byte-aligned and why the compiler cannot
            // store sub-word values in place: every field
            // takes a full slot.
            case OP_LOAD_INDIRECT: {
                U32 offset = (U32)READ();
                U32 mk     = (U32)READ();
                if (vm->sp == 0) RT_ERR("indirect load on empty stack%s", "");
                U64 addr = POP();
                if (!addr) RT_ERR("null dereference%s", "");
                U8* p = (U8*)(uintptr_t)addr + offset;
                U64 v = 0;
                switch (mk) {
                    case MEM_I8:  { int8_t   x; memcpy(&x, p, 1); v = (U64)(I64)x; break; }
                    case MEM_I16: { int16_t  x; memcpy(&x, p, 2); v = (U64)(I64)x; break; }
                    case MEM_I32: { int32_t  x; memcpy(&x, p, 4); v = (U64)(I64)x; break; }
                    case MEM_I64: case MEM_U64: case MEM_PTR:
                        memcpy(&v, p, 8); break;
                    case MEM_U8:  { uint8_t  x; memcpy(&x, p, 1); v = (U64)x; break; }
                    case MEM_U16: { uint16_t x; memcpy(&x, p, 2); v = (U64)x; break; }
                    case MEM_U32: { uint32_t x; memcpy(&x, p, 4); v = (U64)x; break; }
                    case MEM_F32: { uint32_t x; memcpy(&x, p, 4); v = (U64)x; break; }
                    case MEM_F64: memcpy(&v, p, 8); break;
                    default: RT_ERR("bad mem_kind %u", mk);
                }
                PUSH(v);
                break;
            }
            // STORE_INDIRECT: pop value then address, write
            // 8 bytes at address + offset.
            //
            // Operand order matches the codegen: an assignment
            // `p.f = v` emits target (address), then value, so
            // the value is on top and popped first.
            case OP_STORE_INDIRECT: {
                U32 offset = (U32)READ();
                U32 mk     = (U32)READ();
                if (vm->sp < 2) RT_ERR("indirect store needs 2 operands%s", "");
                U64 v    = POP();
                U64 addr = POP();
                if (!addr) RT_ERR("null store through reference%s", "");
                U8* p = (U8*)(uintptr_t)addr + offset;
                switch (mk) {
                    case MEM_I8: case MEM_U8:
                        { uint8_t  b = (uint8_t)v;  memcpy(p, &b, 1); break; }
                    case MEM_I16: case MEM_U16:
                        { uint16_t b = (uint16_t)v; memcpy(p, &b, 2); break; }
                    case MEM_I32: case MEM_U32: case MEM_F32:
                        { uint32_t b = (uint32_t)v; memcpy(p, &b, 4); break; }
                    case MEM_I64: case MEM_U64: case MEM_F64: case MEM_PTR:
                        memcpy(p, &v, 8); break;
                    default: RT_ERR("bad mem_kind %u", mk);
                }
                break;
            }
            case OP_LOAD_ADDR_INDIRECT: {
                U32 offset = (U32)READ();
                if (vm->sp == 0) RT_ERR("addr-of-indirect on empty stack%s", "");
                U64 addr = POP();
                if (!addr) RT_ERR("null address-of%s", "");
                PUSH((U64)(uintptr_t)((U8*)(uintptr_t)addr + offset));
                break;
            }

            // ----- Constants -----
            //
            // LOAD_CONST: push a value from the constant pool.
            // The dispatch is on the pool entry's kind:
            //   I64    — push the value.
            //   BOOL   — push 0 or 1.
            //   STRING — push vm->const_ptrs[idx], the
            //            materialized [len][bytes] pointer.
            //   CSTRING — same, but the pointer is to a
            //             NUL-terminated buffer with no header.
            //   F64    — copy the double's bit pattern into
            //            the U64 slot and push.
            //
            // The pool index is bounds-checked (the validator
            // also checks this, but a defensive check costs
            // little and turns a segfault into a clean error).
            case OP_LOAD_CONST: {
                U32 idx = (U32)READ();
                if (!vm->pool || idx >= (U32)vm->pool->count)
                    RT_ERR("bad constant index %u", idx);
                const Constant* c = &vm->pool->items[idx];
                switch (c->kind) {
                    case CONST_I64:
                        PUSH((U64)c->as.i64);
                        break;
                    case CONST_BOOL:
                        PUSH(c->as.b ? 1 : 0);
                        break;
                    case CONST_STRING:
                        PUSH((U64)(uintptr_t)vm->const_ptrs[idx]);
                        break;
                    case CONST_CSTRING:
                        PUSH((U64)(uintptr_t)vm->const_ptrs[idx]);
                        break;
                    case CONST_F64: {
                        U64 bits;
                        memcpy(&bits, &c->as.f64, 8);
                        PUSH(bits);
                        break;
                    }
                    default:
                        RT_ERR("unknown constant kind %d", (int)c->kind);
                }
                break;
            }

            // LEN: push the length prefix of a string or
            // array. The desc operand tells the VM which
            // layout to expect; both string and array
            // payloads begin with an 8-byte length.
            //
            // A NULL container pushes 0 (the "empty"
            // length). That is what makes `len(null_string)`
            // return 0 rather than crash.
            case OP_LEN: {
                U32 desc_idx = (U32)READ();
                const TypeDesc* d = typetable_get(vm->types, desc_idx);
                if (!d) RT_ERR("bad len desc %u", desc_idx);
                if (d->kind != TD_STRING && d->kind != TD_ARRAY)
                    RT_ERR("len not supported for desc kind %d", (int)d->kind);

                U64 ptr = POP();
                if (!ptr) { PUSH(0); break; }
                U64 len;
                memcpy(&len, (U8*)(uintptr_t)ptr, 8);
                PUSH(len);
                break;
            }

            // ----- Parameterized binary -----
            //
            // The descriptor operand determines both the
            // numeric interpretation of the operands and the
            // operation's semantics. This is the "types are
            // descriptors, not opcodes" design in action.
            //
            // Layout of a BINARY: <OpBinary> <desc_idx>.
            //   OpBinary selects ADD/SUB/MUL/DIV or a
            //   comparison; desc_idx selects the numeric
            //   domain (i8..u64, f32, f64, string, rawptr).
            //
            // The sign/width-specific implementations live in
            // the case below; each is self-contained because
            // libffi-style parameterization would not save
            // much code and would obscure the per-type
            // semantics.
            case OP_BINARY: {
                U32 sub  = (U32)READ();
                U32 tidx = (U32)READ();
                const TypeDesc* d = typetable_get(vm->types, tidx);
                if (!d) RT_ERR("bad binary type idx %u", tidx);
                switch (d->kind) {
                    // Signed integer arithmetic and comparison.
                    // Width truncation happens *after* the
                    // operation, which is what makes i8 + i8
                    // wrap at 8 bits rather than promoting to
                    // i64.
                    case TD_I8: case TD_I16: case TD_I32: case TD_I64: {
                        int bits = (d->kind == TD_I8)  ? 8
                                 : (d->kind == TD_I16) ? 16
                                 : (d->kind == TD_I32) ? 32 : 64;

                        I64 b = (I64)POP();
                        I64 a = (I64)POP();

                        bool is_cmp = (sub == OPB_EQ || sub == OPB_NE ||
                                       sub == OPB_LT || sub == OPB_LE ||
                                       sub == OPB_GT || sub == OPB_GE);
                        if (is_cmp) {
                            // Comparison result is bool (0/1).
                            int r = 0;
                            switch (sub) {
                                case OPB_EQ: r = (a == b); break;
                                case OPB_NE: r = (a != b); break;
                                case OPB_LT: r = (a <  b); break;
                                case OPB_LE: r = (a <= b); break;
                                case OPB_GT: r = (a >  b); break;
                                case OPB_GE: r = (a >= b); break;
                                default: break;
                            }
                            PUSH((U64)r);
                        } else {
                            // Arithmetic, with explicit
                            // unsigned arithmetic before the
                            // signed truncation. That is what
                            // makes wrapping well-defined even
                            // for the signed cases (C signed
                            // overflow is UB otherwise).
                            I64 r = 0;
                            switch (sub) {
                                case OPB_ADD: r = (I64)((U64)a + (U64)b); break;
                                case OPB_SUB: r = (I64)((U64)a - (U64)b); break;
                                case OPB_MUL: r = (I64)((U64)a * (U64)b); break;
                                case OPB_DIV:
                                    if (b == 0) RT_ERR("division by zero%s", "");
                                    r = a / b;
                                    break;
                                default: RT_ERR("bad binary sub-op %u", sub);
                            }
                            switch (bits) {
                                case 8:  r = (I64)(int8_t)r;  break;
                                case 16: r = (I64)(int16_t)r; break;
                                case 32: r = (I64)(int32_t)r; break;
                                default: break;
                            }
                            PUSH((U64)r);
                        }
                        break;
                    }

                    // Unsigned integer arithmetic and comparison.
                    // Same shape as signed; note that division is
                    // the unsigned variety (no UB on INT_MIN / -1).
                    case TD_U8: case TD_U16: case TD_U32: case TD_U64: {
                        int bits = (d->kind == TD_U8)  ? 8
                                 : (d->kind == TD_U16) ? 16
                                 : (d->kind == TD_U32) ? 32 : 64;

                        U64 b = POP();
                        U64 a = POP();

                        bool is_cmp = (sub == OPB_EQ || sub == OPB_NE ||
                                       sub == OPB_LT || sub == OPB_LE ||
                                       sub == OPB_GT || sub == OPB_GE);
                        if (is_cmp) {
                            int r = 0;
                            switch (sub) {
                                case OPB_EQ: r = (a == b); break;
                                case OPB_NE: r = (a != b); break;
                                case OPB_LT: r = (a <  b); break;
                                case OPB_LE: r = (a <= b); break;
                                case OPB_GT: r = (a >  b); break;
                                case OPB_GE: r = (a >= b); break;
                                default: break;
                            }
                            PUSH((U64)r);
                        } else {
                            U64 r = 0;
                            switch (sub) {
                                case OPB_ADD: r = a + b; break;
                                case OPB_SUB: r = a - b; break;
                                case OPB_MUL: r = a * b; break;
                                case OPB_DIV:
                                    if (b == 0) RT_ERR("division by zero%s", "");
                                    r = a / b;
                                    break;
                                default: RT_ERR("bad binary sub-op %u", sub);
                            }
                            switch (bits) {
                                case 8:  r = (U64)(uint8_t)r;  break;
                                case 16: r = (U64)(uint16_t)r; break;
                                case 32: r = (U64)(uint32_t)r; break;
                                default: break;
                            }
                            PUSH(r);
                        }
                        break;
                    }

                    // f32. The stack slot is a U64, but only
                    // the low 32 bits are meaningful. Values
                    // are loaded via memcpy into a float, then
                    // the result is memcpy'd back into a
                    // 32-bit word before PUSH.
                    case TD_F32: {
                        float b, a;
                        U32 tmp = POP(); memcpy(&b, &tmp, 4);
                        tmp = POP();     memcpy(&a, &tmp, 4);

                        bool is_cmp = (sub == OPB_EQ || sub == OPB_NE ||
                                       sub == OPB_LT || sub == OPB_LE ||
                                       sub == OPB_GT || sub == OPB_GE);
                        if (is_cmp) {
                            int r = 0;
                            switch (sub) {
                                case OPB_EQ: r = (a == b); break;
                                case OPB_NE: r = (a != b); break;
                                case OPB_LT: r = (a <  b); break;
                                case OPB_LE: r = (a <= b); break;
                                case OPB_GT: r = (a >  b); break;
                                case OPB_GE: r = (a >= b); break;
                                default: break;
                            }
                            PUSH((U64)r);
                        } else {
                            float r = 0.0;
                            switch (sub) {
                                case OPB_ADD: r = a + b; break;
                                case OPB_SUB: r = a - b; break;
                                case OPB_MUL: r = a * b; break;
                                case OPB_DIV:
                                    if (b == 0.0) RT_ERR("division by zero%s", "");
                                    r = a / b;
                                    break;
                                default: RT_ERR("bad binary sub-op %u", sub);
                            }
                            U32 v; memcpy(&v, &r, 4);
                            PUSH(v);
                        }
                        break;
                    }
                    // f64. Same shape as f32 but with 8-byte
                    // values. NaN comparisons follow IEEE 754
                    // (NaN != NaN, NaN < x is false, etc.);
                    // that is the C behavior and is what the
                    // language inherits.
                    case TD_F64: {
                        double b, a;
                        U64 tmp = POP(); memcpy(&b, &tmp, 8);
                        tmp = POP();     memcpy(&a, &tmp, 8);

                        bool is_cmp = (sub == OPB_EQ || sub == OPB_NE ||
                                       sub == OPB_LT || sub == OPB_LE ||
                                       sub == OPB_GT || sub == OPB_GE);
                        if (is_cmp) {
                            int r = 0;
                            switch (sub) {
                                case OPB_EQ: r = (a == b); break;
                                case OPB_NE: r = (a != b); break;
                                case OPB_LT: r = (a <  b); break;
                                case OPB_LE: r = (a <= b); break;
                                case OPB_GT: r = (a >  b); break;
                                case OPB_GE: r = (a >= b); break;
                                default: break;
                            }
                            PUSH((U64)r);
                        } else {
                            double r = 0.0;
                            switch (sub) {
                                case OPB_ADD: r = a + b; break;
                                case OPB_SUB: r = a - b; break;
                                case OPB_MUL: r = a * b; break;
                                case OPB_DIV:
                                    if (b == 0.0) RT_ERR("division by zero%s", "");
                                    r = a / b;
                                    break;
                                default: RT_ERR("bad binary sub-op %u", sub);
                            }
                            U64 v; memcpy(&v, &r, 8);
                            PUSH(v);
                        }
                        break;
                    }
                    // String operations. Two subtleties:
                    //   - ADD concatenates into a fresh RC
                    //     string (header + length + concatenated
                    //     bytes). The result's refcount is 1;
                    //     the caller owns it.
                    //   - Comparison is lexicographic; NULL
                    //     sorts before any non-NULL and equals
                    //     NULL.
                    case TD_STRING: {
                        U8* b = (U8*)(uintptr_t)POP();
                        U8* a = (U8*)(uintptr_t)POP();
                        switch (sub) {
                            case OPB_ADD: {
                                U64 alen = 0, blen = 0;
                                if (a) memcpy(&alen, a, 8);
                                if (b) memcpy(&blen, b, 8);
                                U64 total = alen + blen;
                                RcHeader* h = (RcHeader*)malloc(
                                    sizeof(RcHeader) + 8 + total
                                );
                                if (!h){
                                    RT_ERR("rc heap out of memory "
                                        "for string concat%s", "");
                                }
                                h->ref_count = 1;
                                h->padding   = 0;
                                U8* block = (U8*)(h + 1);
                                memcpy(block, &total, 8);
                                if (alen) memcpy(block + 8, a + 8, alen);
                                if (blen) memcpy(block + 8 + alen, b + 8, blen);
                                PUSH((U64)(uintptr_t)block);
                                break;
                            }
                            case OPB_EQ: PUSH(strings_equal(a, b) ? 1 : 0); break;
                            case OPB_NE: PUSH(strings_equal(a, b) ? 0 : 1); break;
                            case OPB_LT: PUSH(strings_compare(a, b) <  0);   break;
                            case OPB_LE: PUSH(strings_compare(a, b) <= 0);   break;
                            case OPB_GT: PUSH(strings_compare(a, b) >  0);   break;
                            case OPB_GE: PUSH(strings_compare(a, b) >= 0);   break;
                            default:
                                RT_ERR("operator not supported for strings%s", "");
                        }
                        break;
                    }

                    // Pointer comparison. Only EQ and NE
                    // (ordering is not defined for arbitrary
                    // pointers). The operands are the raw
                    // pointer values; no dereference.
                    case TD_RAWPTR: {
                        I64 b = (I64)POP();
                        I64 a = (I64)POP();
                        int r = 0;
                        switch (sub) {
                            case OPB_EQ: r = (a == b); break;
                            case OPB_NE: r = (a != b); break;
                            default:
                                RT_ERR("operator not supported for pointers%s",
                                       "");
                        }
                        PUSH((U64)r);
                        break;
                    }
                    
                    default:
                        RT_ERR("binary not supported for descriptor kind %d",
                               (int)d->kind);
                }
                break;
            }

            // UNARY: negation and logical not. Descriptor-
            // parameterized like BINARY.
            //   NEG on signed ints and f32/f64.
            //   NOT on bool.
            //   Nothing else is defined.
            case OP_UNARY: {
                U32 sub  = (U32)READ();
                U32 tidx = (U32)READ();
                const TypeDesc* d = typetable_get(vm->types, tidx);
                if (!d) RT_ERR("bad unary type idx %u", tidx);
                switch (d->kind) {
                    // Signed ints: NEG with width truncation.
                    case TD_I8: case TD_I16: case TD_I32: case TD_I64: {
                    int bits = (d->kind == TD_I8)  ? 8
                             : (d->kind == TD_I16) ? 16
                             : (d->kind == TD_I32) ? 32 : 64;
                    I64 a = (I64)POP();
                    if (sub == OPU_NEG) {
                        I64 r = -a;
                        switch (bits) {
                            case 8:  r = (I64)(int8_t)r;  break;
                            case 16: r = (I64)(int16_t)r; break;
                            case 32: r = (I64)(int32_t)r; break;
                            default: break;
                        }
                        PUSH((U64)r);
                    } else {
                        RT_ERR("bad unary sub-op %u on signed int", sub);
                    }
                    break;
                    }
                    // f32 negation.
                    case TD_F32: {
                        U32 tmp = POP();
                        float a; memcpy(&a, &tmp, 4);
                        if (sub == OPU_NEG) {
                            float r = -a;
                            U32 v; memcpy(&v, &r, 4);
                            PUSH(v);
                        } else {
                            RT_ERR("bad unary sub-op %u on f64", sub);
                        }
                        break;
                    }
                    // f64 negation. The error message says
                    // "on f64" which is also what the f32
                    // case says; a small copy-paste leftover.
                    case TD_F64: {
                        U64 tmp = POP();
                        double a; memcpy(&a, &tmp, 8);
                        if (sub == OPU_NEG) {
                            double r = -a;
                            U64 v; memcpy(&v, &r, 8);
                            PUSH(v);
                        } else {
                            RT_ERR("bad unary sub-op %u on f64", sub);
                        }
                        break;
                    }
                    // bool NOT.
                    case TD_BOOL: {
                        I64 a = (I64)POP();
                        if (sub == OPU_NOT) PUSH(a ? 0 : 1);
                        else                RT_ERR("bad unary sub-op %u on bool", sub);
                        break;
                    }
                    default:
                        RT_ERR("unary not supported for descriptor kind %d",
                               (int)d->kind);
                }
                break;
            }

            // ----- Control flow -----
            //
            // JMP: unconditional. The target is an absolute
            // pc (word offset into the code array).
            case OP_JMP:{
                U64 target = READ();
                vm->pc = target;
                break;
            }
            // JMP_IF_FALSE: conditional. Pops the condition;
            // if it is 0, jumps; if non-zero, falls through.
            //
            // The semantics of "false" match the language:
            // any non-zero value is true. In practice
            // conditions are bool (0/1), but the VM does
            // not distinguish.
            case OP_JMP_IF_FALSE: {
                U64 target = READ();
                if (vm->sp == 0) RT_ERR("conditional on empty stack%s", "");
                U64 cond = POP();
                if (!cond) vm->pc = target;
                break;
            }

            case OP_JMP_IF_TRUE: {
                U64 target = READ();
                if (vm->sp == 0) RT_ERR("conditional on empty stack%s", "");
                U64 cond = POP();
                if (cond) vm->pc = target;
                break;
            }
            

            // ----- Scope -----
            //
            // ENTER_SCOPE records the stack pointer and arena
            // mark, then starts a fresh arena scope. EXIT_SCOPE
            // reverses both.
            //
            // The pattern is:
            //   ENTER_SCOPE:
            //     scope_stack[depth].sp  = sp
            //     scope_stack[depth].amp = amp   (enclosing)
            //     depth++
            //     amp = ap                       (new mark)
            //   EXIT_SCOPE:
            //     ap  = amp                      (reset arena)
            //     depth--
            //     sp  = scope_stack[depth].sp    (drop locals)
            //     amp = scope_stack[depth].amp   (restore)
            //
            // The two-part arena state is what makes
            // nested scopes work: on exit, the current mark
            // is used to reset the arena, and then the mark
            // is replaced with the enclosing one.
            case OP_ENTER_SCOPE: {
                if (vm->scope_depth >= SCOPE_STACK_MAX)
                    RT_ERR("scope stack overflow%s", "");
                vm->scope_stack[vm->scope_depth].sp  = vm->sp;
                vm->scope_stack[vm->scope_depth].amp = vm->amp;
                vm->scope_depth++;
                vm->amp = vm->ap;
                break;
            }
            case OP_EXIT_SCOPE: {
                if (vm->scope_depth == 0)
                    RT_ERR("scope stack underflow%s", "");
                vm->ap = vm->amp;
                vm->scope_depth--;
                vm->sp  = vm->scope_stack[vm->scope_depth].sp;
                vm->amp = vm->scope_stack[vm->scope_depth].amp;
                break;
            }

            // ----- Allocation -----
            //
            // OP_ALLOC: arena-allocate a single 8-byte value
            // (the `size` operand is nominally flexible but
            // codegen always passes 8; the VM handles
            // size < 8 by copying only the low bytes).
            // Stack: v -> ptr.
            case OP_ALLOC: {
                U64 size = READ();
                if (size == 0) size = 8;
                if (vm->sp == 0) RT_ERR("allocate on empty stack%s", "");
                U64 value = POP();
                U8* p = arena_alloc(vm, size);
                if (!p) RT_ERR("arena out of memory (request=%llu)",
                               (unsigned long long)size);
                size_t n = size < 8 ? (size_t)size : 8;
                memcpy(p, &value, n);
                PUSH((U64)(uintptr_t)p);
                break;
            }
            // OP_ALLOC_RC: RC-allocate a single 8-byte value.
            // Same as OP_ALLOC but the block is on the RC
            // heap with a header and refcount 1.
            // Stack: v -> ptr.
            case OP_ALLOC_RC: {
                U64 size = READ();
                if (size == 0) size = 8;
                if (vm->sp == 0) RT_ERR("allocate on empty stack%s", "");
                U64 value = POP();
                RcHeader* h = (RcHeader*)malloc(sizeof(RcHeader) + size);
                if (!h) RT_ERR("rc heap out of memory%s", "");
                h->ref_count = 1;
                h->padding   = 0;
                size_t n = size < 8 ? (size_t)size : 8;
                memcpy(h + 1, &value, n);
                PUSH((U64)(uintptr_t)(h + 1));
                break;
            }

            // NEW_ARRAY: build an array in the arena.
            // Stack: v0 v1 ... v(n-1) -> arr_ptr.
            //
            // Layout: [u64 length][elem0][elem1]...
            // Elements are popped in reverse (LIFO) and
            // written into the block in source order.
            //
            // The array is arena-allocated, so it lives
            // until the enclosing scope exits. That matches
            // the language rule that array literals do not
            // escape.
            case OP_NEW_ARRAY: {
                U64 n = READ();
                U64 bytes = 8 + n * 8;
                U8* block = arena_alloc(vm, bytes);
                if (!block)
                    RT_ERR("arena out of memory for array (n=%llu)",
                           (unsigned long long)n);
                memcpy(block, &n, 8);
                for (U64 i = n; i > 0; i--) {
                    U64 v = POP();
                    memcpy(block + 8 + (i - 1) * 8, &v, 8);
                }
                PUSH((U64)(uintptr_t)block);
                break;
            }

            // NEW_RC_ARRAY: RC-managed array. Same layout
            // as NEW_ARRAY but with an RcHeader and a
            // refcount of 1. Used by `rc [lit]` and by
            // struct literals wrapped in `rc`.
            case OP_NEW_RC_ARRAY: {
                U64 n = READ();
                U64 payload_bytes = 8 + n * 8;
                U64 total = sizeof(RcHeader) + payload_bytes;
                RcHeader* h = (RcHeader*)malloc(total);
                if (!h)
                    RT_ERR("rc heap out of memory (array n=%llu)",
                           (unsigned long long)n);
                h->ref_count = 1;
                h->padding   = 0;
                U8* payload = (U8*)(h + 1);
                memcpy(payload, &n, 8);
                for (U64 i = n; i > 0; i--) {
                    U64 v = POP();
                    memcpy(payload + 8 + (i - 1) * 8, &v, 8);
                }
                PUSH((U64)(uintptr_t)payload);
                break;
            }

            // INDEX_LOAD: load an element from an array or
            // a byte from a string. The desc distinguishes
            // the two layouts; both start with a length.
            // Stack: container, index -> element.
            //
            // Bounds are checked; an out-of-range index
            // aborts with a clean error.
            case OP_INDEX_LOAD: {
                U32 desc_idx = (U32)READ();
                const TypeDesc* d = typetable_get(vm->types, desc_idx);
                if (!d) RT_ERR("bad index load desc %u", desc_idx);

                U64 index = POP();
                U64 ptr   = POP();
                if (!ptr) RT_ERR("index through null%s", "");

                U64 len;
                memcpy(&len, (U8*)(uintptr_t)ptr, 8);
                if (index >= len)
                    RT_ERR("index %llu out of bounds (length %llu)",
                           (unsigned long long)index,
                           (unsigned long long)len);

                if (d->kind == TD_STRING) {
                    // Byte read; zero-extended to u64.
                    U8 byte = ((U8*)(uintptr_t)ptr)[8 + index];
                    PUSH((U64)byte);
                } else if (d->kind == TD_ARRAY) {
                    // 8-byte slot read.
                    U64 v;
                    memcpy(&v, (U8*)(uintptr_t)ptr + 8 + index * 8, 8);
                    PUSH(v);
                } else {
                    RT_ERR("index load not supported for desc kind %d",
                           (int)d->kind);
                }
                break;
            }

            // INDEX_STORE: write an element (array only).
            // Stack: container, index, value -> (nothing).
            //
            // Strings are rejected; the language does not
            // allow `s[i] = c`.
            case OP_INDEX_STORE: {
                U32 desc_idx = (U32)READ();
                const TypeDesc* d = typetable_get(vm->types, desc_idx);
                if (!d) RT_ERR("bad index store desc %u", desc_idx);

                U64 value = POP();
                U64 index = POP();
                U64 ptr   = POP();
                if (!ptr) RT_ERR("store through null%s", "");

                if (d->kind != TD_ARRAY)
                    RT_ERR("index-store not supported for desc kind %d",
                           (int)d->kind);
                U64 len;
                memcpy(&len, (U8*)(uintptr_t)ptr, 8);
                if (index >= len)
                    RT_ERR("index %llu out of bounds (length %llu)",
                           (unsigned long long)index,
                           (unsigned long long)len);
                memcpy((U8*)(uintptr_t)ptr + 8 + index * 8, &value, 8);
                break;
            }
            
            // NEW_ENUM: build an enum variant in the arena.
            // Stack: payloads..., tag -> block_ptr.
            //
            // Note the operand order: the tag is pushed
            // *after* the payloads by codegen, so it is
            // popped first. Then the payloads are popped in
            // reverse order and written into the block in
            // source order.
            //
            // Layout: [u64 tag][payload0][payload1]...
            // Each payload occupies a full 8-byte slot. This
            // is the same layout as a struct, minus the
            // interpretation.
            case OP_NEW_ENUM: {
                U64 payload_count = READ();
                U64 bytes = 8 + payload_count * 8;
                U8* block = arena_alloc(vm, bytes);
                if (!block)
                    RT_ERR("arena out of memory for enum%s", "");
                // Pop tag first, then payloads in reverse order.
                U64 tag = POP();
                memcpy(block, &tag, 8);
                for (U64 i = payload_count; i > 0; i--) {
                    U64 v = POP();
                    memcpy(block + 8 + (i - 1) * 8, &v, 8);
                }
                PUSH((U64)(uintptr_t)block);
                break;
            }

            // NEW_RC_ENUM: RC-managed enum variant. Same
            // layout as NEW_ENUM but with an RcHeader
            // prefix and refcount 1.
            //
            // This is what Result and Option use. The
            // RC-managed flag comes from the TypeDef's
            // is_rc_managed field, which sema sets from
            // the template's is_builtin (true for the
            // prelude's types).
            case OP_NEW_RC_ENUM: {
                U64 payload_count = READ();
                U64 payload_bytes = 8 + payload_count * 8;
                U64 total = sizeof(RcHeader) + payload_bytes;
                RcHeader* h = (RcHeader*)malloc(total);
                if (!h) RT_ERR("rc heap out of memory for enum%s", "");
                h->ref_count = 1;
                h->padding   = 0;
                U8* payload = (U8*)(h + 1);
                U64 tag = POP();
                memcpy(payload, &tag, 8);
                for (U64 i = payload_count; i > 0; i--) {
                    U64 v = POP();
                    memcpy(payload + 8 + (i - 1) * 8, &v, 8);
                }
                PUSH((U64)(uintptr_t)payload);
                break;
            }

            // TRAP: unconditional abort with a pool-string
            // message. Emitted by codegen at the end of a
            // match, as a defensive "no arm matched"
            // handler. In a well-formed program this is
            // never reached (sema verifies exhaustiveness),
            // but a nested subpattern that fails at runtime
            // can fall through to it.
            case OP_TRAP: {
                U32 idx = (U32)READ();
                if (vm->pool && idx < (U32)vm->pool->count) {
                    const Constant* c = &vm->pool->items[idx];
                    if (c->kind == CONST_STRING)
                        RT_ERR("%.*s", (int)c->as.str.length,
                               c->as.str.bytes);
                }
                RT_ERR("trap%s", "");
            }
            
            // ----- Reference counting -----
            //
            // NULL is a valid RC value and both ops are no-ops
            // on it. This is what makes the "function with no
            // value-return on some path" case safe: the caller
            // gets 0, holds_rc is false, no drop.
            //
            // The sentinel check (0xFFFFFFFF) is what makes
            // const-arena strings safe to drop: their
            // refcount is not a real count, and neither INC
            // nor DEC should change it.
            case OP_RC_INC: {
                if (vm->sp == 0) RT_ERR("rc_inc on empty stack%s", "");
                U8* payload = (U8*)(uintptr_t)POP();
                if (!payload) break;
                RcHeader* h = ((RcHeader*)payload) - 1;
                if (h->ref_count == 0xFFFFFFFF) break;  // const-arena sentinel, skip
                h->ref_count++;
                break;
            }
            // RC_DEC: drop one reference. If the count hits
            // zero, free the block.
            //
            // The free is a *shallow* free: it frees the
            // block's memory but does not touch any payload
            // pointers stored inside. For a struct or enum
            // with RC-managed fields/payloads, that leaves
            // the inner values' refcounts unchanged. See the
            // file header on reference counting, and the
            // follow-ups for the leak this creates when an
            // RC-managed enum with an RC-managed payload is
            // dropped without extracting the payload.
            case OP_RC_DEC: {
                U32 desc_idx = (U32)READ();   // descriptor
                if (vm->sp == 0) RT_ERR("rc_dec on empty stack%s", "");
                U64 value = POP();
                if (!value) break;
                const TypeDesc* d = typetable_get(vm->types, desc_idx);
                if (!d) RT_ERR("bad rc_dec desc %u", desc_idx);
                rc_drop_deep(vm, value, d);
                break;
            }

            // ----- Calls -----
            //
            // The caller has pushed `argc` arguments. `fp`
            // becomes the first of them, so locals live at
            // fp+argc, fp+argc+1, …. On RET, the callee's
            // arena is dropped and its stack region is
            // overwritten by the return value.
            //
            // Frame setup order:
            //   1. Push a CallFrame capturing the caller's
            //      return_pc, fp, amp, and scope_depth.
            //   2. fp  = sp - argc      (args become slots 0..argc-1)
            //   3. amp = ap             (callee's arena starts here)
            //   4. pc  = entry_pc       (jump to the body)
            //
            // Note that scope_depth is NOT reset: the callee
            // sees the caller's scope_depth as its starting
            // point. Any ENTER_SCOPE the callee does grows
            // from there, and a "return inside a scope"
            // situation is handled by RET restoring the
            // whole scope_depth in one step (rather than
            // requiring the callee to unwind each scope).
            case OP_CALL: {
                U64 func_idx = READ();
                U64 argc     = READ();

                if (!vm->funcs || func_idx >= (U64)vm->funcs->count)
                    RT_ERR("bad call target %llu",
                           (unsigned long long)func_idx);
                if (vm->call_depth >= CALL_STACK_MAX)
                    RT_ERR("call stack overflow%s", "");
                if ((U64)vm->sp < argc)
                    RT_ERR("stack underflow on call%s", "");

                CallFrame* f = &vm->call_stack[vm->call_depth++];
                f->return_pc   = vm->pc;
                f->fp          = vm->fp;
                f->amp         = vm->amp;
                f->scope_depth = vm->scope_depth;
                f->func_idx    = (int)func_idx;
                // call_pc is the pc of the CALL instruction
                // itself; used by the error trace to show
                // "called from here".
                f->call_pc   = vm->current_pc;

                vm->fp  = vm->sp - (U32)argc;
                vm->amp = vm->ap;
                vm->pc  = vm->funcs->items[func_idx].entry_pc;
                break;
            }

            // Indirect call: the func index is on the stack,
            // underneath the args. Semantics and frame setup
            // are identical to OP_CALL after the callee is
            // removed from the stack.
            case OP_CALL_INDIRECT: {
                U64 argc = READ();

                if ((U64)vm->sp < argc + 1)
                    RT_ERR("stack underflow on indirect call%s", "");
                U64 callee_idx = vm->data_stack[vm->sp - argc - 1];

                // Shift the args down one slot, overwriting
                // the callee. After this, the stack layout is
                // exactly what OP_CALL expects: args on top
                // of the caller's frame.
                for (U64 i = 0; i < argc; i++)
                    vm->data_stack[vm->sp - argc - 1 + i] =
                        vm->data_stack[vm->sp - argc + i];
                vm->sp--;

                if (!vm->funcs || callee_idx >= (U64)vm->funcs->count)
                    RT_ERR("bad indirect call target %llu",
                           (unsigned long long)callee_idx);
                if (vm->call_depth >= CALL_STACK_MAX)
                    RT_ERR("call stack overflow%s", "");
                if ((U64)vm->sp < argc)
                    RT_ERR("stack underflow on call%s", "");

                CallFrame* f = &vm->call_stack[vm->call_depth++];
                f->return_pc   = vm->pc;
                f->fp          = vm->fp;
                f->amp         = vm->amp;
                f->scope_depth = vm->scope_depth;
                f->func_idx    = (int)callee_idx;
                f->call_pc     = vm->current_pc;

                vm->fp  = vm->sp - (U32)argc;
                vm->amp = vm->ap;
                vm->pc  = vm->funcs->items[callee_idx].entry_pc;
                break;
            }
            
            // CONVERT: numeric conversions. Two descriptors:
            // the source and the target. The source tells the
            // VM how to interpret the raw stack word; the
            // target tells it what to produce.
            //
            // The conversion matrix is:
            //   int -> int       truncate/sign-extend
            //   int -> u64       reinterpret bits
            //   f64 -> int       reject on NaN/overflow
            //   int -> f64       C-cast semantics
            //   f64 -> f64       identity (bit-preserving)
            //   anything -> bool (0 if zero/NaN, else 1)
            //   bool -> int      identity
            //
            // The src/tgt predicate macros below are local to
            // this case and undef'd at the end.
            case OP_CONVERT: {
                U32 from_idx = (U32)READ();
                U32 to_idx   = (U32)READ();
                const TypeDesc* from = typetable_get(vm->types, from_idx);
                const TypeDesc* to   = typetable_get(vm->types, to_idx);
                if (!from || !to) RT_ERR("bad convert descriptors%s", "");
                if (vm->sp == 0)  RT_ERR("convert on empty stack%s", "");

                U64 raw = POP();
                U64 result = 0;

                // Helper predicates expressed as switches.
                #define SRC_IS_INT_SIGNED \
                    (from->kind == TD_I8  || from->kind == TD_I16 || \
                     from->kind == TD_I32 || from->kind == TD_I64)
                #define SRC_IS_INT_UNSIGNED \
                    (from->kind == TD_U8  || from->kind == TD_U16 || \
                     from->kind == TD_U32 || from->kind == TD_U64)
                #define SRC_IS_INT \
                    (SRC_IS_INT_SIGNED || SRC_IS_INT_UNSIGNED || \
                     from->kind == TD_BOOL)
                #define SRC_IS_F64  (from->kind == TD_F64)
                #define SRC_IS_F32  (from->kind == TD_F32)

                switch (to->kind) {
                    // To signed int. NaN and out-of-range
                    // doubles are errors (the cast would be
                    // UB in C).
                    case TD_I8: case TD_I16: case TD_I32: case TD_I64: {
                        I64 v;
                        if (SRC_IS_INT) {
                            v = (I64)raw;
                        } else if (SRC_IS_F64) {
                            double d; memcpy(&d, &raw, 8);
                            if (d != d)          RT_ERR("cast of NaN to integer%s", "");
                            if (d >  9.2e18)     RT_ERR("cast overflow%s", "");
                            if (d < -9.2e18)     RT_ERR("cast overflow%s", "");
                            v = (I64)d;
                        } else if (SRC_IS_F32) {
                            U32 bits = (U32)raw;
                            float f; memcpy(&f, &bits, 4);
                            if (f != f)          RT_ERR("cast of NaN to integer%s", "");
                            if (f >  9.2e18f)    RT_ERR("cast overflow%s", "");
                            if (f < -9.2e18f)    RT_ERR("cast overflow%s", "");
                            v = (I64)f;
                        } else {
                            RT_ERR("cannot convert to signed integer%s", "");
                        }
                        // Narrow to the target's width, then
                        // sign-extend back to I64 for storage.
                        switch (to->kind) {
                            case TD_I8:  result = (U64)(I64)(int8_t)v;   break;
                            case TD_I16: result = (U64)(I64)(int16_t)v;  break;
                            case TD_I32: result = (U64)(I64)(int32_t)v;  break;
                            case TD_I64: result = (U64)v;                break;
                            default: break;
                        }
                        break;
                    }
                    // To unsigned int. Negative doubles and
                    // NaN are errors.
                    case TD_U8: case TD_U16: case TD_U32: case TD_U64: {
                        U64 v;
                        if (SRC_IS_INT) {
                            v = raw;
                        } else if (SRC_IS_F64) {
                            double d; memcpy(&d, &raw, 8);
                            if (d != d)          RT_ERR("cast of NaN to integer%s", "");
                            if (d < 0.0)         RT_ERR("cast of negative to unsigned%s", "");
                            if (d > 1.8e19)      RT_ERR("cast overflow%s", "");
                            v = (U64)d;
                        } else if (SRC_IS_F32) {
                            U32 bits = (U32)raw;
                            float f; memcpy(&f, &bits, 4);
                            if (f != f)          RT_ERR("cast of NaN to integer%s", "");
                            if (f >  9.2e18f)    RT_ERR("cast overflow%s", "");
                            if (f < -9.2e18f)    RT_ERR("cast overflow%s", "");
                            v = (I64)f;
                        } else {
                            RT_ERR("cannot convert to unsigned integer%s", "");
                        }
                        switch (to->kind) {
                            case TD_U8:  result = (U64)(uint8_t)v;   break;
                            case TD_U16: result = (U64)(uint16_t)v;  break;
                            case TD_U32: result = (U64)(uint32_t)v;  break;
                            case TD_U64: result = v;                 break;
                            default: break;
                        }
                        break;
                    }
                    // To f64. Signed ints and bool cast via
                    // (double)(I64); unsigned via (double)U64.
                    case TD_F64: {
                        double d;
                        if (SRC_IS_INT_SIGNED || from->kind == TD_BOOL) {
                            d = (double)(I64)raw;
                        } else if (SRC_IS_INT_UNSIGNED) {
                            d = (double)raw;
                        } else if (SRC_IS_F64) {
                            memcpy(&d, &raw, 8);
                        } else if (SRC_IS_F32) {
                            U32 bits = (U32)raw;
                            float f; memcpy(&f, &bits, 4);
                            d = (double)f;
                        } else {
                            RT_ERR("cannot convert to f64%s", "");
                        }
                        memcpy(&result, &d, 8);
                        break;
                    }
                    case TD_F32: {
                        float f;
                        if (SRC_IS_INT_SIGNED || from->kind == TD_BOOL) {
                            f = (float)(I64)raw;
                        } else if (SRC_IS_INT_UNSIGNED) {
                            f = (float)raw;
                        } else if (SRC_IS_F64) {
                            double d; memcpy(&d, &raw, 8);
                            f = (float)d;
                        } else if (SRC_IS_F32) {
                            U32 bits = (U32)raw;
                            memcpy(&f, &bits, 4);
                        } else {
                            RT_ERR("cannot convert to f32%s", "");
                        }
                        U32 bits; memcpy(&bits, &f, 4);
                        result = (U64)bits;
                        break;
                    }
                    
                    // To bool. Zero and NaN are false;
                    // everything else is true.
                    case TD_BOOL: {
                        if (SRC_IS_INT) {
                            result = (raw != 0) ? 1 : 0;
                        } else if (SRC_IS_F64) {
                            double d; memcpy(&d, &raw, 8);
                            result = (d != 0.0 && d == d) ? 1 : 0;
                        } else if (SRC_IS_F32) {
                            U32 bits = (U32)raw;
                            float f; memcpy(&f, &bits, 4);
                            result = (f != 0.0f && f == f) ? 1 : 0;
                        } else {
                            RT_ERR("cannot convert to bool%s", "");
                        }
                        break;
                    }
                    default:
                        RT_ERR("conversion to descriptor kind %d not supported",
                               (int)to->kind);
                }

                #undef SRC_IS_INT_SIGNED
                #undef SRC_IS_INT_UNSIGNED
                #undef SRC_IS_INT
                #undef SRC_IS_F64
                #undef SRC_IS_F32

                PUSH(result);
                break;
            }
            
            // RET: return from a function. Pops the return
            // value, tears down the callee's frame, and
            // jumps back to the caller's resume point.
            //
            // Order of operations matters:
            //   1. Pop the return value (top of stack).
            //   2. Free the callee's arena: ap = amp.
            //   3. Truncate the stack to fp (dropping args
            //      and locals).
            //   4. Push the return value (so the caller sees
            //      exactly one new value).
            //   5. Restore fp, amp, scope_depth from the
            //      frame.
            //   6. Jump to return_pc.
            //
            // Scope depth: the callee may have unbalanced
            // ENTER_SCOPEs if it never exited them before
            // returning. Restoring scope_depth from the
            // frame discards those, which is the right
            // behavior.
            case OP_RET: {
                if (vm->call_depth == 0)
                    RT_ERR("return outside function%s", "");
                if (vm->sp <= vm->fp)
                    RT_ERR("return with no value%s", "");

                U64 ret = POP();

                // Free the callee's arena and reset the scope
                // stack to the caller's depth.
                vm->ap = vm->amp;

                // Drop args + locals: reset sp to the frame base.
                vm->sp = vm->fp;
                PUSH(ret);

                CallFrame* f = &vm->call_stack[--vm->call_depth];
                vm->fp          = f->fp;
                vm->amp         = f->amp;
                vm->scope_depth = f->scope_depth;
                vm->pc          = f->return_pc;
                break;
            }

            // CCALL: dispatch to a native function. The
            // native receives `vm` and is responsible for
            // popping its own arguments and pushing its own
            // result.
            //
            // Error handling: natives set `native_failed`
            // via NATIVE_ERR and return. This case checks
            // the flag after the call and propagates the
            // error into vm_run's return value.
            case OP_CCALL: {
                U32 idx = (U32)READ();
                if (idx >= NATIVE_COUNT || !vm->natives[idx])
                    RT_ERR("bad native index %u", idx);
                vm->native_failed = false;
                vm->natives[idx](vm);
                if (vm->native_failed) return 1;
                break;
            }

            // FFI_CALL: foreign function call. The heavy
            // lifting is in ffi_call_dispatch; this case
            // just reads the operands and delegates.
            case OP_FFI_CALL: {
                U32 fidx = (U32)READ();
                U32 argc = (U32)READ();
                if (ffi_call_dispatch(vm, fidx, argc) != 0) return 1;
                break;
            }

            // TRY_JUMP_IF_OK: the `?` operator.
            //
            // Stack effect: peek at top-of-stack.
            //   - If the block's tag is 0 (Ok / Some):
            //       RC_DEC the block (inline, not via
            //       OP_RC_DEC); replace the block's slot
            //       with the payload; jump to target.
            //   - If tag != 0 (Err / None): leave the
            //       block on the stack and fall through.
            //       The codegen emits an OP_RET immediately
            //       after this instruction; the RET returns
            //       the block as the function's result.
            //
            // The RC_DEC on success is what transfers
            // ownership of the payload from the enum block
            // to the caller. The payload's refcount is not
            // modified: the enum and the payload are
            // independent RC allocations, and dropping the
            // enum does not change the payload's count.
            case OP_TRY_JUMP_IF_OK: {
                    U32 target = (U32)READ();
                    if (vm->sp == 0)
                        RT_ERR("`?` on empty stack%s", "");
                    U64 block = vm->data_stack[vm->sp - 1];
                    if (!block)
                        RT_ERR("`?` on null Result/Option%s", "");
                    U64 tag;
                    memcpy(&tag, (U8*)(uintptr_t)block, 8);
                    if (tag == 0) {
                        U64 payload;
                        memcpy(&payload, (U8*)(uintptr_t)block + 8, 8);
                        // Result/Option are always RC-managed; drop the
                        // block now that we have its payload. Payloads are
                        // 64-bit values or pointers to other allocations,
                        // never into the block itself, so this is safe.
                        RcHeader* h = ((RcHeader*)(uintptr_t)block) - 1;
                        if (--h->ref_count == 0) free(h);
                        vm->data_stack[vm->sp - 1] = payload;
                        vm->pc = target;
                    }
                    // tag != 0: leave block; caller emits OP_RET
                    break;
                }
                
            // Unknown opcode. The validator should have
            // caught this before execution; reaching here
            // means either the validator was skipped or
            // there is a bug in the validator.
            default:
                RT_ERR("unknown opcode %d", (int)op);
        }
    }
}