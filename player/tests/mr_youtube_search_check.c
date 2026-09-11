#include "../youtube/mr_youtube_search.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                             \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL: %s\n", message);                         \
            failures++;                                                       \
        }                                                                     \
    } while (0)

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
    mr_youtube_search_results results;
    char url[512];

    mr_youtube_search_results_init(&results);
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
