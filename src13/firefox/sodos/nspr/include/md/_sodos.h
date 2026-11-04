/* _sodos.h - definicoes de plataforma do NSPR para o SODOS.
 *
 * Equivale ao trecho _PR_PTHREADS do _linux.h, sem glibc: threads sao os
 * pthreads cooperativos da libsodos, o poll e o da libsodos e nao ha
 * dlopen/sysv sem/ipv6 no primeiro milestone.
 */
#ifndef nspr_sodos_defs_h___
#define nspr_sodos_defs_h___

#include "prthread.h"
#include <sys/time.h>
#include <sys/select.h>
#include <poll.h>

#define PR_LINKER_ARCH "sodos"
#define _PR_SI_SYSNAME "SODOS"
#define _PR_SI_ARCHITECTURE "x86-64"
#define PR_DLL_SUFFIX ".dll"

#define _PR_VMBASE 0x30000000
#define _PR_STACK_VMBASE 0x50000000
#define _MD_DEFAULT_STACK_SIZE 262144L
#define _MD_MMAP_FLAGS MAP_PRIVATE

#undef HAVE_STACK_GROWING_UP

/* dlopen sobre k_dll_* (sodos/nspr/shim.c). */
#define HAVE_DLL
#define USE_DLFCN

/* Atomics por builtins do gcc (x32 tambem suporta). */
#define _PR_HAVE_ATOMIC_OPS
#define _MD_INIT_ATOMIC()
#define _MD_ATOMIC_INCREMENT(p) __sync_add_and_fetch((p), 1)
#define _MD_ATOMIC_DECREMENT(p) __sync_sub_and_fetch((p), 1)
#define _MD_ATOMIC_ADD(p, v) __sync_add_and_fetch((p), (v))
#define _MD_ATOMIC_SET(p, v) __sync_lock_test_and_set((p), (v))

#define _PR_HAVE_LARGE_OFF_T
#define _PR_STAT_HAS_ONLY_ST_ATIME
#define _PR_HAVE_CLOCK_MONOTONIC

extern void _MD_EarlyInit(void);
#define _MD_EARLY_INIT _MD_EarlyInit
extern void _PR_UnixInit(void);
#define _MD_FINAL_INIT _PR_UnixInit

/* select/poll: o NSPR em modo pthreads chama poll direto. */
extern int select(int, fd_set*, fd_set*, fd_set*, struct timeval*);
extern int poll(struct pollfd*, unsigned int, int);
#define _MD_SELECT select
#define _MD_POLL poll
#define _PR_POLL_AVAILABLE

#define _PR_HAVE_INET_NTOP

extern void _MD_CleanupBeforeExit(void);
#define _MD_CLEANUP_BEFORE_EXIT _MD_CleanupBeforeExit

/* definida em unix_errors.c e usada pelo ptio.c */
extern void _MD_linux_map_sendfile_error(int err);

#endif /* nspr_sodos_defs_h___ */
