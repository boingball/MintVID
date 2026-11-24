/* Minimal assert.h for the freestanding BerryCore E-AC3 build. */
#ifndef BERRY_ASSERT_H
#define BERRY_ASSERT_H
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
void abort(void) __attribute__((noreturn));
#define assert(x) ((x) ? (void)0 : abort())
#endif
#endif
