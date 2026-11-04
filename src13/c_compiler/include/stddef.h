#ifndef _STDDEF_H
#define _STDDEF_H

/* x32: size_t/ptrtoint de 4 bytes; LP64 (gcc -m64): 8 bytes. O cc do SO
 * nao define __SIZEOF_POINTER__, entao o padrao continua 32 bits. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef long ssize_t;
#else
typedef unsigned int size_t;
typedef int ptrdiff_t;
typedef int ssize_t;
#endif
#ifndef __cplusplus
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
typedef int wchar_t;
#else
typedef unsigned short wchar_t;
#endif
#endif

/* C11: tipo com o maior alinhamento exigido por qualquer objeto. */
typedef struct {
  long long __ll;
  long double __ld;
} max_align_t;

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void*)0)
#endif
#endif

#ifdef __GNUC__
#define offsetof(t, m) __builtin_offsetof(t, m)
#else
#define offsetof(t, m) ((size_t)&((t*)0)->m)
#endif

#endif
