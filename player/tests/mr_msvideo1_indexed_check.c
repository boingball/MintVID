/* Prove MS Video 1's direct INDEX8 output is byte-identical to decoding
 * RGB24 and applying the established full-frame ordered dither afterwards. */
#include "../core/mr_avi.h"
#include "../core/mr_dither.h"
#include "../core/mr_msvideo1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mr_source_read_at(mr_source *source, size_t offset, void *dst, size_t len)
{
    (void)source; (void)offset; (void)dst; (void)len;
    return 0;
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *data;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = (uint8_t *)malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return data;
}

static mr_status open_ms1(mr_decoder *dec, const mr_video_info *vi)
{
    memset(dec, 0, sizeof *dec);
    dec->codec = &mr_codec_msvideo1;
    dec->width = vi->width;
    dec->height = vi->height;
    dec->config = vi->config;
    dec->config_len = vi->config_len;
    return dec->codec->open(dec);
}

static int check_depth(mr_avi *avi, int depth)
{
    mr_decoder rgb, indexed;
    mr_packet pkt;
    uint8_t *expected;
    size_t bytes = (size_t)avi->video.width * avi->video.height;
    unsigned frames = 0;
    int ok = 1;

    if (open_ms1(&rgb, &avi->video) != MR_OK ||
        open_ms1(&indexed, &avi->video) != MR_OK) {
        fprintf(stderr, "depth %d: decoder open failed\n", depth);
        return 0;
    }
    if (!mr_msvideo1_set_indexed_output(&indexed, depth)) {
        fprintf(stderr, "depth %d: indexed output selection failed\n", depth);
        rgb.codec->close(&rgb); indexed.codec->close(&indexed);
        return 0;
    }
    expected = (uint8_t *)malloc(bytes);
    if (!expected) {
        rgb.codec->close(&rgb); indexed.codec->close(&indexed);
        return 0;
    }

    mr_avi_rewind(avi);
    while (mr_avi_next_packet(avi, &pkt) == MR_OK) {
        mr_status a, b;
        if (!pkt.is_video) continue;
        a = rgb.codec->decode(&rgb, pkt.data, pkt.len);
        b = indexed.codec->decode(&indexed, pkt.data, pkt.len);
        if (a != MR_OK || b != MR_OK) {
            fprintf(stderr, "depth %d frame %u: decode status %d/%d\n",
                    depth, frames + 1, (int)a, (int)b);
            ok = 0; break;
        }
        mr_dither_rgb_indexed(rgb.frame.data, rgb.frame.width,
                              rgb.frame.height, rgb.frame.stride,
                              expected, rgb.frame.width, 0, depth);
        if (indexed.frame.fmt != MR_PIX_INDEX8 ||
            indexed.frame.stride != indexed.frame.width ||
            indexed.frame.dirty_y0 != rgb.frame.dirty_y0 ||
            indexed.frame.dirty_y1 != rgb.frame.dirty_y1 ||
            memcmp(indexed.frame.data, expected, bytes) != 0) {
            size_t i;
            for (i = 0; i < bytes && indexed.frame.data[i] == expected[i]; i++) {}
            fprintf(stderr,
                    "depth %d frame %u: mismatch at byte %lu (%u != %u)\n",
                    depth, frames + 1, (unsigned long)i,
                    i < bytes ? indexed.frame.data[i] : 0,
                    i < bytes ? expected[i] : 0);
            ok = 0; break;
        }
        frames++;
    }

    if (!frames) ok = 0;
    printf("MS Video 1 direct INDEX%d: %u frames, %s\n",
           depth, frames, ok ? "byte-identical" : "FAILED");
    free(expected);
    rgb.codec->close(&rgb); indexed.codec->close(&indexed);
    return ok;
}

int main(int argc, char **argv)
{
    uint8_t *data;
    size_t len;
    mr_avi avi;
    int ok;
    if (argc != 2) {
        fprintf(stderr, "usage: %s <msvideo1.avi>\n", argv[0]);
        return 2;
    }
    data = slurp(argv[1], &len);
    if (!data || mr_avi_open(&avi, data, len) != MR_OK) {
        fprintf(stderr, "not a readable MS Video 1 AVI: %s\n", argv[1]);
        free(data); return 2;
    }
    ok = check_depth(&avi, 4) && check_depth(&avi, 5) &&
         check_depth(&avi, 8);
    mr_avi_close(&avi);
    free(data);
    return ok ? 0 : 1;
}
