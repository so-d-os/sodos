#ifndef _SODOS_COMPAT_STDIO_H
#define _SODOS_COMPAT_STDIO_H
#include_next <stdio.h>
void setbuf(FILE* f, char* buf);
#endif
