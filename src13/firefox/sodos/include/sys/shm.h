/* sys/shm.h (SODOS) - declaracoes; nao ha IPC System V. */
#ifndef _SYS_SHM_H
#define _SYS_SHM_H
#include <stddef.h>
#include <sys/ipc.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SHM_RDONLY 010000
int shmget(key_t key, size_t size, int flags);
void* shmat(int id, const void* addr, int flags);
int shmdt(const void* addr);
int shmctl(int id, int cmd, void* buf);
#ifdef __cplusplus
}
#endif
#endif
