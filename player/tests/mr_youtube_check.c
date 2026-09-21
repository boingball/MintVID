#include "../core/mr_youtube.h"
#include "../core/mr_alloc.h"
#include "../core/mr_http.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int mock_mode, mock_gets, mock_safari, mock_android;

static int fake_youtube(const char *url, const mr_http_options *options,
                        const char *post_json, unsigned char **out,
                        size_t *out_len, size_t max_size)
{
    const char *body = NULL;
    (void)max_size;
    if (!post_json && strstr(url, "youtube.com/watch?")) {
        mock_gets++;
        body = (mock_mode == 3 || mock_mode == 4)
            ? "{\"INNERTUBE_API_KEY\":\"testkey\","
              "\"INNERTUBE_CLIENT_VERSION\":\"2.test\","
              "\"streamingData\":{\"formats\":[{\"itag\":18,"
              "\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001E, "
              "mp4a.40.2\\\"\",\"url\":\"https://r1.googlevideo.com/"
              "watch-mp4\"}]}}"
            : "{\"INNERTUBE_API_KEY\":\"testkey\","
              "\"INNERTUBE_CLIENT_VERSION\":\"2.test\"}";
    } else if (post_json && strstr(post_json, "\"clientName\":\"WEB\"") &&
               !strstr(post_json, "thirdParty") &&
               strstr(post_json, "Safari/605.1.15") &&
               strstr(options->user_agent, "Safari/605.1.15")) {
        mock_safari++;
        if (mock_mode == 2)
            body = "{\"videoDetails\":{\"isLiveContent\":true},"
                   "\"streamingData\":{\"hlsManifestUrl\":\"https://"
                   "manifest.googlevideo.com/vod/index.m3u8\"}}";
        else if (mock_mode == 1 || mock_mode == 4)
            body = "{\"videoDetails\":{\"isLiveContent\":false}}";
        else if (mock_mode == 5)
            body = "{\"playabilityStatus\":{\"status\":\"ERROR\"}}";
        else
            body = "{\"videoDetails\":{\"isLiveContent\":false},"
                   "\"streamingData\":{\"hlsManifestUrl\":\"https://"
                   "manifest.googlevideo.com/vod/index.m3u8\"}}";
    } else if (post_json && strstr(post_json, "\"clientName\":\"ANDROID\"")) {
        mock_android++;
        body = "{\"streamingData\":{\"formats\":[{\"itag\":18,"
               "\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001E, "
               "mp4a.40.2\\\"\",\"url\":\"https://r1.googlevideo.com/"
               "mp4\"}]}}";
    }
    if (!body) return 0;
    *out_len = strlen(body);
    *out = (unsigned char *)mr_alloc(*out_len + 1);
    if (!*out) return 0;
    memcpy(*out, body, *out_len + 1);
    return 1;
}

