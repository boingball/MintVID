#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int16_t clamp(int64_t x){return x>32767?32767:x<-32768?-32768:(int16_t)x;}
static void ref(const int64_t*u,int16_t*o,int st){for(int i=0;i<32;i++)o[i*st]=clamp(u[i]/-66562);}
extern void scale_clamp_m68k(const int64_t*,int16_t*,int);
int main(void){int64_t u[32];int16_t a[64],b[64];uint32_t x=1;int64_t v[]={0,1,-1,66561,66562,66563,-66561,-66562,-66563,32767LL*66562,32768LL*66562,-32767LL*66562,-32768LL*66562,INT64_MAX,INT64_MIN};for(int t=0;t<10000;t++){for(int i=0;i<32;i++){if(t<16)u[i]=v[t];else{x=x*1664525u+1013904223u;u[i]=(int64_t)(int32_t)x*1000000;}}for(int st=1;st<=2;st++){memset(a,0,sizeof a);memset(b,0,sizeof b);ref(u,a,st);scale_clamp_m68k(u,b,st);for(int i=0;i<32;i++)if(a[i*st]!=b[i*st]){printf("FAIL t=%d i=%d st=%d got=%d exp=%d\\n",t,i,st,b[i*st],a[i*st]);return 1;}}}puts("MP2 scale/clamp: exact boundary and randomized comparisons passed");}
