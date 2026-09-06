/*
 * Conformance check for vendor/libavc_port/ih264_mc_degrade.c.
 *
 * Two distinct claims are checked, because the module makes two very
 * different kinds of promise:
 *
 *   1. MR_MC_QUALITY_FULL is bit-exact.  Its chroma dispatch shortcuts the
 *      dx==0 / dy==0 cases of spec equation (8-266), and its luma table is
 *      just the existing filter set.  Every sample it produces must equal
 *      Ittiam's ih264_inter_pred_chroma() / ih264_inter_pred_luma_*() for
 *      every dx, dy and block geometry the decoder can ask for.  A rounding
 *      slip here is a silent conformance bug in Quality mode.
 *
 *   2. MR_MC_QUALITY_BILINEAR is an approximation, but a *specified* one:
 *      dst = ((4-dx)(4-dy)A + dx(4-dy)B + (4-dx)dy C + dx dy D + 8) >> 4.
 *      It is checked against a direct transcription of that formula, so the
 *      separable two-pass implementation and its half-sample avg_u8x4()
 *      shortcuts cannot drift from the documented definition.
 *
 * Both run on the host and, via tests/run_m68k_check.sh, on real big-endian
 * m68k under qemu - the packed 32-bit paths in the module load longwords at
 * unaligned prediction positions, which a little-endian host cannot vet.
 */
#include "ih264_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ih264d_structs.h"
#include "ih264_inter_pred_filters.h"
#include "ih264_mc_degrade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRIDE 64
#define PAD    8
#define PLANE  (STRIDE * 64)

static UWORD8 g_src[PLANE];
static UWORD8 g_got[PLANE];
static UWORD8 g_exp[PLANE];
static UWORD8 g_tmp[PLANE];

static UWORD32 g_rand = 12345u;
static UWORD8 next_byte(void)
{
    g_rand = g_rand * 1103515245u + 12345u;
    return (UWORD8)(g_rand >> 16);
}

static void fill_source(void)
{
    int i;
    for(i = 0; i < PLANE; i++) g_src[i] = next_byte();
}

/* The sample the decoder passes as pu1_src: inside the plane, with room for
 * the six-tap window on every side. */
static UWORD8 *src_origin(void) { return g_src + PAD * STRIDE + PAD; }

static int compare(const char *what, WORD32 a, WORD32 b, WORD32 ht, WORD32 wd)
{
    WORD32 r, c;
    for(r = 0; r < ht; r++)
        for(c = 0; c < wd; c++)
            if(g_got[r * STRIDE + c] != g_exp[r * STRIDE + c])
            {
                printf("FAIL %s (%ld,%ld) %ldx%ld at row %ld col %ld: "
                       "got %u want %u\n", what, (long)a, (long)b,
                       (long)wd, (long)ht, (long)r, (long)c,
                       g_got[r * STRIDE + c], g_exp[r * STRIDE + c]);
                return 1;
            }
    return 0;
}

/* ---- 1. chroma fast paths are bit-exact ---- */
static int check_chroma(dec_struct_t *codec)
{
    static const WORD32 sizes[][2] = { {2,2},{4,2},{2,4},{4,4},{8,4},{4,8},{8,8} };
    WORD32 dx, dy, i, fail = 0;

    for(i = 0; i < (WORD32)(sizeof sizes / sizeof sizes[0]); i++)
    {
        WORD32 wd = sizes[i][0], ht = sizes[i][1];
        for(dy = 0; dy < 8; dy++)
            for(dx = 0; dx < 8; dx++)
            {
                fill_source();
                memset(g_got, 0xAA, sizeof g_got);
                memset(g_exp, 0x55, sizeof g_exp);
                codec->pf_inter_pred_chroma(src_origin(), g_got, STRIDE,
                                            STRIDE, dx, dy, ht, wd);
                ih264_inter_pred_chroma(src_origin(), g_exp, STRIDE,
                                        STRIDE, dx, dy, ht, wd);
                fail |= compare("chroma", dx, dy, ht, 2 * wd);
            }
    }
    return fail;
}

