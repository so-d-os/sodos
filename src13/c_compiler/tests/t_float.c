/* Testes de ponto flutuante: cc (float/double), printf, strtod e math.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

static int fails;
static int count;

#define CHECK(expect, e) check_ll((long long)(expect), (long long)(e), #e, __LINE__)
#define CHECKF(expect, e) check_d((double)(expect), (double)(e), 0, #e, __LINE__)
#define NEAR(expect, e) check_d((double)(expect), (double)(e), 1e-12, #e, __LINE__)
#define CHECKS(expect, s) check_s(expect, s, __LINE__)

static void check_ll(long long want, long long got, const char* what, int line) {
  count++;
  if (want != got) {
    printf("FALHOU linha %d: %s => %lld (esperava %lld)\n", line, what, got, want);
    fails++;
  }
}

static void check_d(double want, double got, double tol, const char* what, int line) {
  double d = want - got;
  count++;
  if (d < 0) d = -d;
  if (want < 0 ? d > -want * tol : d > want * tol) {
    if (want == got) return;
    printf("FALHOU linha %d: %s => %.17g (esperava %.17g)\n", line, what, got, want);
    fails++;
  }
}

static void check_s(const char* want, const char* got, int line) {
  count++;
  if (strcmp(want, got) != 0) {
    printf("FALHOU linha %d: \"%s\" (esperava \"%s\")\n", line, got, want);
    fails++;
  }
}

/* ---- globais ---- */
double g_d = 1.5;
float g_f = 2.25f;
double g_arr[] = { 1.0, -2.5, 1e10, 0.1 };
double g_expr = 3.0 * 2 + 1 / 4.0;
int g_int_of_d = (int)7.9;
struct mix { char c; double d; float f; int i; };
struct mix g_mix = { 'x', 6.5, 0.5f, 9 };

static double add(double a, double b) { return a + b; }
static float addf(float a, float b) { return a + b; }
static double mix_args(int a, double b, long long c, float d, char e) {
  return a + b + c + d + e;
}
static double ret_int(void) { return 3; }
static int trunc_ret(double d) { return d; }

static double sum_va(int n, ...) {
  va_list ap;
  double s = 0;
  int i;
  va_start(ap, n);
  for (i = 0; i < n; i++) s += va_arg(ap, double);
  va_end(ap);
  return s;
}

static char buf[128];
static char* fmt(const char* f, double v) {
  sprintf(buf, f, v);
  return buf;
}

static void test_cc(void) {
  double a = 1.5, b = 2.0;
  float f = 1.25f;
  int i = 7;
  unsigned u = 4000000000u;
  long long ll = -1234567890123LL;
  unsigned long long ull = 18000000000000000000ULL;
  double* p;
  double z = 0.0;
  double nz = -0.0;

  CHECKF(3.5, a + b);
  CHECKF(-0.5, a - b);
  CHECKF(3.0, a * b);
  CHECKF(0.75, a / b);
  CHECKF(-1.5, -a);
  CHECKF(2.5f, f * 2);
  CHECKF(8.5, a + i);
  CHECK(1, a < b);
  CHECK(0, a > b);
  CHECK(1, a <= 1.5);
  CHECK(1, a >= 1.5);
  CHECK(1, a == 1.5);
  CHECK(0, a != 1.5);
  CHECK(1, f < a);
  CHECK(1, !z);
  CHECK(1, !nz);
  CHECK(0, !a);
  CHECK(1, a && b);
  CHECK(1, z || a);
  CHECK(0, z || nz);
  CHECK(2, a ? 2 : 3);
  CHECK(3, z ? 2 : 3);

  /* conversoes */
  CHECK(1, (int)a);
  CHECK(-1, (int)-a);
  CHECK(-2, (int)-2.99);
  CHECKF(7.0, (double)i);
  CHECKF(4000000000.0, (double)u);
  CHECK(4000000000u, (unsigned)4000000000.0);
  CHECKF(-1234567890123.0, (double)ll);
  CHECK(-1234567890123LL, (long long)-1234567890123.0);
  CHECKF(18000000000000000000.0, (double)ull);
  CHECK(18000000000000000000ULL, (unsigned long long)18000000000000000000.0);
  CHECK(123, (unsigned long long)123.9);
  CHECK(-56, (signed char)(int)200.0);
  CHECK(1, (_Bool)0.1);
  CHECK(0, (_Bool)0.0);
  CHECK(200, (unsigned char)200.7);
  CHECKF(1.25, (double)f);
  CHECKF(0.1f, (float)0.1);
  CHECK(4, sizeof(float));
  CHECK(8, sizeof(double));
  CHECK(8, sizeof(long double));

  /* atribuicoes compostas, ++ */
  a = 1.0;
  a += 2;
  CHECKF(3.0, a);
  a *= 1.5;
  CHECKF(4.5, a);
  a -= 0.5;
  a /= 2;
  CHECKF(2.0, a);
  a++;
  ++a;
  a--;
  CHECKF(3.0, a);
  CHECKF(3.0, a++);
  CHECKF(4.0, a);
  i = 10;
  i += 2.7;
  CHECK(12, i);
  i *= 0.5;
  CHECK(6, i);

  /* globais, arrays, structs, ponteiros */
  CHECKF(1.5, g_d);
  CHECKF(2.25, g_f);
  CHECKF(-2.5, g_arr[1]);
  CHECKF(1e10, g_arr[2]);
  CHECKF(0.1, g_arr[3]);
  CHECKF(6.25, g_expr);
  CHECK(7, g_int_of_d);
  CHECK('x', g_mix.c);
  CHECKF(6.5, g_mix.d);
  CHECKF(0.5, g_mix.f);
  CHECK(9, g_mix.i);
  p = &g_arr[0];
  p[0] = 9.75;
  CHECKF(9.75, g_arr[0]);
  CHECKF(-2.5, *(p + 1));

  /* chamadas */
  CHECKF(4.0, add(1.5, 2.5));
  CHECKF(4.0, add(1, 3));
  CHECKF(3.5f, addf(1.25f, 2.25f));
  CHECKF(1 + 2.5 + 10 + 0.5 + 'A', mix_args(1, 2.5, 10, 0.5f, 'A'));
  CHECKF(3.0, ret_int());
  CHECK(9, trunc_ret(9.99));
  CHECKF(6.75, sum_va(3, 1.0, 2.5, (float)3.25));

  /* NaN e infinito */
  {
    double inf = 1.0 / z;
    double nan = z / z;
    CHECK(1, inf > 1e308);
    CHECK(0, nan == nan);
    CHECK(1, nan != nan);
    CHECK(0, nan < 1.0);
    CHECK(0, nan >= 1.0);
    CHECK(1, isnan(nan));
    CHECK(1, isinf(-inf));
  }
}

