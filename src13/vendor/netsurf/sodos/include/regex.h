/* regex.h - sem regex no SODOS: regcomp sempre falha (REG_ESPACE). */
#ifndef _SODOS_REGEX_H
#define _SODOS_REGEX_H
#include <stddef.h>
typedef struct { size_t re_nsub; void* priv; } regex_t;
typedef int regoff_t;
typedef struct { regoff_t rm_so, rm_eo; } regmatch_t;
#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NOSUB 4
#define REG_NEWLINE 8
#define REG_NOMATCH 1
#define REG_ESPACE 12
int regcomp(regex_t* re, const char* pat, int flags);
int regexec(const regex_t* re, const char* s, size_t n, regmatch_t* m, int flags);
size_t regerror(int err, const regex_t* re, char* buf, size_t size);
void regfree(regex_t* re);
#endif
