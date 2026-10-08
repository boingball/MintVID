#include "mr_iptv.h"
#include "../core/mr_text.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
#include <exec/memory.h>
#include <proto/exec.h>
#endif

#define JSON_MAX_DEPTH 24
#define IPTV_MAX_JSON_CHANNEL_OBJECTS 100000
#define IPTV_MAX_RETAINED_CHANNELS 20000
#define IPTV_MAX_STREAMS 30000
typedef struct {
  const char *start, *p, *end;
  const char *source, *field;
  char field_storage[40];
  size_t object_index;
  int error;
} parser;

static char last_error[256];

const char *mr_iptv_last_error(void) {
  return last_error[0] ? last_error : "invalid JSON";
}
void mr_iptv_set_error(const char *message) {
  snprintf(last_error, sizeof(last_error), "%s",
           message ? message : "IPTV error");
}

static int expected(parser *j, const char *type) {
  char token[20];
  size_t left;
  unsigned char c;
  if (last_error[0])
    return 0;
  j->error = 1;
  left = (size_t)(j->end - j->p);
  c = left ? (unsigned char)*j->p : 0;
  if (!left)
    strcpy(token, "end of input");
  else if (isprint(c))
    snprintf(token, sizeof(token), "'%c'", c);
  else
    snprintf(token, sizeof(token), "0x%02x", (unsigned)c);
  if (j->field && *j->field)
    snprintf(last_error, sizeof(last_error),
             "%s object %lu field %s at byte %lu: expected %s, got %s",
             j->source, (unsigned long)j->object_index, j->field,
             (unsigned long)(j->p - j->start), type, token);
  else
    snprintf(last_error, sizeof(last_error),
             "%s at byte %lu: expected %s, got %s", j->source,
             (unsigned long)(j->p - j->start), type, token);
  return 0;
}

static void ws(parser *j) {
  while (j->p < j->end && isspace((unsigned char)*j->p))
    j->p++;
}
static int hex(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}
/* Reads a JSON string into out.  Display text (names, categories) is
 * converted to Latin-1 for the Amiga fonts; other strings (ids, URLs) keep
 * their bytes, with \u escapes beyond ASCII as '?'. */
static int string_as(parser *j, char *out, size_t cap, int display) {
  size_t n = 0;
  int a, b, c, d;
  unsigned long cp;
  mr_text text;
  ws(j);
  if (j->p == j->end || *j->p != '"')
    return expected(j, "string");
  j->p++;
  mr_text_begin(&text, out, cap);
  while (j->p < j->end && *j->p != '"') {
    unsigned char ch = (unsigned char)*j->p++;
    if (ch < 0x20)
      return j->error = 1, 0;
    cp = ch;
    if (ch == '\\') {
      if (j->p == j->end)
        return j->error = 1, 0;
      ch = (unsigned char)*j->p++;
      if (ch == 'u') {
        if (j->end - j->p < 4 || (a = hex(j->p[0])) < 0 ||
            (b = hex(j->p[1])) < 0 || (c = hex(j->p[2])) < 0 ||
            (d = hex(j->p[3])) < 0)
          return j->error = 1, 0;
        cp = (unsigned long)((a << 12) | (b << 8) | (c << 4) | d);
        j->p += 4;
        if (cp >= 0xd800 && cp <= 0xdbff && j->end - j->p >= 6 &&
            j->p[0] == '\\' && j->p[1] == 'u' && (a = hex(j->p[2])) >= 0 &&
            (b = hex(j->p[3])) >= 0 && (c = hex(j->p[4])) >= 0 &&
            (d = hex(j->p[5])) >= 0) {
          unsigned long low =
              (unsigned long)((a << 12) | (b << 8) | (c << 4) | d);
          if (low >= 0xdc00 && low <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            j->p += 6;
          }
        }
        ch = cp < 128 ? (unsigned char)cp : '?';
      } else if (ch == 'n')
        ch = '\n';
      else if (ch == 'r')
        ch = '\r';
      else if (ch == 't')
        ch = '\t';
      else if (ch == 'b')
        ch = '\b';
      else if (ch == 'f')
        ch = '\f';
      else if (ch != '"' && ch != '\\' && ch != '/')
        return j->error = 1, 0;
      if (cp < 128)
        cp = ch;
    } else if (ch >= 0x80 && display) {
      const char *q = j->p - 1, *close = q;
      while (close < j->end && close < q + 4 && *close != '"' &&
             *close != '\\')
        close++;
      j->p = q + mr_text_utf8_next(q, (size_t)(close - q), &cp);
    }
    if (display)
      mr_text_put(&text, cp);
    else if (cap && n + 1 < cap)
      out[n++] = (char)ch;
  }
  if (j->p == j->end)
    return j->error = 1, 0;
  j->p++;
  if (display)
    mr_text_end(&text);
  else if (cap)
    out[n] = 0;
  return 1;
}
static int string(parser *j, char *out, size_t cap) {
  return string_as(j, out, cap, 0);
}
static int text_string(parser *j, char *out, size_t cap) {
  return string_as(j, out, cap, 1);
}
static int json_null(parser *j) {
  ws(j);
  if ((size_t)(j->end - j->p) >= 4 && !memcmp(j->p, "null", 4)) {
    j->p += 4;
    return 1;
  }
  return 0;
}

