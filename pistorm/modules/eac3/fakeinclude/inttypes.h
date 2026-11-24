/* Minimal inttypes.h for the freestanding BerryCore E-AC3 build (LP64). */
#ifndef BERRY_INTTYPES_H
#define BERRY_INTTYPES_H
#include <stdint.h>
#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIi64 "li"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIX32 "X"
#define PRIX64 "lX"
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define SCNd64 "ld"
#endif
