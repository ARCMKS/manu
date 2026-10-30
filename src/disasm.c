// ============================================================
// disasm.c — Bytecode listing and structural validation.
//
// The disassembler is a view on a chunk, not a stage. It reads
// the code array, the parallel span array, the constant pool,
// and the source map, and prints a human-readable listing.
//
// A single `OPINFO` table is the source of truth for each
// opcode's name and operand count. Sub-operands (OpBinary,
// OpUnary, OpValueType) are rendered by their own tables. This
// prevents the classic drift where a new opcode is added to the
// name table but not to the operand-count switch.
//
// The validator is a separate entry point. It checks structure
// without printing: opcodes in range, no truncated instructions,
// jump and call targets within the code, constant indices in the
// pool, sub-operand values in their enums. main.c runs it before
// every execution; tests run it as part of compile-and-run.
//
// Reading order:
//   1. Opcode metadata (OPINFO table, operand_count).
//   2. Sub-operand name tables (op_binary_name, op_unary_name).
//   3. Listing helpers (print_desc, dump_raw, dump_pool).
//   4. disassemble — the printer.
//   5. validate_bytecode — the structural checker.
// ============================================================
#include "../include/disasm.h"
#include "../include/vm.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

// ============================================================
// Opcode metadata
// ============================================================

// One row per opcode: the mnemonic and how many operand
// words follow. `operands` is the *word count*, not the
// number of logical operands (a CALL has two words: the
// func index and the argc).
//
// The table is filled by designated initializers indexed by
// the OpCode enum value. Any opcode with no entry has a
// NULL `name` and zero `operands`. The NULL case is caught
// by disasm_op_name (which returns "<unnamed-op>") and by
// operand_count (which returns 0).
//
// Adding a new opcode is a two-step change: add the enum
// value in vm.h and add the row here. Forgetting the second
// step is a silent bug (the opcode will be rendered as
// unnamed with 0 operands, and the validator will not check
// its operand count); the compiler cannot catch it because
// designated initializers make the enum and the table
// independent.
typedef struct {
    const char* name;
    int         operands;   // word count after the opcode
} OpInfo;

static const OpInfo OPINFO[OP_COUNT] = {
    [OP_HALT]            = { "HALT",            0 },
    [OP_PUSH]            = { "PUSH",            1 },
    [OP_POP]             = { "POP",             0 },
    [OP_DUP]             = { "DUP",             0 },
    [OP_DUP2]            = { "DUP2",            0 },
    [OP_LOAD_LOCAL]      = { "LOAD_LOCAL",      1 },
    [OP_STORE_LOCAL]     = { "STORE_LOCAL",     1 },
    [OP_LOAD_ADDR_LOCAL] = { "LOAD_ADDR_LOCAL", 1 },
    [OP_LOAD_INDIRECT]   = { "LOAD_INDIRECT",   2 },
    [OP_STORE_INDIRECT]  = { "STORE_INDIRECT",  2 },
    [OP_LOAD_ADDR_INDIRECT] = { "LOAD_ADDR_INDIRECT", 1},
    [OP_LOAD_CONST]      = { "LOAD_CONST",      1 },
    [OP_LEN]             = { "LEN",             1 },
    [OP_BINARY]          = { "BINARY",          2 },
    [OP_UNARY]           = { "UNARY",           2 },
    [OP_JMP]             = { "JMP",             1 },
    [OP_JMP_IF_FALSE]    = { "JMP_IF_FALSE",    1 },
    [OP_JMP_IF_TRUE]     = { "JMP_IF_TRUE",     1 },
    [OP_CALL]            = { "CALL",            2 },
    [OP_CALL_INDIRECT]   = { "CALL_INDIRECT",   1 },
    [OP_RET]             = { "RET",             0 },
    [OP_ENTER_SCOPE]     = { "ENTER_SCOPE",     0 },
    [OP_EXIT_SCOPE]      = { "EXIT_SCOPE",      0 },
    [OP_ALLOC]           = { "ALLOC",           1 },
    [OP_ALLOC_RC]        = { "ALLOC_RC",        1 },
    [OP_RC_INC]          = { "RC_INC",          0 },
    [OP_RC_DEC]          = { "RC_DEC",          1 },
    [OP_NEW_ARRAY]       = { "NEW_ARRAY",       1 },
    [OP_NEW_RC_ARRAY]    = { "NEW_RC_ARRAY",    1 },
    [OP_INDEX_LOAD]      = { "INDEX_LOAD",      1 },
    [OP_INDEX_STORE]     = { "INDEX_STORE",     1 },
    [OP_NEW_ENUM]        = { "NEW_ENUM",        1 },
    [OP_NEW_RC_ENUM]     = { "NEW_RC_ENUM",     1 },
    [OP_TRAP]            = { "TRAP",            1 },
    [OP_CONVERT]         = { "CONVERT",         2 },
    [OP_CCALL]           = { "CCALL",           1 },
    [OP_FFI_CALL]        = { "FFI_CALL",        2 },
    [OP_TRY_JUMP_IF_OK]  = { "TRY_JUMP_IF_OK",  1 },
};

