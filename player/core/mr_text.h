#ifndef MR_TEXT_H
#define MR_TEXT_H

#include <stddef.h>

/* Display text for Amiga system fonts, which are ISO Latin-1.
 *
 * YouTube titles and IPTV channel names arrive as Unicode.  Latin-1 code
 * points are kept as they are; everything else that has a sensible Latin-1
 * spelling is transliterated (typographic quotes, dashes and ellipses, the
 * accented letters of Central European and Vietnamese text, Cyrillic,
 * Greek, fullwidth and "bold/italic" maths letters); emoji, invisible
 * format characters and combining marks are dropped; and a run of anything
 * else (CJK, Arabic, ...) becomes a single '?'.  Control characters become
 * spaces, runs of spaces collapse to one and the result is trimmed.
 *
 * Usage: mr_text_begin(), then mr_text_put() per Unicode scalar (or
 * mr_text_put_utf8() for a byte string), then mr_text_end().  Output is
 * always NUL-terminated and never splits a transliteration at the end of
 * the buffer. */

typedef struct mr_text {
    char *out;
    size_t cap, used;
    int pending_unknown; /* a '?' is owed for a run of unsupported scalars */
    int full;
} mr_text;

/* Cyrillic U+0400..U+042F; lower case is U+0430..U+045F. */
static const char *const mr_text_cyrillic[48] = {
    "E", "Yo", "Dj", "G", "Ye", "Dz", "I", "Yi",
    "J", "Lj", "Nj", "C", "K", "I", "U", "Dz",
    "A", "B", "V", "G", "D", "E", "Zh", "Z",
    "I", "Y", "K", "L", "M", "N", "O", "P",
    "R", "S", "T", "U", "F", "Kh", "Ts", "Ch",
    "Sh", "Shch", "", "Y", "", "E", "Yu", "Ya"
};

/* Greek capitals U+0391..U+03A9; lower case is U+03B1..U+03C9. */
static const char *const mr_text_greek[25] = {
    "A", "V", "G", "D", "E", "Z", "I", "Th", "I", "K", "L", "M", "N",
    "X", "O", "P", "R", "S", "S", "T", "Y", "F", "Ch", "Ps", "O"
};

/* Base letters of U+0100..U+017F (IJ and OE are handled separately). */
static const char mr_text_latin_ext_a[] =
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi"
    "IiIiJjKkkLlLlLlL" "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs"
    "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";

/* Base letters of U+1EA0..U+1EF9 (Vietnamese). */
static const char mr_text_vietnamese[] =
    "AaAaAaAaAaAaAaAa" "AaAaAaAaEeEeEeEe" "EeEeEeEeIiIiOoOo"
    "OoOoOoOoOoOoOoOo" "OoOoUuUuUuUuUuUu" "UuYyYyYyYy";

/* Unicode for Windows-1252 bytes 0x80..0x9F; 0 where cp1252 has none. */
static const unsigned short mr_text_cp1252[32] = {
    0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
    0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178
};

/* Lower-cases an ASCII transliteration into buf. */
static inline const char *mr_text_lower(const char *s, char *buf)
{
    size_t i;
    for (i = 0; s[i]; i++)
        buf[i] = s[i] >= 'A' && s[i] <= 'Z' ? (char)(s[i] - 'A' + 'a') : s[i];
    buf[i] = 0;
    return buf;
}

static inline const char *mr_text_one(unsigned long c, char *buf)
{
    buf[0] = (char)c;
    buf[1] = 0;
    return buf;
}

/* The Latin-1 spelling of cp: a string ("" drops it), or NULL when it has
 * none.  buf must hold 8 bytes. */
