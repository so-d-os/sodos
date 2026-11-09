/* pthread.h - pthreads cooperativos do SODOS (libsodos / SODOS.DLL).
 *
 * O kernel do SODOS nao tem threads preemptivos: os programas sao tarefas
 * cooperativas. Esta implementacao entrega pthreads de verdade (pilha propria,
 * troca de contexto em assembly) mas a troca so acontece quando a thread
 * chama uma funcao que cede: pthread_* bloqueante, sodos_sleep_ms, sched_yield
 * ou poll. Nao ha preempcao por timer.
 *
 * Isso e suficiente para NSPR/Gecko em um unico nucleo (modelo "single core
 * cooperativo") e e o primeiro passo ate o kernel ganhar troca por IRQ.
 */
#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sodos_thread* pthread_t;

typedef struct {
  int type;
  size_t stacksize;
  int detached;
} pthread_attr_t;

/* Nao mexa nos campos: o layout e o contrato com sodos_sync.c. */
typedef struct {
  void* owner;
  void* wait_head;
  void* wait_tail;
  int count;
  int type;
  unsigned magic;
} pthread_mutex_t;

typedef struct {
  void* head;
  void* tail;
  unsigned magic;
  int clockid;
} pthread_cond_t;

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int readers;
  int writer;
  unsigned magic;
} pthread_rwlock_t;

typedef struct {
  int done;
} pthread_once_t;

typedef unsigned pthread_key_t;

typedef struct {
  int type;
} pthread_mutexattr_t;

typedef struct {
  int pshared;
} pthread_condattr_t;

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_ONCE_INIT { 0 }
int pthread_setname_np(pthread_t t, const char* name);
int pthread_getname_np(pthread_t t, char* buf, size_t n);
#define PTHREAD_MUTEX_INITIALIZER { 0, 0, 0, 0, PTHREAD_MUTEX_NORMAL, 0x4D555458u }
#define PTHREAD_COND_INITIALIZER { 0, 0, 0x434F4E44u, 0 }
#define PTHREAD_RWLOCK_INITIALIZER \
  { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0x52574C4Bu }

#define PTHREAD_STACK_MIN 16384
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_KEYS_MAX 64
#define PTHREAD_CANCELED ((void*)-1)

int pthread_create(pthread_t* t, const pthread_attr_t* attr,
                   void* (*fn)(void*), void* arg);
void pthread_exit(void* retval);
int pthread_join(pthread_t t, void** retval);
int pthread_detach(pthread_t t);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_yield(void);
int sched_yield(void);

int pthread_attr_init(pthread_attr_t* a);
int pthread_attr_destroy(pthread_attr_t* a);
int pthread_attr_setstacksize(pthread_attr_t* a, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t* a, size_t* size);
int pthread_attr_getstack(const pthread_attr_t* a, void** addr, size_t* size);
int pthread_getattr_np(pthread_t t, pthread_attr_t* a);
int pthread_attr_setdetachstate(pthread_attr_t* a, int state);
int pthread_attr_getdetachstate(const pthread_attr_t* a, int* state);
int pthread_attr_setguardsize(pthread_attr_t* a, size_t size);
int pthread_attr_getguardsize(const pthread_attr_t* a, size_t* size);

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* attr);
int pthread_mutex_destroy(pthread_mutex_t* m);
int pthread_mutex_lock(pthread_mutex_t* m);
int pthread_mutex_trylock(pthread_mutex_t* m);
int pthread_mutex_unlock(pthread_mutex_t* m);

int pthread_mutexattr_init(pthread_mutexattr_t* a);
int pthread_mutexattr_destroy(pthread_mutexattr_t* a);
int pthread_mutexattr_settype(pthread_mutexattr_t* a, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t* a, int* type);

int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* attr);
int pthread_cond_destroy(pthread_cond_t* c);
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m);
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                           const struct timespec* abstime);
int pthread_cond_signal(pthread_cond_t* c);
int pthread_cond_broadcast(pthread_cond_t* c);
int pthread_condattr_init(pthread_condattr_t* a);
int pthread_condattr_destroy(pthread_condattr_t* a);
int pthread_condattr_setclock(pthread_condattr_t* a, clockid_t id);

int pthread_rwlock_init(pthread_rwlock_t* l, const void* attr);
int pthread_rwlock_destroy(pthread_rwlock_t* l);
int pthread_rwlock_rdlock(pthread_rwlock_t* l);
int pthread_rwlock_wrlock(pthread_rwlock_t* l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t* l);
int pthread_rwlock_trywrlock(pthread_rwlock_t* l);
int pthread_rwlock_unlock(pthread_rwlock_t* l);

int pthread_once(pthread_once_t* once, void (*init)(void));

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void* value);
void* pthread_getspecific(pthread_key_t key);

int pthread_setcancelstate(int state, int* oldstate);
int pthread_setcanceltype(int type, int* oldtype);
int pthread_cancel(pthread_t t);

/* Sem cancelamento assincrono: o handler roda so no pop(1), como em
 * pthread_cleanup_pop(1) da glibc. */
#define pthread_cleanup_push(fn, arg)   {                                        void (*__sodos_cu_fn)(void*) = (fn);     void* __sodos_cu_arg = (arg);
#define pthread_cleanup_pop(run)                          if (run) __sodos_cu_fn(__sodos_cu_arg);             }
int pthread_kill(pthread_t t, int sig);

#ifdef __cplusplus
}
#endif

/* lote2 */
#ifdef __cplusplus
extern "C" {
#endif
#include <sched.h>
#ifndef PTHREAD_PROCESS_PRIVATE
#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1
#endif
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));
int pthread_getcpuclockid(pthread_t t, int* clk);
int pthread_getschedparam(pthread_t t, int* policy, struct sched_param* param);
int pthread_setschedparam(pthread_t t, int policy, const struct sched_param* param);
int pthread_setaffinity_np(pthread_t t, size_t n, const cpu_set_t* set);
int pthread_mutexattr_setpshared(pthread_mutexattr_t* a, int shared);

#ifdef __cplusplus
}
#endif

#endif
