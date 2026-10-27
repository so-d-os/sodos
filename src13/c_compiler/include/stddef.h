#ifndef _STDDEF_H
#define _STDDEF_H

typedef unsigned int size_t;
typedef int ptrdiff_t;
typedef int ssize_t;
typedef unsigned short wchar_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifdef __GNUC__
#define offsetof(t, m) __builtin_offsetof(t, m)
#else
#define offsetof(t, m) ((size_t)&((t*)0)->m)
#endif

#endif
