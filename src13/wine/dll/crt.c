/*
 * crt.c - UCRT (api-ms-win-crt-*, ucrtbase) e MSVCRT embutidos (W0).
 *
 * printf: o va_list da Microsoft e um char* que anda de 8 em 8 bytes
 * (double tambem ocupa 8). Cada especificacao e lida daqui e formatada pelo
 * snprintf da LIBC64, um argumento por vez.
 */
#include <so.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wine_int.h"

typedef char* msva;
#define MSARG(ap, t) (*(t*)(((ap) += 8) - 8))

/* ---------- FILE ---------- */
/* O ucrt trata FILE como opaco; so comparamos ponteiros. */
struct wfile {
  uint8_t pad[28];
  int fd;          /* FILE._file do msvcrt (offset 28) */
  uint8_t rest[16];
};
/* stdin, stdout, stderr: tambem e o "_iob" que o msvcrt antigo exporta */
static struct wfile iob[3] = {{{0}, 0, {0}}, {{0}, 1, {0}}, {{0}, 2, {0}}};
_Static_assert(sizeof(struct wfile) == 48, "FILE do msvcrt x64");

static int is_std(void* f) { return f == &iob[1] || f == &iob[2]; }

/* ---------- formatador ---------- */
struct sink {
  char* buf;   /* destino (sprintf) ou 0 */
  size_t cap;
  size_t len;  /* total gerado (mesmo alem de cap) */
  int con;     /* escreve no console */
  char tmp[512];
  size_t tn;
};

static void sink_flush(struct sink* s) {
  if (s->con && s->tn) wcon_write(s->tmp, s->tn);
  s->tn = 0;
}
static void sink_put(struct sink* s, const char* p, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (s->buf && s->len + 1 < s->cap) s->buf[s->len] = p[i];
    if (s->con) {
      if (s->tn == sizeof(s->tmp)) sink_flush(s);
      s->tmp[s->tn++] = p[i];
    }
    s->len++;
  }
}

