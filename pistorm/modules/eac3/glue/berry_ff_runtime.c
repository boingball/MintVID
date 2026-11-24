#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "libavutil/channel_layout.h"
#include "libavutil/downmix_info.h"
#include "libavutil/lfg.h"
#include "libavutil/crc.h"
#include "libavutil/frame.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/decode.h"

typedef struct BerryRuntime {
    uint32_t magic;
    uint8_t *heap;
    size_t heap_size;
    size_t heap_off;
    int16_t *plane0;
    int16_t *plane1;
    int max_samples;
    AVDownmixInfo downmix_info;
    volatile uint32_t *diag_word;
} BerryRuntime;

static BerryRuntime g_rt __attribute__((section(".data"))) = { .magic = 0x42525432u };

void berry_ff_heap_bind(void *base, size_t bytes) {
    g_rt.heap=(uint8_t*)base; g_rt.heap_size=bytes; g_rt.heap_off=0;
}
size_t berry_ff_heap_used(void) { return g_rt.heap_off; }
void berry_ff_set_planes(int16_t *p0, int16_t *p1, int max_samples) {
    g_rt.plane0=p0; g_rt.plane1=p1; g_rt.max_samples=max_samples;
}
void berry_ff_diag_bind(volatile uint32_t *p){ g_rt.diag_word=p; }

static size_t align32(size_t n){ return (n+31u)&~(size_t)31u; }
void *av_malloc(size_t n) {
    size_t a=align32(n?n:1), off=align32(g_rt.heap_off);
    if(!g_rt.heap || off > g_rt.heap_size || a > g_rt.heap_size-off) return 0;
    void *p=g_rt.heap+off; g_rt.heap_off=off+a; return p;
}
void *av_mallocz(size_t n){ void *p=av_malloc(n); if(p) memset(p,0,n); return p; }
void *av_malloc_array(size_t nmemb,size_t size){ if(size && nmemb > (size_t)-1/size) return 0; return av_malloc(nmemb*size); }
void av_free(void *p){ (void)p; }
void av_freep(void *arg){ void **p=(void**)arg; if(p) *p=0; }
void *av_fast_realloc(void *ptr, unsigned int *size, size_t min_size){
    if(ptr && size && *size>=min_size) return ptr;
    size_t old=size?*size:0; size_t ns=min_size + min_size/16 + 32; void *p=av_malloc(ns);
    if(!p) return 0; if(ptr && old) memcpy(p,ptr,old<min_size?old:min_size); if(size)*size=(unsigned int)ns; return p;
}

void *memset(void *dst,int c,size_t n){ uint8_t*d=dst; while(n--)*d++=(uint8_t)c; return dst; }
void *memcpy(void *dst,const void *src,size_t n){ uint8_t*d=dst; const uint8_t*s=src; while(n--)*d++=*s++; return dst; }
void *memmove(void *dst,const void *src,size_t n){ uint8_t*d=dst; const uint8_t*s=src; if(d<s){while(n--)*d++=*s++;}else{d+=n;s+=n;while(n--)*--d=*--s;}return dst;}
int memcmp(const void*a,const void*b,size_t n){const uint8_t*x=a,*y=b;while(n--){if(*x!=*y)return *x-*y;x++;y++;}return 0;}
void *memchr(const void*s,int c,size_t n){const uint8_t*p=s;while(n--){if(*p==(uint8_t)c)return(void*)p;p++;}return 0;}
int abs(int x){ return x<0?-x:x; }
void abort(void){ if(g_rt.diag_word)*g_rt.diag_word=0xAB0A0001u; for(;;){} }

int64_t av_gcd(int64_t a,int64_t b){ if(a<0)a=-a;if(b<0)b=-b; while(b){int64_t t=a%b;a=b;b=t;} return a; }
int av_get_cpu_flags(void){ return 0; }
void av_log(void *avcl,int level,const char *fmt,...){ (void)avcl;(void)level;(void)fmt; }
void avpriv_request_sample(void *avc,const char *msg,...){ (void)avc;(void)msg; }

