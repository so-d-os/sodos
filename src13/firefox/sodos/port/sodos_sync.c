/* sodos_sync.c - mutex, condicao, rwlock, once, TLS e semaforos cooperativos.
 *
 * Como so ha uma thread rodando por vez, a exclusao mutua nao precisa de
 * atomics: o estado e testado e alterado sem interrupcao. O que importa e a
 * fila de espera: quem nao consegue o recurso sai da fila de prontos e volta
 * quando o dono libera. O bloqueio e sempre explicito (resched).
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <so.h>

#include "pthread.h"
#include "semaphore.h"
#include "sodos.h"
#include "sodos_internal.h"

#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef EPERM
#define EPERM 1
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef EDEADLK
#define EDEADLK 35
#endif
#ifndef EOVERFLOW
#define EOVERFLOW 75
#endif

#define MUTEX_MAGIC 0x4D555458u
#define COND_MAGIC 0x434F4E44u
#define RWLOCK_MAGIC 0x52574C4Bu
#define SEM_MAGIC 0x53454D41u

struct cond_waiter {
  struct cond_waiter* next;
  struct sodos_thread* thread;
  int signaled;
};

static void* q_pop(void** head, void** tail) {
  void* t = *head;
  if (!t) return NULL;
  *head = *(void**)t;
  if (!*head) *tail = NULL;
  return t;
}

/* Filas de thread (mutex, semaforo): o sodos__block as encadeia por qnext. O q_pop
 * acima so serve para os nos de condicao, com o proximo no inicio do no. */
static struct sodos_thread* thread_q_pop(void** head, void** tail) {
  struct sodos_thread* t = (struct sodos_thread*)*head;
  if (!t) return NULL;
  *head = t->qnext;
  if (!*head) *tail = NULL;
  t->qnext = NULL;
  return t;
}

static void q_push(void** head, void** tail, void* t) {
  *(void**)t = NULL;
  if (*tail) *(void**)*tail = t;
  else *head = t;
  *tail = t;
}

/* ---------- modo FFTHREADS: futex do kernel ----------
 * Com sodos__kt as threads rodam de verdade em paralelo (k_thread). Os campos
 * dos tipos do pthread.h sao reaproveitados, sem mudar o layout:
 *   mutex: wait_head (32 bits baixos) = palavra do futex (0 livre, 1 preso,
 *          2 preso com espera); owner/count para recursao e EPERM;
 *   cond:  head = sequencia, tail = quantas threads esperam;
 *   sem:   value = contador (o proprio futex), wait_head = quantas esperam. */
#define KW(p) ((volatile int*)(void*)&(p))
#define A_LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)

static inline void cpu_relax(void) { __asm__ volatile("pause" ::: "memory"); }

