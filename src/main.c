// ============================================================
// main.c — Command-line driver.
//
// Commands:
//   manu run   <file>       compile and run a source file
//   manu check <file>       compile only, report diagnostics
//   manu test  [filter]     run the test suite
//   manu --help             print usage
//
// A bare file path is treated as `run <file>`.
//
// Flags:
//   --trace, -t    enable VM execution trace on stderr
//   --quiet, -q    suppress informational output
//
// Exit codes:
//   0   success
//   1   I/O error, compile error, or failed test run
//   2   runtime error
//   3   bytecode validation failure
//   64  usage error
//
// The command pipeline is uniform across `run` and `check`:
//
//   read file
//     → lex → parse → sema → codegen → Chunk
//       → (run only) validate → vm_init → vm_run
//
// Sema and codegen share a single ConstTable, created here and
// discarded when the command finishes. Diagnostics accumulate in
// a DiagnosticBag and are rendered exactly once, at the end,
// with source excerpts.
//
// Reading order:
//   1. CLI parsing (Command enum, Cli struct, print_usage,
//      parse_args).
//   2. File I/O (read_file).
//   3. The Compilation struct and its lifecycle — used only
//      by cmd_check.
//   4. Runtime error rendering (source_line_bounds,
//      render_runtime_error).
//   5. Commands (cmd_run, cmd_check).
//   6. main().
//
// Note that cmd_run and cmd_check use different structures
// for the same pipeline: cmd_check uses Compilation (which
// owns its SourceMap, ConstTable, etc.), and cmd_run uses a
// ModuleLoader (which owns the SourceMap) plus local
// ConstTable / TypeDefTable / FFITable / SemaCtx / Chunk
// declarations. The two paths are similar but not shared;
// the module loader's multi-file support is why cmd_run
// cannot use Compilation unchanged. This is a known wart.
// ============================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/source_map.h"
#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/sema.h"
#include "../include/codegen.h"
#include "../include/chunk.h"
#include "../include/constant.h"
#include "../include/vm.h"
#include "../include/disasm.h"
#include "../include/diagnostics.h"
#include "../include/common.h"
#include "../include/module.h"
#include "../include/prism_ffi.h"
#include "../include/prelude.h"

// Hard cap on the number of -I include paths. Exceeding it
// is reported but the extra paths are silently dropped
// (see parse_args).
#define MAX_INCLUDES 16
#define MAX_PROG_ARGS 64

// ============================================================
// Argument parsing
// ============================================================

// The three commands. CMD_TEST is commented out at every
// site (it existed once, the test runner was disabled, and
// the comments remain). CMD_HELP is an internal marker set
// by --help / -h and handled by main().
typedef enum {
    CMD_RUN,
    CMD_CHECK,
    CMD_HELP,
} Command;



// Parsed CLI state. All fields are populated by parse_args.
//
// Field notes:
//   cmd         — the selected command. Defaults to CMD_RUN
//                 (a bare path is `run`).
//   file        — the source file path. NULL if no path was
//                 given (which is a usage error for run and
//                 check, but not for help).
//   test_filter — a substring for filtering tests. Currently
//                 never set (CMD_TEST is disabled).
//   trace       — enable the VM's per-instruction trace to
//                 stderr.
//   quiet       — suppress informational output (disasm,
//                 return-value banner). Errors still go to
//                 stderr.
//   include_paths / include_count — -I directories, in the
//                 order they appeared on the command line.
//                 Passed to modload_add_include by cmd_run.
typedef struct {
    Command     cmd;
    const char* file;
    const char* test_filter;
    bool        trace;
    bool        quiet;
    const char* include_paths[MAX_INCLUDES];
    int         include_count;
    const char* prog_argv[MAX_PROG_ARGS];
    int         prog_argc;
} Cli;


