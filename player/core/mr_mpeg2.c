/*
 * MintVID - MPEG-1/MPEG-2 video decoder adapter.
 *
 * libmpeg2 supplies the Main Profile bitstream decoder, reference pictures and
 * display reordering. The adapter consumes one elementary-stream chunk per
 * call and converts displayed YUV420 frames to RGB24.
 */
#include "mr_mpeg2.h"
#include "mr_yuv.h"

#include <string.h>

#include "mpeg2.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct mpeg2_frame_node {
    uint8_t *rgb;                /* RGB24, or packed YUV420P under yuv_output */
    int has_pts;                 /* display-picture tag is valid */
    uint64_t pts_us;             /* PTS belonging to this displayed picture */
    struct mpeg2_frame_node *next;
} mpeg2_frame_node;

typedef struct {
    mpeg2dec_t        *decoder;
    const mpeg2_info_t *info;
    size_t             frame_bytes;
    /* Packed YUV420P geometry, valid while yuv_output is set. Y occupies
     * y_bytes at the front of the node, then Cb, then Cr. */
    int                yuv_output;
    int                uv_width, uv_height;
    size_t             y_bytes, uv_bytes;
    mpeg2_frame_node  *pending_head;
    mpeg2_frame_node  *pending_tail;
    mpeg2_frame_node  *current;
    mpeg2_frame_node  *free_nodes;
    int                flushing;
    int                flush_done;
    int                have_last_output_pts;
    uint64_t           last_output_pts;
    mr_mpeg2_service_fn service;
    void               *service_opaque;
} mpeg2_state;

static mr_status mpeg2_drain_decoder(mr_decoder *dec);

static mr_status emit_rgb(mr_decoder *dec, uint8_t *rgb)
{
    mpeg2_state *s = (mpeg2_state *)dec->priv;
    const mpeg2_sequence_t *seq = s->info->sequence;
    uint8_t *const *planes = s->info->display_fbuf->buf;
    int width = dec->width, height = dec->height;
    int y_stride, uv_stride;

    if (!seq || !planes[0] || !planes[1] || !planes[2] || !rgb)
        return MR_EFORMAT;
    if ((int)seq->picture_width < width) width = (int)seq->picture_width;
    if ((int)seq->picture_height < height) height = (int)seq->picture_height;
    y_stride = (int)seq->width;
    uv_stride = (int)seq->chroma_width;

    mr_yuv420_to_rgb24(rgb, dec->width * 3,
                       planes[0], y_stride,
                       planes[1], uv_stride,
                       planes[2], uv_stride,
                       width, height, s->service, s->service_opaque);

    dec->frame.dirty_y0 = 0;
    dec->frame.dirty_y1 = dec->height;
    return MR_OK;
}

/*
 * The same displayed picture, copied out as packed YUV420P instead of being
 * converted to RGB24.
 *
 * The copy cannot be skipped: libmpeg2 cycles its own framebuffers between
 * reference and display use, so a node that outlives the next mpeg2_parse()
 * must own its pixels. But a plane copy moves 1.5 bytes per pixel with no
 * arithmetic, where emit_rgb() writes 3 bytes per pixel through the colour
 * transform - and it lets the caller dither YUV straight to palette indices
 * (mr_yuv_dither.h) rather than paying for RGB24 on the way out and a second
 * pass to get back down again.
 *
 * Rows are packed to the visible width, not libmpeg2's aligned pitch, so the
 * node stays as small as the picture and the strides handed to the caller are
 * simply width and uv_width.
 */
