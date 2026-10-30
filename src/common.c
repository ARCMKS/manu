// ============================================================
// common.c — Allocation helpers. Abort on OOM (the compiler
// has no useful way to continue if we can't allocate).
//
// The wrappers below are the only place in the compiler where
// malloc-family calls are allowed; everything else calls through
// them. That lets us guarantee (a) a single OOM message, (b) a
// single exit code, and (c) that the "malloc(0)" corner case is
// handled uniformly.
// ============================================================
#include "../include/common.h"
#include <stdio.h>
#include <stdlib.h>

// Allocate n bytes. n == 0 is upgraded to 1 so we never call
// malloc(0), whose return value is implementation-defined (may be
// NULL, which would falsely trip the OOM branch, or a unique
// pointer that must be freed). Callers may treat the return as
// always non-NULL.
void* xmalloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "prism: out of memory\n"); exit(1); }
    return p;
}
// Allocate n elements of sz bytes each, zero-initialized. Same
// n/sz zero-upgrade as xmalloc, for the same reason: calloc(0, x)
// and calloc(x, 0) have the same implementation-defined behaviour
// as malloc(0).
void* xcalloc(size_t n, size_t sz) {
    void* p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) { fprintf(stderr, "prism: out of memory\n"); exit(1); }
    return p;
}
// Resize p to n bytes. Upgrading n == 0 to 1 avoids the same
// malloc(0) ambiguity; callers that want to release memory should
// call free() directly rather than xrealloc(p, 0).
void* xrealloc(void* p, size_t n) {
    void* q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "prism: out of memory\n"); exit(1); }
    return q;
}