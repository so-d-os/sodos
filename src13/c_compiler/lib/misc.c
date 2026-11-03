/* misc.c - ambiente (getenv/setenv do proprio processo), div, locale "C",
 * signal/raise e realpath. */
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <signal.h>
#include <inttypes.h>
#include <errno.h>
#include <so.h>

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
