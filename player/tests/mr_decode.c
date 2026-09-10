/*
 * Host test harness: demux an AVI, decode its video with the MintVID core,
 * and (optionally) validate every frame against a directory of reference PPMs
 * produced by ffmpeg. This is how the portable decoders are proven correct
 * before any of this touches a 68k toolchain.
 *
 *   mr_decode <file.avi>                    - print stream info + frame count
 *   mr_decode <file.avi> --ppm <outdir>     - write decoded frames as PPM
 *   mr_decode <file.avi> --check <refdir>    - compare vs refdir/fNNN.ppm
 *   mr_decode <file.mov> --first-ppm <file>   - dump first decoded RGB frame
 *   mr_decode <file> --time [mode ...]        - print the H.264 stage-time
 *       breakdown (input/core/output + mc/deblock/recon/intra, see
 *       core/mr_h264.h's mr_h264_timing) alongside whatever <mode> would
 *       otherwise print. No-op for a non-H.264 stream. The mc/deblock/
 *       recon/intra breakdown only has real numbers when built with
 *       -DMR_H264_STAGE_PROFILE=1 (see vendor/libavc_port/
 *       ih264d_stage_profile.c) - this is the host-buildable equivalent of
 *       mrplay.c's own --time flag, for structural profiling without an
 *       AmigaOS toolchain. clock()-based, so on qemu-m68k this measures
 *       relative instruction-execution volume per stage, not real hardware
 *       timing - see CLAUDE.md's qemu-vs-hardware note before drawing any
 *       conclusion from it beyond "which stage is structurally biggest".
 */
#include "../core/mr_demux.h"
#include "../core/mr_http.h"
#include "../core/mr_codec.h"
#include "../core/mr_dither.h"
#include "../core/mr_ham.h"
#include "../core/mr_h264.h"
#include "../amiga/mintvid_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

MINTVID_DECLARE_VERSION(mr_decode_version_tag, "mr_decode");

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (len) *len = (size_t)n;
    return b;
}

static void write_ppm(const char *path, const mr_frame *fr)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", fr->width, fr->height);
    int y;
    for (y = 0; y < fr->height; y++)
        fwrite(fr->data + (size_t)y * fr->stride, 1, (size_t)fr->width * 3, f);
    fclose(f);
}

/* Mean absolute error vs a reference PPM, returned as MAE*1000 (fixed point,
 * integer-only so no soft-float is pulled into any build); -1 if the reference
 * is missing or its geometry does not match. maxerr gets the largest single
 * channel absolute difference. */
static long check_ppm(const char *path, const mr_frame *fr, int *maxerr)
{
    size_t len;
    uint8_t *b = slurp(path, &len);
    if (!b) return -1;
    /* parse minimal P6 header */
    int w = 0, h = 0, mx = 0;
    const char *p = (const char *)b;
    if (sscanf(p, "P6 %d %d %d", &w, &h, &mx) != 3) { free(b); return -1; }
    /* advance past header: three whitespace-separated ints after "P6" then 1 ws */
    int fields = 0; size_t i = 2;
    while (i < len && fields < 3) {
        while (i < len && (b[i]==' '||b[i]=='\n'||b[i]=='\t'||b[i]=='\r')) i++;
        while (i < len && b[i] >= '0' && b[i] <= '9') i++;
        fields++;
    }
    i++; /* single whitespace after maxval */
    if (w != fr->width || h != fr->height) { free(b); return -1; }
    uint64_t sum = 0; int mxe = 0; size_t n = (size_t)w * h * 3;
    size_t k;
    for (k = 0; k < n; k++) {
        int row = (int)(k / (w * 3));
        int col = (int)(k % (w * 3));
        int dv = (int)fr->data[(size_t)row * fr->stride + col] - (int)b[i + k];
        if (dv < 0) dv = -dv;
        sum += (uint64_t)dv;
        if (dv > mxe) mxe = dv;
    }
    free(b);
    if (maxerr) *maxerr = mxe;
    return (long)((sum * 1000) / n);
}


