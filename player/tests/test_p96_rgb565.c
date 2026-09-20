/* cc -std=c99 -Wall -Wextra -Werror -Iplayer/amiga player/tests/test_p96_rgb565.c -o /tmp/test_p96_rgb565 */
#include <assert.h>
#include <string.h>
#include "p96_rgb565.h"

int main(void)
{
    unsigned char dst[8];
    const unsigned char rgb[] = {255, 0, 0, 0, 255, 0};
    const unsigned char bgr[] = {0, 0, 255, 0, 255, 0};
    const unsigned char yvyu_black_white[] = {16, 128, 235, 128};
    const unsigned char yuyv_black_white[] = {16, 128, 235, 128};
    const unsigned char yvyu_red[] = {81, 240, 81, 90};
    const unsigned char yuyv_red[] = {81, 90, 81, 240};

    memset(dst, 0xaa, sizeof dst);
    mr_p96_rgb565_rgb24_row(rgb, dst, 2, 0, 1);
    assert(dst[0] == 0x00 && dst[1] == 0xf8); /* red 0xf800 LE */
    assert(dst[2] == 0xe0 && dst[3] == 0x07); /* green 0x07e0 LE */
    assert(dst[4] == 0xaa); /* no overrun */
    mr_p96_rgb565_rgb24_row(bgr, dst, 2, 1, 0);
    assert(dst[0] == 0xf8 && dst[1] == 0x00); /* red BE */
    assert(dst[2] == 0x07 && dst[3] == 0xe0); /* green BE */

    mr_p96_rgb565_yuv422_row(yvyu_black_white, dst, 2, 0, 1);
    assert(dst[0] == 0 && dst[1] == 0); /* black */
    assert(dst[2] == 0xff && dst[3] == 0xff); /* white */
    mr_p96_rgb565_yuv422_row(yuyv_black_white, dst, 2, 1, 0);
    assert(dst[0] == 0 && dst[1] == 0);
    assert(dst[2] == 0xff && dst[3] == 0xff);
    mr_p96_rgb565_yuv422_row(yvyu_red, dst, 2, 0, 0);
    assert(dst[0] >= 0xf0 && dst[1] <= 0x1f);
    mr_p96_rgb565_yuv422_row(yuyv_red, dst, 2, 1, 0);
    assert(dst[0] >= 0xf0 && dst[1] <= 0x1f);
    return 0;
}