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
 *
 * Modo FFTHREADS (LP64, padrao com mais de uma CPU; SET FFTHREADS=0 desliga):
 * cada pthread vira uma k_thread do kernel (kernel/kthread.c), que roda em
 * qualquer CPU, preemptada pelo timer local. A thread principal continua no
 * "mundo" (CPU0, tarefa do WIN). TLS: cada thread recebe o proprio bloco e
 * aponta o FS para ele (k_thread_tls); o ponteiro da sodos_thread fica no TCB,
 * em %fs:0x40. Mutex/cond/sem (sodos_sync.c) e o futex da libc passam a usar
 * k_futex_wait/wake; o resto do port e serializado pelo lock grande
 * (sodos_big_lock).
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

/* ---------- modo FFTHREADS (k_thread) ---------- */
int sodos__kt;
#define KT_TCB_SELF 0x40      /* slot livre do TCB (sodos_tls.c: TCB_SIZE 256) */
#define KT_STACK_SLACK (32 * 1024)  /* IRQs e codigo do kernel usam a mesma pilha */
#define KT_STACK_MIN (128 * 1024)
static volatile int kt_zlock;           /* lista de threads destacadas */
static struct sodos_thread* kt_zombies;

/* O bloco TLS da principal so existe depois do construtor 101 (sodos_tls.c);
 * antes disso o FS aponta para outra coisa e %fs:0x40 nao e nosso. */
static int kt_tls_ok;

#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
static inline struct sodos_thread* kt_self_get(void) {
  struct sodos_thread* s;
  if (__builtin_expect(!kt_tls_ok, 0)) {
    if (!sodos__tls_main()) return NULL;
    kt_tls_ok = 1;
  }
  __asm__ volatile("movq %%fs:0x40, %0" : "=r"(s));
  return s;
}

static inline void kt_self_set(struct sodos_thread* s) {
  __asm__ volatile("movq %0, %%fs:0x40" : : "r"(s) : "memory");
}
#else
/* x32 (SODOS.DLL): sem TLS por thread, o modo k_thread nunca liga. */
static inline struct sodos_thread* kt_self_get(void) { return NULL; }
static inline void kt_self_set(struct sodos_thread* s) { (void)s; (void)kt_tls_ok; }
#endif

/* SET FFTHREADS=0 volta ao cooperativo; sem TLS ou com uma CPU so tambem. */
static int kt_choose(void) {
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
  char v[16];
  int n = k_getenv("FFTHREADS", v, (int)sizeof(v) - 1);
  if (n > 0) {
    v[n] = 0;
    if (v[0] == '0' || v[0] == 'n' || v[0] == 'N') return 0;
  }
  if (!sodos__tls_present()) return 0;
  if (k_cpu_count() < 2 && !(n > 0 && v[0] == '2')) return 0; /* FFTHREADS=2 forca */
  return 1;
#else
  return 0;
#endif
}

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

/* ---------- depuracao: pilhas de todas as threads ----------
 * SODOS_STACKS_AT=<segundos> no ambiente (ou no arquivo D:\\SODOS\\STACKS.AT):
 * passado esse tempo desde a primeira
 * thread, lista na serial os enderecos de codigo da pilha de cada thread (para
 * simbolizar com o ELF). Serve para ver onde um programa portado ficou parado. */
#define ALL_MAX 256
static struct sodos_thread* all_thr[ALL_MAX];
static uint64_t stacks_at;
extern char __executable_start[] __attribute__((weak));
extern char etext[] __attribute__((weak));

static void all_add(struct sodos_thread* t) {
  int i;
  for (i = 0; i < ALL_MAX; i++) {
    if (!all_thr[i]) {
      all_thr[i] = t;
      return;
    }
  }
}

static void all_del(struct sodos_thread* t) {
  int i;
  for (i = 0; i < ALL_MAX; i++)
    if (all_thr[i] == t) all_thr[i] = NULL;
}

