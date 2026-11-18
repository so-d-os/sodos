/* sodos_internal.h - contrato interno da libsodos (nao e API publica). */
#ifndef SODOS_INTERNAL_H
#define SODOS_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include "pthread.h"

enum sodos_tstate {
  ST_READY = 1,
  ST_RUN = 2,
  ST_BLOCK = 3,
  ST_SLEEP = 4,
  ST_DEAD = 5
};

/* Uma thread = pilha propria + contexto salvo (rsp). O resto e estado de
 * escalonamento. Os campos de fila (qnext) sao usados por UMA fila por vez. */
struct sodos_thread {
  uint32_t rsp;         /* rsp salvo pelo sodos_ctx_switch */
  uint32_t alloc;       /* bloco devolvido por k_malloc (0 = pilha do processo) */
  uint32_t stack;       /* base alinhada da pilha */
  uint32_t stack_size;
  void* (*fn)(void*);
  void* arg;
  void* retval;
  int state;
  int detached;
  int finished;         /* ja rodou o trampolim e marcou DEAD */
  struct sodos_thread* qnext;
  struct sodos_thread* join_head;
  struct sodos_thread* join_tail;
  struct sodos_thread* sleep_next;
  uint64_t wake_ms;
  void* tls[PTHREAD_KEYS_MAX];
  unsigned magic;
  void* tls_tp;         /* ponteiro de thread (TLS estatico); NULL = thread principal */
  void* tls_raw;        /* bloco a liberar */
  int ktid;             /* FFTHREADS: tid da k_thread (0 = principal/cooperativa) */
  int err;              /* FFTHREADS: errno desta thread */
  struct sodos_thread* znext;  /* FFTHREADS: lista de threads destacadas */
  uintptr_t kt_stack_top;      /* FFTHREADS: topo da pilha (anotado no kt_entry) */
};

/* sodos_tls.c */
void* sodos__tls_new(void** raw);
void sodos__tls_switch(void* tp);
void* sodos__tls_main(void);
int sodos__tls_present(void);

void sodos__sched_init(void);
struct sodos_thread* sodos__self(void);
void sodos__yield(void);
void sodos__sleep_ms(unsigned ms);
void sodos__block(void** head, void** tail);
void sodos__park(void);
void sodos__wake(struct sodos_thread* t);
int sodos__started(void);
uint64_t sodos__ms(void);

/* Modo das threads (fixado no sodos__sched_init):
 * 0 = cooperativo (todas as threads no "mundo", CPU0);
 * 1 = FFTHREADS: cada pthread e uma k_thread do kernel e roda em qualquer CPU;
 *     mutex/cond/sem viram futex do kernel (k_futex_*). */
extern int sodos__kt;

/* Lock grande do port (recursivo; no-op no modo cooperativo): protege as
 * tabelas de fd (posix-lite, soquetes, pipes, shm), o mmap e o log.
 * sodos__sleep_ms/sodos__yield o soltam enquanto a thread espera. */
void sodos_big_lock(void);
void sodos_big_unlock(void);
int sodos__big_release(void);
void sodos__big_reacquire(int depth);
static inline int sodos__big_guard(void) {
  sodos_big_lock();
  return 0;
}
static inline void sodos__big_guard_end(int* p) {
  (void)p;
  sodos_big_unlock();
}
#define SODOS_BIG_GUARD \
  int sodos_big_g_ __attribute__((cleanup(sodos__big_guard_end), unused)) = sodos__big_guard()

/* Aviso de uso errado de pthread no modo k_thread (vai para a serial). */
void sodos__kt_warn(const char* what, const void* obj);

/* futex de usuario (lock simples 0/1/2) usado pelo modo FFTHREADS */
void sodos__fl_lock(volatile int* w);
void sodos__fl_unlock(volatile int* w);

/* sodos_sync.c */
void sodos__sync_init(void);
void sodos__tls_run_destructors(struct sodos_thread* t);

#endif