static mr_status emit_yuv(mr_decoder *dec, uint8_t *dst)
{
    mpeg2_state *s = (mpeg2_state *)dec->priv;
    const mpeg2_sequence_t *seq = s->info->sequence;
    uint8_t *const *planes = s->info->display_fbuf->buf;
    int width = dec->width, height = dec->height;
    int y_stride, uv_stride, uv_w, uv_h, row;
    uint8_t *dst_u, *dst_v;

    if (!seq || !planes[0] || !planes[1] || !planes[2] || !dst)
        return MR_EFORMAT;
    if ((int)seq->picture_width < width) width = (int)seq->picture_width;
    if ((int)seq->picture_height < height) height = (int)seq->picture_height;
    y_stride = (int)seq->width;
    uv_stride = (int)seq->chroma_width;
    uv_w = (width + 1) / 2;
    uv_h = (height + 1) / 2;
    if (uv_w > s->uv_width) uv_w = s->uv_width;
    if (uv_h > s->uv_height) uv_h = s->uv_height;

    dst_u = dst + s->y_bytes;
    dst_v = dst_u + s->uv_bytes;
    for (row = 0; row < height; row++)
        memcpy(dst + (size_t)row * dec->width,
               planes[0] + (size_t)row * y_stride, (size_t)width);
    for (row = 0; row < uv_h; row++) {
        memcpy(dst_u + (size_t)row * s->uv_width,
               planes[1] + (size_t)row * uv_stride, (size_t)uv_w);
        memcpy(dst_v + (size_t)row * s->uv_width,
               planes[2] + (size_t)row * uv_stride, (size_t)uv_w);
    }
    dec->frame.dirty_y0 = 0;
    dec->frame.dirty_y1 = dec->height;
    return MR_OK;
}

static mpeg2_frame_node *alloc_frame(mpeg2_state *s)
{
    mpeg2_frame_node *node = s->free_nodes;
    if (node) {
        s->free_nodes = node->next;
        node->next = NULL;
        return node;
    }
    node = (mpeg2_frame_node *)calloc(1, sizeof *node);
    if (!node) return NULL;
    node->rgb = (uint8_t *)malloc(s->frame_bytes);
    if (!node->rgb) {
        free(node);
        return NULL;
    }
    return node;
}

static mr_status queue_display_frame(mr_decoder *dec)
{
    mpeg2_state *s = (mpeg2_state *)dec->priv;
    mpeg2_frame_node *node = alloc_frame(s);
    mr_status st;
    if (!node) return MR_ENOMEM;
    node->has_pts = 0;
    node->pts_us = 0;
    st = s->yuv_output ? emit_yuv(dec, node->rgb)
                       : emit_rgb(dec, node->rgb);
    if (st != MR_OK) {
        node->next = s->free_nodes;
        s->free_nodes = node;
        return st;
    }
    /* mpeg2_tag_picture() follows the compressed picture through
     * libmpeg2's I/P/B reordering. Capture the tag from the DISPLAY
     * picture, not from whichever PES packet mrplay is draining. */
    if (s->info->display_picture &&
        (s->info->display_picture->flags & PIC_FLAG_TAGS)) {
        node->has_pts = 1;
        node->pts_us = ((uint64_t)s->info->display_picture->tag << 32) |
                       (uint64_t)s->info->display_picture->tag2;
    }
    if (s->pending_tail) s->pending_tail->next = node;
    else s->pending_head = node;
    s->pending_tail = node;
    return MR_OK;
}

static mr_status pop_display_frame(mr_decoder *dec)
{
    mpeg2_state *s = (mpeg2_state *)dec->priv;
    mpeg2_frame_node *node;
    if (s->current) {
        s->current->next = s->free_nodes;
        s->free_nodes = s->current;
        s->current = NULL;
    }
    node = s->pending_head;
    if (!node) return MR_EAGAIN;
    s->pending_head = node->next;
    if (!s->pending_head) s->pending_tail = NULL;
    node->next = NULL;
    s->current = node;
    dec->frame.data = node->rgb;
    if (s->yuv_output) {
        dec->frame.u_data = node->rgb + s->y_bytes;
        dec->frame.v_data = node->rgb + s->y_bytes + s->uv_bytes;
    }
    dec->frame.dirty_y0 = 0;
    dec->frame.dirty_y1 = dec->height;
    return MR_OK;
}

static mr_status pump(mr_decoder *dec, uint8_t *data, uint32_t len)
{
    mpeg2_state *s = (mpeg2_state *)dec->priv;
    mr_status result = MR_EAGAIN;
    unsigned guard = 0;

    mpeg2_buffer(s->decoder, data, data + len);
    while (guard++ < 100000u) {
        mpeg2_state_t state = mpeg2_parse(s->decoder);
        if (state == STATE_BUFFER)
            break;
        if (state == STATE_INVALID)
            return MR_EFORMAT;
        /* INVALID_END still carries libmpeg2's final display picture when a
         * transport stream ends without an explicit sequence-end code. */
        if ((state == STATE_SLICE || state == STATE_END ||
            state == STATE_INVALID_END) &&
            s->info->display_fbuf) {
            result = queue_display_frame(dec);
            if (result != MR_OK) return result;
            /* queue_display_frame() just ran a full RGB/YUV conversion, and
             * a single packet can hand this loop several complete pictures
             * in a row (see mr_mpeg2_set_service()'s declaration) - service
             * after each one, not just inside the conversion itself, so a
             * run of small pictures with cheap individual conversions still
             * yields regularly. */
            if (s->service) s->service(s->service_opaque);
        }
    }
    return guard >= 100000u ? MR_EFORMAT : result;
}