/* wide: o formato veio de wprintf (%s = wchar_t*, %c = wchar_t). */
static void wfmt(struct sink* s, const char* f, msva ap, int wide) {
  while (*f) {
    char spec[32];
    char out[512];
    int n = 0, len;
    int lmod = 0;      /* 1 h, 2 hh, 3 l, 4 ll/I64/z/j/t, 5 L, 6 w */
    int is_long_str = 0;
    char conv;
    if (*f != '%') {
      const char* p = f;
      while (*p && *p != '%') p++;
      sink_put(s, f, (size_t)(p - f));
      f = p;
      continue;
    }
    spec[n++] = *f++;
    if (*f == '%') {
      sink_put(s, "%", 1);
      f++;
      continue;
    }
    while (*f && strchr("-+ #0", *f) && n < 20) spec[n++] = *f++;
    if (*f == '*') {
      n += snprintf(spec + n, sizeof(spec) - (size_t)n, "%d", MSARG(ap, int));
      f++;
    } else {
      while (*f >= '0' && *f <= '9' && n < 26) spec[n++] = *f++;
    }
    if (*f == '.') {
      spec[n++] = *f++;
      if (*f == '*') {
        n += snprintf(spec + n, sizeof(spec) - (size_t)n, "%d", MSARG(ap, int));
        f++;
      } else {
        while (*f >= '0' && *f <= '9' && n < 28) spec[n++] = *f++;
      }
    }
    for (;;) {
      if (*f == 'h') {
        lmod = lmod == 1 ? 2 : 1;
        f++;
      } else if (*f == 'l') {
        lmod = lmod == 3 ? 4 : 3;
        f++;
      } else if (*f == 'z' || *f == 'j' || *f == 't') {
        lmod = 4;
        f++;
      } else if (*f == 'I' && f[1] == '6' && f[2] == '4') {
        lmod = 4;
        f += 3;
      } else if (*f == 'I' && f[1] == '3' && f[2] == '2') {
        lmod = 0;
        f += 3;
      } else if (*f == 'I') {
        lmod = 4;
        f++;
      } else if (*f == 'L') {
        lmod = 5;
        f++;
      } else if (*f == 'w') {
        lmod = 6;
        f++;
      } else {
        break;
      }
    }
    conv = *f;
    if (!conv) break;
    f++;
    switch (conv) {
      case 'd':
      case 'i':
      case 'u':
      case 'x':
      case 'X':
      case 'o': {
        int64_t v = MSARG(ap, int64_t);
        int sg = conv == 'd' || conv == 'i';
        if (lmod == 4) {
        } else if (lmod == 1) {
          v = sg ? (int64_t)(int16_t)v : (int64_t)(uint16_t)v;
        } else if (lmod == 2) {
          v = sg ? (int64_t)(int8_t)v : (int64_t)(uint8_t)v;
        } else {
          /* int e long sao 32 bits no Windows */
          v = sg ? (int64_t)(int32_t)v : (int64_t)(uint32_t)v;
        }
        spec[n++] = 'l';
        spec[n++] = 'l';
        spec[n++] = conv;
        spec[n] = 0;
        len = snprintf(out, sizeof(out), spec, v);
        break;
      }
      case 'p':
        spec[n++] = 'p';
        spec[n] = 0;
        len = snprintf(out, sizeof(out), "%016llX", (unsigned long long)MSARG(ap, uint64_t));
        break;
      case 'f':
      case 'F':
      case 'e':
      case 'E':
      case 'g':
      case 'G':
      case 'a':
      case 'A': {
        double d = MSARG(ap, double);
        spec[n++] = conv;
        spec[n] = 0;
        len = snprintf(out, sizeof(out), spec, d);
        break;
      }
      case 'c':
      case 'C': {
        int c = (int)MSARG(ap, int64_t);
        int w = (conv == 'C') != wide ? 1 : 0;
        if (lmod == 3 || lmod == 6) w = 1;
        if (lmod == 1) w = 0;
        if (w) {
          wchar16 wc = (wchar16)c;
          len = wutf16_to_8(&wc, 1, out, 8);
        } else {
          out[0] = (char)c;
          len = 1;
        }
        break;
      }
      case 's':
      case 'S':
      case 'Z': {
        const void* p = MSARG(ap, const void*);
        char* tmp = 0;
        int w = (conv == 'S') != wide;
        if (lmod == 3 || lmod == 6) w = 1;
        if (lmod == 1) w = 0;
        if (!p) p = w ? (const void*)u"(null)" : (const void*)"(null)";
        if (w) {
          tmp = wdup16to8((const wchar16*)p);
          p = tmp ? tmp : "";
        }
        is_long_str = 1;
        spec[n++] = 's';
        spec[n] = 0;
        if (n == 2) {
          /* %s sem largura/precisao: sem limite de tamanho */
          sink_put(s, (const char*)p, strlen((const char*)p));
          len = -1;
        } else {
          len = snprintf(out, sizeof(out), spec, (const char*)p);
        }
        if (tmp) k_free(tmp);
        break;
      }
      case 'n': {
        void* p = MSARG(ap, void*);
        if (p) *(int*)p = (int)s->len;
        len = 0;
        break;
      }
      default:
        out[0] = '%';
        out[1] = conv;
        len = 2;
        break;
    }
    (void)is_long_str;
    if (len > 0) sink_put(s, out, (size_t)len < sizeof(out) ? (size_t)len : sizeof(out) - 1);
  }
  sink_flush(s);
}

static char* narrow_fmt(const wchar16* f) { return wdup16to8(f); }

/* ---------- stdio ---------- */
static WINAPI void* __acrt_iob_func(unsigned i) { return i < 3 ? &iob[i] : 0; }

