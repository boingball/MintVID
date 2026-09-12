/*
 * MintVID - HLS (.m3u8) playlist source (VOD and live).
 *
 * Fetches an HLS playlist, resolves a master playlist down to one media
 * variant, then presents that variant's segments - concatenated - as a single
 * forward-only MPEG-TS byte stream for the mr_ts demuxer. Segment sizes are
 * discovered lazily as they are opened, so the source still supports the
 * demuxer's probe-then-rewind access pattern (random access across already
 * seen segments; sequential fetch beyond them).
 *
 * A VOD playlist (EXT-X-ENDLIST, or EXT-X-PLAYLIST-TYPE:VOD) is a fixed list.
 * A live playlist has neither: it is a sliding window of segments that the
 * server keeps extending. We track EXT-X-MEDIA-SEQUENCE so a re-fetch appends
 * only genuinely new segments, and when playback catches up to the last known
 * segment we re-fetch the playlist to discover more - until an EXT-X-ENDLIST
 * finally turns the stream into a bounded one and it ends naturally.
 */
#include "mr_hls.h"
#include "mr_alloc.h"
#include "mr_http.h"
#include "mr_types.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static mr_hls_wait_fn      g_wait_fn;
static void               *g_wait_opaque;
static int                 g_verbose;

void mr_hls_set_verbose(int enabled)
{
    g_verbose = enabled != 0;
}

void mr_hls_set_wait(mr_hls_wait_fn fn, void *opaque)
{
    g_wait_fn = fn;
    g_wait_opaque = opaque;
}

/* Open a playlist (is_segment 0) or media segment (is_segment 1). */
static mr_source *hls_open(const char *url, const mr_http_options *options,
                           int is_segment)
{
    (void)is_segment;
    return mr_http_source_open_ex(url, options);
}

#define HLS_PLAYLIST_MAX (8UL * 1024 * 1024)   /* sane cap on a playlist text  */
#define HLS_SEGMENT_MAX  (24UL * 1024 * 1024)  /* bounded RAM segment fallback */
#define HLS_URL_MAX      MR_HTTP_URL_MAX

/* How many times playback may re-fetch a live playlist that reports no new
 * segment before giving up and reporting end of stream (the player then hands
 * off to its reconnect-at-the-edge path). A serviceable wait now paces each
 * attempt to about half the target duration, so a modest count already covers
 * tens of seconds of edge stall while staying responsive throughout - unlike
 * the old uninterruptible 240-deep spin, which hammered the server and froze
 * the single task for the whole poll. */
#define HLS_LIVE_REFETCH_MAX 40
/* Fallback poll interval when the playlist declares no EXT-X-TARGETDURATION. */
#define HLS_LIVE_REFETCH_WAIT_MS 1000

typedef struct {
    char   **segs;        /* resolved segment URLs                            */
    size_t   nsegs;
    size_t   cap;         /* allocated slots in segs (seg_start holds cap + 1)*/
    size_t  *seg_start;   /* concatenated byte offset of each segment (+ end) */
    size_t   discovered;  /* segments whose size is known (seg_start[0..this])*/
    mr_source *cur;       /* currently open segment source                    */
    size_t   cur_seg;
    /* Live streaming state (unused for VOD). */
    int      live;        /* playlist has no ENDLIST: keep re-fetching        */
    unsigned target_ms;   /* EXT-X-TARGETDURATION: paces live re-fetch polls  */
    char    *playlist_url;/* media playlist URL to re-fetch for new segments  */
    unsigned long next_seq; /* media-sequence of the next not-yet-queued seg  */
    mr_http_options options; /* inherited by playlists and segments            */
    int      have_options;
} hls_source;

/* ---- URL detection ----------------------------------------------------- */

int mr_source_is_hls(const char *url)
{
    const char *q, *dot;
    size_t n;
    if (!mr_source_is_url(url)) return 0;
    q = strchr(url, '?');                       /* ignore any query string      */
    n = q ? (size_t)(q - url) : strlen(url);
    if (n < 5) return 0;
    dot = url + n - 5;
    return dot[0] == '.' && (dot[1] == 'm' || dot[1] == 'M') && dot[2] == '3' &&
           dot[3] == 'u' && (dot[4] == '8');
}