static int pop64(uint64_t x){ int n=0; while(x){ n+=(int)(x&1u); x>>=1; } return n; }
int av_channel_layout_from_mask(AVChannelLayout *cl,uint64_t mask){ if(!cl||!mask)return -22; memset(cl,0,sizeof(*cl)); cl->order=AV_CHANNEL_ORDER_NATIVE; cl->nb_channels=pop64(mask); cl->u.mask=mask; return 0; }
void av_channel_layout_uninit(AVChannelLayout *cl){ if(cl) memset(cl,0,sizeof(*cl)); }
int av_channel_layout_compare(const AVChannelLayout *a,const AVChannelLayout*b){ if(!a||!b)return -22; if(a->order!=b->order||a->nb_channels!=b->nb_channels)return 1; if(a->order==AV_CHANNEL_ORDER_NATIVE)return a->u.mask==b->u.mask?0:1; return 0; }
int av_channel_layout_index_from_channel(const AVChannelLayout *cl,enum AVChannel ch){ if(!cl||cl->order!=AV_CHANNEL_ORDER_NATIVE||ch<0||ch>=64)return -22; uint64_t bit=1ull<<(unsigned)ch; if(!(cl->u.mask&bit))return -2; return pop64(cl->u.mask&(bit-1)); }

void av_lfg_init(AVLFG *c,unsigned int seed){ c->index=0; uint32_t x=seed^0x9e3779b9u; for(int i=0;i<64;i++){ x=x*1664525u+1013904223u+(uint32_t)i; c->state[i]=x; } }
int av_lfg_init_from_data(AVLFG *c,const uint8_t *data,unsigned int len){ uint32_t h=2166136261u; for(unsigned i=0;i<len;i++){h^=data[i];h*=16777619u;} av_lfg_init(c,h); return 0; }

AVDownmixInfo *av_downmix_info_update_side_data(AVFrame *frame){ (void)frame; memset(&g_rt.downmix_info,0,sizeof(g_rt.downmix_info)); return &g_rt.downmix_info; }
int ff_side_data_update_matrix_encoding(AVFrame *frame, enum AVMatrixEncoding matrix_encoding){ (void)frame;(void)matrix_encoding; return 0; }

int ff_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags){
    (void)flags; if(!avctx||!frame||avctx->ch_layout.nb_channels<1||avctx->ch_layout.nb_channels>2) return -22;
    if(frame->nb_samples<1||frame->nb_samples>g_rt.max_samples||!g_rt.plane0) return -12;
    memset(frame->data,0,sizeof(frame->data)); frame->extended_data=frame->data;
    frame->data[0]=(uint8_t*)g_rt.plane0;
    if(avctx->ch_layout.nb_channels>1){ if(!g_rt.plane1)return -12; frame->data[1]=(uint8_t*)g_rt.plane1; }
    return 0;
}

/* Small scalar-FP helpers used only by FFmpeg table/init or rare E-AC-3 SPX paths. */
double fabs(double x){ return x<0?-x:x; }
double floor(double x){ long long i=(long long)x; if((double)i>x)i--; return (double)i; }
double ceil(double x){ long long i=(long long)x; if((double)i<x)i++; return (double)i; }
double sqrt(double x){ if(x<=0)return 0; double y=x>1?x:1; for(int i=0;i<12;i++) y=0.5*(y+x/y); return y; }
float sqrtf(float x){ if(x<=0)return 0; float y=x>1?x:1; for(int i=0;i<8;i++) y=0.5f*(y+x/y); return y; }
static double wrap_pi(double x){ const double tp=6.2831853071795864769, pi=3.14159265358979323846; while(x>pi)x-=tp;while(x<-pi)x+=tp;return x; }
double sin(double x){ x=wrap_pi(x); double x2=x*x; return x*(1.0+x2*(-1.0/6.0+x2*(1.0/120.0+x2*(-1.0/5040.0+x2*(1.0/362880.0+x2*(-1.0/39916800.0)))))); }
double cos(double x){ x=wrap_pi(x); double x2=x*x; return 1.0+x2*(-1.0/2.0+x2*(1.0/24.0+x2*(-1.0/720.0+x2*(1.0/40320.0+x2*(-1.0/3628800.0))))); }
double pow(double a,double b){ if(a==2.0){ int n=(int)b; double f=b-n; double r=1.0+f*(0.6931471805599453+f*(0.2402265069591007+f*0.05550410866482158)); if(n>=0)while(n--)r*=2.0;else while(n++)r*=0.5; return r;} return 1.0; }
