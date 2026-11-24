/*
 * core/mr_avc.c: avcC and length-prefixed samples to Annex-B, and the SPS
 * fields hardware decoders' limits are about.
 *
 *   mr_avc_check                       synthetic SPS, avcC and sample cases
 *   mr_avc_check <media> <ref.h264|->  a fixture: the SPS gives the
 *       demuxer's picture size and the avcC's profile, and every converted
 *       sample's NAL units match ffmpeg's h264_mp4toannexb output (ref.h264)
 *       in order, parameter sets and access unit delimiters aside (the MOV
 *       demuxer drops in-band parameter sets; ffmpeg adds its own). "-"
 *       skips the comparison (MPEG-TS input is Annex-B already).
 */
#include "../core/mr_avc.h"
#include "../core/mr_mov.h"
#include "../core/mr_mkv.h"
#include "../core/mr_ts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mr_source_read_at(mr_source *source, size_t offset, void *dst, size_t len)
{
    (void)source; (void)offset; (void)dst; (void)len; return 0;
}

static int fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", \
    __FILE__, __LINE__, #c); fail = 1; } } while (0)

/* ---- a bit writer for synthetic SPS ---------------------------------- */

typedef struct { uint8_t b[512]; int bits; } bit_writer;

static void put(bit_writer *w, uint32_t v, int n)
{
    while (n--) {
        if (v >> n & 1) w->b[w->bits >> 3] |= (uint8_t)(0x80 >> (w->bits & 7));
        w->bits++;
    }
}

static void put_ue(bit_writer *w, uint32_t v)
{
    int n = 0;
    while ((v + 1) >> (n + 1)) n++;
    put(w, 0, n);
    put(w, v + 1, n + 1);
}

static void put_se(bit_writer *w, int32_t v)
{
    put_ue(w, v > 0 ? (uint32_t)v * 2 - 1 : (uint32_t)(-v) * 2);
}

/* rbsp_trailing_bits, then emulation prevention into nal[1..]; returns
 * the NAL size and how many prevention bytes went in. */
static uint32_t finish_nal(bit_writer *w, uint8_t nal_type, uint8_t *nal,
                           int *epb)
{
    uint32_t n = 1, i, rbsp;
    int zeros = 0;
    put(w, 1, 1);
    while (w->bits & 7) put(w, 0, 1);
    rbsp = (uint32_t)w->bits / 8;
    nal[0] = (uint8_t)(0x60 | nal_type);
    *epb = 0;
    for (i = 0; i < rbsp; i++) {
        if (zeros >= 2 && w->b[i] <= 3) { nal[n++] = 3; zeros = 0; (*epb)++; }
        nal[n++] = w->b[i];
        zeros = w->b[i] ? 0 : zeros + 1;
    }
    return n;
}

typedef struct {
    unsigned profile, chroma, depth, mbw, map_units;
    int frame_mbs_only, scaling, poc_type;
    unsigned crop_bottom;
} sps_spec;

