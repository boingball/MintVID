/*
 * MintVID - H.264/AVC decoder adapter.
 *
 * Ittiam libavc supplies the actual Baseline/Main/High Profile decoder,
 * including CABAC, B slices, multiple references, deblocking and DPB/display
 * reordering.  This file adapts MintVID's avc1/AVCC packets to libavc's
 * Annex-B API and converts its planar YUV420 output to the RGB24 frame used by
 * the current display backends.
 */
#include "mr_h264.h"
#include "mr_yuv.h"

#include "ih264_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ih264d.h"
#include "ih264d_stage_profile.h"
#include "ih264_mc_degrade.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* H.264 permits at most 16 decoded pictures to be held for reordering.  Keep
 * a wider power-of-two ring so the container PTS supplied with an input access
 * unit is still available when libavc later emits that picture for display. */
#define H264_PTS_MAP_CAP 64U

typedef struct {
    uint32_t timestamp;
    uint64_t pts_us;
    int      valid;
    int      has_pts;
} h264_pts_entry;

#if defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)
#include <dos/dos.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#endif

typedef struct {
    iv_obj_t *handle;
    uint8_t  *packet;
    uint32_t  packet_cap;
    uint8_t  *out[3];
    uint32_t  out_size[3];
    uint32_t  out_count;
    uint8_t  *disp[IVD_VIDDEC_MAX_IO_BUFFERS];
    uint32_t  disp_count;
    uint32_t  held_disp_id;
    int       held_disp_valid;
    uint8_t  *rgb;
    uint8_t   nal_length_size;
    uint32_t  timestamp;
    int       flushing;
    int       flush_done;
    mr_h264_service_fn service;
    void     *service_opaque;
    mr_h264_quit_fn quit_fn;
    void     *quit_opaque;
    mr_h264_timing timing;
    int       skip_output;
    int       timing_enabled;
    int       yuv_output;
    int       input_annexb;
    h264_pts_entry pts_map[H264_PTS_MAP_CAP];
    uint64_t  pending_input_pts_us;
    int       pending_input_pts_set;
    int       pending_input_has_pts;
    uint64_t  output_pts_us;
    int       output_pts_valid;
    /* Wedge-safe diagnostic state (active when diag_path != NULL). */
    const char *diag_path;
    uint32_t    diag_au_idx;
    int         diag_width;
    int         diag_height;
    int         pipeline_rgb_done;
} h264_state;

static void h264_remember_input_pts(h264_state *s, uint32_t timestamp)
{
    h264_pts_entry *entry = &s->pts_map[timestamp & (H264_PTS_MAP_CAP - 1U)];
    entry->timestamp = timestamp;
    entry->pts_us = s->pending_input_pts_us;
    entry->valid = s->pending_input_pts_set;
    entry->has_pts = s->pending_input_has_pts;
    s->pending_input_pts_set = 0;
    s->pending_input_has_pts = 0;
}

static void h264_remember_output_pts(h264_state *s, uint32_t timestamp)
{
    h264_pts_entry *entry = &s->pts_map[timestamp & (H264_PTS_MAP_CAP - 1U)];
    s->output_pts_valid = entry->valid && entry->has_pts &&
                          entry->timestamp == timestamp;
    if (s->output_pts_valid) s->output_pts_us = entry->pts_us;
}

static unsigned long h264_elapsed_us(clock_t begin)
{
    return (unsigned long)((clock() - begin) * 1000000UL / CLOCKS_PER_SEC);
}

#if defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)
/* Peek at the GUI/shell stop signals without consuming them.  The outer player
 * still owns normal event handling and therefore still performs clean teardown
 * after h264_decode() returns. */
static int h264_default_quit_probe(void *opaque)
{
    ULONG sig;
    (void)opaque;
    sig = SetSignal(0, 0);
    return (sig & (SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F)) != 0;
}

static void h264_diag_checkpoint(h264_state *s, const char *phase,
                                 uint32_t au_ts, uint32_t off,
                                 uint32_t remaining, clock_t au_mark,
                                 const ih264d_video_decode_op_t *out,
                                 IV_API_CALL_STATUS_T ret)
{
    BPTR fh;
    char buf[320];
    int n;
    ULONG fast;
    unsigned long elapsed_us;

    /* elapsed_us is computed here, after the diag_path check, rather than
     * by the caller: this checkpoint fires twice per libavc sub-call, and
     * on the normal (sub-720p) path diag_path is never set, so an eagerly-
     * evaluated h264_elapsed_us(au_mark) argument would cost a clock() call
     * every time for a value this function was about to throw away. */
    if (!s || !s->diag_path) return;
    elapsed_us = h264_elapsed_us(au_mark);
    fh = Open((CONST_STRPTR)s->diag_path, MODE_NEWFILE);
    if (!fh) return;

    fast = AvailMem(MEMF_FAST);
    if (out) {
        n = snprintf(buf, sizeof buf,
                     "au=%lu %s off=%lu rem=%lu ts=%lu res=%dx%d "
                     "fast=%lu elapsed=%lu us consumed=%lu output=%lu out-ts=%lu "
                     "decoded=%lu error=%08lx ret=%d\n",
                     (unsigned long)s->diag_au_idx, phase,
                     (unsigned long)off, (unsigned long)remaining,
                     (unsigned long)au_ts, s->diag_width, s->diag_height,
                     (unsigned long)fast, elapsed_us,
                     (unsigned long)out->s_ivd_video_decode_op_t.u4_num_bytes_consumed,
                     (unsigned long)out->s_ivd_video_decode_op_t.u4_output_present,
                     (unsigned long)out->s_ivd_video_decode_op_t.u4_ts,
                     (unsigned long)out->s_ivd_video_decode_op_t.u4_frame_decoded_flag,
                     (unsigned long)out->s_ivd_video_decode_op_t.u4_error_code,
                     (int)ret);
    } else {
        n = snprintf(buf, sizeof buf,
                     "au=%lu %s off=%lu rem=%lu ts=%lu res=%dx%d "
                     "fast=%lu elapsed=%lu us\n",
                     (unsigned long)s->diag_au_idx, phase,
                     (unsigned long)off, (unsigned long)remaining,
                     (unsigned long)au_ts, s->diag_width, s->diag_height,
                     (unsigned long)fast, elapsed_us);
    }
    if (n > 0) {
        LONG bytes = n < (int)sizeof buf ? (LONG)n : (LONG)(sizeof buf - 1);
        Write(fh, (APTR)buf, bytes);
    }
    Close(fh);
}

/* Leave a second, allocation-specific breadcrumb. Total free Fast RAM can be
 * misleading on a fragmented Amiga heap, so record the largest contiguous Fast
 * block as well as the exact allocation libavc asked us to make. */
