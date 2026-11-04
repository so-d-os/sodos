/* misc.c - ambiente (getenv/setenv do proprio processo), div, locale "C",
 * signal/raise e realpath. */
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <signal.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <so.h>

/* ---------- runtime C++ ----------
 * Versao vazia para .EXE so em C. O runtime C++ (sodos/cxx) define uma
 * versao forte no proprio .EXE, que vence este import da LIBCX. */
void __sodos_ctors(void) {}
void __sodos_dtors(void) {}

/* ---------- ambiente: tabela "NOME=valor" so deste processo ---------- */
#define ENV_MAX 64
static char* env_tab[ENV_MAX];
static int env_n;

static int env_find(const char* name, size_t len) {
  int i;
  for (i = 0; i < env_n; i++)
    if (strncmp(env_tab[i], name, len) == 0 && env_tab[i][len] == '=') return i;
  return -1;
}

char* getenv(const char* name) {
  int i;
  if (!name) return NULL;
  i = env_find(name, strlen(name));
  return i < 0 ? NULL : env_tab[i] + strlen(name) + 1;
}

int setenv(const char* name, const char* value, int overwrite) {
  size_t ln, lv;
  char* s;
  int i;
  if (!name || !*name || strchr(name, '=')) {
    errno = EINVAL;
    return -1;
  }
  if (!value) value = "";
  ln = strlen(name);
  i = env_find(name, ln);
  if (i >= 0 && !overwrite) return 0;
  lv = strlen(value);
  s = (char*)malloc(ln + lv + 2);
  if (!s) {
    errno = ENOMEM;
    return -1;
  }
  memcpy(s, name, ln);
  s[ln] = '=';
  memcpy(s + ln + 1, value, lv + 1);
  if (i >= 0) {
    free(env_tab[i]);
    env_tab[i] = s;
    return 0;
  }
  if (env_n >= ENV_MAX) {
    free(s);
    errno = ENOMEM;
    return -1;
  }
  env_tab[env_n++] = s;
  return 0;
}

int unsetenv(const char* name) {
  int i;
  if (!name) return -1;
  i = env_find(name, strlen(name));
  if (i >= 0) {
    free(env_tab[i]);
    env_tab[i] = env_tab[--env_n];
  }
  return 0;
}

int putenv(char* s) {
  char name[64];
  char* eq = s ? strchr(s, '=') : NULL;
  size_t n;
  if (!eq) return unsetenv(s);
  n = (size_t)(eq - s);
  if (n >= sizeof(name)) {
    errno = EINVAL;
    return -1;
  }
  memcpy(name, s, n);
  name[n] = 0;
  return setenv(name, eq + 1, 1);
}

/* ---------- div ---------- */
div_t div(int a, int b) {
  div_t r;
  r.quot = a / b;
  r.rem = a % b;
  return r;
}

ldiv_t ldiv(long a, long b) {
  ldiv_t r;
  r.quot = a / b;
  r.rem = a % b;
  return r;
}

lldiv_t lldiv(long long a, long long b) {
  lldiv_t r;
  r.quot = a / b;
  r.rem = a % b;
  return r;
}

intmax_t strtoimax(const char* s, char** end, int base) { return strtoll(s, end, base); }
uintmax_t strtoumax(const char* s, char** end, int base) { return strtoull(s, end, base); }
intmax_t imaxabs(intmax_t v) { return v < 0 ? -v : v; }

/* ---------- locale ---------- */
char* setlocale(int category, const char* locale) {
  (void)category;
  if (!locale || !*locale || strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0)
    return (char*)"C";
  return NULL;
}

struct lconv* localeconv(void) {
  static struct lconv lc;
  static char dot[] = ".";
  static char empty[] = "";
  lc.decimal_point = dot;
  lc.thousands_sep = empty;
  lc.grouping = empty;
  lc.int_curr_symbol = empty;
  lc.currency_symbol = empty;
  lc.mon_decimal_point = empty;
  lc.mon_thousands_sep = empty;
  lc.mon_grouping = empty;
  lc.positive_sign = empty;
  lc.negative_sign = empty;
  lc.int_frac_digits = 127;
  lc.frac_digits = 127;
  lc.p_cs_precedes = 127;
  lc.p_sep_by_space = 127;
  lc.n_cs_precedes = 127;
  lc.n_sep_by_space = 127;
  lc.p_sign_posn = 127;
  lc.n_sign_posn = 127;
  return &lc;
}

/* ---------- sinais ---------- */
static sighandler_t sig_tab[NSIG];

sighandler_t signal(int sig, sighandler_t handler) {
  sighandler_t old;
  if (sig <= 0 || sig >= NSIG || sig == SIGKILL) {
    errno = EINVAL;
    return SIG_ERR;
  }
  old = sig_tab[sig];
  sig_tab[sig] = handler;
  return old;
}

