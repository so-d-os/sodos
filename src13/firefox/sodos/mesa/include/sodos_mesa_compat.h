/* sodos_mesa_compat.h - incluido em toda unidade do Mesa/libdrm (-include).
 * Declara o que a libc do SODOS nao tem; as funcoes ficam em
 * sodos/mesa/sodos_mesa_port.c (ou ja existem no port do Firefox). */
#ifndef SODOS_MESA_COMPAT_H
#define SODOS_MESA_COMPAT_H
#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#else
/* So em C: em C++ estes headers do SODOS trazem o stdbool de C. */
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
void abort(void) __attribute__((noreturn));
void exit(int) __attribute__((noreturn));
#endif
int asprintf(char** out, const char* fmt, ...);
int vasprintf(char** out, const char* fmt, va_list ap);
int mknod(const char* path, mode_t mode, dev_t dev);
FILE* open_memstream(char** buf, size_t* size);
FILE* popen(const char* cmd, const char* mode);
int pclose(FILE* f);
void sync(void);
char* strchrnul(const char* s, int c);
int mincore(void* addr, size_t len, unsigned char* vec);
/* LLVM Support (Unix/Program.inc, Process.inc, Path.inc) */
struct rusage;
pid_t wait4(pid_t pid, int* status, int options, struct rusage* ru);
char* strsignal(int sig);
pid_t getsid(pid_t pid);
#ifndef __cplusplus
#include <pthread.h>
#include <sched.h>
int pthread_getaffinity_np(pthread_t t, size_t n, cpu_set_t* s);
#include <poll.h>
#include <signal.h>
int ppoll(struct pollfd* fds, nfds_t n, const struct timespec* tmo, const sigset_t* mask);
#endif
#ifndef __cplusplus
int clock_nanosleep(int clk, int flags, const struct timespec* req, struct timespec* rem);
#endif
#ifdef __cplusplus
}
#endif
#include <sys/sysmacros.h>
#ifndef SODOS_HAVE_NLINK_T
#define SODOS_HAVE_NLINK_T
typedef unsigned long nlink_t;
#endif
#ifndef _SC_ARG_MAX
#define _SC_ARG_MAX 0
#endif
#ifndef _SC_GETPW_R_SIZE_MAX
#define _SC_GETPW_R_SIZE_MAX 70
#endif
#ifndef EX_IOERR
#define EX_IOERR 74 /* sysexits.h; o LLVM ja guardou HAVE_SYSEXITS_H=0 */
#endif
#ifndef _POSIX_ARG_MAX
#define _POSIX_ARG_MAX 4096
#endif
#ifndef PRIiMAX
#define PRIiMAX "ji"
#endif
#ifndef PRIdMAX
#define PRIdMAX "jd"
#endif
#ifndef PRIuMAX
#define PRIuMAX "ju"
#endif
#ifndef PRIxMAX
#define PRIxMAX "jx"
#endif
#endif /* __ASSEMBLER__ */
#endif
