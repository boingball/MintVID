#include "../iptv/mr_iptv.h"
#include "../core/mr_play_options.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_generated(mr_iptv_directory *directory, size_t count,
                           size_t excluded_prefix) {
  size_t capacity = count * 64 + 2, used = 1, i;
  char *json = (char *)malloc(capacity);
  int result;
  assert(json);
  json[0] = '[';
  for (i = 0; i < count; i++)
    used += (size_t)snprintf(
        json + used, capacity - used,
        "%s{\"id\":\"C%lu\",\"name\":\"N\",\"is_nsfw\":%s}", i ? "," : "",
        (unsigned long)i, i < excluded_prefix ? "true" : "false");
  json[used++] = ']';
  result = mr_iptv_parse_channels(directory, json, used);
  free(json);
  return result;
}

static void write_streaming_fixture(const char *channels_path,
                                    const char *streams_path) {
  FILE *file;
  int i, j;
  file = fopen(channels_path, "wb");
  assert(file);
  fputc('[', file);
  for (i = 0; i < 25000; i++) {
    fprintf(file,
            "%s{\"id\":\"C%d\",\"name\":\"Channel %d\",\"country\":\"%s\","
            "\"is_nsfw\":false,\"closed\":null,\"replaced_by\":null",
            i ? "," : "", i, i,
            i < 300   ? "UK"
            : i < 400 ? "US"
                      : "ZZ");
    if (i == 0) {
      fputs(",\"padding\":\"", file);
      for (j = 0; j < 20000; j++)
        fputc('x', file);
      fputc('"', file);
    }
    fputc('}', file);
  }
  fputc(']', file);
  fclose(file);
  file = fopen(streams_path, "wb");
  assert(file);
  fputc('[', file);
  for (i = 0; i < 30000; i++) {
    const char *prefix = i ? "," : "";
    if (i == 0)
      fprintf(file,
              "%s{\"channel\":\"C0\",\"url\":\"https://example.test/direct\","
              "\"http_referrer\":null,\"user_agent\":null}", prefix);
    else if (i < 500)
      fprintf(file,
              "%s{\"channel\":\"C%d\",\"url\":\"https://example.test/%d.m3u8\","
              "\"http_referrer\":null,\"user_agent\":null}",
              prefix, i % 300, i);
    else if (i < 600)
      fprintf(file,
              "%s{\"channel\":\"C%d\",\"url\":\"https://example.test/%d.ts\"}",
              prefix, 300 + i - 500, i);
    else
      fprintf(file,
              "%s{\"channel\":\"Unrelated%d\",\"url\":\"https://example.test/"
              "%d.ts\"}",
              prefix, i, i);
  }
  fputc(']', file);
  fclose(file);
}

static size_t directory_bytes(const mr_iptv_directory *directory) {
  size_t bytes = directory->channel_capacity * sizeof(*directory->channels), i;
  for (i = 0; i < directory->channel_count; i++)
    bytes += (directory->channels[i].streams ? 4 : 0) *
                 sizeof(mr_iptv_stream) +
             (directory->channels[i].alt_count +
              directory->channels[i].category_count) *
                 MR_IPTV_NAME_MAX;
  return bytes;
}

