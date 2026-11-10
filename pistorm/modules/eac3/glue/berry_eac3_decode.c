#include <stdint.h>
#include <stddef.h>
#include "libavcodec/avcodec.h"
#include "libavcodec/ac3dec.h"
#include "libavutil/channel_layout.h"

int berry_ff_ac3_decode_init_stage(AVCodecContext *avctx,int stage);
void berry_ff_ac3_decode_flush(AVCodecContext *avctx);
int berry_ff_ac3_decode_frame(AVCodecContext *avctx,AVFrame *frame,int *got_frame_ptr,AVPacket *avpkt);
int berry_ff_ac3_decode_end(AVCodecContext *avctx);
void berry_ff_heap_bind(void *base,size_t bytes);
size_t berry_ff_heap_used(void);
void berry_ff_set_planes(int16_t *p0,int16_t *p1,int max_samples);
void berry_ff_diag_bind(volatile uint32_t *p);
void berry_asimd_probe(void);

typedef struct BerryMailbox { uint32_t _pad00[6]; uint32_t in_phys,in_len,out_phys,out_len,param0,param1,work_phys,work_len; } BerryMailbox;
typedef struct BerryRet { uint32_t a,b,c,d; } BerryRet;

typedef struct BerryEAC302Work {
    uint32_t magic,version,initialized,init_stage;
    uint32_t frames,total_pcm_bytes,heap_used,decode_errors;
    AVCodecContext avctx;
    AC3DecodeContext ac3;
    AVFrame frame;
    AVPacket packet;
} BerryEAC302Work;

typedef struct BerryEAC302Out {
    uint32_t marker,status,error,consumed;
    uint32_t samplerate,input_channels,output_channels,samples;
    uint32_t pcm_bytes,pcm_fnv1a,min_sample,max_sample;
    uint32_t total_frames,total_pcm_bytes,heap_used,flags;
    uint8_t reserved[192];
    int16_t plane0[1536];
    int16_t plane1[1536];
    int16_t stereo[1536*2];
} BerryEAC302Out;

#define MAGIC 0x42453342u
#define VERSION 2u
#define MARK 0x45333242u
static inline void *ap(uint32_t p,uint64_t a){return(void*)(uintptr_t)((uint64_t)p+a);}
static uint32_t fnv(const uint8_t*p,uint32_t n){uint32_t h=2166136261u;while(n--){h^=*p++;h*=16777619u;}return h;}
static void z(void*p,uint32_t n){uint8_t*q=p;while(n--)*q++=0;}

