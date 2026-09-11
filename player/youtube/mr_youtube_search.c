#include "mr_youtube_search.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char search_error[160];

static void set_error(const char *text)
{
    snprintf(search_error, sizeof(search_error), "%s", text ? text : "");
}

const char *mr_youtube_search_last_error(void)
{
    return search_error;
}

void mr_youtube_search_results_init(mr_youtube_search_results *results)
{
    if (results) {
        results->items = NULL;
        results->count = 0;
    }
}

void mr_youtube_search_results_free(mr_youtube_search_results *results)
{
    if (!results)
        return;
    free(results->items);
    results->items = NULL;
    results->count = 0;
}

static int append_char(char *out, size_t cap, size_t *used, char c)
{
    if (*used + 1 >= cap)
        return 0;
    out[(*used)++] = c;
    out[*used] = 0;
    return 1;
}

int mr_youtube_search_build_url_mode(char *out, size_t cap,
                                     const char *query,
                                     mr_youtube_search_mode mode)
{
    static const char prefix[] =
        "https://www.youtube.com/results?search_query=";
    static const char hex[] = "0123456789ABCDEF";
    size_t used, i;

    if (!out || !cap || !query || !*query) {
        set_error("Enter something to search for.");
        return 0;
    }
    if (sizeof(prefix) > cap) {
        set_error("Search URL buffer is too small.");
        return 0;
    }
    memcpy(out, prefix, sizeof(prefix));
    used = sizeof(prefix) - 1;
    for (i = 0; query[i]; i++) {
        unsigned char c = (unsigned char)query[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (!append_char(out, cap, &used, (char)c))
                goto too_long;
        } else {
            if (used + 3 >= cap)
                goto too_long;
            out[used++] = '%';
            out[used++] = hex[c >> 4];
            out[used++] = hex[c & 15];
            out[used] = 0;
        }
    }
    if (mode != MR_YOUTUBE_SEARCH_ALL) {
        /* YouTube's current public Type filters. Live is technically a
         * Feature filter, but combines cleanly with the same search page.
         * Values are already escaped for use as the sp query value. */
        static const char *const filters[] = {
            NULL,
            "&sp=EgIQAQ%253D%253D", /* Videos: excludes Shorts */
            "&sp=EgJAAQ%253D%253D", /* Live now */
            "&sp=EgIQCQ%253D%253D"  /* Shorts */
        };
        const char *filter;
        size_t filter_size;
        if (mode < MR_YOUTUBE_SEARCH_VIDEOS ||
            mode > MR_YOUTUBE_SEARCH_SHORTS) {
            set_error("Invalid YouTube search type.");
            return 0;
        }
        filter = filters[mode];
        filter_size = strlen(filter) + 1;
        if (used + filter_size > cap)
            goto too_long;
        memcpy(out + used, filter, filter_size);
    }
    set_error("");
    return 1;

too_long:
    set_error("Search text is too long.");
    return 0;
}

int mr_youtube_search_build_url(char *out, size_t cap,
                                const char *query, int live_only)
{
    return mr_youtube_search_build_url_mode(
        out, cap, query,
        live_only ? MR_YOUTUBE_SEARCH_LIVE : MR_YOUTUBE_SEARCH_ALL);
}

static const char *find_bounded(const char *start, const char *end,
                                const char *needle)
{
    size_t length = strlen(needle);
    const char *p;
    if (!length || end < start || (size_t)(end - start) < length)
        return NULL;
    for (p = start; p + length <= end; p++)
        if (!memcmp(p, needle, length))
            return p;
    return NULL;
}

/* Return the byte after a balanced JSON object, ignoring braces in strings. */
static const char *json_object_end(const char *open, const char *end)
{
    const char *p;
    unsigned depth = 0;
    int in_string = 0, escaped = 0;
    for (p = open; p < end; p++) {
        char c = *p;
        if (in_string) {
            if (escaped)
                escaped = 0;
            else if (c == '\\')
                escaped = 1;
            else if (c == '"')
                in_string = 0;
        } else if (c == '"') {
            in_string = 1;
        } else if (c == '{') {
            depth++;
        } else if (c == '}' && depth) {
            if (--depth == 0)
                return p + 1;
        }
    }
    return NULL;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode a JSON string into a display-safe byte string. Amiga system fonts do
 * not reliably render arbitrary Unicode, so non-Latin \u escapes become '?'. */
static int json_string(const char *quote, const char *end,
                       char *out, size_t cap)
{
    const char *p;
    size_t used = 0;
    if (!quote || quote >= end || *quote != '"' || !cap)
        return 0;
    out[0] = 0;
    for (p = quote + 1; p < end && *p != '"'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') {
            if (++p >= end)
                return 0;
            switch (*p) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = ' '; break;
            case 'r': c = ' '; break;
            case 't': c = ' '; break;
            case 'u': {
                int a, b, d, e;
                unsigned value;
                if (p + 4 >= end || (a = hex_value(p[1])) < 0 ||
                    (b = hex_value(p[2])) < 0 ||
                    (d = hex_value(p[3])) < 0 ||
                    (e = hex_value(p[4])) < 0)
                    return 0;
                value = (unsigned)((a << 12) | (b << 8) | (d << 4) | e);
                c = value >= 32 && value <= 255 ? (unsigned char)value : '?';
                p += 4;
                break;
            }
            default: return 0;
            }
        }
        if (c < 32)
            c = ' ';
        if (used + 1 < cap)
            out[used++] = (char)c;
    }
    if (p >= end || *p != '"')
        return 0;
    out[used] = 0;
    return used != 0;
}

