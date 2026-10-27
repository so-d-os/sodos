/* Testes do cc: cada CHECK compara o valor com o esperado. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

static int fails;
static int count;

#define CHECK(expect, e) check_ll((long long)(expect), (long long)(e), #e, __LINE__)

static void check_ll(long long want, long long got, const char* what, int line) {
  count++;
  if (want != got) {
    printf("FALHOU linha %d: %s => %lld (esperava %lld)\n", line, what, got, want);
    fails++;
  }
}

/* ---- globais ---- */
int g1 = 5;
int g_arr[5] = { 1, 2, 3 };
char g_str[] = "abc";
char* g_ptr = "hello";
char* g_names[] = { "zero", "um", "dois" };
int g_bss;
static int g_static = 7;
long long g_ll = 0x123456789ALL;
struct pt { int x; int y; };
struct pt g_pt = { 10, 20 };
struct pt g_pts[] = { { 1, 2 }, { 3, 4 } };
int* g_pint = &g_arr[2];
short g_sh = -3;
unsigned char g_uc = 250;

struct named {
  const char* name;
  int (*fn)(int);
};

static int dbl(int x) { return x * 2; }
static int tri(int x) { return x * 3; }
struct named g_tab[] = { { "dbl", dbl }, { "tri", tri } };

typedef struct node {
  int val;
  struct node* next;
} node_t;

enum color { RED, GREEN = 5, BLUE };

union u {
  int i;
  char c[4];
  long long ll;
};

/* ---- funcoes ---- */
static int add(int a, int b) { return a + b; }
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static struct pt make_pt(int x, int y) {
  struct pt p;
  p.x = x;
  p.y = y;
  return p;
}

static int sum_pt(struct pt p) { return p.x + p.y; }

static int sum_va(int n, ...) {
  va_list ap;
  int s = 0;
  int i;
  va_start(ap, n);
  for (i = 0; i < n; i++) s += va_arg(ap, int);
  va_end(ap);
  return s;
}

static long long sum_va_ll(int n, ...) {
  va_list ap;
  long long s = 0;
  int i;
  va_start(ap, n);
  for (i = 0; i < n; i++) s += va_arg(ap, long long);
  va_end(ap);
  return s;
}

static int counter(void) {
  static int c = 100;
  return c++;
}

static char ret_char(int x) { return (char)x; }
static unsigned short ret_us(int x) { return (unsigned short)x; }

static int apply(int (*f)(int), int v) { return f(v); }

