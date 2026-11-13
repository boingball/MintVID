/* Minimal math.h for the freestanding BerryCore E-AC3 build. */
#ifndef BERRY_MATH_H
#define BERRY_MATH_H
#define M_E        2.7182818284590452354
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_LOG2_10  3.32192809488736234787
#define M_PHI      1.61803398874989484820
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT1_2  0.70710678118654752440
#define M_SQRT2    1.41421356237309504880
#define NAN        __builtin_nanf("")
#define INFINITY   __builtin_inff()
#define HUGE_VAL   __builtin_huge_val()
#define signbit(x) __builtin_signbit(x)
/* Only functions that FFmpeg's configure does not probe.  The probed ones
   (cbrt, exp2, lrintf, powf, rint, round, ...) come from libavutil/libm.h,
   because this freestanding target has no libm (all HAVE_<func> = 0). */
double sin(double); double cos(double); double tan(double);
double asin(double); double acos(double); double atan(double); double atan2(double, double);
double sinh(double); double cosh(double); double tanh(double);
double exp(double); double log(double); double log10(double);
double pow(double, double); double sqrt(double);
double ceil(double); double floor(double); double fabs(double); double fmod(double, double);
double ldexp(double, int); double frexp(double, int *);
double fmin(double, double); double fmax(double, double);
float sqrtf(float); float ceilf(float); float floorf(float); float fabsf(float);
float fminf(float, float); float fmaxf(float, float);
#endif