static int nullable_string_as(parser *j, char *out, size_t cap,
                              int display) {
  const char *value_start;
  ws(j);
  value_start = j->p;
  if (j->p >= j->end)
    return expected(j, "string or null");
  if (j->p < j->end && *j->p == '"')
    return string_as(j, out, cap, display);
  if (json_null(j)) {
    if (cap)
      out[0] = 0;
    return 1;
  }
  j->p = value_start;
  return expected(j, "string or null");
}
static int nullable_string(parser *j, char *out, size_t cap) {
  return nullable_string_as(j, out, cap, 0);
}

static int number(parser *j) {
  const char *p = j->p;
  if (p < j->end && *p == '-')
    p++;
  if (p == j->end)
    return 0;
  if (*p == '0')
    p++;
  else {
    if (*p < '1' || *p > '9')
      return 0;
    while (p < j->end && *p >= '0' && *p <= '9')
      p++;
  }
  if (p < j->end && *p == '.') {
    p++;
    if (p == j->end || *p < '0' || *p > '9')
      return 0;
    while (p < j->end && *p >= '0' && *p <= '9')
      p++;
  }
  if (p < j->end && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < j->end && (*p == '+' || *p == '-'))
      p++;
    if (p == j->end || *p < '0' || *p > '9')
      return 0;
    while (p < j->end && *p >= '0' && *p <= '9')
      p++;
  }
  j->p = p;
  return 1;
}
static int skip(parser *, int);
static int skip(parser *j, int depth) {
  char key[2];
  char open, close;
  if (depth > JSON_MAX_DEPTH)
    return j->error = 1, 0;
  ws(j);
  if (j->p == j->end)
    return j->error = 1, 0;
  if (*j->p == '"')
    return string(j, key, sizeof(key));
  if (*j->p == '{' || *j->p == '[') {
    open = *j->p++;
    close = open == '{' ? '}' : ']';
    ws(j);
    if (j->p < j->end && *j->p == close)
      return j->p++, 1;
    for (;;) {
      if (open == '{' && (!string(j, key, sizeof(key)) ||
                          (ws(j), j->p == j->end || *j->p++ != ':')))
        return j->error = 1, 0;
      if (!skip(j, depth + 1))
        return 0;
      ws(j);
      if (j->p == j->end)
        return j->error = 1, 0;
      if (*j->p == close)
        return j->p++, 1;
      if (*j->p++ != ',')
        return j->error = 1, 0;
    }
  }
  if (json_null(j))
    return 1;
  if ((size_t)(j->end - j->p) >= 4 && !memcmp(j->p, "true", 4)) {
    j->p += 4;
    return 1;
  }
  if ((size_t)(j->end - j->p) >= 5 && !memcmp(j->p, "false", 5)) {
    j->p += 5;
    return 1;
  }
  if (*j->p == ',' || *j->p == ']' || *j->p == '}')
    return expected(j, "JSON value");
  if (!number(j))
    return expected(j, "JSON value");
  return 1;
}
static int boolean(parser *j, unsigned *v) {
  ws(j);
  if (j->end - j->p >= 4 && !memcmp(j->p, "true", 4))
    return j->p += 4, *v = 1, 1;
  if (j->end - j->p >= 5 && !memcmp(j->p, "false", 5))
    return j->p += 5, *v = 0, 1;
  return j->error = 1, 0;
}
static int str_array(parser *j, char (**a)[MR_IPTV_NAME_MAX], unsigned *count,
                     unsigned max) {
  ws(j);
  if (j->p == j->end || *j->p++ != '[')
    return j->error = 1, 0;
  ws(j);
  if (j->p < j->end && *j->p == ']')
    return j->p++, 1;
  for (;;) {
    char tmp[MR_IPTV_NAME_MAX];
    if (!text_string(j, tmp, sizeof(tmp)))
      return 0;
    if (*count < max) {
      void *items = realloc(*a, (*count + 1) * sizeof(**a));
      if (!items) {
        j->error = 1;
        snprintf(last_error, sizeof(last_error),
                 "%s object %lu field %s: allocation failed for string array",
                 j->source, (unsigned long)j->object_index,
                 j->field ? j->field : "unknown");
        return 0;
      }
      *a = items;
      strcpy((*a)[(*count)++], tmp);
    }
    ws(j);
    if (j->p == j->end)
      return j->error = 1, 0;
    if (*j->p == ']')
      return j->p++, 1;
    if (*j->p++ != ',')
      return j->error = 1, 0;
  }
}

