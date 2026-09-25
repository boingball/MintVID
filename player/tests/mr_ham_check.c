#include "../core/mr_ham.h"

#include <stdio.h>
#include <string.h>

static unsigned state = 0x68616d38U;
static int absi(int v) { return v < 0 ? -v : v; }

static unsigned char random_byte(void)
{
    state = state * 1664525U + 1013904223U;
    return (unsigned char)(state >> 24);
}

static const unsigned char bayer4[4][4] = {
    {  0,  8,  2, 10 }, { 12,  4, 14,  6 },
    {  3, 11,  1,  9 }, { 15,  7, 13,  5 }
};

/* Straightforward original greedy encoder retained here as a bit-exact oracle.
 * With dither, a modify write adds the 4x4 Bayer threshold scaled to one
 * modify step (0..3 for HAM8, 0..15 for HAM6) before truncating. */
static void reference(const unsigned char *rgb, int w, int h, int stride,
                      unsigned char *out, int out_stride, int bits,
                      int y_base, int dither)
{
    int data_bits = bits - 2, mshift = 8 - data_bits;
    int cshift = data_bits, lowmask = (1 << mshift) - 1, x, y;
    for (y = 0; y < h; y++) {
        const unsigned char *s = rgb + (size_t)y * stride;
        unsigned char *d = out + (size_t)y * out_stride;
        int pr = 0, pg = 0, pb = 0;
        for (x = 0; x < w; x++) {
            int r = s[x*3], g = s[x*3+1], b = s[x*3+2];
            int er = (r & lowmask) + absi(g-pg) + absi(b-pb);
            int eg = absi(r-pr) + (g & lowmask) + absi(b-pb);
            int eb = absi(r-pr) + absi(g-pg) + (b & lowmask);
            int es, idx, cr, cg, cb;
            int t = dither ? (bayer4[(y_base + y) & 3][x & 3] << mshift) >> 4 : 0;
            if (bits >= 8) {
                int qr = (r*3+127)/255, qg = (g*3+127)/255, qb = (b*3+127)/255;
                cr=qr*85; cg=qg*85; cb=qb*85; idx=(qr<<4)|(qg<<2)|qb;
                es=absi(r-cr)+absi(g-cg)+absi(b-cb);
            } else {
                int q = (((r+g+b)/3)*15+127)/255;
                cr=cg=cb=q*17; idx=q;
                es=absi(r-cr)+absi(g-cg)+absi(b-cb);
            }
            if (es<=er && es<=eg && es<=eb) {
                d[x]=(unsigned char)idx; pr=cr; pg=cg; pb=cb;
            } else if (er<=eg && er<=eb) {
                int v=r+t>255?255:r+t, q=v>>mshift; d[x]=(unsigned char)((2<<cshift)|q); pr=q<<mshift;
            } else if (eg<=eb) {
                int v=g+t>255?255:g+t, q=v>>mshift; d[x]=(unsigned char)((3<<cshift)|q); pg=q<<mshift;
            } else {
                int v=b+t>255?255:b+t, q=v>>mshift; d[x]=(unsigned char)((1<<cshift)|q); pb=q<<mshift;
            }
        }
    }
}

int main(void)
{
    unsigned char rgb[37*3*9], expected[43*9], actual[43*9];
    int bits, iteration, i, dither;
    for (dither = 0; dither <= 1; dither++)
    for (bits = 6; bits <= 8; bits += 2) {
        for (iteration = 0; iteration < 500; iteration++) {
            int y_base = iteration % 7;
            for (i = 0; i < (int)sizeof rgb; i++) rgb[i] = random_byte();
            /* Near-white samples exercise the clamp before the truncation. */
            if (iteration % 5 == 0)
                for (i = 0; i < (int)sizeof rgb; i++) rgb[i] |= 0xf0;
            memset(expected, 0xa5, sizeof expected);
            memset(actual, 0xa5, sizeof actual);
            reference(rgb, 37, 9, 37*3, expected, 43, bits, y_base, dither);
            if (!dither && !y_base)
                mr_ham_encode(rgb, 37, 9, 37*3, actual, 43, bits);
            else
                mr_ham_encode_ex(rgb, 37, 9, 37*3, actual, 43, bits, y_base,
                                 dither);
            if (memcmp(expected, actual, sizeof actual)) {
                fprintf(stderr, "HAM%d dither=%d mismatch at iteration %d\n",
                        bits, dither, iteration);
                return 1;
            }
            /* Encoding in row bands with the right y_base must give the
             * same bytes as one pass - aga_show() encodes dirty rows only. */
            memset(actual, 0xa5, sizeof actual);
            mr_ham_encode_ex(rgb, 37, 4, 37*3, actual, 43, bits, y_base,
                             dither);
            mr_ham_encode_ex(rgb + 4*37*3, 37, 5, 37*3, actual + 4*43, 43,
                             bits, y_base + 4, dither);
            if (memcmp(expected, actual, sizeof actual)) {
                fprintf(stderr, "HAM%d dither=%d banded mismatch at "
                        "iteration %d\n", bits, dither, iteration);
                return 1;
            }
        }
    }
    puts("HAM checks passed (plain and dithered)");
    return 0;
}
