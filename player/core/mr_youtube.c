/*
 * MintVID - native YouTube live and progressive MP4 resolver.
 *
 * No JavaScript engine or signature decipher is attempted. Public live HLS is
 * handed to the existing HLS path; compatible recorded uploads may provide
 * itag 18/22, HTTPS Google Video MP4s containing H.264 and AAC at 360p/720p.
 */
#include "mr_youtube.h"

#include "mr_alloc.h"
#include "mr_http.h"
#include "mr_source.h"

#include <stdio.h>
#include <string.h>

#define YOUTUBE_PAGE_MAX (4UL * 1024 * 1024)
#define YOUTUBE_BROWSER_UA \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 " \
    "(KHTML, like Gecko) Chrome/120.0 Safari/537.36"
#define YOUTUBE_REFERER "https://www.youtube.com/"
#define MINT_VID_DEFAULT_UA "MintVID/0.1 AmigaOS"
#define YOUTUBE_LIVE_START_SEGMENTS 2
#define YOUTUBE_ANDROID_VERSION "21.08.266"
#define YOUTUBE_ANDROID_UA \
    "com.google.android.youtube/21.08.266 (Linux; U; Android 11) gzip"
#define YOUTUBE_ANDROID_VR_VERSION "1.65.10"
#define YOUTUBE_ANDROID_VR_UA \
    "com.google.android.apps.youtube.vr.oculus/1.65.10 " \
    "(Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip"

static const char *g_last_client = "";
static const char *g_last_media_ua = YOUTUBE_BROWSER_UA;
static mr_youtube_media_kind g_last_kind = MR_YOUTUBE_MEDIA_NONE;

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

static int host_equal(const char *host, size_t len, const char *expected)
{
    size_t i, n = strlen(expected);
    if (len != n) return 0;
    for (i = 0; i < n; i++)
        if (ascii_tolower((unsigned char)host[i]) !=
            ascii_tolower((unsigned char)expected[i])) return 0;
    return 1;
}

int mr_youtube_is_url(const char *url)
{
    const char *host, *end, *colon;
    size_t len;
    if (!url) return 0;
    if (!ascii_ncasecmp(url, "http://", 7)) host = url + 7;
    else if (!ascii_ncasecmp(url, "https://", 8)) host = url + 8;
    else return 0;
    end = host + strcspn(host, "/?#");
    colon = (const char *)memchr(host, ':', (size_t)(end - host));
    if (colon) end = colon;
    len = (size_t)(end - host);
    return host_equal(host, len, "youtube.com") ||
           host_equal(host, len, "www.youtube.com") ||
           host_equal(host, len, "m.youtube.com") ||
           host_equal(host, len, "music.youtube.com") ||
           host_equal(host, len, "youtu.be") ||
           host_equal(host, len, "www.youtube-nocookie.com");
}

static int video_id_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

int mr_youtube_extract_video_id(const char *url, char out[12])
{
    const char *p = NULL, *v;
    size_t i;
    if (!mr_youtube_is_url(url) || !out) return 0;
    v = strstr(url, "v=");
    while (v) {
        if (v == url || v[-1] == '?' || v[-1] == '&') { p = v + 2; break; }
        v = strstr(v + 2, "v=");
    }
    if (!p) {
        static const char *prefixes[] = { "/live/", "/embed/", "/shorts/" };
        for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
            p = strstr(url, prefixes[i]);
            if (p) { p += strlen(prefixes[i]); break; }
        }
    }
    if (!p && strstr(url, "youtu.be/"))
        p = strstr(url, "youtu.be/") + strlen("youtu.be/");
    if (!p) return 0;
    for (i = 0; i < 11; i++) {
        if (!p[i] || !video_id_char((unsigned char)p[i])) return 0;
        out[i] = p[i];
    }
    if (video_id_char((unsigned char)p[11])) return 0;
    out[11] = '\0';
    return 1;
}

