#include <locale.h>
/* wchar.c - funcoes largas minimas para libc++/ICU no SODOS. */
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

size_t wcslen(const wchar_t* s) { size_t n = 0; while (s && s[n]) n++; return n; }
int wcscmp(const wchar_t* a, const wchar_t* b) {
  while (*a && *a == *b) { a++; b++; }
  return (int)*a - (int)*b;
}
int wcsncmp(const wchar_t* a, const wchar_t* b, size_t n) {
  while (n && *a && *a == *b) { a++; b++; n--; }
  return n ? (int)*a - (int)*b : 0;
}
wchar_t* wcscpy(wchar_t* d, const wchar_t* s) { wchar_t* r = d; while ((*d++ = *s++)) {} return r; }
wchar_t* wcsncpy(wchar_t* d, const wchar_t* s, size_t n) {
  size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i];
  for (; i < n; i++) d[i] = 0; return d;
}
wchar_t* wcschr(const wchar_t* s, wchar_t c) {
  for (; *s; s++) if (*s == c) return (wchar_t*)s;
  return c ? NULL : (wchar_t*)s;
}
wchar_t* wcsrchr(const wchar_t* s, wchar_t c) {
  const wchar_t* r = NULL; for (; *s; s++) if (*s == c) r = s;
  return c ? (wchar_t*)r : (wchar_t*)s;
}
wchar_t* wmemcpy(wchar_t* d, const wchar_t* s, size_t n) {
  size_t i; for (i = 0; i < n; i++) d[i] = s[i]; return d;
}
wchar_t* wmemmove(wchar_t* d, const wchar_t* s, size_t n) {
  size_t i; if (d < s) for (i = 0; i < n; i++) d[i] = s[i];
  else for (i = n; i > 0; i--) d[i-1] = s[i-1];
  return d;
}
int wmemcmp(const wchar_t* a, const wchar_t* b, size_t n) {
  size_t i; for (i = 0; i < n; i++) if (a[i] != b[i]) return (int)a[i] - (int)b[i]; return 0;
}
wchar_t* wmemset(wchar_t* s, wchar_t c, size_t n) {
  size_t i; for (i = 0; i < n; i++) s[i] = c; return s;
}

static long long wtoi(const wchar_t* s, wchar_t** end, int base) {
  long long v = 0; int neg = 0;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  for (;; s++) {
    int d;
    if (*s >= '0' && *s <= '9') d = *s - '0';
    else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
    else break;
    if (d >= base) break;
    v = v * base + d;
  }
  if (end) *end = (wchar_t*)s;
  return neg ? -v : v;
}
long wcstol(const wchar_t* s, wchar_t** e, int b) { return (long)wtoi(s, e, b); }
unsigned long wcstoul(const wchar_t* s, wchar_t** e, int b) { return (unsigned long)wtoi(s, e, b); }
long long wcstoll(const wchar_t* s, wchar_t** e, int b) { return wtoi(s, e, b); }
unsigned long long wcstoull(const wchar_t* s, wchar_t** e, int b) { return (unsigned long long)wtoi(s, e, b); }
double wcstod(const wchar_t* s, wchar_t** e) { return (double)wtoi(s, e, 10); }
float wcstof(const wchar_t* s, wchar_t** e) { return (float)wtoi(s, e, 10); }
long double wcstold(const wchar_t* s, wchar_t** e) { return (long double)wtoi(s, e, 10); }

#include <stdarg.h>
int swprintf(wchar_t* s, size_t n, const wchar_t* fmt, ...) {
  (void)fmt; if (s && n) s[0] = 0; return 0;
}
int vswprintf(wchar_t* s, size_t n, const wchar_t* fmt, va_list ap) {
  (void)fmt; (void)ap; if (s && n) s[0] = 0; return 0;
}
int fwprintf(FILE* f, const wchar_t* fmt, ...) { (void)f; (void)fmt; return -1; }
int wprintf(const wchar_t* fmt, ...) { (void)fmt; return -1; }
int vfwprintf(FILE* f, const wchar_t* fmt, va_list ap) { (void)f; (void)fmt; (void)ap; return -1; }
int vwprintf(const wchar_t* fmt, va_list ap) { (void)fmt; (void)ap; return -1; }
wint_t fgetwc(FILE* f) { (void)f; return WEOF; }
wint_t fputwc(wchar_t c, FILE* f) { (void)c; (void)f; return WEOF; }
wint_t getwc(FILE* f) { return fgetwc(f); }
wint_t putwc(wchar_t c, FILE* f) { return fputwc(c, f); }
wint_t ungetwc(wint_t c, FILE* f) { (void)c; (void)f; return WEOF; }
wint_t getwchar(void) { return WEOF; }
wint_t putwchar(wchar_t c) { (void)c; return WEOF; }
wchar_t* fgetws(wchar_t* s, int n, FILE* f) { (void)s; (void)n; (void)f; return NULL; }
int fputws(const wchar_t* s, FILE* f) { (void)s; (void)f; return -1; }
int fwide(FILE* f, int mode) { (void)f; return mode; }

