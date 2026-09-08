/* Microsoft BI_RLE8 palettised bitmap decoder.  BI_RLE4 is deliberately
 * deferred: accepting its subtly different nibble/alignment rules partially
 * would be less safe than rejecting it. */
#include "mr_rle.h"
#include <stdlib.h>
#include <string.h>
typedef struct {int w,h,stride;uint8_t*fb;uint8_t pal[768];} rle_ctx;
static mr_status rle_open(mr_decoder*d){rle_ctx*c=(rle_ctx*)calloc(1,sizeof(*c));uint32_t n;
 if(!c)return MR_ENOMEM;
 c->w=d->width;c->h=d->height;c->stride=c->w*3;
 if(d->config_len<2||mr_rl16(d->config)!=8){free(c);return MR_EUNSUPPORTED;}
 n=d->config_len-2;if(n>768)n=768;memcpy(c->pal,d->config+2,n);
 c->fb=(uint8_t*)calloc((size_t)c->stride,c->h);if(!c->fb){free(c);return MR_ENOMEM;}
 d->priv=c;d->frame.data=c->fb;d->frame.width=c->w;d->frame.height=c->h;
 d->frame.stride=c->stride;d->frame.fmt=MR_PIX_RGB24;return MR_OK;}
static void put(rle_ctx*c,int x,int y,uint8_t i){memcpy(c->fb+(size_t)y*c->stride+x*3,c->pal+i*3,3);}
/* Running out of data is a normal end of frame, not a failure: plenty of
 * encoders omit the end-of-bitmap escape, and AVI's zero-length chunk (an
 * unchanged frame) arrives here as an empty packet. The reference decoder
 * warns and keeps the frame in both cases, so anything already painted stands
 * and the dirty span is reported on every success path - returning an error
 * instead would abort playback of a perfectly good file. Genuinely
 * out-of-range runs and truncated copies stay errors. */
static mr_status rle_decode(mr_decoder*d,const uint8_t*p,uint32_t len){rle_ctx*c=d->priv;const uint8_t*e=p+len;int x=0,y=c->h-1,lo=c->h,hi=0;
 if(!p)return MR_EFORMAT;
 while(p<e){int count,val,i;if(e-p<2)break;count=*p++;val=*p++;
  if(count){if(y<0||x<0||count>c->w-x)return MR_EFORMAT;for(i=0;i<count;i++)put(c,x++,y,(uint8_t)val);if(y<lo)lo=y;if(y+1>hi)hi=y+1;}
  else if(val==0){x=0;y--;}
  else if(val==1)break;                                  /* end of bitmap  */
  else if(val==2){int dx,dy;if(e-p<2)break;dx=*p++;dy=*p++;if(dx>c->w-x||dy>y)return MR_EFORMAT;x+=dx;y-=dy;}
  else {count=val;if(y<0||x<0||count>c->w-x||e-p<count)return MR_EFORMAT;for(i=0;i<count;i++)put(c,x++,y,*p++);if((count&1)&&p<e)p++;if(y<lo)lo=y;if(y+1>hi)hi=y+1;}
 }
 d->frame.dirty_y0=lo;d->frame.dirty_y1=hi;return MR_OK;}
static void rle_close(mr_decoder*d){rle_ctx*c=d->priv;if(c){free(c->fb);free(c);}d->priv=NULL;}
const mr_codec mr_codec_rle={"Microsoft RLE 8",{1,MR_FOURCC('R','L','E','8'),MR_FOURCC('M','R','L','E'),MR_FOURCC('r','l','e','8'),0},rle_open,rle_decode,rle_close,NULL};