static int cmp_int(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

static int sw(int x) {
  switch (x) {
    case 1: return 10;
    case 2:
    case 3: return 23;
    case -5: return -50;
    default: return 0;
  }
}

static int sw_fall(int x) {
  int r = 0;
  switch (x) {
    case 0: r += 1;
    case 1: r += 10; break;
    case 2: r += 100;
  }
  return r;
}

static int use_goto(int n) {
  int i = 0;
again:
  i++;
  if (i < n) goto again;
  return i;
}

static long long ll_sw(long long v) {
  switch (v) {
    case 0x100000000LL: return 1;
    case 5: return 2;
  }
  return 3;
}

int main(void) {
  /* aritmetica */
  CHECK(7, 3 + 4);
  CHECK(-1, 3 - 4);
  CHECK(12, 3 * 4);
  CHECK(3, 10 / 3);
  CHECK(1, 10 % 3);
  CHECK(-3, -10 / 3);
  CHECK(-1, -10 % 3);
  CHECK(2147483647, INT_MAX);
  CHECK(1431655765, 0xFFFFFFFFu / 3);
  CHECK(1, (unsigned)-1 > 0);
  CHECK(0, -1 > 0);
  CHECK(1, -1 < 0u ? 0 : 1);
  CHECK(8, 1 << 3);
  CHECK(-4, -16 >> 2);
  CHECK(0x3FFFFFFC, 0xFFFFFFF0u >> 2);
  CHECK(6, 7 & 14);
  CHECK(15, 7 | 8);
  CHECK(9, 12 ^ 5);
  CHECK(-8, ~7);
  CHECK(1, !0);
  CHECK(0, !5);
  CHECK(1, 2 && 3);
  CHECK(0, 2 && 0);
  CHECK(1, 0 || 3);
  CHECK(5, (1, 2, 5));
  CHECK(4, 1 ? 4 : 5);
  CHECK(1, 3 >= 3);
  CHECK(0, 3 > 3);
  CHECK(1, 3 != 4);

  /* char/short */
  {
    char c = 200;
    unsigned char uc = 200;
    short s = 40000;
    unsigned short us = 40000;
    signed char sc = -1;
    CHECK(-56, c);
    CHECK(200, uc);
    CHECK(-25536, s);
    CHECK(40000, us);
    CHECK(255, (unsigned char)sc);
    CHECK(-3, g_sh);
    CHECK(250, g_uc);
    CHECK(-56, ret_char(200));
    CHECK(65535, ret_us(-1));
    CHECK(1, sizeof(char));
    CHECK(2, sizeof(short));
    CHECK(4, sizeof(int));
    CHECK(4, sizeof(long));
    CHECK(8, sizeof(long long));
    CHECK(4, sizeof(void*));
    CHECK(4, sizeof g_arr[0]);
  }

  /* long long */
  {
    long long a = 0x100000000LL;
    long long b = -7;
    unsigned long long u = 0xFFFFFFFFFFFFFFFFULL;
    CHECK(0x100000005LL, a + 5);
    CHECK(0xFFFFFFFFLL, a - 1);
    CHECK(0x300000000LL, a * 3);
    CHECK(-7000000000LL, b * 1000000000LL);
    CHECK(2, a / 0x80000000LL);
    CHECK(-2, b / 3);
    CHECK(-1, b % 3);
    CHECK(1, u / 0xFFFFFFFFFFFFFFFFULL);
    CHECK(0x7FFFFFFFFFFFFFFFLL, (long long)(u >> 1));
    CHECK(-1, b >> 3);
    CHECK(0x1000000000LL, a << 4);
    CHECK(0x80000000LL, 1LL << 31);
    CHECK(0x100000000LL, 1LL << 32);
    CHECK(1, (1LL << 40) >> 40);
    CHECK(1, a > 5);
    CHECK(1, b < 0);
    CHECK(0, u < 5);
    CHECK(1, a >= a);
    CHECK(1, a == 0x100000000LL);
    CHECK(0, a != 0x100000000LL);
    CHECK(-0x100000000LL, -a);
    CHECK(0x123456789ALL, g_ll);
    CHECK(28, sum_va_ll(3, 10LL, 8LL, 10LL));
    CHECK(1, ll_sw(0x100000000LL));
    CHECK(2, ll_sw(5));
    CHECK(3, ll_sw(6));
    CHECK(-1, (int)(long long)-1);
    CHECK(4294967295LL, (long long)(unsigned)-1);
    CHECK(1, !(a - a));
  }

  /* ponteiros e arrays */
  {
    int a[10];
    int* p = a;
    int i;
    int m[3][4];
    for (i = 0; i < 10; i++) a[i] = i * i;
    CHECK(16, a[4]);
    CHECK(25, *(p + 5));
    CHECK(36, *(a + 6));
    p += 2;
    CHECK(4, *p);
    p++;
    CHECK(9, *p);
    CHECK(3, p - a);
    CHECK(9, *p--);
    CHECK(4, *p);
    CHECK(9, *++p);
    CHECK(40, sizeof(a));
    for (i = 0; i < 12; i++) m[i / 4][i % 4] = i;
    CHECK(6, m[1][2]);
    CHECK(16, sizeof(m[0]));
    CHECK(3, g_arr[2]);
    CHECK(0, g_arr[4]);
    CHECK(3, *g_pint);
    CHECK(4, sizeof(g_str));
    CHECK('b', g_str[1]);
    CHECK('l', g_ptr[2]);
    CHECK('u', g_names[1][0]);
    CHECK(4, (int)strlen(g_names[2]));
    {
      void* vp = a;
      CHECK(4, (char*)((char*)vp + 4) - (char*)a);
    }
  }

  /* structs, unions, enums */
  {
    struct pt p = { 3, 4 };
    struct pt q;
    struct pt* pp = &q;
    node_t n1, n2;
    union u un;
    q = p;
    CHECK(3, q.x);
    pp->y = 9;
    CHECK(9, q.y);
    CHECK(7, sum_pt(p));
    q = make_pt(5, 6);
    CHECK(11, q.x + q.y);
    CHECK(30, sum_pt(make_pt(10, 20)));
    CHECK(20, g_pt.y);
    CHECK(4, g_pts[1].y);
    CHECK(8, sizeof(struct pt));
    n1.val = 1;
    n1.next = &n2;
    n2.val = 2;
    n2.next = NULL;
    CHECK(2, n1.next->val);
    un.ll = 0;
    un.i = 0x41424344;
    CHECK(0x44, un.c[0]);
    CHECK(8, sizeof(un));
    CHECK(0, RED);
    CHECK(6, BLUE);
    CHECK(4, sizeof(enum color));
    {
      struct { char a; int b; char c; } s;
      CHECK(12, sizeof(s));
    }
    {
      struct pt arr[2] = { { 1, 2 }, { 3, 4 } };
      int z[5] = { 1, 2 };
      char s[] = "xyz";
      CHECK(3, arr[1].x);
      CHECK(0, z[3]);
      CHECK(4, sizeof(s));
      CHECK('z', s[2]);
    }
  }

  /* funcoes */
  CHECK(5, add(2, 3));
  CHECK(55, fib(10));
  CHECK(10, sum_va(4, 1, 2, 3, 4));
  CHECK(100, counter());
  CHECK(101, counter());
  CHECK(14, apply(dbl, 7));
  CHECK(21, apply(tri, 7));
  CHECK(12, g_tab[1].fn(4));
  CHECK('d', g_tab[0].name[0]);
  {
    int (*fp)(int, int) = add;
    CHECK(9, fp(4, 5));
    CHECK(9, (*fp)(4, 5));
  }
  {
    int v[5] = { 5, 3, 9, 1, 7 };
    qsort(v, 5, sizeof(int), cmp_int);
    CHECK(1, v[0]);
    CHECK(9, v[4]);
  }

  /* controle */
  CHECK(10, sw(1));
  CHECK(23, sw(3));
  CHECK(-50, sw(-5));
  CHECK(0, sw(99));
  CHECK(11, sw_fall(0));
  CHECK(10, sw_fall(1));
  CHECK(100, sw_fall(2));
  CHECK(5, use_goto(5));
  {
    int i, s = 0;
    for (i = 0; i < 10; i++) {
      if (i == 3) continue;
      if (i == 7) break;
      s += i;
    }
    CHECK(18, s);
    i = 0;
    do {
      i += 2;
    } while (i < 9);
    CHECK(10, i);
    i = 0;
    while (1) {
      if (++i > 4) break;
    }
    CHECK(5, i);
    s = 0;
    for (int k = 0; k < 4; k++) s += k;
    CHECK(6, s);
  }

  /* atribuicoes compostas */
  {
    int x = 10;
    unsigned u = 7;
    long long l = 5;
    char c = 'a';
    x += 5; CHECK(15, x);
    x -= 3; CHECK(12, x);
    x *= 2; CHECK(24, x);
    x /= 5; CHECK(4, x);
    x %= 3; CHECK(1, x);
    x <<= 4; CHECK(16, x);
    x >>= 2; CHECK(4, x);
    x |= 3; CHECK(7, x);
    x &= 5; CHECK(5, x);
    x ^= 1; CHECK(4, x);
    u -= 8; CHECK(4294967295LL, u);
    l *= 1000000000; CHECK(5000000000LL, l);
    c++; CHECK('b', c);
    c += 1; CHECK('c', c);
  }

  /* string e libc */
  {
    char buf[64];
    sprintf(buf, "%d-%s-%x-%c", 42, "ok", 255, 'Z');
    CHECK(0, strcmp(buf, "42-ok-ff-Z"));
    snprintf(buf, 6, "%s", "abcdefgh");
    CHECK(0, strcmp(buf, "abcde"));
    CHECK(123, atoi("123"));
    CHECK(-255, strtol("-0xff", NULL, 16));
    {
      const char* s = "abcd";
      CHECK(3, (int)(strchr(s, 'd') - s));
    }
  }

  CHECK(7, g_static);
  CHECK(0, g_bss);
  CHECK(5, g1);

  if (fails) {
    printf("%d de %d testes falharam\n", fails, count);
    return 1;
  }
  printf("ok: %d testes\n", count);
  return 0;
}
