/* sodos_thread.c - pthreads cooperativos sobre pilhas proprias (x32).
 *
 * Modelo: uma thread corrente (cur) fora da fila de prontos, uma fila FIFO de
 * prontas, uma fila ordenada de threads dormindo. A troca de contexto salva
 * apenas os registradores callee-saved do SysV x32 (rbx, rbp, r12-r15) e o
 * rsp; xmm0-15 sao caller-saved e nao precisam ser preservados.
 *
 * Nao ha preempcao: a troca so acontece em pthread_* bloqueante, sched_yield,
 * sodos_sleep_ms ou poll. E o bastante para o NSPR em um nucleo; a troca por
 * IRQ de timer e trabalho de kernel (ver docs/03-fases.md, M1).
 */
#include <so.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pthread.h"
#include "sodos.h"
#include "sodos_internal.h"

#ifndef ESRCH
#define ESRCH 3
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef EDEADLK
#define EDEADLK 35
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

#define THREAD_MAGIC 0x54485244u
#define DEFAULT_STACK (256 * 1024)
#define MAIN_MAGIC 0x4D41494Eu

extern void sodos_ctx_switch(uint32_t* save_sp, uint32_t new_sp);
extern void sodos_thread_trampoline(void);
void sodos_thread_finish(void* retval);

/* Salva o contexto atual em *save_sp e retoma em new_sp. */
__asm__(
    ".text\n"
    ".globl sodos_ctx_switch\n"
    ".type sodos_ctx_switch,@function\n"
    "sodos_ctx_switch:\n"
    "  pushq %rbx\n"
    "  pushq %rbp\n"
    "  pushq %r12\n"
    "  pushq %r13\n"
    "  pushq %r14\n"
    "  pushq %r15\n"
    "  movl %esp, (%edi)\n"
    "  movl %esi, %esp\n"
    "  popq %r15\n"
    "  popq %r14\n"
    "  popq %r13\n"
    "  popq %r12\n"
    "  popq %rbp\n"
    "  popq %rbx\n"
    "  ret\n"
    ".size sodos_ctx_switch, .-sodos_ctx_switch\n");

/* Primeira entrada de uma thread: [esp] = fn, [esp+4] = arg.
 * A ABI x32 passa o argumento em edi; o trampolim carrega fn em eax e arg
 * em edi antes do call (foi o bug que fazia toda thread receber o ponteiro
 * da funcao como argumento). */
__asm__(
    ".text\n"
    ".globl sodos_thread_trampoline\n"
    ".type sodos_thread_trampoline,@function\n"
    "sodos_thread_trampoline:\n"
    "  movl (%esp), %eax\n"
    "  movl 8(%esp), %edi\n"
    "  call *%rax\n"
    "  movl %eax, %edi\n"
    "  call sodos_thread_finish\n"
    "  ud2\n"
    ".size sodos_thread_trampoline, .-sodos_thread_trampoline\n");

static struct sodos_thread main_thread;
static struct sodos_thread* cur;
static struct sodos_thread* run_h;
static struct sodos_thread* run_t;
static struct sodos_thread* sleep_h;
static struct sodos_thread* zombie_h;
static int sched_inited;
static int in_finish;

static void q_push(struct sodos_thread** head, struct sodos_thread** tail,
                   struct sodos_thread* t) {
  t->qnext = NULL;
  if (*tail) (*tail)->qnext = t;
  else *head = t;
  *tail = t;
}

static struct sodos_thread* q_pop(struct sodos_thread** head,
                                  struct sodos_thread** tail) {
  struct sodos_thread* t = *head;
  if (!t) return NULL;
  *head = t->qnext;
  if (!*head) *tail = NULL;
  t->qnext = NULL;
  return t;
}

static void make_ready(struct sodos_thread* t) {
  t->state = ST_READY;
  q_push(&run_h, &run_t, t);
}

static void reap_zombies(void) {
  /* Nunca no proprio resched da thread que morreu: a pilha dela ainda e a
   * pilha em uso. Quem reaproveita e a proxima thread que escalonar. */
  if (in_finish) return;
  while (zombie_h) {
    struct sodos_thread* z = zombie_h;
    zombie_h = z->qnext;
    if (z->alloc) k_free((void*)(uintptr_t)z->alloc);
    z->magic = 0;
    free(z);
  }
}

static int wake_due(void) {
  int any = 0;
  uint64_t now = sodos__ms();
  while (sleep_h && now >= sleep_h->wake_ms) {
    struct sodos_thread* t = sleep_h;
    sleep_h = t->sleep_next;
    t->sleep_next = NULL;
    make_ready(t);
    any = 1;
  }
  return any;
}

