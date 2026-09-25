/*
 * MintVID - host check for the mr_http fetch-override seam (core/mr_http.c)
 * and core/mr_hls.c's use of it.
 *
 * amiga/hls_fetch.c's background worker depends on this seam routing every
 * playlist and segment fetch through a caller-supplied function instead of
 * mr_hls.c ever touching mr_http's real socket/TLS code directly - that is
 * what lets one dedicated task own this session's whole HTTP/S connection.
 * This test cannot exercise the Amiga worker itself (no AmigaOS toolchain on
 * this host), but it can and does prove the portable half of that contract:
 * once an override is installed, mr_hls_source_open_ex() never reaches the
 * real network at all - if it did, resolving "test.invalid" would fail (or
 * hang) instead of the fake fetch below being called.
 */
#include "../core/mr_hls.h"
#include "../core/mr_http.h"
#include "../core/mr_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_playlist =
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "#EXT-X-TARGETDURATION:2\n"
    "#EXT-X-MEDIA-SEQUENCE:0\n"
    "#EXTINF:2.0,\n"
    "seg0.ts\n"
    "#EXTINF:2.0,\n"
    "seg1.ts\n"
    "#EXTINF:2.0,\n"
    "seg2.ts\n"
    "#EXTINF:2.0,\n"
    "seg3.ts\n"
    "#EXTINF:2.0,\n"
    "seg4.ts\n"
    "#EXT-X-ENDLIST\n";

static int  g_calls;
static int  g_hint_calls;
static int  g_live;
static char g_last_hint_url[256];
/* A long live window (a DVR-style playlist): g_long_count segments of
 * g_long_target seconds each, segment N's body "N%03d". */
static int  g_long_count;
static int  g_long_target;
/* Segments the server adds on every playlist fetch after the first, and how
 * many playlist fetches / playlist hints there have been. */
static int  g_long_grow;
static int  g_playlist_fetches;
static int  g_playlist_hints;
static char g_long_playlist[16384];

static void build_long_playlist(void)
{
    char *p = g_long_playlist;
    int i;
    p += sprintf(p, "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:%d\n"
                    "#EXT-X-MEDIA-SEQUENCE:100\n", g_long_target);
    for (i = 0; i < g_long_count; i++)
        p += sprintf(p, "#EXTINF:%d.0,\nlong%d.ts\n", g_long_target, i);
}

static int fake_override(const char *url, const mr_http_options *options,
                         const char *post_json,
                         unsigned char **out, size_t *out_len,
                         size_t max_size)
{
    const char *body;
    size_t len;
    (void)options; (void)post_json; (void)max_size;
    g_calls++;
    if (g_long_count) {
        static char seg[8];
        const char *name = strstr(url, "long");
        if (strstr(url, "playlist.m3u8")) {
            if (g_playlist_fetches++ && g_long_grow) {
                g_long_count += g_long_grow;
                build_long_playlist();
            }
            body = g_long_playlist;
        }
        else if (name) {
            snprintf(seg, sizeof seg, "N%03d", atoi(name + 4));
            body = seg;
        } else return 0;
        len = strlen(body);
    } else if (strstr(url, "playlist.m3u8")) {
        body = g_playlist;
        len = strlen(body);
        if (g_live) len -= strlen("#EXT-X-ENDLIST\n");
    }
    else if (strstr(url, "seg0.ts"))  { body = "AAAA"; len = 4; }
    else if (strstr(url, "seg1.ts"))  { body = "BBBB"; len = 4; }
    else if (strstr(url, "seg2.ts"))  { body = "CCCC"; len = 4; }
    else if (strstr(url, "seg3.ts"))  { body = "DDDD"; len = 4; }
    else if (strstr(url, "seg4.ts"))  { body = "EEEE"; len = 4; }
    else return 0;
    *out = (unsigned char *)mr_alloc(len + 1);
    if (!*out) return 0;
    memcpy(*out, body, len);
    (*out)[len] = 0;
    *out_len = len;
    return 1;
}

static void fake_hint(const char *url, const mr_http_options *options)
{
    (void)options;
    g_hint_calls++;
    if (strstr(url, "playlist.m3u8")) g_playlist_hints++;
    strncpy(g_last_hint_url, url, sizeof g_last_hint_url - 1);
    g_last_hint_url[sizeof g_last_hint_url - 1] = 0;
}

