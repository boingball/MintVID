/*
 * MintVID - streaming HTTP/HTTPS random-access media source.
 *
 * One response remains open while a demuxer reads sequentially. A seek closes
 * it and starts a Range request at the new byte offset. This keeps TS playback
 * to a handful of connections while still allowing AVI/MOV metadata seeks.
 */
#include "mr_http.h"
#include "mr_alloc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if (defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)) || \
    defined(__amigaos__) || defined(__AMIGA__)
#define MR_HTTP_AMIGA 1
#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#else
#define MR_HTTP_AMIGA 0
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#endif

#if MR_HTTP_AMIGA && defined(HAVE_AMISSL)
#define MR_HTTP_HAVE_TLS 1
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#elif !MR_HTTP_AMIGA && defined(MR_HTTP_HAVE_OPENSSL)
#define MR_HTTP_HAVE_TLS 1
#include <openssl/ssl.h>
#include <openssl/err.h>
#else
#define MR_HTTP_HAVE_TLS 0
#endif

#define HTTP_URL_MAX       MR_HTTP_URL_MAX
#define HTTP_HOST_MAX       256
#define HTTP_PATH_MAX       MR_HTTP_PATH_MAX
#define HTTP_HEADER_MAX   16384
#define HTTP_REQUEST_MAX  (MR_HTTP_URL_MAX + MR_HTTP_USER_AGENT_MAX + \
                           MR_HTTP_REFERER_MAX + 1536)
#define HTTP_DEFAULT_USER_AGENT "MintVID/0.1 AmigaOS"
#define HTTP_CHUNK_LINE_MAX 128
/* Read-ahead / rewind window. Presentation-order delivery makes streaming
 * alternate reads between a file's video-chunk and audio-chunk regions, and the
 * proactive drain below keeps this window filled forward so playback runs from
 * RAM instead of blocking on the network. This remains the compatibility
 * default; mr_http_options.source_buffer_bytes can request a larger runtime
 * Fast RAM window on a big-memory machine. */
#ifndef HTTP_CACHE_SIZE
#define HTTP_CACHE_SIZE  (4096UL * 1024)
#endif
/* Minimum forward window kept ahead of the caller by a (blocking) read, enough
 * to span the video/audio chunk lag so interleaved reads never reconnect. The
 * large default suits random-access AVI/MOV, whose audio and video chunks sit
 * far apart in the file. A forward-only HLS TS interleaves audio and video PES
 * within milliseconds, so it needs almost no window - and a smaller one keeps
 * each blocking refill short enough for the decoded-frame queue to cover it. */
#ifndef HTTP_MIN_WINDOW
#define HTTP_MIN_WINDOW  (512UL * 1024)
#endif
#ifndef HTTP_STREAM_MIN_WINDOW
#define HTTP_STREAM_MIN_WINDOW (128UL * 1024)
#endif
/* Keep at least this much already-read data behind the newest request so the
 * trailing (video) read still hits the window after the leading (audio) read
 * has advanced; must exceed the chunk lag. */
#ifndef HTTP_BACK_WINDOW
#define HTTP_BACK_WINDOW (512UL * 1024)
#endif
/* Only compact the cache once this much can be dropped, so the memmove that
 * slides the linear buffer runs rarely (a few times a second at most) instead
 * of per packet - important on 68k where memory bandwidth is scarce. */
#ifndef HTTP_TRIM_MIN
#define HTTP_TRIM_MIN    (512UL * 1024)
#endif
#define HTTP_REDIRECT_MAX     5
#define HTTP_IO_RETRIES        2

#if MR_HTTP_AMIGA
struct Library *SocketBase = NULL;
#if defined(HAVE_AMISSL)
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *AmiSSLExtBase = NULL;
#endif
#endif

#if MR_HTTP_HAVE_TLS && MR_HTTP_AMIGA
/* OpenAmiSSLTags() below uses AmiSSL_InitAmiSSL=TRUE.  Per AmiSSL's
 * CleanupAmiSSLA contract, CloseAmiSSL() therefore performs the matching
 * cleanup itself; calling CleanupAmiSSL() first would clean the same opener
 * twice.  Keep both normal and failed-initialisation teardown on this one
 * path so that lifecycle rule cannot drift between them again. */
static void http_close_amissl(void)
{
    if (AmiSSLBase) CloseAmiSSL();
    AmiSSLBase = NULL;
    AmiSSLExtBase = NULL;
    if (AmiSSLMasterBase) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
    }
}
#endif

#if MR_HTTP_HAVE_TLS
/* The TLS library and client context are initialised once and then reused for
 * every connection this process makes. AmiSSL in particular is costly to open,
 * initialise, clean up and close, and doing that per HLS segment was the bulk
 * of the stall at each segment boundary. They are released once by
 * http_platform_shutdown at process exit. */
static SSL_CTX *g_ssl_ctx = NULL;
static int      g_tls_inited = 0;
/* Cached TLS session from the last successful handshake. Reusing it for the
 * next connection to the same host lets the server grant an abbreviated
 * handshake (no fresh key exchange), which is the bulk of the per-segment TLS
 * cost on 68k. Reuse is best-effort: a server that declines simply does a full
 * handshake, so this only ever speeds things up. */
static SSL_SESSION *g_tls_session = NULL;
static char         g_tls_session_host[HTTP_HOST_MAX];
#endif

typedef struct {
    char host[HTTP_HOST_MAX];
    char path[HTTP_PATH_MAX];
    unsigned short port;
    int tls;
} http_url;

typedef struct {
    char url[HTTP_URL_MAX];
    mr_http_options options;
    int sock;
    int socket_ready;
    int using_tls;
    int platform_ready;
    int tls_ready;
#if MR_HTTP_HAVE_TLS
    SSL_CTX *ssl_ctx;
    SSL     *ssl;
#endif
    size_t total_len;
    size_t body_pos;
    size_t response_left;
    int response_left_known;
    int streaming;              /* forward-only: no length, no range seeking  */
    int bounded_fetch;          /* caller will buffer through chunked EOF      */
    int chunked;
    size_t chunk_left;
    int chunk_need_crlf;
    int chunk_done;
    unsigned char header[HTTP_HEADER_MAX];
    /* Long signed media URLs need correspondingly large parse/request and
     * redirect buffers. Keep them in this AllocVec-backed context rather than
     * as nested automatic arrays: several 4 KiB locals on the classic Shell
     * stack can corrupt AmiSSL before playback even opens. */
    http_url parsed_url;
    char request[HTTP_REQUEST_MAX];
    char redirect_location[HTTP_URL_MAX];
    char redirect_url[HTTP_URL_MAX];
    size_t prefetch_pos;
    size_t prefetch_len;
    unsigned char *cache;
    size_t cache_cap;
    size_t cache_start;
    size_t cache_len;
    size_t max_read;            /* highest byte the demuxer has asked for      */
    int interrupted;            /* cooperative service hook requested abort    */
} http_source;

static int copy_header_value(char *out, size_t cap, const char *value,
                             const char *name)
{
    size_t n;
    char error[96];
    if (!value) value = "";
    {
        const char *end = (const char *)memchr(value, 0, cap);
        if (!end) {
            snprintf(error, sizeof error, "invalid or overlong HTTP %s", name);
            mr_source_set_error(error);
            return 0;
        }
        n = (size_t)(end - value);
    }
    if (memchr(value, '\r', n) || memchr(value, '\n', n)) {
        snprintf(error, sizeof error, "invalid or overlong HTTP %s", name);
        mr_source_set_error(error);
        return 0;
    }
    memcpy(out, value, n + 1);
    return 1;
}

int mr_http_options_init(mr_http_options *options, const char *user_agent,
                         const char *referer)
{
    if (!options) {
        mr_source_set_error("missing HTTP options storage");
        return 0;
    }
    memset(options, 0, sizeof *options);
    return copy_header_value(options->user_agent,
                             sizeof options->user_agent, user_agent,
                             "User-Agent") &&
           copy_header_value(options->referer, sizeof options->referer,
                             referer, "Referer");
}

static int format_option_headers(const http_source *h, char *out, size_t cap)
{
    const char *ua = h->options.user_agent[0]
                   ? h->options.user_agent : HTTP_DEFAULT_USER_AGENT;
    int n;
    if (h->options.referer[0])
        n = snprintf(out, cap, "User-Agent: %s\r\nReferer: %s\r\n",
                     ua, h->options.referer);
    else
        n = snprintf(out, cap, "User-Agent: %s\r\n", ua);
    return n > 0 && (size_t)n < cap;
}

static int ascii_tolower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int ascii_ncasecmp(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = ascii_tolower((unsigned char)a[i]);
        int cb = ascii_tolower((unsigned char)b[i]);
        if (ca != cb || !ca || !cb) return ca - cb;
    }
    return 0;
}

static int starts_nocase(const char *s, const char *prefix)
{
    return ascii_ncasecmp(s, prefix, strlen(prefix)) == 0;
}

