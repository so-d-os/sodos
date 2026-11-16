/* sodos_mesa.c - o que o Mesa (GL em software do WebGL) usa e a libc do
 * SODOS nao tem. O Mesa entra no FIREFOX.EXE como um objeto so
 * (sodos/scripts/build-mesa.sh -> mesa_sodos.o); o dlopen("libEGL.so") do
 * port devolve as funcoes dele (sodos_extra.c). Ver docs/07-webgl.md. */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

extern int vasprintf(char** out, const char* fmt, va_list ap);

int asprintf(char** out, const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vasprintf(out, fmt, ap);
  va_end(ap);
  return r;
}

char* strchrnul(const char* s, int c) {
  while (*s && *s != (char)c) s++;
  return (char*)s;
}

/* Sem processos filhos nem nos de dispositivo: so caminhos de depuracao do
 * Mesa (ddebug, nomes de modificador do libdrm) chegam aqui. */
FILE* open_memstream(char** buf, size_t* size) {
  (void)buf;
  (void)size;
  errno = ENOSYS;
  return 0;
}
FILE* popen(const char* cmd, const char* mode) {
  (void)cmd;
  (void)mode;
  errno = ENOSYS;
  return 0;
}
int pclose(FILE* f) {
  (void)f;
  errno = ECHILD;
  return -1;
}
int mknod(const char* path, mode_t mode, dev_t dev) {
  (void)path;
  (void)mode;
  (void)dev;
  errno = EPERM;
  return -1;
}
void sync(void) {}

/* O llvmpipe so le a afinidade para repassar as threads de rasterizacao;
 * o SODOS nao fixa threads em CPU. */
int pthread_getaffinity_np(pthread_t t, size_t n, cpu_set_t* s) {
  (void)t;
  return sched_getaffinity(0, n, s);
}

/* O log do Mesa escolhe syslog se MESA_LOG=syslog; o syslog do port e mudo. */
void openlog(const char* ident, int opt, int facility) {
  (void)ident;
  (void)opt;
  (void)facility;
}

/* LLVM Support (o llvmpipe nao cria processos; so precisa ligar). */
struct rusage;
pid_t wait4(pid_t pid, int* status, int options, struct rusage* ru) {
  (void)pid;
  (void)status;
  (void)options;
  (void)ru;
  errno = ECHILD;
  return -1;
}
char* strsignal(int sig) {
  static char buf[24];
  snprintf(buf, sizeof(buf), "signal %d", sig);
  return buf;
}
pid_t getsid(pid_t pid) {
  (void)pid;
  return 1;
}

/* llvmpipe (lp_fence.c, espera de sync_file): so o prazo importa. */
#include <poll.h>
#include <signal.h>
#include <time.h>
int ppoll(struct pollfd* fds, nfds_t n, const struct timespec* tmo, const sigset_t* mask) {
  int ms = -1;
  (void)mask;
  if (tmo) ms = (int)(tmo->tv_sec * 1000 + tmo->tv_nsec / 1000000);
  return poll(fds, n, ms);
}

/* Flags de excecao de ponto flutuante (o NIR testa FE_INVALID ao dobrar
 * constantes). So o SSE (MXCSR) conta: o codigo LP64 nao usa o x87. */
#include <fenv.h>
static unsigned mxcsr_get(void) {
  unsigned v;
  __asm__ volatile("stmxcsr %0" : "=m"(v));
  return v;
}
static void mxcsr_set(unsigned v) { __asm__ volatile("ldmxcsr %0" : : "m"(v)); }
int feclearexcept(int excepts) {
  mxcsr_set(mxcsr_get() & ~((unsigned)excepts & FE_ALL_EXCEPT));
  return 0;
}
int fetestexcept(int excepts) { return (int)(mxcsr_get() & (unsigned)excepts & FE_ALL_EXCEPT); }

/* operator new(size_t, nothrow_t) e variantes. O Firefox troca o operator new
 * comum (mozalloc) mas nao o nothrow; a libc++ sem excecoes entao aborta no
 * primeiro new(nothrow) ("operator new(size_t, nothrow_t) has not been
 * overridden"). O LLVM do llvmpipe usa o nothrow ao gerar codigo. Nomes C++
 * mangled: este arquivo e C. */
extern int posix_memalign(void** out, size_t align, size_t n);
static void* nothrow_alloc(size_t n) { return malloc(n ? n : 1); }
static void* nothrow_aligned(size_t n, size_t a) {
  void* p = 0;
  if (a < sizeof(void*)) a = sizeof(void*);
  if (posix_memalign(&p, a, n ? n : 1) != 0) return 0;
  return p;
}
void* sodos_new_nt(size_t n, const void* nt) __asm__("_ZnwmRKSt9nothrow_t");
void* sodos_newa_nt(size_t n, const void* nt) __asm__("_ZnamRKSt9nothrow_t");
void* sodos_new_al_nt(size_t n, size_t a, const void* nt) __asm__("_ZnwmSt11align_val_tRKSt9nothrow_t");
void* sodos_newa_al_nt(size_t n, size_t a, const void* nt) __asm__("_ZnamSt11align_val_tRKSt9nothrow_t");
void* sodos_new_nt(size_t n, const void* nt) { (void)nt; return nothrow_alloc(n); }
void* sodos_newa_nt(size_t n, const void* nt) { (void)nt; return nothrow_alloc(n); }
void* sodos_new_al_nt(size_t n, size_t a, const void* nt) { (void)nt; return nothrow_aligned(n, a); }
void* sodos_newa_al_nt(size_t n, size_t a, const void* nt) { (void)nt; return nothrow_aligned(n, a); }
