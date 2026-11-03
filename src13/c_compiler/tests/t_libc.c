/* Testes da libc: setjmp, time.h, ambiente, locale, signal, scanf, div,
 * rename/remove, realpath. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <time.h>
#include <sys/time.h>
#include <locale.h>
#include <signal.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>

static int fails;
static int count;

#define CHECK(expect, e) check_ll((long long)(expect), (long long)(e), #e, __LINE__)
#define CHECKS(expect, s) check_s(expect, s, __LINE__)

static void check_ll(long long want, long long got, const char* what, int line) {
  count++;
  if (want != got) {
    printf("FALHOU linha %d: %s => %lld (esperava %lld)\n", line, what, got, want);
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

/* ---- setjmp ---- */
static jmp_buf jb;
static int depth;

static void deep(int n) {
  char pad[100];
  pad[0] = (char)n;
  depth = n + pad[0] - n;
  if (n == 0) longjmp(jb, 42);
  deep(n - 1);
}

static void jump_zero(void) { longjmp(jb, 0); }

static void test_setjmp(void) {
  volatile int pass = 0;
  int r;
  if (setjmp(jb) == 0) {
    pass = 1;
    deep(10);
    CHECK(0, 1); /* nao chega aqui */
  } else {
    CHECK(1, pass);
    CHECK(0, depth);
  }
  r = setjmp(jb);
  if (r == 0) deep(3);
  CHECK(42, r);
  switch (setjmp(jb)) {
    case 0: jump_zero(); break;
    case 1: pass = 2; break;
    default: pass = 3;
  }
  CHECK(2, pass);
  /* o laco continua com as variaveis locais atualizadas */
  {
    int i = 0;
    int v = setjmp(jb);
    i += v;
    if (v < 5) longjmp(jb, v + 1);
    CHECK(5, v);
    CHECK(15, i); /* 0+1+2+3+4+5: i mora na memoria */
  }
  if (!setjmp(jb)) {
    pass = 7;
  }
  CHECK(7, pass);
}

/* ---- time ---- */
static void test_time(void) {
  struct tm tm, *g;
  time_t t, now;
  char buf[128];
  struct timeval tv;
  clock_t c0;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = 2026 - 1900;
  tm.tm_mon = 10;
  tm.tm_mday = 2;
  tm.tm_hour = 21;
  tm.tm_min = 5;
  tm.tm_sec = 9;
  t = mktime(&tm);
  CHECK(1793653509L, t);
  CHECK(1, tm.tm_wday); /* segunda */
  CHECK(305, tm.tm_yday);
  g = gmtime(&t);
  CHECK(2026 - 1900, g->tm_year);
  CHECK(10, g->tm_mon);
  CHECK(2, g->tm_mday);
  CHECK(21, g->tm_hour);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %a %b %j %A %B %p %y %e|%%", g);
  CHECKS("2026-11-02 21:05:09 Mon Nov 306 Monday November PM 26  2|%", buf);
  strftime(buf, sizeof(buf), "%D %T %F %R %u %w %V %G", g);
  CHECKS("11/02/26 21:05:09 2026-11-02 21:05 1 1 45 2026", buf);
  CHECKS("Mon Nov  2 21:05:09 2026\n", asctime(g));
  t = 0;
  CHECKS("Thu Jan  1 00:00:00 1970\n", ctime(&t));
  /* normalizacao */
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = 2024 - 1900;
  tm.tm_mon = 1;
  tm.tm_mday = 30; /* 30/fev/2024 -> 1/mar */
  mktime(&tm);
  CHECK(2, tm.tm_mon);
  CHECK(1, tm.tm_mday);
  t = -86400;
  g = gmtime(&t);
  CHECK(69, g->tm_year);
  CHECK(31, g->tm_mday);
  now = time(0);
  CHECK(1, now > 1700000000L);
  gettimeofday(&tv, 0);
  CHECK(1, tv.tv_sec >= now && tv.tv_usec >= 0 && tv.tv_usec < 1000000);
  c0 = clock();
  CHECK(1, c0 >= 0);
  CHECK(1, difftime(10, 4) == 6.0);
}

/* ---- ambiente, locale, sinais, div ---- */
#define SIGUSR_TEST 10
static int got_sig;
static void on_sig(int s) { got_sig = s; }