static void object_error(char *error, size_t error_size) {
  if (error && error_size)
    snprintf(error, error_size, "%s", mr_iptv_last_error());
}

static int object_begin(parser *j) {
  ws(j);
  if (j->p == j->end || *j->p++ != '{')
    return expected(j, "'{'");
  ws(j);
  return 1;
}

static int object_next_field(parser *j, char *key, size_t key_size) {
  ws(j);
  if (j->p < j->end && *j->p == '}')
    return 0;
  if (!string(j, key, key_size) ||
      (ws(j), j->p == j->end || *j->p++ != ':'))
    return expected(j, "':'");
  snprintf(j->field_storage, sizeof(j->field_storage), "%s", key);
  j->field = j->field_storage;
  return 1;
}

static int object_field_done(parser *j) {
  ws(j);
  if (j->p < j->end && *j->p == ',') {
    j->p++;
    return 1;
  }
  if (j->p < j->end && *j->p == '}')
    return 1;
  return expected(j, "',' or '}'");
}

static int channel_object_pass(const char *json, size_t length,
                               mr_iptv_channel *channel, int metadata) {
  parser j = {json, json, json + length, "channel object", NULL, {0}, 1, 0};
  char key[40];
  if (!object_begin(&j))
    return 0;
  while (j.p < j.end && *j.p != '}') {
    int next = object_next_field(&j, key, sizeof(key));
    unsigned b;
    if (next <= 0)
      return 0;
    if (!metadata && !strcmp(key, "id")) {
      if (!string(&j, channel->id, sizeof(channel->id))) return 0;
    } else if (!metadata && !strcmp(key, "name")) {
      if (!text_string(&j, channel->name, sizeof(channel->name))) return 0;
    } else if (!metadata && !strcmp(key, "country")) {
      if (!string(&j, channel->country, sizeof(channel->country))) return 0;
    } else if (!metadata && !strcmp(key, "is_nsfw")) {
      if (!boolean(&j, &b)) return expected(&j, "boolean");
      channel->is_nsfw = b;
    } else if (!metadata && !strcmp(key, "closed")) {
      char value[8];
      if (!nullable_string(&j, value, sizeof(value))) return 0;
      channel->closed = value[0] != 0;
    } else if (!metadata && !strcmp(key, "replaced_by")) {
      char value[8];
      if (!nullable_string(&j, value, sizeof(value))) return 0;
      channel->replaced = value[0] != 0;
    } else if (metadata && !strcmp(key, "network")) {
      if (!nullable_string_as(&j, channel->network, sizeof(channel->network),
                              1))
        return 0;
    } else if (metadata && !strcmp(key, "alt_names")) {
      if (!str_array(&j, &channel->alt_names, &channel->alt_count, 2)) return 0;
    } else if (metadata && !strcmp(key, "categories")) {
      if (!str_array(&j, &channel->categories, &channel->category_count, 2))
        return 0;
    } else if (!skip(&j, 1)) {
      return expected(&j, "valid JSON value");
    }
    if (!object_field_done(&j))
      return 0;
    ws(&j);
  }
  if (j.p == j.end || *j.p++ != '}')
    return expected(&j, "'}'");
  ws(&j);
  if (j.p != j.end)
    return expected(&j, "end of object");
  return 1;
}