static void source_error_status(const char *prefix, int status)
{
    char text[192];
    snprintf(text, sizeof text, "%s (HTTP status %d)", prefix, status);
    mr_source_set_error(text);
}

static int parse_decimal(const char *p, const char **end, size_t *value)
{
    size_t v = 0;
    int have = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');
        if (v > (SIZE_MAX - digit) / 10u) return 0;
        v = v * 10u + digit;
        p++;
        have = 1;
    }
    if (!have) return 0;
    if (end) *end = p;
    *value = v;
    return 1;
}

static int parse_port(const char *p, const char *end, unsigned short *port)
{
    unsigned long v = 0;
    if (p == end) return 0;
    while (p < end) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10u + (unsigned)(*p++ - '0');
        if (v > 65535u) return 0;
    }
    if (!v) return 0;
    *port = (unsigned short)v;
    return 1;
}

static int parse_url(const char *url, http_url *out)
{
    const char *authority, *slash, *host_end, *colon = NULL, *p;
    size_t n;
    memset(out, 0, sizeof *out);
    if (starts_nocase(url, "http://")) {
        out->tls = 0;
        out->port = 80;
        authority = url + 7;
    } else if (starts_nocase(url, "https://")) {
        out->tls = 1;
        out->port = 443;
        authority = url + 8;
    } else {
        return 0;
    }
    slash = strchr(authority, '/');
    host_end = slash ? slash : authority + strlen(authority);
    if (authority == host_end || memchr(authority, '@',
                                         (size_t)(host_end - authority)))
        return 0;
    /* IPv6 literals are deliberately deferred; Amiga TCP/IP stacks in the
     * target range are predominantly IPv4 and gethostbyname-based. */
    if (*authority == '[') return 0;
    for (p = authority; p < host_end; p++)
        if (*p == ':') colon = p;
    if (colon) {
        if (!parse_port(colon + 1, host_end, &out->port)) return 0;
        host_end = colon;
    }
    n = (size_t)(host_end - authority);
    if (!n || n >= sizeof out->host) return 0;
    memcpy(out->host, authority, n);
    out->host[n] = '\0';
    if (slash) {
        n = strlen(slash);
        if (n >= sizeof out->path) return 0;
        memcpy(out->path, slash, n + 1);
    } else {
        strcpy(out->path, "/");
    }
    return 1;
}

static int resolve_redirect(const char *base_url, const char *location,
                            char *out, size_t out_size)
{
    http_url base;
    const char *scheme;
    int n;
    if (starts_nocase(location, "http://") ||
        starts_nocase(location, "https://")) {
        if (strlen(location) >= out_size) return 0;
        strcpy(out, location);
        return 1;
    }
    if (!parse_url(base_url, &base)) return 0;
    scheme = base.tls ? "https" : "http";
    if (location[0] == '/' && location[1] == '/') {
        n = snprintf(out, out_size, "%s:%s", scheme, location);
    } else if (location[0] == '/') {
        int default_port = (!base.tls && base.port == 80) ||
                           (base.tls && base.port == 443);
        n = default_port
          ? snprintf(out, out_size, "%s://%s%s",
                     scheme, base.host, location)
          : snprintf(out, out_size, "%s://%s:%u%s",
                     scheme, base.host, (unsigned)base.port, location);
    } else {
        char directory[HTTP_PATH_MAX];
        char *last;
        int default_port;
        size_t path_len = strlen(base.path);
        if (path_len >= sizeof directory) return 0;
        memcpy(directory, base.path, path_len + 1);
        last = strrchr(directory, '/');
        if (!last) strcpy(directory, "/");
        else last[1] = '\0';
        default_port = (!base.tls && base.port == 80) ||
                       (base.tls && base.port == 443);
        n = default_port
          ? snprintf(out, out_size, "%s://%s%s%s",
                     scheme, base.host, directory, location)
          : snprintf(out, out_size, "%s://%s:%u%s%s",
                     scheme, base.host, (unsigned)base.port,
                     directory, location);
    }
    return n > 0 && (size_t)n < out_size;
}

int mr_http_resolve_url(const char *base_url, const char *rel,
                        char *out, size_t out_size)
{
    return resolve_redirect(base_url, rel, out, out_size);
}

/* Release the process-wide platform/TLS state. Registered with atexit on the
 * first platform_open so the persistent library handles are closed exactly once
 * on a normal exit. */
static void http_platform_shutdown(void)
{
#if MR_HTTP_HAVE_TLS
    if (g_tls_session) { SSL_SESSION_free(g_tls_session); g_tls_session = NULL; }
    if (g_tls_inited) {
        if (g_ssl_ctx) { SSL_CTX_free(g_ssl_ctx); g_ssl_ctx = NULL; }
#if MR_HTTP_AMIGA
        http_close_amissl();
#endif
        g_tls_inited = 0;
    }
#endif
#if MR_HTTP_AMIGA
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
#endif
}

static void http_register_shutdown(void)
{
    static int registered = 0;
    if (!registered) { registered = 1; atexit(http_platform_shutdown); }
}

/* Release the socket/TLS state from the calling task. The shutdown is
 * idempotent and fully guarded, so explicit early-exit cleanup and the atexit
 * copy can safely meet without releasing the libraries twice. */
void mr_http_net_shutdown(void)
{
    http_platform_shutdown();
}

int mr_http_tls_disabled(void)
{
    return 0;
}

/* Cooperative service hook (see mr_http.h). Called between socket reads during a
 * body fetch so a single-threaded caller keeps presenting/servicing. */
static mr_http_service_fn g_http_service;
static void              *g_http_service_opaque;

void mr_http_set_service(mr_http_service_fn fn, void *opaque)
{
    g_http_service = fn;
    g_http_service_opaque = opaque;
}

/* Fetch override + prefetch hint (see mr_http.h). Lets a platform route every
 * complete-body fetch (mr_http_fetch_text()/mr_http_fetch_buffer()/
 * mr_http_post_json()) through a single task instead of whichever caller
 * happens to run first - see the design note above connect_socket() for why
 * that matters on Amiga (this file's socket/TLS state is process-wide, not
 * per-task). */
static mr_http_fetch_override_fn g_fetch_override;
static mr_http_prefetch_hint_fn  g_prefetch_hint;

void mr_http_set_fetch_override(mr_http_fetch_override_fn fn)
{
    g_fetch_override = fn;
}

int mr_http_fetch_override_active(void)
{
    return g_fetch_override != NULL;
}

void mr_http_set_prefetch_hint(mr_http_prefetch_hint_fn fn)
{
    g_prefetch_hint = fn;
}

void mr_http_prefetch_hint(const char *url, const mr_http_options *options)
{
    if (g_prefetch_hint) g_prefetch_hint(url, options);
}

static int platform_open(http_source *h)
{
    http_register_shutdown();
#if MR_HTTP_AMIGA
    if (!SocketBase)
        SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase) {
        mr_source_set_error("bsdsocket.library v4 is required for HTTP");
        return 0;
    }
#else
    (void)h;
#endif
    h->platform_ready = 1;
    return 1;
}

#if MR_HTTP_HAVE_TLS
static int tls_open(http_source *h)
{
    const SSL_METHOD *method;
    if (h->tls_ready) return 1;
    /* After the first HTTPS connection the library and context are already up:
     * adopt them and skip the whole AmiSSL open/init and context creation, which
     * is what removes the per-segment stall. */
    if (g_tls_inited && g_ssl_ctx) {
        h->ssl_ctx = g_ssl_ctx;
        h->tls_ready = 1;
        return 1;
    }
#if MR_HTTP_AMIGA
    AmiSSLMasterBase =
        OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        mr_source_set_error("AmiSSL v5 is required for HTTPS");
        return 0;
    }
    if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                       AmiSSL_InitAmiSSL, TRUE,
                       AmiSSL_UsesOpenSSLStructs, TRUE,
                       AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                       AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                       AmiSSL_SocketBase, (ULONG)SocketBase,
                       AmiSSL_ErrNoPtr, (ULONG)&errno,
                       TAG_DONE) != 0) {
        mr_source_set_error("cannot initialise AmiSSL");
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
        return 0;
    }
#else
    SSL_library_init();
    SSL_load_error_strings();
#endif
    method = SSLv23_client_method();
    if (!method || !(g_ssl_ctx = SSL_CTX_new(method))) {
        mr_source_set_error("cannot create TLS context");
        goto fail;
    }
#ifdef MR_HTTP_SSL_VERIFY_PEER
    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(g_ssl_ctx) != 1) {
        mr_source_set_error("cannot load TLS root certificates");
        goto fail;
    }
#else
    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, NULL);