static void h264_diag_allocfail(h264_state *s, WORD32 alignment, WORD32 size)
{
    BPTR fh;
    char buf[320];
    int n;
    ULONG fast_total, fast_largest, any_total, any_largest;

    if (!s || !s->diag_path) return;
    fh = Open((CONST_STRPTR)"RAM:MintVID-H264.allocfail", MODE_NEWFILE);
    if (!fh) return;

    fast_total = AvailMem(MEMF_FAST);
    fast_largest = AvailMem(MEMF_FAST | MEMF_LARGEST);
    any_total = AvailMem(MEMF_ANY);
    any_largest = AvailMem(MEMF_ANY | MEMF_LARGEST);
    n = snprintf(buf, sizeof buf,
                 "res=%dx%d requested=%ld alignment=%ld fast_total=%lu "
                 "fast_largest=%lu any_total=%lu any_largest=%lu\n",
                 s->diag_width, s->diag_height, (long)size, (long)alignment,
                 (unsigned long)fast_total, (unsigned long)fast_largest,
                 (unsigned long)any_total, (unsigned long)any_largest);
    if (n > 0) {
        LONG bytes = n < (int)sizeof buf ? (LONG)n : (LONG)(sizeof buf - 1);
        Write(fh, (APTR)buf, bytes);
    }
    Close(fh);
}
static void h264_pipeline_checkpoint(h264_state *s, const char *stage)
{
    BPTR fh;
    char buf[256];
    int n;
    ULONG fast_total, fast_largest;

    if (!s || !s->diag_path || !stage) return;
    fh = Open((CONST_STRPTR)"RAM:MintVID-H264.pipeline", MODE_NEWFILE);
    if (!fh) return;
    fast_total = AvailMem(MEMF_FAST);
    fast_largest = AvailMem(MEMF_FAST | MEMF_LARGEST);
    n = snprintf(buf, sizeof buf,
                 "stage=%s res=%dx%d fast=%lu fast_largest=%lu\n",
                 stage, s->diag_width, s->diag_height,
                 (unsigned long)fast_total, (unsigned long)fast_largest);
    if (n > 0) {
        LONG bytes = n < (int)sizeof buf ? (LONG)n : (LONG)(sizeof buf - 1);
        Write(fh, (APTR)buf, bytes);
    }
    Close(fh);
}
#endif

static void *h264_aligned_alloc(void *context, WORD32 alignment, WORD32 size)
{
    h264_state *s = (h264_state *)context;
    uintptr_t p, aligned;
    size_t total;
    void *raw;

    if (size <= 0) return NULL;
    if (alignment < (WORD32)sizeof(void *))
        alignment = (WORD32)sizeof(void *);
    total = (size_t)size + (size_t)alignment - 1 + sizeof(void *);
    if (total < (size_t)size) return NULL;
    raw = malloc(total);
    if (!raw) {
#if defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)
        h264_diag_allocfail(s, alignment, size);
#else
        (void)s;
#endif
        return NULL;
    }
    p = (uintptr_t)raw + sizeof(void *);
    aligned = (p + (uintptr_t)alignment - 1) &
              ~((uintptr_t)alignment - 1);
    ((void **)aligned)[-1] = raw;
    return (void *)aligned;
}

static void h264_aligned_free(void *context, void *ptr)
{
    (void)context;
    if (ptr) free(((void **)ptr)[-1]);
}

static mr_status reserve_packet(h264_state *s, uint32_t need)
{
    uint8_t *p;
    uint32_t cap;
    if (need <= s->packet_cap) return MR_OK;
    cap = s->packet_cap ? s->packet_cap : 4096;
    while (cap < need) {
        uint32_t next = cap < 0x40000000u ? cap * 2u : need;
        if (next < cap || next < need) next = need;
        cap = next;
    }
    p = (uint8_t *)realloc(s->packet, cap);
    if (!p) return MR_ENOMEM;
    s->packet = p;
    s->packet_cap = cap;
    return MR_OK;
}

static mr_status append_annexb_nal(h264_state *s, uint32_t *used,
                                   const uint8_t *nal, uint32_t len)
{
    mr_status st;
    if (!len || *used > UINT32_MAX - len - 4u) return MR_EFORMAT;
    st = reserve_packet(s, *used + len + 4u);
    if (st != MR_OK) return st;
    s->packet[*used + 0] = 0;
    s->packet[*used + 1] = 0;
    s->packet[*used + 2] = 0;
    s->packet[*used + 3] = 1;
    memcpy(s->packet + *used + 4u, nal, len);
    *used += len + 4u;
    return MR_OK;
}

/* Convert AVCDecoderConfigurationRecord (avcC) SPS/PPS arrays to Annex B. */
static mr_status avcc_config_to_annexb(h264_state *s,
                                       const uint8_t *cfg, uint32_t cfg_len,
                                       uint32_t *out_len)
{
    uint32_t p = 6, used = 0;
    unsigned count, i;
    if (!cfg || cfg_len < 7 || cfg[0] != 1) return MR_EFORMAT;
    s->nal_length_size = (uint8_t)((cfg[4] & 3u) + 1u);

    count = cfg[5] & 0x1fu;
    for (i = 0; i < count; i++) {
        uint32_t n;
        mr_status st;
        if (p + 2u > cfg_len) return MR_EFORMAT;
        n = mr_rb16(cfg + p); p += 2;
        if (n > cfg_len - p) return MR_EFORMAT;
        st = append_annexb_nal(s, &used, cfg + p, n);
        if (st != MR_OK) return st;
        p += n;
    }
    if (p >= cfg_len) return MR_EFORMAT;
    count = cfg[p++];
    for (i = 0; i < count; i++) {
        uint32_t n;
        mr_status st;
        if (p + 2u > cfg_len) return MR_EFORMAT;
        n = mr_rb16(cfg + p); p += 2;
        if (n > cfg_len - p) return MR_EFORMAT;
        st = append_annexb_nal(s, &used, cfg + p, n);
        if (st != MR_OK) return st;
        p += n;
    }
    if (!used) return MR_EFORMAT;
    *out_len = used;
    return MR_OK;
}

static uint32_t read_nal_size(const uint8_t *p, unsigned bytes)
{
    uint32_t n = 0;
    unsigned i;
    for (i = 0; i < bytes; i++) n = (n << 8) | p[i];
    return n;
}

