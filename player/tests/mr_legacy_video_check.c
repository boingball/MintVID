#include "../core/mr_msvideo1.h"
#include "../core/mr_dither.h"
#include "../core/mr_rle.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; } } while(0)
static mr_status start(mr_decoder*d,const mr_codec*c,int w,int h,const uint8_t*cfg,uint32_t n)
{ memset(d,0,sizeof(*d));d->codec=c;d->width=w;d->height=h;d->config=cfg;d->config_len=n;return c->open(d); }
static int eq(const uint8_t *p,int stride,int x,int y,int r,int g,int b)
{p+=y*stride+x*3;return p[0]==r&&p[1]==g&&p[2]==b;}

/* MSVideo1 is bottom-up in both dimensions: the bottom row of 4x4 blocks is
 * coded first, and inside a block the bottom pixel row comes first. The
 * per-pixel selector bit is also inverted - a *set* bit picks the
 * lower-numbered colour of its pair. These vectors pin both, plus the two
 * different ways an 8-colour block is signalled in 8- and 16-bit mode. */
static void msvideo(void)
{
 uint8_t cfg16[]={16,0},cfg8[2+768]={8,0};mr_decoder d;mr_status s;int x,y;
 const uint8_t solid[]={0xe0,0x83};        /* 1-colour RGB555 green         */
 const uint8_t solid_red[]={0x00,0xfc};    /* 1-colour RGB555 red           */
 const uint8_t skip[]={1,0x84};            /* skip run covering one block   */
 /* 2-colour. flags=0x000f: the four set bits are the first pixel row coded,
  * i.e. the block's *bottom* row, and being set they select colours[0]. */
 const uint8_t two[]={0x0f,0x00, 0x00,0x7c, 0x1f,0x00};
 /* 16-bit 8-colour, flagged by bit 15 of the *first* colour word rather than
  * by the header byte. flags=0, so every pixel takes its quadrant's odd
  * colour: green / white on the bottom half, blue / red on the top half. */
 const uint8_t eight16[]={0x00,0x00, 0x00,0xfc, 0xe0,0x03, 0x1f,0x00,
                          0xff,0x7f, 0x00,0x7c, 0x1f,0x00, 0xe0,0x03,
                          0x00,0x7c};
 /* 8-bit mode signals 8 colours with a header byte >= 0x90 instead, and this
  * block is the case that used to be decoded as a flat one - leaving its eight
  * palette bytes in the stream and desynchronising everything after it. */
 const uint8_t eight8[]={0x00,0x90, 0,1,0,2,0,3,0,1};
 const uint8_t one8[]={0x01,0x80};             /* 8-bit flat, palette red   */
 const uint8_t two8[]={0x0f,0x00, 0x01,0x03};  /* 8-bit 2-colour red/blue   */
 cfg8[2+1*3]=255;cfg8[2+2*3+1]=255;cfg8[2+3*3+2]=255;  /* 1=R 2=G 3=B       */

 /* --- 16-bit, single 4x4 block --------------------------------------- */
 CHECK(start(&d,&mr_codec_msvideo1,4,4,cfg16,sizeof(cfg16))==MR_OK);
 s=d.codec->decode(&d,solid,sizeof(solid));CHECK(s==MR_OK);
 for(y=0;y<4;y++)for(x=0;x<4;x++)CHECK(eq(d.frame.data,d.frame.stride,x,y,0,255,0));

 /* A skip run leaves the previous frame in place and reports no dirty rows. */
 s=d.codec->decode(&d,skip,sizeof(skip));CHECK(s==MR_OK);
 CHECK(eq(d.frame.data,d.frame.stride,2,2,0,255,0));
 CHECK(d.frame.dirty_y1<=d.frame.dirty_y0);

 /* Set bits land on the bottom row and select colours[0] (red). */
 s=d.codec->decode(&d,two,sizeof(two));CHECK(s==MR_OK);
 for(x=0;x<4;x++){
   CHECK(eq(d.frame.data,d.frame.stride,x,3,255,0,0));
   for(y=0;y<3;y++)CHECK(eq(d.frame.data,d.frame.stride,x,y,0,0,255));
 }

 s=d.codec->decode(&d,eight16,sizeof(eight16));CHECK(s==MR_OK);
 for(y=2;y<4;y++){
   CHECK(eq(d.frame.data,d.frame.stride,0,y,0,255,0));      /* cols[1] green */
   CHECK(eq(d.frame.data,d.frame.stride,1,y,0,255,0));
   CHECK(eq(d.frame.data,d.frame.stride,2,y,255,255,255));  /* cols[3] white */
   CHECK(eq(d.frame.data,d.frame.stride,3,y,255,255,255));
 }
 for(y=0;y<2;y++){
   CHECK(eq(d.frame.data,d.frame.stride,0,y,0,0,255));      /* cols[5] blue  */
   CHECK(eq(d.frame.data,d.frame.stride,1,y,0,0,255));
   CHECK(eq(d.frame.data,d.frame.stride,2,y,255,0,0));      /* cols[7] red   */
   CHECK(eq(d.frame.data,d.frame.stride,3,y,255,0,0));
 }

 /* A block truncated by a short packet keeps what was already decoded rather
  * than failing the whole stream, matching the reference decoder. */
 s=d.codec->decode(&d,two,5);CHECK(s==MR_OK);
 CHECK(d.frame.dirty_y1<=d.frame.dirty_y0);
 d.codec->close(&d);

 /* --- block order: the first coded block is the frame's bottom one ---- */
 CHECK(start(&d,&mr_codec_msvideo1,4,8,cfg16,sizeof(cfg16))==MR_OK);
 {const uint8_t two_blocks[]={0xe0,0x83, 0x00,0xfc};
  s=d.codec->decode(&d,two_blocks,sizeof(two_blocks));CHECK(s==MR_OK);
  for(y=4;y<8;y++)CHECK(eq(d.frame.data,d.frame.stride,0,y,0,255,0));
  for(y=0;y<4;y++)CHECK(eq(d.frame.data,d.frame.stride,0,y,255,0,0));}
 d.codec->close(&d);

 /* --- a partial block at the right edge is not coded, and not written -- */
 CHECK(start(&d,&mr_codec_msvideo1,6,4,cfg16,sizeof(cfg16))==MR_OK);
 s=d.codec->decode(&d,solid_red,sizeof(solid_red));CHECK(s==MR_OK);
 CHECK(eq(d.frame.data,d.frame.stride,3,0,255,0,0));
 CHECK(eq(d.frame.data,d.frame.stride,4,0,0,0,0));
 CHECK(eq(d.frame.data,d.frame.stride,5,0,0,0,0));
 d.codec->close(&d);

 /* --- 8-bit paletted ------------------------------------------------- */
 CHECK(start(&d,&mr_codec_msvideo1,4,4,cfg8,sizeof(cfg8))==MR_OK);
 s=d.codec->decode(&d,one8,sizeof(one8));CHECK(s==MR_OK);
 CHECK(eq(d.frame.data,d.frame.stride,0,0,255,0,0));
 CHECK(eq(d.frame.data,d.frame.stride,3,3,255,0,0));
 s=d.codec->decode(&d,two8,sizeof(two8));CHECK(s==MR_OK);
 for(x=0;x<4;x++){
   CHECK(eq(d.frame.data,d.frame.stride,x,3,255,0,0));
   for(y=0;y<3;y++)CHECK(eq(d.frame.data,d.frame.stride,x,y,0,0,255));
 }
 s=d.codec->decode(&d,eight8,sizeof(eight8));CHECK(s==MR_OK);
 for(y=2;y<4;y++){
   CHECK(eq(d.frame.data,d.frame.stride,0,y,255,0,0));      /* cols[1]=1 R  */
   CHECK(eq(d.frame.data,d.frame.stride,2,y,0,255,0));      /* cols[3]=2 G  */
 }
 CHECK(eq(d.frame.data,d.frame.stride,0,1,0,0,255));        /* cols[5]=3 B  */
 CHECK(eq(d.frame.data,d.frame.stride,2,1,255,0,0));        /* cols[7]=1 R  */
 CHECK(eq(d.frame.data,d.frame.stride,0,0,0,0,0));          /* cols[4]=0 K  */
 CHECK(eq(d.frame.data,d.frame.stride,1,0,0,0,255));        /* cols[5]=3 B  */
 CHECK(eq(d.frame.data,d.frame.stride,2,0,255,0,0));        /* cols[7]=1 R  */
 CHECK(eq(d.frame.data,d.frame.stride,3,0,0,0,0));          /* cols[6]=0 K  */
 d.codec->close(&d);

 /* The direct INDEX8 path must be byte-identical to decoding RGB24 and then
  * applying the established Bayer converter. Exercise both source formats
  * and every native indexed depth used by AGA. */
 {const int depths[]={4,5,8};unsigned di;
  for(di=0;di<sizeof(depths)/sizeof(depths[0]);di++){
   mr_decoder rgb,indexed;const uint8_t *packet;uint32_t packet_len;
   const uint8_t *config;uint32_t config_len;
   if(di&1){packet=eight8;packet_len=sizeof(eight8);
            config=cfg8;config_len=sizeof(cfg8);}
   else {packet=eight16;packet_len=sizeof(eight16);
         config=cfg16;config_len=sizeof(cfg16);}
   CHECK(start(&rgb,&mr_codec_msvideo1,4,4,config,config_len)==MR_OK);
   CHECK(start(&indexed,&mr_codec_msvideo1,4,4,config,config_len)==MR_OK);
   CHECK(mr_msvideo1_set_indexed_output(&indexed,depths[di]));
   CHECK(indexed.frame.fmt==MR_PIX_INDEX8&&indexed.frame.stride==4);
   CHECK(rgb.codec->decode(&rgb,packet,packet_len)==MR_OK);
   CHECK(indexed.codec->decode(&indexed,packet,packet_len)==MR_OK);
   for(y=0;y<4;y++)for(x=0;x<4;x++){
    const uint8_t *p=rgb.frame.data+y*rgb.frame.stride+x*3;
    uint8_t expected=mr_dither_rgb_indexed_pixel(
        p[0],p[1],p[2],x,y,depths[di]);
    CHECK(indexed.frame.data[y*indexed.frame.stride+x]==expected);
   }
   CHECK(!mr_msvideo1_set_indexed_output(&indexed,depths[di]));
   rgb.codec->close(&rgb);indexed.codec->close(&indexed);
  }
 }

 /* Frames smaller than one block carry no codable data at all. */
 CHECK(start(&d,&mr_codec_msvideo1,4,3,cfg16,sizeof(cfg16))==MR_EUNSUPPORTED);
}
static void rle(void)
{
 uint8_t cfg[2+768]={8,0};mr_decoder d;mr_status s;
 /* red, green, blue */cfg[5]=255;cfg[2+2*3+1]=255;cfg[2+3*3+2]=255;
 CHECK(start(&d,&mr_codec_rle,5,3,cfg,sizeof(cfg))==MR_OK);
 {const uint8_t p[]={3,1,0,0, 0,3,2,3,1,0, 0,0, 0,2,1,0, 2,1,0,1};
  uint8_t guarded[sizeof(p)+2];guarded[0]=0xa5;memcpy(guarded+1,p,sizeof(p));guarded[sizeof(p)+1]=0x5a;
  s=d.codec->decode(&d,guarded+1,sizeof(p));CHECK(s==MR_OK);CHECK(guarded[0]==0xa5&&guarded[sizeof(p)+1]==0x5a);
  CHECK(eq(d.frame.data,d.frame.stride,0,2,255,0,0));CHECK(eq(d.frame.data,d.frame.stride,0,1,0,255,0));
  CHECK(eq(d.frame.data,d.frame.stride,1,1,0,0,255));CHECK(eq(d.frame.data,d.frame.stride,1,0,255,0,0));
 }
 {const uint8_t bad[]={6,1,0,1};s=d.codec->decode(&d,bad,sizeof(bad));CHECK(s==MR_EFORMAT);}
 /* Running out of data is a normal end of frame, not a failure. An AVI
  * zero-length chunk (an unchanged frame) arrives as an empty packet, and
  * plenty of encoders omit the end-of-bitmap escape; treating either as an
  * error aborted playback of files the reference decoder handles fine. */
 {const uint8_t solid[]={5,1,0,0,5,1,0,1};
  s=d.codec->decode(&d,solid,sizeof(solid));CHECK(s==MR_OK);
  CHECK(eq(d.frame.data,d.frame.stride,0,2,255,0,0));
  s=d.codec->decode(&d,solid,0);CHECK(s==MR_OK);            /* empty packet */
  CHECK(d.frame.dirty_y1<=d.frame.dirty_y0);
  CHECK(eq(d.frame.data,d.frame.stride,0,2,255,0,0));        /* kept        */
  s=d.codec->decode(&d,solid,6);CHECK(s==MR_OK);   /* no end-of-bitmap code */
  CHECK(eq(d.frame.data,d.frame.stride,0,2,255,0,0));
  {const uint8_t eol[]={0,0};s=d.codec->decode(&d,eol,sizeof(eol));CHECK(s==MR_OK);}}
 d.codec->close(&d);
}
int main(void){msvideo();rle();if(failures)return 1;puts("legacy video decoder checks passed");return 0;}
