/* cxx_runtime.cpp - runtime C++ minimo do SODOS (fase M2a).
 *
 * O linker do SODOS agora entrega uma tabela contigua de construtores
 * (__init_array_start..__init_array_end); o crt0 chama __sodos_ctors antes
 * de main e __sodos_dtors depois. Aqui ficam tambem new/delete, os guards
 * de static local (__cxa_guard_*) e o registro de destrutores
 * (__cxa_atexit), que o g++ emite para objetos globais.
 *
 * Compilado com -fno-exceptions -fno-rtti: ainda nao ha unwinding DWARF
 * nem typeinfo.
 */
#include <stddef.h>
#include <stdint.h>

extern "C" {
void* malloc(size_t n);
void free(void* p);
void k_exit(int code);
/* libsodos (sodos_linux_abi.c): enderecos da pilha na serial, para simbolizar. */
void sodos_dump_stack(void) __attribute__((weak));

#ifndef SODOS_DYNAMIC
/* Na libc++.so.1 nada disto entra: os construtores quem roda e a libc.so.6
 * (libs/dso/start.c), que tambem tem __cxa_atexit, e __dso_handle e um por
 * modulo (libs/dso/crtdso.c). */
typedef void (*sodos_ctor_fn)(void);
extern sodos_ctor_fn __init_array_start[];
extern sodos_ctor_fn __init_array_end[];

void* __dso_handle = (void*)0;

struct sodos_atexit_ent {
  void (*fn)(void*);
  void* arg;
};

static sodos_atexit_ent s_atexit[128];
static int s_atexit_n;

void __sodos_ctors(void) {
  sodos_ctor_fn* p;
  for (p = __init_array_start; p < __init_array_end; p++) {
    if (*p) (*p)();
  }
}

void __sodos_dtors(void) {
  while (s_atexit_n > 0) {
    sodos_atexit_ent e = s_atexit[--s_atexit_n];
    if (e.fn) e.fn(e.arg);
  }
}

int __cxa_atexit(void (*fn)(void*), void* arg, void* dso) {
  (void)dso;
  if (s_atexit_n >= (int)(sizeof(s_atexit) / sizeof(s_atexit[0]))) return -1;
  s_atexit[s_atexit_n].fn = fn;
  s_atexit[s_atexit_n].arg = arg;
  s_atexit_n++;
  return 0;
}

void __cxa_finalize(void* dso) {
  (void)dso;
  __sodos_dtors();
}
#endif

/* static local: 8 bytes; byte 0 = concluido, byte 1 = em andamento. */
int __cxa_guard_acquire(uint64_t* guard) {
  volatile unsigned char* p = (volatile unsigned char*)guard;
  if (p[0]) return 0;
  p[1] = 1;
  return 1;
}

void __cxa_guard_release(uint64_t* guard) {
  volatile unsigned char* p = (volatile unsigned char*)guard;
  p[0] = 1;
  p[1] = 0;
}

void __cxa_guard_abort(uint64_t* guard) {
  volatile unsigned char* p = (volatile unsigned char*)guard;
  p[0] = 0;
  p[1] = 0;
}

void __cxa_pure_virtual(void) {
  if (sodos_dump_stack) sodos_dump_stack();
  k_exit(4);
  for (;;) {}
}

#ifndef SODOS_DYNAMIC
/* Sem threads reais morrendo: os destrutores de thread_local rodam na saida. */
int __cxa_thread_atexit(void (*fn)(void*), void* obj, void* dso) {
  return __cxa_atexit(fn, obj, dso);
}
#endif

/* Sem demangler: quem chama (abseil) mostra o nome como esta. */
char* __cxa_demangle(const char*, char*, size_t*, int* status) {
  if (status) *status = -2;
  return nullptr;
}

#ifndef SODOS_DYNAMIC
void __stack_chk_fail(void) {
  if (sodos_dump_stack) sodos_dump_stack();
  k_exit(5);
  for (;;) {}
}
#endif
}

void* operator new(size_t n) {
  void* p = malloc(n ? n : 1);
  if (!p) k_exit(6);
  return p;
}

void* operator new[](size_t n) { return operator new(n); }

void operator delete(void* p) noexcept { if (p) free(p); }
void operator delete[](void* p) noexcept { if (p) free(p); }
void operator delete(void* p, size_t) noexcept { if (p) free(p); }
void operator delete[](void* p, size_t) noexcept { if (p) free(p); }
