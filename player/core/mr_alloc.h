/*
 * MintVID - Amiga-safe allocation for shared networking/source objects.
 *
 * HTTP documents are returned to callers in the GUI programs as well as the
 * player. Keep their ownership on Exec's AllocVec/FreeVec heap instead of
 * exposing libnix allocator assumptions across program modules. On host builds
 * these wrappers map directly to malloc/calloc/free.
 */
#ifndef MR_ALLOC_H
#define MR_ALLOC_H

#include <stddef.h>

#if (defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)) || \
    defined(__amigaos__) || defined(__AMIGA__)

#include <exec/memory.h>
#include <proto/exec.h>

static inline void *mr_alloc(size_t n)  { return AllocVec(n, MEMF_ANY); }
static inline void *mr_allocz(size_t n) { return AllocVec(n, MEMF_ANY | MEMF_CLEAR); }
/* Large, explicitly optional media caches should not consume Chip RAM. */
static inline void *mr_alloc_fast(size_t n) { return AllocVec(n, MEMF_FAST); }
static inline void  mr_free(void *p)    { if (p) FreeVec(p); }

#else

#include <stdlib.h>

static inline void *mr_alloc(size_t n)  { return malloc(n); }
static inline void *mr_allocz(size_t n) { return calloc(1, n); }
static inline void *mr_alloc_fast(size_t n) { return malloc(n); }
static inline void  mr_free(void *p)    { free(p); }

#endif

#endif /* MR_ALLOC_H */