__attribute__((used)) void berry_eac3_c_entry(BerryMailbox *mb,uint64_t alias_add,void *module_base,BerryRet *ret){
    (void)module_base; ret->a=ret->b=ret->c=ret->d=0; if(!mb){ret->c=90;return;}
    BerryEAC302Work*w=(BerryEAC302Work*)ap(mb->work_phys,alias_add); BerryEAC302Out*o=(BerryEAC302Out*)ap(mb->out_phys,alias_add);
    if(!w||mb->work_len<sizeof(*w)+262144u){ret->c=91;return;} if(!o||mb->out_len<sizeof(*o)){ret->c=92;return;}
    if(mb->param0==9){o->marker=MARK;o->status=9;o->error=0;ret->a=1;ret->b=0x302B;return;}
    if(mb->param0==8){berry_asimd_probe();o->marker=MARK;o->status=8;o->error=0;ret->a=1;ret->b=0xA51D;return;}

    /* Stage 0: only persistent work reset, heap bind and AVContext wiring. */
    if(mb->param0==0||mb->param0==2){
        z(o,sizeof(*o));
        o->marker=MARK;o->status=0x0F0;o->flags=0xB00000F0u;
        z(w,sizeof(*w));
        o->status=0x100;o->flags=0xB0000000u; berry_ff_diag_bind(&o->flags);
        w->magic=MAGIC;w->version=VERSION;
        uintptr_t hb=((uintptr_t)w+sizeof(*w)+31u)&~(uintptr_t)31u; size_t hs=mb->work_len-(size_t)(hb-(uintptr_t)w);
        berry_ff_heap_bind((void*)hb,hs); w->avctx.priv_data=&w->ac3; w->avctx.err_recognition=0; w->avctx.flags=0;
        int r=berry_ff_ac3_decode_init_stage(&w->avctx,0);
        if(r<0){o->error=(uint32_t)(-r);ret->c=100u+(uint32_t)(-r);return;}
        w->init_stage=0;o->heap_used=(uint32_t)berry_ff_heap_used();o->flags=0xB0000001u;ret->a=1;ret->b=o->heap_used;return;
    }

    /* Stages 1..6 are deliberately separate BerryCore CALL_MODULE calls. */
    if(mb->param0>=10 && mb->param0<=15){
        uint32_t st=mb->param0-9u;
        if(w->magic!=MAGIC){ret->c=93;return;}
        z(o,sizeof(*o));o->marker=MARK;o->status=0x100u+st;o->flags=0xB0000000u|st;berry_ff_diag_bind(&o->flags);
        int r=berry_ff_ac3_decode_init_stage(&w->avctx,(int)st);
        if(r<0){o->error=(uint32_t)(-r);ret->c=100u+(uint32_t)(-r);return;}
        w->init_stage=st;w->heap_used=(uint32_t)berry_ff_heap_used();o->heap_used=w->heap_used;o->flags=0xB0000100u|st;
        if(st==6u)w->initialized=1;
        ret->a=1;ret->b=w->heap_used;ret->c=0;ret->d=st;return;
    }

    if(mb->param0==1){
        if(w->magic!=MAGIC||!w->initialized){ret->c=93;return;} uint8_t*in=ap(mb->in_phys,alias_add); if(!in||mb->in_len<7){ret->c=94;return;}
        z(o,sizeof(*o));o->marker=MARK;o->status=2;o->flags=0xB1000000u; berry_ff_diag_bind(&o->flags); berry_ff_set_planes(o->plane0,o->plane1,1536);
        z(&w->frame,sizeof(w->frame));z(&w->packet,sizeof(w->packet));w->packet.data=in;w->packet.size=(int)mb->in_len;
        int got=0; int r=berry_ff_ac3_decode_frame(&w->avctx,&w->frame,&got,&w->packet);
        if(r<0||!got){w->decode_errors++;o->error=(uint32_t)(r<0?-r:999);ret->c=110u+o->error;return;}
        int n=w->frame.nb_samples; if(n<1||n>1536||w->avctx.ch_layout.nb_channels!=2){o->error=998;ret->c=998;return;}
        int16_t*l=(int16_t*)w->frame.extended_data[0],*rr=(int16_t*)w->frame.extended_data[1];int minv=32767,maxv=-32768;
        for(int i=0;i<n;i++){int lv=l[i],rv=rr[i];o->stereo[i*2]=(int16_t)lv;o->stereo[i*2+1]=(int16_t)rv;if(lv<minv)minv=lv;if(rv<minv)minv=rv;if(lv>maxv)maxv=lv;if(rv>maxv)maxv=rv;}
        uint32_t pb=(uint32_t)n*4u;w->frames++;w->total_pcm_bytes+=pb;w->heap_used=(uint32_t)berry_ff_heap_used();
        o->consumed=(uint32_t)r;o->samplerate=(uint32_t)w->avctx.sample_rate;o->input_channels=(uint32_t)w->ac3.channels;o->output_channels=2;o->samples=(uint32_t)n;o->pcm_bytes=pb;o->pcm_fnv1a=fnv((uint8_t*)o->stereo,pb);o->min_sample=(uint32_t)minv;o->max_sample=(uint32_t)maxv;o->total_frames=w->frames;o->total_pcm_bytes=w->total_pcm_bytes;o->heap_used=w->heap_used;o->flags=0xB1000001u;
        ret->a=1;ret->b=pb;ret->c=0;ret->d=(uint32_t)r;return;
    }
    if(mb->param0==3){if(w->initialized)berry_ff_ac3_decode_end(&w->avctx);w->initialized=0;ret->a=1;return;}
    ret->c=95;
}
