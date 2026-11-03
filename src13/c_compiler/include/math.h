#ifndef _MATH_H
#define _MATH_H

#ifdef __cplusplus
extern "C" {
#endif

/* libm do SO (lib/math.c, dentro do LIBC.DLL). long double = double. */

#ifdef __GNUC__
#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#else
#define HUGE_VAL (1e300 * 1e300)
#define INFINITY ((float)(1e300 * 1e300))
#define NAN ((float)(0.0 / 0.0))
#endif
#define HUGE_VALF ((float)HUGE_VAL)
#define HUGE_VALL HUGE_VAL

#define M_E 2.7182818284590452354
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_1_PI 0.31830988618379067154
#define M_2_PI 0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

int __fpclassify(double x);
int __signbit(double x);
/* genericos: valem para float e double (NaN e o unico x != x) */
#define fpclassify(x) __fpclassify((double)(x))
#define signbit(x) __signbit((double)(x))
#define isnan(x) ((x) != (x))
#define isinf(x) ((x) == (x) && ((x) - (x)) != (x) - (x))
#define isfinite(x) (((x) - (x)) == ((x) - (x)))
#define isnormal(x) (fpclassify(x) == FP_NORMAL)
#define isgreater(a, b) ((a) > (b))
#define isgreaterequal(a, b) ((a) >= (b))
#define isless(a, b) ((a) < (b))
#define islessequal(a, b) ((a) <= (b))
#define isunordered(a, b) (isnan(a) || isnan(b))

double fabs(double x);
double copysign(double x, double y);
double nan(const char* tag);
double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double rint(double x);
double nearbyint(double x);
long lrint(double x);
long long llrint(double x);
long lround(double x);
long long llround(double x);
double fmod(double x, double y);
double remainder(double x, double y);
double modf(double x, double* ip);
double frexp(double x, int* e);
double ldexp(double x, int n);
double scalbn(double x, int n);
int ilogb(double x);
double logb(double x);
double fmin(double a, double b);
double fmax(double a, double b);
double fdim(double a, double b);
double fma(double a, double b, double c);
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double erf(double x);
double erfc(double x);
double lgamma(double x);
double tgamma(double x);
double nextafter(double x, double y);
double nexttoward(double x, long double y);
double remquo(double x, double y, int* q);
double scalbln(double x, long n);

typedef float float_t;
typedef double double_t;
#define islessgreater(a, b) (((a) < (b)) || ((a) > (b)))

float fabsf(float x);
float sqrtf(float x);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
float rintf(float x);
float fmodf(float x, float y);
float modff(float x, float* ip);
float frexpf(float x, int* e);
float ldexpf(float x, int n);
float copysignf(float x, float y);
float fminf(float a, float b);
float fmaxf(float a, float b);
float cbrtf(float x);
float hypotf(float x, float y);
float expf(float x);
float logf(float x);
float log10f(float x);
float log2f(float x);
float powf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float asinhf(float x);
float acoshf(float x);
float atanhf(float x);
float erff(float x);
float erfcf(float x);
float exp2f(float x);
float expm1f(float x);
float fdimf(float a, float b);
float fmaf(float a, float b, float c);
int ilogbf(float x);
float lgammaf(float x);
long long llrintf(float x);
long long llroundf(float x);
long lrintf(float x);
long lroundf(float x);
float log1pf(float x);
float logbf(float x);
float nanf(const char* tag);
float nearbyintf(float x);
float nextafterf(float x, float y);
float nexttowardf(float x, long double y);
float remainderf(float x, float y);
float remquof(float x, float y, int* q);
float scalblnf(float x, long n);
float scalbnf(float x, int n);
float tgammaf(float x);

/* long double = double */
#define fabsl fabs
#define sqrtl sqrt
#define floorl floor
#define ceill ceil
#define fmodl fmod
#define powl pow
#define expl exp
#define logl log
#define log10l log10
#define sinl sin
#define cosl cos
#define tanl tan
#define atanl atan
#define atan2l atan2
#define frexpl frexp
#define ldexpl ldexp
long double modfl(long double x, long double* p);
#define acosl acos
#define asinl asin
#define coshl cosh
#define sinhl sinh
#define tanhl tanh
#define acoshl acosh
#define asinhl asinh
#define atanhl atanh
#define cbrtl cbrt
#define copysignl copysign
#define erfl erf
#define erfcl erfc
#define exp2l exp2
#define expm1l expm1
#define fdiml fdim
#define fmal fma
#define fmaxl fmax
#define fminl fmin
#define hypotl hypot
#define ilogbl ilogb
#define lgammal lgamma
#define llrintl llrint
#define llroundl llround
#define log1pl log1p
#define log2l log2
#define logbl logb
#define lrintl lrint
#define lroundl lround
#define nanl nan
#define nearbyintl nearbyint
#define nextafterl nextafter
#define nexttowardl nexttoward
#define remainderl remainder
#define remquol remquo
#define rintl rint
#define roundl round
#define scalblnl scalbln
#define scalbnl scalbn
#define tgammal tgamma
#define truncl trunc


#ifdef __cplusplus
}
#endif
#endif
