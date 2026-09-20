#!/usr/bin/env python3
"""Apply the P96 MemoryWindow RGB565 compatibility change to a MintVID checkout.

The patch is anchored and refuses to touch a source file whose expected layout
has moved. The original RGBFB_Y4U2V2 path is retained and tried first.
"""
from pathlib import Path
import sys

PATH = Path('player/amiga/display_p96pip.c')


def one(text: str, old: str, new: str, label: str) -> str:
    if text.count(old) != 1:
        raise RuntimeError(f'{label}: expected one source anchor; found {text.count(old)}')
    return text.replace(old, new, 1)


def between(text: str, start: str, end: str, replacement: str, label: str) -> str:
    if text.count(start) != 1 or text.count(end) != 1:
        raise RuntimeError(f'{label}: cannot uniquely identify section')
    i = text.index(start)
    j = text.index(end, i + len(start))
    return text[:i] + replacement + text[j:]


def patch(t: str) -> str:
    t = one(t, '#include "mr_p96_format.h"\n',
            '#include "mr_p96_format.h"\n#include "p96_rgb565.h"\n', 'converter include')
    t = one(t, '    struct BitMap *source_bitmap;   /* P96PIP_SourceBitMap - what we write to */\n',
            '    struct BitMap *source_bitmap;   /* P96PIP_SourceBitMap - what we write to */\n'
            '    ULONG source_format;         /* actual negotiated overlay pixel format */\n',
            'source format state')
    t = one(t, 'static struct Window *open_pip(p96pip_state *s, ULONG type, LONG *err);',
            'static struct Window *open_pip(p96pip_state *s, ULONG type,\n'
            '                               ULONG source_format, LONG *err);',
            'open declaration')
    t = one(t, 'static struct Window *open_pip(p96pip_state *s, ULONG type, LONG *err)\n',
            'static struct Window *open_pip(p96pip_state *s, ULONG type,\n'
            '                               ULONG source_format, LONG *err)\n',
            'open definition')
    t = one(t, '        P96PIP_SourceFormat, (ULONG)RGBFB_Y4U2V2,\n',
            '        P96PIP_SourceFormat, source_format,\n', 'selected source format')

    # Thomas Richter's PIP sample demonstrates that a MemoryWindow is itself
    # hardware-overlaid and rescaled; only the source buffer is fixed-size.
    t = between(t,
        '        /* A real-hardware log (--time) on a driver that only ever grants\n',
        '    }\n\n    /* P96\'s PIP API explicitly ignores WA_Width/WA_Height',
        '        /* The P96 MemoryWindow source has fixed dimensions, but the\n'
        '         * destination may scale in hardware. Do not cap the window\n'
        '         * to the source dimensions: P96PipDemo explicitly enlarges\n'
        '         * a MemoryWindow on Picasso IV/CVision3D. */\n',
        'incorrect memory window resize cap')

    # At native size use the exact default destination behaviour from the
    # known-working P96PipDemo, while preserving aspect-fit tags when needed.
    t = one(t, '    LONG pip_width, pip_height;\n',
            '    LONG pip_width, pip_height;\n    int simple_window;\n', 'simple geometry variable')
    t = one(t, '    *err = 0;\n    win = (struct Window *)p96PIP_OpenTags(\n',
            '    simple_window = !s->fullscreen && s->dx == 0 && s->dy == 0 &&\n'
            '                    s->dw == s->win_w && s->dh == s->win_h;\n'
            '    *err = 0;\n    win = (struct Window *)p96PIP_OpenTags(\n',
            'simple native-window geometry')
    t = one(t,
        '        P96PIP_Relativity, relativity,\n'
        '        P96PIP_Left, (ULONG)s->dx, P96PIP_Top, (ULONG)s->dy,\n'
        '        P96PIP_Width, (ULONG)pip_width,\n'
        '        P96PIP_Height, (ULONG)pip_height,\n',
        '        simple_window ? TAG_IGNORE : P96PIP_Relativity, relativity,\n'
        '        simple_window ? TAG_IGNORE : P96PIP_Left, (ULONG)s->dx,\n'
        '        simple_window ? TAG_IGNORE : P96PIP_Top, (ULONG)s->dy,\n'
        '        simple_window ? TAG_IGNORE : P96PIP_Width, (ULONG)pip_width,\n'
        '        simple_window ? TAG_IGNORE : P96PIP_Height, (ULONG)pip_height,\n',
        'use official defaults for native-size window')

    t = between(t,
        '        s->win = open_pip(s, PIPT_VideoWindow, &err);\n',
        '        if (g_display_want_time) {\n            printf("p96pip: calling p96PIP_GetTags for source bitmap',
        '''        /* VideoWindow refers to live video-input hardware, not a decoded
         * video frame in RAM. Probe the same MemoryWindow formats as the
         * official P96PipDemo, after retaining RiVA's fast YUV-first mode.
         * A format is saved only after its open succeeds. */
        {
            static const ULONG formats[] = {
                RGBFB_Y4U2V2, RGBFB_R5G6B5PC, RGBFB_R5G6B5
            };
            int format_index;
            s->win = NULL;
            s->last_hw_err = 0;
            s->last_mem_err = 0;
            s->hw_overlay = 0;
            for (format_index = 0; format_index < 3; ++format_index) {
                ULONG fmt = formats[format_index];
                s->win = open_pip(s, PIPT_MemoryWindow, fmt, &err);
                if (g_display_want_time) {
                    printf("p96pip: MemoryWindow fmt=%lu %s: %s (err=%ld: %s)\\n",
                           (unsigned long)fmt,
                           fmt == RGBFB_Y4U2V2 ? "YVYU/YUYV" :
                           fmt == RGBFB_R5G6B5PC ? "RGB565PC" : "RGB565BE",
                           s->win ? "opened" : "unavailable",
                           (long)err, pip_err_name(err));
                    Flush(Output());
                }
                if (s->win) {
                    s->source_format = fmt;
                    s->hw_overlay = 1; /* MemoryWindow can use HW overlay. */
                    break;
                }
                s->last_mem_err = err;
            }
        }
        if (!s->win) {
            if (g_display_want_time) {
                printf("p96pip: all MemoryWindow formats failed (err=%ld: %s)\\n",
                       (long)err, pip_err_name(err));
                Flush(Output());
            }
            return 0;
        }

''', 'probe formats in correct PIP type')
    t = one(t,
        '        printf("p96pip: opened %s, window=%dx%d source=%dx%d\\n",\n'
        '               s->hw_overlay ? "hardware (PIPT_VideoWindow)" :\n'
        '                               "PIPT_MemoryWindow overlay",\n'
        '               s->win_w, s->win_h, w, h);\n',
        '        printf("p96pip: opened MemoryWindow format=%lu, window=%dx%d source=%dx%d\\n",\n'
        '               (unsigned long)s->source_format, s->win_w, s->win_h, w, h);\n',
        'opening diagnostics')

    # Convert into the bitmap's negotiated format, NOT into the format we
    # hoped it supported. The little-endian -PC and big-endian formats differ
    # in byte order even on a big-endian 68k CPU.
    t = one(t,
        'static int write_yuv422_rows(struct BitMap *bm, int y0,\n',
        'static int write_yuv422_rows(struct BitMap *bm, ULONG format, int y0,\n',
        'YUV writer signature')
    t = one(t,
        '        memcpy(drow, srow, (size_t)w * 2u);\n',
        '        if (format == RGBFB_Y4U2V2)\n'
        '            memcpy(drow, srow, (size_t)w * 2u);\n'
        '        else\n'
        '            mr_p96_rgb565_yuv422_row(srow, drow, w,\n'
        '                mr_yuv_get_p96_format(), format == RGBFB_R5G6B5PC);\n',
        'YUV writer dispatch')
    t = one(t,
        'static int write_rgb_rows(struct BitMap *bm, int y0,\n',
        'static int write_rgb_rows(struct BitMap *bm, ULONG format, int y0,\n',
        'RGB writer signature')
    t = one(t,
        '        int x;\n        for (x = 0; x < w; x += 2) {\n',
        '        int x;\n        if (format != RGBFB_Y4U2V2) {\n'
        '            mr_p96_rgb565_rgb24_row(src_pixel, dst_pair, w, src_is_bgr,\n'
        '                                      format == RGBFB_R5G6B5PC);\n'
        '            continue;\n'
        '        }\n        for (x = 0; x < w; x += 2) {\n',
        'RGB writer dispatch')
    t = one(t,
        '    if (!write_rgb_rows(s->source_bitmap, dy0,\n',
        '    if (!write_rgb_rows(s->source_bitmap, s->source_format, dy0,\n',
        'RGB caller format')
    timing_old = '        s->timing.dst_format = "Y4U2V2 (P96 PIP source)";\n'
    timing_new = ('        s->timing.dst_format = s->source_format == RGBFB_Y4U2V2\n'
                  '            ? "Y4U2V2 (P96 PIP source)"\n'
                  '            : s->source_format == RGBFB_R5G6B5PC\n'
                  '              ? "RGB565PC (P96 PIP source)" : "RGB565BE (P96 PIP source)";\n')
    if t.count(timing_old) != 2:
        raise RuntimeError('Expected timing output in both RGB and YUV paths')
    t = t.replace(timing_old, timing_new)
    t = one(t,
        '    if (!write_yuv422_rows(s->source_bitmap, dy0,\n',
        '    if (!write_yuv422_rows(s->source_bitmap, s->source_format, dy0,\n',
        'YUV caller format')
    return t


def main() -> None:
    source = PATH.read_text()
    result = patch(source)
    if result == source:
        raise RuntimeError('patch did not change file')
    if '--check' in sys.argv:
        print('Source anchors OK; change can be applied')
    else:
        PATH.write_text(result)
        print('Updated', PATH)


if __name__ == '__main__':
    main()