static WINAPI int __stdio_common_vfprintf(uint64_t opt, void* f, const char* fmt, void* loc, msva ap) {
  struct sink s;
  (void)opt;
  (void)loc;
  memset(&s, 0, sizeof(s));
  s.con = is_std(f);
  wfmt(&s, fmt, ap, 0);
  return (int)s.len;
}
static WINAPI int __stdio_common_vfwprintf(uint64_t opt, void* f, const wchar16* fmt, void* loc, msva ap) {
  struct sink s;
  char* nf = narrow_fmt(fmt);
  (void)opt;
  (void)loc;
  memset(&s, 0, sizeof(s));
  s.con = is_std(f);
  if (nf) wfmt(&s, nf, ap, 1);
  k_free(nf);
  return (int)s.len;
}
static WINAPI int __stdio_common_vsprintf(uint64_t opt, char* buf, size_t n, const char* fmt, void* loc,
                                          msva ap) {
  struct sink s;
  (void)loc;
  memset(&s, 0, sizeof(s));
  s.buf = buf;
  s.cap = buf ? n : 0;
  wfmt(&s, fmt, ap, 0);
  if (buf && n) buf[s.len < n ? s.len : n - 1] = 0;
  if (s.len >= n && !(opt & 2)) return buf ? -1 : (int)s.len; /* legado: -1 se cortou */
  return (int)s.len;
}
static WINAPI int __stdio_common_vsnprintf_s(uint64_t opt, char* buf, size_t n, size_t cnt, const char* fmt,
                                             void* loc, msva ap) {
  size_t lim = cnt < n ? cnt + 1 : n;
  (void)opt;
  return __stdio_common_vsprintf(0, buf, lim, fmt, loc, ap);
}
static WINAPI int __stdio_common_vswprintf(uint64_t opt, wchar16* buf, size_t n, const wchar16* fmt, void* loc,
                                           msva ap) {
  struct sink s;
  char* nf = narrow_fmt(fmt);
  char* tmp = (char*)wzalloc(n * 4 + 4);
  int w = 0;
  (void)loc;
  memset(&s, 0, sizeof(s));
  s.buf = tmp;
  s.cap = n * 4 + 4;
  if (nf && tmp) wfmt(&s, nf, ap, 1);
  if (buf && n && tmp) {
    w = wutf8_to_16(tmp, (int)(s.len < s.cap ? s.len : s.cap - 1), buf, (int)n - 1);
    buf[w] = 0;
  }
  k_free(nf);
  k_free(tmp);
  if ((size_t)w >= n - 1 && s.len >= n && !(opt & 2)) return -1;
  return (int)s.len;
}

/* msvcrt.dll (MinGW antigo): printf/sprintf com reticencias da ABI MS. */
static WINAPI int ms_printf(const char* fmt, ...) {
  __builtin_ms_va_list ap;
  struct sink s;
  __builtin_ms_va_start(ap, fmt);
  memset(&s, 0, sizeof(s));
  s.con = 1;
  wfmt(&s, fmt, (msva)ap, 0);
  __builtin_ms_va_end(ap);
  return (int)s.len;
}
static WINAPI int ms_fprintf(void* f, const char* fmt, ...) {
  __builtin_ms_va_list ap;
  struct sink s;
  __builtin_ms_va_start(ap, fmt);
  memset(&s, 0, sizeof(s));
  s.con = is_std(f);
  wfmt(&s, fmt, (msva)ap, 0);
  __builtin_ms_va_end(ap);
  return (int)s.len;
}
static WINAPI int ms_sprintf(char* buf, const char* fmt, ...) {
  __builtin_ms_va_list ap;
  struct sink s;
  __builtin_ms_va_start(ap, fmt);
  memset(&s, 0, sizeof(s));
  s.buf = buf;
  s.cap = (size_t)-1 / 2;
  wfmt(&s, fmt, (msva)ap, 0);
  __builtin_ms_va_end(ap);
  buf[s.len] = 0;
  return (int)s.len;
}
static WINAPI int ms_snprintf(char* buf, size_t n, const char* fmt, ...) {
  __builtin_ms_va_list ap;
  int r;
  __builtin_ms_va_start(ap, fmt);
  r = __stdio_common_vsprintf(2, buf, n, fmt, 0, (msva)ap);
  __builtin_ms_va_end(ap);
  return r;
}
static WINAPI int ms_vprintf(const char* fmt, msva ap) { return __stdio_common_vfprintf(0, &iob[1], fmt, 0, ap); }
static WINAPI int ms_vfprintf(void* f, const char* fmt, msva ap) { return __stdio_common_vfprintf(0, f, fmt, 0, ap); }
static WINAPI int ms_vsnprintf(char* b, size_t n, const char* fmt, msva ap) {
  return __stdio_common_vsprintf(0, b, n, fmt, 0, ap);
}
static WINAPI int ms_vsprintf(char* b, const char* fmt, msva ap) {
  return __stdio_common_vsprintf(2, b, (size_t)-1 / 2, fmt, 0, ap);
}

