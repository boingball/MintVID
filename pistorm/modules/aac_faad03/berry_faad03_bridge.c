#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifdef BERRY_FAAD_STUB
#include "neaacdec_min.h"
#else
#include "neaacdec.h"
#endif
#include "runtime/berry_runtime.h"

/* BerryCore mailbox offsets used by BC19+ codec ABI. */
typedef struct BerryMailbox {
    uint32_t _pad00[6];       /* 0..23 */
    uint32_t in_phys;         /* 24 */
    uint32_t in_len;          /* 28 */
    uint32_t out_phys;        /* 32 */
    uint32_t out_len;         /* 36 */
    uint32_t param0;          /* 40 */
    uint32_t param1;          /* 44 */
    uint32_t work_phys;       /* 48 */
    uint32_t work_len;        /* 52 */
} BerryMailbox;

typedef struct BerryFaadWork {
    uint32_t magic;           /* BFAD */
    uint32_t version;
    uint32_t stream_off;
    uint32_t frame_index;
    uint64_t decoder;
    uint32_t initialized;
    uint32_t heap_used;
    uint32_t init_rate;
    uint32_t init_channels;
    uint32_t reserved[6];     /* 64-byte control area */
    /* arena starts at +64 */
} BerryFaadWork;

typedef struct BerryFaadOut {
    uint32_t marker;          /* FAD1 */
    uint32_t status;
    uint32_t samplerate;
    uint32_t channels;
    uint32_t bytesconsumed;
    uint32_t samples;
    uint32_t error;
    uint32_t frame_index;
    uint32_t input_offset;
    uint32_t next_offset;
    uint32_t pcm_fnv1a;
    int32_t  first_l;
    int32_t  first_r;
    int32_t  last_l;
    int32_t  last_r;
    uint32_t heap_used;
    uint32_t capabilities;
    uint32_t reserved[47];    /* metadata total 256 bytes */
    int16_t pcm[32768];       /* 65536 bytes, LRLR... for streaming batches */
} BerryFaadOut;

#define BFAD_MAGIC 0x42464144u
#define FAD1_MARK  0x46414431u
#define BFAD_VERSION 1u
#define WORK_CTRL 64u
#define WORK_HEAP_BYTES 65536u
#define WORK_STACK_BYTES 98304u
#define WORK_MIN (WORK_CTRL + WORK_HEAP_BYTES + WORK_STACK_BYTES)
#define OUT_MIN  (256u + 65536u)

#ifdef BERRY_FAAD_RAW_MODULE
void berry_relocate_huffman(uintptr_t base);
void berry_relocate_specrec(uintptr_t base);
void berry_relocate_tns(uintptr_t base);
void berry_relocate_error(uintptr_t base);
static uint32_t g_module_relocated;
static void berry_relocate_module_once(uintptr_t base)
{
    if (g_module_relocated) return;
    berry_relocate_huffman(base);
    berry_relocate_specrec(base);
    berry_relocate_tns(base);
    berry_relocate_error(base);
    g_module_relocated = 1;
}
#endif

static inline void *alias_ptr(uint32_t phys, uint64_t alias_add) {
    return (void *)(uintptr_t)((uint64_t)phys + alias_add);
}
static int adts_len(const uint8_t *p, uint32_t n, uint32_t *frame_bytes) {
    if (n < 7 || p[0] != 0xff || (p[1] & 0xf0) != 0xf0) return -1;
    uint32_t z = ((uint32_t)(p[3]&3)<<11) | ((uint32_t)p[4]<<3) | ((uint32_t)p[5]>>5);
    if (z < 7 || z > n) return -2; *frame_bytes=z; return 0;
}
/* P96W31: stereo, or 5.0/5.1 which FAAD (downMatrix=1) mixes to stereo.
   The library and the HDMI renderer always get 2 channels. */
static int berry_chans_ok(unsigned char c) { return c==2 || c==5 || c==6; }
static void set_ret(uint32_t *r0,uint32_t *r1,uint32_t *r2,uint32_t *r3,uint32_t a,uint32_t b,uint32_t c,uint32_t d){*r0=a;*r1=b;*r2=c;*r3=d;}
static uint32_t fnv1a_update(uint32_t h, const uint8_t *p, uint32_t n) {
    while (n--) { h ^= *p++; h *= 0x01000193u; }
    return h;
}

/* AArch64 C ABI: x0=mailbox, x1=phys->ARM alias addend, x2=module base.
   Result is returned in x0; BerryCore historically also consumes w1-w3, so
   the assembly epilogue below maps a 128-bit struct to x0/x1. For the first
   FAAD test we also mirror detailed data in the output buffer. */
