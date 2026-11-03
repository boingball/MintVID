/* mr_mkv_seek() check.
 *
 * Reads the whole file once to record every packet and which video packets
 * are IDR pictures (AVCC NAL type 5). Then, for a range of targets, seeks
 * and requires that:
 *   - the reported time is the last IDR at or before the target (the first
 *     IDR when the target is earlier than all of them),
 *   - the next packet is that IDR,
 *   - every packet from there to the end matches the linear read exactly.
 * Run on a file with a Cues index and on one written without (pipe output),
 * so both the index path and the cluster-scan fallback are exercised. */
#include "../core/mr_mkv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mr_source_read_at(mr_source *source, size_t offset, void *dst, size_t len)
{
    (void)source; (void)offset; (void)dst; (void)len; return 0;
}

typedef struct {
    int is_video, idr;
    uint64_t pts_us;
    uint32_t len, sum;
} rec;

static uint32_t checksum(const uint8_t *p, uint32_t n)
{
    uint32_t s = 2166136261u;
    while (n--) s = (s ^ *p++) * 16777619u;
    return s;
}

static int avcc_is_idr(const uint8_t *p, uint32_t n)
{
    uint32_t pos = 0;
    while (pos + 4 < n) {
        uint32_t len = ((uint32_t)p[pos] << 24) | ((uint32_t)p[pos + 1] << 16) |
                       ((uint32_t)p[pos + 2] << 8) | p[pos + 3];
        pos += 4;
        if (len == 0 || len > n - pos) return 0;
        if ((p[pos] & 0x1f) == 5) return 1;
        pos += len;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const uint64_t targets_ms[] = {
        0, 1, 500, 1479, 1480, 1481, 2000, 2959, 2960, 4440, 5000,
        5919, 5920, 7399, 7400, 7999, 60000
    };
    FILE *f;
    long n;
    uint8_t *data;
    mr_mkv m;
    mr_packet pkt;
    rec *recs = NULL;
    size_t count = 0, cap = 0, i, t, ntargets = 0;
    uint64_t *targets = NULL;
    int failures = 0, idrs = 0;

    if (argc != 2) return 2;
    f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    data = (uint8_t *)malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    if (mr_mkv_open(&m, data, (size_t)n) != MR_OK) {
        fprintf(stderr, "%s: open failed\n", argv[1]); return 1;
    }

    while (mr_mkv_next_packet(&m, &pkt) == MR_OK) {
        if (count == cap) {
            cap = cap ? cap * 2 : 256;
            recs = (rec *)realloc(recs, cap * sizeof *recs);
            if (!recs) return 2;
        }
        recs[count].is_video = pkt.is_video;
        recs[count].idr = pkt.is_video && avcc_is_idr(pkt.data, pkt.len);
        recs[count].pts_us = pkt.pts_us;
        recs[count].len = pkt.len;
        recs[count].sum = checksum(pkt.data, pkt.len);
        idrs += recs[count].idr;
        count++;
    }
    if (idrs < 2) {
        fprintf(stderr, "%s: need at least two IDR pictures (found %d)\n",
                argv[1], idrs);
        return 1;
    }

    /* Fixed targets, plus one microsecond either side of every IDR. */
    {
        size_t fixed = sizeof targets_ms / sizeof targets_ms[0];
        targets = (uint64_t *)malloc((fixed + 3 * (size_t)idrs) * sizeof *targets);
        if (!targets) return 2;
        for (t = 0; t < fixed; t++) targets[ntargets++] = targets_ms[t] * 1000u;
        for (i = 0; i < count; i++) {
            if (!recs[i].idr) continue;
            if (recs[i].pts_us) targets[ntargets++] = recs[i].pts_us - 1;
            targets[ntargets++] = recs[i].pts_us;
            targets[ntargets++] = recs[i].pts_us + 1;
        }
    }

    for (t = 0; t < ntargets; t++) {
        uint64_t target = targets[t], got = 0;
        size_t want = count, first = count;
        for (i = 0; i < count; i++) {
            if (!recs[i].idr) continue;
            if (first == count) first = i;
            if (recs[i].pts_us <= target &&
                (want == count || recs[i].pts_us >= recs[want].pts_us))
                want = i;
        }
        if (want == count) want = first;
        /* Two IDRs with the same time cannot happen here; the last one in
         * file order at the chosen time is the one a seek must land on. */
        if (mr_mkv_seek(&m, target, &got) != MR_OK) {
            printf("  seek %llu us: failed\n", (unsigned long long)target);
            failures++; continue;
        }
        if (got != recs[want].pts_us) {
            printf("  seek %llu us: landed at %llu us, want %llu us\n",
                   (unsigned long long)target, (unsigned long long)got,
                   (unsigned long long)recs[want].pts_us);
            failures++; continue;
        }
        for (i = want; i < count; i++) {
            if (mr_mkv_next_packet(&m, &pkt) != MR_OK ||
                pkt.is_video != recs[i].is_video ||
                pkt.pts_us != recs[i].pts_us || pkt.len != recs[i].len ||
                checksum(pkt.data, pkt.len) != recs[i].sum) {
                printf("  seek %llu us: packet %lu after the seek differs\n",
                       (unsigned long long)target,
                       (unsigned long)(i - want));
                failures++; break;
            }
        }
        if (i == count && mr_mkv_next_packet(&m, &pkt) == MR_OK) {
            printf("  seek %llu us: extra packets at the end\n",
                   (unsigned long long)target);
            failures++;
        }
    }

    printf("MKV seek %s: %lu packets, %d IDR, cues=%s, %s\n", argv[1],
           (unsigned long)count, idrs, m.cues_state > 0 ? "yes" : "no",
           failures ? "FAILED" : "ok");
    mr_mkv_close(&m); free(recs); free(targets); free(data);
    return failures ? 1 : 0;
}