static WINAPI size_t ms_fwrite(const void* p, size_t sz, size_t n, void* f) {
  if (is_std(f)) wcon_write((const char*)p, sz * n);
  return n;
}
static WINAPI int ms_fputs(const char* s, void* f) {
  if (is_std(f)) wcon_write(s, strlen(s));
  return 0;
}
static WINAPI int ms_puts(const char* s) {
  wcon_write(s, strlen(s));
  wcon_write("\n", 1);
  return 0;
}
static WINAPI int ms_fputc(int c, void* f) {
  char ch = (char)c;
  if (is_std(f)) wcon_write(&ch, 1);
  return c & 0xFF;
}
static WINAPI int ms_putchar(int c) { return ms_fputc(c, &iob[1]); }
static WINAPI int ms_fflush(void* f) {
  (void)f;
  return 0;
}
static WINAPI int ms_fgetc(void* f) {
  if (f != &iob[0]) return -1;
  return wcon_getc();
}
static WINAPI int ms_getchar(void) { return wcon_getc(); }
static WINAPI char* ms_fgets(char* buf, int n, void* f) {
  int len;
  if (f != &iob[0] || n < 2) return 0;
  len = wcon_readline(buf, n - 1);
  if (len < 0) return 0;
  buf[len++] = '\n';
  buf[len] = 0;
  return buf;
}
static WINAPI int ms_setvbuf(void* f, char* b, int m, size_t n) {
  (void)f;
  (void)b;
  (void)m;
  (void)n;
  return 0;
}
static WINAPI int ms_fileno(void* f) { return f ? ((struct wfile*)f)->fd : -1; }
static WINAPI int ms_isatty(int fd) { return fd >= 0 && fd <= 2; }
static WINAPI int ms_setmode(int fd, int mode) {
  (void)fd;
  return mode;
}
static WINAPI int ms_write(int fd, const void* p, unsigned n) {
  if (fd == 1 || fd == 2) wcon_write((const char*)p, n);
  return (int)n;
}

/* ---------- processo / inicializacao ---------- */
static WINAPI int* __p___argc(void) { return &wp()->argc; }
static WINAPI char*** __p___argv(void) { return &wp()->argv; }
static WINAPI wchar16*** __p___wargv(void) { return &wp()->wargv; }
static WINAPI char*** __p__environ(void) { return &wp()->environ; }
static WINAPI wchar16*** __p__wenviron(void) { return &wp()->wenviron; }
static WINAPI int* __p__commode(void) { return &wp()->commode; }
static WINAPI int* __p__fmode(void) { return &wp()->fmode; }
static WINAPI int _configure_narrow_argv(int m) {
  (void)m;
  return 0;
}
static WINAPI int _configure_wide_argv(int m) {
  (void)m;
  return 0;
}
static WINAPI int _initialize_narrow_environment(void) { return 0; }
static WINAPI int _initialize_wide_environment(void) { return 0; }
static WINAPI char** _get_initial_narrow_environment(void) { return wp()->environ; }
static WINAPI wchar16** _get_initial_wide_environment(void) { return wp()->wenviron; }
static WINAPI void _set_app_type(int t) { (void)t; }
static WINAPI int _set_fmode(int m) {
  wp()->fmode = m;
  return 0;
}
static WINAPI int _set_new_mode(int m) {
  (void)m;
  return 0;
}
static WINAPI void* _set_invalid_parameter_handler(void* h) {
  (void)h;
  return 0;
}
static WINAPI void __setusermatherr(void* f) { (void)f; }
static WINAPI void _fpreset(void) {}
static WINAPI unsigned _controlfp(unsigned n, unsigned m) {
  (void)n;
  (void)m;
  return 0x9001F;
}
static WINAPI int _controlfp_s(unsigned* cur, unsigned n, unsigned m) {
  (void)n;
  (void)m;
  if (cur) *cur = 0x9001F;
  return 0;
}
static WINAPI int _seh_filter_exe(unsigned code, void* ep) {
  (void)code;
  (void)ep;
  return 0; /* EXCEPTION_CONTINUE_SEARCH */
}
static WINAPI int __C_specific_handler(void* a, void* b, void* c, void* d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return 1; /* ExceptionContinueSearch */
}

