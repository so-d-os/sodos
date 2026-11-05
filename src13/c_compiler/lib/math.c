/* math.c - libm em C puro (o cc nao tem asm). Reducao de argumento e
 * polinomios do fdlibm (Sun, livre); erro tipico de 1 ulp.
 * gcc -mx32 -mno-sse (LIBCX) nao tem double: o arquivo fica vazio. */
#include <math.h>
#include <string.h>
#include <errno.h>

#if defined(__GNUC__) && defined(__x86_64__) && !defined(__SSE2__)
typedef int math_sem_double;
#else

typedef unsigned long long u64;

static u64 dbits(double x) {
  u64 b;
  memcpy(&b, &x, 8);
  return b;
}

static double bitsd(u64 b) {
  double x;
  memcpy(&x, &b, 8);
  return x;
}

static int dexp(double x) { return (int)((dbits(x) >> 52) & 0x7FF); }

static const double LN2_HI = 6.93147180369123816490e-01;
static const double LN2_LO = 1.90821492927058770002e-10;
static const double INV_LN2 = 1.44269504088896338700e+00;
static const double PI = 3.14159265358979311600e+00;
static const double PI_2 = 1.57079632679489655800e+00;

int __signbit(double x) { return (int)(dbits(x) >> 63); }
int __fpclassify(double x) {
  int e = dexp(x);
  u64 m = dbits(x) & 0xFFFFFFFFFFFFFULL;
  if (e == 0x7FF) return m ? FP_NAN : FP_INFINITE;
  if (e == 0) return m ? FP_SUBNORMAL : FP_ZERO;
  return FP_NORMAL;
}

double fabs(double x) { return bitsd(dbits(x) & 0x7FFFFFFFFFFFFFFFULL); }

double copysign(double x, double y) {
  return bitsd((dbits(x) & 0x7FFFFFFFFFFFFFFFULL) | (dbits(y) & 0x8000000000000000ULL));
}

double nan(const char* tag) {
  (void)tag;
  return bitsd(0x7FF8000000000000ULL);
}

/* x * 2^n, com subnormais e overflow */
double scalbn(double x, int n) {
  double y = x;
  if (n > 1023) {
    y *= bitsd(0x7FE0000000000000ULL); /* 2^1023 */
    n -= 1023;
    if (n > 1023) {
      y *= bitsd(0x7FE0000000000000ULL);
      n -= 1023;
      if (n > 1023) n = 1023;
    }
  } else if (n < -1022) {
    /* 2^-1022 * 2^53 de uma vez evita arredondar duas vezes */
    y *= bitsd(0x0010000000000000ULL) * bitsd(0x4340000000000000ULL);
    n += 1022 - 53;
    if (n < -1022) {
      y *= bitsd(0x0010000000000000ULL) * bitsd(0x4340000000000000ULL);
      n += 1022 - 53;
      if (n < -1022) n = -1022;
    }
  }
  return y * bitsd((u64)(1023 + n) << 52);
}

double ldexp(double x, int n) { return scalbn(x, n); }

double frexp(double x, int* e) {
  u64 b = dbits(x);
  int ex = (int)((b >> 52) & 0x7FF);
  if (ex == 0) {
    if (!(b << 1)) {
      *e = 0;
      return x;
    }
    x *= bitsd(0x43F0000000000000ULL); /* 2^64 */
    b = dbits(x);
    ex = (int)((b >> 52) & 0x7FF) - 64;
  } else if (ex == 0x7FF) {
    *e = 0;
    return x;
  }
  *e = ex - 1022;
  return bitsd((b & 0x800FFFFFFFFFFFFFULL) | (1022ULL << 52));
}

int ilogb(double x) {
  int e;
  if (x == 0 || x != x) return -2147483647 - 1;
  if (dexp(x) == 0x7FF) return 2147483647;
  frexp(x, &e);
  return e - 1;
}

double logb(double x) { return (double)ilogb(x); }

double trunc(double x) {
  u64 b = dbits(x);
  int e = (int)((b >> 52) & 0x7FF) - 1023;
  if (e >= 52) return x;
  if (e < 0) return bitsd(b & 0x8000000000000000ULL);
  return bitsd(b & ~(0xFFFFFFFFFFFFFULL >> e));
}

double floor(double x) {
  double t = trunc(x);
  return t > x ? t - 1.0 : t;
}

double ceil(double x) {
  double t = trunc(x);
  return t < x ? t + 1.0 : t;
}

double round(double x) {
  double t = trunc(x);
  if (fabs(x - t) >= 0.5) t += copysign(1.0, x);
  return t;
}