int main(void) {
  mr_iptv_directory d;
  size_t ids[8];
  int channel_index;
  mr_iptv_filter f;
  const char *c =
      "[{\"id\":\"BBCNews.uk\",\"name\":\"BBC News\",\"alt_names\":[\"BBC "
      "World\"],\"network\":\"BBC\",\"country\":\"UK\",\"categories\":["
      "\"news\"],\"is_nsfw\":false,\"unknown\":{\"x\":[1]}},{\"id\":\"Fun.uk\","
      "\"name\":\"Fun "
      "\\u2603\",\"alt_names\":[],\"network\":\"Net\",\"country\":\"UK\","
      "\"categories\":[\"entertainment\"],\"is_nsfw\":false},{\"id\":\"US.us\","
      "\"name\":\"US "
      "One\",\"country\":\"US\",\"categories\":[\"news\"],\"alt_names\":[],"
      "\"is_nsfw\":false},{\"id\":\"Adult.uk\",\"name\":\"Adult\",\"country\":"
      "\"UK\",\"categories\":[],\"alt_names\":[],\"is_nsfw\":true},{\"id\":"
      "\"Closed.uk\",\"name\":\"Closed\",\"country\":\"UK\",\"categories\":[],"
      "\"alt_names\":[],\"is_nsfw\":false,\"closed\":\"2020\"},{\"id\":\"Empty."
      "uk\",\"name\":\"Empty\",\"country\":\"UK\",\"categories\":[],\"alt_"
      "names\":[],\"is_nsfw\":false}]";
  const char *s =
      "[{\"channel\":\"BBCNews.uk\",\"url\":\"https://one.test/"
      "live.m3u8\",\"status\":\"online\"},{\"channel\":\"BBCNews.uk\",\"url\":"
      "\"http://two.test/live\",\"http_referrer\":\"https://ref.test/"
      "\"},{\"channel\":\"BBCNews.uk\",\"url\":\"https://one.test/"
      "live.mpd?token=1\"},{\"channel\":\"Fun.uk@East\",\"url\":\"https://fun.test/"
      "a.ts\"},{\"channel\":\"US.us\",\"url\":\"not a "
      "url\"},{\"channel\":\"Adult.uk\",\"url\":\"http://adult.test/"
      "a\"},{\"channel\":\"Closed.uk\",\"url\":\"http://closed.test/a\"}]";
  const char *m = "#EXTM3U\n#EXTINF:-1 tvg-id=\"nasa.us\" tvg-name=\"NASA TV\" "
                  "group-title=\"Science\",NASA\nhttps://nasa.test/live.m3u8\n";
  const char *nullable_channels =
      "[{\"id\":\"Example.uk\",\"name\":\"Caf\xc3\xa9 TV\",\"alt_names\":[],"
      "\"network\":null,\"country\":\"UK\",\"categories\":[\"news\"],"
      "\"is_nsfw\":false,\"website\":null,\"closed\":null,"
      "\"replaced_by\":null,"
      "\"owner\":null,\"extra\":{\"nothing\":null,\"values\":[true,false,"
      "null,12.5]}}]  \n";
  const char *nullable_streams =
      "[{\"channel\":null,\"url\":\"https://unused.test/live.m3u8\","
      "\"http_referrer\":null,\"user_agent\":null,\"extra\":null},"
      "{\"channel\":\"Example.uk\",\"url\":\"https://example.com/live.m3u8\","
      "\"http_referrer\":null,\"user_agent\":null}]\t";
  assert(mr_path_is_audio_only("Music:track.MP3"));
  assert(mr_path_is_audio_only("song.flac?download=1"));
  assert(!mr_path_is_audio_only("video.mp4"));
  assert(!mr_path_is_audio_only("drawer.with.dot/video"));
  assert(mr_iptv_valid_url("https://example.test/live.mpd?token=1"));
  assert(!mr_iptv_supported_url("https://example.test/live.mpd?token=1"));
  assert(!mr_iptv_supported_url("https://example.test/LIVE.MPD"));
  assert(mr_iptv_supported_url("https://example.test/live.m3u8?token=1"));
  assert(mr_iptv_supported_url("https://example.test/extensionless"));
  mr_iptv_init(&d);
  assert(mr_iptv_parse_channels(&d, c, strlen(c)));
  assert(d.channel_count == 4);
  assert(mr_iptv_join_streams(&d, s, strlen(s)));
  assert(d.channel_count == 2);
  assert(d.channels[0].stream_count == 2);
  assert(!strstr(d.channels[0].streams[0].url, ".mpd"));
  assert(!strstr(d.channels[0].streams[1].url, ".mpd"));
  assert(d.channels[1].stream_count == 1);
  assert(!strcmp(d.channels[1].name, "Fun ?"));
  f.country = "UK";
  f.category = "news";
  f.search = "world";
  assert(mr_iptv_filter_channels(&d, &f, ids, 8) == 1 && ids[0] == 0);
  f.category = "All";
  f.search = "net";
  assert(mr_iptv_filter_channels(&d, &f, ids, 8) == 1);
  assert(!mr_iptv_parse_channels(&d, "[{", 2));
  assert(d.channel_count == 2);
  assert(!mr_iptv_join_streams(&d, "[", 1));
  {
    const char *object =
        "{\"id\":\"Direct.uk\",\"name\":\"Direct\",\"country\":\"UK\","
        "\"network\":\"Net\",\"alt_names\":[\"One\",\"Two\",\"Three\"],"
        "\"categories\":[\"news\"],\"is_nsfw\":false,\"closed\":null,"
        "\"replaced_by\":null}";
    const char *stream_object =
        "{\"channel\":\"Direct.uk\",\"url\":\"https://test/live.m3u8\","
        "\"http_referrer\":null,\"user_agent\":\"Agent\"}";
    mr_iptv_channel direct;
    mr_iptv_stream direct_stream;
    char channel_id[MR_IPTV_ID_MAX], error[256];
    assert(mr_iptv_parse_channel_object_for_country(
        object, strlen(object), "UK", &direct, error, sizeof(error)));
    assert(!strcmp(direct.id, "Direct.uk") && direct.alt_count == 2);
    free(direct.alt_names);
    free(direct.categories);
    assert(mr_iptv_parse_stream_object(
        stream_object, strlen(stream_object), channel_id, sizeof(channel_id),
        &direct_stream, error, sizeof(error)));
    assert(!strcmp(channel_id, "Direct.uk"));
    assert(!strcmp(direct_stream.user_agent, "Agent"));
    assert(!mr_iptv_parse_stream_object("{\"channel\":", 11, channel_id,
                                        sizeof(channel_id), &direct_stream,
                                        error, sizeof(error)));
    assert(error[0]);
  }
  mr_iptv_free(&d);
  mr_iptv_init(&d);
  assert(mr_iptv_parse_m3u(&d, m, strlen(m)));
  assert(d.channel_count == 1);
  assert(!strcmp(d.channels[0].id, "nasa.us"));
  assert(!strcmp(d.channels[0].categories[0], "Science"));
  assert(!mr_iptv_parse_m3u(&d, "bad", 3));
  assert(d.channel_count == 1);
  mr_iptv_free(&d);
  mr_iptv_init(&d);
  assert(
      mr_iptv_parse_channels(&d, nullable_channels, strlen(nullable_channels)));
  assert(d.channel_count == 1 && !d.channels[0].network[0]);
  assert(!d.channels[0].closed && !d.channels[0].replaced);
  assert(!strcmp(d.channels[0].name, "Caf\xc3\xa9 TV"));
  assert(mr_iptv_join_streams(&d, nullable_streams, strlen(nullable_streams)));
  assert(d.channels[0].stream_count == 1);
  assert(!d.channels[0].streams[0].http_referrer[0]);
  assert(!d.channels[0].streams[0].user_agent[0]);
  assert(mr_iptv_parse_channels(&d, "[{\"website\":null}]",
                                sizeof("[{\"website\":null}]") - 1));
  assert(mr_iptv_parse_channels(
      &d, "[{\"website\":\"https://example.com\"}]",
      sizeof("[{\"website\":\"https://example.com\"}]") - 1));
  assert(!mr_iptv_parse_channels(&d, "[{\"website\":123}]",
                                 sizeof("[{\"website\":123}]") - 1));
  assert(strstr(mr_iptv_last_error(), "field website"));
  assert(strstr(mr_iptv_last_error(), "byte 12"));
  assert(
      !mr_iptv_parse_channels(&d, "[{\"id\":\"x\",\"network\":nul}]",
                              sizeof("[{\"id\":\"x\",\"network\":nul}]") - 1));
  assert(strstr(mr_iptv_last_error(), "field network"));
  assert(strstr(mr_iptv_last_error(), "byte"));
  assert(
      !mr_iptv_parse_channels(&d, "[{\"id\":\"x\",\"network\":NULL}]",
                              sizeof("[{\"id\":\"x\",\"network\":NULL}]") - 1));
  assert(!mr_iptv_parse_channels(
      &d, "[{\"id\":\"x\",\"network\":null \"country\":\"UK\"}]",
      sizeof("[{\"id\":\"x\",\"network\":null \"country\":\"UK\"}]") - 1));
  assert(strstr(mr_iptv_last_error(), "',' or '}'"));
  assert(!mr_iptv_join_streams(
      &d, "[{\"channel\":\"Example.uk\",\"url\":none}]",
      sizeof("[{\"channel\":\"Example.uk\",\"url\":none}]") - 1));
  assert(strstr(mr_iptv_last_error(), "field url"));
  assert(parse_generated(&d, 8191, 0) && d.channel_count == 8191);
  assert(parse_generated(&d, 8192, 0) && d.channel_count == 8192);
  assert(parse_generated(&d, 8193, 0) && d.channel_count == 8193);
  assert(d.channel_capacity == 16384);
  assert(parse_generated(&d, 16384, 0) && d.channel_count == 16384);
  assert(parse_generated(&d, 19999, 0) && d.channel_count == 19999);
  assert(parse_generated(&d, 20000, 0) && d.channel_count == 20000);
  assert(parse_generated(&d, 20001, 0));
  assert(d.channel_count == 20000);
  assert(d.parsed_channel_count == 20001 && d.skipped_channel_count == 1);
  assert(parse_generated(&d, 25000, 0));
  assert(d.channel_count == 20000 && d.skipped_channel_count == 5000);
  assert(parse_generated(&d, 20100, 100));
  assert(d.channel_count == 20000 && d.skipped_channel_count == 100);
  assert(!strcmp(d.channels[19999].id, "C20099"));
  write_streaming_fixture("/tmp/mr_channels.json", "/tmp/mr_streams.json");
  assert(mr_iptv_load_country_files(&d, "/tmp/mr_channels.json",
                                    "/tmp/mr_streams.json", "UK"));
  assert(d.parsed_channel_count == 25000);
  assert(d.parsed_stream_count == 30000);
  assert(d.country_match_count == 300);
  assert(d.eligible_channel_count == 300);
  assert(d.matched_stream_count == 500);
  assert(d.channel_count == 300);
  assert(d.channel_table_realloc_count < 10);
  assert(d.stream_storage_allocation_count == d.channel_count);
  assert(strstr(d.channels[0].streams[0].url, ".m3u8"));
  assert(directory_bytes(&d) < 4 * 1024 * 1024);
  assert(directory_bytes(&d) + 3 * 16384 + 65536 < 8 * 1024 * 1024);
  for (channel_index = 0; channel_index < (int)d.channel_count; channel_index++)
    assert(d.channels[channel_index].stream_count > 0 &&
           d.channels[channel_index].stream_count <= 4);
  assert(!mr_iptv_load_country_files(&d, "/tmp/mr_channels.json",
                                     "/tmp/mr_streams.json", "GB"));
  assert(strstr(mr_iptv_last_error(), "No channel records matched country GB"));
  assert(d.channel_count == 300);
  assert(mr_iptv_load_country_files(&d, "/tmp/mr_channels.json",
                                    "/tmp/mr_streams.json", "US"));
  assert(d.channel_count == 100);
  assert(d.country_match_count == 100 && d.matched_stream_count == 100);
  assert(!mr_iptv_load_country_files(&d, "/tmp/mr_channels.json",
                                     "/tmp/mr_streams.json", "ZZ"));
  assert(strstr(mr_iptv_last_error(),
                "channels found, but none had matching streams"));
  assert(d.channel_count == 100);
  {
    FILE *broken = fopen("/tmp/mr_channels.json", "wb");
    assert(broken);
    fputs("[{\"id\":", broken);
    fclose(broken);
  }
  assert(!mr_iptv_load_country_files(&d, "/tmp/mr_channels.json",
                                     "/tmp/mr_streams.json", "UK"));
  assert(d.channel_count == 100);
  {
    mr_iptv_stream launch;
    char args[4096];
    mr_play_options options;
    memset(&launch, 0, sizeof(launch));
    strcpy(launch.url, "https://example.test/live.m3u8?a=1&b=2");
    mr_play_options_default(&options);
    assert(options.c2p == MR_C2P_KALMS);
    assert(options.h264_performance == MR_H264_PERF_TURBO_GT);
    assert(options.fast_buffer == MR_FAST_BUFFER_AUTO);
    assert(mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                     NULL, NULL));
    assert(strstr(args, "--aga --kalms-c2p --hls-low --hls-max-width=640"));
    assert(strstr(args, "--h264-speed=turbogt"));
    assert(strstr(args, "\"https://example.test/live.m3u8?a=1&b=2\"\n"));
    options.c2p = MR_C2P_STANDARD;
    assert(mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                     NULL, NULL));
    assert(strstr(args, "--aga --wpa"));
    options.c2p = MR_C2P_KALMS;
    options.laced = 1;
    assert(mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                     NULL, NULL));
    assert(strstr(args, "--lace") && !strstr(args, "--2x"));
    options.laced = 0;
    options.scale_2x = 1;
    assert(mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                     NULL, NULL));
    assert(!strstr(args, "--lace") && strstr(args, "--2x"));
    options.laced = 1;
    assert(mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                     NULL, NULL));
    assert(strstr(args, "--lace --2x"));
    options.display = MR_DISPLAY_CGX;
    assert(mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                     NULL, NULL));
    assert(!strstr(args, "--aga") && !strstr(args, "--kalms-c2p"));
    options.display = MR_DISPLAY_P96;
    assert(mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                     NULL, NULL));
    assert(strstr(args, "--p96") && !strstr(args, "--aga") &&
           !strstr(args, "--kalms-c2p"));
    options.display = MR_DISPLAY_CGX;
    options.hls_low = 0;
    options.hls_max_width = 0;
    options.hls_max_height = 0;
    options.live_resync = 0; /* off by default only for a bare "mrplay <url>" */
    assert(!strcmp(args + strlen(args) - strlen(launch.url) - 3,
                   "\"https://example.test/live.m3u8?a=1&b=2\"\n"));
    assert(mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                     NULL, NULL));
    assert(!strcmp(args, "--fast-buffer=auto --h264-speed=turbogt --throughput "
                         "\"https://example.test/live.m3u8?a=1&b=2\"\n"));
    options.h264_performance = MR_H264_PERF_AUTO;
    assert(mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                     NULL, NULL));
    assert(!strcmp(args, "--fast-buffer=auto --throughput "
                         "\"https://example.test/live.m3u8?a=1&b=2\"\n"));
    mr_play_options_default(&options);
    strcpy(launch.user_agent, "Mozilla/5.0 Test Agent");
    assert(mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                     launch.user_agent, NULL));
    assert(strstr(args, "--user-agent \"Mozilla/5.0 Test Agent\""));
    launch.user_agent[0] = 0;
    strcpy(launch.http_referrer, "https://example.test/player");
    assert(mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                     NULL, launch.http_referrer));
    assert(strstr(args, "--referer \"https://example.test/player\""));
    strcpy(launch.user_agent, "Agent *with* \"quotes\"");
    assert(mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                     launch.user_agent,
                                     launch.http_referrer));
    assert(strstr(args, "--user-agent \"Agent **with** *\"quotes*\"\""));
    strcpy(launch.user_agent, "bad\nheader");
    assert(!mr_build_player_arguments(args, sizeof(args), &options, launch.url,
                                      launch.user_agent, NULL));
    assert(mr_build_iptv_arguments(args, sizeof(args), &options));
    assert(strstr(args, "--display aga --c2p kalms --no-laced "
                        "--no-scale-2x --no-copper-vdouble --hls-low"));
    assert(strstr(args, "--h264-speed=turbogt"));
    assert(strstr(args, "--fast-buffer=auto"));
    {
      char *inherited[] = {"iptvgui", "--display", "aga", "--c2p",
                           "kalms", "--laced", "--scale-2x", "--hls-low",
                           "--hls-max-width=640", "--h264-speed=fast",
                           "--fast-buffer=16"};
      char summary[256], first[4096], second[4096], error[128];
      mr_play_options parsed;
      mr_play_options_default(&parsed);
      assert(mr_play_options_parse(&parsed, 11, inherited, error,
                                   sizeof(error)));
      assert(parsed.display == MR_DISPLAY_AGA &&
             parsed.c2p == MR_C2P_KALMS && parsed.laced && parsed.scale_2x &&
             parsed.h264_performance == MR_H264_PERF_FAST &&
             parsed.fast_buffer == MR_FAST_BUFFER_16MB);
      assert(mr_build_player_arguments(first, sizeof(first), &parsed,
                                       launch.url, NULL, NULL));
      assert(mr_build_player_arguments(second, sizeof(second), &parsed,
                                       launch.url, NULL, NULL));
      assert(!strcmp(first, second));
      assert(strstr(first, "--h264-speed=fast"));
      assert(strstr(first, "--fast-buffer=16"));
      mr_play_options_summary(&parsed, summary, sizeof(summary));
      assert(strstr(summary, "Native planar / kalms / Lace on / 2x on"));
      assert(strstr(summary, "H264 Fast"));
      assert(strstr(summary, "Fast buffer 16 MB"));

      parsed.h264_performance = MR_H264_PERF_TURBO;
      assert(mr_build_player_arguments(first, sizeof(first), &parsed,
                                       launch.url, NULL, NULL));
      assert(strstr(first, "--h264-speed=turbo"));
      mr_play_options_summary(&parsed, summary, sizeof(summary));
      assert(strstr(summary, "H264 Turbo"));

      parsed.h264_performance = MR_H264_PERF_TURBO_PLUS;
      assert(mr_build_player_arguments(first, sizeof(first), &parsed,
                                       launch.url, NULL, NULL));
      assert(strstr(first, "--h264-speed=turbo+"));
      mr_play_options_summary(&parsed, summary, sizeof(summary));
      assert(strstr(summary, "H264 Turbo+"));

      parsed.h264_performance = MR_H264_PERF_TURBO_GT;
      assert(mr_build_player_arguments(first, sizeof(first), &parsed,
                                       launch.url, NULL, NULL));
      assert(strstr(first, "--h264-speed=turbogt"));
      mr_play_options_summary(&parsed, summary, sizeof(summary));
      assert(strstr(summary, "H264 TurboGT"));

      {
        char *turbo_args[] = {"iptvgui", "--h264-speed=turbo"};
        char *turbo_plus_args[] = {"iptvgui", "--h264-speed=turbo+"};
        char *turbogt_args[] = {"iptvgui", "--h264-speed=turbogt"};
        mr_play_options turbo_parsed;
        mr_play_options_default(&turbo_parsed);
        assert(mr_play_options_parse(&turbo_parsed, 2, turbo_args, error,
                                     sizeof(error)));
        assert(turbo_parsed.h264_performance == MR_H264_PERF_TURBO);
        mr_play_options_default(&turbo_parsed);
        assert(mr_play_options_parse(&turbo_parsed, 2, turbo_plus_args, error,
                                     sizeof(error)));
        assert(turbo_parsed.h264_performance == MR_H264_PERF_TURBO_PLUS);
        mr_play_options_default(&turbo_parsed);
        assert(mr_play_options_parse(&turbo_parsed, 2, turbogt_args, error,
                                     sizeof(error)));
        assert(turbo_parsed.h264_performance == MR_H264_PERF_TURBO_GT);
      }
    }
    {
      char *inherited[] = {"iptvgui", "--display", "p96"};
      char summary[160], error[128];
      mr_play_options parsed;
      mr_play_options_default(&parsed);
      assert(mr_play_options_parse(&parsed, 3, inherited, error,
                                   sizeof(error)));
      assert(parsed.display == MR_DISPLAY_P96);
      mr_play_options_summary(&parsed, summary, sizeof(summary));
      assert(strstr(summary, "RTG (P96)"));
    }
    {
      char *inherited[] = {"iptvgui", "--display", "ecs32"};
      char summary[160], args[4096], error[128];
      mr_play_options parsed;
      mr_play_options_default(&parsed);
      assert(mr_play_options_parse(&parsed, 3, inherited, error,
                                   sizeof(error)));
      assert(parsed.display == MR_DISPLAY_AGA_ECS32);
      mr_play_options_summary(&parsed, summary, sizeof(summary));
      assert(strstr(summary, "ECS (32)"));
      assert(mr_build_player_arguments(args, sizeof(args), &parsed,
                                       launch.url, NULL, NULL));
      assert(strstr(args, "--aga --ecs32"));
    }
    {
      char *inherited[] = {"iptvgui", "--display", "ecs16"};
      char summary[160], args[4096], error[128];
      mr_play_options parsed;
      mr_play_options_default(&parsed);
      assert(mr_play_options_parse(&parsed, 3, inherited, error,
                                   sizeof(error)));
      assert(parsed.display == MR_DISPLAY_AGA_ECS16);
      mr_play_options_summary(&parsed, summary, sizeof(summary));
      assert(strstr(summary, "ECS (16)"));
      assert(mr_build_player_arguments(args, sizeof(args), &parsed,
                                       launch.url, NULL, NULL));
      assert(strstr(args, "--aga --ecs-fast"));
    }
    {
      /* HAM6 + Copper 2x: options-layer round trip only - the pixel/state
       * correctness argument for repeating a HAM row on real AGA hardware
       * is documented in CLAUDE.md's "AGA copper-assisted vertical
       * doubling notes" HAM extension and can only be confirmed on real
       * hardware. mr_play_options_parse()/append_playback_flags() were
       * already generic across HAM and indexed displays before that
       * extension, so this pins that the HAM6 case keeps working exactly
       * like the pre-existing indexed AGA case below. */
      char *inherited[] = {"iptvgui", "--display", "ham6", "--c2p", "wpa",
                           "--no-laced", "--scale-2x", "--copper-vdouble"};
      char summary[160], args[4096], error[128];
      mr_play_options parsed;
      mr_play_options_default(&parsed);
      assert(mr_play_options_parse(&parsed, 8, inherited, error,
                                   sizeof(error)));
      assert(parsed.display == MR_DISPLAY_HAM6 && parsed.c2p == MR_C2P_WPA &&
             !parsed.laced && parsed.scale_2x && parsed.copper_vdouble);
      assert(mr_build_player_arguments(args, sizeof(args), &parsed,
                                       launch.url, NULL, NULL));
      assert(strstr(args, "--aga --ham6"));
      assert(strstr(args, "--c2p") && strstr(args, "--2x") &&
             strstr(args, "--copper-vdouble"));
      mr_play_options_summary(&parsed, summary, sizeof(summary));
      assert(strstr(summary, "HAM6") && strstr(summary, "2x on (copper)"));
      assert(mr_build_iptv_arguments(args, sizeof(args), &parsed));
      assert(strstr(args, "--display ham6"));
      assert(strstr(args, "--scale-2x --copper-vdouble"));

      /* Dropping --scale-2x must drop --copper-vdouble from the normal
       * (mrplay CLI) argument form even though copper_vdouble itself stays
       * set - append_playback_flags()'s existing "o->scale_2x &&
       * o->copper_vdouble" guard, previously only ever exercised for
       * indexed AGA output; pinned here for HAM explicitly. */
      parsed.scale_2x = 0;
      assert(mr_build_player_arguments(args, sizeof(args), &parsed,
                                       launch.url, NULL, NULL));
      assert(!strstr(args, "--2x") && !strstr(args, "--copper-vdouble"));
      mr_play_options_summary(&parsed, summary, sizeof(summary));
      assert(strstr(summary, "2x off") && !strstr(summary, "(copper)"));
    }
    {
      /* HAM8 + Copper 2x: same round trip, plus pinning that the normal
       * HAM8 build flag is "--ham" (not "--ham6") - append_playback_flags()'s
       * own pre-existing naming, unrelated to this feature but never
       * previously exercised together with --copper-vdouble. */
      char *inherited[] = {"iptvgui", "--display", "ham8", "--c2p", "riva",
                           "--scale-2x", "--copper-vdouble"};
      char summary[160], args[4096], error[128];
      mr_play_options parsed;
      mr_play_options_default(&parsed);
      assert(mr_play_options_parse(&parsed, 7, inherited, error,
                                   sizeof(error)));
      assert(parsed.display == MR_DISPLAY_HAM8 && parsed.c2p == MR_C2P_RIVA &&
             parsed.scale_2x && parsed.copper_vdouble);
      assert(mr_build_player_arguments(args, sizeof(args), &parsed,
                                       launch.url, NULL, NULL));
      assert(strstr(args, "--aga --ham") && !strstr(args, "--ham6"));
      assert(strstr(args, "--riva-c2p") && strstr(args, "--2x") &&
             strstr(args, "--copper-vdouble"));
      mr_play_options_summary(&parsed, summary, sizeof(summary));
      assert(strstr(summary, "HAM8") && strstr(summary, "2x on (copper)"));
    }
    {
      /* Regression: the pre-existing indexed AGA + --2x + --copper-vdouble
       * case (the one confirmed on real AGA hardware) must round-trip
       * exactly as before this HAM extension - no HAM flag appears, and
       * the Copper flag/summary behave identically to the blocks above. */
      char *inherited[] = {"iptvgui", "--display", "aga", "--c2p", "wpa",
                           "--scale-2x", "--copper-vdouble"};
      char summary[160], args[4096], error[128];
      mr_play_options parsed;
      mr_play_options_default(&parsed);
      assert(mr_play_options_parse(&parsed, 7, inherited, error,
                                   sizeof(error)));
      assert(parsed.display == MR_DISPLAY_AGA && parsed.scale_2x &&
             parsed.copper_vdouble);
      assert(mr_build_player_arguments(args, sizeof(args), &parsed,
                                       launch.url, NULL, NULL));
      assert(!strstr(args, "--ham"));
      assert(strstr(args, "--c2p") && strstr(args, "--2x") &&
             strstr(args, "--copper-vdouble"));
      mr_play_options_summary(&parsed, summary, sizeof(summary));
      assert(strstr(summary, "Native planar") &&
             strstr(summary, "2x on (copper)"));
    }
    {
      /* Regression: the normal (non-Copper) HAM6/HAM8/Kalms path is
       * unaffected by this extension - no Copper flag ever appears without
       * --scale-2x, and the summary never claims "(copper)". */
      char *inherited[] = {"iptvgui", "--display", "ham8", "--c2p", "kalms"};
      char summary[160], args[4096], error[128];
      mr_play_options parsed;
      mr_play_options_default(&parsed);
      assert(mr_play_options_parse(&parsed, 5, inherited, error,
                                   sizeof(error)));
      assert(parsed.display == MR_DISPLAY_HAM8 && !parsed.scale_2x &&
             !parsed.copper_vdouble);
      assert(mr_build_player_arguments(args, sizeof(args), &parsed,
                                       launch.url, NULL, NULL));
      assert(strstr(args, "--aga --ham") && strstr(args, "--kalms-c2p"));
      assert(!strstr(args, "--2x") && !strstr(args, "--copper-vdouble"));
      mr_play_options_summary(&parsed, summary, sizeof(summary));
      assert(strstr(summary, "HAM8") && strstr(summary, "2x off") &&
             !strstr(summary, "(copper)"));
    }
  }
  remove("/tmp/mr_channels.json");
  remove("/tmp/mr_streams.json");
  mr_iptv_free(&d);
  puts("IPTV parser/filter checks passed");
  return 0;
}