static void expect(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

int main(int argc, char **argv)
{
    char out[1024];
    char video_id[12];
    mr_youtube_media_kind media_kind;
    mr_http_options base_options, youtube_options;
    const char *raw =
        "before \"hlsManifestUrl\":\"https://manifest.googlevideo.com/"
        "api/manifest/hls_variant/file/index.m3u8\" after";
    const char *escaped =
        "{\n \"hlsManifestUrl\" : \"https:\\/\\/manifest.googlevideo.com/"
        "api\\/manifest\\/hls_variant\\/index.m3u8?x=1\\u0026y=2\"}";
    const char *foreign =
        "\"hlsManifestUrl\":\"https://evil.example/live/index.m3u8\"";
    const char *progressive =
        "{\"streamingData\":{\"formats\":["
        "{\"itag\":17,\"mimeType\":\"video/3gpp; codecs=\\\"mp4v.20.3, "
        "mp4a.40.2\\\"\",\"url\":\"https://r1.googlevideo.com/low\"},"
        "{\"itag\":18,\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001E, "
        "mp4a.40.2\\\"\",\"width\":640,\"height\":360,"
        "\"url\":\"https://r2---sn-test.googlevideo.com/videoplayback?"
        "expire=1\\u0026sig=ok\"}]}}";
    const char *progressive_hd =
        "{\"streamingData\":{\"formats\":["
        "{\"itag\":18,\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001E, "
        "mp4a.40.2\\\"\",\"url\":\"https://r1.googlevideo.com/360\"},"
        "{\"itag\":22,\"mimeType\":\"video/mp4; codecs=\\\"avc1.64001F, "
        "mp4a.40.2\\\"\",\"width\":1280,\"height\":720,"
        "\"url\":\"https://r2.googlevideo.com/720\"}]}}";

    if (argc == 2 || (argc == 3 && !strcmp(argv[1], "--post"))) {
        char *html = NULL;
        size_t html_len = 0;
        int fetched = argc == 2
                    ? mr_http_fetch_text(argv[1], NULL, &html, &html_len, 65536)
                    : mr_http_post_json(argv[2], NULL, "{}", &html,
                                        &html_len, 65536);
        if (!fetched ||
            !html_len || !mr_youtube_extract_live_manifest(html, out,
                                                            sizeof out)) {
            fprintf(stderr, "YouTube HTTP fixture failed\n");
            mr_free(html);
            return 1;
        }
        mr_free(html);
        puts("YouTube HTTP fixture passed");
        return 0;
    }

    expect(mr_youtube_is_url("https://www.youtube.com/watch?v=abc"),
           "www.youtube.com accepted");
    expect(mr_youtube_is_url("https://youtu.be/abc"), "youtu.be accepted");
    expect(mr_youtube_is_url("http://m.youtube.com/live/abc"),
           "mobile YouTube accepted");
    expect(!mr_youtube_is_url("https://notyoutube.com/watch?v=abc"),
           "lookalike host rejected");
    expect(!mr_youtube_is_url("https://youtube.com.evil.test/watch?v=abc"),
           "host suffix attack rejected");
    expect(mr_http_options_init(&base_options, NULL, NULL),
           "generic HTTP options initialised");
    base_options.hls_max_width = 640;
    base_options.hls_max_height = 360;
    base_options.source_buffer_bytes = 8u * 1024u * 1024u;
    expect(mr_youtube_http_options_init(&youtube_options, &base_options) &&
           strstr(youtube_options.user_agent, "Mozilla/5.0") &&
           !strcmp(youtube_options.referer, "https://www.youtube.com/") &&
           youtube_options.hls_max_width == 640 &&
           youtube_options.hls_max_height == 360 &&
           youtube_options.source_buffer_bytes == 8u * 1024u * 1024u &&
           youtube_options.hls_live_start_segments == 2 &&
           youtube_options.hls_buffer_segments,
           "YouTube browser defaults, live edge and HLS limits applied");
    expect(mr_http_options_init(&base_options, "Custom Agent",
                                "https://custom.example/") &&
           mr_youtube_http_options_init(&youtube_options, &base_options) &&
           !strcmp(youtube_options.user_agent, "Custom Agent") &&
           !strcmp(youtube_options.referer, "https://custom.example/"),
           "explicit YouTube headers preserved");
    expect(mr_youtube_extract_video_id(
               "https://www.youtube.com/watch?v=EvsLqQS_80E", video_id) &&
           !strcmp(video_id, "EvsLqQS_80E"), "watch video ID extracted");
    expect(mr_youtube_extract_video_id(
               "https://www.youtube.com/live/EvsLqQS_80E?si=test", video_id) &&
           !strcmp(video_id, "EvsLqQS_80E"), "live video ID extracted");
    expect(mr_youtube_extract_video_id(
               "https://youtu.be/EvsLqQS_80E", video_id) &&
           !strcmp(video_id, "EvsLqQS_80E"), "share video ID extracted");

    expect(mr_youtube_extract_live_manifest(raw, out, sizeof out) &&
           !strcmp(out, "https://manifest.googlevideo.com/api/manifest/"
                        "hls_variant/file/index.m3u8"),
           "plain manifest extracted");
    expect(mr_youtube_extract_live_manifest(escaped, out, sizeof out) &&
           !strcmp(out, "https://manifest.googlevideo.com/api/manifest/"
                        "hls_variant/index.m3u8?x=1&y=2"),
           "JSON escapes decoded");
    expect(!mr_youtube_extract_live_manifest(foreign, out, sizeof out),
           "foreign manifest host rejected");
    expect(!mr_youtube_extract_live_manifest(
               "\"hlsManifestUrl\":\"https://manifest.googlevideo.com/"
               "live/index.m3u8\\nInjected: yes\"", out, sizeof out),
           "escaped control character rejected");
    expect(!mr_youtube_extract_live_manifest("<html>ordinary video</html>",
                                             out, sizeof out),
           "non-live page rejected");
    expect(!mr_youtube_extract_live_manifest(raw, out, 24),
           "truncated output rejected");
    expect(mr_youtube_extract_progressive_360p(progressive, out,
                                                sizeof out) &&
           !strcmp(out, "https://r2---sn-test.googlevideo.com/videoplayback?"
                        "expire=1&sig=ok"),
           "muxed progressive 360p MP4 extracted");
    expect(mr_youtube_extract_progressive(progressive_hd, 1, out,
                                           sizeof out, &media_kind) &&
           media_kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_720P &&
           !strcmp(out, "https://r2.googlevideo.com/720"),
           "muxed progressive 720p MP4 preferred");
    expect(mr_youtube_extract_progressive(progressive_hd, 0, out,
                                           sizeof out, &media_kind) &&
           media_kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_360P &&
           !strcmp(out, "https://r1.googlevideo.com/360"),
           "360p retained for lower quality setting");
    expect(mr_youtube_extract_progressive(progressive, 1, out,
                                           sizeof out, &media_kind) &&
           media_kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_360P,
           "missing 720p automatically falls back to 360p");
    expect(!mr_youtube_extract_progressive_360p(
               "{\"formats\":[{\"itag\":18,\"mimeType\":\"video/mp4; "
               "codecs=\\\"avc1.42001E, mp4a.40.2\\\"\","
               "\"url\":\"https://evil.example/videoplayback\"}]}",
               out, sizeof out), "foreign progressive host rejected");
    expect(!mr_youtube_extract_progressive_360p(
               "{\"formats\":[{\"itag\":18,\"mimeType\":\"video/mp4; "
               "codecs=\\\"avc1.42001E, mp4a.40.2\\\"\","
               "\"url\":\"https://r1.googlevideo.com/videoplayback?x=1"
               "\\u0026n=unsolved\"}]}", out, sizeof out),
           "progressive n challenge rejected");
    expect(!mr_youtube_extract_progressive_360p(
               "{\"formats\":[{\"itag\":18,\"mimeType\":\"video/mp4; "
               "codecs=\\\"avc1.42001E, mp4a.40.2\\\"\","
               "\"signatureCipher\":\"url=hidden\"}]}", out, sizeof out),
           "cipher-only progressive format rejected");
    expect(!mr_youtube_extract_progressive_360p(
               "{\"adaptiveFormats\":[{\"itag\":134,"
               "\"mimeType\":\"video/mp4; codecs=\\\"avc1.4d401e\\\"\","
               "\"url\":\"https://r1.googlevideo.com/video-only\"}]}",
               out, sizeof out), "adaptive video-only format rejected");
    expect(!mr_youtube_extract_progressive_360p(progressive, out, 24),
           "truncated progressive output rejected");
    expect(!strcmp(mr_youtube_last_client(), ""),
           "client diagnostic empty before a successful resolution");

    mr_http_set_fetch_override(fake_youtube);
    expect(mr_http_options_init(&base_options, NULL, NULL),
           "resolver mock options initialised");
    base_options.hls_low = 1;
    mock_mode = mock_gets = mock_safari = mock_android = 0;
    expect(mr_youtube_resolve_media(
               "https://www.youtube.com/watch?v=EvsLqQS_80E",
               &base_options, out, sizeof out, &media_kind) &&
           media_kind == MR_YOUTUBE_MEDIA_HLS_VOD &&
           strstr(out, "/vod/index.m3u8") &&
           mock_gets == 1 && mock_safari == 1 && mock_android == 0 &&
           mr_youtube_media_http_options_init(&youtube_options,
                                               &base_options) &&
           strstr(youtube_options.user_agent, "Safari/605.1.15"),
           "Low selects original muxed recorded HLS with Safari headers");
    mock_gets = mock_safari = mock_android = 0;
    expect(mr_youtube_resolve_mp4_fallback(
               "https://www.youtube.com/watch?v=EvsLqQS_80E",
               &base_options, out, sizeof out, &media_kind) &&
           media_kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_360P &&
           mock_gets == 1 && mock_safari == 0 && mock_android == 1,
           "failed HLS retry skips Safari and finds original muxed MP4");
    mock_mode = 1;
    mock_gets = mock_safari = mock_android = 0;
    expect(mr_youtube_resolve_media(
               "https://www.youtube.com/watch?v=EvsLqQS_80E",
               &base_options, out, sizeof out, &media_kind) &&
           media_kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_360P &&
           mock_gets == 1 && mock_safari == 1 && mock_android == 1,
           "missing recorded HLS falls back to Android 360p");
    mock_mode = 2;
    mock_gets = mock_safari = mock_android = 0;
    expect(mr_youtube_resolve_media(
               "https://www.youtube.com/watch?v=EvsLqQS_80E",
               &base_options, out, sizeof out, &media_kind) &&
           media_kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_360P &&
           mock_android == 1,
           "Safari live HLS is not classified as recorded video");
    mock_mode = 5;
    mock_gets = mock_safari = mock_android = 0;
    expect(mr_youtube_resolve_media(
               "https://www.youtube.com/watch?v=EvsLqQS_80E",
               &base_options, out, sizeof out, &media_kind) &&
           media_kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_360P &&
           mock_safari == 1 && mock_android == 1,
           "Safari playability errors fall back to Android 360p");
    mock_mode = 3;
    mock_gets = mock_safari = mock_android = 0;
    expect(mr_youtube_resolve_media(
               "https://www.youtube.com/watch?v=EvsLqQS_80E",
               &base_options, out, sizeof out, &media_kind) &&
           media_kind == MR_YOUTUBE_MEDIA_HLS_VOD &&
           mock_safari == 1 && mock_android == 0,
           "watch-page MP4 does not prevent Low from trying 144p HLS");
    mock_mode = 4;
    mock_gets = mock_safari = mock_android = 0;
    expect(mr_youtube_resolve_media(
               "https://www.youtube.com/watch?v=EvsLqQS_80E",
               &base_options, out, sizeof out, &media_kind) &&
           media_kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_360P &&
           !strcmp(out, "https://r1.googlevideo.com/watch-mp4") &&
           mock_safari == 1 && mock_android == 0,
           "Low reuses the watch-page MP4 when Safari HLS is unavailable");
    base_options.hls_low = 0;
    base_options.hls_max_height = 360;
    mock_mode = mock_gets = mock_safari = mock_android = 0;
    expect(mr_youtube_resolve_media(
               "https://www.youtube.com/watch?v=EvsLqQS_80E",
               &base_options, out, sizeof out, &media_kind) &&
           media_kind == MR_YOUTUBE_MEDIA_PROGRESSIVE_360P &&
           mock_gets == 1 && mock_safari == 0 && mock_android == 1,
           "360p setting retains existing Android request order");
    mr_http_set_fetch_override(NULL);

    if (failures) return 1;
    puts("YouTube resolver checks passed");
    return 0;
}