/* meio para o par (modo padrao do SSE) */
double rint(double x) {
  static const double two52 = 4503599627370496.0;
  double a = fabs(x), r;
  if (a >= two52 || x != x) return x;
  r = (a + two52) - two52;
  return copysign(r, x);
}

double nearbyint(double x) { return rint(x); }
long lrint(double x) { return (long)rint(x); }
long long llrint(double x) { return (long long)rint(x); }
long lround(double x) { return (long)round(x); }
long long llround(double x) { return (long long)round(x); }

double modf(double x, double* ip) {
  double t = trunc(x);
  *ip = t;
  if (dexp(x) == 0x7FF) return (dbits(x) << 12) ? x : copysign(0.0, x);
  return copysign(x - t, x);
}

/* resto exato: divisao longa das mantissas */
double fmod(double x, double y) {
  u64 bx = dbits(x), by = dbits(y), mx, my;
  int ex = (int)((bx >> 52) & 0x7FF), ey = (int)((by >> 52) & 0x7FF);
  u64 sx = bx & 0x8000000000000000ULL;
  if (ex == 0x7FF || y != y || y == 0) return (x * y) / (x * y);
  if (ey == 0x7FF) return x;
  if (fabs(x) < fabs(y)) return x;
  mx = bx & 0xFFFFFFFFFFFFFULL;
  my = by & 0xFFFFFFFFFFFFFULL;
  if (ex) mx |= 1ULL << 52;
  else ex = 1;
  if (ey) my |= 1ULL << 52;
  else ey = 1;
  while (ex > ey) {
    if (mx >= my) mx -= my;
    mx <<= 1;
    ex--;
  }
  if (mx >= my) mx -= my;
  if (mx == 0) return bitsd(sx);
  return copysign(scalbn((double)(long long)mx, ey - 1075), x);
}

double remainder(double x, double y) {
  double r = fmod(x, y);
  double ay = fabs(y);
  if (fabs(r) * 2 > ay || (fabs(r) * 2 == ay && fmod(trunc(x / y), 2.0) != 0))
    r -= copysign(ay, r);
  return r;
}

double fmin(double a, double b) {
  if (a != a) return b;
  if (b != b) return a;
  return a < b ? a : b;
}

double fmax(double a, double b) {
  if (a != a) return b;
  if (b != b) return a;
  return a > b ? a : b;
}

double fdim(double a, double b) { return a > b ? a - b : 0.0; }
double fma(double a, double b, double c) { return a * b + c; }

/* Raiz exata (arredondada para o mais proximo): metodo bit a bit sobre a
 * mantissa, como o e_sqrt.c do fdlibm, com o resto em 64 bits. */
double sqrt(double a) {
  u64 b = dbits(a), m, q = 0, r = 0, mant;
  int ex, e2, s, i;
  if (a != a || a == 0 || a > 1.7976931348623157e308) return a;
  if (a < 0) {
    errno = EDOM;
    return (a - a) / (a - a);
  }
  ex = (int)((b >> 52) & 0x7FF);
  m = b & 0xFFFFFFFFFFFFFULL;
  if (ex == 0) {
    ex = 1;
    while (!(m & (1ULL << 52))) {
      m <<= 1;
      ex--;
    }
  } else {
    m |= 1ULL << 52;
  }
  e2 = ex - 1075;               /* a = m * 2^e2, m com 53 bits */
  s = (e2 & 1) ? 55 : 54;       /* R = m * 2^s tem 107/108 bits e e2 - s e par */
  for (i = 53; i >= 0; i--) {
    int j = 2 * i;              /* bits j+1 e j de R */
    u64 pair = 0, t;
    if (j + 1 >= s) pair |= ((m >> (j + 1 - s)) & 1) << 1;
    if (j >= s) pair |= (m >> (j - s)) & 1;
    r = (r << 2) | pair;
    t = (q << 2) | 1;
    if (r >= t) {
      r -= t;
      q = (q << 1) | 1;
    } else {
      q <<= 1;
    }
  }
  /* q tem 54 bits: o ultimo e o de arredondamento */
  mant = q >> 1;
  if ((q & 1) && (r || (mant & 1))) mant++;
  return scalbn((double)(long long)mant, (e2 - s) / 2 + 1);
}

double cbrt(double x) {
  double y;
  int i;
  if (x == 0 || x != x || dexp(x) == 0x7FF) return x;
  y = copysign(exp(log(fabs(x)) / 3.0), x);
  for (i = 0; i < 2; i++) y = y - (y * y * y - x) / (3.0 * y * y);
  return y;
}

