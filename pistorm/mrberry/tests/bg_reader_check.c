/*
 * mrberry's background reader (core/mr_source.c) against a forward gap.
 *
 * Reads at 0 and 1 MB, waits 3 s so the reader fills its 8 MB ring, then
 * reads at 10 MB: past the window, but within half a ring of it. Before the
 * fix the reader had no free space to get there and this never returned.
 * Then a read back (a reset) and one further on. Each read must match a
 * plain fread() of the same bytes.
 *
 * Build from the repository root:
 *   m68k-amigaos-gcc -O2 -std=c99 -m68040 -noixemul \
 *       -Ipistorm/mrberry/core -Iplayer/core \
 *       pistorm/mrberry/tests/bg_reader_check.c \
 *       pistorm/mrberry/core/mr_source.c -o bg_reader_check
 * Run on an Amiga or in WinUAE with a file of at least 17 MB:
 *   bg_reader_check <file>        prints PASS; hanging is the bug.
 */
#include <stdio.h>
#include <string.h>
#include <proto/dos.h>
#include "mr_source.h"

#define MB (1024ul * 1024ul)

static unsigned char got[65536], want[65536];

static int check(mr_source *s, FILE *f, unsigned long off)
{
    int ok;
    printf("read at %lu KB... ", off / 1024ul);
    fflush(stdout);
    ok = mr_source_read_at(s, off, got, sizeof got);
    if (fseek(f, (long)off, SEEK_SET) ||
        fread(want, 1, sizeof want, f) != sizeof want) {
        printf("fread failed\n");
        return 0;
    }
    ok = ok && !memcmp(got, want, sizeof got);
    printf("%s\n", ok ? "ok" : "FAILED");
    fflush(stdout);
    return ok;
}

int main(int argc, char **argv)
{
    mr_source *s;
    FILE *f;
    int ok = 1;
    if (argc < 2) {
        printf("usage: bg_reader_check <file of at least 17 MB>\n");
        return 20;
    }
    s = mr_source_open(argv[1]);
    f = fopen(argv[1], "rb");
    /* close the source on every exit: it owns the reader process */
    if (!s || !f || mr_source_length(s) < 17ul * MB) {
        printf(s && f ? "the file must be at least 17 MB\n"
                      : "cannot open the file\n");
        if (s) mr_source_close(s);
        if (f) fclose(f);
        return 20;
    }
    ok &= check(s, f, 0);
    ok &= check(s, f, 1ul * MB);
    Delay(150);                       /* 3 s: the reader fills its ring */
    ok &= check(s, f, 10ul * MB);     /* past the window, < half a ring */
    ok &= check(s, f, 3ul * MB);      /* back: a reset */
    ok &= check(s, f, 16ul * MB + 12345ul);
    mr_source_close(s);
    fclose(f);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 10;
}

/* mr_source.c's network openers. A local file never reaches the ones that
 * open anything. */
int mr_source_is_hls(const char *url) { (void)url; return 0; }
int mr_youtube_is_url(const char *url) { (void)url; return 0; }
void *mr_hls_source_open_ex(void) { return NULL; }
void *mr_http_source_open_ex(void) { return NULL; }
int mr_youtube_http_options_init(void) { return 0; }
int mr_youtube_resolve_media(void) { return 0; }
int mr_youtube_media_http_options_init(void) { return 0; }