#endif
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(g_ssl_ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
    SSL_CTX_set_session_cache_mode(g_ssl_ctx, SSL_SESS_CACHE_CLIENT);
    g_tls_inited = 1;
    h->ssl_ctx = g_ssl_ctx;
    h->tls_ready = 1;
    return 1;

fail:
    /* One-time init failed after (some of) the library came up: unwind so a
     * later attempt starts clean rather than reusing a half-built context. */
    if (g_ssl_ctx) { SSL_CTX_free(g_ssl_ctx); g_ssl_ctx = NULL; }
#if MR_HTTP_AMIGA
    http_close_amissl();
#endif
    return 0;
}
#endif

static void close_socket_only(http_source *h)
{
    if (!h->socket_ready) return;
#if MR_HTTP_AMIGA
    CloseSocket(h->sock);
#else
    close(h->sock);
#endif
    h->sock = -1;
    h->socket_ready = 0;
}

static void close_connection(http_source *h, int healthy)
{
#if MR_HTTP_HAVE_TLS
    if (h->ssl) {
#if MR_HTTP_AMIGA
        if (!healthy) {
            /* MintAMP's real-hardware soak tests found SSL_free unsafe after
             * some peer-drop/SYSCALL paths. Quarantine that one object and
             * let process exit reclaim it instead of risking a hard lock. */
            h->ssl = NULL;
        } else
#endif
        {
            BIO *rbio, *wbio;
            SSL_set_shutdown(h->ssl, SSL_SENT_SHUTDOWN);
            rbio = SSL_get_rbio(h->ssl);
            wbio = SSL_get_wbio(h->ssl);
            if (rbio) BIO_set_close(rbio, BIO_NOCLOSE);
            if (wbio && wbio != rbio) BIO_set_close(wbio, BIO_NOCLOSE);
            SSL_free(h->ssl);
            h->ssl = NULL;
        }
    }
#else
    (void)healthy;
#endif
    close_socket_only(h);
    h->using_tls = 0;
    h->response_left_known = 0;
    h->chunked = 0;
    h->chunk_left = 0;
    h->chunk_need_crlf = 0;
    h->chunk_done = 0;
    h->prefetch_pos = h->prefetch_len = 0;
}

/* connect() blocks until the underlying stack's own TCP timeout - which can
 * run far longer than the 20s SO_RCVTIMEO/SO_SNDTIMEO below cover, since
 * those only bound recv()/send() once a connection exists, not the connect
 * itself. Make the socket non-blocking and re-issue connect() on a short
 * poll rather than block on it once: this mirrors
 * vendor/MintAMP/radio_stream.c's radio_wait_connected(), which documents
 * that a single connect()+select()-for-writability is not reliably
 * portable across every AmigaOS TCP stack (AmiTCP/Miami/Roadshow), and that
 * success shows up as either connect()==0 immediately or errno==EISCONN on
 * a later retry. FIONBIO's value is hard-coded rather than pulled from
 * <sys/ioctl.h>, whose presence varies across the m68k netinclude - same
 * reasoning as that file's own local fallback. */
#if MR_HTTP_AMIGA
#ifndef FIONBIO
#define FIONBIO 0x8004667EUL
#endif
#ifndef EISCONN
#define EISCONN 56
#endif
#endif
#define HTTP_CONNECT_TIMEOUT_MS 20000  /* matches SO_RCVTIMEO/SO_SNDTIMEO   */
#define HTTP_CONNECT_POLL_MS    40

static void connect_poll_sleep(void)
{
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = HTTP_CONNECT_POLL_MS * 1000;
#if MR_HTTP_AMIGA
    WaitSelect(0, NULL, NULL, NULL, &tv, NULL);
#else
    select(0, NULL, NULL, NULL, &tv);
#endif
}

/* bsdsocket.library reports the last socket-call error through Errno(), not
 * through libc's plain errno - this codebase never wires SocketBaseTagList's
 * SBTC_ERRNOPTR to redirect it, so plain errno after a raw socket call is
 * unreliable on real Amiga stacks (AmiTCP/Miami/Roadshow) even though it
 * happens to work on the host. Matches
 * vendor/MintAMP/radio_stream.c's radio_sock_errno(). */
static long connect_last_errno(void)
{
#if MR_HTTP_AMIGA
    return Errno();
#else
    return errno;
#endif
}

static int connect_with_timeout(int sock, const struct sockaddr *addr,
                                socklen_t addrlen)
{
    int tries, budget = HTTP_CONNECT_TIMEOUT_MS / HTTP_CONNECT_POLL_MS;
    int ok = 0;
#if MR_HTTP_AMIGA
    ULONG nb = 1, blk = 0;
    IoctlSocket(sock, FIONBIO, (char *)&nb);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
    for (tries = 0; tries < budget; tries++) {
        int cr = connect(sock, addr, addrlen);
        if (cr == 0 || connect_last_errno() == EISCONN) { ok = 1; break; }
        if (g_http_service && g_http_service(g_http_service_opaque))
            break;                              /* caller asked to abort        */
        connect_poll_sleep();
    }
    /* net_read_some()/net_write_all() assume a blocking socket governed by
     * SO_RCVTIMEO/SO_SNDTIMEO - restore that before returning, success or
     * not, so a non-blocking EWOULDBLOCK from this phase can never leak into
     * the rest of this file's send()/recv() calls. */
#if MR_HTTP_AMIGA
    IoctlSocket(sock, FIONBIO, (char *)&blk);
#else
    fcntl(sock, F_SETFL, flags);
#endif
    return ok;
}

static int connect_socket(http_source *h, const http_url *url)
{
    struct hostent *he;
    struct sockaddr_in sa;
    struct timeval timeout;
    /* gethostbyname() is a genuinely blocking bsdsocket.library call with no
     * non-blocking mode of its own, unlike connect() above - a stuck/slow
     * resolver can leave the calling task sitting inside it far longer than
     * this file's other timeouts, with no way for another task to unstick
     * it. SBTC_BREAKMASK tells bsdsocket.library which signals should abort
     * a blocking call early for the calling task; hls_fetch.c's
     * hls_fetch_cancel()/hls_fetch_kick() send SIGBREAKF_CTRL_C to unstick
     * exactly this call when a fetch is cancelled or the worker needs to
     * stop. Mirrors vendor/MintAMP/radio_stream.c's proven pattern there
     * (same comment, same reasoning) exactly:
     *
     * Scoped tightly to just this one call, cleared again immediately below
     * win or lose - leaving it set would also apply to every later blocking
     * bsdsocket call this task makes, including whatever AmiSSL does
     * internally inside SSL_connect()/SSL_read(), which MintAMP found
     * sensitive to exactly this kind of external signal interference (an
     * unexpected abort deep inside a library that never expected one is a
     * plausible way to corrupt its state rather than cleanly cancel it).
     *
     * The mask alone only says which signal would abort this call - nothing
     * sends SIGBREAKF_CTRL_C otherwise, so this is inert unless a caller
     * explicitly kicks it. Clear any stale pending CTRL_C first: a signal
     * left over from an earlier, unrelated cancel would otherwise sit
     * pending and immediately abort *this* call the moment the mask goes
     * live. */
#if MR_HTTP_AMIGA
    SetSignal(0, SIGBREAKF_CTRL_C);
    SocketBaseTags(SBTM_SETVAL(SBTC_BREAKMASK), (ULONG)SIGBREAKF_CTRL_C,
                   TAG_DONE);
    he = gethostbyname(url->host);
    SocketBaseTags(SBTM_SETVAL(SBTC_BREAKMASK), 0UL, TAG_DONE);
#else
    he = gethostbyname(url->host);
#endif
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        mr_source_set_error("HTTP DNS lookup failed");
        return 0;
    }
    h->sock = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (h->sock < 0) {
        mr_source_set_error("cannot create HTTP socket");
        return 0;
    }
    h->socket_ready = 1;
    timeout.tv_sec = 20;
    timeout.tv_usec = 0;
    setsockopt(h->sock, SOL_SOCKET, SO_RCVTIMEO,
               (const char *)&timeout, sizeof timeout);
    setsockopt(h->sock, SOL_SOCKET, SO_SNDTIMEO,
               (const char *)&timeout, sizeof timeout);
    /* Ask the stack to buffer a big chunk of stream while the player is off
     * decoding/pacing, so the proactive drain has plenty to pull in without
     * blocking. Best-effort: the stack may clamp it. */
    { int rcv = 1 << 20;
      setsockopt(h->sock, SOL_SOCKET, SO_RCVBUF,
                 (const char *)&rcv, sizeof rcv); }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(url->port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    if (!connect_with_timeout(h->sock, (struct sockaddr *)&sa, sizeof sa)) {
        mr_source_set_error("HTTP connection failed");
        close_socket_only(h);
        return 0;
    }
    if (url->tls) {
#if MR_HTTP_HAVE_TLS
        if (!tls_open(h)) {
            close_socket_only(h);
            return 0;
        }
        h->ssl = SSL_new(h->ssl_ctx);
        if (!h->ssl) {
            mr_source_set_error("cannot create TLS session");
            close_socket_only(h);
            return 0;
        }
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
        SSL_set_tlsext_host_name(h->ssl, url->host);
#endif
#ifdef MR_HTTP_SSL_VERIFY_PEER
        {
            X509_VERIFY_PARAM *param = SSL_get0_param(h->ssl);
            if (param) X509_VERIFY_PARAM_set1_host(param, url->host, 0);
        }
#endif
        /* Offer the previous session for this host so the server can resume it
         * with an abbreviated handshake. */
        if (g_tls_session && !strcmp(g_tls_session_host, url->host))
            SSL_set_session(h->ssl, g_tls_session);
        /* SSL_connect() has no cancellation hook of its own, unlike
         * gethostbyname() above - deliberately: MintAMP's own hard-won
         * finding (see the comment above connect_socket()'s gethostbyname()
         * call) is that signal-interrupting AmiSSL's internal blocking
         * calls risks corrupting its state rather than cleanly cancelling
         * it, since AmiSSL was never written to expect an external abort
         * mid-call. It is still bounded, just less precisely: h->sock
         * already has SO_RCVTIMEO/SO_SNDTIMEO (20s) set above, and
         * SSL_connect()'s own internal reads/writes go through that same
         * fd, so a full stuck handshake costs at most a handful of 20s
         * increments, not an unbounded wait. */
        if (SSL_set_fd(h->ssl, h->sock) != 1 ||
            SSL_connect(h->ssl) != 1) {
            mr_source_set_error("HTTPS TLS handshake failed");
            close_connection(h, 0);
            return 0;
        }
        h->using_tls = 1;
        /* Remember this handshake's session for the next same-host connection. */
        {
            SSL_SESSION *sess = SSL_get1_session(h->ssl);
            if (sess) {
                if (g_tls_session) SSL_SESSION_free(g_tls_session);
                g_tls_session = sess;
                strncpy(g_tls_session_host, url->host,
                        sizeof g_tls_session_host - 1);
                g_tls_session_host[sizeof g_tls_session_host - 1] = 0;
            }
        }
#else
        mr_source_set_error(
            "HTTPS support was not compiled in; rebuild with SSL=1");
        close_socket_only(h);
        return 0;
#endif
    }
    return 1;
}

