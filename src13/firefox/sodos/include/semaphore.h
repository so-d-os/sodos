/* semaphore.h - semaforos POSIX do SODOS (libsodos). */
#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int value;
  void* wait_head;
  void* wait_tail;
  unsigned magic;
} sem_t;

#define SEM_FAILED ((sem_t*)0)
#define SEM_VALUE_MAX 0x7FFFFFFF

int sem_init(sem_t* s, int pshared, unsigned value);
int sem_destroy(sem_t* s);
int sem_wait(sem_t* s);
int sem_trywait(sem_t* s);
int sem_post(sem_t* s);
int sem_getvalue(sem_t* s, int* value);

#ifdef __cplusplus
}
#endif

/* lote2 */
#ifdef __cplusplus
extern "C" {
#endif
#include <time.h>
int sem_timedwait(sem_t* s, const struct timespec* abstime);

#ifdef __cplusplus
}
#endif

#endif