static uint32_t make_sps(const sps_spec *s, uint8_t *nal, int *epb)
{
    bit_writer w;
    memset(&w, 0, sizeof w);
    put(&w, s->profile, 8);
    put(&w, 0, 8);
    put(&w, 40, 8);
    put_ue(&w, 0);
    if (s->profile == 100 || s->profile == 110 || s->profile == 122) {
        int i, j;
        put_ue(&w, s->chroma);
        put_ue(&w, s->depth - 8);
        put_ue(&w, s->depth - 8);
        put(&w, 0, 1);
        put(&w, s->scaling, 1);
        for (i = 0; s->scaling && i < 8; i++) {
            put(&w, i == 0 || i == 6, 1);
            for (j = 0; (i == 0 || i == 6) && j < (i < 6 ? 16 : 64); j++)
                put_se(&w, j & 1 ? 3 : -2);
        }
    }
    put_ue(&w, 0);
    put_ue(&w, s->poc_type);
    if (s->poc_type == 0) put_ue(&w, 2);
    else {
        /* Long runs of zero bits: the escape this NAL needs. */
        put(&w, 0, 1);
        put_se(&w, -65000);
        put_se(&w, 65000);
        put_ue(&w, 2);
        put_se(&w, 1 << 20);
        put_se(&w, -(1 << 20));
    }
    put_ue(&w, 4);
    put(&w, 0, 1);
    put_ue(&w, s->mbw - 1);
    put_ue(&w, s->map_units - 1);
    put(&w, s->frame_mbs_only, 1);
    if (!s->frame_mbs_only) put(&w, 0, 1);
    put(&w, 1, 1);
    put(&w, s->crop_bottom != 0, 1);
    if (s->crop_bottom) { put_ue(&w, 0); put_ue(&w, 0); put_ue(&w, 0);
                          put_ue(&w, s->crop_bottom); }
    put(&w, 0, 1);                              /* vui_parameters_present */
    return finish_nal(&w, 7, nal, epb);
}

static void synthetic_sps(void)
{
    static const struct {
        sps_spec s;
        unsigned width, height;
        int fits, wants_epb;
    } cases[] = {
        /* 1080p High: 1088 coded, cropped by 4 chroma rows to 1080. */
        { {100, 1, 8, 120, 68, 1, 0, 0, 4}, 1920, 1080, 1, 0 },
        { {100, 1, 8, 120, 75, 1, 0, 0, 0}, 1920, 1200, 0, 0 }, /* 9000 MBs */
        { {100, 1, 8, 128, 68, 1, 0, 0, 4}, 2048, 1080, 0, 0 }, /* too wide */
        { {110, 1, 10, 80, 45, 1, 0, 0, 0}, 1280, 720, 0, 0 },  /* High 10 */
        { {122, 2, 8, 80, 45, 1, 0, 0, 0}, 1280, 720, 0, 0 },   /* 4:2:2 */
        /* 1080i: 34 field-pair map units, crop unit 4 rows. */
        { {100, 1, 8, 120, 34, 0, 0, 0, 2}, 1920, 1080, 1, 0 },
        /* Scaling lists to skip, and POC type 1 values that need escapes. */
        { {100, 1, 8, 80, 45, 1, 1, 1, 0}, 1280, 720, 1, 1 },
        { {66, 1, 8, 20, 15, 1, 0, 0, 0}, 320, 240, 1, 0 },     /* Baseline */
        { {77, 1, 8, 127, 64, 1, 0, 0, 0}, 2032, 1024, 1, 0 },  /* widest */
    };
    unsigned i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t nal[600];
        mr_h264_sps sps;
        int epb;
        uint32_t n = make_sps(&cases[i].s, nal, &epb);
        if (cases[i].wants_epb) CHECK(epb > 0);
        CHECK(mr_h264_sps_parse(nal, n, &sps) == MR_OK);
        if (sps.width != cases[i].width || sps.height != cases[i].height) {
            fprintf(stderr, "case %u: %ux%u, expected %ux%u\n", i,
                    sps.width, sps.height, cases[i].width, cases[i].height);
            fail = 1;
        }
        CHECK(sps.profile_idc == cases[i].s.profile);
        CHECK(sps.bit_depth_luma == cases[i].s.depth);
        CHECK(mr_h264_sps_fits(&sps, 2032, 8192) == cases[i].fits);
        /* Cut short anywhere before the cropping fields: refused. */
        CHECK(mr_h264_sps_parse(nal, 6, &sps) == MR_EFORMAT);
    }
    {
        uint8_t pps[] = { 0x68, 0xce, 0x38, 0x80 };
        CHECK(mr_h264_sps_parse(pps, sizeof pps, NULL) == MR_EFORMAT);
    }
}

/* ---- avcC and samples ------------------------------------------------ */

