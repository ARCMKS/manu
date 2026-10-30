// ============================================================
// resolve.h — Centralized global name resolution.
//
// The resolver answers one question: given a name (or a
// qualified name) and the file it was written in, which
// declaration does it refer to?
//
// It does NOT resolve local variables. Locals are the
// function frame's business; sema tracks them via FnLocal.
// The resolver's job is everything declared at the top level
// of some file: functions, types, templates, constants, and
// module aliases.
//
// The result of a lookup is a ResolvedDecl: a small tagged
// struct with a kind and an index into the relevant table.
// Sema uses the index directly; it never re-scans.
//
// The resolver does not report diagnostics. It returns
// RD_NONE for anything it cannot resolve; the caller decides
// whether that is an error. This keeps the resolver free of
// message-shaping policy, which belongs to sema.
// ============================================================
#ifndef PRISM_RESOLVE_H
#define PRISM_RESOLVE_H

# include "common.h"
#include "lexer.h"
#include "sema.h"
#include "types.h"
#include "source_map.h"
#include <stdint.h>

// Forward declaration: the resolver takes the whole SemaCtx
// as context, but does not expose its internals.
//struct SemaCtx;
//typedef struct SemaCtx SemaCtx ;

// What a resolved declaration is. The `index` field is
// interpreted according to `kind`:
//   RD_FUNC      index into sc->funcs
//   RD_TYPE      index into sc->structs->items
//   RD_TEMPLATE  index into the monomorphizer's TemplateList
//   RD_CONST     index into the consts table
//   RD_MODULE    the target file_id (stored in `index`)
//   RD_BUILTIN   index into the builtin table (unused for now)
typedef enum{
    RD_NONE = 0,
    RD_FUNC,
    RD_TYPE,
    RD_TEMPLATE,
    RD_CONST,
    RD_MODULE,
    RD_BUILTIN_TYPE,
} ResolveKind;

typedef struct{
    ResolveKind kind;
    int         index;      // valid for func/type/template/const/module
    TypeDef*    builtin;    // valid for builtin_type, null otherwise
} ResolvedDecl;

// A ResolvedDecl meaning "nothing found". Equivalent to
// (ResolvedDecl){ RD_NONE, -1 }.
static inline ResolvedDecl rd_none(void){
    ResolvedDecl r = {RD_NONE, -1, NULL};
    return r;
}

// Resolve a bare name in the scope of `file_id`. The scope
// is: declarations made in this file, then selective imports
// (`using a.b.{ x }`), then nothing. Builtins are handled by
// sema before this call; the resolver does not consider them.
//
// Returns RD_NONE if the name is not a global declaration in
// this file's scope. That is not an error: the name might be
// a local, which is sema's concern.
ResolvedDecl resolve_name(const SemaCtx* sc, uint32_t file_id, Token name);

// Resolve `alias.name` where `alias` is written in `file_id`.
// `alias` must be a module alias (from `using X as alias` or
// `using X` where the alias is derived); the lookup fails if
// it is not.
//
// On success, returns the resolved declaration of `name` in
// the target module. The `RD_MODULE` result is returned only
// if `name` itself resolves to a nested module alias, which
// Prism does not currently support; treat it as "not found"
// for now.
ResolvedDecl resolve_qualified(const SemaCtx* sc, uint32_t file_id,
                               Token alias, Token name);

// Convenience: resolve a bare name as a template. This is a
// subset of resolve_name that only returns RD_TEMPLATE, but
// it is worth having because the monomorphizer's template
// list is not part of the general name scope and callers
// that want a template benefit from the narrower contract.
ResolvedDecl resolve_template(const SemaCtx* sc, uint32_t file_id, Token name);

// Resolve a name in a specific file, regardless of the
// caller's scope. Used by import validation, where the
// caller has already identified which file to look in and
// wants to know whether the named item exists there.
//
// Unlike `resolve_qualified`, this does not consult the
// caller's import environment; it goes straight to the
// target file's top-level declarations.
ResolvedDecl resolve_in_module(const SemaCtx* sc, uint32_t file_id,
                               Token name);

// Get the TypeDef* from a ResolvedDecl whose kind is RD_TYPE
// or RD_BUILTIN_TYPE. Returns NULL for any other kind.
// Callers that want a TypeDef use this instead of switching
// on the kind themselves.
TypeDef* resolved_tdef(const SemaCtx* sc, ResolvedDecl rd);

// Module-table lookups. A FileNamespace holds one file's
// imports (module aliases) and selective imports. These
// accessors were previously private to sema; the resolver
// needs them, so they are exposed here.
FileNamespace* modtable_find(const ModuleTable* mt, uint32_t file_id);
int fnamespace_resolve(const FileNamespace* fn,
                       const SourceMap* smap, Token name);
const SelectiveImport* fnamespace_find_selective(
    const FileNamespace* fn, const SourceMap* smap, Token name);

#endif 