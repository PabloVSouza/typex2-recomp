/* Stand-in for <math.h> when the launcher compiles generated code with its own
 * clang and no system SDK (-nostdinc; launcher/src-tauri/src/recomp.rs). The
 * functions resolve at load time against the C library x2-player has loaded.
 * Only what recomp.h and generated code use. */
#ifndef X2_SDKLESS_MATH_H
#define X2_SDKLESS_MATH_H

#define NAN __builtin_nanf("")
#define INFINITY __builtin_inff()
#define HUGE_VAL __builtin_huge_val()
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, (x))
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)

double fabs(double), sqrt(double), trunc(double), floor(double), ceil(double), round(double);
double nearbyint(double), rint(double);
double sin(double), cos(double), tan(double), atan(double), atan2(double, double);
double exp(double), exp2(double), log(double), log2(double), log10(double), pow(double, double);
double fmod(double, double), remainder(double, double), ldexp(double, int), frexp(double, int *);
double copysign(double, double), modf(double, double *);
float fabsf(float), sqrtf(float), truncf(float), floorf(float), ceilf(float), roundf(float);
float sinf(float), cosf(float);
long lrint(double);
long long llrint(double);

#endif
