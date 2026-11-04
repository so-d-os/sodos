#ifndef _LOCALE_H
#define _LOCALE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* So existe o locale "C" (ponto decimal '.'). */
#define LC_CTYPE 0
#define LC_NUMERIC 1
#define LC_TIME 2
#define LC_COLLATE 3
#define LC_MONETARY 4
#define LC_MESSAGES 5
typedef void* locale_t;
#define MB_CUR_MAX 1
#define LC_ALL 6
#define LC_COLLATE_MASK  (1 << 0)
#define LC_CTYPE_MASK    (1 << 1)
#define LC_MONETARY_MASK (1 << 2)
#define LC_NUMERIC_MASK  (1 << 3)
#define LC_TIME_MASK     (1 << 4)
#define LC_MESSAGES_MASK (1 << 5)
#define LC_ALL_MASK      0x3f

struct lconv {
  char* decimal_point;
  char* thousands_sep;
  char* grouping;
  char* int_curr_symbol;
  char* currency_symbol;
  char* mon_decimal_point;
  char* mon_thousands_sep;
  char* mon_grouping;
  char* positive_sign;
  char* negative_sign;
  char int_frac_digits;
  char frac_digits;
  char int_p_cs_precedes;
  char int_n_cs_precedes;
  char int_p_sep_by_space;
  char int_n_sep_by_space;
  char int_p_sign_posn;
  char int_n_sign_posn;
  char p_cs_precedes;
  char p_sep_by_space;
  char n_cs_precedes;
  char n_sep_by_space;
  char p_sign_posn;
  char n_sign_posn;
};

char* setlocale(int category, const char* locale);
struct lconv* localeconv(void);
locale_t newlocale(int mask, const char* locale, locale_t base);
locale_t duplocale(locale_t loc);
void freelocale(locale_t loc);
locale_t uselocale(locale_t loc);
long long strtoll_l(const char* s, char** end, int base, locale_t loc);
unsigned long long strtoull_l(const char* s, char** end, int base, locale_t loc);
double strtod_l(const char* s, char** end, locale_t loc);
float strtof_l(const char* s, char** end, locale_t loc);
size_t strftime_l(char* s, size_t max, const char* fmt, const struct tm* tm, locale_t loc);
int strcoll_l(const char* a, const char* b, locale_t loc);
size_t strxfrm_l(char* d, const char* s, size_t n, locale_t loc);
long double strtold_l(const char* s, char** end, locale_t loc);


#ifdef __cplusplus
}
#endif
#endif