// Print the usage message. Uses argv[0] so the message
// reflects however the user invoked the binary (e.g.
// "./manu" vs "manu" vs an absolute path). Never returns.
//
// The format is a single printf with the argv[0] substituted
// at four places; the strung-together format string is a
// historical layout choice.
static void print_usage(const char* argv0) {
    printf(
"Usage:\n"
"  %s [flags] run   <file>      compile and run a source file\n"
"  %s [flags] check <file>      compile only, report diagnostics\n"
"  %s [flags] test  [filter]    run the test suite\n"
"  %s --help                    print this help\n"
"\n"
"Flags:\n"
"  --trace, -t    enable VM execution trace on stderr\n"
"  --quiet, -q    suppress informational output\n"
"\n"
"A bare file path is treated as `run <file>`.\n",
        argv0, argv0, argv0, argv0);
}

// Parses arguments into `cli`. Returns 0 on success, non-zero on
// usage error. On failure, the caller prints usage and exits 64.
//
// The parser is a single left-to-right walk. Two passes over
// the argument list are not needed because flags and command
// are position-independent: flags before, between, or after
// the command are all accepted.
//
// Field defaulting: CMD_RUN is the default, no file, no
// flags. `have_command` tracks whether the command (or a
// bare path, which implies `run`) has been seen; the first
// non-flag argument is taken as the command.
//
// Special case for a bare file path: if the first non-flag
// argument is neither `run`, `check`, `help`, nor a flag,
// it is treated as the file argument to an implicit `run`.
//
// Flag handling for `-I`: the next argument is consumed as
// the include directory. The `i + 1 < argc` check catches a
// trailing `-I` with no value; on the failure path the flag
// is silently ignored (the directory is not added), but the
// loop still increments i, so the flag is effectively
// dropped. A more user-friendly version would report an
// error; not done today.
//
// The `a[0] == '-' && a[1] != '\0'` check treats a lone
// `-` as a positional argument (which is unconventional;
// most CLIs treat a lone `-` as stdin). Not a practical
// concern here.
//
// want_help is a deferred flag: if any --help / -h appears
// anywhere, the command is overridden to CMD_HELP after
// the loop. That means `manu run foo --help` prints help
// rather than running foo, which is the usual convention.
static int parse_args(int argc, char** argv, Cli* cli) {
    memset(cli, 0 ,sizeof *cli);

    bool have_command = false;
    bool want_help    = false;
 

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];

        // ----- Flags -----
        // Flags are checked before the command dispatch, so a
        // flag can appear before, between, or after the
        // command and its arguments.
        if (strcmp(a, "--trace") == 0 || strcmp(a, "-t") == 0) {
            cli->trace = true;
            continue;
        }
        if (strcmp(a, "--quiet") == 0 || strcmp(a, "-q") == 0) {
            cli->quiet = true;
            continue;
        }
        // -I <dir> consumes the next argument. Overflow of
        // the include array is reported but the path is
        // dropped.
        if (strcmp(a, "-I") == 0 && i + 1 < argc) {
            if (cli->include_count < MAX_INCLUDES)
                cli->include_paths[cli->include_count++] = argv[++i];
            else
                fprintf(stderr, "too many -I flags (max %d)\n", MAX_INCLUDES);
            continue;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            want_help = true;
            continue;
        }
        // Any other flag-looking argument is an error.
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "unknown flag: %s\n", a);
            return 1;
        }

        // ----- Command (first non-flag) -----
        //
        // The first non-flag argument is the command. If it
        // is not one of the known commands, it is treated
        // as a file path for an implicit `run`.
        if (!have_command) {
            if      (strcmp(a, "run")   == 0) cli->cmd = CMD_RUN;
            else if (strcmp(a, "check") == 0) cli->cmd = CMD_CHECK;
            //else if (strcmp(a, "test")  == 0) cli->cmd = CMD_TEST;
            else if (strcmp(a, "help")  == 0) { want_help = true; }
            else {
                // Bare file path → implicit `run`.
                cli->cmd  = CMD_RUN;
                cli->file = a;
                have_command = true;
                continue;
            }
            have_command = true;
            continue;
        }

        // ----- Positional arguments after the command -----
        //
        // For run/check, the first positional is the file;
        // a second positional is a "extra argument" error.
        // For help, positionals are ignored.
        switch (cli->cmd) {
            case CMD_RUN:
            case CMD_CHECK:
                if (!cli->file) {
                    cli->file = a;
                } else {
                    // Program arguments: everything after the
                    // file path. Overflow is silently truncated;
                    // a warning would be noise for a CLI limit
                    // this generous.
                    if (cli->prog_argc < MAX_PROG_ARGS)
                        cli->prog_argv[cli->prog_argc++] = a;
                }
                break;
            case CMD_HELP:
                break;
        }
    }

    // Deferred --help overrides the command. This is the
    // "help wins" rule: any --help anywhere means help.
    if (want_help) cli->cmd = CMD_HELP;
    return 0;
}