static void test_printf(void) {
  CHECKS("3.141593", fmt("%f", 3.14159265358979));
  CHECKS("3.14", fmt("%.2f", 3.14159265358979));
  CHECKS("  -2.50", fmt("%7.2f", -2.5));
  CHECKS("-2.50  ", fmt("%-7.2f", -2.5));
  CHECKS("+1.0", fmt("%+.1f", 1.0));
  CHECKS("0003.5", fmt("%06.1f", 3.5));
  CHECKS("2", fmt("%.0f", 2.5));
  CHECKS("4", fmt("%.0f", 3.5));
  CHECKS("0.1250", fmt("%.4f", 0.125));
  CHECKS("0.12", fmt("%.2f", 0.125));
  CHECKS("0.13", fmt("%.2f", 0.1251));
  CHECKS("1e+100", fmt("%g", 1e100));
  CHECKS("-1.5", fmt("%g", -1.5));
  CHECKS("1.000000E+00", fmt("%E", 1.0));
  CHECKS("4.9406564584124654e-324", fmt("%.17g", 4.9406564584124654e-324));
  CHECKS("1.7976931348623157e+308", fmt("%.17g", 1.7976931348623157e308));
  CHECKS("100", fmt("%.3g", 99.99));
  CHECKS("1.00e+03", fmt("%.2e", 999.9));
  CHECKS("0.0001", fmt("%g", 0.0001));
  CHECKS("1e-05", fmt("%g", 0.00001));
  CHECKS("3.00000", fmt("%#g", 3.0));
  CHECKS("1.000000e+10", fmt("%e", 1e10));
  CHECKS("1.5E-07", fmt("%.1E", 1.5e-7));
  CHECKS("0.1", fmt("%g", 0.1));
  CHECKS("100000", fmt("%g", 100000.0));
  CHECKS("1e+06", fmt("%g", 1000000.0));
  CHECKS("1.23457e-05", fmt("%g", 0.0000123456789));
  CHECKS("0.30000000000000004", fmt("%.17g", 0.1 + 0.2));
  CHECKS("123456789012", fmt("%.0f", 123456789012.0));
  CHECKS("inf", fmt("%f", 1.0 / 0.0));
  CHECKS("-inf", fmt("%f", -1.0 / 0.0));
  CHECKS("nan", fmt("%f", strtod("nan", 0)));
  CHECKS("-nan", fmt("%f", 0.0 / 0.0));
  CHECKS("0.000000", fmt("%f", 0.0));
  CHECKS("-0.000000", fmt("%f", -0.0));
  CHECKS("1.000000", fmt("%f", 0.9999999));
  CHECKS("2", fmt("%g", 2.0));
  CHECKS("0x1p+0", fmt("%a", 1.0));
}

