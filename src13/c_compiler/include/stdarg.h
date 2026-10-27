#ifndef _STDARG_H
#define _STDARG_H

#ifdef __GNUC__
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, t) __builtin_va_arg(ap, t)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(d, s) __builtin_va_copy(d, s)
#else
/* cc: argumentos cdecl na pilha, cada um ocupa multiplo de 4 bytes. */
typedef char* va_list;
#define __va_size(t) ((sizeof(t) + 3) & ~3)
#define va_start(ap, last) ((ap) = (char*)&(last) + __va_size(last))
#define va_arg(ap, t) (*(t*)(((ap) += __va_size(t)) - __va_size(t)))
#define va_end(ap) ((ap) = 0)
#define va_copy(d, s) ((d) = (s))
#endif

#endif