// ============================================================
// Multi-file compilation
//
// Both `run` and `check` share the same pipeline:
//
//   load root + transitive imports
//     -> merge into one AST_PROGRAM
//     -> prepend the prelude
//     -> sema
//     -> codegen
//
// The only difference is what happens after codegen: `run`
// validates the chunk and executes it, `check` reports
// success. Putting the pipeline in one place means both
// commands see the same prelude, resolve `using`
// declarations the same way, and render diagnostics the
// same way.
//
// One CompileUnit bundles every artifact the pipeline
// produces, so a single cleanup function frees them all.
// Ownership:
//   ml      owns the SourceMap and the loaded files' ASTs
//   program owns the merged AST (taken from ml's files)
//   bag     owns only its internal array
//   consts  is by-value; has no owned heap beyond MAX_CONSTS
//           entries (bounded leak by design)
//   structs owns the TypeDefTable plus the monomorphizer
//           handle installed by sema
//   ffi     is by-value; has no owned heap
//   sc      is by-value; owns sc->modtable, freed by sema
//   chunk   owns its code/lines/cols arrays, pool, funcs,
//           and types tables
// ============================================================

// Helper: directory of a file path, without the trailing slash.
// Writes into `out`; returns true if a non-empty directory was
// extracted. If the path has no slash, returns an empty string
// and false.
//
// Both `/` and `\\` are treated as separators. Used by
// compile_multi to seed the module loader's include paths with
// the root file's directory, so a `using` in the root resolves
// relative to the root file first.
static bool dir_of(const char* path, char* out, size_t cap) {
    const char* last = strrchr(path, '/');
    const char* bs   = strrchr(path, '\\');
    if (bs && (!last || bs > last)) last = bs;
    if (!last) { out[0] = '\0'; return false; }
    size_t n = (size_t)(last - path);
    if (n + 1 >= cap) n = cap - 2;
    memcpy(out, path, n);
    out[n] = '\0';
    return true;
}

typedef struct {
    ModuleLoader  ml;      // owns the SourceMap and the file ASTs
    DiagnosticBag bag;     // diagnostics from load / sema / codegen
    ConstTable    consts;  // filled by sema
    TypeDefTable  structs; // filled by sema (plus monomorphizer handle)
    FFITable      ffi;     // filled by sema
    SemaCtx       sc;      // filled by sema
    AstNode*      program; // merged AST (decls taken from ml's files)
    Chunk         chunk;   // filled by codegen
} CompileUnit;

// Zero every field. Each sub-init is its own function, so the
// growth policies and initial invariants stay in the modules
// that own them.
static void cu_init(CompileUnit* cu) {
    modload_init(&cu->ml);
    diag_init(&cu->bag);
    consts_init(&cu->consts);
    typedef_table_init(&cu->structs);
    ffitable_init(&cu->ffi);
    memset(&cu->sc, 0, sizeof cu->sc);
    cu->program = NULL;
    chunk_init(&cu->chunk);
}