static void test_strtod(void) {
  char* end;
  CHECKF(3.25, atof("3.25"));
  CHECKF(-0.001, atof("-1e-3"));
  CHECKF(1e300, strtod("1e300", 0));
  CHECKF(0.1, strtod("0.1", 0));
  CHECKF(123.0, strtod("  123xyz", &end));
  CHECK('x', *end);
  CHECKF(255.0, strtod("0xff", 0));
  CHECKF(1.5, strtod("0x1.8p0", 0));
  CHECK(1, isinf(strtod("inf", 0)));
  CHECK(1, isnan(strtod("nan", 0)));
  CHECKF(2.2250738585072014e-308, strtod("2.2250738585072014e-308", 0));
  CHECKF(1.7976931348623157e308, strtod("1.7976931348623157e308", 0));
  CHECKF(5e-324, strtod("4.9406564584124654e-324", 0));
  CHECKF(0.30000000000000004, strtod("0.30000000000000004", 0));
  CHECKF(9007199254740993.0, strtod("9007199254740993", 0));
  {
    double d;
    char s[40];
    int ok = 0;
    for (d = 0.001; d < 1e6; d = d * 1.37 + 0.011) {
      sprintf(s, "%.17g", d);
      if (strtod(s, 0) == d) ok++;
      else printf("ida e volta: %s\n", s);
    }
    CHECK(ok, ok);
  }
  {
    int n = -1;
    double v = 0;
    float fv = 0;
    sscanf("2.5 7.25", "%lf %f%n", &v, &fv, &n);
    CHECKF(2.5, v);
    CHECKF(7.25, fv);
    CHECK(8, n);
  }
}

static void test_math(void) {
  NEAR(1.4142135623730951, sqrt(2.0));
  CHECKF(3.0, sqrt(9.0));
  CHECK(1, isnan(sqrt(-1.0)));
  CHECKF(2.0, fabs(-2.0));
  CHECKF(3.0, floor(3.7));
  CHECKF(-4.0, floor(-3.2));
  CHECKF(4.0, ceil(3.2));
  CHECKF(-3.0, ceil(-3.7));
  CHECKF(-3.0, trunc(-3.7));
  CHECKF(4.0, round(3.5));
  CHECKF(-4.0, round(-3.5));
  CHECKF(1.5, fmod(7.5, 2.0));
  CHECKF(-1.5, fmod(-7.5, 2.0));
  NEAR(2.718281828459045, exp(1.0));
  NEAR(1.0, exp(0.0));
  NEAR(4.5399929762484854e-05, exp(-10.0));
  NEAR(0.6931471805599453, log(2.0));
  NEAR(2.302585092994046, log(10.0));
  NEAR(3.0, log10(1000.0));
  NEAR(10.0, log2(1024.0));
  NEAR(1024.0, pow(2.0, 10.0));
  NEAR(1.4142135623730951, pow(2.0, 0.5));
  NEAR(-8.0, pow(-2.0, 3.0));
  NEAR(0.125, pow(2.0, -3.0));
  NEAR(0.8414709848078965, sin(1.0));
  NEAR(0.5403023058681398, cos(1.0));
  NEAR(1.5574077246549023, tan(1.0));
  NEAR(-0.5440211108893698, sin(10.0));
  NEAR(0.8623188722876839, cos(100.0));
  NEAR(0.7853981633974483, atan(1.0));
  NEAR(1.5707963267948966, asin(1.0));
  NEAR(1.0471975511965979, acos(0.5));
  NEAR(2.356194490192345, atan2(1.0, -1.0));
  NEAR(-1.5707963267948966, atan2(-1.0, 0.0));
  NEAR(1.1752011936438014, sinh(1.0));
  NEAR(1.5430806348152437, cosh(1.0));
  NEAR(0.7615941559557649, tanh(1.0));
  NEAR(5.0, hypot(3.0, 4.0));
  {
    int e;
    double m = frexp(48.0, &e);
    double ip;
    CHECKF(0.75, m);
    CHECK(6, e);
    CHECKF(48.0, ldexp(0.75, 6));
    CHECKF(0.25, modf(3.25, &ip));
    CHECKF(3.0, ip);
  }
  CHECKF(1.5f, sqrtf(2.25f));
  CHECKF(2.0, fmin(2.0, 3.0));
  CHECKF(3.0, fmax(2.0, 3.0));
}

int main(void) {
  test_cc();
  test_printf();
  test_strtod();
  test_math();
  if (fails) {
    printf("t_float: %d de %d falharam\n", fails, count);
    return 1;
  }
  printf("t_float: %d testes ok\n", count);
  return 0;
}
