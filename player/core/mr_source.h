/*
 * MintVID - random-access compressed-media source.
 *
 * Demuxers ask for exact byte ranges without caring whether the bytes come
 * from a local stdio file or a buffered HTTP/HTTPS response.
 */
#ifndef MR_SOURCE_H
#define MR_SOURCE_H

#include "mr_types.h"

typedef struct mr_source mr_source;

typedef struct mr_source_timing {
    unsigned long read_ms;          /* all source reads (disk or network) */
    unsigned long network_ms;       /* HTTP reads/open waits */
    unsigned long hls_segment_ms;   /* HLS segment open/read waits */
    unsigned long hls_playlist_ms;  /* HLS playlist fetch/refetch waits */
} mr_source_timing;
struct mr_http_options;

/* Sentinel length for a forward-only (non-seekable) stream whose total size
 * is unknown up front — e.g. length-less chunked HTTP media. Callers must not
 * treat it as a real byte count; use mr_source_is_streaming() to detect it. */
#define MR_SOURCE_LEN_UNKNOWN ((size_t)-1)

int        mr_source_is_url(const char *path);
mr_source *mr_source_open(const char *path);
mr_source *mr_source_open_ex(const char *path,
                             const struct mr_http_options *options);
int        mr_source_read_at(mr_source *s, size_t off, void *dst, size_t len);
size_t     mr_source_length(const mr_source *s);
int        mr_source_is_streaming(const mr_source *s);
const char *mr_source_final_name(const mr_source *s);
void       mr_source_close(mr_source *s);
size_t     mr_source_buffer_capacity(const mr_source *s);
/* True once a local file has been fully read into a Fast RAM block up
 * front - every mr_source_read_at() call is then served from memory with
 * no further disk I/O. False for the windowed stdio-buffer fallback (used
 * when the file does not fit the Fast buffer budget) and for non-local
 * sources. */
int        mr_source_is_memory_cached(const mr_source *s);

/* Process-local cumulative blocking-I/O counters.  They are intentionally
 * queried by the player only at rolling-report boundaries, never per frame. */
void       mr_source_timing_get(mr_source_timing *timing);
void       mr_source_timing_reset(void);
/* Off by default: mr_source_read_at() (and the HLS playlist/segment fetch
 * timers in mr_hls.c) only ever feed mr_source_timing_get(), read solely by
 * the --time report - skip the clock() calls entirely otherwise. Global,
 * like the g_timing it gates, since mr_source has no per-instance state a
 * caller could thread a flag through. */
void       mr_source_set_timing_enabled(int enabled);
int        mr_source_timing_enabled(void);
void       mr_source_timing_add_network(unsigned long ms);
void       mr_source_mark_network(mr_source *s);
void       mr_source_timing_add_hls_segment(unsigned long ms);
void       mr_source_timing_add_hls_playlist(unsigned long ms);

/* Last source-open diagnostic. Borrowed static text; intended for the
 * single-threaded CLI/player startup path. */
const char *mr_source_last_error(void);

/* HTTP backend constructor, used by mr_source_open(). */
mr_source *mr_http_source_open(const char *url);

/* Backend helper: allocate a source around callbacks and take ownership of
 * ctx. final_name is copied for diagnostics. */
mr_source *mr_source_create(void *ctx, size_t len,
                            int (*read_at)(void *, size_t, void *, size_t),
                            void (*close)(void *),
                            const char *final_name);
void       mr_source_set_error(const char *message);
void       mr_source_set_buffer_capacity(mr_source *s, size_t bytes);
void       mr_source_set_memory_cached(mr_source *s, int cached);

#endif /* MR_SOURCE_H */
