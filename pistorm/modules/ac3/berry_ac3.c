#include <stdint.h>
#include <stddef.h>
#include "liba52/config-a52.h"
#include "liba52/a52.h"
#include "liba52/a52_internal.h"

typedef struct BerryMailbox {
    uint32_t _pad00[6];
    uint32_t in_phys, in_len, out_phys, out_len;
    uint32_t param0, param1, work_phys, work_len;
} BerryMailbox;

typedef struct BerryRet { uint32_t a,b,c,d; } BerryRet;

typedef struct BerryAC3Work {
    uint32_t magic;             /* BAC3 */
    uint32_t version;
    uint32_t frame_index;
    uint32_t total_bytes;
    uint32_t total_pcm_bytes;
    uint32_t stream_hash;
    uint32_t batch_index;
    uint32_t reserved0;
    a52_state_t decoder;
    sample_t samples[256*12];
} BerryAC3Work;

typedef struct BerryAC3Out {
    uint32_t marker;            /* AC34 */
    uint32_t status;            /* 1=init, 4=stream batch decoded */
    uint32_t samplerate;
    uint32_t channels;
    uint32_t input_frame_bytes;
    uint32_t input_bitrate;
    uint32_t frames_decoded;
    uint32_t samples_per_channel;
    uint32_t pcm_bytes;
    uint32_t bytes_consumed;
    uint32_t pcm_fnv1a;
    uint32_t nonzero_samples;
    int32_t min_sample;
    int32_t max_sample;
    int32_t first_l;
    int32_t first_r;
    int32_t last_l;
    int32_t last_r;
    uint32_t error_stage;
    uint32_t flags_last;
    uint32_t level_last;
    uint32_t work_bytes;
    uint32_t input_acmod;
    uint32_t input_lfe;
    uint32_t downmix_mode;      /* 2 = stereo */
    uint32_t lfe_policy;        /* 0 = omit LFE */
    uint32_t abs_sum_l;
    uint32_t abs_sum_r;
    uint32_t peak_l;
    uint32_t peak_r;
    uint32_t total_frames;
    uint32_t total_input_bytes;
    uint32_t total_pcm_bytes;
    uint32_t stream_pcm_fnv1a;
    uint32_t batch_index;
    uint32_t reserved[29];      /* metadata total 256 bytes */
    int16_t pcm[24576];         /* <=8 AC3 stereo frames = <=49152 bytes */
} BerryAC3Out;

#define BAC3_MAGIC   0x42414333u
#define BAC3_VERSION 4u
#define AC34_MARK    0x41433334u
#define PCM_CAP_S16  24576u
#define AC3_SHIFT    13
#define OUT_META     256u
#define OUT_MIN      (OUT_META + PCM_CAP_S16*2u)
#define WORK_MIN     ((uint32_t)sizeof(BerryAC3Work))

_Static_assert(offsetof(BerryAC3Out, pcm) == 256u, "BerryAC3Out metadata must be 256 bytes");
_Static_assert(sizeof(((BerryAC3Out*)0)->pcm) == 49152u, "PCM payload size must be 49152 bytes");

static inline void *alias_ptr(uint32_t phys, uint64_t add) {
    return (void *)(uintptr_t)((uint64_t)phys + add);
}
static inline uint32_t fnv_byte(uint32_t h,uint8_t b){ h^=b; return h*0x01000193u; }
static inline uint32_t fnv_s16be(uint32_t h,int16_t v){ uint16_t u=(uint16_t)v; h=fnv_byte(h,(uint8_t)(u>>8)); return fnv_byte(h,(uint8_t)u); }
static inline int16_t pcm16_from_sample(sample_t x){
    int32_t v=x >> AC3_SHIFT;
    if(v < -32768) v=-32768; else if(v > 32767) v=32767;
    return (int16_t)v;
}
static void zero_meta(BerryAC3Out *o){ uint32_t *p=(uint32_t*)(void*)o; uint32_t i; for(i=0;i<OUT_META/4u;i++)p[i]=0; }