static void synthetic_avcc(void)
{
    static const uint8_t sps[] = { 0x67, 0x42, 0xc0, 0x1e, 0xd9 };
    static const uint8_t pps[] = { 0x68, 0xce, 0x3c, 0x80 };
    static const uint8_t idr[] = { 0x65, 0x88, 0x84, 0x00, 0x21 };
    static const uint8_t slice[] = { 0x41, 0x9a, 0x02 };
    uint8_t avcc[64], out[128], sample[64];
    uint32_t n = 0, out_len, need;
    int nls, size;
    const uint8_t *first;
    uint32_t first_len;

    avcc[n++] = 1; avcc[n++] = 0x42; avcc[n++] = 0xc0; avcc[n++] = 0x1e;
    avcc[n++] = 0xfc | 1;                       /* two-byte lengths */
    avcc[n++] = 0xe0 | 1;
    avcc[n++] = 0; avcc[n++] = sizeof sps; memcpy(avcc + n, sps, sizeof sps);
    n += sizeof sps;
    avcc[n++] = 1;
    avcc[n++] = 0; avcc[n++] = sizeof pps; memcpy(avcc + n, pps, sizeof pps);
    n += sizeof pps;
    CHECK(mr_avcc_config_to_annexb(avcc, n, NULL, 0, &need, &nls) == MR_ENOMEM);
    CHECK(need == 8 + sizeof sps + sizeof pps && nls == 2);
    CHECK(mr_avcc_config_to_annexb(avcc, n, out, sizeof out, &out_len, &nls)
          == MR_OK);
    CHECK(out_len == need && !memcmp(out, "\0\0\0\1\x67", 5) &&
          !memcmp(out + 4 + sizeof sps, "\0\0\0\1\x68", 5));
    CHECK(mr_avcc_first_sps(avcc, n, &first, &first_len) == MR_OK &&
          first_len == sizeof sps && !memcmp(first, sps, sizeof sps));
    CHECK(mr_avcc_config_to_annexb(avcc, n - 1, out, sizeof out, &out_len,
                                   &nls) == MR_EFORMAT);
    avcc[4] = 0xfc | 2;                         /* three-byte lengths */
    CHECK(mr_avcc_config_to_annexb(avcc, n, out, sizeof out, &out_len,
                                   &nls) == MR_EFORMAT);

    for (size = 1; size <= 4; size *= 2) {
        uint32_t s = 0;
        int k;
        const uint8_t *nals[2] = { idr, slice };
        const uint32_t lens[2] = { sizeof idr, sizeof slice };
        for (k = 0; k < 2; k++) {
            int b;
            for (b = size - 1; b >= 0; b--) sample[s++] = (uint8_t)(lens[k] >> (b * 8));
            memcpy(sample + s, nals[k], lens[k]);
            s += lens[k];
        }
        need = 8 + sizeof idr + sizeof slice;
        memset(out, 0xEE, sizeof out);
        CHECK(mr_avcc_sample_to_annexb(sample, s, size, out, sizeof out,
                                       &out_len) == MR_OK);
        CHECK(out_len == need && !memcmp(out, "\0\0\0\1", 4) &&
              !memcmp(out + 4, idr, sizeof idr) &&
              !memcmp(out + 8 + sizeof idr, slice, sizeof slice));
        /* One byte short: the size needed comes back, nothing past cap. */
        memset(out, 0xEE, sizeof out);
        CHECK(mr_avcc_sample_to_annexb(sample, s, size, out, need - 1,
                                       &out_len) == MR_ENOMEM);
        CHECK(out_len == need && out[need - 1] == 0xEE);
        CHECK(mr_avcc_sample_to_annexb(sample, s - 1, size, out, sizeof out,
                                       &out_len) == MR_EFORMAT);
    }
    memset(sample, 0, 4);                       /* a zero-length NAL */
    CHECK(mr_avcc_sample_to_annexb(sample, 4, 4, out, sizeof out, &out_len)
          == MR_EFORMAT);
    CHECK(mr_avcc_sample_to_annexb(sample, 4, 3, out, sizeof out, &out_len)
          == MR_EFORMAT);
}