double hypot(double x, double y) {
  double a = fabs(x), b = fabs(y), r;
  if (a > 1.7976931348623157e308 || b > 1.7976931348623157e308) return 1.0 / 0.0;
  if (a != a || b != b) return a + b;
  if (a < b) {
    r = a;
    a = b;
    b = r;
  }
  if (a == 0) return 0;
  r = b / a;
  return a * sqrt(1.0 + r * r);
}

/* ---------- exp / log ---------- */
static const double P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03,
                    P3 = 6.61375632143793436117e-05, P4 = -1.65339022054652515390e-06,
                    P5 = 4.13813679705723846039e-08;

double exp(double x) {
  double hi, lo, r, t, c, y;
  int k;
  if (x != x) return x;
  if (x > 709.782712893383973096) {
    errno = ERANGE;
    return 1.7976931348623157e308 * 1.7976931348623157e308;
  }
  if (x < -745.13321910194110842) {
    errno = ERANGE;
    return 0;
  }
  if (fabs(x) < 3.7252902984e-09) return 1.0 + x;
  k = (int)(x * INV_LN2 + (x < 0 ? -0.5 : 0.5));
  hi = x - k * LN2_HI;
  lo = k * LN2_LO;
  r = hi - lo;
  t = r * r;
  c = r - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
  y = 1.0 - ((lo - (r * c) / (2.0 - c)) - hi);
  return scalbn(y, k);
}

double exp2(double x) { return exp(x * LN2_HI + x * LN2_LO); }

/* e^x - 1 sem cancelar perto de 0 */
double expm1(double x) {
  double t, s;
  int n;
  if (fabs(x) >= 0.5) return exp(x) - 1.0;
  t = x;
  s = x;
  for (n = 2; n < 30; n++) {
    t *= x / n;
    s += t;
    if (fabs(t) < 1e-18 * fabs(s)) break;
  }
  return s;
}

static const double Lg1 = 6.666666666666735130e-01, Lg2 = 3.999999999940941908e-01,
                    Lg3 = 2.857142874366239149e-01, Lg4 = 2.222219843214978396e-01,
                    Lg5 = 1.818357216161805012e-01, Lg6 = 1.531383769920937332e-01,
                    Lg7 = 1.479819860511658591e-01;

/* log(1+f) - f para f em [sqrt(2)/2-1, sqrt(2)-1], e k */
static double log_parts(double x, int* kp, double* fp) {
  double m, f, s, z, w, t1, t2, R, hfsq;
  int k;
  m = frexp(x, &k); /* [0.5, 1) */
  if (m < 0.70710678118654752440) {
    m *= 2;
    k--;
  }
  f = m - 1.0;
  s = f / (2.0 + f);
  z = s * s;
  w = z * z;
  t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
  t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
  R = t2 + t1;
  hfsq = 0.5 * f * f;
  *kp = k;
  *fp = f;
  return s * (hfsq + R) - hfsq;
}

double log(double x) {
  double f, r;
  int k;
  if (x != x || x > 1.7976931348623157e308) return x;
  if (x == 0) {
    errno = ERANGE;
    return -1.0 / 0.0;
  }
  if (x < 0) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  r = log_parts(x, &k, &f);
  return k * LN2_HI - ((-(r + k * LN2_LO)) - f);
}

double log2(double x) {
  double f, r;
  int k;
  if (x != x || x <= 0 || x > 1.7976931348623157e308) return log(x);
  r = log_parts(x, &k, &f);
  return k + (f + r) * INV_LN2;
}

double log10(double x) {
  static const double IVLN10 = 4.34294481903251816668e-01;
  static const double LOG10_2HI = 3.01029995663611771306e-01;
  static const double LOG10_2LO = 3.69423907715893078616e-13;
  double f, r;
  int k;
  if (x != x || x <= 0 || x > 1.7976931348623157e308) return log(x);
  r = log_parts(x, &k, &f);
  return k * LOG10_2HI + (k * LOG10_2LO + (f + r) * IVLN10);
}

double log1p(double x) {
  double u = 1.0 + x;
  if (u == 1.0) return x;
  return log(u) * x / (u - 1.0);
}

static int is_int(double y) { return trunc(y) == y; }
static int is_odd_int(double y) { return is_int(y) && fabs(y) < 9007199254740992.0 && fmod(y, 2.0) != 0; }