// Human-readable name for an opcode. Two failure modes:
//   - out-of-range op: "<unknown>".
//   - in-range but the OPINFO row is NULL (should not happen
//     for any opcode the enum knows about, but the NULL
//     check is defensive): "<unnamed-op>".
const char* disasm_op_name(int op) {
    if (op < 0 || op >= OP_COUNT) return "<unknown>";
    if (OPINFO[op].name == NULL) return "<unnamed-op>";
    return OPINFO[op].name;
}

// Operand word count for an opcode. Out-of-range ops return 0
// (the disassembler will then try to render the opcode word
// itself as an "operand", which is harmless because the
// listing will show the raw value).
//
// This is the single source of truth for the instruction
// decoder's stride. Both disassemble and validate_bytecode
// use it to advance through the code array. If a new opcode
// is added to OPINFO with the wrong operand count, both the
// listing and the validator will mis-step; that is the
// price of a table-driven decoder.
static int operand_count(int op) {
    if (op < 0 || op >= OP_COUNT) return 0;
    return OPINFO[op].operands;
}

// ============================================================
// Sub-operand names
// ============================================================

// Name for an OpBinary sub-operand. The table is indexed by
// the OpBinary enum (see vm.h); the name returned is the
// operator mnemonic without any operand. Used by the
// disassembler's OP_BINARY rendering ("ADD i64").
//
// Out-of-range sub-ops return "?", which is what the
// validator would catch (as a malformed instruction) if it
// were reached; the disassembler just prints the "?".
static const char* op_binary_name(U32 op) {
    static const char* names[OPB_COUNT] = {
        "ADD", "SUB", "MUL", "DIV",
        "EQ",  "NE",  "LT",  "LE", "GT", "GE",
    };
    return op < OPB_COUNT ? names[op] : "?";
}

// Name for an OpUnary sub-operand. Two entries: NEG and NOT.
static const char* op_unary_name(U32 op) {
    static const char* names[OPU_COUNT] = { "NEG", "NOT" };
    return op < OPU_COUNT ? names[op] : "?";
}

static const char* memkind_name(U32 k) {
    static const char* names[MEM_COUNT] = {
        "i8",  "i16", "i32", "i64",
        "u8",  "u16", "u32", "u64",
        "f32", "f64", "ptr",
    };
    return k < MEM_COUNT ? names[k] : "?";
} 

// ============================================================
// Listing helpers
// ============================================================