/* ---- 2. the exact luma table is untouched ---- */
static int check_luma_exact(dec_struct_t *codec)
{
    static const WORD32 sizes[][2] = { {4,4},{8,4},{4,8},{8,8},{16,8},{8,16},{16,16} };
    WORD32 dydx, i, fail = 0;
    ih264_inter_pred_luma_ft *ref[16];

    ref[0]  = ih264_inter_pred_luma_copy;
    ref[1]  = ih264_inter_pred_luma_horz_qpel;
    ref[2]  = ih264_inter_pred_luma_horz;
    ref[3]  = ih264_inter_pred_luma_horz_qpel;
    ref[4]  = ih264_inter_pred_luma_vert_qpel;
    ref[5]  = ih264_inter_pred_luma_horz_qpel_vert_qpel;
    ref[6]  = ih264_inter_pred_luma_horz_hpel_vert_qpel;
    ref[7]  = ih264_inter_pred_luma_horz_qpel_vert_qpel;
    ref[8]  = ih264_inter_pred_luma_vert;
    ref[9]  = ih264_inter_pred_luma_horz_qpel_vert_hpel;
    ref[10] = ih264_inter_pred_luma_horz_hpel_vert_hpel;
    ref[11] = ih264_inter_pred_luma_horz_qpel_vert_hpel;
    ref[12] = ih264_inter_pred_luma_vert_qpel;
    ref[13] = ih264_inter_pred_luma_horz_qpel_vert_qpel;
    ref[14] = ih264_inter_pred_luma_horz_hpel_vert_qpel;
    ref[15] = ih264_inter_pred_luma_horz_qpel_vert_qpel;

    for(i = 0; i < (WORD32)(sizeof sizes / sizeof sizes[0]); i++)
    {
        WORD32 wd = sizes[i][0], ht = sizes[i][1];
        for(dydx = 0; dydx < 16; dydx++)
        {
            fill_source();
            memset(g_got, 0xAA, sizeof g_got);
            memset(g_exp, 0x55, sizeof g_exp);
            codec->apf_inter_pred_luma[dydx](src_origin(), g_got, STRIDE,
                                             STRIDE, ht, wd, g_tmp, dydx);
            ref[dydx](src_origin(), g_exp, STRIDE, STRIDE, ht, wd,
                      g_tmp, dydx);
            fail |= compare("luma-exact", dydx, 0, ht, wd);
        }
    }
    return fail;
}

/* ---- 3. bilinear matches its documented formula ---- */
static void bilinear_reference(const UWORD8 *src, UWORD8 *dst, WORD32 strd,
                               WORD32 ht, WORD32 wd, WORD32 dx, WORD32 dy)
{
    WORD32 r, c;
    for(r = 0; r < ht; r++)
        for(c = 0; c < wd; c++)
        {
            const UWORD8 *p = src + r * strd + c;
            WORD32 a = p[0], b = p[1], cc = p[strd], d = p[strd + 1];
            WORD32 v = (4 - dx) * (4 - dy) * a + dx * (4 - dy) * b
                     + (4 - dx) * dy * cc + dx * dy * d;
            dst[r * STRIDE + c] = (UWORD8)((v + 8) >> 4);
        }
}

static int check_luma_bilinear(dec_struct_t *codec)
{
    static const WORD32 sizes[][2] = { {4,4},{8,4},{4,8},{8,8},{16,8},{8,16},{16,16} };
    WORD32 dydx, i, fail = 0;

    for(i = 0; i < (WORD32)(sizeof sizes / sizeof sizes[0]); i++)
    {
        WORD32 wd = sizes[i][0], ht = sizes[i][1];
        for(dydx = 0; dydx < 16; dydx++)
        {
            WORD32 dx = dydx & 3, dy = (dydx >> 2) & 3;
            fill_source();
            memset(g_got, 0xAA, sizeof g_got);
            memset(g_exp, 0x55, sizeof g_exp);
            codec->apf_inter_pred_luma[dydx](src_origin(), g_got, STRIDE,
                                             STRIDE, ht, wd, g_tmp, dydx);
            bilinear_reference(src_origin(), g_exp, STRIDE, ht, wd, dx, dy);
            fail |= compare("luma-bilinear", dydx, 0, ht, wd);
        }
    }
    return fail;
}

int main(void)
{
    dec_struct_t *codec = (dec_struct_t *)calloc(1, sizeof *codec);
    int fail = 0;

    if(!codec) { printf("FAIL: out of memory\n"); return 1; }

    mr_h264_port_install_inter_pred(codec, MR_MC_QUALITY_FULL);
    fail |= check_chroma(codec);
    fail |= check_luma_exact(codec);

    mr_h264_port_install_inter_pred(codec, MR_MC_QUALITY_BILINEAR);
    fail |= check_luma_bilinear(codec);
    /* The bilinear set leaves chroma on the exact dispatch. */
    fail |= check_chroma(codec);

    /* Reinstalling must restore the exact table, not layer on the previous
     * selection - mr_h264_set_speed_mode() switches modes mid-stream. */
    mr_h264_port_install_inter_pred(codec, MR_MC_QUALITY_FULL);
    fail |= check_luma_exact(codec);

    free(codec);
    if(fail) { printf("H.264 MC degrade checks FAILED\n"); return 1; }
    printf("H.264 MC filter-set checks passed "
           "(exact chroma/luma bit-identical, bilinear matches spec)\n");
    return 0;
}