double pow(double x, double y) {
  double r;
  if (y == 0) return 1.0;
  if (x == 1.0) return 1.0;
  if (x != x || y != y) return x + y;
  if (dexp(y) == 0x7FF) {
    double ax = fabs(x);
    if (ax == 1.0) return 1.0;
    if ((ax > 1.0) == (y > 0)) return y > 0 ? y : -y;
    return 0.0;
  }
  if (x == 0) {
    if (y < 0) {
      errno = ERANGE;
      return is_odd_int(y) ? copysign(1.0 / 0.0, x) : 1.0 / 0.0;
    }
    return is_odd_int(y) ? x : 0.0;
  }
  if (dexp(x) == 0x7FF) {
    if (x > 0) return y > 0 ? x : 0.0;
    if (y > 0) return is_odd_int(y) ? x : -x;
    return is_odd_int(y) ? -0.0 : 0.0;
  }
  if (x < 0 && !is_int(y)) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  /* expoente inteiro pequeno: quadrados sucessivos */
  if (is_int(y) && fabs(y) <= 1024) {
    long long n = (long long)fabs(y);
    double b = x;
    r = 1.0;
    while (n) {
      if (n & 1) r *= b;
      b *= b;
      n >>= 1;
    }
    return y < 0 ? 1.0 / r : r;
  }
  r = exp(y * log(fabs(x)));
  if (x < 0 && is_odd_int(y)) r = -r;
  return r;
}

/* ---------- trigonometria ---------- */
static const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                    S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                    S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
static const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                    C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                    C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;

static double ksin(double x) {
  double z = x * x;
  return x + x * z * (S1 + z * (S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)))));
}

static double kcos(double x) {
  double z = x * x;
  double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
  double hz = 0.5 * z;
  double w = 1.0 - hz;
  return w + (((1.0 - w) - hz) + z * r);
}

/* x = n*(pi/2) + r, |r| <= pi/4 (pi/2 em tres partes, Cody-Waite) */
static int rem_pio2(double x, double* r) {
  static const double INV_PIO2 = 6.36619772367581382433e-01;
  static const double PIO2_1 = 1.57079632673412561417e+00;
  static const double PIO2_2 = 6.07710050630396597660e-11;
  static const double PIO2_3 = 2.02226624871116645580e-21;
  double n, a = fabs(x);
  long long k;
  if (a <= 0.785398163397448279) {
    *r = x;
    return 0;
  }
  if (a > 1e15) x = fmod(x, 2 * PI); /* perde precisao, mas nao trava */
  n = rint(x * INV_PIO2);
  k = (long long)n;
  *r = ((x - n * PIO2_1) - n * PIO2_2) - n * PIO2_3;
  return (int)(k & 3);
}

double sin(double x) {
  double r;
  if (dexp(x) == 0x7FF) return x - x;
  switch (rem_pio2(x, &r)) {
    case 0: return ksin(r);
    case 1: return kcos(r);
    case 2: return -ksin(r);
  }
  return -kcos(r);
}

double cos(double x) {
  double r;
  if (dexp(x) == 0x7FF) return x - x;
  switch (rem_pio2(x, &r)) {
    case 0: return kcos(r);
    case 1: return -ksin(r);
    case 2: return -kcos(r);
  }
  return ksin(r);
}

double tan(double x) {
  double r;
  int n;
  if (dexp(x) == 0x7FF) return x - x;
  n = rem_pio2(x, &r);
  if (n & 1) return -kcos(r) / ksin(r);
  return ksin(r) / kcos(r);
}

static const double atanhi[] = { 4.63647609000806093515e-01, 7.85398163397448278999e-01,
                                 9.82793723247329054082e-01, 1.57079632679489655800e+00 };
static const double atanlo[] = { 2.26987774529616870924e-17, 3.06161699786838301793e-17,
                                 1.39033110312309984516e-17, 6.12323399573676603587e-17 };
static const double aT[] = { 3.33333333333329318027e-01, -1.99999999998764832476e-01,
                             1.42857142725034663711e-01, -1.11111104054623557880e-01,
                             9.09088713343650656196e-02, -7.69187620504482999495e-02,
                             6.66107313738753120669e-02, -5.83357013379057348645e-02,
                             4.97687799461593236017e-02, -3.65315727442169155270e-02,
                             1.62858201153657823623e-02 };

double atan(double x) {
  double ax = fabs(x), z, w, s1, s2;
  int id, neg = x < 0;
  if (x != x) return x;
  if (ax > 1.8446744073709552e19) return neg ? -atanhi[3] - atanlo[3] : atanhi[3] + atanlo[3];
  if (ax < 0.4375) {
    if (ax < 7.4505806e-09) return x;
    id = -1;
  } else {
    x = ax;
    if (ax < 1.1875) {
      if (ax < 0.6875) {
        id = 0;
        x = (2.0 * x - 1.0) / (2.0 + x);
      } else {
        id = 1;
        x = (x - 1.0) / (x + 1.0);
      }
    } else if (ax < 2.4375) {
      id = 2;
      x = (x - 1.5) / (1.0 + 1.5 * x);
    } else {
      id = 3;
      x = -1.0 / x;
    }
  }
  z = x * x;
  w = z * z;
  s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
  s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
  if (id < 0) return x - x * (s1 + s2);
  z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
  return neg ? -z : z;
}