// Free every owned sub-structure. Order matters only in that
// the chunk references the funcs/structs tables (indices into
// them, not pointers), so it can be freed first; the rest are
// independent.
//
// Note: `consts` has no free function. Its entries can own
// string bytes if a future extension adds string constants;
// today they cannot, so this is bounded.
static void cu_free(CompileUnit* cu) {
    chunk_free(&cu->chunk);
    ast_free(cu->program);
    diag_free(&cu->bag);
    typedef_table_free(&cu->structs);
    modload_free(&cu->ml);
}

// The shared pipeline. Returns true iff no errors were
// reported. On success, `cu->chunk` holds the compiled
// program; on failure, the bag has the diagnostics and the
// caller should render them with `diag_flush(&cu->bag,
// &cu->ml.smap)`.
//
// The CLI's include paths, the root file's directory, and the
// current directory are all seeded into the loader in that
// order. `cli->file` is required to be non-NULL; both command
// handlers check for it before calling.
static bool compile_multi(const Cli* cli, CompileUnit* cu) {
    // Include-path setup. Root directory first (so `using`
    // in the root resolves relative to the root), then the
    // current directory (a fallback), then any CLI -I paths
    // in the order given.
    char root_dir[1024];
    if (dir_of(cli->file, root_dir, sizeof root_dir))
        modload_add_include(&cu->ml, root_dir);
    modload_add_include(&cu->ml, ".");
    for (int i = 0; i < cli->include_count; i++)
        modload_add_include(&cu->ml, cli->include_paths[i]);

    // Load the root and its transitive imports. Load
    // diagnostics go into cu->bag, which the caller will
    // render alongside any later sema/codegen diagnostics.
    if (!modload_load(&cu->ml, cli->file, &cu->bag))
        return false;

    // Merge into a single program. Ownership of the decls
    // moves from the loader's per-file ASTs to the merged
    // program; the file_id on each decl's span is preserved,
    // so the resolver can still tell which file a decl came
    // from.
    cu->program = modload_merge(&cu->ml);

    // Prepend the prelude. This is what makes Result and
    // Option available without an explicit `using`.
    {
        AstNode* prelude = prelude_parse(&cu->ml.smap, &cu->bag);
        ast_list_prepend_all(&cu->program->as.program.decls,
                             &prelude->as.program.decls);
        ast_free(prelude);
    }

    // Sema and codegen, each gated on the previous stage
    // succeeding. Running sema on a partially-parsed AST
    // would produce cascading errors; running codegen on a
    // partially-analyzed AST could dereference unresolved
    // types.
    if (!diag_has_errors(&cu->bag))
        sema_analyze(cu->program, &cu->ml.smap, &cu->consts,
                     &cu->structs, &cu->bag, &cu->ffi, &cu->sc);
    if (!diag_has_errors(&cu->bag))
        codegen_program(cu->program, &cu->ml.smap, &cu->chunk,
                        &cu->bag, &cu->consts, &cu->structs,
                        &cu->ffi, &cu->sc);

    return !diag_has_errors(&cu->bag);
}

// ============================================================
// Runtime error rendering
//
// The VM reports a pc and a func index. Given those, and
// optionally a source text plus a line table, this renders:
//
//   - the error message
//   - a source excerpt with a caret, if the source is available
//   - a call trace from deepest to shallowest
//
// If the source text is not available (AOT execution without
// debug info), steps 2 and 3 degrade gracefully: the excerpt is
// skipped and the trace shows only pc offsets and function names.
// ============================================================

