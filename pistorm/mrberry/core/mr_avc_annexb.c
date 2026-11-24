#include "mr_avc_annexb.h"
#include <stdlib.h>
#include <string.h>
static int reserve(mr_avc_annexb *a, uint32_t need){
    uint32_t cap; uint8_t *p;
    if(a->cap>=need) return 1;
    cap=a->cap?a->cap:4096u; while(cap<need){ if(cap>0x40000000u) return 0; cap*=2; }
    p=(uint8_t*)realloc(a->buf,cap); if(!p) return 0; a->buf=p; a->cap=cap; return 1;
}
void mr_avc_annexb_init(mr_avc_annexb *a){ if(a){a->buf=0;a->cap=0;a->nal_length_size=4;} }
void mr_avc_annexb_close(mr_avc_annexb *a){ if(a){free(a->buf);a->buf=0;a->cap=0;a->nal_length_size=4;} }
static void sc(uint8_t *p){p[0]=0;p[1]=0;p[2]=0;p[3]=1;}
mr_status mr_avc_config_to_annexb(mr_avc_annexb *a,const uint8_t *c,uint32_t n,const uint8_t **out,uint32_t *out_n){
    uint32_t i=6,o=0,j,l; unsigned ns,np;
    if(!a||!c||n<7||!out||!out_n||c[0]!=1) return MR_EFORMAT;
    a->nal_length_size=(uint8_t)((c[4]&3u)+1u); ns=c[5]&31u;
    for(j=0;j<ns;j++){ if(i+2>n)return MR_EFORMAT; l=((uint32_t)c[i]<<8)|c[i+1]; i+=2; if(!l||i+l>n)return MR_EFORMAT; if(!reserve(a,o+4+l))return MR_ENOMEM; sc(a->buf+o);o+=4;memcpy(a->buf+o,c+i,l);o+=l;i+=l; }
    if(i>=n)return MR_EFORMAT; np=c[i++];
    for(j=0;j<np;j++){ if(i+2>n)return MR_EFORMAT; l=((uint32_t)c[i]<<8)|c[i+1]; i+=2; if(!l||i+l>n)return MR_EFORMAT; if(!reserve(a,o+4+l))return MR_ENOMEM; sc(a->buf+o);o+=4;memcpy(a->buf+o,c+i,l);o+=l;i+=l; }
    if(!o)return MR_EFORMAT; *out=a->buf;*out_n=o;return MR_OK;
}
mr_status mr_avc_sample_to_annexb(mr_avc_annexb *a,const uint8_t *s,uint32_t n,const uint8_t **out,uint32_t *out_n){
    uint32_t i=0,o=0,l,j; unsigned ls;
    if(!a||!s||!n||!out||!out_n)return MR_EFORMAT; ls=a->nal_length_size; if(ls<1||ls>4)return MR_EFORMAT;
    if(!reserve(a,n+1024u))return MR_ENOMEM;
    while(i<n){ if(i+ls>n)return MR_EFORMAT; l=0; for(j=0;j<ls;j++)l=(l<<8)|s[i++]; if(!l||i+l>n)return MR_EFORMAT; if(!reserve(a,o+4+l))return MR_ENOMEM; sc(a->buf+o);o+=4; memcpy(a->buf+o,s+i,l);o+=l;i+=l; }
    *out=a->buf;*out_n=o;return MR_OK;
}
static int next_nal(const uint8_t *s,uint32_t n,uint32_t *io,uint32_t *start,uint32_t *end){
    uint32_t i=*io, p, q;
    while(i+3<n && !(s[i]==0&&s[i+1]==0&&((s[i+2]==1)||(i+3<n&&s[i+2]==0&&s[i+3]==1)))) i++;
    if(i+3>=n)return 0; p=i+(s[i+2]==1?3:4); q=p;
    while(q+3<n && !(s[q]==0&&s[q+1]==0&&((s[q+2]==1)||(q+3<n&&s[q+2]==0&&s[q+3]==1)))) q++;
    *start=p;*end=(q+3<n)?q:n;*io=*end;return p<*end;
}
mr_status mr_avc_annexb_extract_parameter_sets(const uint8_t *s,uint32_t n,uint8_t *d,uint32_t cap,uint32_t *out_n){
    uint32_t io=0,a,b,o=0; int hs=0,hp=0;
    if(!s||!d||!out_n)return MR_EFORMAT;
    while(next_nal(s,n,&io,&a,&b)){ uint8_t t=s[a]&31u; if(t==7||t==8){ uint32_t l=b-a; if(o+4+l>cap)return MR_ENOMEM; sc(d+o);o+=4;memcpy(d+o,s+a,l);o+=l; if(t==7)hs=1;else hp=1; } }
    if(!hs||!hp)return MR_EAGAIN; *out_n=o;return MR_OK;
}
int mr_avc_annexb_is_keyframe(const uint8_t *s,uint32_t n){ uint32_t io=0,a,b; if(!s)return 0; while(next_nal(s,n,&io,&a,&b)) if((s[a]&31u)==5) return 1; return 0; }
