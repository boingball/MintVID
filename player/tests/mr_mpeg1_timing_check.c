/* Verify the units exposed by the pl_mpeg source wrapper. */
#include "../core/mr_mpeg1.h"

#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char **argv)
{
    unsigned char *data;
    size_t len = 0;
    mr_mpeg1 *mpeg;
    mr_frame frame;
    int64_t pts[3];
    unsigned fps;
    int i;

    if (argc != 2) {
        fprintf(stderr, "usage: mr_mpeg1_timing_check <file.mpg>\n");
        return 2;
    }
    data = slurp(argv[1], &len);
    if (!data) return 2;
    mpeg = mr_mpeg1_open(data, len, 0, 1, 0);
    if (!mpeg) { free(data); return 1; }

    fps = mr_mpeg1_framerate_millihz(mpeg);
    if (fps != 25000) {
        fprintf(stderr, "frame rate is %u millihertz, expected 25000\n", fps);
        mr_mpeg1_close(mpeg); free(data); return 1;
    }
    for (i = 0; i < 3; i++) {
        if (!mr_mpeg1_next(mpeg, &frame, &pts[i])) {
            fprintf(stderr, "stream ended before frame %d\n", i + 1);
            mr_mpeg1_close(mpeg); free(data); return 1;
        }
    }
    for (i = 1; i < 3; i++) {
        int64_t delta = pts[i] - pts[i - 1];
        if (delta < 39990 || delta > 40010) {
            fprintf(stderr, "frame %d timestamp delta is %lld us\n",
                    i + 1, (long long)delta);
            mr_mpeg1_close(mpeg); free(data); return 1;
        }
    }
    mr_mpeg1_close(mpeg);
    free(data);
    return 0;
}