static inline const char *mr_text_latin1(unsigned long cp, char *buf)
{
    if (cp < 0x20 || cp == 0x7f) return " ";
    if (cp < 0x7f) return mr_text_one(cp, buf);
    if (cp < 0xa0 || cp == 0xad) return "";              /* C1, soft hyphen */
    if (cp == 0xa0) return " ";
    if (cp <= 0xff) return mr_text_one(cp, buf);
    if (cp == 0x132) return "IJ";
    if (cp == 0x133) return "ij";
    if (cp == 0x152) return "OE";
    if (cp == 0x153) return "oe";
    if (cp <= 0x17f) return mr_text_one((unsigned char)mr_text_latin_ext_a[cp - 0x100], buf);
    if (cp == 0x192) return "f";
    if (cp == 0x1a0 || cp == 0x1a1) return cp & 1 ? "o" : "O";
    if (cp == 0x1af || cp == 0x1b0) return cp & 1 ? "U" : "u";
    if (cp >= 0x1cd && cp <= 0x1dc)                      /* pinyin tones */
        return mr_text_one((unsigned char)"AaIiOoUuUuUuUuUu"[cp - 0x1cd], buf);
    if (cp >= 0x218 && cp <= 0x21b) return mr_text_one((unsigned char)"SsTt"[cp - 0x218], buf);
    if (cp == 0x2b9 || cp == 0x2bb || cp == 0x2bc) return "'";
    if (cp == 0x2c6) return "^";
    if (cp == 0x2dc) return "~";
    if (cp >= 0x300 && cp <= 0x36f) return "";            /* combining marks */
    if (cp >= 0x386 && cp <= 0x3ce) {
        switch (cp) {
        case 0x386: return "A";
        case 0x388: return "E";
        case 0x389: case 0x38a: case 0x3aa: return "I";
        case 0x38c: case 0x38f: return "O";
        case 0x38e: case 0x3ab: return "Y";
        case 0x390: case 0x3af: case 0x3ca: case 0x3ae: return "i";
        case 0x3ac: return "a";
        case 0x3ad: return "e";
        case 0x3b0: case 0x3cb: case 0x3cd: return "y";
        case 0x3cc: case 0x3ce: return "o";
        case 0x3c2: return "s";
        }
        if (cp >= 0x391 && cp <= 0x3a9 && cp != 0x3a2) return mr_text_greek[cp - 0x391];
        if (cp >= 0x3b1 && cp <= 0x3c9) return mr_text_lower(mr_text_greek[cp - 0x3b1], buf);
        return NULL;
    }
    if (cp >= 0x400 && cp <= 0x42f) return mr_text_cyrillic[cp - 0x400];
    if (cp >= 0x430 && cp <= 0x44f) return mr_text_lower(mr_text_cyrillic[cp - 0x430 + 16], buf);
    if (cp >= 0x450 && cp <= 0x45f) return mr_text_lower(mr_text_cyrillic[cp - 0x450], buf);
    if (cp == 0x490) return "G";
    if (cp == 0x491) return "g";
    if (cp >= 0x1ea0 && cp <= 0x1ef9) return mr_text_one((unsigned char)mr_text_vietnamese[cp - 0x1ea0], buf);
    if ((cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
        cp == 0x202f || cp == 0x205f || cp == 0x3000)
        return " ";
    if ((cp >= 0x200b && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) ||
        (cp >= 0x2060 && cp <= 0x206f) || (cp >= 0x20d0 && cp <= 0x20ff) ||
        (cp >= 0xfe00 && cp <= 0xfe0f) || cp == 0xfeff ||
        (cp >= 0xe0000 && cp <= 0xe007f))
        return "";                                        /* invisible, keycaps */
    if (cp >= 0x2010 && cp <= 0x2015) return "-";
    if ((cp >= 0x2018 && cp <= 0x201b) || cp == 0x2032) return "'";
    if ((cp >= 0x201c && cp <= 0x201f) || cp == 0x2033) return "\"";
    switch (cp) {
    case 0x2020: return "+";
    case 0x2022: case 0x2219: case 0x25cf: return "\xb7";
    case 0x2024: return ".";
    case 0x2025: return "..";
    case 0x2026: return "...";
    case 0x2030: return "%";
    case 0x2039: return "<";
    case 0x203a: return ">";
    case 0x203c: return "!!";
    case 0x2044: case 0x2215: return "/";
    case 0x2047: return "??";
    case 0x2048: return "?!";
    case 0x2049: return "!?";
    case 0x20a4: return "\xa3";
    case 0x20ac: return "EUR";
    case 0x2116: return "No";
    case 0x2117: return "(P)";
    case 0x2122: return "TM";
    case 0x2190: return "<-";
    case 0x2192: return "->";
    case 0x2194: return "<->";
    case 0x21d2: return "=>";
    case 0x2212: return "-";
    case 0x2217: return "*";
    case 0x2248: return "~";
    case 0x2260: return "!=";
    case 0x2264: return "<=";
    case 0x2265: return ">=";
    case 0x2500: case 0x2501: return "-";
    case 0x2502: case 0x2503: return "|";
    case 0x25b6: case 0x25ba: return ">";
    case 0x25c0: case 0x25c4: return "<";
    case 0x2605: case 0x2606: return "*";
    case 0x3001: return ",";
    case 0x3002: return ".";
    case 0x300c: case 0x300d: case 0x300e: case 0x300f: return "\"";
    case 0x3010: return "[";
    case 0x3011: return "]";
    }
    if ((cp >= 0x25a0 && cp <= 0x25ff) || (cp >= 0x2600 && cp <= 0x27bf) ||
        (cp >= 0x2b00 && cp <= 0x2bff) || (cp >= 0x1f000 && cp <= 0x1faff))
        return "";                                        /* emoji, dingbats */
    if (cp >= 0xff01 && cp <= 0xff5e) return mr_text_one(cp - 0xfee0, buf);
    if (cp >= 0x1d400 && cp <= 0x1d6a3) {                 /* maths A-Z a-z */
        unsigned long i = (cp - 0x1d400) % 52;
        return mr_text_one(i < 26 ? 'A' + i : 'a' + i - 26, buf);
    }
    if (cp >= 0x1d7ce && cp <= 0x1d7ff) return mr_text_one('0' + (cp - 0x1d7ce) % 10, buf);
    return NULL;
}

static inline void mr_text_begin(mr_text *t, char *out, size_t cap)
{
    t->out = out;
    t->cap = cap;
    t->used = 0;
    t->pending_unknown = 0;
    t->full = cap == 0;
    if (cap)
        out[0] = 0;
}

static inline void mr_text_emit(mr_text *t, const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    if (t->full) return;
    if (s[0] == ' ' && (t->used == 0 || t->out[t->used - 1] == ' '))
        return;
    if (t->used + n >= t->cap) {
        t->full = 1;
        return;
    }
    while (*s) t->out[t->used++] = *s++;
}

static inline void mr_text_put(mr_text *t, unsigned long cp)
{
    char buf[8];
    const char *s = mr_text_latin1(cp, buf);

    if (!s) {
        t->pending_unknown = 1;
        return;
    }
    if (!s[0]) return;
    if (t->pending_unknown) {
        t->pending_unknown = 0;
        mr_text_emit(t, "?");
    }
    mr_text_emit(t, s);
}

/* Decodes one scalar from s[0..len), which is not empty, and returns the
 * byte count.  A byte that does not start valid UTF-8 is read as
 * Windows-1252, the usual encoding of such text. */
static inline size_t mr_text_utf8_next(const char *s, size_t len, unsigned long *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned long v = 0, minimum = 0;
    size_t n, i;

    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if (p[0] >= 0xc2 && p[0] <= 0xdf) { n = 2; minimum = 0x80; v = p[0] & 0x1f; }
    else if (p[0] >= 0xe0 && p[0] <= 0xef) { n = 3; minimum = 0x800; v = p[0] & 0x0f; }
    else if (p[0] >= 0xf0 && p[0] <= 0xf4) { n = 4; minimum = 0x10000; v = p[0] & 0x07; }
    else n = 0;
    if (n && n <= len) {
        for (i = 1; i < n && (p[i] & 0xc0) == 0x80; i++)
            v = (v << 6) | (p[i] & 0x3f);
        if (i == n && v >= minimum && v <= 0x10ffff && (v < 0xd800 || v > 0xdfff)) {
            *cp = v;
            return n;
        }
    }
    if (p[0] < 0xa0)
        *cp = mr_text_cp1252[p[0] - 0x80] ? mr_text_cp1252[p[0] - 0x80] : 0xfffd;
    else
        *cp = p[0];
    return 1;
}

static inline void mr_text_put_utf8(mr_text *t, const char *s, size_t len)
{
    while (len > 0) {
        unsigned long cp;
        size_t n = mr_text_utf8_next(s, len, &cp);
        mr_text_put(t, cp);
        s += n;
        len -= n;
    }
}

/* Finishes the text and returns its length. */
static inline size_t mr_text_end(mr_text *t)
{
    if (t->pending_unknown) {
        t->pending_unknown = 0;
        mr_text_emit(t, "?");
    }
    while (t->used > 0 && t->out[t->used - 1] == ' ')
        t->used--;
    if (t->cap)
        t->out[t->used] = 0;
    return t->used;
}

/* Converts UTF-8 s[0..len) into Latin-1 display text in out. */
static inline size_t mr_text_from_utf8(char *out, size_t cap, const char *s, size_t len)
{
    mr_text t;
    mr_text_begin(&t, out, cap);
    mr_text_put_utf8(&t, s, len);
    return mr_text_end(&t);
}

#endif