/* ---- small text helpers ------------------------------------------------ */

static char *fetch_text(const char *url, const mr_http_options *options)
{
    int timing = mr_source_timing_enabled();
    clock_t started = timing ? clock() : 0;
    size_t len;
    char *buf;
    /* The complete-response reader accepts chunked playlists with no
     * Content-Length and avoids pointless HEAD/Range probes before reading the
     * bounded text body in full. */
    if (!mr_http_fetch_text(url, options, &buf, &len, HLS_PLAYLIST_MAX))
        return NULL;
    if (timing)
        mr_source_timing_add_hls_playlist((unsigned long)
            ((clock() - started) * 1000UL / CLOCKS_PER_SEC));
    return buf;
}

/* Copy the next line into `line` (without EOL); return the start of the line
 * after (or NULL at end). Trims trailing CR and leading/trailing spaces. */
static char *next_line(char *p, char *line, size_t cap)
{
    char *e;
    size_t n;
    if (!p || !*p) return NULL;
    e = p;
    while (*e && *e != '\n') e++;
    n = (size_t)(e - p);
    while (n && (p[n-1] == '\r' || p[n-1] == ' ' || p[n-1] == '\t')) n--;
    while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
    if (n >= cap) n = cap - 1;
    memcpy(line, p, n);
    line[n] = '\0';
    return *e ? e + 1 : e;
}