int mr_iptv_parse_channel_object_for_country(
    const char *json, size_t length, const char *country,
    mr_iptv_channel *channel, char *error, size_t error_size) {
  int useful;
  last_error[0] = 0;
  if (!json || !channel) {
    mr_iptv_set_error("invalid channel object arguments");
    object_error(error, error_size);
    return 0;
  }
  memset(channel, 0, sizeof(*channel));
  if (!channel_object_pass(json, length, channel, 0)) {
    object_error(error, error_size);
    return 0;
  }
  useful = channel->id[0] && channel->name[0] && !channel->is_nsfw &&
           !channel->closed && !channel->replaced &&
           (!country || !*country || !strcmp(channel->country, country));
  if (useful && !channel_object_pass(json, length, channel, 1)) {
    free(channel->alt_names);
    free(channel->categories);
    memset(channel, 0, sizeof(*channel));
    object_error(error, error_size);
    return 0;
  }
  return 1;
}

int mr_iptv_parse_channel_object(const char *json, size_t length,
                                 mr_iptv_channel *channel, char *error,
                                 size_t error_size) {
  return mr_iptv_parse_channel_object_for_country(
      json, length, NULL, channel, error, error_size);
}

int mr_iptv_parse_stream_channel_object(const char *json, size_t length,
                                        char *channel_id,
                                        size_t channel_id_size, char *error,
                                        size_t error_size) {
  parser j = {json, json, json + length, "stream object", NULL, {0}, 1, 0};
  char key[40];
  last_error[0] = 0;
  if (!json || !channel_id || !channel_id_size) {
    mr_iptv_set_error("invalid stream object arguments");
    object_error(error, error_size);
    return 0;
  }
  channel_id[0] = 0;
  if (!object_begin(&j)) goto fail;
  while (j.p < j.end && *j.p != '}') {
    if (object_next_field(&j, key, sizeof(key)) <= 0) goto fail;
    if (!strcmp(key, "channel")) {
      if (!nullable_string(&j, channel_id, channel_id_size)) goto fail;
    } else if (!skip(&j, 1)) {
      expected(&j, "valid JSON value");
      goto fail;
    }
    if (!object_field_done(&j)) goto fail;
    ws(&j);
  }
  if (j.p == j.end || *j.p++ != '}') { expected(&j, "'}'"); goto fail; }
  ws(&j);
  if (j.p != j.end) { expected(&j, "end of object"); goto fail; }
  return 1;
fail:
  object_error(error, error_size);
  return 0;
}