// Given a source string and a 1-based line number, find the
// byte range of that line. Returns false if the line doesn't
// exist.
//
// The walk counts newlines until the target line is
// reached. `start` points at the first byte of the line
// (which is the byte after the previous newline, or the
// first byte of the file for line 1); `end` points at the
// next newline or the file's end.
//
// Note that this walks from the beginning of the source on
// every call, so rendering N errors is O(N * |source|).
// That is fine for a handful of runtime errors but would
// be slow for a diagnostic-heavy compile; the diagnostic
// path uses SourceMap's precomputed line_starts table
// instead (see smap_decode). This function is only used
// by the runtime error renderer.
static bool source_line_bounds(const char* source, uint32_t line,
                               const char** start, const char** end) {
    if (!source || line == 0) return false;
    const char* p = source;
    uint32_t cur = 1;
    while (cur < line && *p) {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != line) return false;

    *start = p;
    while (*p && *p != '\n') p++;
    *end = p;
    return true;
}

// Render a runtime error with source context and a call
// trace.
//
// Inputs:
//   vm         — the VM whose error/error_pc/error_func_idx
//                /call_stack are the error state.
//   chunk      — the chunk being executed, used for the
//                funcs table (call-trace names).
//   source     — the file's content, or NULL if unavailable.
//   file_path  — the file's path, or NULL.
//   smap       — the SourceMap, used to look up the func
//                names.
//
// Output: everything goes to stderr. The format is similar
// to diag_flush's, with a `-->` line and a caret excerpt.
//
// The call trace is printed from deepest to shallowest. For
// each frame, the "current ip" is the error pc for the top
// frame and the frame's call_pc (the pc at which the
// enclosing frame called this one) for every other frame.
// This is what makes the trace read as "at foo called from
// bar called from baz".
//
// Function names are resolved via the SourceMap from the
// chunk's funcs table. If the func table is empty or the
// func_idx is out of range, the name defaults to
// "<unknown>".
static void render_runtime_error(const VM* vm, const Chunk* chunk,
                                 const char* source, const char* file_path,
                                 const SourceMap* smap) {
    fprintf(stderr, "runtime error: %s\n", vm->error);

    // ---- Source excerpt ----
    //
    // Extract the line and column from the VM's debug
    // arrays. The column defaults to 1 if unset.
    uint32_t line = 0, col = 0;
    if (vm->lines && vm->error_pc < vm->bytecode_len) {
        line = vm->lines[vm->error_pc];
        if (vm->cols) col = vm->cols[vm->error_pc];
        if (col == 0) col = 1;
    }

    if (line > 0) {
        const char* ls = NULL;
        const char* le = NULL;
        if (source_line_bounds(source, line, &ls, &le)) {
            fprintf(stderr, "  --> %s:%u:%u\n",
                    file_path ? file_path : "<source>", line, col);
            fprintf(stderr, "   |\n");
            fprintf(stderr, "%2u | %.*s\n",
                    line, (int)(le - ls), ls);
            fprintf(stderr, "   | ");
            for (uint32_t c = 1; c < col; c++) fputc(' ', stderr);
            fputc('^', stderr);
            fputc('\n', stderr);
        }
    }

    // ---- Trace ----
    //
    // No frames means the error happened before any call
    // (or the VM's call_stack was somehow empty). In either
    // case there is no trace to print.
    if (vm->call_depth == 0) return;

    fprintf(stderr, "\ntrace (most recent call first):\n");

    int top = (int)vm->call_depth - 1;
    for (int i = top; i >= 0; i--) {
        const CallFrame* f = &vm->call_stack[i];

        // Which PC to display for this frame.
        //
        // For the top frame, the error_pc (where the VM
        // was when the error fired). For any other frame,
        // the call_pc of the frame one level deeper —
        // i.e. the pc at which this frame's body called
        // the next frame.
        //
        // The i + 1 index is safe: the loop runs down to
        // i == 0, and for i < top the frame at i+1 exists.
        // For i == top the branch takes the first arm.
        U64 ip = (i == top) ? vm->error_pc : vm->call_stack[i + 1].call_pc;

        // Function name. Look up the sema name via the
        // chunk's funcs table (which codegen populated
        // with Token names). If anything is off (empty
        // table, out-of-range index, missing smap), the
        // name falls back to "<unknown>".
        char namebuf[128] = "<unknown>";
        if (chunk->funcs.count > 0
            && f->func_idx >= 0
            && f->func_idx < chunk->funcs.count) {
            const FuncEntry* fe = &chunk->funcs.items[f->func_idx];
            if (fe->name_buf[0]) {
                snprintf(namebuf, sizeof namebuf, "%s", fe->name_buf);
            } else if (smap) {
                smap_span_text(smap, fe->name.span,
                               namebuf, sizeof namebuf);
            }
        }

        // Offset within function: ip - function's entry_pc.
        // The entry_pc is what codegen recorded when it
        // emitted the function's body.
        U64 func_entry = 0;
        if (f->func_idx >= 0 && f->func_idx < chunk->funcs.count)
            func_entry = chunk->funcs.items[f->func_idx].entry_pc;
        U64 off = ip - func_entry;

        // Line for this ip, if the debug arrays are
        // available.
        uint32_t ip_line = 0;
        if (vm->lines && ip < vm->bytecode_len)
            ip_line = vm->lines[ip];

        // Print with or without the line, depending on
        // whether debug info is available.
        if (ip_line > 0)
            fprintf(stderr, "  at %-16s (%s:%u, ip = 0x%04llx)\n",
                    namebuf, file_path ? file_path : "<source>",
                    ip_line, (unsigned long long)off);
        else
            fprintf(stderr, "  at %-16s (ip = 0x%04llx)\n",
                    namebuf, (unsigned long long)off);
    }
}

