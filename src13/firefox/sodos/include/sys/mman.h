/* sys/mman.h - mmap anonimo do SODOS (libsodos).
 *
 * Fase M0: MAP_ANONYMOUS|MAP_PRIVATE e atendido pelo heap do kernel
 * (k_malloc); arquivo mapeado ainda nao e suportado (ENOSYS). Sem demanda de
 * paginas e sem protecao: mprotect e um no-op. jemalloc deve ser desligado
 * no build do Gecko (--disable-jemalloc) enquanto for assim.
 */
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS

#define MAP_FAILED ((void*)-1)

#define MS_ASYNC 1
#define MS_SYNC 4
#define MS_INVALIDATE 2

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void* addr, size_t len);
int madvise(void* addr, size_t len, int advice);
int mprotect(void* addr, size_t len, int prot);
int msync(void* addr, size_t len, int flags);

#ifdef __cplusplus
}
#endif

#endif