int mr_iptv_parse_stream_object(const char *json, size_t length,
                                char *channel_id, size_t channel_id_size,
                                mr_iptv_stream *stream, char *error,
                                size_t error_size) {
  parser j = {json, json, json + length, "stream object", NULL, {0}, 1, 0};
  char key[40];
  last_error[0] = 0;
  if (!json || !channel_id || !channel_id_size || !stream) {
    mr_iptv_set_error("invalid stream object arguments");
    object_error(error, error_size);
    return 0;
  }
  channel_id[0] = 0;
  memset(stream, 0, sizeof(*stream));
  if (!object_begin(&j)) goto fail;
  while (j.p < j.end && *j.p != '}') {
    if (object_next_field(&j, key, sizeof(key)) <= 0) goto fail;
    if (!strcmp(key, "channel")) {
      if (!nullable_string(&j, channel_id, channel_id_size)) goto fail;
    } else if (!strcmp(key, "url")) {
      if (!string(&j, stream->url, sizeof(stream->url))) goto fail;
    } else if (!strcmp(key, "http_referrer")) {
      if (!nullable_string(&j, stream->http_referrer,
                           sizeof(stream->http_referrer))) goto fail;
    } else if (!strcmp(key, "user_agent")) {
      if (!nullable_string(&j, stream->user_agent,
                           sizeof(stream->user_agent))) goto fail;
    } else if (!skip(&j, 1)) {
      expected(&j, "valid JSON value");
      goto fail;
    }
    if (!object_field_done(&j)) goto fail;
    ws(&j);
  }
  if (j.p == j.end || *j.p++ != '}') { expected(&j, "'}'"); goto fail; }
  ws(&j);
  if (j.p != j.end) { expected(&j, "end of object"); goto fail; }
  return 1;
fail:
  object_error(error, error_size);
  return 0;
}
static int grow_channels(mr_iptv_directory *d, size_t *requested) {
  size_t new_capacity;
  void *new_channels;
  if (d->channel_count < d->channel_capacity)
    return 1;
  if (d->channel_capacity >= IPTV_MAX_RETAINED_CHANNELS)
    return 0;
  new_capacity = d->channel_capacity ? d->channel_capacity * 2 : 64;
  if (new_capacity > IPTV_MAX_RETAINED_CHANNELS)
    new_capacity = IPTV_MAX_RETAINED_CHANNELS;
  if (new_capacity <= d->channel_capacity ||
      new_capacity > ((size_t)-1) / sizeof(*d->channels))
    return 0;
  if (requested)
    *requested = new_capacity;
  new_channels = realloc(d->channels, new_capacity * sizeof(*d->channels));
  if (!new_channels)
    return 0;
  d->channels = new_channels;
  d->channel_capacity = new_capacity;
  return 1;
}