typedef void (WINAPI* vfn)(void);
typedef int (WINAPI* ifn)(void);
static WINAPI void _initterm(vfn* a, vfn* b) {
  for (; a < b; a++)
    if (*a) (*a)();
}
static WINAPI int _initterm_e(ifn* a, ifn* b) {
  for (; a < b; a++)
    if (*a) {
      int r = (*a)();
      if (r) return r;
    }
  return 0;
}
static WINAPI int _crt_atexit(vfn f) {
  struct wproc* p = wp();
  if (p->natexit >= WATEXIT_MAX) return -1;
  p->atexit_fn[p->natexit++] = f;
  return 0;
}
static WINAPI int _crt_at_quick_exit(vfn f) {
  (void)f;
  return 0;
}
static WINAPI void* ms_onexit(vfn f) { return _crt_atexit(f) == 0 ? (void*)f : 0; }
static void run_atexit(void) {
  struct wproc* p = wp();
  while (p->natexit > 0) {
    vfn f = p->atexit_fn[--p->natexit];
    if (f) f();
  }
}
static WINAPI void _cexit(void) { run_atexit(); }
static WINAPI void _c_exit(void) {}
static WINAPI __attribute__((noreturn)) void ms_exit(int c) {
  run_atexit();
  wexit(c);
}
static WINAPI __attribute__((noreturn)) void ms__exit(int c) { wexit(c); }
static WINAPI __attribute__((noreturn)) void ms_abort(void) {
  k_puts("\nabnormal program termination\n");
  wexit(3);
}
static WINAPI void* ms_signal(int s, void* h) {
  (void)s;
  (void)h;
  return 0;
}
static WINAPI int* _errno(void) { return (int*)(wteb() + 0x1810); }
static WINAPI void _invalid_parameter_noinfo(void) {}
static WINAPI __attribute__((noreturn)) void _invalid_parameter_noinfo_noreturn(void) {
  wsay("parametro invalido (CRT)", 0);
  wexit(0xC000000D);
}

/* msvcrt antigo: __getmainargs/__wgetmainargs */
static WINAPI int __getmainargs(int* argc, char*** argv, char*** env, int g, void* si) {
  (void)g;
  (void)si;
  *argc = wp()->argc;
  *argv = wp()->argv;
  *env = wp()->environ;
  return 0;
}
static WINAPI int __wgetmainargs(int* argc, wchar16*** argv, wchar16*** env, int g, void* si) {
  (void)g;
  (void)si;
  *argc = wp()->argc;
  *argv = wp()->wargv;
  *env = wp()->wenviron;
  return 0;
}
static WINAPI void __set_app_type(int t) { (void)t; }

