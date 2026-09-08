#include "../core/mr_codec.h"
#include <stdio.h>
#include <string.h>
#define STUB(n) const mr_codec mr_codec_##n={#n,{0},0,0,0,0}
STUB(h264); STUB(cinepak); STUB(mjpeg); STUB(mpeg2); STUB(msvideo1); STUB(rle); STUB(rawvideo);
STUB(wmv1);
STUB(wmv2);
static int expect(const char s[4], const mr_codec *wanted) {
    const mr_codec *got=mr_codec_find(MR_FOURCC(s[0],s[1],s[2],s[3]));
    if(got!=wanted) { fprintf(stderr,"bad route: %.4s\n",s); return 1; } return 0;
}
int main(void) {
    static const char *iso[]={"DIVX","DX50","XVID","xvid","FMP4","MP4V","mp4v","3IV2","3iv2","3IVX","RMP4","BLZ0","SEDG","M4S2","MP4S"};
    static const char *h[]={"H263","h263","I263","i263","U263","u263","T263","X263",
                            "s263","S263"};
    static const char *no[]={"DIV1","DIV3","DIV4","DIV5","DIV6","MP41","MP43","AP41","COL1","COL0"};
    unsigned i; int fail=0;
    for(i=0;i<sizeof iso/sizeof *iso;i++) fail|=expect(iso[i],&mr_codec_mpeg4);
    fail|=expect("DIV2",&mr_codec_msmpeg4v2); fail|=expect("MP42",&mr_codec_msmpeg4v2);
    for(i=0;i<sizeof h/sizeof *h;i++) fail|=expect(h[i],&mr_codec_h263);
    for(i=0;i<sizeof no/sizeof *no;i++) if(mr_codec_find(MR_FOURCC(no[i][0],no[i][1],no[i][2],no[i][3]))) { fprintf(stderr,"unsafe route: %.4s\n",no[i]); fail=1; }
    { /* Header/error-path regression: PSC, QCIF PTYPE, annex and reset. */
      mr_decoder d; uint8_t pkt[8]={0}; mr_status st;
      if (mr_decoder_open(&d,&mr_codec_h263,176,144)!=MR_OK) return 2;
      st=mr_decoder_decode(&d,pkt,1); if(st!=MR_EFORMAT) fail=1;
      /* 22-bit PSC=0x20, TR=0, PTYPE marker+QCIF+P+advanced prediction,
       * quant=1, CPM=0.  Annex F changes reconstruction, so it must be
       * refused as unsupported rather than decoded approximately. */
      { static const uint8_t ap[7]={0,0,128,2,0x0a,0x41,0}; memcpy(pkt,ap,7); }
      st=mr_decoder_decode(&d,pkt,7); if(st!=MR_EUNSUPPORTED) fail=1;
      if(mr_decoder_reset(&d)!=MR_OK) fail=1;
      mr_decoder_close(&d);
    }
    puts(fail?"registry check failed":"codec registry aliases/header errors: ok"); return fail;
}