int raise(int sig) {
  sighandler_t h;
  if (sig <= 0 || sig >= NSIG) {
    errno = EINVAL;
    return -1;
  }
  h = sig_tab[sig];
  if (h == SIG_IGN) return 0;
  if (h == SIG_DFL) {
    if (sig == SIGCHLD) return 0;
    exit(128 + sig);
  }
  sig_tab[sig] = SIG_DFL;
  h(sig);
  return 0;
}

/* ---------- realpath: junta com a pasta atual e resolve . e .. ---------- */
char* realpath(const char* path, char* out) {
  char tmp[260];
  char res[260];
  char* parts[64];
  int np = 0, i, n = 0;
  char* p;
  if (!path) {
    errno = EINVAL;
    return NULL;
  }
  if (path[0] && path[1] == ':') {
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
  } else {
    k_pwd(tmp, (int)sizeof(tmp));
    if (path[0] == '\\' || path[0] == '/') tmp[2] = 0;
    n = (int)strlen(tmp);
    if (n && tmp[n - 1] != '\\' && n < (int)sizeof(tmp) - 1) tmp[n++] = '\\';
    strncpy(tmp + n, path, sizeof(tmp) - (size_t)n - 1);
    tmp[sizeof(tmp) - 1] = 0;
  }
  for (p = tmp; *p; p++)
    if (*p == '/') *p = '\\';
  /* "D:" + componentes */
  p = tmp + 2;
  while (*p) {
    char* s;
    while (*p == '\\') *p++ = 0;
    if (!*p) break;
    s = p;
    while (*p && *p != '\\') p++;
    if (*p) *p++ = 0;
    if (strcmp(s, ".") == 0) continue;
    if (strcmp(s, "..") == 0) {
      if (np) np--;
      continue;
    }
    if (np < 64) parts[np++] = s;
  }
  res[0] = tmp[0];
  res[1] = ':';
  n = 2;
  if (!np) res[n++] = '\\';
  for (i = 0; i < np; i++) {
    size_t l = strlen(parts[i]);
    if (n + 1 + (int)l >= (int)sizeof(res)) break;
    res[n++] = '\\';
    memcpy(res + n, parts[i], l);
    n += (int)l;
  }
  res[n] = 0;
  if (!out) out = (char*)malloc((size_t)n + 1);
  if (out) strcpy(out, res);
  return out;
}

/* O SODOS ainda nao tem links simbolicos; a ICU so usa para o fuso. */
ssize_t readlink(const char* path, char* buf, size_t n) {
  (void)path;
  (void)buf;
  (void)n;
  errno = ENOENT;
  return -1;
}

/* ---------- locale "C" / extensoes POSIX usadas pela libc++ ---------- */
locale_t newlocale(int mask, const char* locale, locale_t base) {
  (void)mask; (void)locale; (void)base;
  return (locale_t)1;
}
locale_t duplocale(locale_t loc) { return loc; }
void freelocale(locale_t loc) { (void)loc; }
locale_t uselocale(locale_t loc) { (void)loc; return (locale_t)1; }
long long strtoll_l(const char* s, char** end, int base, locale_t loc) {
  (void)loc; return strtoll(s, end, base);
}
unsigned long long strtoull_l(const char* s, char** end, int base, locale_t loc) {
  (void)loc; return strtoull(s, end, base);
}
#if __SIZEOF_POINTER__ == 8
double strtod_l(const char* s, char** end, locale_t loc) {
  (void)loc; return strtod(s, end);
}
float strtof_l(const char* s, char** end, locale_t loc) {
  (void)loc; return strtof(s, end);
}
long double strtold_l(const char* s, char** end, locale_t loc) {
  (void)loc; return strtold(s, end);
}
#endif

int vasprintf(char** out, const char* fmt, va_list ap) {
  va_list cp;
  int n;
  char* p;
  va_copy(cp, ap);
  n = vsnprintf(NULL, 0, fmt, cp);
  va_end(cp);
  if (n < 0) return -1;
  p = (char*)malloc((size_t)n + 1);
  if (!p) return -1;
  va_copy(cp, ap);
  vsnprintf(p, (size_t)n + 1, fmt, cp);
  va_end(cp);
  *out = p;
  return n;
}

int execvp(const char* file, char* const argv[]) { (void)file; (void)argv; errno = ENOENT; return -1; }
int kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
imaxdiv_t imaxdiv(intmax_t n, intmax_t d) { imaxdiv_t r; r.quot = n / d; r.rem = n % d; return r; }
intmax_t wcstoimax(const wchar_t* s, wchar_t** e, int b) { (void)s; (void)e; (void)b; return 0; }
uintmax_t wcstoumax(const wchar_t* s, wchar_t** e, int b) { (void)s; (void)e; (void)b; return 0; }

