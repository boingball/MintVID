/*
 * mr_bench_parse.h - read the summary lines `mrplay --benchmark` prints.
 *
 * Portable C (no AmigaOS headers), so tests/mr_bench_parse_check.c can pin
 * it on the host. mrbench (amiga/mrbench.c) runs one mrplay per file and
 * mode, sends its output to a file, and feeds each line through here.
 *
 * Everything is integer: fps values are kept in tenths. A floating-point
 * printf must never be linked into an Amiga binary of this project (see
 * CLAUDE.md, the -lm/printf crash).
 */
#ifndef MR_BENCH_PARSE_H
#define MR_BENCH_PARSE_H

#include <stdio.h>
#include <string.h>

typedef struct mr_bench_result {
    int have_summary;            /* saw the "benchmark:" line */
    unsigned long elapsed_ms;
    unsigned long decoded, presented, dropped;
    unsigned long decode_us, conv_us, display_us, core_us;
    char backend[48];            /* "display backend: ..." */
    char c2p[32];                /* c2p= from the "AGA path:" line */
    char depth[8];               /* depth= from the "AGA path:" line */
    char stopped[8];             /* "eof" or "user" */
} mr_bench_result;

static void mr_bench_reset(mr_bench_result *r)
{
    memset(r, 0, sizeof(*r));
}

/* Copy the word after `key` (up to a space or end of line) into out. */
static int mr_bench_word(const char *line, const char *key, char *out, size_t cap)
{
    const char *p = strstr(line, key);
    size_t n = 0;
    if (!p || cap == 0) return 0;
    p += strlen(key);
    while (p[n] && p[n] != ' ' && p[n] != '\n' && p[n] != '\r' && n + 1 < cap) {
        out[n] = p[n];
        n++;
    }
    out[n] = 0;
    return n > 0;
}

static int mr_bench_num(const char *line, const char *key, unsigned long *out)
{
    const char *p = strstr(line, key);
    unsigned long v = 0;
    int any = 0;
    if (!p) return 0;
    p += strlen(key);
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned long)(*p - '0');
        p++;
        any = 1;
    }
    if (any) *out = v;
    return any;
}

/* Feed one line of mrplay output. */
static void mr_bench_parse_line(mr_bench_result *r, const char *line)
{
    if (!strncmp(line, "display backend: ", 17)) {
        size_t n = 0;
        const char *p = line + 17;
        while (p[n] && p[n] != '\n' && p[n] != '\r' && n + 1 < sizeof(r->backend)) {
            r->backend[n] = p[n];
            n++;
        }
        r->backend[n] = 0;
    } else if (!strncmp(line, "AGA path:", 9)) {
        mr_bench_word(line, " c2p=", r->c2p, sizeof(r->c2p));
        mr_bench_word(line, " depth=", r->depth, sizeof(r->depth));
    } else if (!strncmp(line, "benchmark: ", 11)) {
        r->have_summary = 1;
        mr_bench_num(line, "elapsed=", &r->elapsed_ms);
        mr_bench_num(line, "decoded=", &r->decoded);
        mr_bench_num(line, "presented=", &r->presented);
        mr_bench_num(line, "dropped=", &r->dropped);
        mr_bench_word(line, "stopped=", r->stopped, sizeof(r->stopped));
    } else if (!strncmp(line, "benchmark per-operation: ", 25)) {
        mr_bench_num(line, "decode=", &r->decode_us);
        mr_bench_num(line, "conversion=", &r->conv_us);
        mr_bench_num(line, "display=", &r->display_us);
        mr_bench_num(line, "core=", &r->core_us);
    }
}

/* Frames actually shown per second, in tenths (capped by the source rate,
 * because mrplay plays in real time). */
static unsigned long mr_bench_fps10(const mr_bench_result *r)
{
    if (!r->elapsed_ms) return 0;
    /* presented * 10000 fits 32 bits for any run under ~429000 frames */
    return r->presented * 10000UL / r->elapsed_ms;
}

/* What the pipeline could sustain with no pacing: one decode, one
 * conversion and one display per frame, in tenths of a frame per second.
 * Decode runs for every decoded frame, conversion and display only for
 * queued/shown ones, so this is an estimate, not a measurement. */
static unsigned long mr_bench_max_fps10(const mr_bench_result *r)
{
    unsigned long us = r->decode_us + r->conv_us + r->display_us;
    if (!us) return 0;
    return 10000000UL / us;
}

#endif
