// ============================================================
// parser.h — Parser entry point.
//
// The parser reads a source string through the Lexer and
// produces a top-level program AST containing function and
// constant declarations. Statements and expressions appear only
// inside function bodies.
//
// On error, the parser reports into the supplied DiagnosticBag
// and returns a program AST anyway (possibly empty or partial).
// Callers check `diag_has_errors(bag)` before proceeding to
// subsequent stages.
//
// Design notes:
//
//   - The parser is single-pass recursive descent with a Pratt
//     precedence chain for binary expressions. There is no
//     separate token buffer: it reads one token ahead (cur)
//     and keeps the last consumed token (prev) for span end
//     computation.
//
//   - Error recovery is panic-mode: an error sets a flag and
//     the parser skips forward to a plausible synchronization
//     point. It never bails out of the whole parse, so a file
//     with many independent errors reports all of them.
//
//   - The public surface is one function. Everything else in
//     parser.c is static. The Parser state struct is not
//     exposed; callers cannot inspect or steer the parser.
//
// Ownership: the parser owns nothing. The returned AST owns
// itself; the Lexer, SourceMap, and DiagnosticBag are borrowed
// for the duration of the call and are not retained.
// ============================================================
#ifndef PRISM_PARSER_H
#define PRISM_PARSER_H

#include "ast.h"
#include "lexer.h"
#include "source_map.h"
#include "diagnostics.h"

// Parses a whole program. `smap` is used for reading identifier
// bytes when the parser needs to compare or report them (e.g.
// for keywords recognized at the parser level). `bag` receives
// every diagnostic the parser emits.
//
// Precondition: the Lexer has been initialized over `source`
// with a valid `file_id` already registered in `smap`.
//
// Returns AST_PROGRAM even in the presence of errors; the caller
// must check the bag.
//
// Note: the return value is *always* a valid AST_PROGRAM node,
// never NULL, even if the whole file was garbage. A caller that
// wants to skip subsequent stages on a failed parse must check
// `diag_has_errors(bag)` itself; the parse result alone is not
// a success/failure signal.
AstNode* parse_program(Lexer* lex, const SourceMap* smap,
                       DiagnosticBag* bag);

#endif // PRISM_PARSER_H