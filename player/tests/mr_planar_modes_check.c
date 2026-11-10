/* Independent composition oracle for experimental fused and shadow modes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/mr_yuv_dither.h"
#include "../core/mr_dither.h"
#include "../core/mr_c2p.h"
#include "../core/mr_scale.h"
#include "../core/mr_yuv_planar_queue.h"
#include "../core/mr_play_options.h"

static unsigned seed = 42;
static unsigned char sample(void) { seed = seed * 1664525u + 1013904223u; return seed >> 24; }
static void check_shape(int w, int h, int dw, int dh)
{
    int pw = (dw + 31) & ~31, cw = (w+1)/2, ch = (h+1)/2, p, i;
    size_t ps = (size_t)(pw/8)*dh, n = ps*5;
    unsigned char *y=malloc(w*h), *u=malloc(cw*ch), *v=malloc(cw*ch);
    unsigned char *idx=calloc(pw,dh), *tmp=malloc(dw*dh), *want=calloc(1,n);
    unsigned char *got=malloc(n+16), *rgb=malloc(w*h*3), *scaled=malloc(dw*dh*3), *planes[8];
    assert(y&&u&&v&&idx&&tmp&&want&&got&&rgb&&scaled);
    for(i=0;i<w*h;i++) y[i]=sample();
    for(i=0;i<cw*ch;i++) { u[i]=sample(); v[i]=sample(); }
    for(i=0;i<w*h*3;i++) rgb[i]=sample();
    for(p=0;p<5;p++) planes[p]=want+p*ps;
    mr_yuv420_dither_indexed_resize(y,w,u,cw,v,cw,w,h,5,tmp,dw,dh,dw,0);
    for(i=0;i<dh;i++) memcpy(idx+i*pw+(pw-dw)/2,tmp+i*dw,dw);
    mr_c2p8(idx,pw,dh,pw,5,planes,pw/8,0,0);
    memset(got,0xa5,n+16);
    mr_yuv420_fused32(y,w,u,cw,v,cw,w,h,got+8,dw,dh,pw,0);
    assert(!memcmp(want,got+8,n));
    for(i=0;i<8;i++) assert(got[i]==0xa5 && got[n+8+i]==0xa5);
    mr_scale_resize_rgb24(rgb,w,h,w*3,scaled,dw,dh,dw*3);
    mr_dither_rgb_indexed(scaled,dw,dh,dw*3,tmp,dw,0,5);
    memset(idx,0,pw*dh); memset(want,0,n);
    for(i=0;i<dh;i++) memcpy(idx+i*pw+(pw-dw)/2,tmp+i*dw,dw);
    mr_c2p8(idx,pw,dh,pw,5,planes,pw/8,0,0);
    mr_rgb_fused32(rgb,w,h,w*3,got+8,dw,dh,pw);
    assert(!memcmp(want,got+8,n));
    free(y);free(u);free(v);free(idx);free(tmp);free(want);free(got);free(rgb);free(scaled);
}
int main(void)
{
    int dims[][4]={{256,144,256,144},{640,360,640,180},{33,17,31,13},
        {17,11,63,29},{1,1,1,1},{320,180,320,200},{192,108,320,180}};
    unsigned char src[19],dst[19],shadow[19]; unsigned i;
    for(i=0;i<sizeof dims/sizeof dims[0];i++) check_shape(dims[i][0],dims[i][1],dims[i][2],dims[i][3]);
    memset(src,0,19);memset(dst,0xff,19);memset(shadow,0,19);
    assert(mr_planar_shadow_copy(src,dst,shadow,19,0)==19);
    assert(!memcmp(dst,src,19));
    assert(mr_planar_shadow_copy(src,dst,shadow,19,1)==0);
    src[5]=2;src[18]=3;
    assert(mr_planar_shadow_copy(src,dst,shadow,19,1)==5);
    assert(!memcmp(dst,src,19)); assert(!memcmp(shadow,src,19));
    assert(mr_yuv_planar_queue_configure32(31,13,32));
    assert(mr_yuv_planar_queue_is_active() && mr_yuv_planar_queue_depth()==5);
    assert(mr_yuv_planar_queue_configure(31,13,32));
    assert(mr_yuv_planar_queue_depth()==8);
    mr_yuv_planar_queue_disable(); assert(!mr_yuv_planar_queue_is_active());
    {
        mr_play_options options, restored;
        char args[4096], error[128];
        char *av[] = {"test", "--c2p", "shadow"};
        mr_play_options_default(&options);
        assert(mr_play_options_parse(&options, 3, av, error, sizeof error));
        assert(options.c2p == MR_C2P_SHADOW);
        options.display = MR_DISPLAY_HAM6;
        assert(mr_build_player_arguments(args, sizeof args, &options, "clip.mp4", NULL, NULL));
        assert(strstr(args, "--shadow") && strstr(args, "--ham6"));
        av[2] = "fused32";
        mr_play_options_default(&restored);
        assert(mr_play_options_parse(&restored, 3, av, error, sizeof error));
        assert(restored.c2p == MR_C2P_FUSED32);
        restored.display = MR_DISPLAY_AGA;
        assert(mr_build_player_arguments(args, sizeof args, &restored, "clip.mp4", NULL, NULL));
        assert(strstr(args, "--fused32"));
    }
    puts("Planar modes: 7 YUV/RGB resize/padding cases, guards, shadow and queue transitions passed");
    return 0;
}
