/* strings.h (SODOS) */
#ifndef _STRINGS_H
#define _STRINGS_H
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);
int ffs(int v);
#ifdef __cplusplus
}
#endif
#endif
