/*
 * Diagnostic accumulators for the H.264 CABAC bin/coeff/mvpred/mbinfo
 * timing breakdown - see ih264d_cabac_profile.h. Plain counters, no
 * wrapping machinery here: unlike ih264d_stage_profile.c (which has to
 * install itself into dec_struct_t's function-pointer table), each of this
 * file's four buckets is fed directly by a --wrap'd entry point that
 * already exists (ih264d_cabac_wrap.c, ih264d_parse_cabac_coeff_port.c,
 * ih264d_mvpred_dispatch_port.c for a real optimisation; ih264d_mbinfo_
 * wrap_port.c purely for this timing), so there is nothing to install -
 * those files just call the add_*() functions below when
 * MR_H264_CABAC_PROFILE opts in.
 */
#include "ih264d_cabac_profile.h"

static unsigned long g_bin_us, g_bin_count;
static unsigned long g_coeff_us, g_coeff_count;
static unsigned long g_mvpred_us, g_mvpred_count;
static unsigned long g_mbinfo_us, g_mbinfo_count;

void mr_h264_cabac_profile_add_bin(unsigned long us)
{
    g_bin_us += us;
    g_bin_count++;
}

void mr_h264_cabac_profile_add_coeff(unsigned long us)
{
    g_coeff_us += us;
    g_coeff_count++;
}

void mr_h264_cabac_profile_add_mvpred(unsigned long us)
{
    g_mvpred_us += us;
    g_mvpred_count++;
}

void mr_h264_cabac_profile_add_mbinfo(unsigned long us)
{
    g_mbinfo_us += us;
    g_mbinfo_count++;
}

void mr_h264_cabac_profile_reset(void)
{
    g_bin_us = 0;
    g_bin_count = 0;
    g_coeff_us = 0;
    g_coeff_count = 0;
    g_mvpred_us = 0;
    g_mvpred_count = 0;
    g_mbinfo_us = 0;
    g_mbinfo_count = 0;
}

void mr_h264_cabac_profile_get(mr_h264_cabac_us *out)
{
    out->bin_us = g_bin_us;
    out->bin_count = g_bin_count;
    out->coeff_us = g_coeff_us;
    out->coeff_count = g_coeff_count;
    out->mvpred_us = g_mvpred_us;
    out->mvpred_count = g_mvpred_count;
    out->mbinfo_us = g_mbinfo_us;
    out->mbinfo_count = g_mbinfo_count;
}
