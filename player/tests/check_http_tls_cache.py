#!/usr/bin/env python3
"""Production TLS cache ownership, LRU, origin isolation and cap-change checks."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
s = (root / 'core/mr_http.c').read_text()
def function(name):
    start = s.index('static void ' + name + '(') if name == 'remember_tls_session' else s.index('void ' + name + '(')
    brace = s.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end] + '\n'
start = s.index('#define HTTP_TLS_SESSION_SLOTS')
cache = s[start:s.index('#endif', start)]
harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "core/mr_http.h"
#define HTTP_HOST_MAX 256
#define MR_HTTP_HAVE_TLS 1
#define OPENSSL_VERSION_NUMBER 0x10101000L
typedef struct { int refs, resumable; } SSL_SESSION;
typedef struct { int unused; } SSL_CTX;
static SSL_CTX *g_ssl_ctx = (SSL_CTX *)1;
static int g_tls_max = MR_HTTP_TLS_AUTO, applied;
static void apply_tls_max(SSL_CTX *ctx) { assert(ctx); applied++; }
static SSL_SESSION *SSL_get1_session(SSL_SESSION *s) { s->refs++; return s; }
static int SSL_SESSION_is_resumable(SSL_SESSION *s) { return s->resumable; }
static void SSL_SESSION_free(SSL_SESSION *s) { assert(s->refs > 0); s->refs--; }
typedef struct {
    SSL_SESSION *ssl;
    int using_tls;
    char tls_host[HTTP_HOST_MAX];
    unsigned short tls_port;
} http_source;
''' + cache + function('remember_tls_session') + function('mr_http_set_tls_max') + r'''
static SSL_SESSION sessions[16];
static void save(unsigned id, const char *host, unsigned short port)
{
    http_source h = {0};
    h.ssl = &sessions[id]; h.using_tls = 1; h.tls_port = port;
    strcpy(h.tls_host, host); remember_tls_session(&h);
}
int main(void)
{
    unsigned i;
    for (i = 0; i < 16; i++) { sessions[i].refs = 1; sessions[i].resumable = 1; }
    save(0, "a", 443); save(1, "b", 443); save(2, "c", 443); save(3, "d", 443);
    assert(find_tls_session("a", 443) == &sessions[0]);
    assert(!find_tls_session("a", 444) && !find_tls_session("other", 443));
    save(4, "a", 443); /* replacement refreshes recency and releases old ref */
    assert(sessions[0].refs == 1 && sessions[4].refs == 2);
    save(5, "e", 443); /* b was least recent; a survives */
    assert(!find_tls_session("b", 443) && sessions[1].refs == 1);
    assert(find_tls_session("a", 443) == &sessions[4]);
    assert(find_tls_session("c", 443) == &sessions[2]);
    save(6, "e", 444); /* distinct port, independent ticket */
    assert(find_tls_session("e", 443) == &sessions[5]);
    assert(find_tls_session("e", 444) == &sessions[6]);
    assert(!find_tls_session("c", 443) && sessions[2].refs == 1);
    sessions[7].resumable = 0; save(7, "e", 443);
    assert(find_tls_session("e", 443) == &sessions[5] && sessions[7].refs == 1);
    clear_tls_sessions(); clear_tls_sessions(); /* idempotent teardown */
    for (i = 0; i < 16; i++) assert(sessions[i].refs == 1);
    save(8, "a", 443); save(9, "b", 443);
    mr_http_set_tls_max(MR_HTTP_TLS_AUTO); /* unchanged cap preserves tickets */
    assert(find_tls_session("a", 443) && !applied);
    mr_http_set_tls_max(MR_HTTP_TLS_12);
    assert(!find_tls_session("a", 443) && !find_tls_session("b", 443) && applied == 1);
    assert(sessions[8].refs == 1 && sessions[9].refs == 1);
    save(10, "a", 443); mr_http_set_tls_max(MR_HTTP_TLS_AUTO);
    assert(!find_tls_session("a", 443) && sessions[10].refs == 1 && applied == 2);
    puts("TLS cache: origin isolation, replacement, LRU eviction, references, cap changes, teardown passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source, binary = Path(tmp) / 'check.c', Path(tmp) / 'check'
    source.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(root),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
