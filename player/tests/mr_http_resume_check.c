/*
 * MintVID - host check that consecutive HTTPS fetches to one host resume the
 * previous TLS session instead of repeating the full handshake.
 *
 * Every HLS segment is a new connection. On a real A1200 each full handshake
 * cost ~4.5 s of 68060 time, and none was ever resumed, because the session
 * was saved straight after SSL_connect(): a TLS 1.3 server only sends its
 * resumption ticket after the handshake, so that session had none.
 * tests/run_http_check.sh runs this against its local TLS fixture server
 * (TLS 1.3 on current Python/OpenSSL). It also checks that
 * mr_http_set_tls_max(MR_HTTP_TLS_12) keeps connections on TLS 1.2 and that
 * they still resume there.
 *
 * Usage: mr_http_resume_check <https-url>
 */
#include "../core/mr_http.h"
#include "../core/mr_alloc.h"
#include "../core/mr_source.h"

#include <stdio.h>

#define FETCHES 4

/* Fetch the URL FETCHES times and check the handshake counters moved as
 * expected: FETCHES handshakes, all but the first resumed. Returns the number
 * of those handshakes that negotiated TLS 1.3, or -1 on failure. */
static int run_phase(const char *name, const char *url)
{
    mr_http_timing before, after;
    unsigned long hs, res;
    int i;
    mr_http_timing_get(&before);
    for (i = 0; i < FETCHES; i++) {
        char *buf = NULL;
        size_t len = 0;
        if (!mr_http_fetch_text_direct(url, NULL, &buf, &len,
                                       64u * 1024u * 1024u)) {
            fprintf(stderr, "FAIL: %s fetch %d: %s\n", name, i,
                    mr_source_last_error());
            return -1;
        }
        mr_free(buf);
    }
    mr_http_timing_get(&after);
    hs = after.tls_handshakes - before.tls_handshakes;
    res = after.tls_resumed - before.tls_resumed;
    if (hs != FETCHES || res != FETCHES - 1) {
        fprintf(stderr, "FAIL: %s: %lu TLS handshakes, %lu resumed "
                "(expected %d, %d)\n", name, hs, res, FETCHES, FETCHES - 1);
        return -1;
    }
    return (int)(after.tls13 - before.tls13);
}

int main(int argc, char **argv)
{
    int v13;
    if (argc != 2) {
        fprintf(stderr, "usage: %s <https-url>\n", argv[0]);
        return 2;
    }
    /* Default: whatever the library and server agree (TLS 1.3 here). */
    v13 = run_phase("auto", argv[1]);
    if (v13 < 0) return 1;
    printf("mr_http_resume_check: auto: %d fetches resumed, %d over TLS 1.3\n",
           FETCHES - 1, v13);
    /* Capping at 1.2 on the live context: the 1.3 session is dropped, so the
     * first connection is a full handshake and the rest resume over 1.2. */
    mr_http_set_tls_max(MR_HTTP_TLS_12);
    v13 = run_phase("tls1.2", argv[1]);
    if (v13 < 0) return 1;
    if (v13 != 0) {
        fprintf(stderr, "FAIL: %d connections used TLS 1.3 with the 1.2 cap\n",
                v13);
        return 1;
    }
    /* And back again. */
    mr_http_set_tls_max(MR_HTTP_TLS_AUTO);
    if (run_phase("auto again", argv[1]) < 0) return 1;
    mr_http_net_shutdown();
    printf("mr_http_resume_check: TLS 1.2 cap: %d fetches resumed, none over "
           "TLS 1.3\n", FETCHES - 1);
    return 0;
}
