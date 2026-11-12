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
#if !defined(__SIZEOF_POINTER__) || __SIZEOF_POINTER__ != 8
/* x32: o SODOS.DLL nao enxerga o crt0 do EXE; usa o padrao do kernel. */
#define __sodos_stack_top ((char*)0)
#define SO_STACK_SIZE ((size_t)0)
#elif defined(SODOS_DYNAMIC)
extern char* __sodos_stack_top;
/* libc.so.6: o tamanho vem do kernel (PT_GNU_STACK), lido em libs/dso/start.c. */
extern unsigned long __sodos_stack_size_v;
#define SO_STACK_SIZE ((size_t)__sodos_stack_size_v)
#else
extern char* __sodos_stack_top;
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
#include <netdb.h>
#include <string.h>

/* O NSS abre o softoken e o freebl por dlopen. No FIREFOX.EXE eles estao
 * ligados dentro (sodos/scripts/link-firefox64.sh): o dlopen devolve um
 * handle falso e o dlsym, as funcoes de entrada. Weak: outro programa liga
 * sem eles. */
/* Do softoken: sdb_measureAccess cria uma pasta temporaria; a auditoria
 * FIPS escreve no syslog (nao ha). */
#include <errno.h>
#include <sys/stat.h>
#include <stdarg.h>
extern unsigned k_ms(void);
char* mkdtemp(char* tpl) {
  static unsigned seq;
  size_t n = tpl ? strlen(tpl) : 0;
  int tries;
  if (n < 6 || strcmp(tpl + n - 6, "XXXXXX") != 0) {
    errno = EINVAL;
    return 0;
  }
  for (tries = 0; tries < 100; tries++) {
    unsigned v = k_ms() * 2654435761u + ++seq;
    int i;
    for (i = 0; i < 6; i++) {
      tpl[n - 6 + i] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[v % 36u];
      v /= 36u;
    }
    if (mkdir(tpl, 0700) == 0) return tpl;
  }
  return 0;
}
void syslog(int pri, const char* fmt, ...) {
  (void)pri;
  (void)fmt;
}

#define DL_SELF ((void*)1)
#define DL_SOFTOKN ((void*)2)
#define DL_FREEBL ((void*)3)
extern void* NSC_GetFunctionList(void*) __attribute__((weak));
extern void* FC_GetFunctionList(void*) __attribute__((weak));
extern void* NSC_GetInterface(void*, void*, void*, unsigned long) __attribute__((weak));
extern void* FC_GetInterface(void*, void*, void*, unsigned long) __attribute__((weak));
extern void* NSC_ModuleDBFunc(unsigned long, char*, void*) __attribute__((weak));
extern const void* FREEBL_GetVector(void) __attribute__((weak));

void* dlopen(const char* f, int m) {
  (void)m;
  if (!f) return DL_SELF;
  if (strstr(f, "softokn3") && NSC_GetFunctionList) return DL_SOFTOKN;
  if (strstr(f, "freebl") && FREEBL_GetVector) return DL_FREEBL;
  return 0;
}

/* O NSPR do Firefox (_PR_INET6_PROBE) procura o getaddrinfo por dlsym; sem
 * ele cai no gethostbyname_r e todo nome falha. So o que alguem procura por
 * nome entra aqui. */
void* dlsym(void* h, const char* n) {
  if (!n) return 0;
  if (h == DL_SOFTOKN) {
    if (!strcmp(n, "NSC_GetFunctionList")) return (void*)NSC_GetFunctionList;
    if (!strcmp(n, "FC_GetFunctionList")) return (void*)FC_GetFunctionList;
    if (!strcmp(n, "NSC_GetInterface")) return (void*)NSC_GetInterface;
    if (!strcmp(n, "FC_GetInterface")) return (void*)FC_GetInterface;
    if (!strcmp(n, "NSC_ModuleDBFunc")) return (void*)NSC_ModuleDBFunc;
    return 0;
  }
  if (h == DL_FREEBL) {
    if (!strcmp(n, "FREEBL_GetVector")) return (void*)FREEBL_GetVector;
    return 0;
  }
  if (!strcmp(n, "getaddrinfo")) return (void*)getaddrinfo;
  if (!strcmp(n, "freeaddrinfo")) return (void*)freeaddrinfo;
  if (!strcmp(n, "getnameinfo")) return (void*)getnameinfo;
  if (!strcmp(n, "gai_strerror")) return (void*)gai_strerror;
  return 0;
}
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