int main(int argc, char **argv)
{
    int argi = 2, force_memory = 0, hls_buffer_segments = 0;
    int h264_speed = -1, h264_yuv = 0, do_time = 0;
    const char *user_agent = NULL, *referer = NULL;
    mr_http_options http_options;
    const char *mode;
    const char *dir;
    if (argc < 2) {
        fprintf(stderr, "usage: mr_decode <file> [--memory] "
                        "[--ppm dir|--check dir|--first-ppm file]\n");
        return 2;
    }
    while (argc > argi) {
        if (!strcmp(argv[argi], "--memory")) {
            force_memory = 1;
            argi++;
        } else if (!strcmp(argv[argi], "--hls-buffer-segments")) {
            hls_buffer_segments = 1;
            argi++;
        } else if (!strcmp(argv[argi], "--user-agent") && argc > argi + 1) {
            user_agent = argv[argi + 1];
            argi += 2;
        } else if (!strcmp(argv[argi], "--referer") && argc > argi + 1) {
            referer = argv[argi + 1];
            argi += 2;
        } else if (!strncmp(argv[argi], "--h264-speed=", 13)) {
            const char *speed = argv[argi] + 13;
            h264_speed = !strcmp(speed, "quality") ? MR_H264_SPEED_QUALITY :
                         !strcmp(speed, "balanced") ? MR_H264_SPEED_BALANCED :
                         !strcmp(speed, "fast") ? MR_H264_SPEED_FAST :
                         !strcmp(speed, "turbo") ? MR_H264_SPEED_TURBO :
                         (!strcmp(speed, "turbo+") || !strcmp(speed, "turbo-plus"))
                             ? MR_H264_SPEED_TURBO_PLUS :
                         (!strcmp(speed, "turbogt") || !strcmp(speed, "turbo-gt"))
                             ? MR_H264_SPEED_TURBO_GT : -2;
            if (h264_speed < 0) {
                fprintf(stderr, "invalid H.264 speed mode\n");
                return 2;
            }
            argi++;
        } else if (!strcmp(argv[argi], "--h264-yuv")) {
            h264_yuv = 1;
            argi++;
        } else if (!strcmp(argv[argi], "--time")) {
            do_time = 1;
            argi++;
        } else {
            break;
        }
    }
    mode = argc > argi ? argv[argi] : NULL;
    dir  = argc > argi + 1 ? argv[argi + 1] : NULL;

    size_t len = 0;
    uint8_t *buf = NULL;
    if (!mr_http_options_init(&http_options, user_agent, referer)) {
        fprintf(stderr, "invalid HTTP options: %s\n", mr_source_last_error());
        return 2;
    }
    http_options.hls_buffer_segments = hls_buffer_segments;
    mr_demux *dx = force_memory ? NULL :
        mr_demux_open_file_ex(argv[1], (user_agent || referer ||
                                        hls_buffer_segments)
                                      ? &http_options : NULL);

    if (!dx) {
        if (!force_memory && mr_demux_is_file_backed_container(argv[1])) {
            fprintf(stderr, "cannot open stream: %s\n",
                    mr_demux_last_open_error());
            return 2;
        }
        buf = slurp(argv[1], &len);
        if (!buf) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

        dx = mr_demux_open(buf, len);
        if (!dx) {
            fprintf(stderr, "not a supported container "
                            "(need AVI, MOV/MP4, MKV, MPEG-TS/PS or raw video)\n");
            free(buf);
            return 2;
        }
    }
    const mr_video_info *vi = mr_demux_video(dx);
    const mr_audio_info *ai = mr_demux_audio(dx);
    char video_codec[96], audio_codec[96];
    uint32_t fc = vi->fourcc;
    printf("container: %s\n", mr_demux_container_name(dx));
    mr_demux_describe_video_codec(dx, video_codec, sizeof video_codec);
    mr_demux_describe_audio_codec(dx, audio_codec, sizeof audio_codec);
    printf("codecs: video=%s, audio=%s\n", video_codec, audio_codec);
    /* uint32_t is 'unsigned long' on m68k-amigaos but 'unsigned int' on the
     * host, so cast explicitly to keep the formats portable and warning-clean:
     * %lu + unsigned long, and %c fourcc bytes promoted to int. */
    printf("video: %dx%d fourcc='%c%c%c%c' rate=%lu/%lu (~%lu fps)\n",
           vi->width, vi->height,
           (int)(fc & 0xff), (int)((fc >> 8) & 0xff),
           (int)((fc >> 16) & 0xff), (int)((fc >> 24) & 0xff),
           (unsigned long)vi->rate, (unsigned long)vi->scale,
           (unsigned long)(vi->scale ? vi->rate / vi->scale : 0u));
    if (ai->valid && ai->format_tag == MR_AUDIO_FORMAT_PCM) {
        printf("audio: PCM %c%u%s%s %lu Hz",
               ai->pcm_signed ? 'S' : 'U', (unsigned)ai->bits_per_sample,
               ai->bits_per_sample > 8
                   ? (ai->pcm_big_endian ? "BE" : "LE") : "",
               ai->channels == 1 ? " mono" : ai->channels == 2 ? " stereo" : "",
               (unsigned long)ai->sample_rate);
        if (ai->codec_tag > 0xffff)
            printf(" (%c%c%c%c)", (int)(ai->codec_tag & 255),
                   (int)((ai->codec_tag >> 8) & 255),
                   (int)((ai->codec_tag >> 16) & 255),
                   (int)((ai->codec_tag >> 24) & 255));
        putchar('\n');
    } else if (ai->valid)
        printf("audio: tag=0x%04x %lu Hz %u ch %u-bit\n",
               (unsigned)ai->format_tag, (unsigned long)ai->sample_rate,
               (unsigned)ai->channels, (unsigned)ai->bits_per_sample);

    const mr_codec *codec = mr_codec_find(fc);
    if (!codec) { printf("no decoder for this fourcc\n");
                  mr_demux_close(dx); free(buf); return 1; }

    mr_decoder dec;
    if (mr_decoder_open_config(&dec, codec, vi->width, vi->height,
                               vi->config, vi->config_len) != MR_OK) {
        fprintf(stderr, "decoder open failed\n");
        mr_demux_close(dx); free(buf); return 1;
    }
#ifdef MR_HAVE_H264
    if (h264_yuv && codec == &mr_codec_h264)
        mr_h264_set_yuv_output(&dec, 1);
    if (h264_speed >= 0 && codec == &mr_codec_h264 &&
        !mr_h264_set_speed_mode(&dec, (mr_h264_speed_mode)h264_speed)) {
        fprintf(stderr, "H.264 speed mode rejected\n");
        mr_decoder_close(&dec); mr_demux_close(dx); free(buf); return 1;
    }
    if (do_time && codec == &mr_codec_h264)
        mr_h264_set_timing_enabled(&dec, 1);
#endif
    unsigned long t_input_us = 0, t_core_us = 0, t_output_us = 0;
    unsigned long t_mc_us = 0, t_deblock_us = 0, t_recon_us = 0, t_intra_us = 0;
    unsigned long t_input_max = 0, t_core_max = 0, t_output_max = 0;
    unsigned long t_mc_max = 0, t_deblock_max = 0, t_recon_max = 0, t_intra_max = 0;

    int frame = 0, bad = 0;
    long worst_mae = 0;   /* MAE * 1000 */
    int  checked = 0;     /* frames actually compared against a reference */
    unsigned long audio_bytes = 0, audio_pkts = 0;

    /* --dirty: verify the decoder's reported changed-row span actually covers
     * every row that differs from the previous frame (safety of dirty-row
     * rendering). */
    int do_dirty = (mode && !strcmp(mode, "--dirty"));
    uint8_t *prev = NULL; long dirty_viol = 0; unsigned long dirty_rows = 0, tot_rows = 0;
    if (do_dirty) prev = calloc((size_t)vi->width * vi->height * 3, 1);

    /* --dither: exercise the AGA 8-bit path on the host - dither each frame to
     * palette indices, map back to RGB, write it out, and report the round-trip
     * MAE (dithering trades per-pixel error for no banding, so this is bounded,
     * not near-zero). */
    int do_dither = (mode && !strcmp(mode, "--dither") && dir);
    int do_ham    = (mode && !strcmp(mode, "--ham") && dir);
    uint8_t *d_idx = NULL, *d_rgb = NULL, d_pal[256 * 3];
    long dither_worst = 0, ham_worst = 0;
    if (do_dither || do_ham) {
        d_idx = malloc((size_t)vi->width * vi->height);
        d_rgb = malloc((size_t)vi->width * vi->height * 3);
        mr_dither_palette(d_pal);
        if (!d_idx || !d_rgb) { fprintf(stderr, "oom\n"); return 1; }
    }

    mr_packet pkt;
    while (mr_demux_next_packet(dx, &pkt) == MR_OK) {
        if (!pkt.is_video) { audio_bytes += pkt.len; audio_pkts++; continue; }
        if (pkt.len == 0) continue;
        {
            mr_status ds;
            /* No-op for any non-H.264 decoder (see the guard in
             * mr_h264_set_input_annexb() itself) - MPEG-TS hands over
             * Annex-B-native H.264 packets (pkt.is_annexb) since the
             * decoder now decodes straight from them instead of always
             * converting through AVCC first. */
            mr_h264_set_input_annexb(&dec, pkt.is_annexb);
            ds = mr_decoder_decode(&dec, pkt.data, pkt.len);
            if (ds == MR_EAGAIN || ds == MR_SKIPPED)
                continue;                    /* reorder delay / Turbo drop    */
            if (ds != MR_OK) {
                fprintf(stderr, "decode error at frame %d\n", frame); break;
            }
#ifdef MR_HAVE_H264
            if (do_time) {
                mr_h264_timing t;
                mr_h264_frame_timing(&dec, &t);
                t_input_us += t.input_us; t_core_us += t.core_us;
                t_output_us += t.output_us; t_mc_us += t.mc_us;
                t_deblock_us += t.deblock_us; t_recon_us += t.recon_us;
                t_intra_us += t.intra_us;
                if (t.input_us > t_input_max) t_input_max = t.input_us;
                if (t.core_us > t_core_max) t_core_max = t.core_us;
                if (t.output_us > t_output_max) t_output_max = t.output_us;
                if (t.mc_us > t_mc_max) t_mc_max = t.mc_us;
                if (t.deblock_us > t_deblock_max) t_deblock_max = t.deblock_us;
                if (t.recon_us > t_recon_max) t_recon_max = t.recon_us;
                if (t.intra_us > t_intra_max) t_intra_max = t.intra_us;
            }
#endif
        }
decoded_output:
        ;
        frame++;
        if (mode && !strcmp(mode, "--first-ppm") && dir) {
            write_ppm(dir, &dec.frame);
            break;
        }
        char path[512];
        if (do_dirty) {
            int w = dec.frame.width, h = dec.frame.height, y, changed_lo = h, changed_hi = 0;
            for (y = 0; y < h; y++) {
                const uint8_t *cur = dec.frame.data + (size_t)y * dec.frame.stride;
                const uint8_t *pv  = prev + (size_t)y * w * 3;
                if (memcmp(cur, pv, (size_t)w * 3) != 0) {
                    if (y < changed_lo) changed_lo = y;
                    if (y + 1 > changed_hi) changed_hi = y + 1;
                }
                memcpy((void *)pv, cur, (size_t)w * 3);
            }
            tot_rows += h;
            if (changed_hi > changed_lo) {
                dirty_rows += (unsigned long)(dec.frame.dirty_y1 - dec.frame.dirty_y0);
                /* every actually-changed row must fall inside the reported span */
                if (changed_lo < dec.frame.dirty_y0 || changed_hi > dec.frame.dirty_y1)
                    dirty_viol++;
            }
            goto drain_decoded_output;
        }
        if (mode && !strcmp(mode, "--ppm") && dir) {
            snprintf(path, sizeof path, "%s/f%03d.ppm", dir, frame);
            write_ppm(path, &dec.frame);
        } else if (mode && !strcmp(mode, "--check") && dir) {
            snprintf(path, sizeof path, "%s/f%03d.ppm", dir, frame);
            int mxe = 0; long mae = check_ppm(path, &dec.frame, &mxe);
            if (mae < 0) { printf("  frame %3d: no reference\n", frame); }
            else {
                checked++;
                if (mae > worst_mae) worst_mae = mae;
                if (mae > 6000) { bad++;
                    printf("  frame %3d: MAE=%ld.%03ld maxerr=%d  <-- high\n",
                           frame, mae / 1000, mae % 1000, mxe); }
            }
        } else if (do_dither) {
            int w = dec.frame.width, h = dec.frame.height, i;
            long sum = 0;
            mr_dither_rgb8(dec.frame.data, w, h, dec.frame.stride, d_idx, w, 0);
            for (i = 0; i < w * h; i++) {
                const uint8_t *pe = &d_pal[d_idx[i] * 3];
                d_rgb[i * 3 + 0] = pe[0];
                d_rgb[i * 3 + 1] = pe[1];
                d_rgb[i * 3 + 2] = pe[2];
            }
            for (i = 0; i < w * h * 3; i++) {
                int row = i / (w * 3), col = i % (w * 3);
                int dv = (int)dec.frame.data[(size_t)row * dec.frame.stride + col]
                       - (int)d_rgb[i];
                sum += dv < 0 ? -dv : dv;
            }
            { long mae = (sum * 1000) / (w * h * 3);
              if (mae > dither_worst) dither_worst = mae; }
            { mr_frame fr; fr.width = w; fr.height = h; fr.fmt = MR_PIX_RGB24;
              fr.stride = w * 3; fr.data = d_rgb;
              snprintf(path, sizeof path, "%s/f%03d.ppm", dir, frame);
              write_ppm(path, &fr); }
        } else if (do_ham) {
            int w = dec.frame.width, h = dec.frame.height, i;
            uint8_t hpal[64 * 3]; long sum = 0;
            mr_ham_palette(hpal, 8);
            mr_ham_encode(dec.frame.data, w, h, dec.frame.stride, d_idx, w, 8);
            mr_ham_decode(d_idx, w, h, w, hpal, d_rgb, w * 3, 8);
            for (i = 0; i < w * h * 3; i++) {
                int row = i / (w * 3), col = i % (w * 3);
                int dv = (int)dec.frame.data[(size_t)row * dec.frame.stride + col]
                       - (int)d_rgb[i];
                sum += dv < 0 ? -dv : dv;
            }
            { long mae = (sum * 1000) / (w * h * 3);
              if (mae > ham_worst) ham_worst = mae; }
            { mr_frame fr; fr.width = w; fr.height = h; fr.fmt = MR_PIX_RGB24;
              fr.stride = w * 3; fr.data = d_rgb;
              snprintf(path, sizeof path, "%s/f%03d.ppm", dir, frame);
              write_ppm(path, &fr); }
        }
drain_decoded_output:
        if (mr_decoder_drain(&dec) == MR_OK)
            goto decoded_output;
    }
    /* Drain any reordered frames held by the decoder (MPEG-4 B-VOPs). */
    while (mr_decoder_flush(&dec) == MR_OK) {
        char path[512];
        frame++;
        if (mode && !strcmp(mode, "--ppm") && dir) {
            snprintf(path, sizeof path, "%s/f%03d.ppm", dir, frame);
            write_ppm(path, &dec.frame);
        } else if (mode && !strcmp(mode, "--check") && dir) {
            snprintf(path, sizeof path, "%s/f%03d.ppm", dir, frame);
            int mxe = 0; long mae = check_ppm(path, &dec.frame, &mxe);
            if (mae < 0) printf("  frame %3d: no reference\n", frame);
            else { checked++;
                   if (mae > worst_mae) worst_mae = mae;
                   if (mae > 6000) { bad++;
                       printf("  frame %3d: MAE=%ld.%03ld maxerr=%d  <-- high\n",
                              frame, mae / 1000, mae % 1000, mxe); } }
        }
    }
    printf("decoded %d frames\n", frame);
#ifdef MR_HAVE_H264
    if (do_time && frame) {
        /* mc/deblock/recon/intra are timed as many small clock() calls (one
         * per MB-level primitive) and core as one larger span per NAL - under
         * qemu's emulated clock, quantization noise on the many small calls
         * can make their sum exceed the enclosing span it should be part of.
         * Clamp rather than let an unsigned subtraction wrap into a huge
         * number; this is a measurement-resolution artifact of the profiling
         * environment; see CLAUDE.md's qemu-timing note. */
        unsigned long stage_sum = t_mc_us + t_deblock_us + t_recon_us + t_intra_us;
        unsigned long other_us = t_core_us > stage_sum ? t_core_us - stage_sum : 0;
        printf("h264 stages: input=%lu/%lu us core=%lu/%lu us "
               "output=%lu/%lu us mc=%lu/%lu us deblock=%lu/%lu us "
               "recon=%lu/%lu us intra=%lu/%lu us other=%lu/frame\n",
               t_input_us / (unsigned long)frame, t_input_max,
               t_core_us / (unsigned long)frame, t_core_max,
               t_output_us / (unsigned long)frame, t_output_max,
               t_mc_us / (unsigned long)frame, t_mc_max,
               t_deblock_us / (unsigned long)frame, t_deblock_max,
               t_recon_us / (unsigned long)frame, t_recon_max,
               t_intra_us / (unsigned long)frame, t_intra_max,
               other_us / (unsigned long)frame);
    }
#endif
    if (do_dirty) {
        printf("dirty rows: %lu / %lu total (%lu%%), coverage violations=%ld\n",
               dirty_rows, tot_rows,
               tot_rows ? 100UL * dirty_rows / tot_rows : 0UL, dirty_viol);
        free(prev);
        mr_decoder_close(&dec); mr_demux_close(dx); free(buf);
        return dirty_viol ? 1 : 0;
    }
    if (do_dither) {
        printf("dither round-trip worst MAE=%ld.%03ld/255\n",
               dither_worst / 1000, dither_worst % 1000);
        free(d_idx); free(d_rgb);
    }
    if (do_ham) {
        printf("HAM8 round-trip worst MAE=%ld.%03ld/255\n",
               ham_worst / 1000, ham_worst % 1000);
        free(d_idx); free(d_rgb);
    }
    if (audio_pkts)
        printf("audio: %lu packets, %lu bytes\n", audio_pkts, audio_bytes);
    if (mode && !strcmp(mode, "--check")) {
        printf("worst per-frame MAE=%ld.%03ld, frames checked=%d, "
               "frames over threshold=%d\n",
               worst_mae / 1000, worst_mae % 1000, checked, bad);
        /* Comparing nothing is a failure, not a pass: a decoder that gives up
         * on its first frame would otherwise report "0 over threshold" and
         * exit 0, which is how a completely broken decoder stayed green. */
        if (!checked)
            printf("  no frame was compared against a reference\n");
        mr_decoder_close(&dec); mr_demux_close(dx); free(buf);
        return (bad || !checked) ? 1 : 0;
    }
    mr_decoder_close(&dec); mr_demux_close(dx); free(buf);
    return 0;
}
