#include <stdint.h>
#include <stddef.h>

typedef struct BerryMailbox {
    uint32_t _pad00[6];
    uint32_t in_phys,in_len,out_phys,out_len,param0,param1,work_phys,work_len;
} BerryMailbox;
typedef struct BerryRet { uint32_t a,b,c,d; } BerryRet;

void berry_eac3_c_entry(BerryMailbox *mb,uint64_t alias_add,void *module_base,BerryRet *ret);

#define AC305_MARK        0x41433334u /* 'AC34' compatibility marker */
#define AC305_STATUS      4u
#define E302_MARK         0x45333242u /* 'E32B' */
#define E302_STEREO_OFF   6400u
#define SCRATCH_OFF       0xB000u
#define PCM_HEAD_OFF      256u
#define OUT_LIMIT         0x11000u
#define CLAIMED_WORK_LEN  0x00080000u
#define MAX_FRAMES_BATCH  16u

static inline uint8_t *ap(uint32_t p,uint64_t add){return (uint8_t*)(uintptr_t)((uint64_t)p+add);}
static void copyb(uint8_t*d,const uint8_t*s,uint32_t n){while(n--)*d++=*s++;}
static void zerob(uint8_t*d,uint32_t n){while(n--)*d++=0;}
static uint32_t eac3_size(const uint8_t*p,uint32_t remain){
    if(remain<4u || p[0]!=0x0b || p[1]!=0x77) return 0;
    uint32_t n=(((((uint32_t)p[2])&7u)<<8)|(uint32_t)p[3])+1u;
    n <<= 1;
    return (n>=7u && n<=remain) ? n : 0u;
}
static void put32(uint8_t*p,uint32_t off,uint32_t v){*(uint32_t*)(void*)(p+off)=v;}
static uint32_t get32(const uint8_t*p,uint32_t off){return *(const uint32_t*)(const void*)(p+off);}

/* Exact FFmpeg-9.0.2/E302 object-layout constants, statically verified against
 * the packaged AArch64-BE disassembly:
 *   BerryEAC302Work.ac3 starts at work+0x380.
 *   AC3DecodeContext.downmix_layout is at ac3+0x138.
 * AV_CHANNEL_LAYOUT_STEREO is { NATIVE=1, nb_channels=2, mask=3, opaque=0 }.
 * Setting this requests FFmpeg's own fixed-point Lo/Ro stereo downmix before
 * ff_get_buffer(), so the proven two-plane/PCM16 HDMI path stays unchanged. */
#define E302_WORK_AC3_OFF       0x0380u
#define E302_AC3_DMIX_OFF       0x0138u
#define E302_STEREO_DMIX_OFF    (E302_WORK_AC3_OFF + E302_AC3_DMIX_OFF)
static void put64(uint8_t*p,uint32_t off,uint64_t v){*(uint64_t*)(void*)(p+off)=v;}
static int force_stereo_downmix(BerryMailbox *mb,uint64_t add){
    uint8_t *work=ap(mb->work_phys,add);
    if(!work || mb->work_len < E302_STEREO_DMIX_OFF + 24u) return 0;
    put32(work,E302_STEREO_DMIX_OFF+0u,1u);
    put32(work,E302_STEREO_DMIX_OFF+4u,2u);
    put64(work,E302_STEREO_DMIX_OFF+8u,3u);
    put64(work,E302_STEREO_DMIX_OFF+16u,0u);
    return 1;
}

