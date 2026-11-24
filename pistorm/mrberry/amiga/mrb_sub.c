/* S1 (28.10.2026): SRT subtitles for mrberry's OSD layer.
 *
 * mrb_sub_load() reads <film>.srt (same name, extension .srt), converts the
 * text to Latin-1 (UTF-8 or Windows-1252 files), removes <i>/<font> and
 * {\an8} tags and keeps the cues sorted by start time.
 * mrb_sub_tick(film time) says whether the picture has to change: a new cue
 * starts (picture drawn, show it) or the shown cue ended (hide it).  When no
 * text is due nothing is drawn or sent; the layer is only hidden.
 *
 * Picture: MRB_SUB_W x MRB_SUB_H RGBA, transparent, up to two lines centred
 * at the bottom, white DejaVu Sans Bold with a soft black outline. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mrb_sub.h"
#include "s1_font.h"

typedef struct {
    uint32_t start_ms, end_ms;
    char *text;                       /* Latin-1, lines separated by '\n' */
} mrb_cue;

static mrb_cue *cues;
static unsigned ncues;
static int shown = -1;                /* cue on screen, -1 = none */
static uint8_t *pix;                  /* RGBA picture */
static uint8_t *fillA, *tmp1, *tmp2, *tmp3;  /* alpha work buffers */

#define OUTLINE 3
#define LINE_H  56
#define MAX_TEXT_W (MRB_SUB_W - 2 * 24)

/* ---- character set ----------------------------------------------------- */
static int utf8_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;
    int high = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c < 0x80) { i++; continue; }
        high = 1;
        if ((c & 0xe0) == 0xc0 && i + 1 < n && (s[i + 1] & 0xc0) == 0x80) { i += 2; continue; }
        if ((c & 0xf0) == 0xe0 && i + 2 < n && (s[i + 1] & 0xc0) == 0x80 &&
            (s[i + 2] & 0xc0) == 0x80) { i += 3; continue; }
        if ((c & 0xf8) == 0xf0 && i + 3 < n && (s[i + 1] & 0xc0) == 0x80 &&
            (s[i + 2] & 0xc0) == 0x80 && (s[i + 3] & 0xc0) == 0x80) { i += 4; continue; }
        return 0;
    }
    return high;
}

/* Unicode code point -> Latin-1 (some typographic characters as ASCII). */
static size_t put_cp(char *d, unsigned long cp)
{
    if (cp < 0x100) {
        if (cp >= 0x80 && cp < 0xa0) return 0;   /* C1 controls */
        d[0] = (char)cp;
        return 1;
    }
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201a: case 0x2032: d[0] = '\''; return 1;
    case 0x201c: case 0x201d: case 0x201e: case 0x2033: d[0] = '"'; return 1;
    case 0x2013: case 0x2014: case 0x2212: d[0] = '-'; return 1;
    case 0x2026: memcpy(d, "...", 3); return 3;
    case 0x20ac: memcpy(d, "EUR", 3); return 3;
    case 0x2022: d[0] = (char)0xb7; return 1;
    case 0x00a0: d[0] = ' '; return 1;
    case 0xfeff: return 0;                     /* BOM */
    default: d[0] = '?'; return 1;
    }
}

/* Windows-1252 bytes 0x80..0x9f */
static const unsigned short cp1252[32] = {
    0x20ac,0,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,0x02c6,0x2030,0x0160,0x2039,0x0152,0,0x017d,0,
    0,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x02dc,0x2122,0x0161,0x203a,0x0153,0,0x017e,0x0178
};