static int field_string(const char *start, const char *end, const char *field,
                        char *out, size_t cap)
{
    const char *p = find_bounded(start, end, field);
    if (!p)
        return 0;
    p += strlen(field);
    return json_string(p, end, out, cap);
}

static int nested_text(const char *start, const char *end, const char *object,
                       char *out, size_t cap)
{
    const char *p = find_bounded(start, end, object);
    const char *object_open, *object_end;
    if (!p)
        return 0;
    object_open = strchr(p + strlen(object), '{');
    if (!object_open || object_open >= end)
        return 0;
    object_end = json_object_end(object_open, end);
    if (!object_end)
        return 0;
    if (field_string(object_open, object_end, "\"text\":", out, cap))
        return 1;
    if (field_string(object_open, object_end, "\"simpleText\":", out, cap))
        return 1;
    return field_string(object_open, object_end, "\"content\":", out, cap);
}

static int already_present(const mr_youtube_search_results *results,
                           const char *video_id)
{
    size_t i;
    for (i = 0; i < results->count; i++)
        if (!strcmp(results->items[i].video_id, video_id))
            return 1;
    return 0;
}

static int channel_id_valid(const char *id)
{
    size_t i, length;
    if (!id || (length = strlen(id)) < 3 || length >=
        MR_YOUTUBE_SEARCH_CHANNEL_ID_MAX || id[0] != 'U' || id[1] != 'C')
        return 0;
    for (i = 2; i < length; i++)
        if (!isalnum((unsigned char)id[i]) && id[i] != '_' && id[i] != '-')
            return 0;
    return 1;
}

static int parse_shorts(mr_youtube_search_results *results,
                        const char *document, const char *end)
{
    static const char marker[] = "\"shortsLockupViewModel\":{";
    const char *p = document;

    while (results->count < MR_YOUTUBE_SEARCH_MAX_RESULTS) {
        const char *shorts = find_bounded(p, end, marker);
        const char *open, *close;
        mr_youtube_search_result item;
        if (!shorts)
            break;
        open = shorts + sizeof(marker) - 2;
        close = json_object_end(open, end);
        if (!close)
            break;
        memset(&item, 0, sizeof(item));
        if (field_string(open, close, "\"videoId\":", item.video_id,
                         sizeof(item.video_id)) &&
            strlen(item.video_id) == 11 &&
            !already_present(results, item.video_id) &&
            nested_text(open, close, "\"primaryText\":", item.title,
                        sizeof(item.title))) {
            snprintf(item.row, sizeof(item.row), "[SHORT] %s", item.title);
            results->items[results->count++] = item;
        }
        p = close;
    }
    return 1;
}

int mr_youtube_search_parse_mode(mr_youtube_search_results *results,
                                 const char *document, size_t document_size,
                                 mr_youtube_search_mode mode)
{
    static const char marker[] = "\"videoRenderer\":{";
    static const char grid_marker[] = "\"gridVideoRenderer\":{";
    const char *p, *end;
    mr_youtube_search_result *items;

    if (!results || !document) {
        set_error("Invalid YouTube search document.");
        return 0;
    }
    mr_youtube_search_results_free(results);
    items = (mr_youtube_search_result *)calloc(
        MR_YOUTUBE_SEARCH_MAX_RESULTS, sizeof(*items));
    if (!items) {
        set_error("Not enough memory for YouTube results.");
        return 0;
    }
    results->items = items;
    end = document + document_size;
    if (mode == MR_YOUTUBE_SEARCH_SHORTS) {
        parse_shorts(results, document, end);
        if (!results->count)
            set_error("No YouTube Shorts found.");
        else
            set_error("");
        return 1;
    }
    p = document;
    while (results->count < MR_YOUTUBE_SEARCH_MAX_RESULTS) {
        const char *video = find_bounded(p, end, marker);
        const char *grid = find_bounded(p, end, grid_marker);
        size_t marker_size;
        const char *open;
        if (!video && !grid) break;
        if (!grid || (video && video < grid)) {
            p = video;
            marker_size = sizeof(marker);
        } else {
            p = grid;
            marker_size = sizeof(grid_marker);
        }
        open = p + marker_size - 2;
        const char *close = json_object_end(open, end);
        mr_youtube_search_result item;
        int live;
        if (!close)
            break;
        memset(&item, 0, sizeof(item));
        live = find_bounded(open, close, "BADGE_STYLE_TYPE_LIVE_NOW") != NULL ||
               find_bounded(open, close, "\"style\":\"LIVE\"") != NULL ||
               find_bounded(open, close, "\"isLive\":true") != NULL ||
               find_bounded(open, close, "\"label\":\"LIVE NOW\"") != NULL;
        if ((mode != MR_YOUTUBE_SEARCH_LIVE || live) &&
            field_string(open, close, "\"videoId\":", item.video_id,
                         sizeof(item.video_id)) &&
            strlen(item.video_id) == 11 &&
            !already_present(results, item.video_id) &&
            nested_text(open, close, "\"title\":", item.title,
                        sizeof(item.title))) {
            if (!nested_text(open, close, "\"ownerText\":", item.channel,
                             sizeof(item.channel)))
                nested_text(open, close, "\"longBylineText\":", item.channel,
                            sizeof(item.channel));
            if (!field_string(open, close, "\"browseId\":", item.channel_id,
                              sizeof(item.channel_id)) ||
                !channel_id_valid(item.channel_id))
                item.channel_id[0] = 0;
            item.live = live;
            snprintf(item.row, sizeof(item.row), "%s%s%s%s",
                     live ? "[LIVE] " : "", item.title,
                     item.channel[0] ? " - " : "", item.channel);
            results->items[results->count++] = item;
        }
        p = close;
    }
    if (mode == MR_YOUTUBE_SEARCH_ALL &&
        results->count < MR_YOUTUBE_SEARCH_MAX_RESULTS)
        parse_shorts(results, document, end);
    if (!results->count) {
        set_error(mode == MR_YOUTUBE_SEARCH_LIVE
                      ? "No live YouTube results found."
                      : "No YouTube video results found.");
        return 1;
    }
    set_error("");
    return 1;
}

