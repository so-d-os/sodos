/* sodos_cxx.h - runtime C++ minimo do SODOS (M2a).
 *
 * Ainda NAO e a biblioteca padrao C++ (libc++/libstdc++): cobre o que o
 * compilador gera para classes, heranca virtual, templates, new/delete e
 * variaveis staticas com construtor. Inclua este header antes de usar
 * "new" sem <new>.
 */
#ifndef SODOS_CXX_H
#define SODOS_CXX_H

#include <stddef.h>

void* operator new(size_t n);
void* operator new[](size_t n);
void operator delete(void* p) noexcept;
void operator delete[](void* p) noexcept;
void operator delete(void* p, size_t n) noexcept;
void operator delete[](void* p, size_t n) noexcept;

extern "C" void __sodos_ctors(void);
extern "C" void __sodos_dtors(void);

#endif