/* Convert one MP4 sample (one AVCC access unit) to Annex B. */
static mr_status avcc_sample_to_annexb(h264_state *s,
                                       const uint8_t *data, uint32_t len,
                                       uint32_t *out_len)
{
    uint32_t p = 0, used = 0;
    unsigned nls = s->nal_length_size;
    if (nls < 1 || nls > 4) return MR_EFORMAT;
    while (p < len) {
        uint32_t n;
        mr_status st;
        if (len - p < nls) return MR_EFORMAT;
        n = read_nal_size(data + p, nls);
        p += nls;
        if (!n || n > len - p) return MR_EFORMAT;
#ifdef MR_H264_DEBUG
        fprintf(stderr, " nal=%u type=%u", (unsigned)n,
                (unsigned)(data[p] & 0x1f));
#endif
        st = append_annexb_nal(s, &used, data + p, n);
        if (st != MR_OK) return st;
        p += n;
    }
    if (!used) return MR_EFORMAT;
#ifdef MR_H264_DEBUG
    fputc('\n', stderr);
#endif
    *out_len = used;
    return MR_OK;
}

static IV_API_CALL_STATUS_T set_decode_mode(h264_state *s,
                                            IVD_VIDEO_DECODE_MODE_T mode,
                                            IVD_FRAME_SKIP_MODE_T skip_mode)
{
    ih264d_ctl_set_config_ip_t in;
    ih264d_ctl_set_config_op_t out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.s_ivd_ctl_set_config_ip_t.u4_size = sizeof in;
    in.s_ivd_ctl_set_config_ip_t.e_cmd = IVD_CMD_VIDEO_CTL;
    in.s_ivd_ctl_set_config_ip_t.e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
    in.s_ivd_ctl_set_config_ip_t.e_vid_dec_mode = mode;
    in.s_ivd_ctl_set_config_ip_t.u4_disp_wd = 0;
    in.s_ivd_ctl_set_config_ip_t.e_frm_skip_mode = skip_mode;
    in.s_ivd_ctl_set_config_ip_t.e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
    out.s_ivd_ctl_set_config_op_t.u4_size = sizeof out;
    return ih264d_api_function(s->handle, &in, &out);
}

static void fill_output_desc(const h264_state *s, ivd_out_bufdesc_t *out)
{
    uint32_t i;
    memset(out, 0, sizeof *out);
    out->u4_num_bufs = s->out_count;
    for (i = 0; i < s->out_count && i < 3; i++) {
        out->pu1_bufs[i] = s->out[i];
        out->u4_min_out_buf_size[i] = s->out_size[i];
    }
}

static IV_API_CALL_STATUS_T release_display_buffer(h264_state *s,
                                                    uint32_t id);

/* In shared-display mode libavc decodes luma straight into these buffers.
 * Planar output still requires its UV split, but no longer copies the full
 * luma plane from the decoder's internal semi-planar picture. */
static mr_status setup_display_buffers(h264_state *s,
                                       const ivd_ctl_getbufinfo_op_t *info)
{
    ivd_set_display_frame_ip_t *in;
    ivd_set_display_frame_op_t out;
    size_t total = 0;
    uint32_t i, j;
    IV_API_CALL_STATUS_T ret;

    if (!info->u4_num_disp_bufs ||
        info->u4_num_disp_bufs > IVD_VIDDEC_MAX_IO_BUFFERS)
        return MR_EFORMAT;
    for (j = 0; j < s->out_count; j++) {
        if (total > (size_t)INT32_MAX - s->out_size[j])
            return MR_EFORMAT;
        total += s->out_size[j];
    }

    in = (ivd_set_display_frame_ip_t *)calloc(1, sizeof *in);
    if (!in) return MR_ENOMEM;
    memset(&out, 0, sizeof out);
    in->u4_size = sizeof *in;
    in->e_cmd = IVD_CMD_SET_DISPLAY_FRAME;
    in->num_disp_bufs = info->u4_num_disp_bufs;
    out.u4_size = sizeof out;

    for (i = 0; i < info->u4_num_disp_bufs; i++) {
        ivd_out_bufdesc_t *desc = &in->s_disp_buffer[i];
        uint8_t *p;

        s->disp[i] = (uint8_t *)h264_aligned_alloc(s, 128, (WORD32)total);
        if (!s->disp[i]) {
            free(in);
            return MR_ENOMEM;
        }
        s->disp_count++;
        p = s->disp[i];
        desc->u4_num_bufs = s->out_count;
        for (j = 0; j < s->out_count; j++) {
            desc->pu1_bufs[j] = p;
            desc->u4_min_out_buf_size[j] = s->out_size[j];
            p += s->out_size[j];
        }
    }

    ret = ih264d_api_function(s->handle, in, &out);
    free(in);
    if (ret != IV_SUCCESS) return MR_EFORMAT;

    /* SET_DISPLAY_FRAME initially hands every slot to the application.  Mark
     * all of them available before the first picture; prior to DPB creation
     * libavc records these releases and applies them during initialization. */
    for (i = 0; i < s->disp_count; i++) {
        if (release_display_buffer(s, i) != IV_SUCCESS)
            return MR_EFORMAT;
    }
    return MR_OK;
}

static IV_API_CALL_STATUS_T release_display_buffer(h264_state *s,
                                                    uint32_t id)
{
    ivd_rel_display_frame_ip_t in;
    ivd_rel_display_frame_op_t out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.u4_size = sizeof in;
    in.e_cmd = IVD_CMD_REL_DISPLAY_FRAME;
    in.u4_disp_buf_id = id;
    out.u4_size = sizeof out;
    return ih264d_api_function(s->handle, &in, &out);
}

static IV_API_CALL_STATUS_T release_held_display_buffer(h264_state *s)
{
    IV_API_CALL_STATUS_T ret;
    if (!s->held_disp_valid) return IV_SUCCESS;
    ret = release_display_buffer(s, s->held_disp_id);
    if (ret == IV_SUCCESS) s->held_disp_valid = 0;
    return ret;
}

static IV_API_CALL_STATUS_T decode_annexb(h264_state *s, uint32_t ts,
                                          const uint8_t *data, uint32_t len,
                                          ih264d_video_decode_op_t *out)
{
    ih264d_video_decode_ip_t in;
    memset(&in, 0, sizeof in);
    memset(out, 0, sizeof *out);
    in.s_ivd_video_decode_ip_t.u4_size = sizeof in;
    in.s_ivd_video_decode_ip_t.e_cmd = IVD_CMD_VIDEO_DECODE;
    in.s_ivd_video_decode_ip_t.u4_ts = ts;
    in.s_ivd_video_decode_ip_t.pv_stream_buffer = (void *)data;
    in.s_ivd_video_decode_ip_t.u4_num_Bytes = len;
    fill_output_desc(s, &in.s_ivd_video_decode_ip_t.s_out_buffer);
    out->s_ivd_video_decode_op_t.u4_size = sizeof *out;
    return ih264d_api_function(s->handle, &in, out);
}