// Print a type descriptor recursively by index. Used by the
// disassembler's rendering of opcodes that take a type
// descriptor operand (OP_LEN, OP_BINARY, OP_PRINT, ...).
//
// Recursive for compound kinds: `ref<X>` prints "ref<" then
// recursively prints X then ">". The `print_desc` calls for
// the inner types recurse through the descriptor's `aux`
// field (the index of the inner descriptor).
//
// Note that the recursion has no cycle detection. The VM's
// TypeTable is built by interning, which does not produce
// cycles, so this is safe today. If a future change allowed
// a descriptor to point back to itself, this would infinite-
// recurse.
static void print_desc(uint32_t idx, const TypeTable* types) {
    const TypeDesc* d = typetable_get(types, idx);
    if (!d) { printf("td#%u", idx); return; }
    switch (d->kind) {
        case TD_VOID:   printf("void");   break;
        case TD_I8:     printf("i8");     break;
        case TD_I16:    printf("i16");    break;
        case TD_I32:    printf("i32");    break;
        case TD_I64:    printf("i64");    break;
        case TD_U8:     printf("u8");     break;
        case TD_U16:    printf("u16");    break;
        case TD_U32:    printf("u32");    break;
        case TD_U64:    printf("u64");    break;
        case TD_F32:    printf("f32");    break;
        case TD_F64:    printf("f64");    break;
        case TD_BOOL:   printf("bool");   break;
        case TD_STRING: printf("string"); break;
        // Compound cases: open, recurse, close.
        case TD_REF:    printf("ref<");   print_desc(d->aux, types); printf(">"); break;
        case TD_RC:     printf("rc<");    print_desc(d->aux, types); printf(">"); break;
        case TD_RAWPTR: printf("*");      print_desc(d->aux, types);         break;
        case TD_ARRAY:  printf("[");      print_desc(d->aux, types); break;
        // Struct and enum descriptors do not carry their
        // fields or variant names in the TypeDesc itself;
        // those live in the side tables (fields,
        // enum_variants). The disassembler does not look
        // them up, so it renders only a placeholder.
        case TD_STRUCT: printf("struct#%u", d->aux); break;
        case TD_EXTERN_STRUCT: printf("extern-struct#%u",d->aux); break;
        case TD_ENUM:   printf("enum"); break;
        default:        printf("td#%u", idx); break;
            
    }
}

// Print the raw code array as hex and decimal. Debug-only;
// gated by a static flag inside disassemble (see below).
//
// Each line is `offset  hex_value  decimal_value`. The
// output is not parsed by anything; it exists to help debug
// a misbehaving codegen.
static void dump_raw(const U64* code, size_t length) {
    printf("--- raw words (%zu) ---\n", length);
    for (size_t i = 0; i < length; i++)
        printf("  %04zu  0x%016" PRIx64 "  %" PRIu64 "\n",
               i, code[i], code[i]);
    printf("\n");
}

// Print the constant pool. Called unconditionally (the
// disassemble gate is `debug_dump_pool` below).
//
// One entry per line, with the kind and the value. Strings
// are rendered with a small escape table (newline, tab,
// return, quote, backslash) and non-printable bytes as
// \xNN. Cstrings get an extra `\0` marker at the end to
// remind the reader that they are NUL-terminated at
// runtime.
static void dump_pool(const ConstantPool* pool) {
    if (!pool || pool->count == 0) {
        printf("--- pool: (empty) ---\n\n");
        return;
    }
    printf("--- pool (%d) ---\n", pool->count);
    for (int i = 0; i < pool->count; i++) {
        const Constant* c = &pool->items[i];
        switch (c->kind) {
            case CONST_I64:
                printf("  [%d] i64     %lld\n", i, (long long)c->as.i64);
                break;
            case CONST_F64:
                printf("  [%d] f64     %g\n", i, c->as.f64);
                break;
            case CONST_BOOL:
                printf("  [%d] bool    %s\n", i, c->as.b ? "true" : "false");
                break;
            case CONST_STRING:
                printf("  [%d] string  len=%u  \"", i, c->as.str.length);
                for (uint32_t j = 0; j < c->as.str.length; j++) {
                    unsigned char ch = (unsigned char)c->as.str.bytes[j];
                    switch (ch) {
                        case '\n': fputs("\\n", stdout); break;
                        case '\t': fputs("\\t", stdout); break;
                        case '\r': fputs("\\r", stdout); break;
                        case '"':  fputs("\\\"", stdout); break;
                        case '\\': fputs("\\\\", stdout); break;
                        default:
                            if (ch >= 32 && ch < 127) putchar(ch);
                            else printf("\\x%02x", ch);
                    }
                }
                printf("\"\n");
                break;
            case CONST_CSTRING:
                // Same as string but with a trailing \0
                // marker (cstrings are NUL-terminated at
                // runtime; the pool bytes themselves are
                // not, but the marker reminds the reader).
                printf("  [%d] cstring len=%u  \"", i, c->as.str.length);
                for (uint32_t j = 0; j < c->as.str.length; j++) {
                    unsigned char ch = (unsigned char)c->as.str.bytes[j];
                    if (ch >= 32 && ch < 127) putchar(ch);
                    else printf("\\x%02x", ch);
                }
                printf("\"\\0\n");
                break;

            
        }
    }
    printf("\n");
}