/* ---- fixtures -------------------------------------------------------- */

typedef struct { uint32_t off, len; } nal_ref;
typedef struct { uint8_t *data; uint32_t len, cap; nal_ref *nals;
                 unsigned count, nals_cap; } nal_list;

static void append(nal_list *l, const uint8_t *p, uint32_t n)
{
    if (l->len + n > l->cap) {
        l->cap = (l->len + n) * 2;
        l->data = (uint8_t *)realloc(l->data, l->cap);
    }
    memcpy(l->data + l->len, p, n);
    l->len += n;
}

/* Split an Annex-B stream into NAL units, keeping all but parameter sets
 * and access unit delimiters. */
static void split_annexb(const uint8_t *p, uint32_t len, nal_list *l)
{
    uint32_t i = 0, start = 0;
    int in_nal = 0;
    for (;;) {
        int at_code = i + 3 <= len && !p[i] && !p[i + 1] && p[i + 2] == 1;
        if (at_code || i >= len) {
            if (in_nal) {
                uint32_t end = i;
                while (end > start && !p[end - 1]) end--;
                if (end > start) {
                    unsigned type = p[start] & 0x1f;
                    if (type != 7 && type != 8 && type != 9) {
                        if (l->count == l->nals_cap) {
                            l->nals_cap = l->nals_cap ? l->nals_cap * 2 : 256;
                            l->nals = (nal_ref *)realloc(l->nals,
                                l->nals_cap * sizeof *l->nals);
                        }
                        l->nals[l->count].off = l->len;
                        l->nals[l->count].len = end - start;
                        l->count++;
                        append(l, p + start, end - start);
                    }
                }
            }
            if (i >= len) break;
            i += 3;
            start = i;
            in_nal = 1;
            continue;
        }
        i++;
    }
}

static uint8_t *load(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t *)malloc((size_t)*n + 1);
    if (!data || fread(data, 1, (size_t)*n, f) != (size_t)*n) {
        fclose(f); free(data); return NULL;
    }
    fclose(f);
    return data;
}

/* Show a differing NAL so a CI log says what changed. */
static void dump_nal(const char *who, const uint8_t *p, uint32_t len)
{
    uint32_t j;
    fprintf(stderr, "  %-6s len %lu:", who, (unsigned long)len);
    for (j = 0; j < len && j < 48; j++)
        fprintf(stderr, " %02x", p[j]);
    fprintf(stderr, "%s\n", len > 48 ? " ..." : "");
}

