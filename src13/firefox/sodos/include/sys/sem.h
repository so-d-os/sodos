/* sys/sem.h (SODOS) - declaracoes; o SODOS nao tem semaforos System V. */
#ifndef _SYS_SEM_H
#define _SYS_SEM_H
#include <stddef.h>
#include <sys/ipc.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GETVAL 12
#define SETVAL 16
#define GETPID 11
#define SEM_UNDO 0x1000
struct sembuf {
  unsigned short sem_num;
  short sem_op;
  short sem_flg;
};
int semget(key_t key, int nsems, int flags);
int semop(int id, struct sembuf* ops, size_t n);
int semctl(int id, int num, int cmd, ...);
#ifdef __cplusplus
}
#endif
/* lote2 */
struct semid_ds {
  struct ipc_perm sem_perm;
  long sem_otime;
  long sem_ctime;
  unsigned long sem_nsems;
};

#endif