static int starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int range_contains_nocase(const char *begin, const char *end,
                                 const char *needle)
{
    size_t n = strlen(needle);
    const char *p;
    if (!n || end < begin || (size_t)(end - begin) < n) return 0;
    for (p = begin; p + n <= end; p++) {
        size_t i;
        for (i = 0; i < n; i++) {
            int a = (unsigned char)p[i];
            int b = (unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

/* A master can advertise several codecs at the same resolution. YouTube now
 * commonly offers AV1/VP9 beside AVC; choosing solely by bandwidth can select
 * a rendition MintVID cannot decode and make the TS probe consume megabytes
 * before it gives up. If CODECS is absent, retain the historical permissive
 * behaviour. If it is present, require a video codec we actually support. */
static int variant_codecs_supported(const char *stream_inf)
{
    const char *p = strstr(stream_inf, "CODECS=");
    const char *end;
    if (!p) return 1;
    p += strlen("CODECS=");
    if (*p == '"') {
        p++;
        end = strchr(p, '"');
    } else {
        end = strchr(p, ',');
    }
    if (!end) end = p + strlen(p);
    return range_contains_nocase(p, end, "avc1") ||
           range_contains_nocase(p, end, "avc3") ||
           range_contains_nocase(p, end, "mp4v") ||
           range_contains_nocase(p, end, "mp2v") ||
           range_contains_nocase(p, end, "mp1v");
}

/* ---- segment list ------------------------------------------------------ */

/* Ensure segs[] has room for one more entry; seg_start[] tracks nsegs + 1
 * offsets, with seg_start[0] anchored at 0 on the first allocation. */
static int seg_reserve(hls_source *h)
{
    size_t nc;
    char **ns;
    size_t *nss;
    if (h->nsegs < h->cap) return 1;
    nc = h->cap ? h->cap * 2 : 64;
    ns = (char **)realloc(h->segs, nc * sizeof *ns);
    if (!ns) return 0;
    h->segs = ns;
    nss = (size_t *)realloc(h->seg_start, (nc + 1) * sizeof *nss);
    if (!nss) return 0;
    if (!h->cap) nss[0] = 0;
    h->seg_start = nss;
    h->cap = nc;
    return 1;
}

static int append_seg(hls_source *h, const char *url)
{
    if (!seg_reserve(h)) return 0;
    h->segs[h->nsegs] = (char *)malloc(strlen(url) + 1);
    if (!h->segs[h->nsegs]) return 0;
    strcpy(h->segs[h->nsegs], url);
    h->nsegs++;
    return 1;
}

typedef struct {
    unsigned char *buf;
    size_t len;
} hls_mem_source;

static int hls_mem_read(void *opaque, size_t off, void *dst, size_t len)
{
    hls_mem_source *m = (hls_mem_source *)opaque;
    if (off > m->len || len > m->len - off) return 0;
    memcpy(dst, m->buf + off, len);
    return 1;
}

static void hls_mem_close(void *opaque)
{
    hls_mem_source *m = (hls_mem_source *)opaque;
    if (!m) return;
    mr_free(m->buf);
    mr_free(m);
}

/* Some Google Video live segments are chunked and deliberately omit a length.
 * Buffer one bounded segment so the concatenating HLS source can assign its
 * byte offsets and still support the demuxer's short probe rewind. */
static mr_source *hls_open_buffered(const char *url,
                                    const mr_http_options *options)
{
    hls_mem_source *m;
    mr_source *s;
    unsigned char *buf = NULL;
    size_t len = 0;
    if (!mr_http_fetch_buffer(url, options, &buf, &len, HLS_SEGMENT_MAX))
        return NULL;
    m = (hls_mem_source *)mr_alloc(sizeof *m);
    if (!m) {
        mr_free(buf);
        mr_source_set_error("not enough memory for buffered HLS segment");
        return NULL;
    }
    m->buf = buf;
    m->len = len;
    s = mr_source_create(m, len, hls_mem_read, hls_mem_close, url);
    return s;
}

/* Discard stale entries at the head of an initial sliding window. next_seq is
 * deliberately unchanged: it already names the first segment after the full
 * playlist, which prevents a refresh from re-queueing the discarded entries. */
static void keep_live_tail(hls_source *h, size_t keep)
{
    size_t drop, i;
    if (!keep || h->nsegs <= keep) return;
    drop = h->nsegs - keep;
    for (i = 0; i < drop; i++) free(h->segs[i]);
    memmove(h->segs, h->segs + drop, keep * sizeof *h->segs);
    h->nsegs = keep;
    h->discovered = 0;
    h->seg_start[0] = 0;
}

/* ---- playlist parsing -------------------------------------------------- */

/* Choose a variant from a master playlist and resolve it against base_url into
 * `out`. Returns 1 if a variant was found.
 *
 * Selection: among the variants that fit the quality ceiling (hls_max_width/
 * height/fps, each 0 = don't care) pick the *highest* bandwidth, so a capable
 * machine (e.g. a PiStorm) uses its headroom instead of always the smallest
 * rendition. hls_low forces the lowest-bandwidth variant instead - the safe
 * choice for slower gear. A lowest-bandwidth fallback is always kept so a
 * master whose every rendition exceeds the ceiling still plays something. */
static int pick_variant(char *text, const char *base_url,
                        char *out, size_t out_size,
                        const mr_http_options *options)
{
    char line[HLS_URL_MAX];
    char chosen[HLS_URL_MAX];   /* best variant within the ceiling            */
    char fallback[HLS_URL_MAX]; /* lowest-bandwidth variant, ceiling ignored  */
    char *p = text;
    unsigned long chosen_bw = 0, fallback_bw = 0;
    int have_chosen = 0, have_fallback = 0, want_low, pending = 0;
    int pending_supported = 1;
    unsigned long pending_bw = 0;
    unsigned pending_width = 0, pending_height = 0, pending_fps = 0;
    want_low = options && options->hls_low;
    while ((p = next_line(p, line, sizeof line)) != NULL) {
        if (starts(line, "#EXT-X-STREAM-INF")) {
            const char *bw = strstr(line, "BANDWIDTH=");
            const char *resolution = strstr(line, "RESOLUTION=");
            const char *fps = strstr(line, "FRAME-RATE=");
            pending = 1;
            pending_supported = variant_codecs_supported(line);
            pending_bw = bw ? strtoul(bw + 10, NULL, 10) : 0;
            pending_width = pending_height = pending_fps = 0;
            if (resolution)
                sscanf(resolution + 11, "%ux%u", &pending_width,
                       &pending_height);
            if (fps)
                pending_fps = (unsigned)strtoul(fps + 11, NULL, 10);
        } else if (line[0] && line[0] != '#' && pending) {
            int fits;
            pending = 0;
            if (!pending_supported) continue;
            /* Always track the lowest-bandwidth variant as a safety net. */
            if (!have_fallback || pending_bw < fallback_bw) {
                memcpy(fallback, line, sizeof fallback);
                fallback_bw = pending_bw;
                have_fallback = 1;
            }
            fits = !options ||
                   ((!options->hls_max_width ||
                     pending_width <= options->hls_max_width) &&
                    (!options->hls_max_height ||
                     pending_height <= options->hls_max_height) &&
                    (!options->hls_max_fps ||
                     pending_fps <= options->hls_max_fps));
            if (fits &&
                (!have_chosen ||
                 (want_low ? pending_bw < chosen_bw : pending_bw > chosen_bw))) {
                memcpy(chosen, line, sizeof chosen);
                chosen_bw = pending_bw;
                have_chosen = 1;
            }
        }
    }
    if (have_chosen && mr_http_resolve_url(base_url, chosen, out, out_size))
        return 1;
    if (have_fallback && mr_http_resolve_url(base_url, fallback, out, out_size))
        return 1;
    return 0;
}

/* Merge a (possibly refreshed) media playlist into the segment list. Segments
 * are numbered by EXT-X-MEDIA-SEQUENCE + position, so only those at or beyond
 * next_seq are appended - a re-fetch of a sliding window skips the ones we
 * already hold. Sets h->live from the ENDLIST / PLAYLIST-TYPE tags. Returns
 * MR_OK (with *added = how many new segments were queued), or an error for an
 * encrypted playlist. */
static mr_status merge_playlist(char *text, const char *base_url,
                                hls_source *h, int *added)
{
    char line[HLS_URL_MAX], resolved[HLS_URL_MAX];
    char *p = text;
    unsigned long media_seq = 0, idx = 0;
    int endlist = 0, vod = 0;
    *added = 0;
    while ((p = next_line(p, line, sizeof line)) != NULL) {
        if (starts(line, "#EXT-X-KEY") && !strstr(line, "METHOD=NONE")) {
            mr_source_set_error("encrypted HLS (EXT-X-KEY) is not supported");
            return MR_EUNSUPPORTED;
        }
        if (starts(line, "#EXT-X-MEDIA-SEQUENCE:")) {
            media_seq = strtoul(line + strlen("#EXT-X-MEDIA-SEQUENCE:"),
                                NULL, 10);
            continue;
        }
        if (starts(line, "#EXT-X-TARGETDURATION:")) {
            unsigned long secs = strtoul(
                line + strlen("#EXT-X-TARGETDURATION:"), NULL, 10);
            if (secs) h->target_ms = (unsigned)(secs * 1000UL);
            continue;
        }
        if (starts(line, "#EXT-X-PLAYLIST-TYPE:") && strstr(line, "VOD"))
            vod = 1;
        if (starts(line, "#EXT-X-ENDLIST")) { endlist = 1; continue; }
        if (!line[0] || line[0] == '#') continue;      /* tag or blank         */
        {
            unsigned long seq = media_seq + idx++;
            if (seq < h->next_seq) continue;           /* already queued       */
            if (!mr_http_resolve_url(base_url, line, resolved, sizeof resolved))
                continue;
            if (!append_seg(h, resolved)) return MR_ENOMEM;
            h->next_seq = seq + 1;
            (*added)++;
        }
    }
    h->live = !(endlist || vod);
    return MR_OK;
}

/* ---- segment stream ---------------------------------------------------- */

/* Open segment `i`, recording its length so seg_start stays contiguous. Must
 * be opened in order for its start offset to be known. */
static int open_seg(hls_source *h, size_t i)
{
    int timing = mr_source_timing_enabled();
    clock_t started = timing ? clock() : 0;
    /* Independent of the --time-gated `started` above (and of the fuller
     * mr_source_timing_add_hls_segment() accounting further down): a single
     * clock() read bracketing this whole call, always taken under g_verbose
     * alone, is cheap enough (one libc call per segment open, not per
     * packet/frame) to report "requested -> completed" timing even when the
     * caller never asked for full --time instrumentation. This is what lets
     * a real-hardware log show exactly which segment a stall happened on:
     * "opening segment N" with no matching "ready"/"failed" line before the
     * log goes quiet means the fetch that started for segment N never
     * returned. */
    clock_t diag_started = g_verbose ? clock() : 0;
    mr_source *s;
    size_t len;
    if (i >= h->nsegs) return 0;
    if (h->cur && h->cur_seg == i) return 1;
    if (i > h->discovered) return 0;               /* start offset unknown     */
    /* Close the previous segment's source *before* opening the next one. The
     * platform TLS layer (AmiSSL) keeps global session state, so two HTTP/S
     * connections must never be open at the same time - overlapping them makes
     * the second close tear the first's state down twice. Closing first also
     * keeps only one segment's read-ahead buffer resident at a time. */
    if (h->cur) { mr_source_close(h->cur); h->cur = NULL; }
    if (g_verbose)
        printf("HLS: opening segment %lu of %lu\n",
               (unsigned long)(i + 1), (unsigned long)h->nsegs);
    /* A fetch override (e.g. amiga/hls_fetch.c's background worker) only
     * intercepts the complete-body fetch functions (mr_http_fetch_text()/
     * mr_http_fetch_buffer()), not the streaming mr_http_source_open_ex()
     * path hls_open() falls back to - that path opens its own connection
     * directly and would bypass the override, breaking the "only one task
     * ever touches this session's HTTP/S state" invariant it exists for. So
     * force the buffered path whenever an override is active, regardless of
     * hls_buffer_segments (every override-routed fetch is already a
     * complete-body fetch, a strict superset of what that option asks for). */
    if (mr_http_fetch_override_active() ||
        (h->have_options && h->options.hls_buffer_segments))
        s = hls_open_buffered(h->segs[i], &h->options);
    else
        s = hls_open(h->segs[i], h->have_options ? &h->options : NULL, 1);
    if (!s) {
        if (g_verbose)
            printf("HLS: segment %lu open failed: %s (URL %lu bytes, %lu ms)\n",
                   (unsigned long)(i + 1), mr_source_last_error(),
                   (unsigned long)strlen(h->segs[i]),
                   (unsigned long)((clock() - diag_started) * 1000UL /
                                    CLOCKS_PER_SEC));
        return 0;
    }
    len = mr_source_length(s);
    if (!len || len == MR_SOURCE_LEN_UNKNOWN) {
        mr_source_close(s);
        mr_source_set_error("HLS segment omitted a usable Content-Length");
        if (g_verbose)
            printf("HLS: segment %lu has no usable length (%lu ms)\n",
                   (unsigned long)(i + 1),
                   (unsigned long)((clock() - diag_started) * 1000UL /
                                    CLOCKS_PER_SEC));
        return 0;
    }
    if (g_verbose)
        printf("HLS: segment %lu ready (%lu KB, %lu ms)\n",
               (unsigned long)(i + 1), (unsigned long)(len / 1024),
               (unsigned long)((clock() - diag_started) * 1000UL /
                                CLOCKS_PER_SEC));
    h->cur = s;
    h->cur_seg = i;
    h->seg_start[i + 1] = h->seg_start[i] + len;
    if (i + 1 > h->discovered) h->discovered = i + 1;
    if (timing)
        mr_source_timing_add_hls_segment((unsigned long)
            ((clock() - started) * 1000UL / CLOCKS_PER_SEC));
    /* Best-effort background lookahead for the next segment, if its URL is
     * already known - a no-op unless a fetch override installed a hint (see
     * mr_http_prefetch_hint()). Never fires for a not-yet-discovered next
     * segment (i+1 == h->nsegs at the live edge), which is also exactly the
     * case hls_refetch_live()'s playlist poll needs the worker free for -
     * see amiga/hls_fetch.c's design note on this. */
    if (i + 1 < h->nsegs)
        mr_http_prefetch_hint(h->segs[i + 1],
                              h->have_options ? &h->options : NULL);
    return 1;
}

/* Return the segment index whose byte range contains `off`, opening segments
 * forward as needed to discover it. Returns nsegs if `off` is at/after the end
 * of the currently known segments. */
static size_t locate(hls_source *h, size_t off, int *open_failed)
{
    size_t i;
    *open_failed = 0;
    for (i = 0; i < h->discovered; i++)
        if (off < h->seg_start[i + 1]) return i;
    /* discover forward until off is covered or segments run out */
    while (h->discovered < h->nsegs) {
        if (!open_seg(h, h->discovered)) {
            *open_failed = 1;
            break;
        }
        if (off < h->seg_start[h->cur_seg + 1]) return h->cur_seg;
    }
    return h->nsegs;
}

/* Playback has caught up to the last known segment of a live stream: re-fetch
 * the playlist until it grows (new segments) or ends (ENDLIST). Returns 1 if
 * at least one new segment was appended, 0 if the stream ended or stalled. */
static int hls_refetch_live(hls_source *h)
{
    int tries;
    unsigned wait_ms;
    /* Playback has consumed the last known segment, so its source is done with:
     * close it before fetching the playlist so only one HTTP/S connection is
     * ever open at once (see open_seg). */
    if (h->cur) { mr_source_close(h->cur); h->cur = NULL; }
    if (g_verbose)
        printf("HLS: playback reached the live edge (%lu known segments); "
               "polling playlist for more\n", (unsigned long)h->nsegs);
    /* Poll no faster than about half the target duration: fresh segments appear
     * on that cadence, so a tighter poll only hammers the server (and, single-
     * threaded, freezes the caller for each round-trip). */
    wait_ms = h->target_ms ? h->target_ms / 2 : HLS_LIVE_REFETCH_WAIT_MS;
    if (wait_ms < 500) wait_ms = 500;
    if (wait_ms > 2000) wait_ms = 2000;
    for (tries = 0; tries < HLS_LIVE_REFETCH_MAX; tries++) {
        char *text;
        int added;
        mr_status st;
        /* Wait between attempts (not before the first, which may already find a
         * new segment). The hook keeps audio/video/UI alive during the pause and
         * returns nonzero if the user asked to quit. */
        if (tries && g_wait_fn && g_wait_fn(g_wait_opaque, wait_ms))
            return 0;
        text = fetch_text(h->playlist_url,
                          h->have_options ? &h->options : NULL);
        if (!text) {
            if (g_verbose)
                printf("HLS: live playlist re-fetch failed (attempt %d/%d): "
                       "%s\n", tries + 1, HLS_LIVE_REFETCH_MAX,
                       mr_source_last_error());
            return 0;                               /* playlist gone / error    */
        }
        st = merge_playlist(text, h->playlist_url, h, &added);
        mr_free(text);
        if (st != MR_OK) return 0;
        if (added > 0) {
            if (g_verbose)
                printf("HLS: live playlist grew by %d segment(s) (attempt "
                       "%d/%d)\n", added, tries + 1, HLS_LIVE_REFETCH_MAX);
            return 1;                                /* fresh segments to play   */
        }
        if (!h->live) return 0;                     /* ENDLIST arrived: done    */
    }
    if (g_verbose)
        printf("HLS: live playlist stalled after %d attempts - giving up\n",
               HLS_LIVE_REFETCH_MAX);
    mr_source_set_error("live HLS playlist stalled (no new segments)");
    return 0;
}

static int hls_read_at(void *opaque, size_t off, void *dst, size_t len)
{
    hls_source *h = (hls_source *)opaque;
    unsigned char *out = (unsigned char *)dst;
    if (!len) return 1;
    while (len) {
        int open_failed;
        size_t i = locate(h, off, &open_failed);
        size_t local, avail, take;
        if (open_failed) return 0;
        if (i >= h->nsegs) {
            /* Ran off the end of the known segments. For live, pull more from
             * the playlist and retry; for VOD/ended, this is end of stream. */
            if (h->live && hls_refetch_live(h)) continue;
            return 0;
        }
        if (!open_seg(h, i)) return 0;
        local = off - h->seg_start[i];
        avail = h->seg_start[i + 1] - h->seg_start[i] - local;
        take  = len < avail ? len : avail;
        if (!mr_source_read_at(h->cur, local, out, take)) return 0;
        out += take; off += take; len -= take;
    }
    return 1;
}

static void hls_close(void *opaque)
{
    hls_source *h = (hls_source *)opaque;
    size_t i;
    if (!h) return;
    if (h->cur) mr_source_close(h->cur);
    for (i = 0; i < h->nsegs; i++) free(h->segs[i]);
    free(h->segs);
    free(h->seg_start);
    free(h->playlist_url);
    free(h);
}

/* ---- open -------------------------------------------------------------- */

mr_source *mr_hls_source_open_ex(const char *url,
                                 const mr_http_options *options)
{
    char *text;
    char media_url[HLS_URL_MAX];
    const char *base;
    hls_source *h;
    mr_status st;
    int added;

    text = fetch_text(url, options);
    if (!text) return NULL;

    /* Master playlist? Resolve to one media variant and refetch. */
    base = url;
    if (strstr(text, "#EXT-X-STREAM-INF")) {
        if (g_verbose) printf("HLS: master playlist received\n");
        if (!pick_variant(text, url, media_url, sizeof media_url, options)) {
            mr_free(text);
            mr_source_set_error("no playable variant in HLS master playlist");
            return NULL;
        }
        if (g_verbose) printf("HLS: compatible media variant selected\n");
        mr_free(text);
        text = fetch_text(media_url, options);
        if (!text) return NULL;
        base = media_url;
        if (strstr(text, "#EXT-X-STREAM-INF")) {
            mr_free(text);
            mr_source_set_error("nested HLS master playlist is not supported");
            return NULL;
        }
    }

    h = (hls_source *)calloc(1, sizeof *h);
    if (!h) { mr_free(text); mr_source_set_error("out of memory for HLS"); return NULL; }
    if (options) {
        if (!mr_http_options_init(&h->options, options->user_agent,
                                  options->referer)) {
            mr_free(text);
            hls_close(h);
            return NULL;
        }
        h->have_options = 1;
        h->options.hls_low = options->hls_low;
        h->options.hls_live_start_segments =
            options->hls_live_start_segments;
        h->options.hls_buffer_segments = options->hls_buffer_segments;
        h->options.hls_max_width = options->hls_max_width;
        h->options.hls_max_height = options->hls_max_height;
        h->options.hls_max_fps = options->hls_max_fps;
        h->options.source_buffer_bytes = options->source_buffer_bytes;
    }
    st = merge_playlist(text, base, h, &added);
    mr_free(text);
    if (st != MR_OK) { hls_close(h); return NULL; }
    if (!h->nsegs) {
        hls_close(h);
        mr_source_set_error("HLS playlist has no segments");
        return NULL;
    }
    if (h->live && options && options->hls_live_start_segments &&
        h->nsegs > options->hls_live_start_segments) {
        size_t before = h->nsegs;
        keep_live_tail(h, options->hls_live_start_segments);
        if (g_verbose)
            printf("HLS: skipped %lu stale startup segments\n",
                   (unsigned long)(before - h->nsegs));
    }
    if (g_verbose)
        printf("HLS: %lu initial segments (%s)\n",
               (unsigned long)h->nsegs, h->live ? "live" : "bounded");

    /* A live playlist (no ENDLIST) keeps growing: remember its URL so playback
     * can re-fetch it to discover new segments as the stream advances. */
    if (h->live) {
        h->playlist_url = (char *)malloc(strlen(base) + 1);
        if (!h->playlist_url) {
            hls_close(h);
            mr_source_set_error("out of memory for HLS");
            return NULL;
        }
        strcpy(h->playlist_url, base);
    }

    /* Streaming (unknown total length): the demuxer reads forward and treats a
     * short read as end of stream. mr_source_create already closes the context
     * (hls_close) if it fails, so we must not close it again here. */
    return mr_source_create(h, MR_SOURCE_LEN_UNKNOWN, hls_read_at, hls_close, url);
}

mr_source *mr_hls_source_open(const char *url)
{
    return mr_hls_source_open_ex(url, NULL);
}
