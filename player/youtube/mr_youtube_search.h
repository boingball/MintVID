#ifndef MR_YOUTUBE_SEARCH_H
#define MR_YOUTUBE_SEARCH_H

#include <stddef.h>

#define MR_YOUTUBE_SEARCH_MAX_RESULTS 100
#define MR_YOUTUBE_SEARCH_TITLE_MAX 192
#define MR_YOUTUBE_SEARCH_CHANNEL_MAX 128
#define MR_YOUTUBE_SEARCH_CHANNEL_ID_MAX 32
#define MR_YOUTUBE_SEARCH_ROW_MAX 352

typedef struct mr_youtube_search_result {
    char video_id[12];
    char title[MR_YOUTUBE_SEARCH_TITLE_MAX];
    char channel[MR_YOUTUBE_SEARCH_CHANNEL_MAX];
    char channel_id[MR_YOUTUBE_SEARCH_CHANNEL_ID_MAX];
    char row[MR_YOUTUBE_SEARCH_ROW_MAX];
    int live;
} mr_youtube_search_result;

typedef struct mr_youtube_search_results {
    mr_youtube_search_result *items;
    size_t count;
} mr_youtube_search_results;

typedef enum mr_youtube_search_mode {
    MR_YOUTUBE_SEARCH_ALL = 0,
    MR_YOUTUBE_SEARCH_VIDEOS,
    MR_YOUTUBE_SEARCH_LIVE,
    MR_YOUTUBE_SEARCH_SHORTS
} mr_youtube_search_mode;

void mr_youtube_search_results_init(mr_youtube_search_results *results);
void mr_youtube_search_results_free(mr_youtube_search_results *results);

/* Build the public YouTube search-page URL. No API key or account is needed. */
int mr_youtube_search_build_url(char *output, size_t output_size,
                                const char *query, int live_only);
int mr_youtube_search_build_url_mode(char *output, size_t output_size,
                                     const char *query,
                                     mr_youtube_search_mode mode);

/* Parse normal video and Shorts objects from YouTube's search-page JSON. */
int mr_youtube_search_parse(mr_youtube_search_results *results,
                            const char *document, size_t document_size,
                            int live_only);
int mr_youtube_search_parse_mode(mr_youtube_search_results *results,
                                 const char *document, size_t document_size,
                                 mr_youtube_search_mode mode);

int mr_youtube_search_watch_url(char *output, size_t output_size,
                                const mr_youtube_search_result *result);
int mr_youtube_channel_videos_url(char *output, size_t output_size,
                                  const mr_youtube_search_result *result);

/* Recognize a pasted YouTube video URL (watch/live/shorts/embed, youtu.be,
 * with or without a leading scheme/www./m. or extra query parameters) and
 * fill in a synthetic single result for it - no network access, just URL
 * parsing, so a link copied from a browser can be queued for playback the
 * same way a real search result is, without actually searching for it.
 * Returns 0 (leaving *out untouched) if `text` isn't a recognizable
 * YouTube video URL. */
int mr_youtube_url_parse(mr_youtube_search_result *out, const char *text);

const char *mr_youtube_search_last_error(void);

#endif
