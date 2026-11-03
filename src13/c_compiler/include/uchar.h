/* uchar.h - o minimo para a libc++ obter mbstate_t sem <wchar.h>.
 * O SODOS nao tem conversao multibyte; so o tipo e as funcoes basicas. */
#ifndef _UCHAR_H
#define _UCHAR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef struct {
  unsigned int __count;
  unsigned int __value;
} mbstate_t;

#ifndef __cplusplus
typedef unsigned short char16_t;
typedef unsigned int char32_t;
#endif

size_t mbrtoc16(char16_t* pc16, const char* s, size_t n, mbstate_t* ps);
size_t c16rtomb(char* s, char16_t c16, mbstate_t* ps);
size_t mbrtoc32(char32_t* pc32, const char* s, size_t n, mbstate_t* ps);
size_t c32rtomb(char* s, char32_t c32, mbstate_t* ps);

#ifdef __cplusplus
}
#endif

#endif