static char *to_latin1(const unsigned char *s, size_t n)
{
    char *out = (char *)malloc(n * 3 + 1), *d = out;
    size_t i = 0;
    int u8 = utf8_valid(s, n);
    if (!out) return NULL;
    if (n >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) { i = 3; u8 = 1; }
    while (i < n) {
        unsigned c = s[i];
        unsigned long cp;
        if (c < 0x80) { *d++ = (char)c; i++; continue; }
        if (u8) {
            if ((c & 0xe0) == 0xc0 && i + 1 < n) { cp = ((c & 0x1fUL) << 6) | (s[i + 1] & 0x3f); i += 2; }
            else if ((c & 0xf0) == 0xe0 && i + 2 < n) {
                cp = ((c & 0x0fUL) << 12) | ((s[i + 1] & 0x3fUL) << 6) | (s[i + 2] & 0x3f); i += 3;
            } else { cp = '?'; i += ((c & 0xf8) == 0xf0) ? 4 : 1; }
        } else {
            cp = c;
            if (c >= 0x80 && c < 0xa0) cp = cp1252[c - 0x80] ? cp1252[c - 0x80] : '?';
            i++;
        }
        d += put_cp(d, cp);
    }
    *d = 0;
    return out;
}

/* ---- SRT parsing ------------------------------------------------------- */
static int parse_time(const char *p, uint32_t *ms, const char **end)
{
    unsigned long h = 0, m = 0, s = 0, f = 0;
    int nd = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') h = h * 10 + (unsigned long)(*p++ - '0');
    if (*p++ != ':') return 0;
    while (*p >= '0' && *p <= '9') m = m * 10 + (unsigned long)(*p++ - '0');
    if (*p++ != ':') return 0;
    while (*p >= '0' && *p <= '9') s = s * 10 + (unsigned long)(*p++ - '0');
    if (*p == ',' || *p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (nd < 3) { f = f * 10 + (unsigned long)(*p - '0'); nd++; }
            p++;
        }
        while (nd < 3) { f *= 10; nd++; }
    }
    *ms = (uint32_t)(((h * 60 + m) * 60 + s) * 1000 + f);
    if (end) *end = p;
    return 1;
}