// ============================================================
// Commands
// ============================================================

// `manu run <file>`: compile, validate, run.
//
// The pipeline:
//   1. Set up a ModuleLoader with the root's directory and
//      the CLI's -I paths as include paths.
//   2. Load the root file and its transitive imports. The
//      loader is responsible for lexing and parsing every
//      file; failures here abort before any sema work.
//   3. Merge all loaded files into one AST_PROGRAM.
//   4. Parse the prelude and prepend its decls to the
//      merged program.
//   5. Run sema. It fills ConstTable, TypeDefTable, and
//      FFITable, and populates the SemaCtx's funcs table.
//   6. Run codegen into a fresh Chunk.
//   7. Render any diagnostics. If there were any errors,
//      abort before running.
//   8. Disassemble (unless quiet).
//   9. Validate the bytecode structurally.
//  10. Set up a VMConfig and a VM, run.
//  11. If the VM returns non-zero, render the runtime error
//      (with source context and a call trace).
//
// Cleanup: every resource is freed before returning, on
// every return path. The pattern is to free at each exit
// rather than using goto; the function is long but the
// resource set is stable.
//
// The `modload_load` failure path frees the loader and
// returns 1. The `diag_has_errors` after sema/codegen
// path frees everything and returns 1. The validation
// failure path frees everything and returns 3. The runtime
// error path frees everything and returns 2.
//
// A note on the prelude: it is parsed after the user
// program is merged, and its decls are prepended. The
// prelude's SourceMap entry is added to the same map, so
// the prelude's tokens have file_ids that sema can resolve.
// `manu run <file>`: compile, validate, run.
//
// The compile pipeline is in compile_multi. This handler
// adds the post-compile steps that are specific to
// execution: disassembly (optional), structural
// validation, VM setup, and — on a runtime error —
// source-context rendering.
static int cmd_run(const Cli* cli) {
    if (!cli->file) {
        fprintf(stderr, "run: missing <file>\n");
        return 64;
    }

    CompileUnit cu;
    cu_init(&cu);

    if (!compile_multi(cli, &cu)) {
        if (cu.bag.count > 0) diag_flush(&cu.bag, &cu.ml.smap);
        fprintf(stderr, "\n%d error(s); aborting.\n", cu.bag.error_count);
        cu_free(&cu);
        return 1;
    }

    // Disassembly (debug output; --quiet suppresses it).
    if (!cli->quiet)
        disassemble(&cu.chunk, &cu.ml.smap, cli->file, cli->file);

    // Structural validation. A failure here means a codegen
    // bug (or a corrupted chunk); exit code 3 distinguishes
    // it from a source error (1) and a runtime error (2).
    if (validate_bytecode(cu.chunk.code, (size_t)cu.chunk.count,
                          &cu.chunk.pool, &cu.chunk.funcs,
                          &cu.chunk.types) != 0) {
        fprintf(stderr, "bytecode failed structural validation.\n");
        cu_free(&cu);
        return 3;
    }

    // Set up and run the VM. VMConfig and VM both borrow from
    // cu.chunk / cu.ffi, which stay alive until cu_free.
    VMConfig cfg = {
        .bytecode     = cu.chunk.code,
        .bytecode_len = (size_t)cu.chunk.count,
        .lines        = cu.chunk.lines,
        .cols         = cu.chunk.cols,
        .pool         = &cu.chunk.pool,
        .funcs        = &cu.chunk.funcs,
        .types        = &cu.chunk.types,
        .ffi          = &cu.ffi,
        .prog_argc    = cli->prog_argc,
        .prog_argv    = (const char**)cli->prog_argv,
    };
    VM vm; vm_init(&vm, &cfg);
    vm.trace = cli->trace;

    int rc = vm_run(&vm);
    if (rc != 0) {
        // Find the file the erroring function belongs to.
        // If the func table or the SourceMap does not have
        // the information, the renderer degrades to a
        // trace-only report.
        uint32_t    fid       = 0;
        const char* file_path = "<unknown>";
        const char* source    = NULL;

        if (vm.error_func_idx >= 0
            && vm.error_func_idx < cu.chunk.funcs.count) {
            fid = cu.chunk.funcs.items[vm.error_func_idx].name.span.file_id;
            SourceFile* sf = smap_get(&cu.ml.smap, fid);
            if (sf) {
                file_path = sf->path;
                source    = sf->content;
            }
        }

        render_runtime_error(&vm, &cu.chunk, source, file_path,
                             &cu.ml.smap);

        vm_free(&vm);
        cu_free(&cu);
        return 2;
    }

    if (!cli->quiet) {
        printf("return value = %lld\n", (long long)vm_top(&vm));
        printf("arena high-water = %lld bytes\n",
               (long long)(vm.ap - vm.arena));
    }

    vm_free(&vm);
    cu_free(&cu);
    return 0;
}

