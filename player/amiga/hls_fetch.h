/*
 * MintVID - background HLS fetch worker (Amiga).
 *
 * Starts a background process that becomes the sole task touching
 * core/mr_http.c's network state for this session (installed via
 * mr_http_set_fetch_override()/mr_http_set_prefetch_hint(), see that
 * file's design note above connect_socket() for why one owner matters on
 * Amiga - the socket/TLS globals there are process-wide, not per-task).
 * The main player task never opens a connection itself once this is
 * active: every playlist and segment fetch mr_hls.c makes routes through
 * the worker, and several segments of background lookahead (compressed
 * bytes, not decoded frames - see HLS_FETCH_LOOKAHEAD_DEPTH in hls_fetch.c)
 * ride alongside it, fetched one at a time in order as the worker frees up.
 *
 * hls_fetch_start() returns 1 if the worker came up (async fetch active
 * for this session) or 0 if it could not be created, in which case
 * core/mr_http.c behaves exactly as it does today (every fetch runs
 * synchronously on whichever task calls it) and the caller does nothing
 * further - there is no partial/mixed mode.
 */
#ifndef HLS_FETCH_H
#define HLS_FETCH_H

/* Called while this module blocks the main task waiting on the worker
 * (never for longer than one ~20ms poll tick at a time - see hls_fetch.c).
 * Same contract as mrplay.c's own service_player_during_io()/
 * mr_hls_set_wait() hooks: keep audio/video/UI serviced, return non-zero to
 * request an abort of the wait. */
typedef int (*hls_fetch_service_fn)(void *opaque);

int  hls_fetch_start(int verbose);
int  hls_fetch_active(void);
/* Collect completed lookahead and start the next queued segment without
 * blocking playback. Call once per scheduler iteration. */
void hls_fetch_poll(void);

void hls_fetch_set_service(hls_fetch_service_fn fn, void *opaque);

/* Bump the request generation: any reply belonging to an earlier generation
 * is discarded (its buffer freed) instead of being handed to a caller or
 * kept cached. Call this before closing an mr_hls source that might have an
 * in-flight or cached prefetch outstanding - stream switch, live reconnect,
 * stop (hls_fetch_stop() already calls it). Safe even when
 * hls_fetch_active() is false. Never sends anything to the worker itself -
 * see hls_fetch.c's design note on why that is safe with a single in-flight
 * request. */
void hls_fetch_cancel(void);

/* Stop the worker: freeze queued lookahead, cancel (which also signals it
 * in case it is inside a masked DNS lookup), drain the one in-flight reply,
 * tell it to exit, and wait for its acknowledgement. The wait deliberately
 * has no abandon/timeout escape: this worker executes code in mrplay's own
 * loaded segment, so main() must not return and unload that code until the
 * worker is gone. Underlying connect/read/write operations remain bounded;
 * a periodic diagnostic is printed if shutdown takes more than five seconds.
 * The worker releases its own bsdsocket/AmiSSL state before acknowledging.
 * Must be called before the main task's own mr_http_net_shutdown(). Safe to
 * call even if the worker was never started; idempotent. */
void hls_fetch_stop(void);

/* --time diagnostics: fetch hit/miss counts (a "hit" is a request the
 * worker had already finished or was already fetching for the URL asked
 * for; a "miss" needed a fresh blocking round trip) and the worst observed
 * main-task wait for the worker, in ms. */
void hls_fetch_stats(unsigned long *hits, unsigned long *misses,
                     unsigned long *worst_wait_ms);

#endif /* HLS_FETCH_H */