int main(void)
{
    mr_source *s;
    mr_http_options options;
    unsigned char buf[8];
    int fails = 0;

    mr_http_set_fetch_override(fake_override);
    mr_http_set_prefetch_hint(fake_hint);

    if (!mr_http_fetch_override_active()) {
        printf("FAIL: override not reported active\n");
        fails++;
    }

    mr_http_options_init(&options, NULL, NULL);
    options.source_buffer_bytes = 64u * 1024u * 1024u;
    s = mr_hls_source_open_ex("http://test.invalid/playlist.m3u8", &options);
    if (!s) {
        printf("FAIL: mr_hls_source_open_ex returned NULL: %s\n",
               mr_source_last_error());
        mr_http_set_fetch_override(NULL);
        mr_http_set_prefetch_hint(NULL);
        return 1;
    }

    if (!mr_source_read_at(s, 0, buf, 4) || memcmp(buf, "AAAA", 4) != 0) {
        printf("FAIL: segment 0 content mismatch\n");
        fails++;
    }
    /* VOD should queue every known segment within the lookahead window. */
    if (g_hint_calls != 4 || !strstr(g_last_hint_url, "seg4.ts")) {
        printf("FAIL: expected four prefetch hints through seg4.ts, got %d hint(s) "
               "(last=%s)\n", g_hint_calls, g_last_hint_url);
        fails++;
    }
    if (!mr_source_read_at(s, 4, buf, 4) || memcmp(buf, "BBBB", 4) != 0) {
        printf("FAIL: segment 1 content mismatch\n");
        fails++;
    }
    /* Exactly 3 fetches expected: the playlist plus the two read segments. If the
     * override weren't honoured, mr_hls_source_open_ex() would have tried to
     * resolve test.invalid via the real network instead. */
    if (g_calls != 3) {
        printf("FAIL: expected 3 override calls (playlist+2 segments), "
               "got %d\n", g_calls);
        fails++;
    }

    mr_source_close(s);
    /* The same playlist without ENDLIST is live: avoid downloading most of
     * the sliding window and drifting away from the broadcast. */
    g_live = 1;
    g_hint_calls = 0;
    s = mr_hls_source_open_ex("http://test.invalid/playlist.m3u8", &options);
    if (!s || !mr_source_read_at(s, 0, buf, 4) ||
        g_hint_calls != 3 || !strstr(g_last_hint_url, "seg3.ts")) {
        printf("FAIL: live HLS expected three lookahead hints through seg3.ts\n");
        fails++;
    }
    if (s) mr_source_close(s);

    /* A long live window with no requested start count begins about 30 s
     * before the newest segment, not at the oldest entry: 8 s targets keep
     * four segments (40 listed -> start at long36), 2 s targets keep
     * fifteen (40 -> long25), and a short window is kept whole. */
    {
        static const struct { int count, target, first; } cases[] = {
            { 40, 8, 36 }, { 40, 2, 25 }, { 3, 8, 0 }, { 40, 20, 37 }
        };
        size_t i;
        for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            char want[8];
            g_long_count = cases[i].count;
            g_long_target = cases[i].target;
            build_long_playlist();
            snprintf(want, sizeof want, "N%03d", cases[i].first);
            s = mr_hls_source_open_ex("http://test.invalid/playlist.m3u8",
                                      &options);
            if (!s || !mr_source_read_at(s, 0, buf, 4) ||
                memcmp(buf, want, 4) != 0) {
                printf("FAIL: live window of %d x %d s should start at "
                       "long%d\n", cases[i].count, cases[i].target,
                       cases[i].first);
                fails++;
            }
            if (s) mr_source_close(s);
        }
        /* An explicit count still wins (YouTube asks for two). */
        g_long_count = 40;
        g_long_target = 8;
        build_long_playlist();
        options.hls_live_start_segments = 2;
        s = mr_hls_source_open_ex("http://test.invalid/playlist.m3u8",
                                  &options);
        if (!s || !mr_source_read_at(s, 0, buf, 4) ||
            memcmp(buf, "N038", 4) != 0) {
            printf("FAIL: hls_live_start_segments=2 should start at long38\n");
            fails++;
        }
        if (s) mr_source_close(s);
        options.hls_live_start_segments = 0;
        g_long_count = 0;
    }

    /* A live playlist is re-read in the background before playback reaches
     * its last known segment, so reading straight through never stalls at
     * the edge. 40 x 8 s keeps long36..long39. Opening long36 leaves three
     * segments ahead, so the playlist is hinted then and taken when long37
     * opens: 4 segments were added, so long36..long43 are known. Opening
     * long40 leaves three ahead again, and long41's open takes the next
     * refresh. Reading long36..long43 must therefore cost exactly three
     * playlist fetches (open + two refreshes) and two playlist hints. */
    {
        int k;
        g_long_count = 40;
        g_long_target = 8;
        g_long_grow = 4;
        g_playlist_fetches = 0;
        g_playlist_hints = 0;
        build_long_playlist();
        s = mr_hls_source_open_ex("http://test.invalid/playlist.m3u8",
                                  &options);
        for (k = 0; s && k < 8; k++) {
            char want[8];
            snprintf(want, sizeof want, "N%03d", 36 + k);
            if (!mr_source_read_at(s, (size_t)k * 4, buf, 4) ||
                memcmp(buf, want, 4) != 0) {
                printf("FAIL: early live refresh: segment %d should be "
                       "long%d\n", k, 36 + k);
                fails++;
                break;
            }
        }
        if (!s || g_playlist_fetches != 3 || g_playlist_hints != 2) {
            printf("FAIL: early live refresh expected 3 playlist fetches and "
                   "2 playlist hints, got %d and %d\n", g_playlist_fetches,
                   g_playlist_hints);
            fails++;
        }
        if (s) mr_source_close(s);
        g_long_count = 0;
        g_long_grow = 0;
    }
    mr_http_set_fetch_override(NULL);
    mr_http_set_prefetch_hint(NULL);

    if (fails) {
        printf("mr_hls fetch-override checks FAILED: %d mismatch(es)\n",
               fails);
        return 1;
    }
    printf("mr_hls fetch-override checks passed\n");
    return 0;
}
