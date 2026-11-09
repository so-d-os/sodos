/* iconv.h - iconv minimo do SODOS (firefox/sodos/port/sodos_iconv.c).
 * Conjuntos: UTF-8, ASCII, ISO-8859-1 (LATIN1), UTF-16/UCS-2 e UTF-32/UCS-4
 * (LE/BE; sem sufixo = BE). Outros nomes falham com EINVAL em iconv_open. */
#ifndef _ICONV_H
#define _ICONV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* iconv_t;

iconv_t iconv_open(const char* tocode, const char* fromcode);
size_t iconv(iconv_t cd, char** inbuf, size_t* inbytesleft,
             char** outbuf, size_t* outbytesleft);
int iconv_close(iconv_t cd);

#ifdef __cplusplus
}
#endif

#endif
