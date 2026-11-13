/* Minimal stdio.h for the freestanding BerryCore E-AC3 build. */
#ifndef BERRY_STDIO_H
#define BERRY_STDIO_H
#include <stddef.h>
#include <stdarg.h>
typedef struct BerryFILE FILE;
extern FILE *stdin, *stdout, *stderr;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int sprintf(char *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int vfprintf(FILE *, const char *, va_list);
int sscanf(const char *, const char *, ...);
int fputs(const char *, FILE *);
int puts(const char *);
#endif