typedef struct { uint32_t a,b,c,d; } BerryRet;
BerryRet berry_faad_c_entry(BerryMailbox *mb, uint64_t alias_add, void *module_base) {
#ifdef BERRY_FAAD_RAW_MODULE
    berry_relocate_module_once((uintptr_t)module_base);
#else
    (void)module_base;
#endif
    BerryRet ret={0,0,0,0};
    if (!mb) { ret.c=90; return ret; }
    uint32_t cmd=mb->param0;
    BerryFaadWork *w=(BerryFaadWork*)alias_ptr(mb->work_phys,alias_add);
    BerryFaadOut *o=(BerryFaadOut*)alias_ptr(mb->out_phys,alias_add);
    if (!w || mb->work_len < WORK_MIN) { ret.c=91; return ret; }
    if (!o || mb->out_len < OUT_MIN) { ret.c=92; return ret; }

    if (cmd==0 || cmd==2) {
        memset(w,0,WORK_CTRL);
        memset(o,0,256);
        w->magic=BFAD_MAGIC; w->version=BFAD_VERSION;
        berry_heap_bind((uint8_t*)w+WORK_CTRL, WORK_HEAP_BYTES);
        NeAACDecHandle h=NeAACDecOpen();
        if (!h || berry_heap_failed()) { ret.c=93; return ret; }
        NeAACDecConfigurationPtr cfg=NeAACDecGetCurrentConfiguration(h);
        if (!cfg) { ret.c=94; return ret; }
        cfg->defObjectType=LC; cfg->defSampleRate=48000; cfg->outputFormat=FAAD_FMT_16BIT;
        cfg->downMatrix=1; /* P96W31: FAAD mixes 5.0/5.1 down to stereo */ cfg->useOldADTSFormat=0; cfg->dontUpSampleImplicitSBR=1;
        if (!NeAACDecSetConfiguration(h,cfg)) { ret.c=95; return ret; }
        w->decoder=(uint64_t)(uintptr_t)h;
        w->heap_used=(uint32_t)berry_heap_used();
        o->marker=FAD1_MARK; o->status=1; o->heap_used=w->heap_used; o->capabilities=(uint32_t)NeAACDecGetCapabilities();
        ret.a=1; return ret;
    }

    if (cmd==3) {
        if (w->magic!=BFAD_MAGIC || !w->decoder) { ret.c=96; return ret; }
        uint8_t *input=(uint8_t*)alias_ptr(mb->in_phys,alias_add);
        if (!input || !mb->in_len) { ret.c=97; return ret; }
        NeAACDecHandle h=(NeAACDecHandle)(uintptr_t)w->decoder;
        if (!w->initialized) {
            unsigned long rate=0; unsigned char chans=0;
            long z=NeAACDecInit(h,input,mb->in_len,&rate,&chans);
            if (z<0 || rate==0 || !berry_chans_ok(chans)) { ret.c=99; return ret; }
            w->initialized=1; w->init_rate=(uint32_t)rate; w->init_channels=2;
            w->reserved[0]=chans;   /* source channels (2, 5 or 6) */
        }

        memset(o,0,256);
        o->marker=FAD1_MARK; o->status=4; o->samplerate=w->init_rate; o->channels=w->init_channels;
        o->input_offset=w->stream_off;
        uint32_t local_off=0, frames=0, blocks=0, pcm_bytes=0, total_samples=0, stop_reason=0;
        uint32_t hash=0x811c9dc5u;
        uint8_t *pcm_base=(uint8_t*)o + 256u;
        const uint32_t pcm_cap=65536u;
        int have_first=0;

        while (local_off < mb->in_len) {
            uint32_t left=mb->in_len-local_off;
            if (left < 7u) { stop_reason=1; break; }
            uint32_t frame_bytes=0;
            int ar=adts_len(input+local_off,left,&frame_bytes);
            if (ar==-2) { stop_reason=1; break; }
            if (ar) { ret.c=98; return ret; }
            if (pcm_cap-pcm_bytes < 4096u && frames!=0u) { stop_reason=2; break; }

            NeAACDecFrameInfo fi; memset(&fi,0,sizeof(fi));
            void *dst=pcm_base+pcm_bytes;
            void *rv=NeAACDecDecode2(h,&fi,input+local_off,frame_bytes,&dst,pcm_cap-pcm_bytes);
            (void)rv;
            if (fi.error) { o->error=fi.error; ret.c=100u+fi.error; return ret; }
            if (fi.bytesconsumed==0 || fi.bytesconsumed>left) { ret.c=101; return ret; }
            if (fi.samples && fi.channels!=2) { ret.c=103; return ret; }
            local_off += (uint32_t)fi.bytesconsumed;
            w->stream_off += (uint32_t)fi.bytesconsumed;
            w->frame_index++;
            frames++;

            if (fi.samples) {
                uint32_t nb=(uint32_t)fi.samples*2u;
                if (nb > pcm_cap-pcm_bytes) { ret.c=102; return ret; }
                if (dst != pcm_base+pcm_bytes) memcpy(pcm_base+pcm_bytes,dst,nb);
                if (!have_first) {
                    int16_t *q=(int16_t*)(void*)(pcm_base+pcm_bytes);
                    o->first_l=q[0]; o->first_r=q[1]; have_first=1;
                }
                int16_t *q=(int16_t*)(void*)(pcm_base+pcm_bytes);
                o->last_l=q[fi.samples-2]; o->last_r=q[fi.samples-1];
                hash=fnv1a_update(hash,pcm_base+pcm_bytes,nb);
                pcm_bytes += nb;
                total_samples += (uint32_t)fi.samples;
                blocks++;
            }
            if ((uint32_t)fi.bytesconsumed < frame_bytes) break;
        }

        o->bytesconsumed=local_off; o->samples=total_samples; o->frame_index=w->frame_index;
        o->next_offset=w->stream_off; o->pcm_fnv1a=blocks ? hash : 0u;
        w->heap_used=(uint32_t)berry_heap_used(); o->heap_used=w->heap_used;
        o->capabilities=(uint32_t)NeAACDecGetCapabilities();
        o->reserved[0]=frames; o->reserved[1]=blocks; o->reserved[2]=pcm_bytes;
        o->reserved[3]=total_samples; o->reserved[4]=local_off; o->reserved[5]=stop_reason;
        o->reserved[6]=pcm_cap; o->reserved[7]=w->frame_index;
        o->reserved[8]=w->reserved[0];
        ret.a=o->pcm_fnv1a; ret.b=local_off; ret.c=0; ret.d=w->frame_index;
        return ret;
    }

    if (cmd!=1 || w->magic!=BFAD_MAGIC || !w->decoder) { ret.c=96; return ret; }
    uint8_t *input=(uint8_t*)alias_ptr(mb->in_phys,alias_add);
    if (!input || w->stream_off >= mb->in_len) { ret.c=97; return ret; }
    uint32_t left=mb->in_len-w->stream_off;
    uint8_t *frame=input+w->stream_off;
    uint32_t frame_bytes=0;
    if (adts_len(frame,left,&frame_bytes)) { ret.c=98; return ret; }

    NeAACDecHandle h=(NeAACDecHandle)(uintptr_t)w->decoder;
    if (!w->initialized) {
        unsigned long rate=0; unsigned char chans=0;
        long z=NeAACDecInit(h,frame,left,&rate,&chans);
        if (z<0 || rate==0 || !berry_chans_ok(chans)) { ret.c=99; return ret; }
        w->initialized=1; w->init_rate=(uint32_t)rate; w->init_channels=2;
        w->reserved[0]=chans;
    }

    memset(o,0,256);
    o->marker=FAD1_MARK; o->frame_index=w->frame_index+1; o->input_offset=w->stream_off;
    NeAACDecFrameInfo fi; memset(&fi,0,sizeof(fi));
    void *pcm=o->pcm;
    void *rv=NeAACDecDecode2(h,&fi,frame,frame_bytes,&pcm,sizeof(o->pcm));
    o->status=rv ? 2u : 3u;
    o->samplerate=(uint32_t)fi.samplerate; o->channels=fi.channels;
    o->bytesconsumed=(uint32_t)fi.bytesconsumed; o->samples=(uint32_t)fi.samples; o->error=fi.error;
    if (fi.error) { ret.c=100u+fi.error; return ret; }
    if (fi.bytesconsumed==0 || fi.bytesconsumed>left) { ret.c=101; return ret; }
    if (fi.samples && fi.channels!=2) { ret.c=103; return ret; }
    w->stream_off += (uint32_t)fi.bytesconsumed;
    w->frame_index++;
    o->next_offset=w->stream_off;
    if (fi.samples && pcm != o->pcm) memcpy(o->pcm,pcm,fi.samples*2u);
    if (fi.samples) {
        o->pcm_fnv1a=berry_fnv1a32(o->pcm,fi.samples*2u);
        o->first_l=o->pcm[0]; o->first_r=o->pcm[1];
        o->last_l=o->pcm[fi.samples-2]; o->last_r=o->pcm[fi.samples-1];
    }
    w->heap_used=(uint32_t)berry_heap_used(); o->heap_used=w->heap_used; o->capabilities=(uint32_t)NeAACDecGetCapabilities();
    ret.a=o->pcm_fnv1a; ret.b=(uint32_t)fi.bytesconsumed; ret.c=0; ret.d=w->frame_index;
    return ret;
}