unsigned long getauxval(unsigned long type) { (void)type; return 0; }

int mbtowc(wchar_t* pwc, const char* s, size_t n) {
  if (!s) return 0;
  if (n == 0) return -1;
  if (pwc) *pwc = (unsigned char)s[0];
  return s[0] ? 1 : 0;
}
int wctomb(char* s, wchar_t wc) {
  if (!s) return 0;
  s[0] = (char)wc;
  return 1;
}
size_t mbstowcs(wchar_t* dst, const char* src, size_t n) {
  size_t i = 0;
  while (i < n && src[i]) { if (dst) dst[i] = (unsigned char)src[i]; i++; }
  if (i < n && dst) dst[i] = 0;
  return i;
}
size_t wcstombs(char* dst, const wchar_t* src, size_t n) {
  size_t i = 0;
  while (i < n && src[i]) { if (dst) dst[i] = (char)src[i]; i++; }
  if (i < n && dst) dst[i] = 0;
  return i;
}

long syscall(long number, ...) { (void)number; errno = ENOSYS; return -1; }

struct termios;
int tcgetattr(int fd, struct termios* t) { (void)fd; (void)t; return -1; }
int tcsetattr(int fd, int act, const struct termios* t) { (void)fd; (void)act; (void)t; return -1; }

char* basename(char* path) {
  char* p = path; char* r = path;
  if (!path || !*path) return (char*)".";
  for (p = path; *p; p++) if (*p == '/') r = p + 1;
  return r;
}
char* dirname(char* path) {
  static char dot[] = ".";
  char* p;
  if (!path || !*path) return dot;
  p = path;
  for (char* q = path; *q; q++) if (*q == '/') p = q;
  if (p == path) return (char *)"/";
  *p = 0;
  return path;
}

unsigned long __stack_chk_guard = 0x00000aff;
void __stack_chk_fail(void) { abort(); }

int getpid(void) { return 1; }
int getppid(void) { return 1; }
int gettid(void) { return 1; }

int fork(void) { errno = ENOSYS; return -1; }

int wait(int* status) { (void)status; errno = ECHILD; return -1; }
int waitpid(int pid, int* status, int options) { (void)pid; (void)status; (void)options; errno = ECHILD; return -1; }

int pipe(int fds[2]) { (void)fds; errno = ENOSYS; return -1; }
int dup(int fd) { (void)fd; errno = ENOSYS; return -1; }
int dup2(int a, int b) { (void)a; (void)b; errno = ENOSYS; return -1; }
int fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; errno = ENOSYS; return -1; }

int execv(const char* f, char* const a[]) { (void)f; (void)a; errno = ENOENT; return -1; }
int execve(const char* f, char* const a[], char* const e[]) { (void)f; (void)a; (void)e; errno = ENOENT; return -1; }

char* nl_langinfo(int item) { (void)item; return (char*)""; }

void* catopen(const char* n, int f) { (void)n; (void)f; return (void*)-1; }
char* catgets(void* c, int s, int m, const char* d) { (void)c; (void)s; (void)m; return (char*)d; }
int catclose(void* c) { (void)c; return 0; }

int sigemptyset(sigset_t* set) { if (set) memset(set, 0, sizeof(*set)); return 0; }
int sigfillset(sigset_t* set) { if (set) memset(set, 0xff, sizeof(*set)); return 0; }
int sigaddset(sigset_t* set, int sig) { if (set && sig > 0 && sig < 32) set->val[sig/32] |= 1u << (sig%32); return 0; }
int sigdelset(sigset_t* set, int sig) { if (set && sig > 0 && sig < 32) set->val[sig/32] &= ~(1u << (sig%32)); return 0; }
int sigismember(const sigset_t* set, int sig) { return set && sig > 0 && sig < 32 && (set->val[sig/32] & (1u << (sig%32))); }
int sigaction(int sig, const struct sigaction* act, struct sigaction* old) { (void)sig; (void)act; (void)old; return 0; }
int sigprocmask(int how, const sigset_t* set, sigset_t* old) { (void)how; (void)set; (void)old; return 0; }
int sigwait(const sigset_t* set, int* sig) { (void)set; if (sig) *sig = 0; return 0; }

int madvise(void* a, size_t l, int adv) { (void)a; (void)l; (void)adv; return 0; }

int __popcountdi2(unsigned long long x) { int n = 0; while (x) { n += (int)(x & 1); x >>= 1; } return n; }
int __clzdi2(unsigned long long x) { int n = 64; while (x) { n--; x >>= 1; } return n; }
int __ctzdi2(unsigned long long x) { int n = 0; if (!x) return 64; while (!(x & 1)) { n++; x >>= 1; } return n; }
int __ffsdi2(unsigned long long x) { return x ? __ctzdi2(x) + 1 : 0; }
