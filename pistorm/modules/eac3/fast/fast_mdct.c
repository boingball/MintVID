#include <stdint.h>
#include <stddef.h>
#include "berry_cos_q31.h"

typedef struct BerryTXContext BerryTXContext;
struct BerryTXContext {
    int len;
    int inv;
    int *map;
    void *exp;
    void *tmp;
    BerryTXContext *sub;
    void *fn[4];
    int nb_sub;
    int _pad4c;
    const void *cd[4];
    const void *cd_self;
    int type;
    int _pad7c;
    uint64_t flags;
    int map_dir;
    float scale_f;
    double scale_d;
    void *opaque;
};

static inline int32_t clip32_s128(__int128 x) {
    if (x > (__int128)2147483647) return 2147483647;
    if (x < -((__int128)2147483648)) return (int32_t)0x80000000u;
    return (int32_t)x;
}

static inline int32_t round_q62_to_q31(__int128 x) {
    const __int128 r = ((__int128)1 << 30);
    __int128 q;
    if (x >= 0) q = (x + r) >> 31;
    else q = -(((-x) + r) >> 31);
    return clip32_s128(q);
}

__attribute__((used,noinline,section(".text.fastmdct")))
void berry_fast_mdct_int32(BerryTXContext *s, void *out_v, void *in_v, ptrdiff_t stride)
{
    int32_t *dst = (int32_t *)out_v;
    int32_t *src = (int32_t *)in_v;
    const int N = s->len;
    if ((N != 128 && N != 256) || !s->inv) return;
    const int half = N >> 1;
    const int phase_step = 256 / N;
    const ptrdiff_t ss = stride / (ptrdiff_t)sizeof(int32_t);
    for (int i = 0; i < half; ++i) {
        unsigned idx_d = (unsigned)((2*N - 2*i - 1) * phase_step) & 2047u;
        unsigned idx_u = (unsigned)((3*N + 2*i + 1) * phase_step) & 2047u;
        const unsigned add_d = (idx_d << 1) & 2047u;
        const unsigned add_u = (idx_u << 1) & 2047u;
        __int128 sum_d = 0, sum_u = 0;
        for (int j = 0; j < N; ++j) {
            const int32_t v = src[(ptrdiff_t)j * ss];
            sum_d += (int64_t)v * (int64_t)berry_cos_q31[idx_d];
            sum_u += (int64_t)v * (int64_t)berry_cos_q31[idx_u];
            idx_d = (idx_d + add_d) & 2047u;
            idx_u = (idx_u + add_u) & 2047u;
        }
        dst[i] = round_q62_to_q31(sum_d);
        dst[i + half] = round_q62_to_q31(-sum_u);
    }
}