static void free_frame_list(mpeg2_frame_node *node)
{
    while (node) {
        mpeg2_frame_node *next = node->next;
        free(node->rgb);
        free(node);
        node = next;
    }
}

static void mpeg2_close_decoder(mr_decoder *dec)
{
    mpeg2_state *s = dec ? (mpeg2_state *)dec->priv : NULL;
    if (!s) return;
    if (s->decoder) mpeg2_close(s->decoder);
    free_frame_list(s->pending_head);
    free_frame_list(s->current);
    free_frame_list(s->free_nodes);
    free(s);
    dec->priv = NULL;
    dec->frame.data = NULL;
}

static mr_status mpeg2_open_decoder(mr_decoder *dec)
{
    mpeg2_state *s;
    size_t pixels;
    if ((size_t)dec->width > SIZE_MAX / (size_t)dec->height)
        return MR_ENOMEM;
    pixels = (size_t)dec->width * (size_t)dec->height;
    if (pixels > SIZE_MAX / 3u) return MR_ENOMEM;

    s = (mpeg2_state *)calloc(1, sizeof *s);
    if (!s) return MR_ENOMEM;
    dec->priv = s;
    s->decoder = mpeg2_init();
    if (!s->decoder) {
        mpeg2_close_decoder(dec);
        return MR_ENOMEM;
    }
    s->info = mpeg2_info(s->decoder);
    s->frame_bytes = pixels * 3u;

    dec->frame.width = dec->width;
    dec->frame.height = dec->height;
    dec->frame.fmt = MR_PIX_RGB24;
    dec->frame.stride = dec->width * 3;
    dec->frame.data = NULL;
    dec->frame.dirty_y0 = 0;
    dec->frame.dirty_y1 = 0;
    dec->drain = mpeg2_drain_decoder;
    return MR_OK;
}

/* See mr_mpeg2_service_fn's declaration in mr_mpeg2.h. Mirrors
 * mr_h264_set_service(); a no-op on a decoder that is not this codec, so
 * callers need not test first, same as every other mr_mpeg2_set_*(). */
void mr_mpeg2_set_service(mr_decoder *dec, mr_mpeg2_service_fn fn,
                          void *opaque)
{
    mpeg2_state *s;
    if (!dec || dec->codec != &mr_codec_mpeg2) return;
    s = (mpeg2_state *)dec->priv;
    if (!s) return;
    s->service = fn;
    s->service_opaque = opaque;
}

/*
 * Hand back YUV420P planes instead of RGB24, mirroring
 * mr_h264_set_yuv_output(). The caller opts in when it is going to dither to
 * palette indices anyway - the display_supports_yuv_indexed() route - so that
 * the picture never becomes RGB24 at all.
 *
 * Set it once, straight after open (or after mr_decoder_reset(), which brings
 * back a default-off state), exactly as the H.264 path does. Toggling it drops
 * anything already queued, because the node buffers change both size and
 * meaning; that is safe at the intended call site, where nothing is queued
 * yet, and honest rather than silently reinterpreting RGB bytes as planes.
 */
void mr_mpeg2_set_yuv_output(mr_decoder *dec, int enabled)
{
    mpeg2_state *s;
    if (!dec || dec->codec != &mr_codec_mpeg2) return;
    s = (mpeg2_state *)dec->priv;
    if (!s || s->yuv_output == (enabled != 0)) return;

    free_frame_list(s->pending_head);
    free_frame_list(s->current);
    free_frame_list(s->free_nodes);
    s->pending_head = s->pending_tail = s->current = s->free_nodes = NULL;

    s->yuv_output = enabled != 0;
    if (s->yuv_output) {
        s->uv_width  = (dec->width + 1) / 2;
        s->uv_height = (dec->height + 1) / 2;
        s->y_bytes   = (size_t)dec->width * (size_t)dec->height;
        s->uv_bytes  = (size_t)s->uv_width * (size_t)s->uv_height;
        s->frame_bytes = s->y_bytes + 2u * s->uv_bytes;
        dec->frame.fmt = MR_PIX_YUV420P;
        dec->frame.stride = dec->width;
        dec->frame.u_stride = s->uv_width;
        dec->frame.v_stride = s->uv_width;
    } else {
        s->frame_bytes = (size_t)dec->width * (size_t)dec->height * 3u;
        dec->frame.fmt = MR_PIX_RGB24;
        dec->frame.stride = dec->width * 3;
    }
    dec->frame.data = NULL;
    dec->frame.dirty_y0 = 0;
    dec->frame.dirty_y1 = 0;
}

