/* wchar.h minimo para compilar ICU/libc++ no SODOS. */
#ifndef _WCHAR_H
#define _WCHAR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <locale.h>
#include <uchar.h>
#include <stdio.h>

typedef int wint_t;
#define WEOF (-1)

size_t wcslen(const wchar_t* s);
int wcscmp(const wchar_t* a, const wchar_t* b);
int wcsncmp(const wchar_t* a, const wchar_t* b, size_t n);
wchar_t* wcscpy(wchar_t* d, const wchar_t* s);
wchar_t* wcsncpy(wchar_t* d, const wchar_t* s, size_t n);
wchar_t* wcscat(wchar_t* d, const wchar_t* s);
wchar_t* wcschr(const wchar_t* s, wchar_t c);
wchar_t* wcsrchr(const wchar_t* s, wchar_t c);
wchar_t* wcsstr(const wchar_t* h, const wchar_t* n);
wchar_t* wmemchr(const wchar_t* s, wchar_t c, size_t n);
wchar_t* wmemcpy(wchar_t* d, const wchar_t* s, size_t n);
wchar_t* wmemmove(wchar_t* d, const wchar_t* s, size_t n);
int wmemcmp(const wchar_t* a, const wchar_t* b, size_t n);
wchar_t* wmemset(wchar_t* s, wchar_t c, size_t n);
wint_t btowc(int c);
int wctob(wint_t c);
int mbsinit(const mbstate_t* ps);
size_t mbrlen(const char* s, size_t n, mbstate_t* ps);
size_t mbrtowc(wchar_t* pwc, const char* s, size_t n, mbstate_t* ps);
size_t wcrtomb(char* s, wchar_t wc, mbstate_t* ps);
size_t mbsrtowcs(wchar_t* dst, const char** src, size_t len, mbstate_t* ps);
size_t wcsrtombs(char* dst, const wchar_t** src, size_t len, mbstate_t* ps);
size_t wcsnrtombs(char* dst, const wchar_t** src, size_t nwc, size_t len, mbstate_t* ps);
size_t mbsnrtowcs(wchar_t* dst, const char** src, size_t nms, size_t len, mbstate_t* ps);

wint_t fgetwc(FILE* f);
wint_t fputwc(wchar_t c, FILE* f);
wint_t getwc(FILE* f);
wint_t putwc(wchar_t c, FILE* f);
wint_t ungetwc(wint_t c, FILE* f);
int fwide(FILE* f, int mode);
int fwprintf(FILE* f, const wchar_t* fmt, ...);
int wprintf(const wchar_t* fmt, ...);
int swprintf(wchar_t* s, size_t n, const wchar_t* fmt, ...);
int vfwprintf(FILE* f, const wchar_t* fmt, va_list ap);
int vwprintf(const wchar_t* fmt, va_list ap);
int vswprintf(wchar_t* s, size_t n, const wchar_t* fmt, va_list ap);
int fwscanf(FILE* f, const wchar_t* fmt, ...);
int wscanf(const wchar_t* fmt, ...);
int swscanf(const wchar_t* s, const wchar_t* fmt, ...);

long wcstol(const wchar_t* s, wchar_t** e, int base);
unsigned long wcstoul(const wchar_t* s, wchar_t** e, int base);
long long wcstoll(const wchar_t* s, wchar_t** e, int base);
unsigned long long wcstoull(const wchar_t* s, wchar_t** e, int base);
double wcstod(const wchar_t* s, wchar_t** e);
float wcstof(const wchar_t* s, wchar_t** e);
long double wcstold(const wchar_t* s, wchar_t** e);
int wcscoll(const wchar_t* a, const wchar_t* b);
size_t wcsxfrm(wchar_t* d, const wchar_t* s, size_t n);
int wcscoll_l(const wchar_t* a, const wchar_t* b, locale_t loc);
size_t wcsxfrm_l(wchar_t* d, const wchar_t* s, size_t n, locale_t loc);
int iswalpha_l(wint_t c, locale_t loc);
int iswalnum_l(wint_t c, locale_t loc);
int iswblank_l(wint_t c, locale_t loc);
int iswcntrl_l(wint_t c, locale_t loc);
int iswdigit_l(wint_t c, locale_t loc);
int iswgraph_l(wint_t c, locale_t loc);
int iswlower_l(wint_t c, locale_t loc);
int iswprint_l(wint_t c, locale_t loc);
int iswpunct_l(wint_t c, locale_t loc);
int iswspace_l(wint_t c, locale_t loc);
int iswupper_l(wint_t c, locale_t loc);
int iswxdigit_l(wint_t c, locale_t loc);
wint_t towlower_l(wint_t c, locale_t loc);
wint_t towupper_l(wint_t c, locale_t loc);
size_t wcsftime(wchar_t* s, size_t max, const wchar_t* fmt, const struct tm* tm);

wchar_t* fgetws(wchar_t* s, int n, FILE* f);
int fputws(const wchar_t* s, FILE* f);
wint_t getwchar(void);
wint_t putwchar(wchar_t c);
int vfwscanf(FILE* f, const wchar_t* fmt, va_list ap);
int vswscanf(const wchar_t* s, const wchar_t* fmt, va_list ap);
int vwscanf(const wchar_t* fmt, va_list ap);
size_t wcscspn(const wchar_t* s, const wchar_t* reject);
wchar_t* wcsncat(wchar_t* d, const wchar_t* s, size_t n);
wchar_t* wcspbrk(const wchar_t* s, const wchar_t* accept);
size_t wcsspn(const wchar_t* s, const wchar_t* accept);
wchar_t* wcstok(wchar_t* s, const wchar_t* delim, wchar_t** save);
size_t wcstombs(char* dst, const wchar_t* src, size_t n);
size_t mbstowcs(wchar_t* dst, const char* src, size_t n);

#ifdef __cplusplus
}
#endif

#endif
