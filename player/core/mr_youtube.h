/*
 * MintVID - minimal YouTube media resolver.
 *
 * This is deliberately not a general YouTube extractor. It accepts public
 * live HLS plus directly usable muxed 360p/720p MP4s for compatible uploads.
 */
#ifndef MR_YOUTUBE_H
#define MR_YOUTUBE_H

#include <stddef.h>

struct mr_http_options;

/* True only for HTTP(S) URLs on YouTube's own watch/share hosts. */
int mr_youtube_is_url(const char *url);
int mr_youtube_extract_video_id(const char *url, char out[12]);

/* Build the HTTP identity used throughout a YouTube request chain. Browser
 * defaults are supplied when the caller left MintVID's generic UA/referer in
 * place; explicit caller values and all HLS quality limits are preserved. */
int mr_youtube_http_options_init(struct mr_http_options *out,
                                 const struct mr_http_options *base);

/* Extract a YouTube HLS manifest from already-downloaded watch-page HTML.
 * Exposed for deterministic host tests. */
int mr_youtube_extract_live_manifest(const char *html, char *out,
                                     size_t out_size);

typedef enum mr_youtube_media_kind {
    MR_YOUTUBE_MEDIA_NONE = 0,
    MR_YOUTUBE_MEDIA_HLS,
    MR_YOUTUBE_MEDIA_PROGRESSIVE_360P,
    MR_YOUTUBE_MEDIA_PROGRESSIVE_720P,
    MR_YOUTUBE_MEDIA_HLS_VOD
} mr_youtube_media_kind;

/* Name and kind used by the most recent successful resolution. */
const char *mr_youtube_last_client(void);
mr_youtube_media_kind mr_youtube_last_media_kind(void);

/* Extract a directly playable, muxed 360p MP4 (itag 18) from a player JSON
 * response. Exposed for deterministic host tests. */
int mr_youtube_extract_progressive_360p(const char *json, char *out,
                                        size_t out_size);

/* Prefer the muxed 720p MP4 (itag 22) when requested, with an automatic
 * fallback to itag 18. Returns the selected media kind through kind. */
int mr_youtube_extract_progressive(const char *json, int prefer_720p,
                                   char *out, size_t out_size,
                                   mr_youtube_media_kind *kind);

/* Apply the media client's HTTP identity after a successful resolution. */
int mr_youtube_media_http_options_init(struct mr_http_options *out,
                                       const struct mr_http_options *base);

/* Resolve live HLS or a muxed 360p/optional 720p recorded-video URL. */
int mr_youtube_resolve_media(const char *url,
                             const struct mr_http_options *options,
                             char *out, size_t out_size,
                             mr_youtube_media_kind *kind);

/* Retry a recorded HLS failure with the original muxed MP4, skipping HLS. */
int mr_youtube_resolve_mp4_fallback(const char *url,
                                    const struct mr_http_options *options,
                                    char *out, size_t out_size,
                                    mr_youtube_media_kind *kind);

/* Download a public YouTube watch page and resolve its signed live manifest. */
int mr_youtube_resolve_live(const char *url,
                            const struct mr_http_options *options,
                            char *out, size_t out_size);

#endif /* MR_YOUTUBE_H */
