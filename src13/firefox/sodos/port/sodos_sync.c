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

/* ---------- mutex ---------- */

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* attr) {
  if (!m) return EINVAL;
  memset(m, 0, sizeof(*m));
  m->type = attr ? attr->type : PTHREAD_MUTEX_NORMAL;
  m->magic = MUTEX_MAGIC;
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* m) {
  if (!m) return EINVAL;
  if (m->owner || m->wait_head) return EBUSY;
  m->magic = 0;
  return 0;
}

int pthread_mutex_lock(pthread_mutex_t* m) {
  struct sodos_thread* self;
  if (!m) return EINVAL;
  self = sodos__self();
  if (m->owner == self) {
    if (m->type == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
    m->count++;
    return 0;
  }
  while (m->owner) {
    sodos__block(&m->wait_head, &m->wait_tail);
    self = sodos__self();
  }
  m->owner = self;
  m->count = 1;
  return 0;
}

int pthread_mutex_trylock(pthread_mutex_t* m) {
  struct sodos_thread* self;
  if (!m) return EINVAL;
  self = sodos__self();
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
  if (m->owner != self) return EPERM;
  if (--m->count > 0) return 0;
  m->owner = NULL;
  if (m->wait_head) {
    struct sodos_thread* t = thread_q_pop(&m->wait_head, &m->wait_tail);
    sodos__wake(t);
  }
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
  m->owner = NULL;
  m->count = 0;
  if (m->wait_head) {
    struct sodos_thread* t = thread_q_pop(&m->wait_head, &m->wait_tail);
    sodos__wake(t);
  }
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
  if (c->head) return EBUSY;
  c->magic = 0;
  return 0;
}

int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
  struct cond_waiter cw;
  int saved;

  if (!c || !m) return EINVAL;
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

  memset(&cw, 0, sizeof(cw));
  cw.thread = sodos__self();

  cond_release_mutex(m, &saved);
  q_push(&c->head, &c->tail, &cw);

  while (!cw.signaled) {
    uint64_t now = (uint64_t)time(NULL) * 1000ULL + (sodos__ms() % 1000ULL);
    uint64_t left;
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
  cw = (struct cond_waiter*)q_pop(&c->head, &c->tail);
  if (!cw) return 0;
  cw->signaled = 1;
  sodos__wake(cw->thread);
  return 0;
}

int pthread_cond_broadcast(pthread_cond_t* c) {
  struct cond_waiter* cw;
  if (!c) return EINVAL;
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

int pthread_once(pthread_once_t* once, void (*init)(void)) {
  static pthread_mutex_t once_mutex = PTHREAD_MUTEX_INITIALIZER;
  if (!once || !init) return EINVAL;
  pthread_mutex_lock(&once_mutex);
  if (!once->done) {
    once->done = 1;
    init();
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

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
  int i;
  if (!key) return EINVAL;
  for (i = 0; i < TLS_KEYS; i++) {
    if (!tls_keys[i].used) {
      tls_keys[i].used = 1;
      tls_keys[i].dtor = destructor;
      *key = (pthread_key_t)i;
      return 0;
    }
  }
  return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
  if (key >= TLS_KEYS || !tls_keys[key].used) return EINVAL;
  tls_keys[key].used = 0;
  tls_keys[key].dtor = NULL;
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
  if (s->wait_head) return EBUSY;
  s->magic = 0;
  return 0;
}

int sem_wait(sem_t* s) {
  if (!s) return EINVAL;
  while (s->value <= 0) sodos__block(&s->wait_head, &s->wait_tail);
  s->value--;
  return 0;
}

int sem_trywait(sem_t* s) {
  if (!s) return EINVAL;
  if (s->value <= 0) return EAGAIN;
  s->value--;
  return 0;
}

int sem_post(sem_t* s) {
  if (!s) return EINVAL;
  if (s->value == SEM_VALUE_MAX) return EOVERFLOW;
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
