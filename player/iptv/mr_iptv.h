#ifndef MR_IPTV_H
#define MR_IPTV_H

#include <stddef.h>

#define MR_IPTV_ID_MAX 128
#define MR_IPTV_NAME_MAX 128
#define MR_IPTV_URL_MAX 1024
#define MR_IPTV_REFERRER_MAX 1024
#define MR_IPTV_USER_AGENT_MAX 256
#define MR_IPTV_ALT_MAX 8
#define MR_IPTV_STREAM_MAX 8
#define MR_IPTV_CATEGORY_MAX 8

/* Public exec MsgPort name the running player publishes so the IPTV controller
 * can find it and ask it to stop before launching another stream. Only one
 * player runs at a time, so a fixed name is sufficient. */
#define MR_IPTV_PLAYER_PORT "MintVID.player"

/* Public exec MsgPort name the IPTV browser GUI (iptvgui/iptvgui-GT)
 * publishes once its window is open and interactive. MintVID Control
 * launches iptvgui as a separate process via LoadSeg()/CreateNewProcTags();
 * that returns as soon as the process exists, long before the child has
 * loaded/refreshed its channel cache and actually opened a window - the
 * launching GUI polls FindPort() for this name (same pattern as
 * MR_IPTV_PLAYER_PORT above) to know when to stop showing a busy indicator.
 * Not used for messaging, only existence - PA_IGNORE, no replies expected. */
#define MR_IPTV_GUI_PORT "MintVID.iptvgui"

typedef struct {
  char url[MR_IPTV_URL_MAX];
  char http_referrer[MR_IPTV_REFERRER_MAX];
  char user_agent[MR_IPTV_USER_AGENT_MAX];
} mr_iptv_stream;

typedef struct {
  char id[MR_IPTV_ID_MAX], name[MR_IPTV_NAME_MAX];
  char network[MR_IPTV_NAME_MAX], country[8];
  char (*alt_names)[MR_IPTV_NAME_MAX];
  char (*categories)[MR_IPTV_NAME_MAX];
  unsigned alt_count, category_count, stream_count;
  unsigned is_nsfw : 1, closed : 1, replaced : 1;
  mr_iptv_stream *streams;
} mr_iptv_channel;

typedef struct {
  mr_iptv_channel *channels;
  size_t channel_count, channel_capacity;
  size_t parsed_channel_count, skipped_channel_count, parsed_stream_count;
  size_t country_match_count, eligible_channel_count, matched_stream_count;
  size_t channel_table_realloc_count, stream_storage_allocation_count;
} mr_iptv_directory;

typedef struct {
  const char *country, *category, *search;
} mr_iptv_filter;

void mr_iptv_init(mr_iptv_directory *directory);
void mr_iptv_free(mr_iptv_directory *directory);
int mr_iptv_parse_channels(mr_iptv_directory *, const char *, size_t);
int mr_iptv_join_streams(mr_iptv_directory *, const char *, size_t);
int mr_iptv_parse_channel_object(const char *json, size_t length,
                                 mr_iptv_channel *channel, char *error,
                                 size_t error_size);
int mr_iptv_parse_channel_object_for_country(
    const char *json, size_t length, const char *country,
    mr_iptv_channel *channel, char *error, size_t error_size);
int mr_iptv_parse_stream_object(const char *json, size_t length,
                                char *channel_id, size_t channel_id_size,
                                mr_iptv_stream *stream, char *error,
                                size_t error_size);
int mr_iptv_parse_stream_channel_object(const char *json, size_t length,
                                        char *channel_id,
                                        size_t channel_id_size, char *error,
                                        size_t error_size);
int mr_iptv_parse_m3u(mr_iptv_directory *, const char *, size_t);
int mr_iptv_channel_visible(const mr_iptv_channel *, const mr_iptv_filter *);
size_t mr_iptv_filter_channels(const mr_iptv_directory *,
                               const mr_iptv_filter *, size_t *, size_t);
int mr_iptv_valid_url(const char *url);
int mr_iptv_supported_url(const char *url);
int mr_iptv_build_mrplay_args(const mr_iptv_stream *stream, char *out,
                              size_t out_size);
const char *mr_iptv_last_error(void);
void mr_iptv_set_error(const char *message);
int mr_iptv_load_country_files(mr_iptv_directory *directory,
                               const char *channels_path,
                               const char *streams_path, const char *country);

#endif