static int net_write_all(http_source *h, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    while (len) {
        int n;
#if MR_HTTP_HAVE_TLS
        if (h->using_tls)
            n = SSL_write(h->ssl, p, len > 0x7fffffffUL
                                      ? 0x7fffffff : (int)len);
        else
#endif
            n = (int)send(h->sock, (const char *)p,
                          len > 0x7fffffffUL ? 0x7fffffff : (int)len, 0);
        if (n <= 0) return 0;
        p += n;
        len -= (size_t)n;
    }
    return 1;
}

static int net_read_some(http_source *h, void *buf, size_t len)
{
    int n;
    int amount = len > 0x7fffffffUL ? 0x7fffffff : (int)len;
    /* SO_RCVTIMEO alone leaves the foreground task inside recv()/SSL_read()
     * for as long as twenty seconds.  Slice that idle wait when a player
     * service hook is installed: Intuition messages, Paula and queued video
     * then keep moving even when the CDN temporarily sends no bytes. */
    if (g_http_service) {
        int slices;
        for (slices = 0; slices < 200; slices++) {
            fd_set rf;
            struct timeval tv;
            int ready;
#if MR_HTTP_HAVE_TLS
            if (h->using_tls && h->ssl && SSL_pending(h->ssl) > 0) break;
#endif
            if (g_http_service(g_http_service_opaque)) {
                h->interrupted = 1;
                mr_source_set_error("HTTP read interrupted");
                return 0;
            }
            FD_ZERO(&rf);
            FD_SET(h->sock, &rf);
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
#if MR_HTTP_AMIGA
            ready = WaitSelect(h->sock + 1, &rf, NULL, NULL, &tv, NULL);
#else
            ready = select(h->sock + 1, &rf, NULL, NULL, &tv);
#endif
            if (ready > 0) break;
            if (ready < 0) return 0;
        }
        if (slices == 200) {
            mr_source_set_error("HTTP read timed out");
            return 0;
        }
    }
#if MR_HTTP_HAVE_TLS
    if (h->using_tls)
        n = SSL_read(h->ssl, buf, amount);
    else
#endif
        n = (int)recv(h->sock, (char *)buf, amount, 0);
    return n;
}

static int raw_read_some(http_source *h, void *buf, size_t len)
{
    if (h->prefetch_pos < h->prefetch_len) {
        size_t n = h->prefetch_len - h->prefetch_pos;
        if (n > len) n = len;
        memcpy(buf, h->header + h->prefetch_pos, n);
        h->prefetch_pos += n;
        return (int)n;
    }
    return net_read_some(h, buf, len);
}