double atan2(double y, double x) {
  if (x != x || y != y) return x + y;
  if (y == 0) {
    if (__signbit(x)) return copysign(PI, y);
    return y;
  }
  if (x == 0) return y > 0 ? PI_2 : -PI_2;
  if (dexp(x) == 0x7FF) {
    if (dexp(y) == 0x7FF) {
      if (x > 0) return y > 0 ? PI / 4 : -PI / 4;
      return y > 0 ? 3 * PI / 4 : -3 * PI / 4;
    }
    if (x > 0) return copysign(0.0, y);
    return copysign(PI, y);
  }
  if (dexp(y) == 0x7FF) return y > 0 ? PI_2 : -PI_2;
  if (x > 0) return atan(y / x);
  return y >= 0 ? atan(y / x) + PI : atan(y / x) - PI;
}

double asin(double x) {
  if (x > 1 || x < -1) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}

double acos(double x) {
  if (x > 1 || x < -1) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}

double sinh(double x) {
  double t, a = fabs(x);
  if (a > 22) {
    t = exp(a * 0.5);
    t = 0.5 * t * t;
    return x < 0 ? -t : t;
  }
  t = expm1(a);
  t = 0.5 * (t + t / (t + 1.0));
  return x < 0 ? -t : t;
}

double cosh(double x) {
  double a = fabs(x), t;
  if (a > 22) {
    t = exp(a * 0.5);
    return 0.5 * t * t;
  }
  t = exp(a);
  return 0.5 * (t + 1.0 / t);
}

double tanh(double x) {
  double a = fabs(x), t;
  if (x != x) return x;
  if (a > 22) return x < 0 ? -1.0 : 1.0;
  t = expm1(2.0 * a);
  t = t / (t + 2.0);
  return x < 0 ? -t : t;
}

double asinh(double x) {
  double a = fabs(x), r;
  if (a > 1e8) r = log(a) + LN2_HI + LN2_LO;
  else r = log1p(a + a * a / (1.0 + sqrt(1.0 + a * a)));
  return x < 0 ? -r : r;
}

double acosh(double x) {
  if (x < 1) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  if (x > 1e8) return log(x) + LN2_HI + LN2_LO;
  return log(x + sqrt(x * x - 1.0));
}

double atanh(double x) {
  double a = fabs(x), r;
  if (a > 1) {
    errno = EDOM;
    return (x - x) / (x - x);
  }
  r = 0.5 * log1p(2.0 * a / (1.0 - a));
  return x < 0 ? -r : r;
}

/* ---------- versoes float ---------- */
float fabsf(float x) { return (float)fabs(x); }
float sqrtf(float x) { return (float)sqrt(x); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float truncf(float x) { return (float)trunc(x); }
float roundf(float x) { return (float)round(x); }
float fmodf(float x, float y) { return (float)fmod(x, y); }
float expf(float x) { return (float)exp(x); }
float logf(float x) { return (float)log(x); }
float log10f(float x) { return (float)log10(x); }
float log2f(float x) { return (float)log2(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float tanf(float x) { return (float)tan(x); }
float asinf(float x) { return (float)asin(x); }
float acosf(float x) { return (float)acos(x); }
float atanf(float x) { return (float)atan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float sinhf(float x) { return (float)sinh(x); }
float coshf(float x) { return (float)cosh(x); }
float tanhf(float x) { return (float)tanh(x); }
float hypotf(float x, float y) { return (float)hypot(x, y); }
float ldexpf(float x, int n) { return (float)ldexp(x, n); }
float frexpf(float x, int* e) { return (float)frexp(x, e); }
float copysignf(float x, float y) { return (float)copysign(x, y); }
float fminf(float a, float b) { return (float)fmin(a, b); }
float fmaxf(float a, float b) { return (float)fmax(a, b); }
float cbrtf(float x) { return (float)cbrt(x); }
float rintf(float x) { return (float)rint(x); }
float modff(float x, float* ip) {
  double d;
  float r = (float)modf(x, &d);
  *ip = (float)d;
  return r;
}

float nearbyintf(float x) { return x; }

float scalbnf(float x, int n) { return (float)scalbn(x, n); }

#endif
