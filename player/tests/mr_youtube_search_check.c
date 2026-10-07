#include "../youtube/mr_youtube_search.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                             \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL: %s\n", message);                         \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* Exercise the public parser, including the rows shared by both Amiga GUIs. */
static void check_text(const char *text, const char *expected)
{
    static const char prefix[] =
        "{\"videoRenderer\":{\"videoId\":\"LATIN123456\","
        "\"title\":{\"simpleText\":\"";
    static const char suffix[] = "\"}}}";
    size_t size = strlen(prefix) + strlen(text) + strlen(suffix);
    char *document = (char *)malloc(size + 1);
    mr_youtube_search_results results;
    CHECK(document != NULL, "allocate text fixture");
    if (!document) return;
    snprintf(document, size + 1, "%s%s%s", prefix, text, suffix);
    mr_youtube_search_results_init(&results);
    /* Exact-size, non-NUL-terminated input catches unbounded UTF-8 reads. */
    {
        char *bounded = (char *)malloc(size);
        CHECK(bounded != NULL, "allocate bounded fixture");
        if (!bounded) { free(document); return; }
        memcpy(bounded, document, size);
        CHECK(mr_youtube_search_parse(&results, bounded, size, 0),
              "parse text fixture");
        free(bounded);
    }
    if (expected) {
        CHECK(results.count == 1, "valid Unicode title retained");
        if (results.count) {
            CHECK(!strcmp(results.items[0].title, expected),
                  "title converted to Latin-1");
            CHECK(!strcmp(results.items[0].row, expected),
                  "display row converted to Latin-1");
        }
    } else {
        CHECK(results.count == 0, "invalid Unicode title rejected");
    }
    mr_youtube_search_results_free(&results);
    free(document);
}

static void check_unicode(void)
{
    unsigned value;
    char raw[3], escaped[7], expected[2];
    for (value = 128; value <= 255; value++) {
        raw[0] = (char)(0xc0 | (value >> 6));
        raw[1] = (char)(0x80 | (value & 63));
        raw[2] = 0;
        snprintf(escaped, sizeof(escaped), "\\u%04x", value);
        expected[0] = (char)value;
        expected[1] = 0;
        check_text(raw, expected);
        check_text(escaped, expected);
    }
    check_text("Caf\xc3\xa9 \xc3\xa8 \xc3\xa0 \xc3\xb1 \xc3\xbc \xc3\x9f",
               "Caf\xe9 \xe8 \xe0 \xf1 \xfc \xdf");
    check_text("A\xe2\x82\xac \xf0\x9f\x98\x80 Z", "A? ? Z");
    check_text("A\\u20ac \\ud83d\\ude00 Z", "A? ? Z");
    check_text("\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf"
               "\xf0\x90\x80\x80\xf4\x8f\xbf\xbf", "\x80?????");
    check_text("A\\u0000\\n\\tB", "A?  B");
    check_text("\xc0\xaf", NULL);       /* overlong */
    check_text("\xe0\x80\xaf", NULL); /* overlong */
    check_text("\xf0\x80\x80\xaf", NULL);
    check_text("\xed\xa0\x80", NULL); /* surrogate */
    check_text("\xf4\x90\x80\x80", NULL); /* beyond U+10FFFF */
    check_text("\xf5\x80\x80\x80", NULL);
    check_text("\x80", NULL);           /* stray continuation */
    check_text("\xc3", NULL);           /* truncated */
    check_text("\xe2\x82", NULL);
    check_text("\xf0\x9f\x98", NULL);
    check_text("\xc3" "A", NULL);      /* bad continuation */
    check_text("\\ud83d", NULL);
    check_text("\\ude00", NULL);
    check_text("\\ud83d\\u00e9", NULL);
    check_text("\\u00e", NULL);
    check_text("\\u00xz", NULL);

    {
        char long_text[2 * MR_YOUTUBE_SEARCH_TITLE_MAX + 1];
        char truncated[MR_YOUTUBE_SEARCH_TITLE_MAX];
        size_t i;
        for (i = 0; i < MR_YOUTUBE_SEARCH_TITLE_MAX; i++) {
            long_text[2 * i] = (char)0xc3;
            long_text[2 * i + 1] = (char)0xa9;
        }
        long_text[2 * i] = 0;
        memset(truncated, 0xe9, sizeof(truncated) - 1);
        truncated[sizeof(truncated) - 1] = 0;
        check_text(long_text, truncated);
    }
}

