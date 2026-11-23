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
/* Mesa (EGL + GLES em software, sodos/scripts/build-mesa.sh): o GLLibraryEGL
 * do Firefox abre libEGL.so e libGLESv2.so e procura cada funcao por nome;
 * o eglGetProcAddress do Mesa acha as funcoes EGL e GL. */
#define DL_MESA ((void*)4)
extern void* sodos_mesa_eglGetProcAddress(const char*) __attribute__((weak));
/* FFmpeg (video H.264/HEVC/AAC/VP9, sodos/scripts/build-ffmpeg.sh): o
 * FFVPXRuntimeLinker abre libmozavutil.so e libmozavcodec.so e procura cada
 * funcao por nome; a tabela gerada (sodos_av_syms.c) responde. */
#define DL_AVUTIL ((void*)5)
#define DL_AVCODEC ((void*)6)
extern void* sodos_av_dlsym(const char*) __attribute__((weak));

void* dlopen(const char* f, int m) {
  (void)m;
  if (!f) return DL_SELF;
  if (strstr(f, "softokn3") && NSC_GetFunctionList) return DL_SOFTOKN;
  if (strstr(f, "freebl") && FREEBL_GetVector) return DL_FREEBL;
  if ((strstr(f, "libEGL") || strstr(f, "libGLESv2")) && sodos_mesa_eglGetProcAddress)
    return DL_MESA;
  if (sodos_av_dlsym && strstr(f, "mozavutil")) return DL_AVUTIL;
  if (sodos_av_dlsym && strstr(f, "mozavcodec")) return DL_AVCODEC;
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
  if (h == DL_MESA) return sodos_mesa_eglGetProcAddress(n);
  if (h == DL_AVUTIL || h == DL_AVCODEC) return sodos_av_dlsym(n);
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

/* ---- __dynamic_cast (ABI Itanium) ----------------------------------------
 * Antes devolvia sempre 0. O ICU usa dynamic_cast (udat_toPattern pergunta se
 * o formato e SimpleDateFormat): com 0 ele dava U_UNSUPPORTED_ERROR e todo
 * Intl.DateTimeFormat().resolvedOptions() / formatRange() lancava "internal
 * error while computing Intl data" (o WAWebClock do web.whatsapp.com parava).
 *
 * type_info (o compilador gera): { vptr; const char* name } e, conforme o
 * tipo, + base (si) ou + flags, base_count, bases[] (vmi). Os vtables de
 * __class_type_info etc. sao os do sodos_rtti.c; o tipo de cada type_info se
 * reconhece pelo vptr (= endereco do vtable + 16). */
extern char _ZTVN10__cxxabiv117__class_type_infoE[];
extern char _ZTVN10__cxxabiv120__si_class_type_infoE[];
extern char _ZTVN10__cxxabiv121__vmi_class_type_infoE[];

struct rt_ti {
  const void* vptr;
  const char* name;
};
struct rt_si {
  struct rt_ti ti;
  const struct rt_ti* base;
};
struct rt_base {
  const struct rt_ti* type;
  long offset_flags; /* bit 0 virtual, bit 1 public, offset >> 8 */
};
struct rt_vmi {
  struct rt_ti ti;
  unsigned flags;
  unsigned base_count;
  struct rt_base bases[1];
};

static int rt_same(const struct rt_ti* a, const struct rt_ti* b) {
  const char *x, *y;
  if (a == b) return 1;
  if (!a || !b) return 0;
  x = a->name;
  y = b->name;
  if (*x == '*') x++; /* nome local ("*N...") */
  if (*y == '*') y++;
  return strcmp(x, y) == 0;
}

/* Procura em obj (tipo t) os subobjetos de tipo dst. Grava ate max em out;
 * devolve quantos achou (contando repetidos de base virtual uma vez so). */
static int rt_find(const char* obj, const struct rt_ti* t, const struct rt_ti* dst, const char** out,
                   int n, int max, int depth) {
  const char* v = (const char*)t->vptr;
  int i;
  if (depth > 64) return n;
  if (rt_same(t, dst)) {
    for (i = 0; i < n; i++)
      if (out[i] == obj) return n;
    if (n < max) out[n] = obj;
    return n + 1;
  }
  if (v == _ZTVN10__cxxabiv120__si_class_type_infoE + 16)
    return rt_find(obj, ((const struct rt_si*)t)->base, dst, out, n, max, depth + 1);
  if (v == _ZTVN10__cxxabiv121__vmi_class_type_infoE + 16) {
    const struct rt_vmi* m = (const struct rt_vmi*)t;
    unsigned k;
    for (k = 0; k < m->base_count; k++) {
      long of = m->bases[k].offset_flags, off = of >> 8;
      const char* b;
      if (of & 1) { /* base virtual: o deslocamento esta no vtable do obj */
        const char* vt = *(const char* const*)obj;
        b = obj + *(const long*)(vt + off);
      } else {
        b = obj + off;
      }
      n = rt_find(b, m->bases[k].type, dst, out, n, max, depth + 1);
    }
  }
  return n;
}

void* __dynamic_cast(const void* sub, const void* src, const void* dst, long hint) {
  const char* vt;
  const char* whole;
  const struct rt_ti* wt;
  const char* found[8];
  int n, i;
  (void)hint;
  if (!sub) return 0;
  vt = *(const char* const*)sub;
  whole = (const char*)sub + ((const long*)vt)[-2]; /* offset-to-top */
  wt = ((const struct rt_ti* const*)vt)[-1];        /* tipo do objeto inteiro */
  if (!wt || !dst) return 0;
  n = rt_find(whole, wt, (const struct rt_ti*)dst, found, 0, 8, 0);
  if (n == 1) return (void*)found[0];
  if (n > 1 && n <= 8) {
    /* varias bases dst: a que contem o sub de tipo src (descida a partir dele) */
    for (i = 0; i < n; i++) {
      const char* s[8];
      int m = rt_find(found[i], (const struct rt_ti*)dst, (const struct rt_ti*)src, s, 0, 8, 0), j;
      for (j = 0; j < m && j < 8; j++)
        if (s[j] == (const char*)sub) return (void*)found[i];
    }
  }
  return 0; /* nao e desse tipo, ou ambiguo */
}

void __cxa_bad_typeid(void) { abort(); }
void __cxa_bad_cast(void) { abort(); }
void __cxa_pure_virtual(void) { abort(); }
void __cxa_throw(void* a, void* b, void (*c)(void*)) { (void)a; (void)b; (void)c; abort(); }
void* __cxa_allocate_exception(unsigned long n) { return __builtin_malloc(n); }
void __cxa_free_exception(void* p) { __builtin_free(p); }
void* __cxa_begin_catch(void* p) { return p; }
void __cxa_end_catch(void) {}
void __cxa_rethrow(void) { abort(); }