int mr_iptv_parse_channels(mr_iptv_directory *out, const char *data,
                           size_t len) {
  parser j = {data, data, data + len, "channels.json", NULL, {0}, 1, 0};
  mr_iptv_directory d;
  mr_iptv_channel c;
  int c_active = 0;
  last_error[0] = 0;
  mr_iptv_init(&d);
  ws(&j);
  if (j.p == j.end || *j.p != '[') {
    expected(&j, "'['");
    goto fail;
  }
  j.p++;
  ws(&j);
  if (j.p < j.end && *j.p == ']') {
    j.p++;
    goto done;
  }
  for (;;) {
    j.field = NULL;
    memset(&c, 0, sizeof(c));
    c_active = 1;
    ws(&j);
    if (j.p == j.end || *j.p != '{') {
      expected(&j, "'{' or ']'");
      goto fail;
    }
    j.p++;
    ws(&j);
    while (j.p < j.end && *j.p != '}') {
      char key[40];
      unsigned b;
      if (!string(&j, key, sizeof(key)) ||
          (ws(&j), j.p == j.end || *j.p != ':')) {
        if (!j.error)
          expected(&j, "':'");
        goto fail;
      }
      j.p++;
      strcpy(j.field_storage, key);
      j.field = j.field_storage;
      if (!strcmp(key, "id")) {
        if (!string(&j, c.id, sizeof(c.id)))
          goto fail;
      } else if (!strcmp(key, "name")) {
        if (!text_string(&j, c.name, sizeof(c.name)))
          goto fail;
      } else if (!strcmp(key, "network")) {
        if (!nullable_string_as(&j, c.network, sizeof(c.network), 1))
          goto fail;
      } else if (!strcmp(key, "owner") || !strcmp(key, "website") ||
                 !strcmp(key, "logo")) {
        char ignored[2];
        if (!nullable_string(&j, ignored, sizeof(ignored)))
          goto fail;
      } else if (!strcmp(key, "country")) {
        if (!string(&j, c.country, sizeof(c.country)))
          goto fail;
      } else if (!strcmp(key, "alt_names")) {
        if (!str_array(&j, &c.alt_names, &c.alt_count, MR_IPTV_ALT_MAX))
          goto fail;
      } else if (!strcmp(key, "categories")) {
        if (!str_array(&j, &c.categories, &c.category_count,
                       MR_IPTV_CATEGORY_MAX))
          goto fail;
      } else if (!strcmp(key, "is_nsfw")) {
        if (!boolean(&j, &b))
          goto fail;
        c.is_nsfw = b;
      } else if (!strcmp(key, "closed")) {
        char x[8];
        if (!nullable_string(&j, x, sizeof(x)))
          goto fail;
        c.closed = x[0] != 0;
      } else if (!strcmp(key, "replaced_by")) {
        char x[8];
        if (!nullable_string(&j, x, sizeof(x)))
          goto fail;
        c.replaced = x[0] != 0;
      } else if (!skip(&j, 1))
        goto fail;
      ws(&j);
      if (j.p < j.end && *j.p == ',') {
        j.p++;
        ws(&j);
      } else
        break;
    }
    if (j.p == j.end || *j.p != '}') {
      expected(&j, "',' or '}'");
      goto fail;
    }
    j.p++;
    d.parsed_channel_count++;
    if (d.parsed_channel_count > IPTV_MAX_JSON_CHANNEL_OBJECTS) {
      snprintf(last_error, sizeof(last_error),
               "channels.json: JSON object safety limit reached at %lu entries",
               (unsigned long)d.parsed_channel_count);
      goto fail;
    }
    if (c.id[0] && c.name[0] && !c.is_nsfw && !c.closed && !c.replaced) {
      if (d.channel_count >= IPTV_MAX_RETAINED_CHANNELS) {
        d.skipped_channel_count++;
        free(c.alt_names);
        free(c.categories);
        c_active = 0;
      } else {
        if (d.channel_count == d.channel_capacity) {
          size_t requested = d.channel_capacity;
          if (!grow_channels(&d, &requested)) {
            fprintf(stderr, "IPTV: growing channels %lu -> %lu\n",
                    (unsigned long)d.channel_capacity,
                    (unsigned long)requested);
            fprintf(stderr, "IPTV: channel record size %lu bytes\n",
                    (unsigned long)sizeof(*d.channels));
            fprintf(stderr, "IPTV: requested allocation %lu bytes\n",
                    (unsigned long)(requested * sizeof(*d.channels)));
#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
            fprintf(stderr, "IPTV: available memory %lu bytes\n",
                    (unsigned long)AvailMem(MEMF_ANY));
#endif
            snprintf(last_error, sizeof(last_error),
                     "channels.json: unable to grow channel table beyond %lu "
                     "entries (requested %lu bytes)",
                     (unsigned long)d.channel_capacity,
                     (unsigned long)(requested * sizeof(*d.channels)));
            goto fail;
          }
        }
        d.channels[d.channel_count++] = c;
        c_active = 0;
      }
    } else {
      d.skipped_channel_count++;
      free(c.alt_names);
      free(c.categories);
      c_active = 0;
    }
    ws(&j);
    if (j.p == j.end)
      goto fail;
    if (*j.p == ']') {
      j.p++;
      break;
    }
    if (*j.p != ',') {
      j.field = NULL;
      expected(&j, "',' or ']'");
      goto fail;
    }
    j.p++;
    j.object_index++;
  }
done:
  ws(&j);
  if (j.p != j.end) {
    expected(&j, "end of input");
    goto fail;
  }
  mr_iptv_free(out);
  *out = d;
  return 1;
fail:
  if (c_active) {
    free(c.alt_names);
    free(c.categories);
  }
  if (!last_error[0])
    expected(&j, "valid JSON");
  mr_iptv_free(&d);
  return 0;
}

typedef struct {
  char channel[MR_IPTV_ID_MAX];
  mr_iptv_stream stream;
} pending;

static int add_stream(mr_iptv_channel *channel, const mr_iptv_stream *stream) {
  mr_iptv_stream *items;
  if (channel->stream_count >= MR_IPTV_STREAM_MAX)
    return 1;
  items = (mr_iptv_stream *)realloc(
      channel->streams, (channel->stream_count + 1) * sizeof(*items));
  if (!items)
    return 0;
  channel->streams = items;
  channel->streams[channel->stream_count++] = *stream;
  return 1;
}