static void idle_wait(void) {
  uint64_t now = sodos__ms();
  uint64_t deadline = sleep_h ? sleep_h->wake_ms : now + 1;
  /* Cede para as outras tarefas do SO (WIN, HTTPD) e volta a checar o PIT. */
  while (sodos__ms() < deadline) k_yield();
}

/* Escolhe a proxima thread. Chamado com a thread atual fora da fila de
 * prontos (BLOCK/SLEEP/DEAD) ou marcada READY (yield). */
static void resched(void) {
  reap_zombies();
  for (;;) {
    struct sodos_thread* next = q_pop(&run_h, &run_t);
    if (next) {
      struct sodos_thread* prev = cur;
      next->state = ST_RUN;
      cur = next;
      if (next != prev) {
        sodos__tls_switch(next->tls_tp);
        sodos_ctx_switch(&prev->rsp, next->rsp);
      }
      in_finish = 0;   /* quem morreu ja saiu da propria pilha */
      return;
    }
    if (cur && cur->state == ST_RUN) return;
    if (wake_due()) continue;
    if (!sleep_h) {
      sodos_log(SODOS_LOG_ERROR,
                "deadlock: nenhuma thread pronta (thread atual bloqueada)");
      sodos_log_flush();
      k_exit(3);
      for (;;) {}
    }
    idle_wait();
  }
}

void sodos__sched_init(void) {
  if (sched_inited) return;
  sched_inited = 1;
  memset(&main_thread, 0, sizeof(main_thread));
  main_thread.magic = MAIN_MAGIC;
  main_thread.state = ST_RUN;
  cur = &main_thread;
  sodos__sync_init();
}

int sodos__started(void) { return sched_inited; }

struct sodos_thread* sodos__self(void) {
  if (!sched_inited) sodos__sched_init();
  return cur;
}

void sodos__yield(void) {
  if (!sched_inited) sodos__sched_init();
  if (cur->state != ST_RUN) return;
  make_ready(cur);
  resched();
}

void sodos__block(void** head, void** tail) {
  struct sodos_thread** h = (struct sodos_thread**)head;
  struct sodos_thread** t = (struct sodos_thread**)tail;
  cur->state = ST_BLOCK;
  q_push(h, t, cur);
  resched();
}

void sodos__wake(struct sodos_thread* t) {
  if (t && t->state == ST_BLOCK) make_ready(t);
}

/* Bloqueia sem entrar em fila: quem chama guardou o ponteiro da thread em
 * algum no (cond/sem) e chama sodos__wake quando for a hora. */
void sodos__park(void) {
  if (!sched_inited) sodos__sched_init();
  cur->state = ST_BLOCK;
  resched();
}

void sodos__sleep_ms(unsigned ms) {
  uint64_t deadline;
  struct sodos_thread** pp;
  if (!sched_inited) sodos__sched_init();
  if (ms == 0) {
    sodos__yield();
    return;
  }
  deadline = sodos__ms() + ms;
  cur->state = ST_SLEEP;
  cur->wake_ms = deadline;
  pp = &sleep_h;
  while (*pp && (*pp)->wake_ms <= deadline) pp = &(*pp)->sleep_next;
  cur->sleep_next = *pp;
  *pp = cur;
  resched();
}

/* ---------- criacao ---------- */

static uint32_t alloc_stack(struct sodos_thread* t, size_t size) {
  uint32_t raw;
  uint32_t base;
  uint32_t top;
  uint32_t* sp;

  if (size < PTHREAD_STACK_MIN) size = PTHREAD_STACK_MIN;
  size = (size + 15u) & ~(size_t)15u;
  raw = (uint32_t)(uintptr_t)k_malloc((unsigned)(size + 16));
  if (!raw) return 0;
  base = (raw + 15u) & ~15u;
  t->alloc = raw;
  t->stack = base;
  t->stack_size = (uint32_t)size;

  top = (base + (uint32_t)size) & ~15u;
  sp = (uint32_t*)(uintptr_t)top;

  sp -= 2;                 /* arg */
  sp[0] = (uint32_t)(uintptr_t)t->arg;
  sp[1] = 0;
  sp -= 2;                 /* fn */
  sp[0] = (uint32_t)(uintptr_t)t->fn;
  sp[1] = 0;
  sp -= 2;                 /* retorno do trampolim */
  sp[0] = (uint32_t)(uintptr_t)&sodos_thread_trampoline;
  sp[1] = 0;
  /* 6 registradores callee-saved, na ordem inversa dos pops */
  sp -= 2; sp[0] = 0; sp[1] = 0;   /* r15 */
  sp -= 2; sp[0] = 0; sp[1] = 0;   /* r14 */
  sp -= 2; sp[0] = 0; sp[1] = 0;   /* r13 */
  sp -= 2; sp[0] = 0; sp[1] = 0;   /* r12 */
  sp -= 2; sp[0] = 0; sp[1] = 0;   /* rbp */
  sp -= 2; sp[0] = 0; sp[1] = 0;   /* rbx */
  return (uint32_t)(uintptr_t)sp;
}

