/* Minimal stdlib.h for the freestanding BerryCore E-AC3 build. */
#ifndef BERRY_STDLIB_H
#define BERRY_STDLIB_H
#include <stddef.h>
void abort(void) __attribute__((noreturn));
int abs(int);
long labs(long);
long long llabs(long long);
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
long strtol(const char *, char **, int);
double strtod(const char *, char **);
char *getenv(const char *);
#endif