int mr_iptv_join_streams(mr_iptv_directory *d, const char *data, size_t len) {
  parser j = {data, data, data + len, "streams.json", NULL, {0}, 1, 0};
  pending *all = NULL;
  size_t n = 0, cap = 0, i, k, parsed_streams = 0, retained = 0;
  last_error[0] = 0;
  ws(&j);
  if (j.p == j.end || *j.p != '[') {
    expected(&j, "'['");
    goto fail;
  }
  j.p++;
  ws(&j);
  while (j.p < j.end && *j.p != ']') {
    pending x;
    memset(&x, 0, sizeof(x));
    j.field = NULL;
    if (j.p == j.end || *j.p != '{') {
      expected(&j, "'{' or ']'");
      goto fail;
    }
    j.p++;
    parsed_streams++;
    ws(&j);
    while (j.p < j.end && *j.p != '}') {
      char key[40];
      if (!string(&j, key, sizeof(key)) ||
          (ws(&j), j.p == j.end || *j.p != ':')) {
        if (!j.error)
          expected(&j, "':'");
        goto fail;
      }
      j.p++;
      strcpy(j.field_storage, key);
      j.field = j.field_storage;
      if (!strcmp(key, "channel")) {
        if (!nullable_string(&j, x.channel, sizeof(x.channel)))
          goto fail;
      } else if (!strcmp(key, "url")) {
        if (!string(&j, x.stream.url, sizeof(x.stream.url)))
          goto fail;
      } else if (!strcmp(key, "http_referrer")) {
        if (!nullable_string(&j, x.stream.http_referrer,
                             sizeof(x.stream.http_referrer)))
          goto fail;
      } else if (!strcmp(key, "user_agent")) {
        if (!nullable_string(&j, x.stream.user_agent,
                             sizeof(x.stream.user_agent)))
          goto fail;
      } else if (!skip(&j, 1))
        goto fail;
      ws(&j);
      if (j.p < j.end && *j.p == ',') {
        j.p++;
        ws(&j);
      } else
        break;
    }
    if (j.p == j.end || *j.p != '}') {
      expected(&j, "',' or '}'");
      goto fail;
    }
    j.p++;
    if (x.channel[0] && mr_iptv_supported_url(x.stream.url)) {
      if (n == IPTV_MAX_STREAMS) {
        snprintf(last_error, sizeof(last_error),
                 "streams.json: stream limit reached at %lu entries",
                 (unsigned long)n);
        goto fail;
      }
      if (n == cap) {
        size_t nc = cap ? cap * 2 : 128;
        void *p = realloc(all, nc * sizeof(*all));
        if (!p) {
          snprintf(
              last_error, sizeof(last_error),
              "streams.json: unable to grow stream table beyond %lu entries",
              (unsigned long)cap);
          goto fail;
        }
        all = p;
        cap = nc;
      }
      all[n++] = x;
    }
    ws(&j);
    if (j.p < j.end && *j.p == ',') {
      j.p++;
      ws(&j);
      j.object_index++;
    } else
      break;
  }
  if (j.p == j.end || *j.p != ']') {
    j.field = NULL;
    expected(&j, "',' or ']'");
    goto fail;
  }
  j.p++;
  ws(&j);
  if (j.p != j.end) {
    expected(&j, "end of input");
    goto fail;
  }
  for (i = 0; i < d->channel_count; i++) {
    mr_iptv_channel *c = &d->channels[i];
    free(c->streams);
    c->streams = NULL;
    c->stream_count = 0;
    for (k = 0; k < n; k++) {
      if (!strcmp(all[k].channel, c->id) && !add_stream(c, &all[k].stream)) {
        snprintf(last_error, sizeof(last_error),
                 "streams.json: allocation failed joining channel %s", c->id);
        goto fail;
      }
    }
    for (k = 0; k < n && c->stream_count < MR_IPTV_STREAM_MAX; k++) {
      size_t z = strlen(c->id);
      if (!strncmp(all[k].channel, c->id, z) && all[k].channel[z] == '@' &&
          !add_stream(c, &all[k].stream)) {
        snprintf(last_error, sizeof(last_error),
                 "streams.json: allocation failed joining channel %s", c->id);
        goto fail;
      }
    }
    if (!c->stream_count) {
      free(c->alt_names);
      free(c->categories);
      free(c->streams);
      d->skipped_channel_count++;
    } else {
      if (retained != i)
        d->channels[retained] = *c;
      retained++;
    }
  }
  d->channel_count = retained;
  d->parsed_stream_count = parsed_streams;
  free(all);
  return 1;
fail:
  if (!last_error[0])
    expected(&j, "valid JSON");
  free(all);
  return 0;
}