static int raw_read_exact(http_source *h, void *buf, size_t len)
{
    unsigned char *out = (unsigned char *)buf;
    size_t done = 0;
    while (done < len) {
        int n = raw_read_some(h, out + done, len - done);
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    return 1;
}

static int raw_read_line(http_source *h, char *line, size_t line_size)
{
    size_t used = 0;
    if (!line_size) return 0;
    while (used + 1 < line_size) {
        unsigned char c;
        if (!raw_read_exact(h, &c, 1)) return 0;
        if (c == '\n') {
            if (used && line[used - 1] == '\r') used--;
            line[used] = '\0';
            return 1;
        }
        line[used++] = (char)c;
    }
    line[0] = '\0';
    mr_source_set_error("HTTP chunk header is too large");
    return 0;
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_chunk_size(const char *line, size_t *size)
{
    size_t value = 0;
    int have = 0;
    while (*line == ' ' || *line == '\t') line++;
    while (*line) {
        int digit = hex_value((unsigned char)*line);
        if (digit < 0) break;
        if (value > (SIZE_MAX - (unsigned)digit) / 16u) return 0;
        value = value * 16u + (unsigned)digit;
        line++;
        have = 1;
    }
    if (!have) return 0;
    while (*line == ' ' || *line == '\t') line++;
    if (*line && *line != ';') return 0;
    *size = value;
    return 1;
}

static int begin_chunk(http_source *h)
{
    char line[HTTP_CHUNK_LINE_MAX];
    if (h->chunk_done) return 0;
    if (h->chunk_need_crlf) {
        unsigned char crlf[2];
        if (!raw_read_exact(h, crlf, sizeof crlf) ||
            crlf[0] != '\r' || crlf[1] != '\n') {
            mr_source_set_error("invalid HTTP chunk delimiter");
            return 0;
        }
        h->chunk_need_crlf = 0;
    }
    if (!raw_read_line(h, line, sizeof line) ||
        !parse_chunk_size(line, &h->chunk_left)) {
        mr_source_set_error("invalid HTTP chunk size");
        return 0;
    }
    if (!h->chunk_left) {
        size_t trailer_bytes = 0;
        do {
            if (!raw_read_line(h, line, sizeof line)) return 0;
            trailer_bytes += strlen(line) + 2;
            if (trailer_bytes > HTTP_HEADER_MAX) {
                mr_source_set_error("HTTP chunk trailers are too large");
                return 0;
            }
        } while (line[0]);
        h->chunk_done = 1;
        return 0;
    }
    if (h->response_left_known && h->chunk_left > h->response_left) {
        mr_source_set_error("HTTP chunk exceeds response length");
        return 0;
    }
    return 1;
}

static int find_header_end(const unsigned char *buf, size_t len)
{
    size_t i;
    for (i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return (int)(i + 4);
    return -1;
}

static int header_value(const unsigned char *headers, size_t header_len,
                        const char *name, char *out, size_t out_size)
{
    size_t name_len = strlen(name);
    size_t pos = 0;
    while (pos < header_len) {
        size_t start = pos, end;
        while (pos < header_len && headers[pos] != '\n') pos++;
        end = pos;
        if (end > start && headers[end - 1] == '\r') end--;
        if (end - start > name_len &&
            ascii_ncasecmp((const char *)headers + start, name, name_len) == 0 &&
            headers[start + name_len] == ':') {
            size_t value = start + name_len + 1;
            size_t n;
            while (value < end &&
                   (headers[value] == ' ' || headers[value] == '\t'))
                value++;
            n = end - value;
            if (n >= out_size) n = out_size - 1;
            memcpy(out, headers + value, n);
            out[n] = '\0';
            return 1;
        }
        if (pos < header_len) pos++;
    }
    if (out_size) out[0] = '\0';
    return 0;
}

static int contains_nocase(const char *text, const char *needle)
{
    size_t n = strlen(needle);
    while (*text) {
        if (ascii_ncasecmp(text, needle, n) == 0) return 1;
        text++;
    }
    return 0;
}

static int response_status(const unsigned char *headers, size_t len)
{
    const char *p = (const char *)headers;
    const char *end = p + len;
    if (len < 12 || ascii_ncasecmp(p, "HTTP/", 5) != 0) return 0;
    while (p < end && *p != ' ') p++;
    while (p < end && *p == ' ') p++;
    if (p + 3 > end || p[0] < '0' || p[0] > '9' ||
        p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9')
        return 0;
    return (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
}

static int parse_content_range(const char *value, size_t *start,
                               size_t *last, size_t *total)
{
    const char *p = value, *end;
    while (*p == ' ' || *p == '\t') p++;
    if (ascii_ncasecmp(p, "bytes", 5) != 0) return 0;
    p += 5;
    while (*p == ' ' || *p == '\t') p++;
    if (!parse_decimal(p, &end, start) || *end != '-') return 0;
    p = end + 1;
    if (!parse_decimal(p, &end, last) || *end != '/') return 0;
    p = end + 1;
    if (*p == '*' || !parse_decimal(p, &end, total)) return 0;
    while (*end == ' ' || *end == '\t') end++;
    return !*end && *last >= *start && *last < *total;
}

static int read_headers(http_source *h, size_t *header_len)
{
    size_t used = 0;
    int end = -1;
    while (used < sizeof h->header) {
        int n = net_read_some(h, h->header + used, sizeof h->header - used);
        if (n <= 0) {
            mr_source_set_error("HTTP server closed before response headers");
            return 0;
        }
        used += (size_t)n;
        end = find_header_end(h->header, used);
        if (end >= 0) break;
    }
    if (end < 0) {
        mr_source_set_error("HTTP response headers are too large");
        return 0;
    }
    *header_len = (size_t)end;
    h->prefetch_pos = (size_t)end;
    h->prefetch_len = used;
    return 1;
}

static int probe_request_length(http_source *h, const char *method,
                                int one_byte_range, size_t *total_out)
{
    int redirects;
    for (redirects = 0; redirects <= HTTP_REDIRECT_MAX; redirects++) {
        http_url *url = &h->parsed_url;
        char option_headers[MR_HTTP_USER_AGENT_MAX + MR_HTTP_REFERER_MAX + 40];
        char host_header[HTTP_HOST_MAX + 16];
        char content_length[64], content_range[128];
        size_t header_len, length = 0;
        size_t range_start = 0, range_last = 0, total = 0;
        int status, n, default_port;

        if (!parse_url(h->url, url)) return 0;
        close_connection(h, 1);
        if (!connect_socket(h, url)) return 0;
        default_port = (!url->tls && url->port == 80) ||
                       (url->tls && url->port == 443);
        if (default_port)
            snprintf(host_header, sizeof host_header, "%s", url->host);
        else
            snprintf(host_header, sizeof host_header, "%s:%u",
                     url->host, (unsigned)url->port);
        if (!format_option_headers(h, option_headers, sizeof option_headers)) {
            mr_source_set_error("HTTP option headers are too large");
            close_connection(h, 1);
            return 0;
        }
        n = snprintf(h->request, sizeof h->request,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "%s"
                     "Accept: */*\r\n"
                     "Accept-Encoding: identity\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "\r\n",
                     method, url->path, host_header, option_headers,
                     one_byte_range ? "Range: bytes=0-0\r\n" : "");
        if (n <= 0 || (size_t)n >= sizeof h->request ||
            !net_write_all(h, h->request, (size_t)n) ||
            !read_headers(h, &header_len)) {
            close_connection(h, 0);
            return 0;
        }
        status = response_status(h->header, header_len);
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            if (redirects == HTTP_REDIRECT_MAX ||
                !header_value(h->header, header_len, "Location",
                              h->redirect_location,
                              sizeof h->redirect_location) ||
                !resolve_redirect(h->url, h->redirect_location,
                                  h->redirect_url,
                                  sizeof h->redirect_url)) {
                close_connection(h, 1);
                return 0;
            }
            strcpy(h->url, h->redirect_url);
            close_connection(h, 1);
            continue;
        }
        if (status == 206 &&
            header_value(h->header, header_len, "Content-Range",
                         content_range, sizeof content_range) &&
            parse_content_range(content_range, &range_start,
                                &range_last, &total) &&
            range_start == 0 && total) {
            close_connection(h, 1);
            *total_out = total;
            return 1;
        }
        if (status == 200 &&
            header_value(h->header, header_len, "Content-Length",
                         content_length, sizeof content_length)) {
            const char *end;
            if (parse_decimal(content_length, &end, &length)) {
                while (*end == ' ' || *end == '\t') end++;
                if (!*end && length) {
                    close_connection(h, 1);
                    *total_out = length;
                    return 1;
                }
            }
        }
        close_connection(h, 1);
        return 0;
    }
    return 0;
}

static int probe_total_length(http_source *h)
{
    size_t total = 0;
    if (probe_request_length(h, "HEAD", 0, &total) ||
        probe_request_length(h, "GET", 1, &total)) {
        h->total_len = total;
        return 1;
    }
    mr_source_set_error("chunked HTTP media omitted a seekable file length");
    return 0;
}

static int begin_response(http_source *h, size_t offset)
{
    int redirects;
    for (redirects = 0; redirects <= HTTP_REDIRECT_MAX; redirects++) {
        http_url *url = &h->parsed_url;
        char option_headers[MR_HTTP_USER_AGENT_MAX + MR_HTTP_REFERER_MAX + 40];
        char host_header[HTTP_HOST_MAX + 16];
        char range_header[64];
        char content_length[64], content_range[128], transfer[64];
        size_t header_len, response_len = 0, content_length_value = 0;
        size_t range_start = 0, range_last = 0, total = 0;
        int status, n, default_port, have_content_length, chunked;
        int bounded_unknown = 0;

        if (!parse_url(h->url, url)) {
            mr_source_set_error("invalid HTTP/HTTPS URL");
            return 0;
        }
        close_connection(h, 1);
        if (!connect_socket(h, url)) return 0;

        default_port = (!url->tls && url->port == 80) ||
                       (url->tls && url->port == 443);
        if (default_port)
            snprintf(host_header, sizeof host_header, "%s", url->host);
        else
            snprintf(host_header, sizeof host_header, "%s:%u",
                     url->host, (unsigned)url->port);
        if (!format_option_headers(h, option_headers, sizeof option_headers)) {
            mr_source_set_error("HTTP option headers are too large");
            close_connection(h, 1);
            return 0;
        }
        range_header[0] = '\0';
        if (offset)
            snprintf(range_header, sizeof range_header,
                     "Range: bytes=%lu-\r\n", (unsigned long)offset);
        n = snprintf(h->request, sizeof h->request,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "%s"
                     "Accept: */*\r\n"
                     "Accept-Encoding: identity\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "\r\n",
                     url->path, host_header, option_headers, range_header);
        if (n <= 0 || (size_t)n >= sizeof h->request) {
            mr_source_set_error("HTTP request is too large");
            close_connection(h, 1);
            return 0;
        }
        if (!net_write_all(h, h->request, (size_t)n) ||
            !read_headers(h, &header_len)) {
            close_connection(h, 0);
            return 0;
        }
        status = response_status(h->header, header_len);
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            if (redirects == HTTP_REDIRECT_MAX) {
                mr_source_set_error("too many HTTP redirects");
                close_connection(h, 1);
                return 0;
            }
            if (!header_value(h->header, header_len, "Location",
                              h->redirect_location,
                              sizeof h->redirect_location) ||
                !resolve_redirect(h->url, h->redirect_location,
                                  h->redirect_url,
                                  sizeof h->redirect_url)) {
                mr_source_set_error("invalid HTTP redirect");
                close_connection(h, 1);
                return 0;
            }
            strcpy(h->url, h->redirect_url);
            close_connection(h, 1);
            continue;
        }
        if (status != 200 && status != 206) {
            source_error_status("HTTP media request failed", status);
            close_connection(h, 1);
            return 0;
        }
        if (offset && status != 206) {
            mr_source_set_error(
                "HTTP server does not support byte-range seeking");
            close_connection(h, 1);
            return 0;
        }

        chunked = header_value(h->header, header_len, "Transfer-Encoding",
                               transfer, sizeof transfer) &&
                  contains_nocase(transfer, "chunked");
        have_content_length = header_value(h->header, header_len,
                                           "Content-Length",
                                           content_length,
                                           sizeof content_length);
        if (have_content_length) {
            const char *end;
            if (!parse_decimal(content_length, &end, &response_len)) {
                mr_source_set_error("invalid HTTP Content-Length");
                close_connection(h, 1);
                return 0;
            }
            while (*end == ' ' || *end == '\t') end++;
            if (*end) {
                mr_source_set_error("invalid HTTP Content-Length");
                close_connection(h, 1);
                return 0;
            }
            content_length_value = response_len;
        }

        if (status == 206) {
            if (!header_value(h->header, header_len, "Content-Range",
                              content_range, sizeof content_range) ||
                !parse_content_range(content_range, &range_start,
                                     &range_last, &total) ||
                range_start != offset || !total) {
                mr_source_set_error("invalid HTTP Content-Range");
                close_connection(h, 1);
                return 0;
            }
            response_len = range_last - range_start + 1;
            if (!chunked && have_content_length &&
                response_len != content_length_value) {
                mr_source_set_error("HTTP range length mismatch");
                close_connection(h, 1);
                return 0;
            }
        } else {
            if (!have_content_length) {
                if (chunked && h->total_len) {
                    total = h->total_len;
                    response_len = total;
                } else if (chunked && h->bounded_fetch) {
                    /* A complete-response caller can read through the final
                     * zero chunk and learn the length itself; avoid extra HEAD
                     * and Range probes which this CDN may not support. */
                    bounded_unknown = 1;
                } else if (chunked && !h->streaming) {
                    /* No length in the response. Try to discover one (HEAD or a
                     * range probe) so the media stays seekable; if the server
                     * offers neither, fall back to forward-only streaming. */
                    close_connection(h, 1);
                    if (!probe_total_length(h))
                        h->streaming = 1;
                    return begin_response(h, offset);
                } else if (chunked) {
                    /* Streaming re-entry: length stays unknown and EOF is
                     * signalled by the terminating zero-size chunk. */
                    total = 0;
                    response_len = 0;
                } else {
                    mr_source_set_error(
                        "HTTP media requires Content-Length or Content-Range");
                    close_connection(h, 1);
                    return 0;
                }
            } else {
                total = response_len;
            }
        }
        if (h->streaming || bounded_unknown) {
            /* A length-less stream can only be read from the start; a nonzero
             * offset would mean a seek the server cannot honour. */
            if (offset != 0) {
                mr_source_set_error("cannot seek a length-less HTTP stream");
                close_connection(h, 1);
                return 0;
            }
            h->total_len = 0;
            h->body_pos = 0;
            h->response_left_known = 0;
            h->response_left = 0;
            h->chunked = 1;
            h->chunk_left = 0;
            h->chunk_need_crlf = 0;
            h->chunk_done = 0;
            return 1;
        }
        if (!total || !response_len) {
            mr_source_set_error(
                "HTTP media requires Content-Length or Content-Range");
            close_connection(h, 1);
            return 0;
        }
        if (h->total_len && h->total_len != total) {
            mr_source_set_error("HTTP media length changed during playback");
            close_connection(h, 1);
            return 0;
        }
        h->total_len = total;
        h->body_pos = offset;
        h->response_left_known = 1;
        h->response_left = response_len;
        h->chunked = chunked;
        h->chunk_left = 0;
        h->chunk_need_crlf = 0;
        h->chunk_done = 0;
        return 1;
    }
    return 0;
}

static int begin_json_post_response(http_source *h, const char *json)
{
    int redirects;
    size_t json_len = strlen(json);
    if (json_len > 1024) {
        mr_source_set_error("HTTP JSON request body is too large");
        return 0;
    }
    for (redirects = 0; redirects <= HTTP_REDIRECT_MAX; redirects++) {
        http_url *url = &h->parsed_url;
        char option_headers[MR_HTTP_USER_AGENT_MAX + MR_HTTP_REFERER_MAX + 40];
        char host_header[HTTP_HOST_MAX + 16];
        char content_length[64], transfer[64];
        size_t header_len, response_len = 0;
        int status, n, default_port, chunked, have_content_length;

        if (!parse_url(h->url, url)) {
            mr_source_set_error("invalid HTTP/HTTPS URL");
            return 0;
        }
        close_connection(h, 1);
        if (!connect_socket(h, url)) return 0;
        default_port = (!url->tls && url->port == 80) ||
                       (url->tls && url->port == 443);
        if (default_port)
            snprintf(host_header, sizeof host_header, "%s", url->host);
        else
            snprintf(host_header, sizeof host_header, "%s:%u",
                     url->host, (unsigned)url->port);
        if (!format_option_headers(h, option_headers, sizeof option_headers)) {
            mr_source_set_error("HTTP option headers are too large");
            close_connection(h, 1);
            return 0;
        }
        n = snprintf(h->request, sizeof h->request,
                     "POST %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "%s"
                     "Content-Type: application/json\r\n"
                     "Accept: application/json\r\n"
                     "Accept-Encoding: identity\r\n"
                     "Content-Length: %lu\r\n"
                     "Connection: close\r\n"
                     "\r\n%s",
                     url->path, host_header, option_headers,
                     (unsigned long)json_len, json);
        if (n <= 0 || (size_t)n >= sizeof h->request) {
            mr_source_set_error("HTTP JSON request is too large");
            close_connection(h, 1);
            return 0;
        }
        if (!net_write_all(h, h->request, (size_t)n) ||
            !read_headers(h, &header_len)) {
            close_connection(h, 0);
            return 0;
        }
        status = response_status(h->header, header_len);
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            if (redirects == HTTP_REDIRECT_MAX ||
                !header_value(h->header, header_len, "Location",
                              h->redirect_location,
                              sizeof h->redirect_location) ||
                !resolve_redirect(h->url, h->redirect_location,
                                  h->redirect_url,
                                  sizeof h->redirect_url)) {
                mr_source_set_error("invalid HTTP redirect");
                close_connection(h, 1);
                return 0;
            }
            strcpy(h->url, h->redirect_url);
            close_connection(h, 1);
            continue;
        }
        if (status != 200) {
            source_error_status("HTTP JSON request failed", status);
            close_connection(h, 1);
            return 0;
        }
        chunked = header_value(h->header, header_len, "Transfer-Encoding",
                               transfer, sizeof transfer) &&
                  contains_nocase(transfer, "chunked");
        have_content_length = header_value(h->header, header_len,
                                           "Content-Length",
                                           content_length,
                                           sizeof content_length);
        if (have_content_length) {
            const char *end;
            if (!parse_decimal(content_length, &end, &response_len)) {
                mr_source_set_error("invalid HTTP Content-Length");
                close_connection(h, 1);
                return 0;
            }
            while (*end == ' ' || *end == '\t') end++;
            if (*end) {
                mr_source_set_error("invalid HTTP Content-Length");
                close_connection(h, 1);
                return 0;
            }
        }
        h->body_pos = 0;
        h->response_left_known = have_content_length;
        h->response_left = have_content_length ? response_len : 0;
        h->streaming = !have_content_length;
        h->chunked = chunked;
        h->chunk_left = 0;
        h->chunk_need_crlf = 0;
        h->chunk_done = 0;
        return 1;
    }
    return 0;
}

static int copy_response_bytes(http_source *h, unsigned char *dst, size_t len)
{
    size_t done = 0;
    if (h->interrupted) return 0;
    while (done < len) {
        size_t want = len - done;
        int n;
        if (h->response_left_known) {
            if (!h->response_left) break;
            if (want > h->response_left) want = h->response_left;
        }
        if (h->chunked) {
            if (!h->chunk_left && !begin_chunk(h)) break;
            if (want > h->chunk_left) want = h->chunk_left;
        }
        n = raw_read_some(h, dst + done, want);
        if (n <= 0) break;
        done += (size_t)n;
        h->body_pos += (size_t)n;
        if (h->response_left_known) h->response_left -= (size_t)n;
        if (h->chunked) {
            h->chunk_left -= (size_t)n;
            if (!h->chunk_left) h->chunk_need_crlf = 1;
        }
        /* Between socket reads, let the caller present queued video / feed audio.
         * This is what stops a blocking segment fetch from freezing playback for
         * its whole duration: the single task services here instead of only
         * after the read returns (when the backlog is already late). Cheap when
         * nothing is due; NULL (host builds) skips it entirely. */
        if (g_http_service && g_http_service(g_http_service_opaque)) {
            h->interrupted = 1;
            mr_source_set_error("HTTP read interrupted");
            break;
        }
    }
    return (int)done;
}

static int cache_copy(const http_source *h, size_t off,
                      unsigned char *dst, size_t len)
{
    if (!h->cache || off < h->cache_start ||
        off - h->cache_start > h->cache_len ||
        len > h->cache_len - (off - h->cache_start))
        return 0;
    memcpy(dst, h->cache + (off - h->cache_start), len);
    return 1;
}

static void cache_store(http_source *h, size_t off,
                        const unsigned char *data, size_t len)
{
    size_t end, cache_end, overlap, keep;
    if (!h->cache || !len) return;
    if (len >= h->cache_cap) {
        memcpy(h->cache, data + len - h->cache_cap, h->cache_cap);
        h->cache_start = off + len - h->cache_cap;
        h->cache_len = h->cache_cap;
        return;
    }
    end = off + len;
    cache_end = h->cache_start + h->cache_len;
    if (h->cache_len && off <= cache_end && end >= h->cache_start) {
        if (off < h->cache_start) {
            /* This uncommon backwards extension is simpler and bounded when
             * rebuilt as a fresh range. Sequential playback takes the append
             * branch below. */
            memcpy(h->cache, data, len);
            h->cache_start = off;
            h->cache_len = len;
            return;
        }
        overlap = cache_end > off ? cache_end - off : 0;
        if (overlap > len) overlap = len;
        if (len > overlap) {
            size_t add = len - overlap;
            if (h->cache_len + add > h->cache_cap) {
                size_t drop = h->cache_len + add - h->cache_cap;
                memmove(h->cache, h->cache + drop, h->cache_len - drop);
                h->cache_start += drop;
                h->cache_len -= drop;
            }
            memcpy(h->cache + h->cache_len, data + overlap, add);
            h->cache_len += add;
        }
        return;
    }
    keep = len;
    memcpy(h->cache, data, keep);
    h->cache_start = off;
    h->cache_len = keep;
}

/* Is there data to read without blocking? SSL may hold already-decrypted bytes
 * that select() cannot see, so check that first. */
static int http_readable(http_source *h)
{
    fd_set rf;
    struct timeval tv;
    if (!h->socket_ready || h->sock < 0) return 0;
#if MR_HTTP_HAVE_TLS
    if (h->using_tls && h->ssl && SSL_pending(h->ssl) > 0) return 1;
#endif
    FD_ZERO(&rf);
    FD_SET(h->sock, &rf);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
#if MR_HTTP_AMIGA
    return WaitSelect(h->sock + 1, &rf, NULL, NULL, &tv, NULL) > 0;
#else
    return select(h->sock + 1, &rf, NULL, NULL, &tv) > 0;
#endif
}

/* Drop already-consumed bytes off the front of the linear cache, keeping
 * HTTP_BACK_WINDOW behind the newest request so a trailing (video) read still
 * hits after the leading (audio) read has moved on. This frees room for the
 * drain to keep filling forward. */
static void http_cache_trim(http_source *h)
{
    size_t keep_from, drop;
    if (!h->cache || h->max_read <= HTTP_BACK_WINDOW) return;
    keep_from = h->max_read - HTTP_BACK_WINDOW;
    if (keep_from <= h->cache_start) return;
    drop = keep_from - h->cache_start;
    if (drop < HTTP_TRIM_MIN) return;              /* batch: rare memmove       */
    if (drop >= h->cache_len) return;              /* never drop live data     */
    memmove(h->cache, h->cache + drop, h->cache_len - drop);
    h->cache_start += drop;
    h->cache_len   -= drop;
}

/* Pull the open response forward into the cache. `min_only` fills just up to
 * HTTP_MIN_WINDOW with blocking reads (so presentation-order reads never fall
 * outside the window and reconnect); otherwise it also drains whatever the
 * socket already has - data the OS received while the player was decoding or
 * pacing - up to the full buffer, without blocking. Reads only forward and
 * stays contiguous with body_pos. */
static void http_readahead(http_source *h, int min_only)
{
    size_t cache_end, forward;
    /* Forward-only streams need only a tiny window (see HTTP_STREAM_MIN_WINDOW);
     * keeping it small also keeps each blocking refill short. */
    size_t min_window = h->streaming ? HTTP_STREAM_MIN_WINDOW : HTTP_MIN_WINDOW;

    if (!h->socket_ready || !h->cache) return;
    if (h->cache_start + h->cache_len != h->body_pos) return; /* not contiguous */
    http_cache_trim(h);

    /*
     * Measure the bytes retained ahead of the furthest demuxer request.
     * cache_len includes the rewind window behind playback, so using it here
     * caused blocking read-ahead to stop once the total cache exceeded
     * HTTP_MIN_WINDOW even when almost no unread data remained ahead.
     */
    cache_end = h->cache_start + h->cache_len;
    forward = cache_end > h->max_read ? cache_end - h->max_read : 0;

    /* Bootstrap: guarantee a minimum forward window with blocking reads. */
    while (forward < min_window && h->cache_len < h->cache_cap) {
        size_t room = h->cache_cap - h->cache_len;
        size_t want = min_window - forward;
        int n;

        if (want > room) want = room;
        if (want > 65536) want = 65536;

        n = copy_response_bytes(h, h->cache + h->cache_len, want);
        if (n <= 0) return;
        h->cache_len += (size_t)n;
        forward += (size_t)n;
    }
    if (min_only) return;
    /* Proactive: absorb whatever is already waiting on the socket, no block. */
    while (h->cache_len < h->cache_cap && http_readable(h)) {
        size_t room = h->cache_cap - h->cache_len;
        int n = copy_response_bytes(h, h->cache + h->cache_len,
                                    room < 65536 ? room : 65536);
        if (n <= 0) break;
        h->cache_len += (size_t)n;
    }
}

static int http_read_at(void *opaque, size_t off, void *dst, size_t len)
{
    http_source *h = (http_source *)opaque;
    unsigned char *out = (unsigned char *)dst;
    size_t done = 0;
    int retries = 0;
    if (!len) return 1;
    if (off + len > h->max_read) h->max_read = off + len;
    if (cache_copy(h, off, out, len)) {
        /* Served from RAM: still top up from any socket data that arrived while
         * the player was busy, so the window stays ahead of playback. */
        http_readahead(h, 0);
        return 1;
    }

    /* A demuxer may reread a header that overlaps the current network
     * position (TS's 512-byte sniff versus 188-byte packets). Reuse the cached
     * prefix and continue from the already-open response without reconnecting. */
    if (h->socket_ready && off < h->body_pos && off >= h->cache_start &&
        h->body_pos - off < len &&
        cache_copy(h, off, out, h->body_pos - off)) {
        done = h->body_pos - off;
    }
    if (!h->socket_ready || h->body_pos != off + done) {
        done = 0;
        if (!begin_response(h, off)) return 0;
        /* A reconnect means we seeked to a new region - re-anchor the read-ahead
         * here. Without this, a one-time metadata read (a moov stored at the end
         * of the file) leaves max_read at EOF, so the back-window trim evicts the
         * front-of-file mdat the playback then reads, reconnecting every packet. */
        h->max_read = off + len;
    }
    while (done < len) {
        int n = copy_response_bytes(h, out + done, len - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (h->interrupted) return 0;
        close_connection(h, 0);
        if (done == len) break;
        if (retries++ >= HTTP_IO_RETRIES ||
            !begin_response(h, off + done))
            return 0;
    }
    cache_store(h, off, out, len);
    http_readahead(h, 0);
    return 1;
}

static void platform_close(http_source *h)
{
    /* Tear down only this connection's socket and SSL object. The bsdsocket and
     * AmiSSL libraries and the SSL_CTX deliberately persist for the life of the
     * process (released once by http_platform_shutdown); this is what lets each
     * subsequent HLS segment reconnect without re-initialising AmiSSL and
     * rebuilding the TLS context, the dominant cost at every segment boundary. */
    close_connection(h, 1);
#if MR_HTTP_HAVE_TLS
    h->ssl_ctx = NULL;          /* borrowed pointer to g_ssl_ctx; never freed here */
#endif
    h->platform_ready = 0;
}

static void http_close(void *opaque)
{
    http_source *h = (http_source *)opaque;
    if (!h) return;
    platform_close(h);
    mr_free(h->cache);
    mr_free(h);
}

mr_source *mr_http_source_open_ex(const char *url,
                                  const mr_http_options *options)
{
    http_source *h;
    mr_source *source;
    size_t n, actual_cache;
    if (!url || !*url || strlen(url) >= HTTP_URL_MAX) {
        mr_source_set_error("HTTP URL is empty or too long");
        return NULL;
    }
    h = (http_source *)mr_allocz(sizeof *h);
    if (!h) {
        mr_source_set_error("not enough memory for HTTP source");
        return NULL;
    }
    h->sock = -1;
    if (options && !mr_http_options_init(&h->options,
                                         options->user_agent,
                                         options->referer)) {
        mr_free(h);
        return NULL;
    }
    if (options) {
        h->options.hls_low = options->hls_low;
        h->options.hls_live_start_segments =
            options->hls_live_start_segments;
        h->options.hls_buffer_segments = options->hls_buffer_segments;
        h->options.hls_max_width = options->hls_max_width;
        h->options.hls_max_height = options->hls_max_height;
        h->options.hls_max_fps = options->hls_max_fps;
        h->options.source_buffer_bytes = options->source_buffer_bytes;
    }
    if (h->options.source_buffer_bytes) {
        size_t attempt = h->options.source_buffer_bytes;
        while (attempt >= HTTP_CACHE_SIZE) {
            h->cache = (unsigned char *)mr_alloc_fast(attempt);
            if (h->cache) {
                h->cache_cap = attempt;
                break;
            }
            attempt /= 2;
        }
    }
    if (!h->cache) {
        h->cache = (unsigned char *)mr_alloc(HTTP_CACHE_SIZE);
        h->cache_cap = HTTP_CACHE_SIZE;
    }
    if (!h->cache) {
        mr_source_set_error("not enough memory for HTTP rewind cache");
        mr_free(h);
        return NULL;
    }
    n = strlen(url);
    memcpy(h->url, url, n + 1);
    if (!platform_open(h) || !begin_response(h, 0)) {
        http_close(h);
        return NULL;
    }
    actual_cache = h->cache_cap;
    source = mr_source_create(h,
                              h->streaming ? MR_SOURCE_LEN_UNKNOWN : h->total_len,
                              http_read_at, http_close, h->url);
    mr_source_set_buffer_capacity(source, actual_cache);
    mr_source_mark_network(source);
    return source;
}

mr_source *mr_http_source_open(const char *url)
{
    return mr_http_source_open_ex(url, NULL);
}

int mr_http_fetch_text(const char *url, const mr_http_options *options,
                       char **out, size_t *out_len, size_t max_size)
{
    if (g_fetch_override)
        return g_fetch_override(url, options, NULL,
                                (unsigned char **)out, out_len, max_size);
    return mr_http_fetch_text_direct(url, options, out, out_len, max_size);
}

int mr_http_fetch_text_direct(const char *url, const mr_http_options *options,
                              char **out, size_t *out_len, size_t max_size)
{
    http_source *h;
    unsigned char *buf = NULL;
    size_t total = 0, cap = 0;
    int ok = 0;
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!url || !*url || strlen(url) >= HTTP_URL_MAX || !out || !out_len ||
        !max_size || max_size == (size_t)-1) {
        mr_source_set_error("invalid HTTP text fetch arguments");
        return 0;
    }
    h = (http_source *)mr_allocz(sizeof *h);
    if (!h) {
        mr_source_set_error("not enough memory for HTTP text fetch");
        return 0;
    }
    h->sock = -1;
    h->bounded_fetch = 1;
    memcpy(h->url, url, strlen(url) + 1);
    if (options) {
        if (!mr_http_options_init(&h->options, options->user_agent,
                                  options->referer)) goto done;
    }
    if (!platform_open(h) || !begin_response(h, 0)) goto done;
    if (h->response_left_known && h->response_left > max_size) {
        mr_source_set_error("HTTP text response exceeds size limit");
        goto done;
    }
    cap = h->response_left_known ? h->response_left : 32768;
    if (!cap) cap = 1;
    if (cap > max_size) cap = max_size;
    buf = (unsigned char *)mr_alloc(cap + 1);
    if (!buf) {
        mr_source_set_error("not enough memory for HTTP text response");
        goto done;
    }
    for (;;) {
        int n;
        size_t want;
        if (total == cap) {
            unsigned char *grown;
            size_t next;
            if (cap == max_size) {
                /* Read one byte to distinguish exactly-at-limit from overflow. */
                unsigned char extra;
                n = copy_response_bytes(h, &extra, 1);
                if (n > 0) {
                    mr_source_set_error("HTTP text response exceeds size limit");
                    goto done;
                }
                break;
            }
            next = cap > max_size / 2 ? max_size : cap * 2;
            grown = (unsigned char *)mr_alloc(next + 1);
            if (!grown) {
                mr_source_set_error("not enough memory for HTTP text response");
                goto done;
            }
            memcpy(grown, buf, total);
            mr_free(buf);
            buf = grown;
            cap = next;
        }
        want = cap - total;
        if (want > 16384) want = 16384;
        n = copy_response_bytes(h, buf + total, want);
        if (n <= 0) break;
        total += (size_t)n;
    }
    if (!total || (h->response_left_known && h->response_left) ||
        (!h->response_left_known && h->chunked && !h->chunk_done)) {
        mr_source_set_error("HTTP text response ended before completion");
        goto done;
    }
    buf[total] = '\0';
    *out = (char *)buf;
    *out_len = total;
    buf = NULL;
    ok = 1;
done:
    platform_close(h);
    mr_free(h);
    mr_free(buf);
    return ok;
}

int mr_http_fetch_buffer(const char *url, const mr_http_options *options,
                         unsigned char **out, size_t *out_len,
                         size_t max_size)
{
    char *buf = NULL;
    if (out) *out = NULL;
    if (!out || !mr_http_fetch_text(url, options, &buf, out_len, max_size))
        return 0;
    *out = (unsigned char *)buf;
    return 1;
}

int mr_http_post_json(const char *url, const mr_http_options *options,
                      const char *json, char **out, size_t *out_len,
                      size_t max_size)
{
    if (g_fetch_override)
        return g_fetch_override(url, options, json,
                                (unsigned char **)out, out_len, max_size);
    return mr_http_post_json_direct(url, options, json, out, out_len,
                                    max_size);
}

int mr_http_post_json_direct(const char *url, const mr_http_options *options,
                             const char *json, char **out, size_t *out_len,
                             size_t max_size)
{
    http_source *h;
    unsigned char *buf = NULL;
    size_t total = 0, cap = 0;
    int ok = 0;
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!url || !*url || strlen(url) >= HTTP_URL_MAX || !json || !out ||
        !out_len || !max_size || max_size == (size_t)-1) {
        mr_source_set_error("invalid HTTP JSON POST arguments");
        return 0;
    }
    h = (http_source *)mr_allocz(sizeof *h);
    if (!h) {
        mr_source_set_error("not enough memory for HTTP JSON POST");
        return 0;
    }
    h->sock = -1;
    memcpy(h->url, url, strlen(url) + 1);
    if (options && !mr_http_options_init(&h->options, options->user_agent,
                                         options->referer)) goto done;
    if (!platform_open(h) || !begin_json_post_response(h, json)) goto done;
    if (h->response_left_known && h->response_left > max_size) {
        mr_source_set_error("HTTP JSON response exceeds size limit");
        goto done;
    }
    cap = h->response_left_known ? h->response_left : 32768;
    if (!cap) cap = 1;
    if (cap > max_size) cap = max_size;
    buf = (unsigned char *)mr_alloc(cap + 1);
    if (!buf) {
        mr_source_set_error("not enough memory for HTTP JSON response");
        goto done;
    }
    for (;;) {
        int n;
        size_t want;
        if (total == cap) {
            unsigned char *grown;
            size_t next;
            if (cap == max_size) {
                unsigned char extra;
                n = copy_response_bytes(h, &extra, 1);
                if (n > 0) {
                    mr_source_set_error("HTTP JSON response exceeds size limit");
                    goto done;
                }
                break;
            }
            next = cap > max_size / 2 ? max_size : cap * 2;
            grown = (unsigned char *)mr_alloc(next + 1);
            if (!grown) {
                mr_source_set_error("not enough memory for HTTP JSON response");
                goto done;
            }
            memcpy(grown, buf, total);
            mr_free(buf);
            buf = grown;
            cap = next;
        }
        want = cap - total;
        if (want > 16384) want = 16384;
        n = copy_response_bytes(h, buf + total, want);
        if (n <= 0) break;
        total += (size_t)n;
    }
    if (!total || (h->response_left_known && h->response_left) ||
        (!h->response_left_known && h->chunked && !h->chunk_done)) {
        mr_source_set_error("HTTP JSON response ended before completion");
        goto done;
    }
    buf[total] = '\0';
    *out = (char *)buf;
    *out_len = total;
    buf = NULL;
    ok = 1;
done:
    platform_close(h);
    mr_free(h);
    mr_free(buf);
    return ok;
}

int mr_http_download_file(const char *url, const char *path, size_t max_size)
{
    http_source *h;
    FILE *file = NULL;
    unsigned char buffer[16384];
    size_t total = 0;
    int ok = 0;
    if (!url || !*url || strlen(url) >= HTTP_URL_MAX || !path || !*path ||
        !max_size) {
        mr_source_set_error("invalid HTTP download arguments");
        return 0;
    }
    h = (http_source *)mr_allocz(sizeof *h);
    if (!h) {
        mr_source_set_error("not enough memory for HTTP download");
        return 0;
    }
    h->sock = -1;
    memcpy(h->url, url, strlen(url) + 1);
    if (!platform_open(h) || !begin_response(h, 0)) goto done;
    file = fopen(path, "wb");
    if (!file) {
        mr_source_set_error("cannot create HTTP download file");
        goto done;
    }
    for (;;) {
        size_t want = sizeof buffer;
        int n;
        if (total >= max_size) want = 1;
        else if (want > max_size - total) want = max_size - total;
        n = copy_response_bytes(h, buffer, want);
        if (n <= 0) break;
        if (total >= max_size || (size_t)n > max_size - total) {
            mr_source_set_error("HTTP download exceeds size limit");
            goto done;
        }
        if (fwrite(buffer, 1, (size_t)n, file) != (size_t)n) {
            mr_source_set_error("cannot write HTTP download file");
            goto done;
        }
        total += (size_t)n;
    }
    if (!total || (h->response_left_known && h->response_left) ||
        (!h->response_left_known && h->chunked && !h->chunk_done)) {
        mr_source_set_error("HTTP download ended before response completed");
        goto done;
    }
    if (fclose(file) != 0) {
        file = NULL;
        mr_source_set_error("cannot finish HTTP download file");
        goto done;
    }
    file = NULL;
    ok = 1;
done:
    if (file) fclose(file);
    platform_close(h);
    mr_free(h);
    if (!ok) remove(path);
    return ok;
}
