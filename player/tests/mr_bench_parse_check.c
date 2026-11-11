/* Pins amiga/mr_bench_parse.h against the lines mrplay --benchmark prints.
 * The format strings below are copied from amiga/mrplay.c; if they change
 * there, change them here too. */
#include <stdio.h>
#include <string.h>
#include "../amiga/mr_bench_parse.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    mr_bench_result r;
    char line[512];

    mr_bench_reset(&r);
    mr_bench_parse_line(&r, "codec probe: avc1\n");
    snprintf(line, sizeof(line), "display backend: %s\n", "AGA (Kalms)");
    mr_bench_parse_line(&r, line);
    snprintf(line, sizeof(line),
             "AGA path: chipset=%s depth=%d ham=%d%s ehb=%d scale=%d "
             "resize=%d c2p=%s yuv=%s copper=%d%s\n",
             "AGA", 5, 0, "", 0, 1, 0, "fused32-fallback", "supported", 0, "");
    mr_bench_parse_line(&r, line);
    snprintf(line, sizeof(line),
             "benchmark: elapsed=%lu ms decoded=%u presented=%u dropped=%u "
             "drain-frames=%u stopped=%s\n", 12000UL, 300U, 288U, 12U, 0U, "eof");
    mr_bench_parse_line(&r, line);
    snprintf(line, sizeof(line),
             "benchmark totals: video-decode=%lu ms audio-decode=%lu ms "
             "yuv-indexed=%lu ms/%u frames yuv-rgb=%lu ms/%u frames "
             "display=%lu ms/%u frames\n", 9000UL, 0UL, 3000UL, 288U, 0UL, 0U,
             600UL, 288U);
    mr_bench_parse_line(&r, line);
    snprintf(line, sizeof(line),
             "benchmark per-operation: decode=%lu us conversion=%lu us "
             "display=%lu us core=%lu us\n", 30000UL, 10416UL, 2083UL, 29000UL);
    mr_bench_parse_line(&r, line);

    CHECK(r.have_summary);
    CHECK(!strcmp(r.backend, "AGA (Kalms)"));
    CHECK(!strcmp(r.c2p, "fused32-fallback"));
    CHECK(!strcmp(r.depth, "5"));
    CHECK(!strcmp(r.stopped, "eof"));
    CHECK(r.elapsed_ms == 12000 && r.decoded == 300 && r.presented == 288 &&
          r.dropped == 12);
    /* "display=" in the totals line must not be mistaken for per-op */
    CHECK(r.decode_us == 30000 && r.conv_us == 10416 && r.display_us == 2083 &&
          r.core_us == 29000);
    CHECK(mr_bench_fps10(&r) == 240);           /* 288 in 12 s = 24.0 */
    CHECK(mr_bench_max_fps10(&r) == 235);       /* 1e6/42499 us = 23.5 */

    /* A run that never printed a summary. */
    mr_bench_reset(&r);
    mr_bench_parse_line(&r, "no decoder for fourcc 'XVID'\n");
    CHECK(!r.have_summary && mr_bench_fps10(&r) == 0 && mr_bench_max_fps10(&r) == 0);

    if (fails) return 1;
    printf("mr_bench_parse_check: OK\n");
    return 0;
}