static void ser_txt(const char* s) {
  for (; *s; s++) __asm__ volatile("outb %0, %1" :: "a"((unsigned char)*s), "d"((unsigned short)0x3F8));
}

static void ser_hex(uint64_t v) {
  char b[17];
  int i;
  for (i = 0; i < 16; i++) b[i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
  b[16] = 0;
  ser_txt(b + 7);
}

static void dump_one(struct sodos_thread* t, uintptr_t* sp, uintptr_t* end) {
  int shown = 0;
  ser_txt("[thr] ");
  ser_hex((uintptr_t)t);
  ser_txt(t == cur ? " atual" : t->state == ST_SLEEP ? " dorme" : t->state == ST_BLOCK ? " bloqueada" :
          t->state == ST_READY ? " pronta" : " morta");
  ser_txt(" fn=");
  ser_hex((uintptr_t)t->fn);
  ser_txt("\n");
  for (; sp < end && shown < 28; sp++) {
    if ((char*)*sp >= __executable_start && (char*)*sp < etext) {
      ser_txt("[stk] ");
      ser_hex(*sp);
      ser_txt("\n");
      shown++;
    }
  }
}

static void dump_all(void) {
  int i;
  uintptr_t* here = (uintptr_t*)__builtin_frame_address(0);
  ser_txt("[thr] base ");
  ser_hex((uintptr_t)__executable_start);
  ser_txt("\n");
  dump_one(&main_thread, cur == &main_thread ? here : (uintptr_t*)(uintptr_t)main_thread.rsp,
           (cur == &main_thread ? here : (uintptr_t*)(uintptr_t)main_thread.rsp) + 60000);
  for (i = 0; i < ALL_MAX; i++) {
    struct sodos_thread* t = all_thr[i];
    uintptr_t* sp;
    uintptr_t* end;
    if (!t || t->state == ST_DEAD) continue;
    sp = t == cur ? here : (uintptr_t*)(uintptr_t)t->rsp;
    end = (uintptr_t*)(uintptr_t)(t->stack + t->stack_size);
    if (sp < (uintptr_t*)(uintptr_t)t->stack || sp > end) continue;
    dump_one(t, sp, end);
  }
}

static void stacks_check(void) {
  if (!stacks_at || sodos__ms() < stacks_at) return;
  stacks_at = 0;
  dump_all();
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
    all_del(z);
    free(z);
  }
}

/* Fatia cooperativa (sodos_coop_due/yield, mais abaixo): conta desde que a
 * thread atual ganhou a CPU. */
#define COOP_SLICE_MS 10
static uint64_t coop_last;
static unsigned coop_n;
static int coop_flag;

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

static uint64_t os_last;   /* ultimo k_yield (ver os_yield_due) */

static void idle_wait(void) {
  uint64_t now = sodos__ms();
  uint64_t deadline = sleep_h ? sleep_h->wake_ms : now + 1;
  /* Cede para as outras tarefas do SO (WIN, HTTPD) e volta a checar o PIT. */
  while (sodos__ms() < deadline) k_yield();
  os_last = sodos__ms();
}

/* O WIN so le mouse/teclado e desenha quando o programa devolve a vez
 * (k_yield). Com threads sempre prontas (timers, socket, JS) o idle_wait
 * nunca rodava e a tela travava: a cada OS_SLICE_MS cede ao SO mesmo assim. */
#define OS_SLICE_MS 8

static void os_yield_due(void) {
  uint64_t now = sodos__ms();
  if (now - os_last < OS_SLICE_MS) return;
  k_yield();
  os_last = sodos__ms();
}

/* Escolhe a proxima thread. Chamado com a thread atual fora da fila de
 * prontos (BLOCK/SLEEP/DEAD) ou marcada READY (yield). */
