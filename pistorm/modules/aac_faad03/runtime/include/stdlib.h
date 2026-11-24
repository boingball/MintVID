#ifndef BERRY_STDLIB_H
#define BERRY_STDLIB_H
#include <stddef.h>
void *malloc(size_t n);
void free(void *p);
void *calloc(size_t n, size_t s);
void *realloc(void *p, size_t n);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
#ifndef abs
static inline int abs(int x) { return x < 0 ? -x : x; }
#endif
#endif