static mr_status emit_rgb(mr_decoder *dec,
                          const ivd_video_decode_op_t *base)
{
    h264_state *s = (h264_state *)dec->priv;
    const iv_yuv_buf_t *f = &base->s_disp_frm_buf;
    const uint8_t *yp = (const uint8_t *)f->pv_y_buf;
    const uint8_t *up = (const uint8_t *)f->pv_u_buf;
    const uint8_t *vp = (const uint8_t *)f->pv_v_buf;
    int width = dec->width, height = dec->height;
    if (!yp || !up || !vp) return MR_ERR;
#if defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)
    if (!s->pipeline_rgb_done) h264_pipeline_checkpoint(s, "rgb-enter");
#endif

    if (s->yuv_output) {
        /* Hand back libavc's own display-buffer pointers directly - no
         * RGB24 allocation, no mr_yuv420_to_rgb24() call. Matches the RGB
         * path's dec->frame.width/height/dirty_y1 convention (dec->width/
         * dec->height, not the f->u4_y_wd/u4_y_ht clamp below) for
         * consistency with it. */
        dec->frame.width = dec->width;
        dec->frame.height = dec->height;
        dec->frame.fmt = MR_PIX_YUV420P;
        dec->frame.data = (uint8_t *)yp;
        dec->frame.stride = (int)f->u4_y_strd;
        dec->frame.u_data = (uint8_t *)up;
        dec->frame.v_data = (uint8_t *)vp;
        dec->frame.u_stride = (int)f->u4_u_strd;
        dec->frame.v_stride = (int)f->u4_v_strd;
        dec->frame.dirty_y0 = 0;
        dec->frame.dirty_y1 = dec->height;
#if defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)
        if (!s->pipeline_rgb_done) {
            h264_pipeline_checkpoint(s, "rgb-complete");
            s->pipeline_rgb_done = 1;
        }
#endif
        return MR_OK;
    }

    /* Do not reserve a full RGB24 frame during h264_open(). At 1080p that is
     * 6,220,800 bytes held idle while libavc performs its much larger first-
     * picture / DPB allocations. Allocate RGB only after libavc has actually
     * produced a picture, giving its decoder state first claim on contiguous
     * Fast RAM. */
    if (!s->rgb) {
        size_t rgb_bytes;
        if ((size_t)dec->width * (size_t)dec->height > SIZE_MAX / 3u)
            return MR_ENOMEM;
        rgb_bytes = (size_t)dec->width * (size_t)dec->height * 3u;
        s->rgb = (uint8_t *)malloc(rgb_bytes);
        if (!s->rgb) {
#if defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)
            h264_diag_allocfail(s, (WORD32)sizeof(void *), (WORD32)rgb_bytes);
#endif
            return MR_ENOMEM;
        }
        dec->frame.data = s->rgb;
    }
    if ((int)f->u4_y_wd < width) width = (int)f->u4_y_wd;
    if ((int)f->u4_y_ht < height) height = (int)f->u4_y_ht;

    mr_yuv420_to_rgb24(s->rgb, dec->width * 3,
                       yp, (int)f->u4_y_strd,
                       up, (int)f->u4_u_strd,
                       vp, (int)f->u4_v_strd,
                       width, height, s->service, s->service_opaque);
    dec->frame.width = dec->width;
    dec->frame.height = dec->height;
    dec->frame.fmt = MR_PIX_RGB24;
    dec->frame.stride = dec->width * 3;
    dec->frame.data = s->rgb;
    dec->frame.dirty_y0 = 0;
    dec->frame.dirty_y1 = dec->height;
#if defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)
    if (!s->pipeline_rgb_done) {
        h264_pipeline_checkpoint(s, "rgb-complete");
        s->pipeline_rgb_done = 1;
    }
#endif
    return MR_OK;
}

static void h264_close(mr_decoder *dec);

