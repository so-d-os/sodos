/* time.h - time do SODOS + difftime (double) e strptime (shim/posix_extra.c) */
#ifndef _SODOS_COMPAT_TIME_H
#define _SODOS_COMPAT_TIME_H
#include_next <time.h>
double difftime(time_t a, time_t b);
char* strptime(const char* s, const char* fmt, struct tm* tm);
#endif
