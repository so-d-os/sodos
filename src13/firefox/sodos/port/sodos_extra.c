#include <pthread.h>
#include <sys/prctl.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

int pthread_setname_np(pthread_t t, const char* name) {
  (void)t; (void)name;
  return 0;
}

int pthread_getname_np(pthread_t t, char* buf, size_t n) {
  (void)t;
  if (buf && n) buf[0] = 0;
  return 0;
}

int prctl(int option, ...) {
  (void)option;
  return 0;
}

/* Limites da pilha do programa: o crt0 registra o rsp inicial e o linker
 * define __sodos_stack_size (simbolo absoluto) com o tamanho pedido ao
 * loader. O SpiderMonkey usa isto para o limite de recursao. */
extern char* __sodos_stack_top;
#ifdef SODOS_DYNAMIC
/* libc.so.6: o tamanho vem do kernel (PT_GNU_STACK), lido em libs/dso/start.c. */
extern unsigned long __sodos_stack_size_v;
#define SO_STACK_SIZE ((size_t)__sodos_stack_size_v)
#else
extern char __sodos_stack_size[];
#define SO_STACK_SIZE ((size_t)(uintptr_t)__sodos_stack_size)
#endif
#define SO_STACK_HEADROOM 0x4000u /* argv/ambiente ficam no topo da pilha */

int pthread_attr_getstack(const pthread_attr_t* a, void** addr, size_t* size) {
  uintptr_t top = (uintptr_t)__sodos_stack_top;
  size_t sz = SO_STACK_SIZE;
  (void)a;
  if (!top || sz < 0x20000u) {
    /* Sem a informacao do loader: assume o padrao do kernel. */
    sz = 0x100000u;
    if (!top) top = (uintptr_t)&sz;
  }
  if (sz > SO_STACK_HEADROOM * 2) sz -= SO_STACK_HEADROOM;
  if (addr) *addr = (void*)(top - sz);
  if (size) *size = sz;
  return 0;
}
int pthread_getattr_np(pthread_t t, pthread_attr_t* a) {
  (void)t; (void)a; return 0;
}

int pthread_condattr_setclock(pthread_condattr_t* a, clockid_t id) { (void)a; (void)id; return 0; }

int waitpid_mangled(int pid, int* status, int options) __asm__("_Z7waitpidiPii");
int wait_mangled(int* status) __asm__("_Z4waitPi");
int waitpid_mangled(int pid, int* status, int options) { return waitpid(pid, status, options); }
int wait_mangled(int* status) { return wait(status); }

#include <setjmp.h>
void longjmp_mangled(void* b, int v) __asm__("_Z7longjmpPyi");
int setjmp_mangled(void* b) __asm__("_Z6setjmpPy");
void longjmp_mangled(void* b, int v) { longjmp(*(jmp_buf*)b, v); }
int setjmp_mangled(void* b) { return setjmp(*(jmp_buf*)b); }

#include <dlfcn.h>
#ifndef SODOS_DYNAMIC
/* Tudo esta no proprio executavel: dlopen(NULL) e o programa, o resto nao existe.
 * Na libc.so.6 o dlopen de verdade fica em libs/dso/dl.c. */
void* dlopen(const char* f, int m) { (void)m; return f ? 0 : (void*)1; }
void* dlsym(void* h, const char* n) { (void)h; (void)n; return 0; }
char* dlerror(void) { return (char*)"no dynamic loader"; }
int dlclose(void* h) { (void)h; return 0; }
#endif
void* dlopen_m(void* f, int m) __asm__("_Z6dlopenPKci");
void* dlsym_m(void* h, const char* n) __asm__("_Z5dlsymPvPKc");
char* dlerror_m(void) __asm__("_Z6dlerrorv");
int dlclose_m(void* h) __asm__("_Z7dlclosePv");
void* dlopen_m(void* f, int m) { return dlopen((const char*)f, m); }
void* dlsym_m(void* h, const char* n) { return dlsym(h, n); }
char* dlerror_m(void) { return dlerror(); }
int dlclose_m(void* h) { return dlclose(h); }

#include <sys/resource.h>
int getrusage(int who, struct rusage* u) { (void)who; if (u) { for (unsigned i = 0; i < sizeof(*u); i++) ((char*)u)[i] = 0; } return 0; }
int getrusage_m(int who, struct rusage* u) __asm__("_Z9getrusageiP6rusage");
int getrusage_m(int who, struct rusage* u) { return getrusage(who, u); }

#include <sys/time.h>
int gettimeofday_m(struct timeval* tv, void* tz) __asm__("_Z12gettimeofdayP7timevalPv");
int gettimeofday_m(struct timeval* tv, void* tz) { return gettimeofday(tv, tz); }

void* __dynamic_cast(const void* sub, const void* src, const void* dst, long hint) { (void)sub; (void)src; (void)dst; (void)hint; return 0; }

void __cxa_bad_typeid(void) { abort(); }
void __cxa_bad_cast(void) { abort(); }
void __cxa_pure_virtual(void) { abort(); }
void __cxa_throw(void* a, void* b, void (*c)(void*)) { (void)a; (void)b; (void)c; abort(); }
void* __cxa_allocate_exception(unsigned long n) { return __builtin_malloc(n); }
void __cxa_free_exception(void* p) { __builtin_free(p); }
void* __cxa_begin_catch(void* p) { return p; }
void __cxa_end_catch(void) {}
void __cxa_rethrow(void) { abort(); }