static mr_status h264_open(mr_decoder *dec)
{
    h264_state *s;
    ih264d_create_ip_t create_in;
    ih264d_create_op_t create_out;
    ih264d_video_decode_op_t decode_out;
    ivd_ctl_getbufinfo_ip_t info_in;
    ivd_ctl_getbufinfo_op_t info_out;
    ih264d_ctl_set_num_cores_ip_t cores_in;
    ih264d_ctl_set_num_cores_op_t cores_out;
    uint32_t cfg_len, off;
    uint32_t i;

    if (!dec->config || dec->config_len < 7) return MR_EFORMAT;
    s = (h264_state *)calloc(1, sizeof *s);
    if (!s) return MR_ENOMEM;
    dec->priv = s;

#if defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)
    /* The GUI stops mrplay with Ctrl-F (and later escalates to Ctrl-C).  Peek
     * those bits between synchronous libavc sub-calls by default so high-res
     * streams do not begin another expensive call after Stop was requested. */
    s->quit_fn = h264_default_quit_probe;
    s->quit_opaque = NULL;

    /* Keep the Open/Write/Close checkpoint off the normal 360p path.  It exists
     * specifically so a wedged 720p/1080p hardware run leaves readable state in
     * RAM: even when stdout remains locked by the still-running player. */
    if ((uint32_t)dec->width * (uint32_t)dec->height >= 1280u * 720u) {
        s->diag_path = "RAM:MintVID-H264.last";
        s->diag_width = dec->width;
        s->diag_height = dec->height;
    }
#endif

    memset(&create_in, 0, sizeof create_in);
    memset(&create_out, 0, sizeof create_out);
    create_in.s_ivd_create_ip_t.u4_size = sizeof create_in;
    create_in.s_ivd_create_ip_t.e_cmd = IVD_CMD_CREATE;
    create_in.s_ivd_create_ip_t.e_output_format = IV_YUV_420P;
    create_in.s_ivd_create_ip_t.u4_share_disp_buf = 1;
    create_in.s_ivd_create_ip_t.pf_aligned_alloc = h264_aligned_alloc;
    create_in.s_ivd_create_ip_t.pf_aligned_free = h264_aligned_free;
    create_in.s_ivd_create_ip_t.pv_mem_ctxt = s;
    create_out.s_ivd_create_op_t.u4_size = sizeof create_out;
    if (ih264d_api_function(NULL, &create_in, &create_out) != IV_SUCCESS)
        goto bad_format;
    s->handle = (iv_obj_t *)create_out.s_ivd_create_op_t.pv_handle;
    s->handle->pv_fxns = (void *)&ih264d_api_function;
    s->handle->u4_size = sizeof *s->handle;

    memset(&cores_in, 0, sizeof cores_in);
    memset(&cores_out, 0, sizeof cores_out);
    cores_in.u4_size = sizeof cores_in;
    cores_in.e_cmd = IVD_CMD_VIDEO_CTL;
    cores_in.e_sub_cmd =
        (IVD_CONTROL_API_COMMAND_TYPE_T)IH264D_CMD_CTL_SET_NUM_CORES;
    cores_in.u4_num_cores = 1;
    cores_out.u4_size = sizeof cores_out;
    if (ih264d_api_function(s->handle, &cores_in, &cores_out) != IV_SUCCESS)
        goto bad_format;

    if (set_decode_mode(s, IVD_DECODE_HEADER, IVD_SKIP_NONE) != IV_SUCCESS)
        goto bad_format;
    if (avcc_config_to_annexb(s, dec->config, dec->config_len,
                              &cfg_len) != MR_OK)
        goto bad_format;

    /* Header mode can consume one NAL at a time.  Continue until every SPS
     * and PPS from avcC has been offered, requiring forward progress. */
    off = 0;
    while (off < cfg_len) {
        IV_API_CALL_STATUS_T ret =
            decode_annexb(s, 0, s->packet + off, cfg_len - off, &decode_out);
        uint32_t used =
            decode_out.s_ivd_video_decode_op_t.u4_num_bytes_consumed;
        if (!used || used > cfg_len - off) {
            if (ret == IV_SUCCESS) break;
            goto bad_format;
        }
        off += used;
    }

    memset(&info_in, 0, sizeof info_in);
    memset(&info_out, 0, sizeof info_out);
    info_in.u4_size = sizeof info_in;
    info_in.e_cmd = IVD_CMD_VIDEO_CTL;
    info_in.e_sub_cmd = IVD_CMD_CTL_GETBUFINFO;
    info_out.u4_size = sizeof info_out;
    if (ih264d_api_function(s->handle, &info_in, &info_out) != IV_SUCCESS)
        goto bad_format;
    if (info_out.u4_min_num_out_bufs < 3) goto bad_format;
    s->out_count = 3;
    for (i = 0; i < 3; i++) {
        s->out_size[i] = info_out.u4_min_out_buf_size[i];
        if (!s->out_size[i]) goto bad_format;
        s->out[i] = (uint8_t *)malloc(s->out_size[i]);
        if (!s->out[i]) goto no_memory;
    }
    {
        mr_status display_status = setup_display_buffers(s, &info_out);
        if (display_status == MR_ENOMEM) goto no_memory;
        if (display_status != MR_OK) goto bad_format;
    }
    if (set_decode_mode(s, IVD_DECODE_FRAME, IVD_SKIP_NONE) != IV_SUCCESS)
        goto bad_format;

    dec->frame.width = dec->width;
    dec->frame.height = dec->height;
    dec->frame.fmt = MR_PIX_RGB24;
    dec->frame.stride = dec->width * 3;
    dec->frame.data = NULL; /* allocated lazily by emit_rgb() */
    dec->frame.dirty_y0 = 0;
    dec->frame.dirty_y1 = 0;
    return MR_OK;

no_memory:
    h264_close(dec);
    return MR_ENOMEM;
bad_format:
    h264_close(dec);
    return MR_EFORMAT;
}