// `manu check <file>`: compile only, report diagnostics.
//
// Uses the same pipeline as cmd_run, so `check` resolves
// `using` declarations and loads the prelude exactly as
// `run` does. The only post-compile step is rendering the
// bag.
static int cmd_check(const Cli* cli) {
    if (!cli->file) {
        fprintf(stderr, "check: missing <file>\n");
        return 64;
    }

    CompileUnit cu;
    cu_init(&cu);

    bool ok = compile_multi(cli, &cu);

    if (cu.bag.count > 0)
        diag_flush(&cu.bag, &cu.ml.smap);
    if (ok && !cli->quiet)
        printf("ok\n");

    cu_free(&cu);
    return ok ? 0 : 1;
}

//static int cmd_test(const Cli* cli) {
//    return test_run_all(cli->test_filter, cli->quiet);
//}

// ============================================================
// Entry point
// ============================================================
int main(int argc, char** argv) {
    Cli cli;
    if (parse_args(argc, argv, &cli) != 0) {
        print_usage(argv[0]);
        return 64;
    }

    switch (cli.cmd) {
        case CMD_HELP:  print_usage(argv[0]); return 0;
        case CMD_RUN:   return cmd_run(&cli);
        case CMD_CHECK: return cmd_check(&cli);
    }
    return 0;
}