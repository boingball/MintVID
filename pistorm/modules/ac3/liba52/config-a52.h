#ifndef BERRYAC3_CONFIG_A52_H
#define BERRYAC3_CONFIG_A52_H
#include <stdint.h>
#define LIBA52_FIXED 1
#define IBSS_ATTR
#define IDATA_ATTR
#define ICONST_ATTR
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define betoh32(x) ((uint32_t)(x))
#elif defined(__GNUC__) || defined(__clang__)
#define betoh32(x) __builtin_bswap32((uint32_t)(x))
#else
#define betoh32(x) ((((uint32_t)(x)&0xffu)<<24)|(((uint32_t)(x)&0xff00u)<<8)|(((uint32_t)(x)&0xff0000u)>>8)|(((uint32_t)(x)>>24)&0xffu))
#endif
#endif