static mr_status h264_decode(mr_decoder *dec,
                             const uint8_t *data, uint32_t len)
{
    h264_state *s = (h264_state *)dec->priv;
    IV_API_CALL_STATUS_T ret;
    uint32_t annexb_len;
    const uint8_t *annexb_buf;
    uint32_t au_ts;
    mr_status st;
    mr_status captured_status = MR_EAGAIN;
    mr_status decode_failure = MR_OK;
    int output_captured = 0;
    int intentional_skip = 0;
    clock_t input_mark;
    clock_t au_mark;

    if (!s || !data || !len) return MR_EFORMAT;
    /* A directly exposed YUV frame is borrowed until this call.  Return its
     * display slot before libavc needs another decode target. */
    if (release_held_display_buffer(s) != IV_SUCCESS) return MR_ERR;
    s->flushing = 0;
    s->flush_done = 0;
    s->output_pts_valid = 0;
    memset(&s->timing, 0, sizeof s->timing);

    /* input_mark/call_mark/rgb_mark/stage-profile bookkeeping below only
     * ever populate s->timing, which nothing reads unless
     * mr_h264_set_timing_enabled() was turned on (mrplay.c does that under
     * --time). Skipping them keeps a normal playback run from paying for
     * several clock() calls every decoded frame for numbers nobody looks
     * at. */
    if (s->input_annexb) {
        /* Already Annex-B (MPEG-TS, via mr_h264_set_input_annexb()) - decode
         * straight from the caller's own buffer, no AVCC->Annex-B scan+copy
         * into s->packet at all. */
        annexb_buf = data;
        annexb_len = len;
    } else {
        if (s->timing_enabled) input_mark = clock();
        st = avcc_sample_to_annexb(s, data, len, &annexb_len);
        if (s->timing_enabled) s->timing.input_us = h264_elapsed_us(input_mark);
        if (st != MR_OK) return st;
        annexb_buf = s->packet;
    }
    if (s->service) s->service(s->service_opaque);

    /* One input AU gets exactly one libavc timestamp, even when the decoder
     * consumes the Annex-B data over several sub-calls. */
    au_ts = s->timestamp++;
    h264_remember_input_pts(s, au_ts);
    /* au_mark only ever feeds h264_diag_checkpoint(), which itself no-ops
     * unless s->diag_path is set (see that function) - i.e. only on a
     * >=720p run with --time. Skip the clock() call on every other decode
     * (the common case, including everything below 720p) rather than
     * compute a value that gets thrown away. */
#if defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)
    au_mark = s->diag_path ? clock() : 0;
#else
    au_mark = 0;
#endif
    ret = IV_SUCCESS;

    {
        uint32_t off = 0;
        while (off < annexb_len) {
            ih264d_video_decode_op_t sub_out;
            IV_API_CALL_STATUS_T r;
            uint32_t used;
            clock_t call_mark;

            /* Never consume the stop signal here: just stop launching more
             * expensive libavc work once the current sub-call has returned. */
            if (s->quit_fn && s->quit_fn(s->quit_opaque))
                break;

#if defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)
            h264_diag_checkpoint(s, "sub-pre", au_ts, off,
                                 annexb_len - off, au_mark,
                                 NULL, IV_SUCCESS);
#endif

            if (s->timing_enabled) {
                mr_h264_stage_profile_reset();
                call_mark = clock();
            }
            r = decode_annexb(s, au_ts, annexb_buf + off,
                              annexb_len - off, &sub_out);
            if (s->timing_enabled) {
                s->timing.core_us += h264_elapsed_us(call_mark);
                mr_h264_stage_us stage;
                mr_h264_stage_profile_get(&stage);
                s->timing.mc_us += stage.mc_us;
                s->timing.deblock_us += stage.deblock_us;
                s->timing.recon_us += stage.recon_us;
                s->timing.intra_us += stage.intra_us;
            }
            used = sub_out.s_ivd_video_decode_op_t.u4_num_bytes_consumed;

#if defined(AMIGA_M68K) && !defined(MR_HOST_BUILD)
            h264_diag_checkpoint(s, "sub-post", au_ts, off,
                                 annexb_len - off, au_mark,
                                 &sub_out, r);
#endif

#ifdef MR_H264_DEBUG
            fprintf(stderr, "h264 ts=%lu out-ts=%lu in=%lu annexb=%lu off=%lu "
                    "ret=%d consumed=%lu decoded=%lu output=%lu "
                    "error=%08lx type=%d\n",
                    (unsigned long)au_ts,
                    (unsigned long)sub_out.s_ivd_video_decode_op_t.u4_ts,
                    (unsigned long)len,
                    (unsigned long)annexb_len, (unsigned long)off,
                    (int)r, (unsigned long)used,
                    (unsigned long)sub_out.s_ivd_video_decode_op_t.u4_frame_decoded_flag,
                    (unsigned long)sub_out.s_ivd_video_decode_op_t.u4_output_present,
                    (unsigned long)sub_out.s_ivd_video_decode_op_t.u4_error_code,
                    (int)sub_out.s_ivd_video_decode_op_t.e_pic_type);
#endif

            /* libavc owns the YUV pointers in sub_out.  Preserve the first
             * output immediately.  RGB and skipped frames can release their
             * shared display slot at once; direct YUV keeps it until the next
             * decode call, matching mr_frame's documented borrowed lifetime. */
            if (sub_out.s_ivd_video_decode_op_t.u4_output_present) {
                uint32_t disp_id =
                    sub_out.s_ivd_video_decode_op_t.u4_disp_buf_id;
                if (!output_captured) {
                    output_captured = 1;
                    s->held_disp_id = disp_id;
                    s->held_disp_valid = 1;
                    h264_remember_output_pts(
                        s, sub_out.s_ivd_video_decode_op_t.u4_ts);
                    if (s->skip_output) {
                        dec->frame.dirty_y0 = dec->frame.dirty_y1 = 0;
                        captured_status = MR_OK;
                    } else {
                        clock_t rgb_mark = 0;
                        if (s->timing_enabled) rgb_mark = clock();
                        captured_status = emit_rgb(
                            dec, &sub_out.s_ivd_video_decode_op_t);
                        if (s->timing_enabled)
                            s->timing.output_us += h264_elapsed_us(rgb_mark);
                    }
                    if (s->skip_output || !s->yuv_output ||
                        captured_status != MR_OK) {
                        if (release_held_display_buffer(s) != IV_SUCCESS &&
                            captured_status == MR_OK)
                            captured_status = MR_ERR;
                    }
                } else {
                    /* One access unit should not normally emit twice, but do
                     * not strand a shared buffer if a damaged stream does. */
                    if (release_display_buffer(s, disp_id) != IV_SUCCESS &&
                        decode_failure == MR_OK)
                        decode_failure = MR_ERR;
                }
            }

            if (r != IV_SUCCESS) {
                uint32_t error = sub_out.s_ivd_video_decode_op_t.u4_error_code;

                /*
                 * Restored libavc Turbo modes intentionally return IV_FAIL
                 * with IVD_DEC_FRM_SKIPPED.  That is flow control, not a bad
                 * bitstream: advance by the bytes the decoder consumed and
                 * let the next sub-call start at the picture boundary it
                 * handed back to us.
                 */
                if ((error & IVD_ERROR_MASK) == IVD_DEC_FRM_SKIPPED) {
                    intentional_skip = 1;
                    if (!used || used > annexb_len - off)
                        break;
                    off += used;
                    if (s->service) s->service(s->service_opaque);
                    continue;
                }

                ret = r;
                /* Hardware caught 0x0000402b at 1080p: fatal plus
                 * IVD_MEM_ALLOC_FAILED. Preserve that distinction so mrplay can
                 * tear down cleanly instead of feeding more AUs to a dead codec. */
                if ((error & IVD_ERROR_MASK) == IVD_MEM_ALLOC_FAILED)
                    decode_failure = MR_ENOMEM;
                else
                    decode_failure = MR_EFORMAT;
                break;
            }
            if (!used || used > annexb_len - off)
                break;

            off += used;
            if (s->service) s->service(s->service_opaque);
        }
        s->diag_au_idx++;
    }

    if (s->service) s->service(s->service_opaque);
    if (decode_failure != MR_OK) {
        release_held_display_buffer(s);
        return decode_failure;
    }
    if (output_captured)
        return captured_status;
    if (intentional_skip)
        return MR_SKIPPED;
    return ret == IV_SUCCESS ? MR_EAGAIN : MR_EFORMAT;
}

void mr_h264_set_skip_output(mr_decoder *dec, int skip)
{
    h264_state *s;
    if (!dec || dec->codec != &mr_codec_h264 || !dec->priv) return;
    s = (h264_state *)dec->priv;
    s->skip_output = skip != 0;
}

/* Off by default: h264_decode()'s per-call clock() timestamps and the
 * mr_h264_stage_profile_reset()/_get() bookkeeping only ever feed
 * mr_h264_frame_timing(), which nothing reads unless a caller actually
 * wants the --time breakdown. mrplay.c turns this on when --time is
 * passed; the host mr_decode CLI never calls this, so it stays disabled
 * there too. */
void mr_h264_set_timing_enabled(mr_decoder *dec, int enabled)
{
    h264_state *s;
    if (!dec || dec->codec != &mr_codec_h264 || !dec->priv) return;
    s = (h264_state *)dec->priv;
    s->timing_enabled = enabled != 0;
}

/* Off by default: emit_rgb() converts to RGB24 as usual unless a caller
 * opts in, in which case it hands back libavc's own Y/Cb/Cr display-buffer
 * pointers directly (dec->frame.fmt = MR_PIX_YUV420P) and skips both the
 * RGB24 allocation and mr_yuv420_to_rgb24() call entirely - for a consumer
 * (e.g. the AGA direct-to-indexed dither or mrplay's direct-to-RTG-queue
 * conversion) that wants the raw planes instead of an RGB24 intermediate.
 * Those pointers are borrowed from
 * libavc's own display picture buffer, exactly like the RGB path already
 * borrows them for the synchronous conversion call - valid until the next
 * decode, same lifetime mr_frame's own contract already documents. */
void mr_h264_set_yuv_output(mr_decoder *dec, int enabled)
{
    h264_state *s;
    if (!dec || dec->codec != &mr_codec_h264 || !dec->priv) return;
    s = (h264_state *)dec->priv;
    s->yuv_output = enabled != 0;
}

