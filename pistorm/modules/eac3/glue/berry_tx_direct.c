#include <stdint.h>
#include <stddef.h>

/* EAC302D diagnostic TX shim.
 * No FFmpeg headers on purpose: this file is independently buildable and
 * mirrors only the stable AVTXContext layout used by FFmpeg 9.0.2 tx_priv.h.
 * The scalar naive inverse MDCT function itself stays FFmpeg code from
 * core_eac302b.o; the linker exports its section start below.
 */

typedef struct BerryTXContext BerryTXContext;
typedef void (*BerryTXFn)(BerryTXContext *s, void *out, void *in, ptrdiff_t stride);

struct BerryTXContext {
    int len;                    /* 0x00 */
    int inv;                    /* 0x04 */
    int *map;                   /* 0x08 */
    void *exp;                  /* 0x10 */
    void *tmp;                  /* 0x18 */
    BerryTXContext *sub;        /* 0x20 */
    BerryTXFn fn[4];            /* 0x28 */
    int nb_sub;                 /* 0x48 */
    int _pad4c;
    const void *cd[4];          /* 0x50 */
    const void *cd_self;        /* 0x70 */
    int type;                   /* 0x78 */
    int _pad7c;
    uint64_t flags;             /* 0x80 */
    int map_dir;                /* 0x88 */
    float scale_f;              /* 0x8c */
    double scale_d;             /* 0x90 */
    void *opaque;               /* 0x98 */
};

_Static_assert(sizeof(BerryTXContext) == 0xA0, "AVTXContext size/layout mismatch");
_Static_assert(offsetof(BerryTXContext, cd_self) == 0x70, "AVTXContext cd_self offset mismatch");
_Static_assert(offsetof(BerryTXContext, scale_f) == 0x8c, "AVTXContext scale_f offset mismatch");
_Static_assert(offsetof(BerryTXContext, scale_d) == 0x90, "AVTXContext scale_d offset mismatch");

extern void *av_mallocz(size_t n);

/* Linker-defined address at the start of FFmpeg's local
 * .text.ff_tx_mdct_naive_inv_int32_c input section.  Referencing this symbol
 * produces a PC-relative ADRP/ADD, therefore it remains correct when the raw
 * module is loaded at an arbitrary Fast-RAM base. */
extern unsigned char __berry_ff_tx_mdct_naive_inv_int32_c[];

int __wrap_av_tx_init(void **ctx_out, void **fn_out, int type, int inv,
                      int len, const void *scale_ptr, uint64_t flags)
{
    if (!ctx_out || !fn_out || !scale_ptr)
        return -22;

    /* EAC302B calls av_tx_init only for inverse int32 MDCT 128 and 256.
     * We deliberately do not touch FFmpeg's static codelet pointer tables. */
    if (!inv || (len != 128 && len != 256))
        return -38;

    BerryTXContext *s = (BerryTXContext *)av_mallocz(sizeof(*s));
    if (!s)
        return -12;

    const float scale = *(const float *)scale_ptr;
    s->len = len;
    s->inv = inv;
    s->type = type;
    s->flags = flags;
    s->scale_f = scale;
    s->scale_d = (double)scale;

    *ctx_out = s;
    *fn_out = (void *)(uintptr_t)__berry_ff_tx_mdct_naive_inv_int32_c;
    return 0;
}

/* Faster scalar cos helper for the diagnostic O(N^2) naive IMDCT.  EAC302B's
 * runtime helper used repeated +/-2*pi subtraction; this constant-time range
 * reduction is the already prepared EAC302C improvement. */
static double berry_wrap_pi(double x)
{
    const double tp = 6.2831853071795864769;
    const double pi = 3.14159265358979323846;
    long long k = (long long)(x / tp);
    x -= (double)k * tp;
    if (x > pi) x -= tp;
    else if (x < -pi) x += tp;
    return x;
}

double __wrap_cos(double x)
{
    const double pi = 3.14159265358979323846;
    const double hp = 1.57079632679489661923;
    double sign = 1.0;
    x = berry_wrap_pi(x);
    if (x > hp) { x = pi - x; sign = -1.0; }
    else if (x < -hp) { x = -pi - x; sign = -1.0; }
    const double x2 = x*x;
    return sign*(1.0 + x2*(-1.0/2.0 + x2*(1.0/24.0 + x2*(-1.0/720.0 +
           x2*(1.0/40320.0 + x2*(-1.0/3628800.0 + x2*(1.0/479001600.0)))))));
}