__attribute__((used)) void berry_ac3_c_entry(BerryMailbox *mb,uint64_t alias_add,void *module_base,BerryRet *ret)
{
    BerryAC3Work *w; BerryAC3Out *o; uint32_t cmd;
    (void)module_base;
    ret->a=ret->b=ret->c=ret->d=0;
    if(!mb){ret->c=90;return;}
    w=(BerryAC3Work*)alias_ptr(mb->work_phys,alias_add);
    o=(BerryAC3Out*)alias_ptr(mb->out_phys,alias_add);
    if(!w || mb->work_len<WORK_MIN){ret->c=91;return;}
    if(!o || mb->out_len<OUT_MIN){ret->c=92;return;}
    cmd=mb->param0;

    if(cmd==0u || cmd==2u){
        uint32_t *wp=(uint32_t*)(void*)w; uint32_t i;
        for(i=0;i<WORK_MIN/4u;i++)wp[i]=0;
        zero_meta(o);
        if(!a52_init_work(&w->decoder,w->samples,0)){ret->c=93;return;}
        w->magic=BAC3_MAGIC; w->version=BAC3_VERSION; w->stream_hash=0x811c9dc5u;
        o->marker=AC34_MARK; o->status=1u; o->work_bytes=WORK_MIN; o->stream_pcm_fnv1a=w->stream_hash;
        ret->a=1u; ret->b=WORK_MIN; return;
    }

    if(cmd==3u){
        uint8_t *in; uint32_t off=0,frames=0,pcm_n=0,hash=0x811c9dc5u,nonzero=0;
        uint32_t abs_l=0,abs_r=0,peak_l=0,peak_r=0;
        int32_t minv=32767,maxv=-32768; int have_first=0;
        if(w->magic!=BAC3_MAGIC || w->version!=BAC3_VERSION){ret->c=94;return;}
        in=(uint8_t*)alias_ptr(mb->in_phys,alias_add);
        if(!in || mb->in_len<7u){ret->c=95;return;}
        zero_meta(o); o->marker=AC34_MARK; o->status=4u; o->work_bytes=WORK_MIN;
        while(off+7u<=mb->in_len && frames<8u){
            int flags=0,rate=0,bitrate=0,flen,blk;
            level_t level=(level_t)(1<<24); sample_t bias=0;
            flen=a52_syncinfo(in+off,&flags,&rate,&bitrate);
            if(frames==0u){o->input_acmod=(uint32_t)(flags & A52_CHANNEL_MASK);o->input_lfe=(flags & A52_LFE)?1u:0u;o->downmix_mode=A52_STEREO;o->lfe_policy=0u;}
            if(flen<=0){o->error_stage=10u;ret->c=110u;return;}
            if(off+(uint32_t)flen>mb->in_len)break;
            flags=A52_STEREO|A52_ADJUST_LEVEL;
            if(a52_frame(&w->decoder,in+off,&flags,&level,bias)){o->error_stage=20u;ret->c=120u;return;}
            a52_dynrng(&w->decoder,0,0);
            if(frames==0u){o->samplerate=(uint32_t)rate;o->channels=2u;o->input_frame_bytes=(uint32_t)flen;o->input_bitrate=(uint32_t)bitrate;}
            for(blk=0;blk<6;blk++){
                sample_t *s; uint32_t i;
                if(a52_block(&w->decoder)){o->error_stage=30u+(uint32_t)blk;ret->c=130u+(uint32_t)blk;return;}
                s=a52_samples(&w->decoder);
                for(i=0;i<256u;i++){
                    int16_t l=pcm16_from_sample(s[i]); int16_t r=pcm16_from_sample(s[256u+i]);
                    if(pcm_n+2u>PCM_CAP_S16){o->error_stage=50u;ret->c=150u;return;}
                    o->pcm[pcm_n++]=l; o->pcm[pcm_n++]=r;
                    hash=fnv_s16be(hash,l); hash=fnv_s16be(hash,r);
                    w->stream_hash=fnv_s16be(w->stream_hash,l); w->stream_hash=fnv_s16be(w->stream_hash,r);
                    { uint32_t al=(uint32_t)(l<0?-(int32_t)l:l), ar=(uint32_t)(r<0?-(int32_t)r:r);
                      abs_l+=al; abs_r+=ar; if(al>peak_l)peak_l=al; if(ar>peak_r)peak_r=ar; }
                    if(l){nonzero++;} if(r){nonzero++;}
                    if(l<minv)minv=l;if(l>maxv)maxv=l;if(r<minv)minv=r;if(r>maxv)maxv=r;
                    if(!have_first){o->first_l=l;o->first_r=r;have_first=1;}
                    o->last_l=l;o->last_r=r;
                }
            }
            off+=(uint32_t)flen; frames++;
            o->flags_last=(uint32_t)flags; o->level_last=(uint32_t)level;
        }
        o->frames_decoded=frames; o->samples_per_channel=frames*1536u;
        o->pcm_bytes=pcm_n*2u; o->bytes_consumed=off; o->pcm_fnv1a=hash;
        o->nonzero_samples=nonzero; o->min_sample=minv; o->max_sample=maxv;
        o->abs_sum_l=abs_l; o->abs_sum_r=abs_r; o->peak_l=peak_l; o->peak_r=peak_r;
        w->frame_index+=frames; w->total_bytes+=off; w->total_pcm_bytes+=o->pcm_bytes; w->batch_index++;
        o->total_frames=w->frame_index; o->total_input_bytes=w->total_bytes; o->total_pcm_bytes=w->total_pcm_bytes;
        o->stream_pcm_fnv1a=w->stream_hash; o->batch_index=w->batch_index;
        ret->a=1u; ret->b=frames; ret->c=0u; ret->d=o->pcm_bytes; return;
    }
    ret->c=96u;
}