void mr_h264_set_input_pts(mr_decoder *dec, int has_pts, uint64_t pts_us)
{
    h264_state *s;
    if (!dec || dec->codec != &mr_codec_h264 || !dec->priv) return;
    s = (h264_state *)dec->priv;
    s->pending_input_pts_us = pts_us;
    s->pending_input_pts_set = 1;
    s->pending_input_has_pts = has_pts != 0;
}

/*
 * Set for the next mr_decoder_decode() call only (consumed and not reset
 * back to 0 here - the caller is expected to pass the current packet's own
 * mr_packet.is_annexb every time, e.g. mrplay.c calling this right before
 * decode alongside mr_h264_set_input_pts()). When set, mr_h264_decode()
 * skips avcc_sample_to_annexb() entirely and decodes straight from the
 * caller's own buffer - see that function and mr_ts.c's emit_pes(), the
 * only current producer of Annex-B-native packets (MPEG-TS).
 */
void mr_h264_set_input_annexb(mr_decoder *dec, int is_annexb)
{
    h264_state *s;
    if (!dec || dec->codec != &mr_codec_h264 || !dec->priv) return;
    s = (h264_state *)dec->priv;
    s->input_annexb = is_annexb != 0;
}

int mr_h264_output_pts(mr_decoder *dec, uint64_t *pts_us)
{
    h264_state *s;
    if (!dec || dec->codec != &mr_codec_h264 || !dec->priv || !pts_us)
        return 0;
    s = (h264_state *)dec->priv;
    if (!s->output_pts_valid) return 0;
    *pts_us = s->output_pts_us;
    return 1;
}

void mr_h264_set_service(mr_decoder *dec, mr_h264_service_fn fn, void *opaque)
{
    h264_state *s;
    if (!dec || dec->codec != &mr_codec_h264 || !dec->priv) return;
    s = (h264_state *)dec->priv;
    s->service = fn;
    s->service_opaque = opaque;
}

void mr_h264_set_quit(mr_decoder *dec, mr_h264_quit_fn fn, void *opaque)
{
    h264_state *s;
    if (!dec || dec->codec != &mr_codec_h264 || !dec->priv) return;
    s = (h264_state *)dec->priv;
    s->quit_fn = fn;
    s->quit_opaque = opaque;
}

void mr_h264_set_diag(mr_decoder *dec, const char *path, int width, int height)
{
    h264_state *s;
    if (!dec || dec->codec != &mr_codec_h264 || !dec->priv) return;
    s = (h264_state *)dec->priv;
    s->diag_path = path;
    s->diag_width = width;
    s->diag_height = height;
    s->diag_au_idx = 0;
}

void mr_h264_frame_timing(mr_decoder *dec, mr_h264_timing *timing)
{
    h264_state *s;
    if (!timing) return;
    memset(timing, 0, sizeof *timing);
    if (!dec || dec->codec != &mr_codec_h264 || !dec->priv) return;
    s = (h264_state *)dec->priv;
    *timing = s->timing;
}

int mr_h264_set_speed_mode(mr_decoder *dec, mr_h264_speed_mode mode)
{
    h264_state *s;
    ih264d_ctl_degrade_ip_t in;
    ih264d_ctl_degrade_op_t out;
    IVD_FRAME_SKIP_MODE_T skip_mode = IVD_SKIP_NONE;
    /*
     * The inter-prediction half of what i4_degrade_type asks for below is
     * dead in the vendored decoder - it sets ps_dec->i4_mv_frac_mask and
     * nothing ever reads it again, so every picture stays at six-tap quality
     * however hard the mode asks for cheaper filters. ih264_mc_degrade.c
     * supplies the missing filter sets; pick one here alongside the degrade
     * request, so a mode that says "fastest inter prediction filters"
     * actually gets them.
     *
     * The swap is per stream rather than per picture, which costs nothing in
     * fidelity: only inter pictures perform motion compensation at all, so a
     * stream-wide swap is exactly libavc's i4_degrade_pics 4 ("all frames")
     * for MC purposes, and every mode that degrades below asks for 4.
     *
     * Every mode asks for 4 because a *mixed* degrade policy is actively
     * harmful in the vendored decoder. i4_degrade_pics 1 and 3 leave some
     * pictures undegraded, and libavc skips ih264d_set_deblocking_parameters()
     * and pf_compute_bs() per macroblock on the degraded ones
     * (ih264d_parse_pslice.c) while ps_dec->ps_deblk_pic - the per-macroblock
     * deblocking descriptor array those calls fill in - persists across
     * pictures. So the next undegraded picture deblocks against the previous
     * pictures' stale MB_DISABLE_FILTERING flags, boundary strengths and QPs:
     * wrong output, and far more edges filtered than the picture actually
     * has. Measured on a 320x180 CABAC stream under qemu-m68k, the old
     * i4_degrade_pics 3 Fast mode spent 51% of the whole decode inside
     * ih264d_deblock_mb_nonmbaff() and ran ~47% SLOWER than Quality, which
     * deblocks every picture properly. Only an all-or-nothing policy keeps
     * that array consistent with the picture being filtered.
     */
    mr_mc_quality mc = MR_MC_QUALITY_FULL;
    if (!dec || dec->codec != &mr_codec_h264 || !dec->priv) return 0;
    s = (h264_state *)dec->priv;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.u4_size = sizeof in;
    in.e_cmd = IVD_CMD_VIDEO_CTL;
    in.e_sub_cmd =
        (IVD_CONTROL_API_COMMAND_TYPE_T)IH264D_CMD_CTL_DEGRADE;
    in.i4_nondegrade_interval = 4;
    switch (mode) {
    case MR_H264_SPEED_BALANCED:
        /* Deblocking off, spec-exact motion compensation. */
        in.i4_degrade_type = (1 << 1);
        in.i4_degrade_pics = 4;
        break;
    case MR_H264_SPEED_TURBO:
        /* Fast's cheap filtering plus decoder-level B-picture skipping. */
        mc = MR_MC_QUALITY_BILINEAR;
        skip_mode = IVD_SKIP_B;
        in.i4_degrade_type = (1 << 1) | (1 << 3);
        in.i4_degrade_pics = 4;
        break;
    case MR_H264_SPEED_TURBO_PLUS:
        /* Deliberately aggressive: ask libavc to skip both P and B pictures,
         * so every displayed frame is a keyframe. That makes the keyframe
         * decode - full intra prediction plus deblocking, with none of the
         * time savings B/P-skip gives every other picture - the single
         * blocking call between displayed frames, and on a slow CPU (e.g. a
         * bare 68060) it can run long enough to drain Paula's whole hardware
         * buffer with no audio_service() in between (h264_decode()'s service
         * hook only fires between NAL sub-calls, never mid-call - see
         * mr_h264_set_service()'s callers). Degrading every decoded picture
         * disables I-frame deblocking here, which shortens exactly that
         * blocking call and keeps Paula fed. Turbo+ is
         * already documented as a last-resort keyframe/slideshow mode, so
         * trading a little keyframe sharpness for smooth audio is the right
         * side of that trade. */
        mc = MR_MC_QUALITY_BILINEAR;
        skip_mode = IVD_SKIP_PB;
        in.i4_degrade_type = (1 << 1) | (1 << 3);
        in.i4_degrade_pics = 4;
        break;
    case MR_H264_SPEED_TURBO_GT:
        /* Retained as a selectable name - the GUI choosers, --h264-speed=
         * turbogt and saved settings all still resolve - but its policy is
         * now Turbo's.
         *
         * TurboGT used to differ from Turbo by asking for i4_degrade_pics 4
         * instead of 3, i.e. by disabling deblocking on keyframes too. That
         * distinction is gone because every degrading mode now asks for 4:
         * mixing degraded and undegraded pictures is what corrupted the
         * per-macroblock deblocking state (see the note above), so an
         * all-or-nothing policy is the only correct one and Turbo already
         * uses it. The remaining candidate lever, truncating motion vectors
         * to whole samples, was measured and rejected - 3-4% for 17 dB (see
         * ih264_mc_degrade.h). Nothing worth having is left between Turbo
         * and Turbo+'s keyframe slideshow, so TurboGT stops pretending
         * otherwise rather than shipping a mode that is only nominally
         * faster. It is now both quicker and much cleaner than the TurboGT
         * of previous releases. */
        mc = MR_MC_QUALITY_BILINEAR;
        skip_mode = IVD_SKIP_B;
        in.i4_degrade_type = (1 << 1) | (1 << 3);
        in.i4_degrade_pics = 4;
        break;
    case MR_H264_SPEED_FAST:
        /* Balanced plus bilinear instead of six-tap interpolation. */
        mc = MR_MC_QUALITY_BILINEAR;
        in.i4_degrade_type = (1 << 1) | (1 << 2);
        in.i4_degrade_pics = 4;
        break;
    default:
        in.i4_degrade_type = 0;
        in.i4_degrade_pics = 0;
        break;
    }
    out.u4_size = sizeof out;
    if (ih264d_api_function(s->handle, &in, &out) != IV_SUCCESS)
        return 0;
    if (!mr_h264_port_set_mc_quality(s->handle, mc)) return 0;

    /*
     * The vendored libavc frame-skip API is restored by the companion
     * submodule patch. Use SETPARAMS again so the decoder owns all picture
     * boundary bookkeeping rather than reaching into dec_struct_t here.
     */
    return set_decode_mode(s, IVD_DECODE_FRAME, skip_mode) == IV_SUCCESS;
}