int pthread_create(pthread_t* out, const pthread_attr_t* attr,
                   void* (*fn)(void*), void* arg) {
  struct sodos_thread* t;
  size_t stacksize;

  if (!fn) return EINVAL;
  if (!sched_inited) sodos__sched_init();

  t = (struct sodos_thread*)calloc(1, sizeof(*t));
  if (!t) return EAGAIN;
  t->fn = fn;
  t->arg = arg;
  t->magic = THREAD_MAGIC;
  t->detached = attr ? attr->detached : 0;
  stacksize = (attr && attr->stacksize) ? attr->stacksize : DEFAULT_STACK;
  t->rsp = alloc_stack(t, stacksize);
  if (!t->rsp) {
    free(t);
    return EAGAIN;
  }
  t->tls_tp = sodos__tls_new(&t->tls_raw);
  t->state = ST_READY;
  q_push(&run_h, &run_t, t);
  if (out) *out = t;
  return 0;
}

void sodos_thread_finish(void* retval) {
  struct sodos_thread* t = cur;
  struct sodos_thread* j;

  t->retval = retval;
  t->state = ST_DEAD;
  t->finished = 1;
  sodos__tls_run_destructors(t);

  while ((j = q_pop(&t->join_head, &t->join_tail)) != NULL) make_ready(j);

  if (t->detached) {
    t->qnext = zombie_h;
    zombie_h = t;
  }
  in_finish = 1;
  resched();
  for (;;) {}
}

void pthread_exit(void* retval) {
  if (!sched_inited) sodos__sched_init();
  sodos_thread_finish(retval);
}

int pthread_join(pthread_t t, void** retval) {
  if (!t || t->magic != THREAD_MAGIC) return ESRCH;
  if (t->detached) return EINVAL;
  while (!t->finished)
    sodos__block((void**)&t->join_head, (void**)&t->join_tail);
  if (retval) *retval = t->retval;
  if (t->alloc) k_free((void*)(uintptr_t)t->alloc);
  free(t->tls_raw);
  t->magic = 0;
  free(t);
  return 0;
}

int pthread_detach(pthread_t t) {
  if (!t || t->magic != THREAD_MAGIC) return ESRCH;
  t->detached = 1;
  if (t->finished && t->state == ST_DEAD) {
    t->qnext = zombie_h;
    zombie_h = t;
  }
  return 0;
}

pthread_t pthread_self(void) {
  if (!sched_inited) sodos__sched_init();
  return cur;
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

int pthread_yield(void) {
  sodos__yield();
  return 0;
}

int sched_yield(void) {
  sodos__yield();
  return 0;
}
/* ---------- atributos ---------- */

int pthread_attr_init(pthread_attr_t* a) {
  if (!a) return EINVAL;
  a->type = PTHREAD_MUTEX_NORMAL;
  a->stacksize = DEFAULT_STACK;
  a->detached = 0;
  return 0;
}

int pthread_attr_destroy(pthread_attr_t* a) {
  (void)a;
  return 0;
}

int pthread_attr_setstacksize(pthread_attr_t* a, size_t size) {
  if (!a || size < PTHREAD_STACK_MIN) return EINVAL;
  a->stacksize = size;
  return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t* a, size_t* size) {
  if (!a || !size) return EINVAL;
  *size = a->stacksize ? a->stacksize : DEFAULT_STACK;
  return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t* a, int state) {
  if (!a) return EINVAL;
  a->detached = (state == PTHREAD_CREATE_DETACHED);
  return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t* a, int* state) {
  if (!a || !state) return EINVAL;
  *state = a->detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE;
  return 0;
}

int pthread_attr_setguardsize(pthread_attr_t* a, size_t size) {
  (void)a;
  (void)size;
  return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t* a, size_t* size) {
  (void)a;
  if (size) *size = 0;
  return 0;
}

/* Cancelamento nao existe: o SO encerra o programa inteiro no Ctrl+C. */
int pthread_setcancelstate(int state, int* oldstate) {
  if (oldstate) *oldstate = 0;
  (void)state;
  return 0;
}

int pthread_setcanceltype(int type, int* oldtype) {
  if (oldtype) *oldtype = 0;
  (void)type;
  return 0;
}

int pthread_cancel(pthread_t t) {
  (void)t;
  return ENOSYS;
}