// ============================================================
// Disassembly
// ============================================================

// Print a full listing of a chunk.
//
// Layout of the output:
//   - a header line with the chunk name and word count;
//   - an optional raw dump (gated by debug_dump_raw, off);
//   - an optional pool dump (gated by debug_dump_pool, on);
//   - a decoded listing, one instruction per line, with
//     columns: offset, location, mnemonic, operands.
//
// The instruction loop walks the code array by
// operand_count(op) + 1 words at a time. Each instruction
// is decoded and rendered by a switch on the opcode.
//
// Location column: uses the parallel lines[] and cols[]
// arrays. If the chunk has no debug info (lines == NULL) or
// the line at this offset is 0, the column shows "<none>".
//
// Truncation: if an instruction's operands would extend
// past the end of the code array, the listing prints a
// "<truncated>" line and stops. That is the only error the
// disassembler reports; it is a debug output, not a
// validator.
//
// The HALT case does not stop the listing: dead code after
// a HALT is worth seeing, so the loop continues and prints
// a note.
void disassemble(const Chunk* chunk,
                 const SourceMap* smap,
                 const char* file_path,
                 const char* name) {
    const U64*           code   = chunk->code;
    size_t               length = (size_t)chunk->count;
    const uint32_t*      lines  = chunk->lines;
    const uint32_t*      cols   = chunk->cols;
    const ConstantPool*  pool   = &chunk->pool;
    const TypeTable*     types  = &chunk->types;
    const FuncTable*     funcs  = &chunk->funcs;

    printf("==========================================\n");
    printf("Bytecode listing: %s (%zu words)\n",
           name ? name : "<anonymous>", length);
    printf("==========================================\n");

    // set this true to debug dump raw hex code, pool
    //
    // These are compile-time toggles used during
    // development. debug_dump_pool is on by default
    // because the pool is usually the interesting part
    // when a program misbehaves; debug_dump_raw is off
    // because it duplicates the decoded listing at a
    // lower level and is rarely useful.
    bool debug_dump_raw = false;
    bool debug_dump_pool = true;
    if(debug_dump_raw) dump_raw(code, length);
    if (debug_dump_pool) dump_pool(pool);

    printf("--- decoded ---\n");
    printf("%-6s %-24s %-16s %s\n",
           "off", "location", "mnemonic", "operands");

    size_t i = 0;
    while (i < length) {
        U64 raw = code[i];
        int op  = (int)raw;
        int n   = operand_count(op);

        // Truncation guard: the last instruction must have all
        // its operands inside the code array.
        if (i + 1 + (size_t)n > length) {
            printf("%-6zu %-24s %-16s <truncated: needs %d operands>\n",
                   i, "<none>", "<error>", n);
            break;
        }

        // Location column: decode the span into file:line:col.
        // Location column: line:col from the parallel arrays, or
        // <none> if the chunk has no debug info.
        //
        // Note: the comment above ("decode the span") is a
        // leftover from an earlier design where the chunk
        // stored spans; the actual implementation reads
        // the precomputed lines[] and cols[] arrays.
        char locbuf[64] = "<none>";
        if (lines && cols && lines[i] > 0) {
            if (file_path)
                snprintf(locbuf, sizeof locbuf, "%s:%u:%u",
                         file_path, lines[i], cols[i]);
            else
                snprintf(locbuf, sizeof locbuf, "%u:%u",
                         lines[i], cols[i]);
        }

        printf("%-6zu %-24s %-16s", i, locbuf, disasm_op_name(op));

        // Operand rendering. Each opcode that takes operands
        // renders them by name/meaning; the default case is
        // silent, which is correct for opcodes with no operands.
        //
        // The operand words have already been bounds-checked
        // above (the truncation guard), so every code[i+k]
        // below is safe.
        switch (op) {
            case OP_PUSH: {
                // The operand is a raw word; render it as a
                // signed integer, which is what a source-level
                // PUSH would have been.
                I64 v = (I64)code[i + 1];
                printf("%lld", (long long)v);
                break;
            }
            case OP_LOAD_LOCAL:
            case OP_STORE_LOCAL:
            case OP_LOAD_ADDR_LOCAL:
                // Slot number.
                printf("slot %" PRIu64, code[i + 1]);
                break;
            case OP_LOAD_INDIRECT:
            case OP_STORE_INDIRECT:
                printf("offset %" PRIu64 " %s",
                       code[i + 1], memkind_name((U32)code[i + 2]));
                break;

            case OP_LOAD_ADDR_INDIRECT:
                printf("offset %" PRIu64, code[i + 1]);
                break;
            case OP_LOAD_CONST: {
                // Pool index, plus a comment showing the
                // pooled value.
                U64 idx = code[i + 1];
                printf("[%" PRIu64 "]", idx);
                if (pool && idx < (U64)pool->count) {
                    const Constant* c = &pool->items[idx];
                    switch (c->kind) {
                        case CONST_I64:
                            printf("   ; i64 %lld",
                                   (long long)c->as.i64);
                            break;
                        case CONST_F64:
                            printf("    ; f64 %lf",
                                    (double)c->as.f64);
                            break;
                        case CONST_BOOL:
                            printf("   ; bool %d", (int)c->as.b);
                            break;
                        case CONST_STRING:
                            printf("   ; string len=%u \"%.*s\"",
                                   c->as.str.length,
                                   (int)c->as.str.length,
                                   c->as.str.bytes);
                            break;
                        case CONST_CSTRING:
                            printf("   ; cstring len=%u \"%.*s\\0\"",
                                   c->as.str.length,
                                   (int)c->as.str.length,
                                   c->as.str.bytes);
                            break;
                    }
                } else {
                    // Pool index is out of bounds; this is a
                    // structural error, but the disassembler
                    // just notes it rather than aborting.
                    printf("   ; !! pool has %d entries",
                           pool ? pool->count : 0);
                }
                break;
            }

            case OP_LEN:
                // Container descriptor.
                print_desc((U32)code[i + 1], types);
                break;

            case OP_INDEX_LOAD:
            case OP_INDEX_STORE:
                // Container descriptor.
                print_desc((U32)code[i + 1], types);
                break;
            
            case OP_BINARY:
                // "ADD i64" etc.
                printf("%s ", op_binary_name((U32)code[i + 1]));
                print_desc((U32)code[i + 2], types);
                break;
            case OP_UNARY:
                // "NEG i64" etc.
                printf("%s ", op_unary_name((U32)code[i + 1]));
                print_desc((U32)code[i + 2], types);
                break;
            case OP_JMP:
            case OP_JMP_IF_TRUE:
            case OP_JMP_IF_FALSE:
                // Target PC.
                printf("-> %" PRIu64, code[i + 1]);
                break;

            case OP_TRY_JUMP_IF_OK:
                // Target PC on success.
                printf("-> %" PRIu64, code[i + 1]);
                break;
                // OP_CALL now prints the callee's printable name
                // (a source name for user functions, a mangled
                // name for instantiations). Falls back to the
                // Token text via the SourceMap if name_buf is
                // empty (should not happen for codegen output).
            case OP_CALL: {
                U64 fidx = code[i + 1];
                U64 argc = code[i + 2];
                char fname[128] = "<unknown>";
                if (funcs && fidx < (U64)funcs->count) {
                    if (funcs->items[fidx].name_buf[0]) {
                        snprintf(fname, sizeof fname, "%s",
                                    funcs->items[fidx].name_buf);
                    } else if (smap) {
                        smap_span_text(smap,
                            funcs->items[fidx].name.span,
                            fname, sizeof fname);
                    }
                }
                printf("%s argc=%" PRIu64, fname, argc);
                break;
            }
            case OP_CALL_INDIRECT:
                printf("argc=%" PRIu64, code[i + 1]);
                break;
                
            case OP_CCALL: {
                U64 idx = code[i + 1];
                // Full native-name table, indexed by NativeId. The
                // designated initializers keep the mapping stable if
                // the enum is ever reordered; a missing entry renders
                // as "native#N" via the range check below.
                static const char* const NATIVE_NAMES[NATIVE_COUNT] = {
                    [NATIVE_STDOUT_WRITE] = "stdout_write",
                    [NATIVE_STDERR_WRITE] = "stderr_write",
                    [NATIVE_FFI_CSTRING]  = "ffi_cstring",
                    [NATIVE_FFI_STRING]   = "ffi_string",
                    [NATIVE_FFI_ERROR]    = "ffi_error",
                    [NATIVE_FFI_ERRNO]    = "ffi_errno",
                    [NATIVE_RAW_NULL]     = "raw_null",
                    [NATIVE_RAW_IS_NULL]  = "raw_is_null",
                    [NATIVE_RAW_OFFSET]   = "raw_offset",
                    [NATIVE_RAW_LOAD_U8]  = "raw_load_u8",
                    [NATIVE_RAW_STORE_U8] = "raw_store_u8",
                    [NATIVE_FFI_ALLOC]    = "ffi_alloc",
                    [NATIVE_FFI_FREE]     = "ffi_free",
                };
                if (idx < NATIVE_COUNT && NATIVE_NAMES[idx])
                    printf("%s", NATIVE_NAMES[idx]);
                else
                    printf("native#%" PRIu64, idx);
                break;
            }

            case OP_FFI_CALL:
                printf("ffi#%" PRIu64 " argc=%" PRIu64,
                       code[i + 1], code[i + 2]);
                break;
            
            case OP_ALLOC:
            case OP_ALLOC_RC:
                // Byte size.
                printf("size=%" PRIu64, code[i + 1]);
                break;
            case OP_NEW_ARRAY:
            case OP_NEW_RC_ARRAY:
                // Element count.
                printf("n=%" PRIu64, code[i + 1]);
                break;
            case OP_NEW_ENUM:
            case OP_NEW_RC_ENUM:
                // Payload count.
                printf("n=%" PRIu64, code[i + 1]);
                break;

            case OP_RC_DEC:
                print_desc((U32)code[i + 1], types);
                break;
                
            case OP_TRAP: {
                // Pool index of the trap message.
                U64 idx = code[i + 1];
                printf("[%" PRIu64 "]", idx);
                if (pool && idx < (U64)pool->count) {
                    const Constant* c = &pool->items[idx];
                    if (c->kind == CONST_STRING)
                        printf("   ; \"%.*s\"",
                               (int)c->as.str.length, c->as.str.bytes);
                }
                break;
            }
            case OP_CONVERT:
                // "from -> to" descriptors.
                print_desc((U32)code[i + 1], types);
                printf(" -> ");
                print_desc((U32)code[i + 2], types);
                break;
                
            default:
                // Opcodes with no operands, or that render
                // nothing extra.
                break;
        }
        printf("\n");

        i += 1 + (size_t)n;

        // Do not stop at HALT. Dead code after a HALT is worth
        // seeing; the listing is more useful when complete.
        if (i >= length) break;
        if (op == OP_HALT)
            printf("        (--- %zu words past this HALT ---)\n",
                   length - i);
    }
    printf("\n");
}

