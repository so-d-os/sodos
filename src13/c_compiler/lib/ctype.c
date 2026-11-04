#include <locale.h>
/* ctype.c - <ctype.h> (somente ASCII) */
#include <ctype.h>

int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isblank(int c) { return c == ' ' || c == '\t'; }
int isascii(int c) { return (unsigned)c <= 0x7f; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

int isalnum_l(int c, locale_t loc) { (void)loc; return isalnum(c); }
int isalpha_l(int c, locale_t loc) { (void)loc; return isalpha(c); }
int isblank_l(int c, locale_t loc) { (void)loc; return isblank(c); }
int iscntrl_l(int c, locale_t loc) { (void)loc; return iscntrl(c); }
int isdigit_l(int c, locale_t loc) { (void)loc; return isdigit(c); }
int isgraph_l(int c, locale_t loc) { (void)loc; return isgraph(c); }
int islower_l(int c, locale_t loc) { (void)loc; return islower(c); }
int isprint_l(int c, locale_t loc) { (void)loc; return isprint(c); }
int ispunct_l(int c, locale_t loc) { (void)loc; return ispunct(c); }
int isspace_l(int c, locale_t loc) { (void)loc; return isspace(c); }
int isupper_l(int c, locale_t loc) { (void)loc; return isupper(c); }
int isxdigit_l(int c, locale_t loc) { (void)loc; return isxdigit(c); }

int toupper_l(int c, locale_t l) { (void)l; return toupper(c); }
int tolower_l(int c, locale_t l) { (void)l; return tolower(c); }
