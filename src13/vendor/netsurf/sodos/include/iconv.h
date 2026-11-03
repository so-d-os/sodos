/* iconv.h - iconv minimo do port do NetSurf (sodos/shim/iconv.c). */
#ifndef _SODOS_ICONV_H
#define _SODOS_ICONV_H
#include <stddef.h>
typedef void* iconv_t;
iconv_t iconv_open(const char* tocode, const char* fromcode);
size_t iconv(iconv_t cd, char** inbuf, size_t* inleft, char** outbuf, size_t* outleft);
int iconv_close(iconv_t cd);
#endif
