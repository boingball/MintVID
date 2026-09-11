/*
 * MintVID - HTTP/HTTPS media source internals.
 */
#ifndef MR_HTTP_H
#define MR_HTTP_H

#if (((defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)) || \
      defined(__amigaos__) || defined(__AMIGA__))) && \
    defined(__GNUC__)
/*
 * The classic Amiga SDK inline stubs use unsigned-char CONST_STRPTR names and
 * non-const STRPTR/APTR parameters for several read-only socket arguments.
 * GCC consequently reports pointer-sign and discarded-qualifier warnings at
 * otherwise valid OpenLibrary(), gethostbyname(), setsockopt() and send()
 * calls. Keep those SDK-only diagnostics scoped to this HTTP translation unit
 * rather than weakening the warning policy for the complete Amiga build.
 */
#pragma GCC diagnostic ignored "-Wpointer-sign"
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
#endif

#include "mr_source.h"
#include <stddef.h>

#define MR_HTTP_URL_MAX  4096
#define MR_HTTP_PATH_MAX 3840
#define MR_HTTP_USER_AGENT_MAX 256
#define MR_HTTP_REFERER_MAX 1024

typedef struct mr_http_options {
    char user_agent[MR_HTTP_USER_AGENT_MAX];
    char referer[MR_HTTP_REFERER_MAX];
    int hls_low;
    /* For slow live-start resolvers, retain only this many newest segments
     * from the first sliding playlist. Zero preserves the complete window. */
    unsigned hls_live_start_segments;
    /* Download each HLS segment to a bounded RAM buffer before exposing it to
     * the demuxer. Required by CDNs that use chunked transfer without length. */
    int hls_buffer_segments;
    unsigned hls_max_width;
    unsigned hls_max_height;
    unsigned hls_max_fps;
    /* Optional local/progressive-HTTP read-ahead storage. On classic Amiga
     * builds this is allocated specifically from Fast RAM. Zero keeps each
     * backend's small/default buffer. HLS already buffers complete segments. */
    size_t source_buffer_bytes;
} mr_http_options;

int mr_http_options_init(mr_http_options *options, const char *user_agent,
                         const char *referer);

mr_source *mr_http_source_open(const char *url);
mr_source *mr_http_source_open_ex(const char *url,
                                  const mr_http_options *options);

/* Download a complete text response into a task-safe allocated buffer. The
 * caller owns *out and releases it with mr_free(). One NUL byte is appended but
 * is not included in *out_len. Both fixed-length and chunked bodies work.
 * Routed through the installed fetch override, if any - see
 * mr_http_set_fetch_override() below. */
int mr_http_fetch_text(const char *url, const mr_http_options *options,
                       char **out, size_t *out_len, size_t max_size);

/* Binary-safe complete-response variant. The returned task-safe buffer belongs
 * to the caller and must be released with mr_free(). Implemented as
 * mr_http_fetch_text(), so it is covered by the fetch override too. */
int mr_http_fetch_buffer(const char *url, const mr_http_options *options,
                         unsigned char **out, size_t *out_len,
                         size_t max_size);

/* POST a small JSON document and return the complete text response. Used by
 * lightweight resolver APIs that cannot be represented as a media GET. Routed
 * through the installed fetch override, if any. */
int mr_http_post_json(const char *url, const mr_http_options *options,
                      const char *json, char **out, size_t *out_len,
                      size_t max_size);

/* Unconditional variants of the two functions above: always perform the fetch
 * on the calling task, ignoring any installed override. Only the override's
 * own implementation should call these - everyone else wants the (possibly
 * redirected) public functions above. See mr_http_set_fetch_override(). */
int mr_http_fetch_text_direct(const char *url, const mr_http_options *options,
                              char **out, size_t *out_len, size_t max_size);
int mr_http_post_json_direct(const char *url, const mr_http_options *options,
                             const char *json, char **out, size_t *out_len,
                             size_t max_size);

/*
 * Redirect every mr_http_fetch_text()/mr_http_post_json()/
 * mr_http_fetch_buffer() call through `fn` instead of performing the fetch
 * directly on the calling task. Intended for a platform that wants exactly
 * one task to ever touch this file's socket/TLS state (see the design note
 * in mr_http.c above connect_socket()) - e.g. amiga/hls_fetch.c's background
 * worker, installed once before any other network call in the session so it
 * is that task, not whichever caller happened to run first. `post_json` is
 * NULL for a GET-shaped fetch (mr_http_fetch_text()/mr_http_fetch_buffer())
 * and non-NULL for mr_http_post_json(). Pass NULL to restore direct
 * per-caller-task fetching (the default; host builds and callers that never
 * install one are unaffected). Not reentrant with itself - `fn` must not
 * call back into mr_http_fetch_text()/mr_http_post_json() (use the _direct
 * variants above from inside it, which is exactly what a fetch override
 * needs anyway: perform the real fetch on whichever task is running `fn`). */
typedef int (*mr_http_fetch_override_fn)(const char *url,
    const mr_http_options *options, const char *post_json,
    unsigned char **out, size_t *out_len, size_t max_size);
void mr_http_set_fetch_override(mr_http_fetch_override_fn fn);
int  mr_http_fetch_override_active(void);

/* Best-effort background-lookahead hint for the next fetch this session is
 * likely to need (e.g. the next HLS segment) - a no-op unless a fetch
 * override has installed one via mr_http_set_prefetch_hint(). Never blocks;
 * never guarantees anything is actually prefetched. */
typedef void (*mr_http_prefetch_hint_fn)(const char *url,
                                         const mr_http_options *options);
void mr_http_set_prefetch_hint(mr_http_prefetch_hint_fn fn);
void mr_http_prefetch_hint(const char *url, const mr_http_options *options);

/* Download a complete response to a file using the shared redirect, TLS,
 * timeout and chunk decoder. The destination is removed on failure. */
int mr_http_download_file(const char *url, const char *path, size_t max_size);

/* Resolve a possibly-relative URL (an HLS variant/segment) against a base URL.
 * Handles absolute, scheme-relative (//host), root-relative (/path) and
 * directory-relative forms. Returns 1 on success. */
int mr_http_resolve_url(const char *base_url, const char *rel,
                        char *out, size_t out_size);

/* Release the process-wide socket/TLS state from the CURRENT task. bsdsocket
 * must be closed by the task that opened it, so a background task that performed
 * all the networking calls this before exiting; the atexit shutdown then no-ops.
 * Idempotent. */
void mr_http_net_shutdown(void);

/* Non-zero once an unhealthy TLS drop has disabled HTTPS for the rest of this
 * process (see mr_http.c). A live player uses this to stop retrying a reconnect
 * that can never succeed and end cleanly instead. Always zero on non-TLS
 * builds. */
int mr_http_tls_disabled(void);

/* Cooperative service hook, called between the individual socket reads that make
 * up a body fetch. A single-threaded player installs its audio/video service
 * pump here so it keeps presenting already-decoded frames and feeding audio
 * while an HLS segment downloads, instead of freezing for the whole (blocking)
 * read. Pass NULL to disable (the default; host builds install none). */
/* Return non-zero to interrupt the current body read.  This lets a foreground
 * player honour ESC/Close while the socket itself is idle, instead of waiting
 * for the network timeout before it can begin normal teardown. */
typedef int (*mr_http_service_fn)(void *opaque);
void mr_http_set_service(mr_http_service_fn fn, void *opaque);

#endif /* MR_HTTP_H */