static void resched(void) {
  reap_zombies();
  stacks_check();
  os_yield_due();
  /* Acorda quem ja venceu o prazo antes de escolher: sem isto, uma thread
   * que so cede a vez (sched_yield em laco, ex.: espera de futex) sempre se
   * reencontra na fila e as que dormem (poll, nanosleep) nunca acordam. */
  wake_due();
  for (;;) {
    struct sodos_thread* next = q_pop(&run_h, &run_t);
    if (next) {
      struct sodos_thread* prev = cur;
      next->state = ST_RUN;
      cur = next;
      if (next != prev) {
        sodos__tls_switch(next->tls_tp);
        sodos_ctx_switch(&prev->rsp, next->rsp);
        /* (ja na thread que voltou a rodar) a fatia dela recomeca */
        coop_last = sodos__ms();
        coop_flag = 0;
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
    stacks_check();
  }
}

void sodos__sched_init(void) {
  if (sched_inited) return;
  sched_inited = 1;
  memset(&main_thread, 0, sizeof(main_thread));
  main_thread.magic = MAIN_MAGIC;
  main_thread.state = ST_RUN;
  cur = &main_thread;
  sodos__kt = kt_choose();
  {
    /* Nao ha como passar ambiente pela linha de comando: vale tambem o
     * arquivo D:\\SODOS\\STACKS.AT com o numero de segundos. */
    char buf[16];
    const char* e = getenv("SODOS_STACKS_AT");
    int n = e ? -1 : k_fload("D:\\SODOS\\STACKS.AT", buf, (int)sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = 0;
      e = buf;
    }
    if (e && atoi(e) > 0) stacks_at = sodos__ms() + (uint64_t)atoi(e) * 1000u;
  }
  sodos__sync_init();
  {
    sodos_log(SODOS_LOG_INFO, "threads: %s (%d CPUs)",
              sodos__kt ? "k_thread (FFTHREADS)" : "cooperativas", k_cpu_count());
    ser_txt(sodos__kt ? "[ffthr] pthreads = k_threads (SET FFTHREADS=0 desliga)\n"
                      : "[ffthr] pthreads cooperativas\n");
  }
}

int sodos__started(void) { return sched_inited; }

/* CPUs que o programa deve achar que tem (sysconf, sched_getaffinity): com
 * k_threads, as do kernel; cooperativo, uma (mais threads so atrapalham). */
int sodos_ncpu(void) {
  int n;
  if (!sched_inited) sodos__sched_init();
  if (!sodos__kt) return 1;
  n = k_cpu_count();
  return n < 1 ? 1 : n;
}

struct sodos_thread* sodos__self(void) {
  if (!sched_inited) sodos__sched_init();
  if (sodos__kt) {
    struct sodos_thread* s = kt_self_get();
    return s ? s : &main_thread;
  }
  return cur;
}

/* Erro de uso de pthread no modo k_thread: vai para a serial (o log do port
 * so e gravado com sodos_init) com o endereco de quem chamou. */
void sodos__kt_warn(const char* what, const void* obj) {
  ser_txt("[ffthr] ");
  ser_txt(what);
  ser_txt(" obj=");
  ser_hex((uint64_t)(uintptr_t)obj);
  ser_txt(" thr=");
  ser_hex((uint64_t)(uintptr_t)kt_self_get());
  ser_txt(" ret=");
  ser_hex((uint64_t)(uintptr_t)__builtin_return_address(0) - (uintptr_t)__executable_start);
  ser_txt("\n");
}

/* errno por thread (c_compiler/lib/stdlib.c chama se existir); NULL = o global. */
int* __sodos_errno(void) {
  struct sodos_thread* s;
  if (!sodos__kt) return NULL;
  s = kt_self_get();
  return s ? &s->err : &main_thread.err;
}

static void kt_yield(void) {
  int d = sodos__big_release();
  if (kt_self_get() != &main_thread) {
    k_thread_yield();
  } else {
    /* Principal (mundo/WIN): o WIN ja a preempta no IRQ0; cede de vez em
     * quando para o desktop ler entrada mesmo se ela girar em sched_yield. */
    uint64_t now = sodos__ms();
    if (now - os_last >= OS_SLICE_MS) {
      k_yield();
      os_last = sodos__ms();
    }
  }
  sodos__big_reacquire(d);
}

void sodos__yield(void) {
  if (!sched_inited) sodos__sched_init();
  if (sodos__kt) {
    kt_yield();
    return;
  }
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
  if (sodos__kt) {
    /* Espera num futex que ninguem acorda: o kernel devolve no prazo. No
     * mundo (WIN) o desktop roda enquanto isso; nas k_threads a CPU fica
     * livre para as outras. */
    volatile int z = 0;
    int d = sodos__big_release();
    k_futex_wait(&z, 0, (int)(ms > 0x7FFFFFFFu ? 0x7FFFFFFFu : ms));
    sodos__big_reacquire(d);
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

/* ---------- FFTHREADS: criacao e fim ---------- */

static int kt_entry(void* p) {
  struct sodos_thread* t = (struct sodos_thread*)p;
  k_thread_tls((unsigned long long)(uintptr_t)t->tls_tp);
  kt_self_set(t);
  sodos_thread_finish(t->fn(t->arg));
  return 0;
}

static void kt_free(struct sodos_thread* t) {
  free(t->tls_raw);
  t->magic = 0;
  free(t);
}

/* Threads destacadas que ja terminaram: o kernel solta o slot e a pilha no
 * k_thread_join; o bloco TLS so pode sair depois (o codigo do kernel que a
 * thread roda ate o fim le o guarda de pilha em %fs:0x28). */
static void kt_reap(void) {
  struct sodos_thread** pp;
  struct sodos_thread* done = NULL;
  sodos__fl_lock(&kt_zlock);
  pp = &kt_zombies;
  while (*pp) {
    struct sodos_thread* z = *pp;
    if (__atomic_load_n(&z->finished, __ATOMIC_ACQUIRE)) {
      *pp = z->znext;
      z->znext = done;
      done = z;
      continue;
    }
    pp = &z->znext;
  }
  sodos__fl_unlock(&kt_zlock);
  while (done) {
    struct sodos_thread* z = done;
    done = z->znext;
    k_thread_join(z->ktid, NULL);
    kt_free(z);
  }
}

static void kt_zombie_add(struct sodos_thread* t) {
  sodos__fl_lock(&kt_zlock);
  t->znext = kt_zombies;
  kt_zombies = t;
  sodos__fl_unlock(&kt_zlock);
}

static int kt_create(pthread_t* out, const pthread_attr_t* attr,
                     void* (*fn)(void*), void* arg) {
  struct sodos_thread* t;
  size_t stacksize;
  int tid;

  kt_reap();
  t = (struct sodos_thread*)calloc(1, sizeof(*t));
  if (!t) return EAGAIN;
  t->fn = fn;
  t->arg = arg;
  t->magic = THREAD_MAGIC;
  t->detached = attr ? attr->detached : 0;
  t->state = ST_RUN;
  t->tls_tp = sodos__tls_new(&t->tls_raw);
  if (!t->tls_tp) {
    free(t);
    return EAGAIN;
  }
  stacksize = (attr && attr->stacksize) ? attr->stacksize : DEFAULT_STACK;
  if (stacksize < KT_STACK_MIN) stacksize = KT_STACK_MIN;
  stacksize += KT_STACK_SLACK;
  if (stacksize > 0x10000000u) stacksize = 0x10000000u;
  t->stack_size = (uint32_t)stacksize;
  /* Antes de criar: a thread pode terminar antes de k_thread_create voltar. */
  if (t->detached) kt_zombie_add(t);
  tid = k_thread_create(kt_entry, t, (unsigned)stacksize, 0);
  if (tid <= 0) {
    sodos_log(SODOS_LOG_ERROR, "pthread_create: k_thread_create=%d (pilha %u)", tid,
              (unsigned)stacksize);
    sodos__kt_warn("pthread_create: k_thread_create falhou", (void*)(intptr_t)tid);
    if (t->detached) {
      /* tira da lista de novo */
      struct sodos_thread** pp;
      sodos__fl_lock(&kt_zlock);
      for (pp = &kt_zombies; *pp; pp = &(*pp)->znext)
        if (*pp == t) {
          *pp = t->znext;
          break;
        }
      sodos__fl_unlock(&kt_zlock);
    }
    kt_free(t);
    return EAGAIN;
  }
  /* kt_reap so le ktid depois de finished (que a thread grava no fim). */
  __atomic_store_n(&t->ktid, tid, __ATOMIC_RELEASE);
  if (out) *out = t;
  return 0;
}

static void kt_finish(struct sodos_thread* t, void* retval) {
  t->retval = retval;
  sodos__tls_run_destructors(t);
  while (sodos__big_release()) {
  }
  /* Espera o criador gravar o tid (kt_reap faz k_thread_join com ele). */
  while (!__atomic_load_n(&t->ktid, __ATOMIC_ACQUIRE)) k_thread_yield();
  t->state = ST_DEAD;
  __atomic_store_n(&t->finished, 1, __ATOMIC_RELEASE);
  k_thread_exit(0);
  for (;;) {}
}

int pthread_create(pthread_t* out, const pthread_attr_t* attr,
                   void* (*fn)(void*), void* arg) {
  struct sodos_thread* t;
  size_t stacksize;

  if (!fn) return EINVAL;
  if (!sched_inited) sodos__sched_init();
  if (sodos__kt) return kt_create(out, attr, fn, arg);

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
  all_add(t);
  q_push(&run_h, &run_t, t);
  if (out) *out = t;
  return 0;
}

void sodos_thread_finish(void* retval) {
  struct sodos_thread* t = cur;
  struct sodos_thread* j;

  if (sodos__kt) {
    t = kt_self_get();
    if (t && t != &main_thread) kt_finish(t, retval);
    k_exit(0);   /* pthread_exit na principal: o programa acaba */
    for (;;) {}
  }

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
  if (sodos__kt) {
    int d;
    if (t == sodos__self()) return EDEADLK;
    d = sodos__big_release();
    k_thread_join(t->ktid, NULL);
    sodos__big_reacquire(d);
    if (retval) *retval = t->retval;
    kt_free(t);
    return 0;
  }
  while (!t->finished)
    sodos__block((void**)&t->join_head, (void**)&t->join_tail);
  if (retval) *retval = t->retval;
  if (t->alloc) k_free((void*)(uintptr_t)t->alloc);
  free(t->tls_raw);
  t->magic = 0;
  all_del(t);
  free(t);
  return 0;
}

int pthread_detach(pthread_t t) {
  if (!t || t->magic != THREAD_MAGIC) return ESRCH;
  if (sodos__kt) {
    if (t->detached) return EINVAL;
    t->detached = 1;
    kt_zombie_add(t);
    return 0;
  }
  t->detached = 1;
  if (t->finished && t->state == ST_DEAD) {
    t->qnext = zombie_h;
    zombie_h = t;
  }
  return 0;
}

pthread_t pthread_self(void) { return sodos__self(); }

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

int pthread_yield(void) {
  sodos__yield();
  return 0;
}

int sched_yield(void) {
  sodos__yield();
  return 0;
}

/* ---------- fatia de tempo cooperativa ----------
 * Para lacos longos que nunca chamam o sistema (a vCPU do QEMU rodando o
 * guest): sodos_coop_due diz se a thread ja passou da fatia (~10 ms) e
 * sodos_coop_yield cede a vez se passou. A contagem evita ler o PIT
 * (k_ms) a cada chamada. O poll() tambem chama sodos_coop_yield: um laco
 * de eventos com fd sempre pronto nao pode prender as outras threads. */
int sodos_coop_due(void) {
  uint64_t now;
  if (sodos__kt) return 0;   /* k_threads: o timer do kernel ja preempta */
  if (coop_flag) return 1;
  if (++coop_n & 63) return 0;
  now = sodos__ms();
  if (now - coop_last < COOP_SLICE_MS) return 0;
  coop_flag = 1;
  return 1;
}

void sodos_coop_yield(void) {
  if (!sodos_coop_due()) return;
  sodos__yield();
  /* Sem outra thread pronta o yield volta direto: comeca outra fatia. */
  coop_last = sodos__ms();
  coop_flag = 0;
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

/* ---------- futex (FUTEX_WAIT / FUTEX_WAKE da libc LP64) ----------
 * Antes o FUTEX_WAIT da libc (c_compiler/lib/misc.c) cedia a vez em laco ate o
 * valor mudar e o FUTEX_WAKE nao fazia nada. Com as threads do Rust (Glean,
 * WebRender, rayon) esperando assim, cada volta do escalonador passava por
 * todas: troca de TLS (wrmsr/wrfsbase, que numa VM saem para o emulador) e
 * relogio a cada uma, ~10% da CPU com o Firefox aberto (tools/qprof).
 * Agora a thread dorme na fila de sono e o wake a acorda na hora. A fatia de
 * FX_SLICE_MS e so a rede para quem mudar o valor sem chamar wake. */
#define FX_SLICE_MS 50

struct fx_wait {
  volatile int* addr;
  struct sodos_thread* t;
  int woken;
  struct fx_wait* next;
};
static struct fx_wait* fx_head;

/* Tira t da fila de sono antes do prazo e a poe na de prontos. */
static void unsleep(struct sodos_thread* t) {
  struct sodos_thread** pp = &sleep_h;
  if (!t || t->state != ST_SLEEP) return;
  while (*pp && *pp != t) pp = &(*pp)->sleep_next;
  if (*pp) *pp = t->sleep_next;
  t->sleep_next = NULL;
  make_ready(t);
}

static void fx_unlink(struct fx_wait* w) {
  struct fx_wait** pp = &fx_head;
  while (*pp && *pp != w) pp = &(*pp)->next;
  if (*pp) *pp = w->next;
}

/* 0 acordou (ou o valor mudou); 1 o valor ja era outro (EAGAIN);
 * 2 venceu o prazo (ETIMEDOUT). deadline_ms: sodos__ms() absoluto, 0 = sem. */
int sodos_futex_wait(volatile int* addr, int val, uint64_t deadline_ms) {
  struct fx_wait w;
  if (!sched_inited) sodos__sched_init();
  if (*addr != val) return 1;
  if (sodos__kt) {
    int tmo = -1, r;
    if (deadline_ms) {
      uint64_t now = sodos__ms();
      if (now >= deadline_ms) return 2;
      tmo = deadline_ms - now > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)(deadline_ms - now);
    }
    r = k_futex_wait(addr, val, tmo);
    if (r == -11) return 1;
    if (r == -110) return 2;
    return 0;
  }
  w.addr = addr;
  w.t = cur;
  w.woken = 0;
  w.next = fx_head;
  fx_head = &w;
  for (;;) {
    uint64_t now = sodos__ms();
    unsigned slice = FX_SLICE_MS;
    if (w.woken || *addr != val) break;
    if (deadline_ms) {
      if (now >= deadline_ms) {
        fx_unlink(&w);
        return 2;
      }
      if (deadline_ms - now < slice) slice = (unsigned)(deadline_ms - now);
    }
    sodos__sleep_ms(slice ? slice : 1);
  }
  fx_unlink(&w);
  return 0;
}

/* Acorda ate n threads esperando em addr; devolve quantas. */
int sodos_futex_wake(volatile int* addr, int n) {
  struct fx_wait* w;
  int k = 0;
  if (sodos__kt) return k_futex_wake(addr, n);
  for (w = fx_head; w && k < n; w = w->next) {
    if (w->addr != addr || w->woken) continue;
    w->woken = 1;
    unsleep(w->t);
    k++;
  }
  return k;
}
