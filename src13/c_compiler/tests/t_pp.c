/* t_pp.c - testes do pre-processador */
#include <stdio.h>
#include <string.h>

static int fails, total;
#define CHECK(c) do { total++; if (!(c)) { fails++; printf("FALHOU linha %d: %s\n", __LINE__, #c); } } while (0)

#define ONE 1
#define TWO (ONE + ONE)
#define SQ(x) ((x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a##b
#define XCAT(a, b) CAT(a, b)
#define EMPTY
#define SUM3(...) sum3(__VA_ARGS__)
#define FIRST(a, ...) a
#define LOG(fmt, ...) snprintf(buf, sizeof(buf), fmt, __VA_ARGS__)
#define F(x) G(x) + 1
#define G(x) F(x) * 2
#define NEST(x) SQ(x + 1)
#define CALL(f, x) f(x)

static int sum3(int a, int b, int c) { return a + b + c; }
static int SELF = 3;
#define SELF SELF + 1

#if defined(ONE) && TWO == 2 && !defined NOPE
static int if_ok = 1;
#else
static int if_ok = 0;
#endif

#if 0
#error isso nao pode aparecer
#elif SQ(3) == 9
static int elif_ok = 1;
#else
static int elif_ok = 0;
#endif

#ifdef NOPE
static int ifdef_ok = 0;
#elif defined(TWO)
#  if 1 ? 0 : 1
static int ifdef_ok = 0;
#  else
static int ifdef_ok = 1;
#  endif
#endif

#define TEMP 5
#undef TEMP
#ifndef TEMP
static int undef_ok = 1;
#endif

#if (-1 < 0) && (0xFF == 255) && (1 << 4 == 16) && ('A' == 65) && (7 / 2 == 3) && (7 % 4 == 3)
static int expr_ok = 1;
#else
static int expr_ok = 0;
#endif

#if UNDEFINED_NAME == 0
static int undef_name_zero = 1;
#endif

int main(void) {
  char buf[64];
  int XCAT(var, 7) = 77;
  int i = 2;

  CHECK(TWO == 2);
  CHECK(SQ(3) == 9);
  CHECK(SQ(1 + 2) == 9);
  CHECK(MAX(3, 8) == 8);
  CHECK(MAX(SQ(2), 3) == 4);
  CHECK(strcmp(STR(a + b), "a + b") == 0);
  CHECK(strcmp(STR( spaced   out ), "spaced out") == 0);
  CHECK(strcmp(XSTR(TWO), "(1 + 1)") == 0);
  CHECK(strcmp(STR(TWO), "TWO") == 0);
  CHECK(strcmp(STR("q\n"), "\"q\\n\"") == 0);
  CHECK(var7 == 77);
  CHECK(CAT(1, 0) == 10);
  CHECK(SUM3(1, 2, 3) == 6);
  CHECK(FIRST(4, 5, 6) == 4);
  LOG("%d-%s", 9, "x");
  CHECK(strcmp(buf, "9-x") == 0);
  CHECK(SELF == 4);
  CHECK(NEST(2) == 9);
  CHECK(CALL(SQ, 4) == 16);
  CHECK(if_ok && elif_ok && ifdef_ok && undef_ok && expr_ok && undef_name_zero);
  CHECK(ONE EMPTY == 1);
  CHECK(__LINE__ == 95);
  CHECK(strstr(__FILE__, "t_pp.c") != 0);
  CHECK(SQ(i + 1) == 9);
  {
    int F = 0; /* F nao seguido de ( nao expande */
    CHECK(F == 0);
  }
#line 500
  CHECK(__LINE__ == 500);

  if (fails) {
    printf("%d de %d testes falharam\n", fails, total);
    return 1;
  }
  printf("ok: %d testes de pre-processador\n", total);
  return 0;
}
