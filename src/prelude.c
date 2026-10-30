// ============================================================
// prelude.c — The prelude source and its parser.
//
// The prelude is written in Prism itself. That is deliberate:
// it exercises the same lexer, parser, and sema that user
// code does, so any change to those passes is validated
// against the prelude on every compile. It also keeps the
// definition of Result/Option in one place instead of
// duplicating their structure in sema.
//
// The two types declared here are the only generic types the
// language currently ships. They are marked is_builtin at
// the AST level, which register_struct_or_enum in sema.c
// turns into is_rc_managed = true on the TypeDef. That is
// what makes `rc<Result<...>>` and `rc<Option<...>>` the
// canonical forms of those types throughout the compiler.
// ============================================================
#include "../include/prelude.h"
#include "../include/lexer.h"
#include "../include/parser.h"

// The prelude source. Kept as a single string so it can be
// added to the SourceMap in one call. Each declaration is on
// its own line, ending with a newline (the parser's ASI
// relies on the newline to terminate the declaration; the
// final newline is not strictly required but matches the
// usual source-file convention).
//
// `Result<T, E>` has two variants: Ok(T) and Err(E).
// `Option<T>` has two variants: Some(T) and None.
//
// The order of the variants is load-bearing: variant tags
// are assigned in declaration order (see type_declaration
// in parser.c), so Ok/Some have tag 0 and Err/None have
// tag 1. Codegen and the runtime `?` operator both rely on
// these tags (see the `tag == 0` check in OP_TRY_JUMP_IF_OK
// in vm.c).
static const char* const PRELUDE_SOURCE =
    "type Result<T, E> = enum { Ok(T), Err(E) }\n"
    "type Option<T>    = enum { Some(T), None }\n";

// Synthetic path for the prelude's SourceMap entry. The
// angle brackets are a convention for "not a real file
// on disk"; nothing in the compiler tries to open it.
static const char* const PRELUDE_PATH = "<prelude>";

// Parse the prelude into a fresh AST_PROGRAM. Steps:
//   1. Add the source to the SourceMap under the synthetic
//      path. This returns a file_id that the lexer will
//      stamp into every span it produces for this source.
//   2. Lex and parse the source into an AST.
//   3. Walk the top-level declarations and set is_builtin
//      on each. Sema reads this flag (via register_struct_or_enum)
//      to set is_rc_managed on the resulting TypeDef.
//
// The caller (main.c, via cmd_run) takes ownership of the
// returned program and prepends its decls to the user
// program. The prelude's SourceMap entry remains in the map
// for the life of the compilation.
AstNode* prelude_parse(SourceMap* smap, DiagnosticBag* bag) {
    uint32_t fid = (uint32_t)smap_add(smap, PRELUDE_PATH, PRELUDE_SOURCE);
    Lexer lex;
    lexer_init(&lex, PRELUDE_SOURCE, fid);
    AstNode* prog = parse_program(&lex, smap, bag);
    for (int i = 0; i < prog->as.program.decls.count; i++)
        prog->as.program.decls.items[i]->is_builtin = true;
    return prog;
}