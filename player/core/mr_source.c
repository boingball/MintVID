/*
 * MintVID - local-file and generic source ownership.
 */
#include "mr_source.h"
#include "mr_hls.h"
#include "mr_http.h"
#include "mr_youtube.h"
#include "mr_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MR_SOURCE_NAME_MAX 1024
#define MR_SOURCE_ERROR_MAX 192

struct mr_source {
    void   *ctx;
    size_t  len;
    int   (*read_at)(void *, size_t, void *, size_t);
    void  (*close)(void *);
    char    final_name[MR_SOURCE_NAME_MAX];
    int     network;
};

typedef struct {
    FILE   *file;
    size_t  pos;
    int     pos_valid;
} file_source;

static char g_source_error[MR_SOURCE_ERROR_MAX];
static mr_source_timing g_timing;
static int g_timing_enabled = 0;

void mr_source_set_timing_enabled(int enabled) { g_timing_enabled = enabled != 0; }
int  mr_source_timing_enabled(void) { return g_timing_enabled; }

static unsigned long elapsed_ms(clock_t start)
{
    clock_t elapsed = clock() - start;
    return (unsigned long)((elapsed * 1000UL) / CLOCKS_PER_SEC);
}

void mr_source_timing_get(mr_source_timing *timing)
{
    if (timing) *timing = g_timing;
}
void mr_source_timing_reset(void) { memset(&g_timing, 0, sizeof g_timing); }
void mr_source_timing_add_network(unsigned long ms) { g_timing.network_ms += ms; }
void mr_source_mark_network(mr_source *s) { if (s) s->network = 1; }
void mr_source_timing_add_hls_segment(unsigned long ms) { g_timing.hls_segment_ms += ms; }
void mr_source_timing_add_hls_playlist(unsigned long ms) { g_timing.hls_playlist_ms += ms; }


void mr_source_set_error(const char *message)
{
    size_t n;
    if (!message) message = "source open failed";
    n = strlen(message);
    if (n >= sizeof g_source_error) n = sizeof g_source_error - 1;
    memcpy(g_source_error, message, n);
    g_source_error[n] = '\0';
}

const char *mr_source_last_error(void)
{
    return g_source_error[0] ? g_source_error : "source open failed";
}

static int starts_nocase(const char *s, const char *prefix)
{
    while (*prefix) {
        int a = (unsigned char)*s++;
        int b = (unsigned char)*prefix++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}

int mr_source_is_url(const char *path)
{
    return path && (starts_nocase(path, "http://") ||
                    starts_nocase(path, "https://"));
}

mr_source *mr_source_create(void *ctx, size_t len,
                            int (*read_at)(void *, size_t, void *, size_t),
                            void (*close)(void *),
                            const char *final_name)
{
    mr_source *s;
    size_t n;
    /* len == MR_SOURCE_LEN_UNKNOWN marks a forward-only stream; only a truly
     * zero-length source is rejected. */
    if (!ctx || !read_at || !close || !len) return NULL;
    s = (mr_source *)mr_allocz(sizeof *s);
    if (!s) {
        close(ctx);
        mr_source_set_error("not enough memory for media source");
        return NULL;
    }
    s->ctx = ctx;
    s->len = len;
    s->read_at = read_at;
    s->close = close;
    if (!final_name) final_name = "";
    n = strlen(final_name);
    if (n >= sizeof s->final_name) n = sizeof s->final_name - 1;
    memcpy(s->final_name, final_name, n);
    s->final_name[n] = '\0';
    return s;
}

static int file_read_at(void *opaque, size_t off, void *dst, size_t len)
{
    file_source *f = (file_source *)opaque;
    if (!f || !f->file || (!dst && len)) return 0;
    if (!f->pos_valid || f->pos != off) {
        if (off > 0x7fffffffUL || fseek(f->file, (long)off, SEEK_SET) != 0) {
            f->pos_valid = 0;
            return 0;
        }
    }
    if (len && fread(dst, 1, len, f->file) != len) {
        f->pos_valid = 0;
        return 0;
    }
    f->pos = off + len;
    f->pos_valid = 1;
    return 1;
}

static void file_close(void *opaque)
{
    file_source *f = (file_source *)opaque;
    if (!f) return;
    if (f->file) fclose(f->file);
    free(f);
}

static mr_source *open_local_file(const char *path)
{
    file_source *ctx;
    mr_source *source;
    long end;
    FILE *file = fopen(path, "rb");
    if (!file) {
        mr_source_set_error("cannot open local file");
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (end = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        mr_source_set_error("cannot determine local file size");
        return NULL;
    }
    ctx = (file_source *)calloc(1, sizeof *ctx);
    if (!ctx) {
        fclose(file);
        mr_source_set_error("not enough memory for local file");
        return NULL;
    }
    ctx->file = file;
    ctx->pos = 0;
    ctx->pos_valid = 1;
    source = mr_source_create(ctx, (size_t)end, file_read_at, file_close, path);
    return source;
}

mr_source *mr_source_open_ex(const char *path,
                             const struct mr_http_options *options)
{
    char youtube_media[MR_HTTP_URL_MAX];
    mr_http_options youtube_options;
    mr_youtube_media_kind youtube_kind;
    g_source_error[0] = '\0';
    if (!path || !*path) {
        mr_source_set_error("empty media path");
        return NULL;
    }
    if (mr_source_is_hls(path))
        return mr_hls_source_open_ex(path, options);
    if (mr_youtube_is_url(path)) {
        if (!mr_youtube_http_options_init(&youtube_options, options) ||
            !mr_youtube_resolve_media(path, &youtube_options, youtube_media,
                                      sizeof youtube_media, &youtube_kind) ||
            !mr_youtube_media_http_options_init(&youtube_options, options))
            return NULL;
        if (youtube_kind == MR_YOUTUBE_MEDIA_HLS)
            return mr_hls_source_open_ex(youtube_media, &youtube_options);
        return mr_http_source_open_ex(youtube_media, &youtube_options);
    }
    if (mr_source_is_url(path))
        return mr_http_source_open_ex(path, options);
    return open_local_file(path);
}

mr_source *mr_source_open(const char *path)
{
    return mr_source_open_ex(path, NULL);
}

int mr_source_read_at(mr_source *s, size_t off, void *dst, size_t len)
{
    if (!s || (!dst && len)) return 0;
    /* Seekable sources are bounds-checked against their known size. A streaming
     * source has no known end, so the backend reports EOF via a short read. */
    if (s->len != MR_SOURCE_LEN_UNKNOWN &&
        (off > s->len || len > s->len - off))
        return 0;
    if (!g_timing_enabled)
        return s->read_at(s->ctx, off, dst, len);
    {
        clock_t started = clock();
        int ok = s->read_at(s->ctx, off, dst, len);
        unsigned long ms = elapsed_ms(started);
        g_timing.read_ms += ms;
        if (s->network) g_timing.network_ms += ms;
        return ok;
    }
}

size_t mr_source_length(const mr_source *s)
{
    return s ? s->len : 0;
}

int mr_source_is_streaming(const mr_source *s)
{
    return s && s->len == MR_SOURCE_LEN_UNKNOWN;
}

const char *mr_source_final_name(const mr_source *s)
{
    return s ? s->final_name : "";
}

void mr_source_close(mr_source *s)
{
    if (!s) return;
    s->close(s->ctx);
    mr_free(s);
}