static void test_misc(void) {
  div_t d;
  lldiv_t ld;
  char buf[64];
  char* p;
  CHECK(0, getenv("TLIBC") != 0);
  CHECK(0, setenv("TLIBC", "um", 1));
  CHECKS("um", getenv("TLIBC"));
  setenv("TLIBC", "dois", 0);
  CHECKS("um", getenv("TLIBC"));
  putenv("TLIBC=tres");
  CHECKS("tres", getenv("TLIBC"));
  unsetenv("TLIBC");
  CHECK(0, getenv("TLIBC") != 0);

  CHECKS("C", setlocale(LC_ALL, ""));
  CHECK(0, setlocale(LC_ALL, "pt_BR") != 0);
  CHECKS(".", localeconv()->decimal_point);

  CHECK(SIG_DFL, signal(SIGUSR_TEST, on_sig));
  raise(SIGUSR_TEST);
  CHECK(SIGUSR_TEST, got_sig);
  signal(SIGINT, SIG_IGN);
  CHECK(0, raise(SIGINT));

  d = div(-7, 2);
  CHECK(-3, d.quot);
  CHECK(-1, d.rem);
  ld = lldiv(10000000000LL, 3);
  CHECK(3333333333LL, ld.quot);
  CHECK(1, ld.rem);
  sprintf(buf, "%" PRId64 "|%" PRIu32, (int64_t)-5, (uint32_t)7);
  CHECKS("-5|7", buf);
  CHECK(260, PATH_MAX);

  p = realpath("A\\B\\..\\.\\C.TXT", 0);
  CHECK(1, p != 0 && strlen(p) > 3);
  CHECK(0, strcmp(p + strlen(p) - 8, "\\A\\C.TXT"));
  free(p);
}


/* ---- scanf ---- */
static void test_scanf(void) {
  int a = 0, b = 0, n = 0;
  unsigned u = 0;
  long long ll = 0;
  char s[32], t[32], c = 0;
  short h = 0;
  CHECK(3, sscanf("12 0x1f -7", "%d %i %lld", &a, &b, &ll));
  CHECK(12, a);
  CHECK(31, b);
  CHECK(-7, ll);
  CHECK(2, sscanf("nome=valor;", "%[^=]=%[^;]", s, t));
  CHECKS("nome", s);
  CHECKS("valor", t);
  CHECK(3, sscanf("abc  X 077", "%s %c %o", s, &c, &u));
  CHECKS("abc", s);
  CHECK('X', c);
  CHECK(63, u);
  CHECK(1, sscanf("12345", "%3d%n", &a, &n));
  CHECK(123, a);
  CHECK(3, n);
  CHECK(1, sscanf("ff", "%x", &u));
  CHECK(255, u);
  CHECK(1, sscanf("-32768", "%hd", &h));
  CHECK(-32768, h);
  CHECK(-1, sscanf("", "%d", &a));
  CHECK(0, sscanf("x", "%d", &a));
  CHECK(1, sscanf("10%", "%d%%", &a));
  CHECK(2, sscanf("1,2", "%d,%d", &a, &b));
  CHECK(2, b);
}

/* ---- arquivos ---- */
static void test_files(void) {
  FILE* f = fopen("TLIBC1.TXT", "w");
  char line[32];
  char name[L_tmpnam];
  CHECK(1, f != 0);
  fputs("conteudo\n", f);
  fclose(f);
  remove("TLIBC2.TXT");
  CHECK(0, rename("TLIBC1.TXT", "TLIBC2.TXT"));
  CHECK(0, fopen("TLIBC1.TXT", "r") != 0);
  f = fopen("TLIBC2.TXT", "r");
  CHECK(1, f != 0);
  fgets(line, sizeof(line), f);
  fclose(f);
  CHECKS("conteudo\n", line);
  CHECK(0, remove("TLIBC2.TXT"));
  CHECK(-1, remove("TLIBC2.TXT"));
  CHECK(ENOENT, errno);
  CHECK(1, tmpnam(name) != 0 && strlen(name) > 4);
}

int main(void) {
  test_setjmp();
  test_time();
  test_misc();
  test_scanf();
  test_files();
  if (fails) {
    printf("t_libc: %d de %d falharam\n", fails, count);
    return 1;
  }
  printf("t_libc: %d testes ok\n", count);
  return 0;
}