static int fixture(const char *path, const char *ref_path)
{
    const char *ext = strrchr(path, '.');
    long n;
    uint8_t *data = load(path, &n);
    mr_mov mov; mr_mkv mkv; mr_ts ts;
    const mr_video_info *vi;
    int kind = !ext ? 0 : !strcmp(ext, ".mp4") ? 1 : !strcmp(ext, ".mkv") ? 2 :
               (!strcmp(ext, ".ts") || !strcmp(ext, ".m2ts")) ? 3 : 0;
    const uint8_t *sps_nal;
    uint32_t sps_len, out_len;
    mr_h264_sps sps;
    nal_list ours, ref;
    mr_packet pkt;
    uint8_t *conv = NULL;
    uint32_t conv_cap = 0;
    int nls = 4;
    unsigned i;

    if (!data || !kind) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    memset(&ours, 0, sizeof ours);
    memset(&ref, 0, sizeof ref);
    if ((kind == 1 && mr_mov_open(&mov, data, (size_t)n) != MR_OK) ||
        (kind == 2 && mr_mkv_open(&mkv, data, (size_t)n) != MR_OK) ||
        (kind == 3 && mr_ts_open(&ts, data, (size_t)n) != MR_OK)) {
        fprintf(stderr, "%s: open failed\n", path);
        return 2;
    }
    vi = kind == 1 ? &mov.video : kind == 2 ? &mkv.video : &ts.video;
    CHECK(vi->fourcc == MR_FOURCC('a','v','c','1'));
    CHECK(mr_avcc_config_to_annexb(vi->config, vi->config_len, NULL, 0,
                                   &out_len, &nls) == MR_ENOMEM && out_len > 8);
    CHECK(mr_avcc_first_sps(vi->config, vi->config_len, &sps_nal, &sps_len)
          == MR_OK);
    CHECK(mr_h264_sps_parse(sps_nal, sps_len, &sps) == MR_OK);
    if (sps.width != (unsigned)vi->width || sps.height != (unsigned)vi->height ||
        sps.profile_idc != vi->config[1]) {
        fprintf(stderr, "%s: SPS %ux%u profile %u, container %dx%d profile %u\n",
                path, sps.width, sps.height, sps.profile_idc, vi->width,
                vi->height, vi->config[1]);
        fail = 1;
    }
    for (;;) {
        mr_status st;
        /* The demuxers set only what they carry (MPEG-TS alone sets
         * is_annexb); mr_demux_next_packet() clears the rest first. */
        memset(&pkt, 0, sizeof pkt);
        st = kind == 1 ? mr_mov_next_packet(&mov, &pkt) :
             kind == 2 ? mr_mkv_next_packet(&mkv, &pkt) :
                         mr_ts_next_packet(&ts, &pkt);
        if (st != MR_OK) break;
        if (!pkt.is_video) continue;
        if (pkt.is_annexb) {
            split_annexb(pkt.data, pkt.len, &ours);
            continue;
        }
        if (mr_avcc_sample_to_annexb(pkt.data, pkt.len, nls, conv, conv_cap,
                                     &out_len) == MR_ENOMEM) {
            conv_cap = out_len;
            conv = (uint8_t *)realloc(conv, conv_cap);
            CHECK(mr_avcc_sample_to_annexb(pkt.data, pkt.len, nls, conv,
                                           conv_cap, &out_len) == MR_OK);
        }
        split_annexb(conv, out_len, &ours);
    }
    if (strcmp(ref_path, "-")) {
        long rn;
        uint8_t *rdata = load(ref_path, &rn);
        if (!rdata) { fprintf(stderr, "cannot read %s\n", ref_path); return 2; }
        split_annexb(rdata, (uint32_t)rn, &ref);
        free(rdata);
        if (ours.count != ref.count) {
            fprintf(stderr, "%s: %u NAL units, ffmpeg %u\n", path, ours.count,
                    ref.count);
            fail = 1;
        }
        for (i = 0; i < ours.count && i < ref.count; i++) {
            if (ours.nals[i].len != ref.nals[i].len ||
                memcmp(ours.data + ours.nals[i].off,
                       ref.data + ref.nals[i].off, ours.nals[i].len)) {
                fprintf(stderr, "%s: NAL %u differs from ffmpeg's\n", path, i);
                dump_nal("ours", ours.data + ours.nals[i].off, ours.nals[i].len);
                dump_nal("ffmpeg", ref.data + ref.nals[i].off, ref.nals[i].len);
                fail = 1;
                break;
            }
        }
    }
    printf("%s: %ux%u profile %u level %u, %u NAL units%s\n", path, sps.width,
           sps.height, sps.profile_idc, sps.level_idc, ours.count,
           strcmp(ref_path, "-") ? " = ffmpeg h264_mp4toannexb" : "");
    if (kind == 1) mr_mov_close(&mov);
    else if (kind == 2) mr_mkv_close(&mkv);
    else mr_ts_close(&ts);
    free(conv); free(ours.data); free(ours.nals); free(ref.data); free(ref.nals);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3) {
        int rc = fixture(argv[1], argv[2]);
        return rc ? rc : fail;
    }
    synthetic_sps();
    synthetic_avcc();
    puts(fail ? "AVC check failed"
              : "AVC: SPS limits, emulation prevention, avcC and samples ok");
    return fail;
}