static mr_status h264_flush(mr_decoder *dec)
{
    h264_state *s = (h264_state *)dec->priv;
    ivd_ctl_flush_ip_t flush_in;
    ivd_ctl_flush_op_t flush_out;
    ih264d_video_decode_op_t out;
    IV_API_CALL_STATUS_T ret;
    mr_status emit_status;
    if (!s) return MR_EAGAIN;
    if (release_held_display_buffer(s) != IV_SUCCESS) return MR_ERR;
    if (s->flush_done) return MR_EAGAIN;
    s->output_pts_valid = 0;
    if (!s->flushing) {
        memset(&flush_in, 0, sizeof flush_in);
        memset(&flush_out, 0, sizeof flush_out);
        flush_in.u4_size = sizeof flush_in;
        flush_in.e_cmd = IVD_CMD_VIDEO_CTL;
        flush_in.e_sub_cmd = IVD_CMD_CTL_FLUSH;
        flush_out.u4_size = sizeof flush_out;
        ret = ih264d_api_function(s->handle, &flush_in, &flush_out);
        if (ret != IV_SUCCESS) {
            s->flush_done = 1;
            return MR_EAGAIN;
        }
        s->flushing = 1;
    }
    /* Zero-length flush poke: libavc reads nothing from this pointer at
     * num_Bytes=0, but s->packet can now be NULL for the life of a session
     * that decoded only Annex-B-native input (mr_h264_set_input_annexb(),
     * e.g. pure MPEG-TS) and so never allocated it via
     * avcc_sample_to_annexb() - pass a guaranteed non-NULL empty buffer
     * instead of relying on that being safe. */
    ret = decode_annexb(s, s->timestamp,
                        s->packet ? s->packet : (const uint8_t *)"",
                        0, &out);
    if (out.s_ivd_video_decode_op_t.u4_output_present) {
        h264_remember_output_pts(s, out.s_ivd_video_decode_op_t.u4_ts);
        s->held_disp_id = out.s_ivd_video_decode_op_t.u4_disp_buf_id;
        s->held_disp_valid = 1;
        emit_status = emit_rgb(dec, &out.s_ivd_video_decode_op_t);
        if (!s->yuv_output || emit_status != MR_OK) {
            if (release_held_display_buffer(s) != IV_SUCCESS &&
                emit_status == MR_OK)
                emit_status = MR_ERR;
        }
        return emit_status;
    }
    s->flush_done = 1;
    return MR_EAGAIN;
}

static void h264_close(mr_decoder *dec)
{
    h264_state *s = dec ? (h264_state *)dec->priv : NULL;
    uint32_t i;
    if (!s) return;
    if (s->handle) {
        ih264d_delete_ip_t in;
        ih264d_delete_op_t out;
        release_held_display_buffer(s);
        memset(&in, 0, sizeof in);
        memset(&out, 0, sizeof out);
        in.s_ivd_delete_ip_t.u4_size = sizeof in;
        in.s_ivd_delete_ip_t.e_cmd = IVD_CMD_DELETE;
        out.s_ivd_delete_op_t.u4_size = sizeof out;
        ih264d_api_function(s->handle, &in, &out);
    }
    for (i = 0; i < 3; i++) free(s->out[i]);
    for (i = 0; i < s->disp_count; i++)
        h264_aligned_free(s, s->disp[i]);
    free(s->packet);
    free(s->rgb);
    free(s);
    dec->priv = NULL;
    dec->frame.data = NULL;
}

const mr_codec mr_codec_h264 = {
    "H.264/AVC (libavc)",
    {
        MR_FOURCC('a','v','c','1'),
        0, 0, 0, 0, 0, 0, 0
    },
    h264_open,
    h264_decode,
    h264_close,
    h264_flush
};
