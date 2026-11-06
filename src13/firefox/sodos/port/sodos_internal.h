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
};

/* sodos_tls.c */
void* sodos__tls_new(void** raw);
void sodos__tls_switch(void* tp);

void sodos__sched_init(void);
struct sodos_thread* sodos__self(void);
void sodos__yield(void);
void sodos__sleep_ms(unsigned ms);
void sodos__block(void** head, void** tail);
void sodos__park(void);
void sodos__wake(struct sodos_thread* t);
int sodos__started(void);
uint64_t sodos__ms(void);

/* sodos_sync.c */
void sodos__sync_init(void);
void sodos__tls_run_destructors(struct sodos_thread* t);

#endif
