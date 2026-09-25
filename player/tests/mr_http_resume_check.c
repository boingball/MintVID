/*
 * MintVID - host check that consecutive HTTPS fetches to one host resume the
 * previous TLS session instead of repeating the full handshake.
 *
 * Every HLS segment is a new connection. On a real A1200 each full handshake
 * cost ~4.5 s of 68060 time, and none was ever resumed, because the session
 * was saved straight after SSL_connect(): a TLS 1.3 server only sends its
 * resumption ticket after the handshake, so that session had none.
 * tests/run_http_check.sh runs this against its local TLS fixture server
 * (TLS 1.3 on current Python/OpenSSL).
 *
 * Usage: mr_http_resume_check <https-url>
 */
#include "../core/mr_http.h"
#include "../core/mr_alloc.h"
#include "../core/mr_source.h"

#include <stdio.h>

#define FETCHES 4

int main(int argc, char **argv)
{
    mr_http_timing t;
    int i;
    if (argc != 2) {
        fprintf(stderr, "usage: %s <https-url>\n", argv[0]);
        return 2;
    }
    for (i = 0; i < FETCHES; i++) {
        char *buf = NULL;
        size_t len = 0;
        if (!mr_http_fetch_text_direct(argv[1], NULL, &buf, &len,
                                       64u * 1024u * 1024u)) {
            fprintf(stderr, "FAIL: fetch %d: %s\n", i, mr_source_last_error());
            return 1;
        }
        mr_free(buf);
    }
    mr_http_timing_get(&t);
    mr_http_net_shutdown();
    if (t.tls_handshakes != FETCHES || t.tls_resumed != FETCHES - 1) {
        fprintf(stderr, "FAIL: %lu TLS handshakes, %lu resumed "
                "(expected %d, %d)\n", t.tls_handshakes, t.tls_resumed,
                FETCHES, FETCHES - 1);
        return 1;
    }
    printf("mr_http_resume_check: %d fetches, %lu resumed TLS sessions\n",
           FETCHES, t.tls_resumed);
    return 0;
}
