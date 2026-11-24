#include "berry_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Kept in module .data; linker script makes zero-state bytes part of the binary. */
static uintptr_t g_heap_base = 1;
static size_t g_heap_size = 1;
static size_t g_heap_pos = 1;
static int g_heap_oom = 1;

typedef struct berry_FILE { int dummy; } FILE;
static FILE g_stderr_obj = {0};
FILE *stderr = 0;

void berry_heap_bind(void *base, size_t bytes) {
    g_heap_base = (uintptr_t)base;
    g_heap_size = bytes;
    g_heap_pos = 0;
    g_heap_oom = 0;
}
size_t berry_heap_used(void) { return g_heap_pos; }
size_t berry_heap_capacity(void) { return g_heap_size; }
int berry_heap_failed(void) { return g_heap_oom; }

void *malloc(size_t n) {
    if (!n) n = 1;
    size_t p = (g_heap_pos + 15u) & ~(size_t)15u;
    if (p > g_heap_size || n > g_heap_size - p) { g_heap_oom = 1; return 0; }
    void *r = (void *)(g_heap_base + p);
    g_heap_pos = p + n;
    return r;
}
void free(void *p) { (void)p; }
void *calloc(size_t n, size_t s) {
    if (n && s > (size_t)-1 / n) { g_heap_oom = 1; return 0; }
    size_t z = n*s; void *p = malloc(z); if (p) memset(p,0,z); return p;
}
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    /* FAAD2 LC path used here does not need realloc in the reference run.
       Fail closed instead of silently copying an unknown old allocation size. */
    (void)n; g_heap_oom = 1; return 0;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd=d; const unsigned char *ss=s; while(n--) *dd++=*ss++; return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dd=d; const unsigned char *ss=s;
    if (dd < ss) while(n--) *dd++=*ss++; else { dd += n; ss += n; while(n--) *--dd=*--ss; }
    return d;
}
void *memset(void *d, int c, size_t n) { unsigned char *p=d; while(n--) *p++=(unsigned char)c; return d; }
int memcmp(const void *a,const void *b,size_t n) { const unsigned char*x=a,*y=b; while(n--){ if(*x!=*y) return *x<*y?-1:1; x++;y++; } return 0; }
size_t strlen(const char *s) { const char*p=s; while(*p)p++; return (size_t)(p-s); }

/* Tiny insertion sort: sufficient for the small metadata arrays for which libfaad uses qsort. */
void qsort(void *base, size_t nmemb, size_t size, int (*cmp)(const void*,const void*)) {
    unsigned char *b=base; if (!size || nmemb<2) return;
    for(size_t i=1;i<nmemb;i++) {
        size_t j=i;
        while(j && cmp(b+(j-1)*size,b+j*size)>0) {
            for(size_t k=0;k<size;k++){ unsigned char t=b[(j-1)*size+k]; b[(j-1)*size+k]=b[j*size+k]; b[j*size+k]=t; }
            --j;
        }
    }
}

int fprintf(FILE *f,const char *fmt,...) { (void)f;(void)fmt; return 0; }
int printf(const char *fmt,...) { (void)fmt; return 0; }

uint32_t berry_fnv1a32(const void *data,size_t n) {
    const unsigned char*p=data; uint32_t h=0x811c9dc5u;
    while(n--){ h^=*p++; h*=0x01000193u; }
    return h;
}