void sodos__fl_lock(volatile int* w) {
  int c = 0, i;
  if (__atomic_compare_exchange_n(w, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
  /* Quem segura costuma soltar logo (outra CPU): gira um pouco antes do kernel. */
  for (i = 0; i < 64; i++) {
    cpu_relax();
    c = 0;
    if (__atomic_load_n(w, __ATOMIC_RELAXED) == 0 &&
        __atomic_compare_exchange_n(w, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return;
  }
  c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
  while (c != 0) {
    k_futex_wait(w, 2, -1);
    c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
  }
}

/* Depois de esperar num cond: pega ja como "com espera" (2) para o proximo
 * unlock acordar quem mais estiver na fila. */
static void fl_lock_contended(volatile int* w) {
  int c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
  while (c != 0) {
    k_futex_wait(w, 2, -1);
    c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
  }
}

void sodos__fl_unlock(volatile int* w) {
  if (__atomic_exchange_n(w, 0, __ATOMIC_RELEASE) == 2) k_futex_wake(w, 1);
}

static int kt_mutex_lock(pthread_mutex_t* m, int try_only) {
  struct sodos_thread* self = sodos__self();
  if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == self) {
    if (m->type == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
    m->count++;
    return 0;
  }
  if (try_only) {
    int c = 0;
    if (!__atomic_compare_exchange_n(KW(m->wait_head), &c, 1, 0, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
      return EBUSY;
  } else {
    sodos__fl_lock(KW(m->wait_head));
  }
  __atomic_store_n(&m->owner, (void*)self, __ATOMIC_RELAXED);
  m->count = 1;
  return 0;
}

static int kt_mutex_unlock(pthread_mutex_t* m) {
  if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != sodos__self()) {
    sodos__kt_warn("mutex_unlock: nao e o dono", m);
    return EPERM;
  }
  if (--m->count > 0) return 0;
  __atomic_store_n(&m->owner, (void*)0, __ATOMIC_RELAXED);
  sodos__fl_unlock(KW(m->wait_head));
  return 0;
}

/* Espera no cond com o mutex m (dono = self). tmo_ms < 0 = sem prazo.
 * Devolve 0 ou ETIMEDOUT (o chamador ainda confere o relogio). */
static int kt_cond_wait(pthread_cond_t* c, pthread_mutex_t* m, int tmo_ms) {
  struct sodos_thread* self = sodos__self();
  int seq, saved;
  if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != self) {
    sodos__kt_warn("cond_wait: mutex de outra thread", m);
    return EPERM;
  }
  __atomic_add_fetch(KW(c->tail), 1, __ATOMIC_SEQ_CST);
  seq = __atomic_load_n(KW(c->head), __ATOMIC_SEQ_CST);
  saved = m->count;
  m->count = 0;
  __atomic_store_n(&m->owner, (void*)0, __ATOMIC_RELAXED);
  sodos__fl_unlock(KW(m->wait_head));
  /* Acordar sem sinal e permitido (o chamador confere o predicado); o
   * prazo quem decide e o pthread_cond_timedwait, pelo relogio. */
  (void)k_futex_wait(KW(c->head), seq, tmo_ms);
  __atomic_sub_fetch(KW(c->tail), 1, __ATOMIC_SEQ_CST);
  fl_lock_contended(KW(m->wait_head));
  __atomic_store_n(&m->owner, (void*)self, __ATOMIC_RELAXED);
  m->count = saved;
  return 0;
}

static void kt_cond_wake(pthread_cond_t* c, int n) {
  __atomic_add_fetch(KW(c->head), 1, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(KW(c->tail), __ATOMIC_SEQ_CST) > 0) k_futex_wake(KW(c->head), n);
}

/* ---------- lock grande do port ---------- */
static volatile int big_w;
static void* volatile big_owner;
static int big_depth;

void sodos_big_lock(void) {
  struct sodos_thread* self;
  if (!sodos__started()) sodos__sched_init();
  if (!sodos__kt) return;
  self = sodos__self();
  if (__atomic_load_n(&big_owner, __ATOMIC_RELAXED) == self) {
    big_depth++;
    return;
  }
  sodos__fl_lock(&big_w);
  __atomic_store_n(&big_owner, (void*)self, __ATOMIC_RELAXED);
  big_depth = 1;
}

void sodos_big_unlock(void) {
  if (!sodos__kt) return;
  if (__atomic_load_n(&big_owner, __ATOMIC_RELAXED) != sodos__self()) return;
  if (--big_depth > 0) return;
  __atomic_store_n(&big_owner, (void*)0, __ATOMIC_RELAXED);
  sodos__fl_unlock(&big_w);
}

/* Solta o lock inteiro se esta thread o segura; devolve a profundidade. */
int sodos__big_release(void) {
  int d;
  if (!sodos__kt || __atomic_load_n(&big_owner, __ATOMIC_RELAXED) != sodos__self()) return 0;
  d = big_depth;
  big_depth = 0;
  __atomic_store_n(&big_owner, (void*)0, __ATOMIC_RELAXED);
  sodos__fl_unlock(&big_w);
  return d;
}

void sodos__big_reacquire(int depth) {
  if (depth <= 0) return;
  sodos__fl_lock(&big_w);
  __atomic_store_n(&big_owner, (void*)sodos__self(), __ATOMIC_RELAXED);
  big_depth = depth;
}

/* ---------- mutex ---------- */

/* Solta o mutex: com fila, passa direto para o primeiro (FIFO). */
static void mutex_handoff(pthread_mutex_t* m) {
  struct sodos_thread* t = m->wait_head ? thread_q_pop(&m->wait_head, &m->wait_tail) : NULL;
  m->owner = t;
  m->count = t ? 1 : 0;
  if (t) sodos__wake(t);
}

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* attr) {
  if (!m) return EINVAL;
  memset(m, 0, sizeof(*m));
  m->type = attr ? attr->type : PTHREAD_MUTEX_NORMAL;
  m->magic = MUTEX_MAGIC;
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* m) {
  if (!m) return EINVAL;
  if (sodos__kt) {
    if (A_LOAD(KW(m->wait_head))) return EBUSY;
    m->magic = 0;
    return 0;
  }
  if (m->owner || m->wait_head) return EBUSY;
  m->magic = 0;
  return 0;
}

int pthread_mutex_lock(pthread_mutex_t* m) {
  struct sodos_thread* self;
  if (!m) return EINVAL;
  self = sodos__self();
  if (sodos__kt) return kt_mutex_lock(m, 0);
  if (m->owner == self) {
    if (m->type == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
    m->count++;
    return 0;
  }
  /* Quem estava na fila recebe o mutex do unlock (handoff): ao acordar ja e
   * o dono. Sem isso, a thread que soltou pega de novo antes da acordada
   * rodar (a vCPU do QEMU com a BQL) e a da fila morre de fome. */
  while (m->owner != self) {
    if (!m->owner) {
      m->owner = self;
      m->count = 1;
      break;
    }
    sodos__block(&m->wait_head, &m->wait_tail);
    self = sodos__self();
  }
  return 0;
}

int pthread_mutex_trylock(pthread_mutex_t* m) {
  struct sodos_thread* self;
  if (!m) return EINVAL;
  self = sodos__self();
  if (sodos__kt) return kt_mutex_lock(m, 1);
  if (m->owner == self) {
    if (m->type == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
    m->count++;
    return 0;
  }
  if (m->owner) return EBUSY;
  m->owner = self;
  m->count = 1;
  return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* m) {
  struct sodos_thread* self;
  if (!m) return EINVAL;
  self = sodos__self();
  if (sodos__kt) return kt_mutex_unlock(m);
  if (m->owner != self) return EPERM;
  if (--m->count > 0) return 0;
  mutex_handoff(m);
  return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t* a) {
  if (!a) return EINVAL;
  a->type = PTHREAD_MUTEX_NORMAL;
  return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t* a) { (void)a; return 0; }

int pthread_mutexattr_settype(pthread_mutexattr_t* a, int type) {
  if (!a) return EINVAL;
  a->type = type;
  return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t* a, int* type) {
  if (!a || !type) return EINVAL;
  *type = a->type;
  return 0;
}

/* ---------- condicao ---------- */

static void cond_release_mutex(pthread_mutex_t* m, int* saved) {
  *saved = m->count;
  m->count = 0;
  mutex_handoff(m);
}

static void cond_reacquire(pthread_mutex_t* m, int saved) {
  pthread_mutex_lock(m);
  m->count = saved;
}

int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* attr) {
  (void)attr;
  if (!c) return EINVAL;
  memset(c, 0, sizeof(*c));
  c->magic = COND_MAGIC;
  c->clockid = CLOCK_REALTIME;
  return 0;
}

int pthread_cond_destroy(pthread_cond_t* c) {
  if (!c) return EINVAL;
  if (sodos__kt) {
    c->magic = 0;
    return 0;
  }
  if (c->head) return EBUSY;
  c->magic = 0;
  return 0;
}

int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
  struct cond_waiter cw;
  int saved;

  if (!c || !m) return EINVAL;
  if (sodos__kt) return kt_cond_wait(c, m, -1);
  if (m->owner != sodos__self()) return EPERM;

  memset(&cw, 0, sizeof(cw));
  cw.thread = sodos__self();

  cond_release_mutex(m, &saved);
  q_push(&c->head, &c->tail, &cw);
  sodos__park();                 /* signal faz sodos__wake(cw.thread) */
  cond_reacquire(m, saved);
  return 0;
}

static void cond_unlink(pthread_cond_t* c, struct cond_waiter* cw) {
  struct cond_waiter** pp = (struct cond_waiter**)&c->head;
  while (*pp) {
    if (*pp == cw) {
      *pp = cw->next;
      if (c->tail == cw) {
        struct cond_waiter* t = (struct cond_waiter*)c->head;
        while (t && t->next) t = t->next;
        c->tail = t;
      }
      return;
    }
    pp = &(*pp)->next;
  }
}

int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                           const struct timespec* abstime) {
  struct cond_waiter cw;
  uint64_t deadline;
  int saved;

  if (!c || !m || !abstime) return EINVAL;
  if (m->owner != sodos__self()) return EPERM;

  deadline = abstime->tv_sec > 0
                 ? (uint64_t)abstime->tv_sec * 1000ULL +
                       (uint64_t)(abstime->tv_nsec / 1000000L)
                 : 0;

  if (sodos__kt) {
    /* mesmo criterio de relogio do modo cooperativo (abaixo) */
    struct timespec ts;
    uint64_t now, left;
    if (m->owner != sodos__self()) return EPERM;
    clock_gettime(abstime->tv_sec < 1000000000L ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    now = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
    if (now >= deadline) return ETIMEDOUT;
    left = deadline - now;
    if (left > 0x7FFFFFFFu) left = 0x7FFFFFFFu;
    if (kt_cond_wait(c, m, (int)left) != 0) return EPERM;
    clock_gettime(abstime->tv_sec < 1000000000L ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    now = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
    return now >= deadline ? ETIMEDOUT : 0;
  }

  memset(&cw, 0, sizeof(cw));
  cw.thread = sodos__self();

  cond_release_mutex(m, &saved);
  q_push(&c->head, &c->tail, &cw);

  while (!cw.signaled) {
    /* O prazo vem do relogio que o chamador usou: o Gecko pede CLOCK_MONOTONIC
     * (o pthread_condattr_setclock daqui nao guarda o relogio), o NSPR a hora
     * de parede. Um prazo pequeno (antes de 2001) so pode ser o monotonico. */
    struct timespec ts;
    uint64_t now, left;
    clock_gettime(abstime->tv_sec < 1000000000L ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    now = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
    if (now >= deadline) {
      cond_unlink(c, &cw);
      cond_reacquire(m, saved);
      return ETIMEDOUT;
    }
    left = deadline - now;
    sodos__sleep_ms(left > 2 ? 2 : (unsigned)left);
  }
  cond_reacquire(m, saved);
  return 0;
}

int pthread_cond_signal(pthread_cond_t* c) {
  struct cond_waiter* cw;
  if (!c) return EINVAL;
  if (sodos__kt) {
    kt_cond_wake(c, 1);
    return 0;
  }
  cw = (struct cond_waiter*)q_pop(&c->head, &c->tail);
  if (!cw) return 0;
  cw->signaled = 1;
  sodos__wake(cw->thread);
  return 0;
}

int pthread_cond_broadcast(pthread_cond_t* c) {
  struct cond_waiter* cw;
  if (!c) return EINVAL;
  if (sodos__kt) {
    kt_cond_wake(c, -1);
    return 0;
  }
  while ((cw = (struct cond_waiter*)q_pop(&c->head, &c->tail)) != NULL) {
    cw->signaled = 1;
    sodos__wake(cw->thread);
  }
  return 0;
}

int pthread_condattr_init(pthread_condattr_t* a) {
  if (a) a->pshared = 0;
  return 0;
}

int pthread_condattr_destroy(pthread_condattr_t* a) { (void)a; return 0; }

/* ---------- rwlock ---------- */

int pthread_rwlock_init(pthread_rwlock_t* l, const void* attr) {
  (void)attr;
  if (!l) return EINVAL;
  memset(l, 0, sizeof(*l));
  pthread_mutex_init(&l->mutex, NULL);
  pthread_cond_init(&l->cond, NULL);
  l->magic = RWLOCK_MAGIC;
  return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t* l) {
  if (!l) return EINVAL;
  pthread_cond_destroy(&l->cond);
  pthread_mutex_destroy(&l->mutex);
  l->magic = 0;
  return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t* l) {
  if (!l) return EINVAL;
  pthread_mutex_lock(&l->mutex);
  while (l->writer) pthread_cond_wait(&l->cond, &l->mutex);
  l->readers++;
  pthread_mutex_unlock(&l->mutex);
  return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t* l) {
  if (!l) return EINVAL;
  pthread_mutex_lock(&l->mutex);
  while (l->writer || l->readers) pthread_cond_wait(&l->cond, &l->mutex);
  l->writer = 1;
  pthread_mutex_unlock(&l->mutex);
  return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t* l) {
  if (!l) return EINVAL;
  pthread_mutex_lock(&l->mutex);
  if (l->writer) {
    pthread_mutex_unlock(&l->mutex);
    return EBUSY;
  }
  l->readers++;
  pthread_mutex_unlock(&l->mutex);
  return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t* l) {
  if (!l) return EINVAL;
  pthread_mutex_lock(&l->mutex);
  if (l->writer || l->readers) {
    pthread_mutex_unlock(&l->mutex);
    return EBUSY;
  }
  l->writer = 1;
  pthread_mutex_unlock(&l->mutex);
  return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t* l) {
  if (!l) return EINVAL;
  pthread_mutex_lock(&l->mutex);
  if (l->writer) l->writer = 0;
  else if (l->readers > 0) l->readers--;
  pthread_cond_broadcast(&l->cond);
  pthread_mutex_unlock(&l->mutex);
  return 0;
}

/* ---------- once ---------- */

/* done: 0 = nunca, 1 = init rodando, 2 = pronto. O mutex (recursivo na
 * pratica: o dono pode pegar de novo) segura as outras threads ate o init
 * acabar; um init que chama pthread_once do mesmo once nao entra de novo. */
int pthread_once(pthread_once_t* once, void (*init)(void)) {
  static pthread_mutex_t once_mutex = PTHREAD_MUTEX_INITIALIZER;
  if (!once || !init) return EINVAL;
  if (__atomic_load_n(&once->done, __ATOMIC_ACQUIRE) == 2) return 0;
  if (!sodos__started()) sodos__sched_init();
  if (sodos__kt) {
    /* Um futex por once (nao o mutex global): um init que espera outra
     * thread que tambem usa pthread_once nao trava as duas. */
    for (;;) {
      int v = __atomic_load_n(&once->done, __ATOMIC_ACQUIRE);
      if (v == 2) return 0;
      if (v == 0) {
        if (!__atomic_compare_exchange_n(&once->done, &v, 1, 0, __ATOMIC_ACQUIRE,
                                         __ATOMIC_RELAXED))
          continue;
        init();
        __atomic_store_n(&once->done, 2, __ATOMIC_RELEASE);
        k_futex_wake(&once->done, -1);
        return 0;
      }
      k_futex_wait(&once->done, 1, 50);
    }
  }
  pthread_mutex_lock(&once_mutex);
  if (!once->done) {
    once->done = 1;
    init();
    __atomic_store_n(&once->done, 2, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&once_mutex);
  return 0;
}

/* ---------- TLS ---------- */

#define TLS_KEYS PTHREAD_KEYS_MAX

static struct {
  int used;
  void (*dtor)(void*);
} tls_keys[TLS_KEYS];

void sodos__sync_init(void) {
  memset(tls_keys, 0, sizeof(tls_keys));
}

static volatile int keys_w;

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
  int i;
  if (!key) return EINVAL;
  if (!sodos__started()) sodos__sched_init();
  if (sodos__kt) sodos__fl_lock(&keys_w);
  for (i = 0; i < TLS_KEYS; i++) {
    if (!tls_keys[i].used) {
      tls_keys[i].used = 1;
      tls_keys[i].dtor = destructor;
      *key = (pthread_key_t)i;
      if (sodos__kt) sodos__fl_unlock(&keys_w);
      return 0;
    }
  }
  if (sodos__kt) sodos__fl_unlock(&keys_w);
  return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
  if (key >= TLS_KEYS || !tls_keys[key].used) return EINVAL;
  tls_keys[key].dtor = NULL;
  __atomic_store_n(&tls_keys[key].used, 0, __ATOMIC_RELEASE);
  return 0;
}

int pthread_setspecific(pthread_key_t key, const void* value) {
  struct sodos_thread* self;
  if (key >= TLS_KEYS || !tls_keys[key].used) return EINVAL;
  self = sodos__self();
  self->tls[key] = (void*)value;
  return 0;
}

void* pthread_getspecific(pthread_key_t key) {
  struct sodos_thread* self;
  if (key >= TLS_KEYS) return NULL;
  self = sodos__self();
  return self->tls[key];
}

void sodos__tls_run_destructors(struct sodos_thread* t) {
  int pass, i;
  for (pass = 0; pass < PTHREAD_DESTRUCTOR_ITERATIONS; pass++) {
    int ran = 0;
    for (i = 0; i < TLS_KEYS; i++) {
      if (t->tls[i] && tls_keys[i].dtor) {
        void* v = t->tls[i];
        t->tls[i] = NULL;
        tls_keys[i].dtor(v);
        ran = 1;
      }
    }
    if (!ran) break;
  }
  memset(t->tls, 0, sizeof(t->tls));
}

/* ---------- semaforos ---------- */

int sem_init(sem_t* s, int pshared, unsigned value) {
  (void)pshared;
  if (!s || value > SEM_VALUE_MAX) return EINVAL;
  memset(s, 0, sizeof(*s));
  s->value = (int)value;
  s->magic = SEM_MAGIC;
  return 0;
}

int sem_destroy(sem_t* s) {
  if (!s) return EINVAL;
  if (sodos__kt) {
    s->magic = 0;
    return 0;
  }
  if (s->wait_head) return EBUSY;
  s->magic = 0;
  return 0;
}

static int kt_sem_trywait(sem_t* s) {
  int v = A_LOAD(&s->value);
  while (v > 0) {
    if (__atomic_compare_exchange_n(&s->value, &v, v - 1, 0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED))
      return 0;
  }
  return EAGAIN;
}

int sem_wait(sem_t* s) {
  if (!s) return EINVAL;
  if (!sodos__started()) sodos__sched_init();
  if (sodos__kt) {
    while (kt_sem_trywait(s) != 0) {
      __atomic_add_fetch(KW(s->wait_head), 1, __ATOMIC_SEQ_CST);
      if (__atomic_load_n(&s->value, __ATOMIC_SEQ_CST) <= 0) k_futex_wait(&s->value, 0, -1);
      __atomic_sub_fetch(KW(s->wait_head), 1, __ATOMIC_SEQ_CST);
    }
    return 0;
  }
  while (s->value <= 0) sodos__block(&s->wait_head, &s->wait_tail);
  s->value--;
  return 0;
}

int sem_trywait(sem_t* s) {
  if (!s) return EINVAL;
  if (sodos__kt) return kt_sem_trywait(s);
  if (s->value <= 0) return EAGAIN;
  s->value--;
  return 0;
}

int sem_post(sem_t* s) {
  if (!s) return EINVAL;
  if (s->value == SEM_VALUE_MAX) return EOVERFLOW;
  if (sodos__kt) {
    __atomic_add_fetch(&s->value, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(KW(s->wait_head), __ATOMIC_SEQ_CST) > 0) k_futex_wake(&s->value, 1);
    return 0;
  }
  s->value++;
  if (s->wait_head) {
    struct sodos_thread* t = thread_q_pop(&s->wait_head, &s->wait_tail);
    sodos__wake(t);
  }
  return 0;
}

int sem_getvalue(sem_t* s, int* value) {
  if (!s || !value) return EINVAL;
  *value = s->value;
  return 0;
}
