/* regex.h - o SODOS nao tem regex; o driconf (xmlconfig) so o usa para casar
 * nomes de programa, entao nada casa. */
#ifndef _SODOS_REGEX_H
#define _SODOS_REGEX_H
typedef int regex_t;
#define REG_EXTENDED 0
#define REG_NOSUB 0
#define REG_NOMATCH 1
static inline int regcomp(regex_t* r, const char* s, int f) { (void)r; (void)s; (void)f; return 0; }
static inline int regexec(const regex_t* r, const char* s, int n, void* p, int f) { (void)r; (void)s; (void)n; (void)p; (void)f; return REG_NOMATCH; }
static inline void regfree(regex_t* r) { (void)r; }
#endif