__attribute__((used))
void berry_eac3_compat_entry(BerryMailbox *mb,uint64_t alias_add,void *module_base,BerryRet *ret){
    ret->a=ret->b=ret->c=ret->d=0;
    if(!mb){ret->c=90;return;}

    /* One-shot init expected by AC307A library. Translate to E302 stages. */
    if(mb->param0==0){
        BerryMailbox t=*mb; BerryRet r;
        static const uint32_t stages[7]={0,10,11,12,13,14,15};
        t.work_len=CLAIMED_WORK_LEN;
        for(uint32_t i=0;i<7;i++){
            t.param0=stages[i];
            berry_eac3_c_entry(&t,alias_add,module_base,&r);
            if(r.a!=1 || r.c!=0){ret->c=r.c?r.c:901u+i;return;}
        }
        ret->a=1;ret->b=384;return;
    }

    /* AC307A gives a contiguous compressed batch. Parse every E-AC3 syncframe,
       decode with the hardware-proven E302 core, and concatenate stereo PCM. */
    if(mb->param0==3){
        if(!mb->in_len || mb->out_len<OUT_LIMIT){ret->c=920;return;}
        uint8_t *in=ap(mb->in_phys,alias_add);
        uint8_t *out=ap(mb->out_phys,alias_add);
        if(!in||!out){ret->c=921;return;}
        if(!force_stereo_downmix(mb,alias_add)){ret->c=908;return;}
        zerob(out,256);
        uint8_t *scratch=out+SCRATCH_OFF;
        uint32_t in_off=0, pcm_off=0, frames=0, samples_ch=0, rate=0;

        while(in_off<mb->in_len){
            if(frames>=MAX_FRAMES_BATCH){ret->c=922;return;}
            uint32_t fs=eac3_size(in+in_off,mb->in_len-in_off);
            if(!fs){ret->c=930u+frames;return;}

            BerryMailbox t=*mb; BerryRet r;
            t.in_phys=mb->in_phys+in_off;
            t.in_len=fs;
            t.out_phys=mb->out_phys+SCRATCH_OFF;
            t.out_len=mb->out_len-SCRATCH_OFF;
            t.param0=1; t.param1=0; t.work_len=CLAIMED_WORK_LEN;
            berry_eac3_c_entry(&t,alias_add,module_base,&r);
            if(r.a!=1 || r.c!=0){ret->c=r.c?r.c:940u+frames;return;}

            uint32_t consumed=get32(scratch,12);
            uint32_t sr=get32(scratch,16);
            uint32_t inch=get32(scratch,20);
            uint32_t outch=get32(scratch,24);
            uint32_t ns=get32(scratch,28);
            uint32_t pb=get32(scratch,32);
            uint32_t heap=get32(scratch,56);
            uint32_t expected_heap=(inch>2u)?416u:384u;
            if(get32(scratch,0)!=E302_MARK || get32(scratch,4)!=2u || get32(scratch,8)!=0u ||
               consumed!=fs || sr<8000u || sr>192000u || inch<2u || inch>6u || outch!=2u ||
               ns<1u || ns>1536u || pb!=ns*4u || heap!=expected_heap){ret->c=950u+frames;return;}
            if(PCM_HEAD_OFF+pcm_off+pb>SCRATCH_OFF){ret->c=923;return;}
            copyb(out+PCM_HEAD_OFF+pcm_off,scratch+E302_STEREO_OFF,pb);
            if(!rate) rate=sr; else if(rate!=sr){ret->c=924;return;}
            in_off += consumed; pcm_off += pb; samples_ch += ns; frames++;
        }
        if(!frames || in_off!=mb->in_len){ret->c=925;return;}

        put32(out,0,AC305_MARK);
        put32(out,4,AC305_STATUS);
        put32(out,8,rate);
        put32(out,12,2u);
        put32(out,16,0u);
        put32(out,20,0u);
        put32(out,24,frames);
        put32(out,28,samples_ch);
        put32(out,32,pcm_off);
        put32(out,36,in_off);
        /* offsets 40..255 remain zero; offset 72 is AC307A error_stage */
        ret->a=1;ret->b=pcm_off;ret->c=0;ret->d=in_off;return;
    }

    { BerryMailbox t=*mb; t.work_len=CLAIMED_WORK_LEN;
      berry_eac3_c_entry(&t,alias_add,module_base,ret); }
}