/* remove <...> and {...} tags, trim spaces; in place */
static void clean_line(char *s)
{
    char *d = s, *p = s;
    while (*p) {
        if (*p == '<') { char *q = strchr(p, '>'); if (q) { p = q + 1; continue; } }
        if (*p == '{') { char *q = strchr(p, '}'); if (q) { p = q + 1; continue; } }
        if (*p == '\t') { *d++ = ' '; p++; continue; }
        *d++ = *p++;
    }
    *d = 0;
    while (d > s && d[-1] == ' ') *--d = 0;
    p = s;
    while (*p == ' ') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int cue_cmp(const void *a, const void *b)
{
    const mrb_cue *x = (const mrb_cue *)a, *y = (const mrb_cue *)b;
    return x->start_ms < y->start_ms ? -1 : x->start_ms > y->start_ms ? 1 : 0;
}

static unsigned parse_srt(char *t)
{
    unsigned cap = 0;
    char *line = t;
    while (line && *line) {
        char *nl = strchr(line, '\n'), *arrow;
        if (nl) *nl = 0;
        if (nl && nl > line && nl[-1] == '\r') nl[-1] = 0;
        arrow = strstr(line, "-->");
        if (arrow) {
            uint32_t a, b;
            const char *e;
            char text[512];
            size_t tl = 0;
            if (parse_time(line, &a, &e) && parse_time(arrow + 3, &b, NULL) && b > a) {
                /* text lines up to an empty line */
                char *l = nl ? nl + 1 : NULL;
                while (l && *l) {
                    char *n2 = strchr(l, '\n');
                    if (n2) *n2 = 0;
                    if (n2 && n2 > l && n2[-1] == '\r') n2[-1] = 0;
                    if (!*l) { l = n2 ? n2 + 1 : NULL; break; }
                    if (strstr(l, "-->")) {      /* missing blank line: next cue */
                        if (n2) *n2 = '\n';
                        break;
                    }
                    clean_line(l);
                    if (*l && tl + strlen(l) + 2 < sizeof text) {
                        if (tl) text[tl++] = '\n';
                        memcpy(text + tl, l, strlen(l));
                        tl += strlen(l);
                    }
                    l = n2 ? n2 + 1 : NULL;
                }
                text[tl] = 0;
                if (tl) {
                    if (ncues == cap) {
                        unsigned nc = cap ? cap * 2 : 256;
                        mrb_cue *c = (mrb_cue *)realloc(cues, nc * sizeof *c);
                        if (!c) break;
                        cues = c; cap = nc;
                    }
                    cues[ncues].start_ms = a;
                    cues[ncues].end_ms = b;
                    cues[ncues].text = strdup(text);
                    if (cues[ncues].text) ncues++;
                }
                line = l;
                continue;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    /* a number line before "-->" ends up nowhere: only lines after the
     * time line are text.  Sort (files are nearly always sorted). */
    if (ncues > 1) qsort(cues, ncues, sizeof *cues, cue_cmp);
    return ncues;
}

static void cue_cap_reset(void);
/* v96.83: the cues alone (key S loads another file while the film plays;
 * the picture buffers stay) */
static void clear_cues(void)
{
    unsigned i;
    for (i = 0; i < ncues; i++) free(cues[i].text);
    free(cues); cues = NULL; ncues = 0;
    cue_cap_reset();
}

/* Picture and work buffers, once per film.  1 = ok. */
int mrb_sub_buffers(void)
{
    if (pix) return 1;
    pix = (uint8_t *)calloc((size_t)MRB_SUB_W * MRB_SUB_H, 4u);
    fillA = (uint8_t *)malloc((size_t)MRB_SUB_W * MRB_SUB_H);
    tmp1 = (uint8_t *)malloc((size_t)MRB_SUB_W * MRB_SUB_H);
    tmp2 = (uint8_t *)malloc((size_t)MRB_SUB_W * MRB_SUB_H);
    tmp3 = (uint8_t *)malloc((size_t)MRB_SUB_W * MRB_SUB_H);
    if (!pix || !fillA || !tmp1 || !tmp2 || !tmp3) { mrb_sub_free(); return 0; }
    return 1;
}

/* v96.83: no subtitles any more (key S -> Off) */
void mrb_sub_clear(void)
{
    clear_cues();
    if (shown != -1) shown = -2;      /* something may be on screen: hide */
}

void mrb_sub_free(void)
{
    clear_cues();
    free(pix); pix = NULL;
    free(fillA); free(tmp1); free(tmp2); free(tmp3);
    fillA = tmp1 = tmp2 = tmp3 = NULL;
    shown = -1;
}

/* film "dh0:dir/name.mkv" -> "dh0:dir/name.srt" */
static void srt_name(char *dst, size_t size, const char *film)
{
    size_t n = strlen(film), i, dot = (size_t)-1;
    if (n + 5 > size) n = size - 5;
    memcpy(dst, film, n);
    dst[n] = 0;
    for (i = 0; i < n; i++) {
        if (dst[i] == ':' || dst[i] == '/') dot = (size_t)-1;
        else if (dst[i] == '.') dot = i;
    }
    if (dot != (size_t)-1) dst[dot] = 0;
    strcat(dst, ".srt");
}

unsigned mrb_sub_load(const char *film, const char *explicit_name, char *used, size_t used_size)
{
    char name[512];
    FILE *f;
    long len;
    unsigned char *raw;
    char *txt;
    clear_cues();
    shown = -2;                       /* v96.83: next tick decides */
    if (explicit_name && *explicit_name) {
        strncpy(name, explicit_name, sizeof name - 1);
        name[sizeof name - 1] = 0;
    } else srt_name(name, sizeof name, film);
    if (used && used_size) { strncpy(used, name, used_size - 1); used[used_size - 1] = 0; }
    f = fopen(name, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4L * 1024 * 1024) { fclose(f); return 0; }
    raw = (unsigned char *)malloc((size_t)len + 1);
    if (!raw) { fclose(f); return 0; }
    if (fread(raw, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(raw); return 0; }
    fclose(f);
    raw[len] = 0;
    txt = to_latin1(raw, (size_t)len);
    free(raw);
    if (!txt) return 0;
    parse_srt(txt);
    free(txt);
    if (!ncues) return 0;
    if (!mrb_sub_buffers()) return 0;
    return ncues;
}

const uint8_t *mrb_sub_pixels(void) { return pix; }
unsigned mrb_sub_count(void) { return ncues; }

/* ---- drawing ------------------------------------------------------------ */
static int text_width(const char *s, size_t n)
{
    int w = 0;
    size_t i;
    for (i = 0; i < n; i++) w += s1f_glyphs[(unsigned char)s[i]].adv;
    return w;
}

static void draw_glyph(int x, int top, unsigned char c)
{
    const s1f_glyph *g = &s1f_glyphs[c];
    const unsigned char *d = s1f_data + g->off;
    int gx, gy, rowb = (g->w + 1) / 2;
    for (gy = 0; gy < g->h; gy++) {
        int py = top + g->yo + gy;
        if (py < 0 || py >= MRB_SUB_H) continue;
        for (gx = 0; gx < g->w; gx++) {
            int px = x + g->xo + gx;
            unsigned v = d[gy * rowb + gx / 2];
            unsigned a = (gx & 1) ? (v & 15u) : (v >> 4);
            uint8_t *t;
            if (!a || px < 0 || px >= MRB_SUB_W) continue;
            a *= 17u;
            t = fillA + py * MRB_SUB_W + px;
            if (a > *t) *t = (uint8_t)a;
        }
    }
}

/* horizontal max over [x-r, x+r] */
static void hmax(const uint8_t *src, uint8_t *dst, int r)
{
    int x, y, k;
    for (y = 0; y < MRB_SUB_H; y++) {
        const uint8_t *s = src + y * MRB_SUB_W;
        uint8_t *d = dst + y * MRB_SUB_W;
        for (x = 0; x < MRB_SUB_W; x++) {
            uint8_t m = 0;
            int a = x - r < 0 ? 0 : x - r, b = x + r >= MRB_SUB_W ? MRB_SUB_W - 1 : x + r;
            for (k = a; k <= b; k++) if (s[k] > m) m = s[k];
            d[x] = m;
        }
    }
}

/* Split the cue into at most two lines that fit MAX_TEXT_W. */
static int layout(const char *text, char lines[2][256])
{
    char words[512];
    int n = 0;
    size_t wl = 0;
    const char *p;
    /* lines from the file first; if more than two, join and wrap */
    int filelines = 1;
    for (p = text; *p; p++) if (*p == '\n') filelines++;
    strncpy(words, text, sizeof words - 1);
    words[sizeof words - 1] = 0;
    if (filelines <= 2) {
        char *a = words, *nl = strchr(words, '\n');
        if (nl) *nl = 0;
        if (text_width(a, strlen(a)) <= MAX_TEXT_W &&
            (!nl || text_width(nl + 1, strlen(nl + 1)) <= MAX_TEXT_W)) {
            strncpy(lines[0], a, 255); lines[0][255] = 0;
            if (nl) { strncpy(lines[1], nl + 1, 255); lines[1][255] = 0; return 2; }
            return 1;
        }
        if (nl) *nl = ' ';
    } else {
        char *q;
        for (q = words; *q; q++) if (*q == '\n') *q = ' ';
    }
    /* greedy word wrap */
    wl = strlen(words);
    p = words;
    while (*p && n < 2) {
        const char *start = p, *best = NULL;
        const char *q = p;
        while (*q) {
            const char *e = q;
            while (*e && *e != ' ') e++;
            if (text_width(start, (size_t)(e - start)) > MAX_TEXT_W && best) break;
            best = e;
            if (!*e) break;
            q = e + 1;
        }
        if (!best) best = start + strlen(start);
        {
            size_t len = (size_t)(best - start);
            if (len > 255) len = 255;
            if (n == 1 && *best) {            /* text left over: mark it */
                while (len > 0 && text_width(start, len) + text_width("...", 3) > MAX_TEXT_W) len--;
                memcpy(lines[n], start, len);
                memcpy(lines[n] + len, "...", 4);
            } else {
                memcpy(lines[n], start, len);
                lines[n][len] = 0;
            }
        }
        n++;
        p = best;
        while (*p == ' ') p++;
    }
    (void)wl;
    return n;
}

static void draw_cue(const char *text)
{
    char lines[2][256];
    int n = layout(text, lines), i, x, y;
    size_t total = (size_t)MRB_SUB_W * MRB_SUB_H;
    memset(fillA, 0, total);
    for (i = 0; i < n; i++) {
        /* one line: the lower slot */
        int slot = (n == 1) ? 1 : i;
        int top = 8 + slot * LINE_H;
        size_t len = strlen(lines[i]);
        int w = text_width(lines[i], len);
        size_t k;
        x = (MRB_SUB_W - w) / 2;
        for (k = 0; k < len; k++) {
            unsigned char c = (unsigned char)lines[i][k];
            draw_glyph(x, top, c);
            x += s1f_glyphs[c].adv;
        }
    }
    /* outline = round max filter (radius OUTLINE) of the fill alpha */
    hmax(fillA, tmp1, 1);
    hmax(fillA, tmp2, 2);
    hmax(fillA, tmp3, 3);
    for (y = 0; y < MRB_SUB_H; y++) {
        uint8_t *o = pix + (size_t)y * MRB_SUB_W * 4u;
        const uint8_t *f = fillA + y * MRB_SUB_W;
        for (x = 0; x < MRB_SUB_W; x++, o += 4) {
            /* disk radius 3: rows dy=0,±1,±2 use half width 3, ±3 uses 1 */
            unsigned m = 0, a, fa = f[x];
            int dy;
            for (dy = -OUTLINE; dy <= OUTLINE; dy++) {
                int yy = y + dy;
                const uint8_t *row;
                if (yy < 0 || yy >= MRB_SUB_H) continue;
                row = (dy == -3 || dy == 3) ? tmp1 : (dy == -2 || dy == 2) ? tmp2 : tmp3;
                a = row[yy * MRB_SUB_W + x];
                if (a > m) m = a;
            }
            if (!m) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            /* white fill over a black outline, straight alpha */
            o[0] = o[1] = o[2] = (uint8_t)((fa * 255u) / m);
            o[3] = (uint8_t)m;
        }
    }
}

/* Index of the cue on screen at film time ms, or -1. */
static int find_cue(uint32_t ms)
{
    unsigned lo = 0, hi = ncues;
    int i;
    while (lo < hi) {                        /* first cue with start > ms */
        unsigned mid = (lo + hi) / 2;
        if (cues[mid].start_ms <= ms) lo = mid + 1; else hi = mid;
    }
    for (i = (int)lo - 1; i >= 0 && i >= (int)lo - 4; i--)
        if (ms < cues[i].end_ms) return i;
    return -1;
}

int mrb_sub_tick(uint32_t film_ms)
{
    int c;
    if (!pix) return MRB_SUB_KEEP;
    if (!ncues) {                     /* v96.83: Off - clear what is shown */
        if (shown == -1) return MRB_SUB_KEEP;
        shown = -1;
        return MRB_SUB_HIDE;
    }
    c = find_cue(film_ms);
    if (c == shown) return MRB_SUB_KEEP;
    shown = c;
    if (c < 0) return MRB_SUB_HIDE;
    draw_cue(cues[c].text);
    return MRB_SUB_SHOW;
}

const char *mrb_sub_text(int i) { return (i >= 0 && (unsigned)i < ncues) ? cues[i].text : ""; }
int mrb_sub_shown(void) { return shown; }
void mrb_sub_reset(void) { shown = -1; }
/* v96.83: draw a short message (e.g. the new subtitle file) in the
 * subtitle picture; the next tick after it replaces or hides it. */
void mrb_sub_banner(const char *text)
{
    if (!pix) return;
    draw_cue(text);
    shown = -2;
}

/* ---- S2: several SRT files per film ------------------------------------ */
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* fname belongs to the film when it is "<base>.srt" or "<base>.<label>.srt"
 * (case does not matter).  label = the part between, "Standard" for none. */
int mrb_sub_file_label(const char *base, const char *fname, char *label, size_t size)
{
    size_t bl = strlen(base), fl = strlen(fname), i;
    if (fl < bl + 4) return 0;
    for (i = 0; i < bl; i++) if (lc((unsigned char)base[i]) != lc((unsigned char)fname[i])) return 0;
    if (lc(fname[fl - 4]) != '.' || lc(fname[fl - 3]) != 's' ||
        lc(fname[fl - 2]) != 'r' || lc(fname[fl - 1]) != 't') return 0;
    if (fl == bl + 4) {                       /* <base>.srt */
        strncpy(label, "Standard", size - 1); label[size - 1] = 0;
        return 1;
    }
    if (fname[bl] != '.' || fl < bl + 6) return 0;
    {
        size_t n = fl - 4 - (bl + 1);
        if (n >= size) n = size - 1;
        memcpy(label, fname + bl + 1, n);
        label[n] = 0;
    }
    return 1;
}

/* does the label contain the word w (case-insensitive, words split at
 * anything that is not a letter or digit)? */
static int has_word(const char *s, const char *w)
{
    size_t wl = strlen(w), i;
    const char *p = s;
    while (*p) {
        const char *e;
        while (*p && !((lc(*p) >= 'a' && lc(*p) <= 'z') || (*p >= '0' && *p <= '9'))) p++;
        e = p;
        while (*e && ((lc(*e) >= 'a' && lc(*e) <= 'z') || (*e >= '0' && *e <= '9'))) e++;
        if ((size_t)(e - p) == wl) {
            for (i = 0; i < wl; i++) if (lc(p[i]) != lc(w[i])) break;
            if (i == wl) return 1;
        }
        p = e;
    }
    return 0;
}

int mrb_sub_is_forced(const char *label) { return has_word(label, "forced"); }

/* words that name a language, first = ISO 639-1 */
static const char *const lang_words[][8] = {
    {"de","ger","deu","german","deutsch",0},
    {"en","eng","english","englisch",0},
    {"fr","fre","fra","french","francais","franzoesisch",0},
    {"es","spa","spanish","spanisch","espanol",0},
    {"it","ita","italian","italienisch","italiano",0},
    {"pt","por","portuguese","portugiesisch","portugues",0},
    {"nl","dut","nld","dutch","niederlaendisch",0},
    {"ja","jpn","japanese","japanisch",0},
    {"ru","rus","russian","russisch",0},
    {"pl","pol","polish","polnisch",0},
    {"sv","swe","swedish","schwedisch",0},
    {"da","dan","danish","daenisch",0},
    {"no","nor","nob","norwegian","norwegisch",0},
    {"fi","fin","finnish","finnisch",0},
    {"tr","tur","turkish","tuerkisch",0},
    {"zh","chi","zho","chinese","chinesisch",0},
    {"ko","kor","korean","koreanisch",0},
    {0}
};

/* does the label name the audio language lang ("ger", "de", "de-DE")? */
int mrb_sub_lang_match(const char *label, const char *lang)
{
    char code[12];
    size_t i = 0;
    int k, j;
    if (!lang || !*lang) return 0;
    while (lang[i] && lang[i] != '-' && lang[i] != '_' && i < sizeof code - 1) { code[i] = (char)lc(lang[i]); i++; }
    code[i] = 0;
    for (k = 0; lang_words[k][0]; k++) {
        int hit = 0;
        for (j = 0; lang_words[k][j]; j++) if (!strcmp(code, lang_words[k][j])) hit = 1;
        if (hit) {
            for (j = 0; lang_words[k][j]; j++) if (has_word(label, lang_words[k][j])) return 1;
            return 0;
        }
    }
    return has_word(label, code);
}

/* Start rule: a forced file in the language of the sound, else the first
 * forced file, else none.  Returns 1-based index or 0. */
int mrb_sub_pick_default(const char labels[][64], unsigned n, const char *lang)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (mrb_sub_is_forced(labels[i]) && mrb_sub_lang_match(labels[i], lang)) return (int)i + 1;
    for (i = 0; i < n; i++)
        if (mrb_sub_is_forced(labels[i])) return (int)i + 1;
    return 0;
}

/* ---- v96.84: subtitle blocks from the MKV while the film plays ---------
 * text = the block (UTF-8).  ass: Matroska ASS/SSA block
 * "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text"; only the
 * text is used (\N = new line, {...} styling removed). */
static unsigned cue_cap;
int mrb_sub_add(uint32_t start_ms, uint32_t end_ms, const uint8_t *data, size_t len, int ass)
{
    char *raw, *lat, *p, *d, text[512];
    size_t tl = 0, i;
    unsigned pos;
    if (!data || !len || len > 8192 || ncues >= 20000) return 0;
    raw = (char *)malloc(len + 1);
    if (!raw) return 0;
    memcpy(raw, data, len);
    raw[len] = 0;
    p = raw;
    if (ass) {
        int commas = 0;
        while (*p && commas < 8) if (*p++ == ',') commas++;
        if (commas < 8) p = raw;
    }
    /* \N, \n -> new line, \h -> space */
    for (d = p, i = 0; p[i]; i++) {
        if (p[i] == '\\' && (p[i + 1] == 'N' || p[i + 1] == 'n')) { *d++ = '\n'; i++; }
        else if (p[i] == '\\' && p[i + 1] == 'h') { *d++ = ' '; i++; }
        else if (p[i] == '\r') continue;
        else *d++ = p[i];
    }
    *d = 0;
    lat = to_latin1((const unsigned char *)p, strlen(p));
    free(raw);
    if (!lat) return 0;
    for (p = lat; p && *p;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        clean_line(p);
        if (*p && tl + strlen(p) + 2 < sizeof text) {
            if (tl) text[tl++] = '\n';
            memcpy(text + tl, p, strlen(p));
            tl += strlen(p);
        }
        p = nl ? nl + 1 : NULL;
    }
    text[tl] = 0;
    free(lat);
    if (!tl) return 0;
    if (end_ms <= start_ms) end_ms = start_ms + 4000;
    if (ncues == cue_cap || !cues) {
        unsigned nc = cue_cap ? cue_cap * 2 : 256;
        mrb_cue *c;
        if (ncues < cue_cap && cues) nc = cue_cap;
        c = (mrb_cue *)realloc(cues, nc * sizeof *c);
        if (!c) return 0;
        cues = c; cue_cap = nc;
    }
    /* sorted by start (blocks come in order; a seek may go back) */
    pos = ncues;
    while (pos > 0 && cues[pos - 1].start_ms > start_ms) pos--;
    if (pos > 0 && cues[pos - 1].start_ms == start_ms &&
        !strcmp(cues[pos - 1].text, text)) return 0;      /* read twice */
    memmove(cues + pos + 1, cues + pos, (ncues - pos) * sizeof *cues);
    cues[pos].start_ms = start_ms;
    cues[pos].end_ms = end_ms;
    cues[pos].text = strdup(text);
    if (!cues[pos].text) { memmove(cues + pos, cues + pos + 1, (ncues - pos) * sizeof *cues); return 0; }
    ncues++;
    return 1;
}
static void cue_cap_reset(void) { cue_cap = 0; }
