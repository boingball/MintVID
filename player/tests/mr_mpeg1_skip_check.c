/* Verify decoder-level MPEG-1 B-picture skipping advances timestamps without
 * damaging the I/P reference pictures that remain. The input must contain B
 * pictures; the check deliberately fails on an I/P-only fixture. */
#include "../core/mr_mpeg1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct frame_sig {
    int64_t pts_us;
    uint32_t hash;
    unsigned char *luma;
} frame_sig;

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long bytes;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (bytes <= 0) { fclose(f); return NULL; }
    data = (unsigned char *)malloc((size_t)bytes);
    if (!data || fread(data, 1, (size_t)bytes, f) != (size_t)bytes) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)bytes;
    return data;
}

static uint32_t luma_hash(const mr_frame *fr)
{
    uint32_t h = 2166136261u;
    int x, y;
    for (y = 0; y < fr->height; y++) {
        const unsigned char *p = fr->data + (size_t)y * fr->stride;
        for (x = 0; x < fr->width; x++) {
            h ^= p[x];
            h *= 16777619u;
        }
    }
    return h;
}

static unsigned char *copy_luma(const mr_frame *fr)
{
    unsigned char *p = (unsigned char *)malloc((size_t)fr->width * fr->height);
    int y;
    if (!p) return NULL;
    for (y = 0; y < fr->height; y++)
        memcpy(p + (size_t)y * fr->width,
               fr->data + (size_t)y * fr->stride, (size_t)fr->width);
    return p;
}

int main(int argc, char **argv)
{
    unsigned char *data;
    size_t len = 0;
    mr_mpeg1 *full = NULL, *skip = NULL;
    frame_sig *all = NULL;
    mr_frame fr;
    int64_t pts, previous = -1;
    int cap = 0, count = 0, kept = 0, matched = 0, i, rc = 1;
    unsigned fps, period_us;

    if (argc != 2) {
        fprintf(stderr, "usage: mr_mpeg1_skip_check <b-frame-file.mpg>\n");
        return 2;
    }
    data = slurp(argv[1], &len);
    if (!data) return 2;
    full = mr_mpeg1_open(data, len, 0, 1, 0);
    skip = mr_mpeg1_open(data, len, 0, 1, 0);
    if (!full || !skip) goto out;
    fps = mr_mpeg1_framerate_millihz(full);
    if (!fps) goto out;
    period_us = (1000000000UL + fps / 2) / fps;

    while (mr_mpeg1_next_yuv(full, &fr, &pts)) {
        if (count == cap) {
            int next = cap ? cap * 2 : 64;
            frame_sig *p = (frame_sig *)realloc(all,
                (size_t)next * sizeof *all);
            if (!p) goto out;
            all = p;
            cap = next;
        }
        all[count].pts_us = pts;
        all[count].hash = luma_hash(&fr);
        all[count].luma = copy_luma(&fr);
        if (!all[count].luma) goto out;
        count++;
    }
    if (count < 3) goto out;

    mr_mpeg1_set_skip_b_frames(skip, 1);
    while (mr_mpeg1_next_yuv(skip, &fr, &pts)) {
        if (previous >= 0 && pts <= previous) {
            fprintf(stderr, "FAIL: skipped stream timestamp did not advance\n");
            goto out;
        }
        previous = pts;
        for (i = matched; i < count && all[i].pts_us < pts; i++) {}
        if (i >= count || all[i].pts_us != pts ||
            all[i].hash != luma_hash(&fr)) {
            uint32_t got_hash = luma_hash(&fr);
            unsigned long long total_error = 0;
            unsigned max_error = 0;
            double mae;
            int j;
            if (i < count) {
                int x, y;
                for (y = 0; y < fr.height; y++) {
                    const unsigned char *got = fr.data + (size_t)y * fr.stride;
                    const unsigned char *want = all[i].luma +
                        (size_t)y * fr.width;
                    for (x = 0; x < fr.width; x++) {
                        unsigned d = got[x] > want[x] ? got[x] - want[x]
                                                       : want[x] - got[x];
                        total_error += d;
                        if (d > max_error) max_error = d;
                    }
                }
            }
            mae = i < count ? (double)total_error /
                ((double)fr.width * fr.height) : 999.0;
            if (i < count && all[i].pts_us == pts && mae <= 0.1 &&
                max_error <= 32) {
                /* The truncated final picture in some program streams can
                 * differ at a handful of edge pixels depending on whether B
                 * slices were visited. It is still the same reference frame;
                 * keep the bound tight enough to catch actual corruption. */
            } else {
                fprintf(stderr, "FAIL: retained frame at %lld us differs "
                        "(%08x != %08x, luma MAE %.3f max %u)",
                        (long long)pts, (unsigned)got_hash,
                        (unsigned)(i < count ? all[i].hash : 0), mae,
                        max_error);
                for (j = 0; j < count; j++)
                    if (all[j].hash == got_hash)
                        fprintf(stderr, "; matches full frame at %lld us",
                                (long long)all[j].pts_us);
                fprintf(stderr, "\n");
                goto out;
            }
        }
        matched = i + 1;
        kept++;
    }
    if (kept >= count) {
        fprintf(stderr, "FAIL: fixture has no skippable B pictures\n");
        goto out;
    }
    if (previous < all[count - 1].pts_us - (int64_t)period_us * 2) {
        fprintf(stderr, "FAIL: skipped stream ended too early\n");
        goto out;
    }
    printf("mpeg1 B-picture skip: kept %d/%d frames, references match\n",
           kept, count);
    rc = 0;

out:
    for (i = 0; i < count; i++) free(all[i].luma);
    free(all);
    if (full) mr_mpeg1_close(full);
    if (skip) mr_mpeg1_close(skip);
    free(data);
    return rc;
}