/* Tag the next MPEG picture with the container PTS. libmpeg2 carries this
 * through decode reordering and exposes it again on display_picture. */
void mr_mpeg2_set_input_pts(mr_decoder *dec, int has_pts, uint64_t pts_us)
{
    mpeg2_state *s;
    if (!dec || dec->codec != &mr_codec_mpeg2 || !has_pts) return;
    s = (mpeg2_state *)dec->priv;
    if (!s || !s->decoder) return;
    mpeg2_tag_picture(s->decoder, (uint32_t)(pts_us >> 32),
                      (uint32_t)(pts_us & 0xffffffffu));
}

/* Return a usable anchor timestamp for the picture currently exposed in
 * dec->frame. MPEG PES packets can contain more than one picture, so
 * libmpeg2 may legitimately propagate the same packet tag onto consecutive
 * displayed pictures. Such a duplicate is NOT a second frame timestamp.
 * Suppress duplicate/backward tags and let mrplay's existing display-order
 * synthetic clock advance that picture by one frame period instead. The next
 * genuinely newer tag remains an authoritative container anchor. */
int mr_mpeg2_output_pts(mr_decoder *dec, uint64_t *pts_us)
{
    mpeg2_state *s;
    uint64_t cur;
    if (!dec || dec->codec != &mr_codec_mpeg2 || !pts_us) return 0;
    s = (mpeg2_state *)dec->priv;
    if (!s || !s->current || !s->current->has_pts) return 0;
    cur = s->current->pts_us;
    if (s->have_last_output_pts && cur <= s->last_output_pts) return 0;
    s->last_output_pts = cur;
    s->have_last_output_pts = 1;
    *pts_us = cur;
    return 1;
}

static mr_status mpeg2_decode_packet(mr_decoder *dec,
                                     const uint8_t *data, uint32_t len)
{
    mpeg2_state *s = (mpeg2_state *)dec->priv;
    if (!s || !data || !len) return MR_EFORMAT;
    s->flushing = 0;
    s->flush_done = 0;
    {
        mr_status st = pump(dec, (uint8_t *)data, len);
        if (st != MR_OK && st != MR_EAGAIN) return st;
        return pop_display_frame(dec);
    }
}

static mr_status mpeg2_flush_decoder(mr_decoder *dec)
{
    static uint8_t sequence_end[4] = { 0x00, 0x00, 0x01, 0xb7 };
    mpeg2_state *s = (mpeg2_state *)dec->priv;
    mr_status st;
    if (!s || s->flush_done) return MR_EAGAIN;
    st = pop_display_frame(dec);
    if (st == MR_OK) return st;
    if (!s->flushing) {
        s->flushing = 1;
        st = pump(dec, sequence_end, sizeof sequence_end);
        if (st != MR_OK && st != MR_EAGAIN) return st;
        st = pop_display_frame(dec);
        if (st == MR_OK) return st;
    }
    s->flush_done = 1;
    return MR_EAGAIN;
}

static mr_status mpeg2_drain_decoder(mr_decoder *dec)
{
    /* A single libmpeg2 parse pass can complete both a reordered picture and
     * the following display picture.  Hand every already-converted picture to
     * the player now, before the demuxer advances through any intervening
     * audio PES packets. */
    return pop_display_frame(dec);
}

const mr_codec mr_codec_mpeg2 = {
    "MPEG-1/2 Video (libmpeg2)",
    {
        MR_FOURCC('m','p','g','2'),
        MR_FOURCC('M','P','G','2'),
        MR_FOURCC('m','p','g','1'),
        0, 0, 0, 0, 0
    },
    mpeg2_open_decoder,
    mpeg2_decode_packet,
    mpeg2_close_decoder,
    mpeg2_flush_decoder
};