// ============================================================
// Validation
//
// A structural pass. It does not execute, does not print the
// program, and does not reason about types. It checks the
// invariants that the VM assumes: every opcode is a valid one,
// no instruction is truncated, every jump/call target is
// within the code, every constant index is within the pool,
// every sub-op operand is within its enum.
//
// Returns 0 if the bytecode is well-formed, -1 otherwise. On
// failure it prints the first structural violation to stderr.
//
// The "first violation and stop" behavior is deliberate:
// after one bad instruction the rest of the code is
// unreliable (the stride may be wrong), so reporting further
// violations would produce noise. The caller either fixes
// the underlying bug or the code is not executable.
//
// Per-opcode checks are in the switch below. Every check
// mirrors an invariant that the VM's dispatch loop relies on
// (a check the VM does not itself perform, because it trusts
// codegen). This is the layering: codegen produces, validator
// checks, VM executes without checking.
// ============================================================
int validate_bytecode(const U64* code, size_t length,
                      const ConstantPool* pool,
                      const FuncTable* funcs,
                      const TypeTable* types) {
    size_t i = 0;
    while (i < length) {
        U64 raw = code[i];
        // Opcode range check.
        if (raw >= OP_COUNT) {
            fprintf(stderr,
                "validator: unknown opcode %" PRIu64 " at %zu\n", raw, i);
            return -1;
        }
        int op = (int)raw;
        int n  = operand_count(op);
        // Truncation: the last instruction must have all its
        // operand words inside the code.
        if (i + 1 + (size_t)n > length) {
            fprintf(stderr,
                "validator: truncated instruction at %zu\n", i);
            return -1;
        }
        // Per-opcode checks.
        switch (op) {
            case OP_JMP:
            case OP_JMP_IF_TRUE:
            case OP_JMP_IF_FALSE: {
                // Jump targets must land inside the code.
                // Note: a target equal to `length` is
                // allowed (it is the "one past the end"
                // position that a jump to the end of the
                // code would have). The check is
                // `>= length` (reject), so length itself
                // is rejected. In practice jumps never
                // target exactly length; they target the
                // next instruction's offset, which is <
                // length for any non-empty program.
                U64 target = code[i + 1];
                if (target >= length) {
                    fprintf(stderr,
                        "validator: %s at %zu targets %" PRIu64
                        " but code is %zu words\n",
                        disasm_op_name(op), i, target, length);
                    return -1;
                }
                break;
            }
            case OP_CALL: {
                // Function index must be in the func table.
                U64 fidx = code[i + 1];
                if (funcs && fidx >= (U64)funcs->count) {
                    fprintf(stderr,
                        "validator: CALL at %zu func#%llu "
                        "but chunk has %d functions\n",
                        i, (unsigned long long)fidx, funcs->count);
                    return -1;
                }
                break;
            }
            case OP_CALL_INDIRECT:
                if (code[i + 1] > 8) {
                    fprintf(stderr,
                        "validator: CALL_INDIRECT argc=%" PRIu64
                        " exceeds 8 at %zu\n", code[i + 1], i);
                    return -1;
                }
                break;
            

            case OP_FFI_CALL:
                // Best-effort: we don't have the FFITable here.
                // The argc cap (16) is the size of the VM's
                // marshalling arrays, so a call with more
                // args would overflow them. That is the
                // reason for the check.
                if (code[i + 2] > 16) {
                    fprintf(stderr,
                        "validator: FFI_CALL argc=%" PRIu64 " exceeds 16 at %zu\n",
                        code[i + 2], i);
                    return -1;
                }
                break;
            
            case OP_LOAD_CONST: {
                // Pool index must be in the pool.
                U64 idx = code[i + 1];
                if (!pool || idx >= (U64)pool->count) {
                    fprintf(stderr,
                        "validator: LOAD_CONST at %zu idx=%" PRIu64
                        " but pool has %d entries\n",
                        i, idx, pool ? pool->count : 0);
                    return -1;
                }
                break;
            }
            case OP_LEN:
            case OP_INDEX_LOAD:
            case OP_INDEX_STORE:
                // Type descriptor index must be valid.
                if (!typetable_get(types, (U32)code[i + 1])) {
                    fprintf(stderr,
                        "validator: bad type idx %" PRIu64
                        " at %zu\n", code[i + 1], i);
                    return -1;
                }
                break;

            case OP_LOAD_INDIRECT:
            case OP_STORE_INDIRECT:
                if (code[i + 2] >= MEM_COUNT) {
                    fprintf(stderr,
                        "validator: bad mem_kind %" PRIu64
                        " at %zu\n", code[i + 2], i);
                    return -1;
                }
                break;
            case OP_LOAD_ADDR_INDIRECT:
                // No extra invariants; the offset is opaque.
                break;
                
            case OP_BINARY:
                // Sub-op and descriptor must be valid.
                if (code[i + 1] >= OPB_COUNT) {
                    fprintf(stderr,
                        "validator: bad binary sub-op %" PRIu64
                        " at %zu\n", code[i + 1], i);
                    return -1;
                }
                if (!typetable_get(types, (U32)code[i + 2])) {
                    fprintf(stderr,
                        "validator: bad binary type idx %" PRIu64
                        " at %zu\n", code[i + 2], i);
                    return -1;
                }
                break;
            case OP_UNARY:
                // Same shape as BINARY.
                if (code[i + 1] >= OPU_COUNT) {
                    fprintf(stderr,
                        "validator: bad unary sub-op %" PRIu64
                        " at %zu\n", code[i + 1], i);
                    return -1;
                }
                if (!typetable_get(types, (U32)code[i + 2])) {
                    fprintf(stderr,
                        "validator: bad unary type idx %" PRIu64
                        " at %zu\n", code[i + 2], i);
                    return -1;
                }
                break;
            case OP_ALLOC:
            case OP_ALLOC_RC:
                // Size-0 allocations are rejected by the VM
                // (it substitutes 8), so the validator
                // treats 0 as malformed. In practice
                // codegen never emits a 0 size.
                if (code[i + 1] == 0) {
                    fprintf(stderr,
                        "validator: %s at %zu has size 0\n",
                        disasm_op_name(op), i);
                    return -1;
                }
                break;

            case OP_RC_DEC:
                if (!typetable_get(types, (U32)code[i + 1])) {
                    fprintf(stderr,
                        "validator: bad rc_dec desc %" PRIu64 " at %zu\n",
                        code[i + 1], i);
                    return -1;
                }
                break;
                
            case OP_CONVERT:
                // Both source and target descriptors must be
                // valid.
                if (!typetable_get(types, (U32)code[i + 1]) ||
                    !typetable_get(types, (U32)code[i + 2])) {
                    fprintf(stderr,
                        "validator: bad convert descriptors at %zu\n", i);
                    return -1;
                }
                break;
            case OP_TRAP:{
                // Trap message's pool index must be valid.
                if (!pool || code[i + 1] >= (U64)pool->count){
                    fprintf(stderr,
                        "validator: TRAP with bad pool idx %" PRIu64
                        " at %zu\n", code[i + 1], i);
                    return -1;
                }
                break;
                }

            case OP_TRY_JUMP_IF_OK:
                // Target must be inside the code (same rule
                // as JMP).
                if (code[i + 1] >= length) {
                    fprintf(stderr,
                        "validator: TRY_JUMP_IF_OK at %zu targets %" PRIu64
                        " but code is %zu words\n", i, code[i + 1], length);
                    return -1;
                }
                break;
                
                
            default:
                // Opcodes with no extra invariants beyond
                // the range and truncation checks above.
                break;
        }
        i += 1 + (size_t)n;
    }
    return 0;
}