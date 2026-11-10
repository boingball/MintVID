#ifndef BERRY_STDIO_H
#define BERRY_STDIO_H
typedef struct berry_FILE { int dummy; } FILE;
extern FILE *stderr;
int fprintf(FILE *f, const char *fmt, ...);
int printf(const char *fmt, ...);
#endif
