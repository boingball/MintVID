/* MintVID P96 MemoryWindow RGB565 fallback.
 * Standalone host-testable C; the P96 YUV overlay remains the fast path.
 * Pixels are packed explicitly so the PC and big-endian formats are correct
 * regardless of host/Amiga CPU endianness. */
#ifndef MINTVID_P96_RGB565_H
#define MINTVID_P96_RGB565_H

static unsigned char mr_p96_565_clamp(int v)
{
    return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void mr_p96_565_store(unsigned char *dst, int r, int g, int b,
                             int little_endian)
{
    unsigned int p = ((unsigned int)(r & 0xf8) << 8) |
                     ((unsigned int)(g & 0xfc) << 3) |
                     ((unsigned int)b >> 3);
    dst[0] = (unsigned char)(little_endian ? p : p >> 8);
    dst[1] = (unsigned char)(little_endian ? p >> 8 : p);
}

/* src pixel triplets are RGB24 or BGR24, never ARGB; dst is 2 bytes/pixel. */
static void mr_p96_rgb565_rgb24_row(const unsigned char *src,
                                    unsigned char *dst, int width,
                                    int is_bgr, int little_endian)
{
    int x;
    for (x = 0; x < width; ++x) {
        int r = src[is_bgr ? 2 : 0];
        int g = src[1];
        int b = src[is_bgr ? 0 : 2];
        mr_p96_565_store(dst, r, g, b, little_endian);
        src += 3;
        dst += 2;
    }
}

/* YUV422 from mr_yuv420_to_y4u2v2(): YVYU if yuyv=0, YUYV if yuyv=1.
 * P96's RGB565 overlay needs studio-range BT.601 YUV -> RGB conversion.
 * This path is for boards that rejected native YUV; on YUV-capable cards
 * MintVID directly copies packed YUV and bypasses these multiplications. */
static void mr_p96_rgb565_yuv422_row(const unsigned char *src,
                                     unsigned char *dst, int width,
                                     int yuyv, int little_endian)
{
    int x;
    for (x = 0; x < width; x += 2) {
        int u = (int)src[yuyv ? 1 : 3] - 128;
        int v = (int)src[yuyv ? 3 : 1] - 128;
        int i;
        for (i = 0; i < 2; ++i) {
            int y = (int)src[i * 2] - 16;
            int a, r, g, b;
            if (y < 0) y = 0;
            a = 298 * y + 128;
            r = mr_p96_565_clamp((a + 409 * v) >> 8);
            g = mr_p96_565_clamp((a - 100 * u - 208 * v) >> 8);
            b = mr_p96_565_clamp((a + 516 * u) >> 8);
            mr_p96_565_store(dst + i * 2, r, g, b, little_endian);
        }
        src += 4;
        dst += 4;
    }
}

#endif /* MINTVID_P96_RGB565_H */