int main(void)
{
    static const char fixture[] =
        "<html><script>var ytInitialData={\"contents\":["
        "{\"videoRenderer\":{\"videoId\":\"LIVE1234567\","
        "\"title\":{\"runs\":[{\"text\":\"News \\u0026 weather\"}]},"
        "\"ownerText\":{\"runs\":[{\"text\":\"Test Channel\"}]},"
        "\"navigationEndpoint\":{\"browseEndpoint\":{"
        "\"browseId\":\"UCTEST_channel-1234567890\"}},"
        "\"badges\":[{\"metadataBadgeRenderer\":{"
        "\"style\":\"BADGE_STYLE_TYPE_LIVE_NOW\"}}]}},"
        "{\"videoRenderer\":{\"videoId\":\"VIDEO123456\","
        "\"title\":{\"simpleText\":\"Recorded video\"},"
        "\"ownerText\":{\"runs\":[{\"text\":\"Archive\"}]}}},"
        "{\"videoRenderer\":{\"videoId\":\"LIVE1234567\","
        "\"title\":{\"runs\":[{\"text\":\"Duplicate\"}]},"
        "\"badges\":[{\"metadataBadgeRenderer\":{"
        "\"style\":\"BADGE_STYLE_TYPE_LIVE_NOW\"}}]}}]};</script></html>";
    static const char channel_fixture[] =
        "{\"contents\":[{\"gridVideoRenderer\":{"
        "\"videoId\":\"CHANV123456\","
        "\"title\":{\"simpleText\":\"Channel upload\"}}}]}";
    static const char shorts_fixture[] =
        "{\"items\":[{\"shortsLockupViewModel\":{"
        "\"entityId\":\"shorts-shelf-item-SHORT123456\","
        "\"onTap\":{\"reelWatchEndpoint\":{"
        "\"videoId\":\"SHORT123456\"}},"
        "\"overlayMetadata\":{\"primaryText\":{"
        "\"content\":\"Tiny Amiga adventure\"}}}}]}";
    static const char unicode_fixture[] =
        "{\"items\":[{\"videoRenderer\":{\"videoId\":\"LATIN123456\","
        "\"title\":{\"simpleText\":\"Caf\xc3\xa9 \\u00e8\"},"
        "\"ownerText\":{\"runs\":[{\"text\":\"Fran\xc3\xa7" "ais\"}]},"
        "\"badges\":[{\"style\":\"LIVE\"}]}},"
        "{\"shortsLockupViewModel\":{\"videoId\":\"SHORT123456\","
        "\"primaryText\":{\"content\":\"Espa\xc3\xb1" "a \\ud83d\\ude00\"}}}]}";
    mr_youtube_search_results results;
    char url[512];

    mr_youtube_search_results_init(&results);
    check_unicode();
    CHECK(mr_youtube_search_build_url(url, sizeof(url), "Amiga live!", 1),
          "build live search URL");
    CHECK(strstr(url, "Amiga%20live%21") != NULL, "URL encodes query");
    CHECK(strstr(url, "sp=EgJAAQ%253D%253D") != NULL,
          "live search includes YouTube live filter");
    CHECK(mr_youtube_search_build_url_mode(
              url, sizeof(url), "Amiga", MR_YOUTUBE_SEARCH_VIDEOS),
          "build long-form video search URL");
    CHECK(strstr(url, "sp=EgIQAQ%253D%253D") != NULL,
          "video search includes YouTube Videos filter");
    CHECK(mr_youtube_search_build_url_mode(
              url, sizeof(url), "Amiga", MR_YOUTUBE_SEARCH_SHORTS),
          "build Shorts search URL");
    CHECK(strstr(url, "sp=EgIQCQ%253D%253D") != NULL,
          "Shorts search includes YouTube Shorts filter");
    CHECK(mr_youtube_search_build_url_mode(
              url, sizeof(url), "#Amiga", MR_YOUTUBE_SEARCH_HASHTAGS),
          "build hashtag page URL");
    CHECK(!strcmp(url, "https://www.youtube.com/hashtag/Amiga"),
          "hashtag search strips optional leading hash");
    CHECK(mr_youtube_search_build_url_mode(
              url, sizeof(url), "Amiga", MR_YOUTUBE_SEARCH_HASHTAGS),
          "build hashtag URL without leading hash");
    CHECK(!strcmp(url, "https://www.youtube.com/hashtag/Amiga"),
          "plain hashtag name accepted");
    CHECK(!mr_youtube_search_build_url_mode(
              url, sizeof(url), "Amiga demo", MR_YOUTUBE_SEARCH_HASHTAGS),
          "hashtag search rejects spaces");
    CHECK(strstr(mr_youtube_search_last_error(), "without spaces") != NULL,
          "hashtag-space error is useful");

    CHECK(mr_youtube_search_parse(&results, fixture, strlen(fixture), 1),
          "parse live-only fixture");
    CHECK(results.count == 1, "live-only parse filters and deduplicates");
    if (results.count) {
        CHECK(!strcmp(results.items[0].video_id, "LIVE1234567"),
              "video id extracted");
        CHECK(!strcmp(results.items[0].title, "News & weather"),
              "JSON title decoded");
        CHECK(!strcmp(results.items[0].channel, "Test Channel"),
              "channel extracted");
        CHECK(!strcmp(results.items[0].channel_id,
                      "UCTEST_channel-1234567890"), "channel ID extracted");
        CHECK(results.items[0].live, "live badge detected");
        CHECK(mr_youtube_search_watch_url(url, sizeof(url), &results.items[0]),
              "watch URL built");
        CHECK(!strcmp(url,
                      "https://www.youtube.com/watch?v=LIVE1234567"),
              "watch URL correct");
        CHECK(mr_youtube_channel_videos_url(url, sizeof(url),
                                            &results.items[0]),
              "channel videos URL built");
        CHECK(!strcmp(url, "https://www.youtube.com/channel/"
                           "UCTEST_channel-1234567890/videos"),
              "channel videos URL correct");
    }
    mr_youtube_search_results_free(&results);

    CHECK(mr_youtube_search_parse(&results, unicode_fixture,
                                  strlen(unicode_fixture), 0),
          "parse mixed raw UTF-8 and escaped Unicode");
    CHECK(results.count == 2, "Unicode video and Shorts retained");
    if (results.count == 2) {
        CHECK(!strcmp(results.items[0].channel, "Fran\xe7" "ais"),
              "channel name converted to Latin-1");
        CHECK(!strcmp(results.items[0].row,
                      "[LIVE] Caf\xe9 \xe8 - Fran\xe7" "ais"),
              "live row preserves Latin-1 title and channel");
        CHECK(!strcmp(results.items[1].row, "[SHORT] Espa\xf1" "a ?"),
              "Shorts title converted with one placeholder per emoji");
    }
    mr_youtube_search_results_free(&results);

    CHECK(mr_youtube_search_parse_mode(
              &results, shorts_fixture, strlen(shorts_fixture),
              MR_YOUTUBE_SEARCH_SHORTS),
          "parse Shorts fixture");
    CHECK(results.count == 1, "Shorts renderer extracted");
    if (results.count) {
        CHECK(!strcmp(results.items[0].video_id, "SHORT123456"),
              "Shorts video ID extracted");
        CHECK(!strcmp(results.items[0].title, "Tiny Amiga adventure"),
              "Shorts title extracted");
        CHECK(strstr(results.items[0].row, "[SHORT]") != NULL,
              "Shorts result labelled");
    }
    mr_youtube_search_results_free(&results);

    CHECK(mr_youtube_search_parse_mode(
              &results, shorts_fixture, strlen(shorts_fixture),
              MR_YOUTUBE_SEARCH_ALL),
          "all search accepts Shorts renderers");
    CHECK(results.count == 1, "all search includes Shorts");
    mr_youtube_search_results_free(&results);

    CHECK(mr_youtube_search_parse_mode(
              &results, fixture, strlen(fixture),
              MR_YOUTUBE_SEARCH_HASHTAGS),
          "parse hashtag-page video renderers");
    CHECK(results.count == 2,
          "hashtag mode includes normal video renderers");
    mr_youtube_search_results_free(&results);

    CHECK(mr_youtube_search_parse(&results, channel_fixture,
                                  strlen(channel_fixture), 0),
          "parse channel grid-video fixture");
    CHECK(results.count == 1 &&
          !strcmp(results.items[0].video_id, "CHANV123456"),
          "gridVideoRenderer channel upload extracted");
    mr_youtube_search_results_free(&results);

    CHECK(mr_youtube_search_parse(&results, fixture, strlen(fixture), 0),
          "parse all-result fixture");
    CHECK(results.count == 2, "all-result parse includes recorded video");
    if (results.count == 2)
        CHECK(!results.items[1].live, "recorded video is not marked live");
    mr_youtube_search_results_free(&results);

    CHECK(!mr_youtube_search_build_url(url, sizeof(url), "", 1),
          "empty query rejected");
    CHECK(strstr(mr_youtube_search_last_error(), "Enter") != NULL,
          "empty-query error is useful");

    {
        mr_youtube_search_result pasted;
        static const char *const good[] = {
            "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
            "http://youtube.com/watch?v=dQw4w9WgXcQ",
            "  https://m.youtube.com/watch?v=dQw4w9WgXcQ&t=30s",
            "https://youtu.be/dQw4w9WgXcQ",
            "https://youtu.be/dQw4w9WgXcQ?si=abc123",
            "www.youtube.com/live/dQw4w9WgXcQ",
            "https://www.youtube.com/shorts/dQw4w9WgXcQ",
            "https://www.youtube.com/embed/dQw4w9WgXcQ",
            "https://www.youtube.com/watch?list=PL1&v=dQw4w9WgXcQ&index=2",
        };
        static const char *const bad[] = {
            "dQw4w9WgXcQ",                                /* bare id, no URL */
            "https://www.youtube.com/results?search_query=x",
            "https://www.youtube.com/watch?v=short",       /* id too short   */
            "https://www.youtube.com/watch?v=dQw4w9WgXcQX", /* id too long   */
            "https://example.com/watch?v=dQw4w9WgXcQ",
            "",
        };
        size_t i;
        for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
            CHECK(mr_youtube_url_parse(&pasted, good[i]), good[i]);
            CHECK(!strcmp(pasted.video_id, "dQw4w9WgXcQ"), good[i]);
        }
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
            CHECK(!mr_youtube_url_parse(&pasted, bad[i]), bad[i]);
    }

    if (failures)
        return 1;
    puts("YouTube search checks passed");
    return 0;
}