/* ---------- memoria ---------- */
static WINAPI void* ms_malloc(size_t n) { return wheap_alloc(n); }
static WINAPI void* ms_calloc(size_t a, size_t b) {
  void* p;
  if (b && a > (size_t)-1 / b) return 0;
  p = wheap_alloc(a * b);
  if (p) memset(p, 0, (a && b) ? a * b : 1);
  return p;
}
static WINAPI void ms_free(void* p) { wheap_free(p); }
static WINAPI void* ms_realloc(void* p, size_t n) {
  if (!n) {
    ms_free(p);
    return 0;
  }
  return wheap_realloc(p, n);
}
static WINAPI size_t ms_msize(void* p) { return wheap_size(p); }
static WINAPI void* _aligned_malloc(size_t n, size_t a) {
  return k_malloc_aligned((unsigned)(n ? n : 1), (unsigned)(a < 16 ? 16 : a));
}
static WINAPI void _aligned_free(void* p) { ms_free(p); }

/* ---------- string / memoria (wrappers da LIBC64) ---------- */
static WINAPI size_t ms_strlen(const char* s) { return strlen(s); }
static WINAPI size_t ms_strnlen(const char* s, size_t n) {
  size_t i = 0;
  while (i < n && s[i]) i++;
  return i;
}
static WINAPI int ms_strcmp(const char* a, const char* b) { return strcmp(a, b); }
static WINAPI int ms_strncmp(const char* a, const char* b, size_t n) { return strncmp(a, b, n); }
static WINAPI char* ms_strcpy(char* d, const char* s) { return strcpy(d, s); }
static WINAPI char* ms_strncpy(char* d, const char* s, size_t n) { return strncpy(d, s, n); }
static WINAPI char* ms_strcat(char* d, const char* s) { return strcat(d, s); }
static WINAPI char* ms_strchr(const char* s, int c) { return strchr(s, c); }
static WINAPI char* ms_strrchr(const char* s, int c) { return strrchr(s, c); }
static WINAPI char* ms_strstr(const char* a, const char* b) { return strstr(a, b); }
static WINAPI char* ms_strdup(const char* s) {
  size_t n = strlen(s) + 1;
  char* d = (char*)k_malloc((unsigned)n);
  if (d) memcpy(d, s, n);
  return d;
}
static WINAPI void* ms_memcpy(void* d, const void* s, size_t n) { return memcpy(d, s, n); }
static WINAPI void* ms_memmove(void* d, const void* s, size_t n) { return memmove(d, s, n); }
static WINAPI void* ms_memset(void* d, int c, size_t n) { return memset(d, c, n); }
static WINAPI int ms_memcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
static WINAPI void* ms_memchr(const void* s, int c, size_t n) { return memchr(s, c, n); }
static WINAPI size_t ms_wcslen(const wchar16* s) { return wstrlen16(s); }
static WINAPI int ms_atoi(const char* s) { return atoi(s); }
static WINAPI long ms_strtol(const char* s, char** e, int b) { return (long)strtol(s, e, b); }
static WINAPI unsigned long ms_strtoul(const char* s, char** e, int b) { return (unsigned long)strtoul(s, e, b); }
/* 64 bits (long do Windows tem 32: os de 64 sao os "ll" e _strtoi64) */
static WINAPI unsigned long long ms_strtoull(const char* s, char** e, int b) {
  unsigned long long v = 0;
  const char* p = s;
  int neg = 0, any = 0;
  while (*p == ' ' || *p == '	') p++;
  if (*p == '-' || *p == '+') neg = *p++ == '-';
  if ((b == 0 || b == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    p += 2;
    b = 16;
  } else if (b == 0) {
    b = p[0] == '0' ? 8 : 10;
  }
  for (;; p++) {
    int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'z' ? *p - 'a' + 10
          : *p >= 'A' && *p <= 'Z' ? *p - 'A' + 10 : 99;
    if (d >= b) break;
    v = v * (unsigned)b + (unsigned)d;
    any = 1;
  }
  if (e) *e = (char*)(any ? p : s);
  return neg ? (unsigned long long)(-(long long)v) : v;
}
static WINAPI long long ms_strtoll(const char* s, char** e, int b) { return (long long)ms_strtoull(s, e, b); }
static WINAPI double ms_strtod(const char* s, char** e) { return strtod(s, e); }
static WINAPI double ms_atof(const char* s) { return strtod(s, 0); }
static WINAPI int ms_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static WINAPI int ms_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static WINAPI int ms_isdigit(int c) { return c >= '0' && c <= '9'; }
static WINAPI int ms_isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static WINAPI int ms_isalpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static WINAPI char* ms_getenv(const char* n) {
  /* O valor fica no heap do programa: o ucrt devolve ponteiro estavel. */
  char tmp[260];
  int len = k_getenv(n, tmp, (int)sizeof(tmp));
  char* d;
  if (len < 0) return 0;
  d = (char*)k_malloc((unsigned)len + 1);
  if (d) memcpy(d, tmp, (size_t)len + 1);
  return d;
}
static WINAPI void ms_qsort(void* base, size_t n, size_t sz, int (WINAPI* cmp)(const void*, const void*)) {
  /* insercao: simples e estavel; W0 nao tem programa que ordene muito */
  char* b = (char*)base;
  char* t = (char*)k_malloc((unsigned)sz);
  size_t i, j;
  if (!t) return;
  for (i = 1; i < n; i++) {
    memcpy(t, b + i * sz, sz);
    for (j = i; j > 0 && cmp(b + (j - 1) * sz, t) > 0; j--) memcpy(b + j * sz, b + (j - 1) * sz, sz);
    memcpy(b + j * sz, t, sz);
  }
  k_free(t);
}

/* ---------- math / tempo ---------- */
static WINAPI double ms_frexp(double x, int* e) { return frexp(x, e); }
static WINAPI double ms_sqrt(double x) { return sqrt(x); }
static WINAPI double ms_floor(double x) { return floor(x); }
static WINAPI double ms_ceil(double x) { return ceil(x); }
static WINAPI double ms_fabs(double x) { return fabs(x); }
static WINAPI double ms_pow(double a, double b) { return pow(a, b); }
static int tz_daylight;
static long tz_timezone;
static char* tz_names[2] = {"UTC", "UTC"};
static WINAPI int* __daylight(void) { return &tz_daylight; }
static WINAPI long* __timezone(void) { return &tz_timezone; }
static WINAPI char** __tzname(void) { return tz_names; }
static WINAPI void _tzset(void) {}
static WINAPI int64_t _time64(int64_t* t) {
  int d = k_date(), h = k_time();
  int64_t y, m, dd, days, s;
  if (d <= 0) d = 20260101;
  if (h < 0) h = 0;
  y = d / 10000;
  m = d / 100 % 100;
  dd = d % 100;
  y -= m <= 2;
  days = 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1 - 719468;
  s = days * 86400 + (h / 10000) * 3600 + (h / 100 % 100) * 60 + h % 100;
  if (t) *t = s;
  return s;
}
static WINAPI long ms_clock(void) { return (long)k_ms(); }

/* msvcrt antigo: variaveis importadas como DADO (o slot do import recebe o
 * endereco da variavel). Sao do DLL, nao do processo: valem para o EXE que
 * esta rodando (crt_set_process, chamado no inicio de cada processo). */
static int crt_fmode, crt_commode, crt_argc_v;
static char* crt_acmdln_v;
static wchar16* crt_wcmdln_v;
static char** crt_argv_v;
static char** crt_environ_v;
void crt_set_process(struct wproc* p) {
  crt_acmdln_v = p->cmdline;
  crt_wcmdln_v = p->wcmdline;
  crt_argc_v = p->argc;
  crt_argv_v = p->argv;
  crt_environ_v = p->environ;
  crt_fmode = 0;
  crt_commode = 0;
}
static WINAPI int _XcptFilter(unsigned long code, void* ep) {
  (void)code;
  (void)ep;
  return 0; /* EXCEPTION_CONTINUE_SEARCH */
}
static WINAPI int ms__purecall(void) {
  wsay("chamada de funcao virtual pura", 0);
  wexit(3);
}
static WINAPI void* ms_memcpy_s(void* d, size_t dn, const void* s2, size_t n) {
  if (n > dn) n = dn;
  memcpy(d, s2, n);
  return 0;
}

#define S(n) {#n, (void*)n}
#define M(n) {#n, (void*)ms_##n}

const struct wine_sym crt_syms[] = {
    /* stdio */
    S(__acrt_iob_func), S(__stdio_common_vfprintf), S(__stdio_common_vfwprintf),
    S(__stdio_common_vsprintf), S(__stdio_common_vsnprintf_s), S(__stdio_common_vswprintf),
    M(printf), M(fprintf), M(sprintf), {"_snprintf", (void*)ms_snprintf}, M(snprintf), M(vprintf),
    M(vfprintf), {"_vsnprintf", (void*)ms_vsnprintf}, M(vsnprintf), M(vsprintf), M(fwrite), M(fputs),
    M(puts), M(fputc), {"putc", (void*)ms_fputc}, M(putchar), M(fflush), M(fgetc), {"getc", (void*)ms_fgetc},
    M(getchar), M(fgets), M(setvbuf), {"_fileno", (void*)ms_fileno}, {"_isatty", (void*)ms_isatty},
    {"_setmode", (void*)ms_setmode}, {"_write", (void*)ms_write},
    /* runtime */
    S(__p___argc), S(__p___argv), S(__p___wargv), S(__p__environ), S(__p__wenviron), S(__p__commode),
    S(__p__fmode), S(_configure_narrow_argv), S(_configure_wide_argv),
    S(_initialize_narrow_environment), S(_initialize_wide_environment),
    S(_get_initial_narrow_environment), S(_get_initial_wide_environment), S(_set_app_type),
    S(_set_fmode), S(_set_new_mode), S(_set_invalid_parameter_handler), S(__setusermatherr),
    S(_fpreset), S(_controlfp), S(_controlfp_s), S(_seh_filter_exe), S(__C_specific_handler),
    S(_initterm), S(_initterm_e), S(_crt_atexit), S(_crt_at_quick_exit), {"atexit", (void*)_crt_atexit},
    {"_onexit", (void*)ms_onexit}, S(_cexit), S(_c_exit), M(exit), {"_exit", (void*)ms__exit},
    {"quick_exit", (void*)ms__exit}, M(abort), M(signal), S(_errno), S(_invalid_parameter_noinfo),
    S(_invalid_parameter_noinfo_noreturn), S(__getmainargs), S(__wgetmainargs), S(__set_app_type),
    {"_iob", (void*)iob}, {"__initenv", (void*)&crt_environ_v},
    {"_fmode", (void*)&crt_fmode}, {"_commode", (void*)&crt_commode}, {"_acmdln", (void*)&crt_acmdln_v},
    {"_wcmdln", (void*)&crt_wcmdln_v}, {"__argc", (void*)&crt_argc_v}, {"__argv", (void*)&crt_argv_v},
    {"_environ", (void*)&crt_environ_v}, S(_XcptFilter), {"_purecall", (void*)ms__purecall},
    {"memcpy_s", (void*)ms_memcpy_s},
    /* heap */
    M(malloc), M(calloc), M(free), M(realloc), {"_msize", (void*)ms_msize}, S(_aligned_malloc),
    S(_aligned_free),
    /* string */
    M(strlen), M(strnlen), M(strcmp), M(strncmp), M(strcpy), M(strncpy), M(strcat), M(strchr),
    M(strrchr), M(strstr), {"_strdup", (void*)ms_strdup}, M(strdup), M(memcpy), M(memmove), M(memset),
    M(memcmp), M(memchr), M(wcslen), M(atoi), M(strtol), M(strtoul), M(strtoull), M(strtoll), {"_strtoui64", (void*)ms_strtoull}, {"_strtoi64", (void*)ms_strtoll}, M(strtod), M(atof), M(toupper),
    M(tolower), M(isdigit), M(isspace), M(isalpha), M(getenv), M(qsort),
    /* math / tempo */
    M(frexp), M(sqrt), M(floor), M(ceil), M(fabs), M(pow), S(__daylight), S(__timezone), S(__tzname),
    S(_tzset), S(_time64), {"time", (void*)_time64}, M(clock),
    {0, 0}};

