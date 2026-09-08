/*
 * MintVID - MPEG-1/MPEG-2 video decoder adapter.
 *
 * libmpeg2 supplies the Main Profile bitstream decoder, reference pictures and
 * display reordering. The adapter consumes one elementary-stream chunk per
 * call and converts displayed YUV420 frames to RGB24.
 */
#include "mr_mpeg2.h"
#include "mr_yuv.h"

#include "mpeg2.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct mpeg2_frame_node {
    uint8_t *rgb;
    struct mpeg2_frame_node *next;
} mpeg2_frame_node;

typedef struct {
    mpeg2dec_t        *decoder;
    const mpeg2_info_t *info;
    size_t             frame_bytes;
    mpeg2_frame_node  *pending_head;
    mpeg2_frame_node  *pending_tail;
    mpeg2_frame_node  *current;
    mpeg2_frame_node  *free_nodes;
    int                flushing;
    int                flush_done;
} mpeg2_state;

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
                       width, height, NULL, NULL);

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
    st = emit_rgb(dec, node->rgb);
    if (st != MR_OK) {
        node->next = s->free_nodes;
        s->free_nodes = node;
        return st;
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
    return MR_OK;
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
