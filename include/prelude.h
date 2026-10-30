// ============================================================
// prelude.h — The compiler's built-in prelude.
//
// The prelude is a small source fragment that is compiled
// alongside every user program. It defines the language's
// built-in generic types (currently Result and Option) in
// terms of the language itself, rather than special-casing
// them inside sema or codegen.
//
// prelude_parse is called once per compilation, before sema,
// and its declarations are prepended to the user's top-level
// declarations. Because the prelude's types are registered
// with is_builtin = true, codegen knows to treat them as
// RC-managed (see the is_rc_managed flag on TypeDef).
//
// The prelude is re-parsed on every compilation. That is
// cheap (the source is tiny) and avoids a shared-state
// lifetime question between consecutive compilations in the
// same process.
// ============================================================
#ifndef PRISM_PRELUDE_H
#define PRISM_PRELUDE_H

#include "ast.h"
#include "source_map.h"
#include "diagnostics.h"

// Parses the compiler prelude into a fresh AST_PROGRAM, adds its
// source to `smap` (once), and marks every top-level declaration
// as `is_builtin`. The caller owns the returned AST and is
// expected to prepend its declarations to the user program.
//
// Note: prelude_parse appends the prelude source to the
// SourceMap under the synthetic path "<prelude>". Calling it
// twice on the same SourceMap adds two entries with the same
// path but different file ids; the parser does not dedupe.
// The current call site in main.c calls it once.
//
// Failure mode: if the prelude source somehow fails to parse
// (which would be a compiler bug, since it is hard-coded),
// the function returns the partially-parsed program and the
// bag has errors in it. The caller is expected to check the
// bag as it would for any other parse.
AstNode* prelude_parse(SourceMap* smap, DiagnosticBag* bag);

#endif