int mr_youtube_http_options_init(mr_http_options *out,
                                 const mr_http_options *base)
{
    const char *ua = YOUTUBE_BROWSER_UA;
    const char *referer = YOUTUBE_REFERER;
    if (!out) {
        mr_source_set_error("invalid YouTube HTTP options");
        return 0;
    }
    if (base && base->user_agent[0] &&
        strcmp(base->user_agent, MINT_VID_DEFAULT_UA))
        ua = base->user_agent;
    if (base && base->referer[0]) referer = base->referer;
    if (!mr_http_options_init(out, ua, referer)) return 0;
    /* Resolving YouTube costs several HTTPS round trips. Its six-segment live
     * window can advance while that happens, so starting with its oldest item
     * commonly produces a Google Video 403 before playback even begins. */
    out->hls_live_start_segments = YOUTUBE_LIVE_START_SEGMENTS;
    out->hls_buffer_segments = 1;
    if (base) {
        out->hls_low = base->hls_low;
        out->hls_max_width = base->hls_max_width;
        out->hls_max_height = base->hls_max_height;
        out->hls_max_fps = base->hls_max_fps;
        out->source_buffer_bytes = base->source_buffer_bytes;
    }
    return 1;
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode one JSON string. YouTube manifest URLs are ASCII; accepting only
 * ASCII \u escapes avoids needing a Unicode implementation in this tiny path. */
static int decode_json_string(const char *p, char *out, size_t out_size)
{
    size_t used = 0;
    if (!p || *p != '"' || !out || out_size < 2) return 0;
    p++;
    while (*p && *p != '"') {
        unsigned value = (unsigned char)*p++;
        if (value == '\\') {
            int a, b, c, d;
            value = (unsigned char)*p++;
            if (!value) return 0;
            if (value == 'u') {
                if (!p[0] || !p[1] || !p[2] || !p[3]) return 0;
                a = hex_value((unsigned char)p[0]);
                b = hex_value((unsigned char)p[1]);
                c = hex_value((unsigned char)p[2]);
                d = hex_value((unsigned char)p[3]);
                if (a < 0 || b < 0 || c < 0 || d < 0) return 0;
                value = (unsigned)((a << 12) | (b << 8) | (c << 4) | d);
                p += 4;
                if (!value || value > 0x7f) return 0;
            } else if (value == 'b') value = '\b';
            else if (value == 'f') value = '\f';
            else if (value == 'n') value = '\n';
            else if (value == 'r') value = '\r';
            else if (value == 't') value = '\t';
            else if (value != '"' && value != '\\' && value != '/') return 0;
        }
        if (value < 0x20 || value == 0x7f) return 0;
        if (used + 1 >= out_size) return 0;
        out[used++] = (char)value;
    }
    if (*p != '"') return 0;
    out[used] = '\0';
    return 1;
}

static int is_google_hls(const char *url)
{
    static const char prefix[] = "https://manifest.googlevideo.com/";
    return !strncmp(url, prefix, sizeof prefix - 1) && strstr(url, ".m3u8");
}

/* YouTube's /n/<value> path component is a throttling challenge, unrelated to
 * a PO token. The value must be transformed by code extracted from the current
 * player JavaScript before GVS will serve the media. This native resolver has
 * no JavaScript engine, so never hand an unsolved URL to HLS. */
static int manifest_needs_n_transform(const char *url)
{
    return url && strstr(url, "/n/") != NULL;
}

const char *mr_youtube_last_client(void)
{
    return g_last_client;
}

mr_youtube_media_kind mr_youtube_last_media_kind(void)
{
    return g_last_kind;
}

int mr_youtube_media_http_options_init(mr_http_options *out,
                                       const mr_http_options *base)
{
    mr_http_options resolved;
    if (!mr_youtube_http_options_init(&resolved, base)) return 0;
    if (g_last_kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_360P ||
        g_last_kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_720P) {
        if (!mr_http_options_init(out, g_last_media_ua, YOUTUBE_REFERER))
            return 0;
        out->hls_low = resolved.hls_low;
        out->hls_max_width = resolved.hls_max_width;
        out->hls_max_height = resolved.hls_max_height;
        out->hls_max_fps = resolved.hls_max_fps;
        out->hls_live_start_segments = resolved.hls_live_start_segments;
        out->hls_buffer_segments = resolved.hls_buffer_segments;
        out->source_buffer_bytes = resolved.source_buffer_bytes;
        return 1;
    }
    *out = resolved;
    return 1;
}

static int extract_config_string(const char *html, const char *name,
                                 char *out, size_t out_size)
{
    char key[80];
    const char *p;
    int n = snprintf(key, sizeof key, "\"%s\"", name);
    if (!html || !name || n <= 0 || (size_t)n >= sizeof key) return 0;
    p = strstr(html, key);
    if (!p) return 0;
    p += (size_t)n;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return decode_json_string(p, out, out_size);
}

int mr_youtube_extract_live_manifest(const char *html, char *out,
                                     size_t out_size)
{
    static const char key[] = "\"hlsManifestUrl\"";
    const char *p;
    if (!html || !out || !out_size) return 0;
    p = html;
    while ((p = strstr(p, key)) != NULL) {
        p += sizeof key - 1;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != ':') {
            if (!*p) break;
            p++;
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (decode_json_string(p, out, out_size) && is_google_hls(out))
            return 1;
    }
    out[0] = '\0';
    return 0;
}

static const char *skip_json_space(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    return p;
}

static const char *json_string_end(const char *p, const char *end)
{
    if (p >= end || *p != '"') return NULL;
    for (p++; p < end; p++) {
        if (*p == '\\') {
            if (++p >= end) return NULL;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

static const char *json_compound_end(const char *p, const char *end,
                                     char open, char close)
{
    unsigned depth = 0;
    for (; p < end; p++) {
        const char *next;
        if (*p == '"') {
            next = json_string_end(p, end);
            if (!next) return NULL;
            p = next - 1;
        } else if (*p == open) {
            depth++;
        } else if (*p == close) {
            if (!depth || !--depth) return p + 1;
        }
    }
    return NULL;
}

static const char *json_value_end(const char *p, const char *end)
{
    p = skip_json_space(p, end);
    if (p >= end) return NULL;
    if (*p == '"') return json_string_end(p, end);
    if (*p == '{') return json_compound_end(p, end, '{', '}');
    if (*p == '[') return json_compound_end(p, end, '[', ']');
    while (p < end && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

static const char *json_object_field(const char *object,
                                     const char *object_end,
                                     const char *name)
{
    const char *p = object + 1;
    size_t name_len = strlen(name);
    while (p < object_end) {
        const char *key_end, *value, *value_end;
        p = skip_json_space(p, object_end);
        if (p >= object_end || *p == '}') break;
        if (*p != '"' || !(key_end = json_string_end(p, object_end)))
            return NULL;
        value = skip_json_space(key_end, object_end);
        if (value >= object_end || *value++ != ':') return NULL;
        value = skip_json_space(value, object_end);
        value_end = json_value_end(value, object_end);
        if (!value_end) return NULL;
        if ((size_t)(key_end - p) == name_len + 2 &&
            !strncmp(p + 1, name, name_len))
            return value;
        p = skip_json_space(value_end, object_end);
        if (p < object_end && *p == ',') p++;
    }
    return NULL;
}

static int googlevideo_url(const char *url)
{
    const char *host, *end;
    size_t len, suffix_len;
    static const char suffix[] = ".googlevideo.com";
    if (!url || strncmp(url, "https://", 8)) return 0;
    host = url + 8;
    end = host + strcspn(host, "/?#:");
    len = (size_t)(end - host);
    suffix_len = sizeof suffix - 1;
    return host_equal(host, len, "googlevideo.com") ||
           (len > suffix_len &&
            !ascii_ncasecmp(host + len - suffix_len, suffix, suffix_len));
}

static int url_has_n_parameter(const char *url)
{
    const char *p = strchr(url, '?');
    if (!p) return 0;
    p++;
    while (*p) {
        const char *amp;
        if (p[0] == 'n' && p[1] == '=') return 1;
        amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

static int extract_progressive_itag(const char *json, unsigned wanted_itag,
                                    char *out, size_t out_size)
{
    static const char formats_key[] = "\"formats\"";
    const char *p, *end;
    if (!json || !out || out_size < 2) return 0;
    out[0] = '\0';
    end = json + strlen(json);
    p = json;
    while ((p = strstr(p, formats_key)) != NULL) {
        const char *array, *array_end;
        p += sizeof formats_key - 1;
        array = skip_json_space(p, end);
        if (array >= end || *array++ != ':') continue;
        array = skip_json_space(array, end);
        if (array >= end || *array != '[') continue;
        array_end = json_compound_end(array, end, '[', ']');
        if (!array_end) return 0;
        p = array + 1;
        while (p < array_end - 1) {
            const char *object_end, *itag, *mime, *media_url;
            char mime_text[160];
            p = skip_json_space(p, array_end - 1);
            if (p >= array_end - 1) break;
            if (*p != '{') {
                const char *next = json_value_end(p, array_end - 1);
                if (!next) return 0;
                p = next;
                if (p < array_end - 1 && *p == ',') p++;
                continue;
            }
            object_end = json_compound_end(p, array_end, '{', '}');
            if (!object_end) return 0;
            itag = json_object_field(p, object_end, "itag");
            mime = json_object_field(p, object_end, "mimeType");
            media_url = json_object_field(p, object_end, "url");
            if (itag &&
                ((wanted_itag == 18 && itag[0] == '1' && itag[1] == '8' &&
                  (itag[2] == ',' || itag[2] == '}' || itag[2] == ' ' ||
                   itag[2] == '\t' || itag[2] == '\r' || itag[2] == '\n')) ||
                 (wanted_itag == 22 && itag[0] == '2' && itag[1] == '2' &&
                  (itag[2] == ',' || itag[2] == '}' || itag[2] == ' ' ||
                   itag[2] == '\t' || itag[2] == '\r' || itag[2] == '\n'))) &&
                mime && decode_json_string(mime, mime_text,
                                            sizeof mime_text) &&
                strstr(mime_text, "video/mp4") &&
                strstr(mime_text, "avc1") && strstr(mime_text, "mp4a") &&
                media_url && decode_json_string(media_url, out, out_size) &&
                googlevideo_url(out) && !url_has_n_parameter(out))
                return 1;
            p = object_end;
            if (p < array_end - 1 && *p == ',') p++;
        }
        p = array_end;
    }
    out[0] = '\0';
    return 0;
}

int mr_youtube_extract_progressive(const char *json, int prefer_720p,
                                   char *out, size_t out_size,
                                   mr_youtube_media_kind *kind)
{
    if (!kind) return 0;
    *kind = MR_YOUTUBE_MEDIA_NONE;
    if (prefer_720p && extract_progressive_itag(json, 22, out, out_size)) {
        *kind = MR_YOUTUBE_MEDIA_PROGRESSIVE_720P;
        return 1;
    }
    if (extract_progressive_itag(json, 18, out, out_size)) {
        *kind = MR_YOUTUBE_MEDIA_PROGRESSIVE_360P;
        return 1;
    }
    return 0;
}

int mr_youtube_extract_progressive_360p(const char *json, char *out,
                                        size_t out_size)
{
    return extract_progressive_itag(json, 18, out, out_size);
}

static int options_prefer_720p(const mr_http_options *options)
{
    if (!options || options->hls_low) return 0;
    if (options->hls_max_height)
        return options->hls_max_height >= 720;
    if (options->hls_max_width)
        return options->hls_max_width >= 1280;
    return 1;
}

/* Returns 1 for immediately usable media, -1 when media was present but
 * carried an unsolved n challenge, -2 when the only usable format was a
 * muxed 360p fallback while a 720p+ stream was requested (kept by the
 * caller in case no client offers the requested quality), and 0 for a
 * failed/no-media reply. */
static int try_player_media(const char *api_url,
                            const mr_http_options *options,
                            const char *json, const char *client,
                            const char *media_ua, char *out, size_t out_size,
                            int prefer_720p, mr_youtube_media_kind *kind)
{
    char *reply = NULL;
    size_t reply_len = 0;
    int ok;
    if (!mr_http_post_json(api_url, options, json, &reply, &reply_len,
                           YOUTUBE_PAGE_MAX))
        return 0;
    (void)reply_len;
    ok = mr_youtube_extract_live_manifest(reply, out, out_size);
    if (ok && !manifest_needs_n_transform(out)) {
        g_last_client = client;
        g_last_media_ua = media_ua;
        g_last_kind = MR_YOUTUBE_MEDIA_HLS;
        *kind = g_last_kind;
        mr_free(reply);
        return 1;
    }
    if (ok) {
        mr_free(reply);
        return -1;
    }
    ok = mr_youtube_extract_progressive(reply, prefer_720p, out, out_size,
                                        kind);
    mr_free(reply);
    if (!ok) return 0;
    if (prefer_720p && *kind != MR_YOUTUBE_MEDIA_PROGRESSIVE_720P)
        return -2;
    g_last_client = client;
    g_last_media_ua = media_ua;
    g_last_kind = *kind;
    return 1;
}

/* Remembers the first muxed 360p fallback seen while still searching for a
 * requested 720p+ stream, so a later client offering real 720p is not
 * short-circuited by an earlier client's 360p-only response. */
static void keep_progressive_fallback(const char *client, const char *media_ua,
                                      const char *media_url,
                                      mr_youtube_media_kind kind,
                                      int *have_fallback,
                                      const char **fallback_client,
                                      const char **fallback_media_ua,
                                      mr_youtube_media_kind *fallback_kind,
                                      char *fallback_media,
                                      size_t fallback_media_size)
{
    size_t len;
    if (*have_fallback || fallback_media_size < 1) return;
    len = strlen(media_url);
    if (len >= fallback_media_size) len = fallback_media_size - 1;
    memcpy(fallback_media, media_url, len);
    fallback_media[len] = '\0';
    *fallback_client = client;
    *fallback_media_ua = media_ua;
    *fallback_kind = kind;
    *have_fallback = 1;
    printf("YouTube: %s supplied only muxed 360p; continuing 720p search\n",
           client);
}

int mr_youtube_resolve_media(const char *url,
                             const mr_http_options *options,
                             char *out, size_t out_size,
                             mr_youtube_media_kind *kind)
{
    mr_http_options fetch_options, vr_options;
    char *html = NULL;
    size_t html_len = 0;
    char video_id[12], api_key[80], client_version[64];
    char api_url[256], json[1024];
    char fallback_media[MR_HTTP_URL_MAX];
    int have_fallback = 0;
    const char *fallback_client = "";
    const char *fallback_media_ua = YOUTUBE_BROWSER_UA;
    mr_youtube_media_kind fallback_kind = MR_YOUTUBE_MEDIA_NONE;
    int n, ok, result, saw_n_challenge = 0;
    int prefer_720p = options_prefer_720p(options);
    g_last_client = "";
    g_last_media_ua = YOUTUBE_BROWSER_UA;
    g_last_kind = MR_YOUTUBE_MEDIA_NONE;
    if (kind) *kind = MR_YOUTUBE_MEDIA_NONE;
    if (!mr_youtube_is_url(url) || !out || out_size < 2 || !kind) {
        mr_source_set_error("invalid YouTube URL or output buffer");
        return 0;
    }
    if (!mr_youtube_http_options_init(&fetch_options, options))
        return 0;
    if (!mr_http_fetch_text(url, &fetch_options, &html, &html_len,
                            YOUTUBE_PAGE_MAX))
        return 0;
    (void)html_len;
    ok = mr_youtube_extract_live_manifest(html, out, out_size);
    if (ok && !manifest_needs_n_transform(out)) {
        g_last_client = "watch page";
        g_last_kind = MR_YOUTUBE_MEDIA_HLS;
        *kind = g_last_kind;
        mr_free(html);
        return 1;
    }
    if (ok) saw_n_challenge = 1;
    if (mr_youtube_extract_progressive(html, prefer_720p, out, out_size,
                                       kind)) {
        if (!prefer_720p || *kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_720P) {
            g_last_client = "watch page";
            g_last_kind = *kind;
            mr_free(html);
            return 1;
        }
        keep_progressive_fallback("watch page", YOUTUBE_BROWSER_UA, out,
                                  *kind, &have_fallback, &fallback_client,
                                  &fallback_media_ua, &fallback_kind,
                                  fallback_media, sizeof fallback_media);
    }

    /* Modern watch pages often omit streamingData even for a public live
     * stream. The same page still publishes its Innertube API key and current
     * WEB client version; use those to ask the lightweight player endpoint for
     * the authoritative streamingData object. */
    if (!mr_youtube_extract_video_id(url, video_id) ||
        !extract_config_string(html, "INNERTUBE_API_KEY",
                               api_key, sizeof api_key) ||
        (!extract_config_string(html, "INNERTUBE_CLIENT_VERSION",
                                client_version, sizeof client_version) &&
         !extract_config_string(html, "INNERTUBE_CONTEXT_CLIENT_VERSION",
                                client_version, sizeof client_version))) {
        mr_free(html);
        mr_source_set_error("YouTube page omitted player API configuration");
        return 0;
    }
    mr_free(html);
    html = NULL;
    n = snprintf(api_url, sizeof api_url,
                 "https://www.youtube.com/youtubei/v1/player?key=%s&prettyPrint=false",
                 api_key);
    if (n <= 0 || (size_t)n >= sizeof api_url) {
        mr_source_set_error("YouTube player API URL is too long");
        return 0;
    }
    /* Streamlink's current live-only YouTube implementation uses this Android
     * profile without a JavaScript challenge solver. Prefer it because its HLS
     * response can avoid the WEB client's /n/ path challenge entirely. */
    n = snprintf(json, sizeof json,
                 "{\"videoId\":\"%s\",\"contentCheckOk\":true,"
                 "\"racyCheckOk\":true,\"context\":{\"client\":{"
                 "\"clientName\":\"ANDROID\",\"clientVersion\":\"%s\","
                 "\"platform\":\"DESKTOP\",\"clientScreen\":\"EMBED\","
                 "\"clientFormFactor\":\"UNKNOWN_FORM_FACTOR\","
                 "\"browserName\":\"Chrome\"},\"user\":{"
                 "\"lockedSafetyMode\":\"false\"},\"request\":{"
                 "\"useSsl\":\"true\"}}}",
                 video_id, YOUTUBE_ANDROID_VERSION);
    if (n <= 0 || (size_t)n >= sizeof json) {
        mr_source_set_error("YouTube Android player request is too large");
        return 0;
    }
    result = try_player_media(api_url, &fetch_options, json, "ANDROID",
                              YOUTUBE_ANDROID_UA, out, out_size, prefer_720p,
                              kind);
    if (result > 0) return 1;
    if (result == -2)
        keep_progressive_fallback("ANDROID", YOUTUBE_ANDROID_UA, out, *kind,
                                  &have_fallback, &fallback_client,
                                  &fallback_media_ua, &fallback_kind,
                                  fallback_media, sizeof fallback_media);
    else if (result < 0) saw_n_challenge = 1;

    /* Android VR still exposes the classic muxed 360p MP4 on many public
     * recorded videos. This is MintVID's deliberately narrow VOD target:
     * itag 18 contains H.264 video and AAC audio in one seekable file. */
    n = snprintf(json, sizeof json,
                 "{\"videoId\":\"%s\",\"contentCheckOk\":true,"
                 "\"racyCheckOk\":true,\"context\":{\"client\":{"
                 "\"clientName\":\"ANDROID_VR\","
                 "\"clientVersion\":\"%s\",\"deviceMake\":\"Oculus\","
                 "\"deviceModel\":\"Quest 3\",\"androidSdkVersion\":32,"
                 "\"userAgent\":\"%s\",\"osName\":\"Android\","
                 "\"osVersion\":\"12L\"}}}",
                 video_id, YOUTUBE_ANDROID_VR_VERSION, YOUTUBE_ANDROID_VR_UA);
    if (n <= 0 || (size_t)n >= sizeof json) {
        mr_source_set_error("YouTube Android VR request is too large");
        return 0;
    }
    if (!mr_http_options_init(&vr_options, YOUTUBE_ANDROID_VR_UA,
                              YOUTUBE_REFERER))
        return 0;
    result = try_player_media(api_url, &vr_options, json, "ANDROID_VR",
                              YOUTUBE_ANDROID_VR_UA, out, out_size,
                              prefer_720p, kind);
    if (result > 0) return 1;
    if (result == -2)
        keep_progressive_fallback("ANDROID_VR", YOUTUBE_ANDROID_VR_UA, out,
                                  *kind, &have_fallback, &fallback_client,
                                  &fallback_media_ua, &fallback_kind,
                                  fallback_media, sizeof fallback_media);
    else if (result < 0) saw_n_challenge = 1;

    /* Prefer the embedded WEB client next for embeddable public streams; its
     * thirdParty context identifies a genuine external embed origin. Live HLS
     * currently does not require a GVS Proof-of-Origin token, so a later media
     * 403 must not automatically be diagnosed as a missing token. */
    n = snprintf(json, sizeof json,
                 "{\"context\":{\"client\":{"
                 "\"clientName\":\"WEB_EMBEDDED_PLAYER\","
                 "\"clientVersion\":\"%s\",\"hl\":\"en\",\"gl\":\"GB\"},"
                 "\"thirdParty\":{\"embedUrl\":\"https://www.reddit.com/\"}},"
                 "\"videoId\":\"%s\",\"contentCheckOk\":true,"
                 "\"racyCheckOk\":true}", client_version, video_id);
    if (n <= 0 || (size_t)n >= sizeof json) {
        mr_source_set_error("YouTube player API request is too large");
        return 0;
    }
    result = try_player_media(api_url, &fetch_options, json,
                              "WEB_EMBEDDED_PLAYER", YOUTUBE_BROWSER_UA,
                              out, out_size, prefer_720p, kind);
    if (result > 0) return 1;
    if (result == -2)
        keep_progressive_fallback("WEB_EMBEDDED_PLAYER", YOUTUBE_BROWSER_UA,
                                  out, *kind, &have_fallback, &fallback_client,
                                  &fallback_media_ua, &fallback_kind,
                                  fallback_media, sizeof fallback_media);
    else if (result < 0) saw_n_challenge = 1;

    /* Non-embeddable streams may reject WEB_EMBEDDED_PLAYER. Keep the normal
     * WEB request as a compatibility fallback, although deployments enforcing
     * a GVS PO token can still reject its eventual media segments. */
    n = snprintf(json, sizeof json,
                 "{\"context\":{\"client\":{\"clientName\":\"WEB\","
                 "\"clientVersion\":\"%s\",\"hl\":\"en\",\"gl\":\"GB\"}},"
                 "\"videoId\":\"%s\",\"contentCheckOk\":true,"
                 "\"racyCheckOk\":true}", client_version, video_id);
    if (n <= 0 || (size_t)n >= sizeof json)
        return 0;
    result = try_player_media(api_url, &fetch_options, json, "WEB",
                              YOUTUBE_BROWSER_UA, out, out_size, prefer_720p,
                              kind);
    if (result > 0) return 1;
    if (result == -2)
        keep_progressive_fallback("WEB", YOUTUBE_BROWSER_UA, out, *kind,
                                  &have_fallback, &fallback_client,
                                  &fallback_media_ua, &fallback_kind,
                                  fallback_media, sizeof fallback_media);
    else if (result < 0) saw_n_challenge = 1;

    /* No client offered the requested 720p+ stream. Use the muxed 360p
     * fallback an earlier client already supplied rather than failing or
     * re-fetching everything from scratch. */
    if (have_fallback) {
        size_t len = strlen(fallback_media);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, fallback_media, len);
        out[len] = '\0';
        g_last_client = fallback_client;
        g_last_media_ua = fallback_media_ua;
        g_last_kind = fallback_kind;
        *kind = fallback_kind;
        printf("YouTube: no client offered 720p; using muxed 360p from %s\n",
               fallback_client);
        return 1;
    }
    mr_source_set_error(saw_n_challenge
        ? "YouTube media URL requires a player n challenge"
        : "YouTube returned no compatible live HLS or muxed MP4");
    return 0;
}

int mr_youtube_resolve_live(const char *url,
                            const mr_http_options *options,
                            char *out, size_t out_size)
{
    mr_youtube_media_kind kind;
    if (!mr_youtube_resolve_media(url, options, out, out_size, &kind))
        return 0;
    if (kind == MR_YOUTUBE_MEDIA_HLS) return 1;
    out[0] = '\0';
    mr_source_set_error("YouTube URL is not a public live HLS stream");
    g_last_client = "";
    g_last_kind = MR_YOUTUBE_MEDIA_NONE;
    return 0;
}