size_t wcsnrtombs(char* dst, const wchar_t** src, size_t nwc, size_t len, mbstate_t* ps) {
  (void)ps; size_t i = 0;
  while (i < nwc && i < len && (*src)[i]) { if (dst) dst[i] = (char)(*src)[i]; i++; }
  if (i < len && dst) dst[i] = 0;
  *src += i;
  return i;
}
size_t mbsnrtowcs(wchar_t* dst, const char** src, size_t nms, size_t len, mbstate_t* ps) {
  (void)ps; size_t i = 0;
  while (i < nms && i < len && (*src)[i]) { if (dst) dst[i] = (unsigned char)(*src)[i]; i++; }
  if (i < len && dst) dst[i] = 0;
  *src += i;
  return i;
}

int wcscoll_l(const wchar_t* a, const wchar_t* b, locale_t l) { (void)l; return wcscmp(a,b); }
size_t wcsxfrm_l(wchar_t* d, const wchar_t* s, size_t n, locale_t l) { (void)l; return (size_t)wcsncpy(d,s,n); }
int iswalpha_l(wint_t c, locale_t l) { (void)l; return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int iswalnum_l(wint_t c, locale_t l) { (void)l; return iswalpha_l(c,l) || (c >= '0' && c <= '9'); }
int iswblank_l(wint_t c, locale_t l) { (void)l; return c == ' ' || c == 9; }
int iswcntrl_l(wint_t c, locale_t l) { (void)l; return c < 32 || c == 127; }
int iswdigit_l(wint_t c, locale_t l) { (void)l; return c >= '0' && c <= '9'; }
int iswgraph_l(wint_t c, locale_t l) { (void)l; return c > 32 && c < 127; }
int iswlower_l(wint_t c, locale_t l) { (void)l; return c >= 'a' && c <= 'z'; }
int iswprint_l(wint_t c, locale_t l) { (void)l; return c >= 32 && c < 127; }
int iswpunct_l(wint_t c, locale_t l) { (void)l; return iswgraph_l(c,l) && !iswalnum_l(c,l); }
int iswspace_l(wint_t c, locale_t l) { (void)l; return c == ' ' || (c >= 9 && c <= 13); }
int iswupper_l(wint_t c, locale_t l) { (void)l; return c >= 'A' && c <= 'Z'; }
int iswxdigit_l(wint_t c, locale_t l) { (void)l; return iswdigit_l(c,l) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
wint_t towlower_l(wint_t c, locale_t l) { (void)l; return iswupper_l(c,l) ? c + 32 : c; }
wint_t towupper_l(wint_t c, locale_t l) { (void)l; return iswlower_l(c,l) ? c - 32 : c; }

wint_t btowc(int c) { return c == EOF ? WEOF : (wint_t)c; }
int wctob(wint_t c) { return c == WEOF ? EOF : (int)(unsigned char)c; }
int mbsinit(const mbstate_t* ps) { (void)ps; return 1; }
size_t mbrlen(const char* s, size_t n, mbstate_t* ps) { (void)ps; if (!s || !n) return 0; return s[0] ? 1 : 0; }
size_t mbrtowc(wchar_t* pwc, const char* s, size_t n, mbstate_t* ps) {
  (void)ps; if (!s) return 0; if (!n) return (size_t)-2; if (!*s) { if (pwc) *pwc = 0; return 0; }
  if (pwc) *pwc = (unsigned char)*s; return 1;
}
size_t wcrtomb(char* s, wchar_t wc, mbstate_t* ps) { (void)ps; if (!s) return 1; s[0] = (char)wc; return 1; }
size_t mbsrtowcs(wchar_t* d, const char** s, size_t n, mbstate_t* ps) {
  const char* p;
  size_t i = 0;
  (void)ps;
  if (!s || !*s) return (size_t)-1;
  p = *s;
  if (!d) {
    /* Consulta de tamanho: nao escreve e nao mexe em *s. */
    while (p[i]) i++;
    return i;
  }
  while (i < n && p[i]) { d[i] = (unsigned char)p[i]; i++; }
  if (i < n) { d[i] = 0; *s = 0; }
  else *s = p + i;
  return i;
}
size_t wcsrtombs(char* d, const wchar_t** s, size_t n, mbstate_t* ps) {
  const wchar_t* p;
  size_t i = 0;
  (void)ps;
  if (!s || !*s) return (size_t)-1;
  p = *s;
  if (!d) {
    /* Consulta de tamanho (usada por JS::EncodeUtf8ToNarrow). */
    while (p[i]) i++;
    return i;
  }
  while (i < n && p[i]) { d[i] = (char)p[i]; i++; }
  if (i < n) { d[i] = 0; *s = 0; }
  else *s = p + i;
  return i;
}