int mr_youtube_search_parse(mr_youtube_search_results *results,
                            const char *document, size_t document_size,
                            int live_only)
{
    return mr_youtube_search_parse_mode(
        results, document, document_size,
        live_only ? MR_YOUTUBE_SEARCH_LIVE : MR_YOUTUBE_SEARCH_ALL);
}

int mr_youtube_channel_videos_url(char *out, size_t cap,
                                  const mr_youtube_search_result *result)
{
    int length;
    if (!out || !cap || !result || !channel_id_valid(result->channel_id))
        return 0;
    length = snprintf(out, cap, "https://www.youtube.com/channel/%s/videos",
                      result->channel_id);
    return length > 0 && (size_t)length < cap;
}

static int is_video_id_char(unsigned char c)
{
    return isalnum(c) || c == '_' || c == '-';
}

/* Copy exactly 11 video-id characters from p into id (size 12) and require
 * the next byte to end the id (not a 12th id character, which would mean a
 * garbled/truncated paste rather than a real 11-character YouTube id). */
static int extract_id_at(const char *p, char id[12])
{
    size_t i;
    for (i = 0; i < 11 && p[i] && is_video_id_char((unsigned char)p[i]); i++)
        id[i] = p[i];
    id[i] = 0;
    if (i != 11) return 0;
    return !is_video_id_char((unsigned char)p[11]);
}

int mr_youtube_url_parse(mr_youtube_search_result *out, const char *text)
{
    const char *host, *path, *p;
    char id[12];

    if (!out || !text) return 0;
    while (isspace((unsigned char)*text)) text++;
    if (!strncmp(text, "http://", 7)) host = text + 7;
    else if (!strncmp(text, "https://", 8)) host = text + 8;
    else host = text;                    /* tolerate a bare pasted domain */
    if (!strncmp(host, "www.", 4)) host += 4;
    else if (!strncmp(host, "m.", 2)) host += 2;

    if (!strncmp(host, "youtu.be/", 9)) {
        if (!extract_id_at(host + 9, id)) return 0;
    } else if (!strncmp(host, "youtube.com/", 12)) {
        path = host + 12;
        if (!strncmp(path, "watch", 5) && (path[5] == '?' || path[5] == 0)) {
            p = strchr(path, '?');
            if (!p) return 0;
            p++;
            for (;;) {
                if (!strncmp(p, "v=", 2)) {
                    if (!extract_id_at(p + 2, id)) return 0;
                    break;
                }
                if (!(p = strchr(p, '&'))) return 0;
                p++;
            }
        } else if (!strncmp(path, "live/", 5)) {
            if (!extract_id_at(path + 5, id)) return 0;
        } else if (!strncmp(path, "shorts/", 7)) {
            if (!extract_id_at(path + 7, id)) return 0;
        } else if (!strncmp(path, "embed/", 6)) {
            if (!extract_id_at(path + 6, id)) return 0;
        } else return 0;
    } else return 0;

    memset(out, 0, sizeof(*out));
    memcpy(out->video_id, id, sizeof id);
    snprintf(out->title, sizeof(out->title), "Video from pasted URL");
    snprintf(out->row, sizeof(out->row), "[URL] %s", out->video_id);
    return 1;
}

int mr_youtube_search_watch_url(char *out, size_t cap,
                                const mr_youtube_search_result *result)
{
    int length;
    if (!out || !cap || !result || strlen(result->video_id) != 11)
        return 0;
    length = snprintf(out, cap, "https://www.youtube.com/watch?v=%s",
                      result->video_id);
    return length > 0 && (size_t)length < cap;
}
