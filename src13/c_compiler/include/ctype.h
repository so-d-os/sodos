#ifndef _CTYPE_H
#define _CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif
#include <locale.h>

int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);
int isblank(int c);
int isascii(int c);
int toupper_l(int c, locale_t loc);
int tolower_l(int c, locale_t loc);
int isalnum_l(int c, locale_t loc);
int isalpha_l(int c, locale_t loc);
int isblank_l(int c, locale_t loc);
int iscntrl_l(int c, locale_t loc);
int isdigit_l(int c, locale_t loc);
int isgraph_l(int c, locale_t loc);
int islower_l(int c, locale_t loc);
int isprint_l(int c, locale_t loc);
int ispunct_l(int c, locale_t loc);
int isspace_l(int c, locale_t loc);
int isupper_l(int c, locale_t loc);
int isxdigit_l(int c, locale_t loc);

int isupper(int c);
int islower(int c);
int isxdigit(int c);
int isprint(int c);
int ispunct(int c);
int iscntrl(int c);
int isgraph(int c);
int toupper(int c);
int tolower(int c);


#ifdef __cplusplus
}
#endif
#endif
