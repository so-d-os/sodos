/* kthread.c - escalonador SMP: filas por CPU, threads de programas, futex,
 * balanceamento e o lock grande do kernel (BKL). Ver kthread.h.
 *
 * Regras de lock (sempre com IRQs desligadas enquanto se segura um spinlock):
 *   fx_lock (futex)  ->  rq lock de uma CPU
 *   dois rq locks: em ordem de indice (move_one)
 * O BKL e outra coisa: e pego com IRQs ligadas, gira (cedendo a CPU a outras
 * threads se houver) e nunca e segurado por quem dorme.
 *
 * Troca de contexto: ctx_switch (modrun.asm) salva rbx/rbp/r12-r15 na pilha
 * da tarefa; o resto do estado (FPU/SSE/AVX por xsave, base do FS) e salvo
 * aqui. A preempcao acontece dentro do handler do timer: o frame da
 * interrupcao fica na pilha da thread e o iretq sai quando ela volta.
 * on_cpu impede que outra CPU retome uma tarefa antes de a CPU dela terminar
 * de salvar o rsp (finish_switch zera o on_cpu da anterior).
 */
#include <stdint.h>
#include "kthread.h"
#include "smp.h"
#include "serial.h"
#include "mm.h"
#include "module.h"
#include "irq.h"
#include "sched.h"
#include "vt.h"
int fs_held_by_me(void); /* kexports.c */

void con_break_poll(void);
void mod_exit(int code);

enum { T_FREE = 0, T_RUNNING, T_READY, T_BLOCKED, T_DEAD };
enum { K_WORLD = 1, K_IDLE, K_THREAD };

#define SLICE_MS     8      /* fatia de uma thread antes do round-robin */
#define BAL_MS       50     /* balanceamento periodico (puxar) */
#define LEND_MS      10     /* o mundo empresta a CPU no maximo isto por vez */
#define XS_MAX       4096
#define STACK_DEF    (256u * 1024u)
#define STACK_MIN    (16u * 1024u)

#define E_AGAIN     -11
#define E_TIMEDOUT  -110
#define E_SRCH      -3
#define E_INVAL     -22
#define E_NOMEM     -12
#define E_DEADLK    -35

struct spin { volatile int v; };

struct task {
  uint64_t rsp;
  uint8_t* xs;              /* area do xsave/fxsave (alinhada a 64); 0 = idle */
  uint8_t* xs_raw;          /* bloco do kmalloc de xs (so threads de programa) */
  uint64_t fs;
  volatile int state;
  volatile int on_cpu;
  volatile int cpu;
  volatile int migrate_to;  /* >= 0: vai para essa CPU ao sair desta */
  volatile uint32_t mask;   /* afinidade */
  int bkl_depth;
  int kind;
  int tid;
  uint64_t last_exp;        /* ultimo k_* chamado pelo thunk (if_guard) */
  uint32_t pid;
  char drive;
  struct task* qnext;
  volatile int* fw_addr;
  struct task* fw_next;
  uint32_t fw_deadline;     /* irq_ticks; 0 = sem limite */
  volatile int fw_woken;
  volatile int fw_listed;
  volatile int killed;
  volatile int exited;      /* palavra do join */
  int exit_code;
  int detached;
  uint8_t* stack;
  int (*fn)(void*);
  void* arg;
  uint32_t slice_ms;
  struct mod_tsave msave;
  uint64_t teb;             /* TEB do Windows (WINE.DLL); 0 = nao e thread Windows */
};

/* self e curr nos offsets 0 e 0x18: lidos com um mov %gs:N (atomico frente a
 * preempcao, como o "current" do Linux).
 *
 * Thread do Windows (task.teb != 0, WINE.DLL): o codigo dela le o TEB direto
 * pelo gs (gs:8 StackBase, gs:0x10 StackLimit, gs:0x30, gs:0x58, e o Go ate
 * gs:0x1480+n), entao enquanto ela roda a base do GS e o TEB, tambem dentro
 * do kernel. Para o kernel continuar achando a CPU e a tarefa, TEB+0
 * (ExceptionList, sem uso em x64) recebe a struct cpu e TEB+0x18
 * (SubSystemTib, sem uso em x64) a task a cada troca (gs_load). TEB+8 e +0x10
 * ficam com os limites da pilha (o V8 confere o rsp contra o StackBase).
 * Sem thread Windows nada disso roda: o GS fica sempre na struct cpu. */
struct cpu {
  struct cpu* self;
  uint64_t unused8;         /* no TEB: StackBase */
  uint64_t gs_now;          /* base do GS carregada nesta CPU */
  struct task* volatile curr; /* offset 0x18 */
  int index;
  volatile int online;
  struct spin lock;
  struct task* idle;
  struct task* qhead;
  struct task* qtail;
  volatile int nr;
  volatile int need_resched;
  struct task* last;
  volatile uint32_t busy_ms, idle_ms;
  uint32_t bal_ms;
  volatile uint32_t tlb_ack;
  uint32_t switches;
};

static struct cpu cpus[KT_CPU_MAX];
static volatile uint32_t online_mask = 1;
static volatile int n_online = 1;
/* Limite termico (kt_set_cpu_cap): CPUs de indice >= cpu_cap ficam
 * "estacionadas" em hlt, sem threads. A CPU0 (o mundo) nunca para. */
static volatile int cpu_cap = KT_CPU_MAX;
static inline int parked(const struct cpu* c) { return c->index != 0 && c->index >= cpu_cap; }
static struct cpu* pick_cpu(uint32_t mask);
static struct task world;
static struct task idle_t[KT_CPU_MAX];
static struct task kts[KT_MAX];
static uint8_t world_xs[XS_MAX] __attribute__((aligned(64)));
static uint8_t idle0_stack[16384] __attribute__((aligned(16)));
static struct spin fx_lock;
static struct task* fx_head;
static volatile int bkl_lock;
static volatile int bkl_waiters;   /* threads girando no BKL */
static struct task* volatile bkl_owner;
static int kt_up;
static volatile int stopping;
static volatile int stop_cpu = -1;
static int next_tid = 1;
static volatile uint32_t tlb_gen;
static int xs_mode;               /* 0 = fxsave, 1 = xsave */
static uint32_t xs_lo, xs_hi, xs_size = 512;
static int fsgs;
/* Contadores para o comando smp (diagnostico). */
static volatile uint32_t st_bkl_spin, st_lend, st_fx_wait, st_fx_timeout, st_world_bkl_ms;

/* ---------- primitivas ---------- */
static inline void cpu_pause(void) { __asm__ volatile ("pause" ::: "memory"); }
static inline struct cpu* this_cpu(void);
static inline int stop_here(void);
static inline uint64_t irq_save(void) {
  uint64_t f;
  __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
  return f;
}
static inline void irq_restore(uint64_t f) {
  if (f & 0x200u) __asm__ volatile ("sti" ::: "memory");
}
static inline void spin_lock(struct spin* l) {
  while (__atomic_exchange_n(&l->v, 1, __ATOMIC_ACQUIRE))
    while (l->v) cpu_pause();
}
static inline void spin_unlock(struct spin* l) { __atomic_store_n(&l->v, 0, __ATOMIC_RELEASE); }

static inline struct cpu* this_cpu(void) {
  struct cpu* c;
  if (!kt_up) return &cpus[0];
  __asm__ volatile ("mov %%gs:0, %0" : "=r"(c));
  return c;
}
static inline struct task* cur(void) {
  struct task* t;
  if (!kt_up) return &world;
  __asm__ volatile ("mov %%gs:0x18, %0" : "=r"(t));
  return t;
}

/* Excecao fatal em outra CPU: esta para (a que trata a falha continua). */
static inline int stop_here(void) { return stopping && this_cpu()->index != stop_cpu; }

static void gs_write(uint64_t v);
/* Base do GS da tarefa t nesta CPU (interrupcoes desligadas). */
static inline void gs_load(struct cpu* c, struct task* t) {
  uint64_t v = (uint64_t)(uintptr_t)c;
  if (t->teb) {
    uint64_t* tb = (uint64_t*)(uintptr_t)t->teb;
    tb[0] = (uint64_t)(uintptr_t)c;
    tb[3] = (uint64_t)(uintptr_t)t; /* TEB+0x18 */
    v = t->teb;
  }
  if (c->gs_now != v) {
    c->gs_now = v;
    gs_write(v);
  }
}

static uint64_t fs_read(void) {
  uint32_t lo, hi;
  if (fsgs) {
    uint64_t v;
    __asm__ volatile ("rdfsbase %0" : "=r"(v));
    return v;
  }
  __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
  return ((uint64_t)hi << 32) | lo;
}
static void fs_write(uint64_t v) {
  if (fsgs) {
    __asm__ volatile ("wrfsbase %0" :: "r"(v) : "memory");
    return;
  }
  __asm__ volatile ("wrmsr" :: "c"(0xC0000100), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static void fpu_save(struct task* t) {
  if (!t->xs) return;
  if (xs_mode) __asm__ volatile ("xsave64 (%0)" :: "r"(t->xs), "a"(xs_lo), "d"(xs_hi) : "memory");
  else __asm__ volatile ("fxsave64 (%0)" :: "r"(t->xs) : "memory");
}
static void fpu_restore(struct task* t) {
  if (!t->xs) return;
  if (xs_mode) __asm__ volatile ("xrstor64 (%0)" :: "r"(t->xs), "a"(xs_lo), "d"(xs_hi) : "memory");
  else __asm__ volatile ("fxrstor64 (%0)" :: "r"(t->xs) : "memory");
}
/* Estado inicial: x87 com FCW 0x37F, MXCSR 0x1F80, XSTATE_BV 0 (o resto
 * volta ao estado de reset no xrstor). */
static void fpu_init_area(uint8_t* xs) {
  uint32_t i;
  for (i = 0; i < xs_size; i++) xs[i] = 0;
  *(uint16_t*)xs = 0x037F;
  *(uint32_t*)(xs + 24) = 0x1F80;
  *(uint32_t*)(xs + 28) = 0xFFFF;   /* MXCSR_MASK (ignorado no restore) */
}

static void xs_probe(void) {
  uint64_t cr4;
  uint32_t a, b, c, d;
  __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
  fsgs = (cr4 >> 16) & 1;
  xs_mode = 0;
  xs_size = 512;
  if (!(cr4 & (1u << 18))) return;              /* OSXSAVE */
  __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0xD), "c"(0));
  if (b < 576 || b > XS_MAX) return;
  __asm__ volatile ("xgetbv" : "=a"(xs_lo), "=d"(xs_hi) : "c"(0));
  xs_mode = 1;
  xs_size = b;
}

static void put_dec(uint32_t v) {
  char b[12];
  int i = 0;
  if (!v) {
    serial_putc('0');
    return;
  }
  while (v && i < 11) {
    b[i++] = (char)('0' + v % 10u);
    v /= 10u;
  }
  while (i > 0) serial_putc(b[--i]);
}

/* ---------- fila por CPU (com o lock dela) ---------- */
static void rq_push(struct cpu* c, struct task* t) {
  t->qnext = 0;
  if (c->qtail) c->qtail->qnext = t;
  else c->qhead = t;
  c->qtail = t;
  c->nr++;
  t->cpu = c->index;
}

static void rq_remove(struct cpu* c, struct task* t) {
  struct task* p = 0;
  struct task* q = c->qhead;
  while (q && q != t) {
    p = q;
    q = q->qnext;
  }
  if (!q) return;
  if (p) p->qnext = t->qnext;
  else c->qhead = t->qnext;
  if (c->qtail == t) c->qtail = p;
  t->qnext = 0;
  c->nr--;
}

/* O mundo (shell/WIN) primeiro: e quem atende teclado, mouse e tela. */
static struct task* rq_pick(struct cpu* c) {
  struct task* t;
  for (t = c->qhead; t; t = t->qnext)
    if (t->kind == K_WORLD) break;
  if (!t) t = c->qhead;
  if (t) rq_remove(c, t);
  return t;
}

static int load_of(const struct cpu* c) {
  return c->nr + (c->curr != c->idle ? 1 : 0);
}

static void resched_cpu(struct cpu* c) {
  c->need_resched = 1;
  if (c != this_cpu() && c->online) smp_send_ipi(c->index, 0xF3);
}

/* ---------- troca de contexto ---------- */
static void finish_switch(struct cpu* c) {
  struct task* last = c->last;
  c->last = 0;
  if (!last) return;
  if (last->migrate_to >= 0) {
    struct cpu* d = &cpus[last->migrate_to];
    last->migrate_to = -1;
    __atomic_store_n(&last->on_cpu, 0, __ATOMIC_RELEASE);
    spin_lock(&d->lock);
    rq_push(d, last);
    spin_unlock(&d->lock);
    resched_cpu(d);
  } else {
    __atomic_store_n(&last->on_cpu, 0, __ATOMIC_RELEASE);
  }
}

static int first_cpu(uint32_t mask) {
  int i;
  for (i = 0; i < KT_CPU_MAX; i++)
    if (((mask & online_mask) >> i) & 1u) return i;
  return 0;
}

static void kt_schedule(void) {
  uint64_t fl = irq_save();
  struct cpu* c = this_cpu();
  struct task* prev = c->curr;
  struct task* next;

  if (stop_here()) {
    for (;;) __asm__ volatile ("cli; hlt");
  }
  if (parked(c) && prev->kind == K_THREAD && prev->migrate_to < 0 &&
      (prev->state == T_RUNNING || prev->state == T_READY)) {
    struct cpu* d = pick_cpu(prev->mask);
    if (d != c && !parked(d)) prev->migrate_to = d->index;
  }
  spin_lock(&c->lock);
  c->need_resched = 0;
  if (prev->kind != K_IDLE && prev->migrate_to < 0 &&
      (prev->state == T_RUNNING || prev->state == T_READY)) {
    prev->state = T_READY;
    if (!((prev->mask >> c->index) & 1u)) prev->migrate_to = first_cpu(prev->mask);
    else rq_push(c, prev);
  } else if (prev->migrate_to >= 0 && prev->state == T_RUNNING) {
    prev->state = T_READY;
  }
  next = rq_pick(c);
  if (!next) next = c->idle;
  if (next == prev) {
    prev->state = T_RUNNING;
    prev->slice_ms = 0;
    spin_unlock(&c->lock);
    irq_restore(fl);
    return;
  }
  next->state = T_RUNNING;
  next->cpu = c->index;
  next->slice_ms = 0;
  c->curr = next;
  c->switches++;
  spin_unlock(&c->lock);

  fpu_save(prev);
  prev->fs = fs_read();
  while (__atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE)) cpu_pause();
  next->on_cpu = 1;
  c->last = prev;
  ctx_switch(&prev->rsp, next->rsp);
  /* De volta como prev, talvez noutra CPU. */
  c = this_cpu();
  /* Antes do finish_switch: ele libera a tarefa anterior para outra CPU, e
   * se ela for thread Windows o TEB (o GS daqui ate agora) muda de dono. */
  gs_load(c, prev);
  finish_switch(c);
  fpu_restore(prev);
  if (fs_read() != prev->fs) fs_write(prev->fs);
  irq_restore(fl);
}

/* Pedido de limpeza da TLB pendente para esta CPU (kt_tlb_shootdown)?
 * Atende sem esperar a IPI: quem espera o BKL ou outra limpeza pode estar com
 * as interrupcoes desligadas, e a CPU dona do BKL ficava girando no
 * kt_tlb_shootdown ate o limite (segundos no PC real) a cada VirtualAlloc ou
 * VirtualProtect: a CPU0 (o mundo) presa no bkl_take, teclado e tela parados,
 * o Claude levando 20+ min para abrir no notebook. */
static void kt_tlb_poll(void) {
  struct cpu* c = this_cpu();
  uint32_t g = __atomic_load_n(&tlb_gen, __ATOMIC_ACQUIRE);
  if (c->tlb_ack != g) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
    c->tlb_ack = g;
  }
}

/* ---------- BKL ---------- */
static void kt_die(struct task* t) __attribute__((noreturn));
static void yield_raw(struct task* t);

/* Threads dormindo a espera do BKL (bkl_sleep). Quem solta acorda uma. */
static volatile int bkl_sleepers;
static volatile uint32_t st_bkl_sleep;
static void bkl_sleep(struct task* t);
#define BKL_SPINS 400   /* voltas girando antes de dormir */

static void bkl_take(struct task* t, int depth) {
  if (depth <= 0) return;
  int waiting = 0;
  int spins = 0;
  if (bkl_owner == t) {
    t->bkl_depth = depth;
    return;
  }
  for (;;) {
    uint64_t fl = irq_save();
    if (!bkl_lock && !__atomic_exchange_n(&bkl_lock, 1, __ATOMIC_ACQUIRE)) {
      bkl_owner = t;
      t->bkl_depth = depth;
      if (t->kind == K_THREAD) mod_thread_enter(&t->msave, t->pid, t->drive);
      if (waiting) __atomic_sub_fetch(&bkl_waiters, 1, __ATOMIC_RELAXED);
      irq_restore(fl);
      return;
    }
    irq_restore(fl);
    if (!waiting && t->kind == K_THREAD) {
      waiting = 1;
      __atomic_add_fetch(&bkl_waiters, 1, __ATOMIC_RELAXED);
    }
    if (waiting && t->killed) __atomic_sub_fetch(&bkl_waiters, 1, __ATOMIC_RELAXED);
    st_bkl_spin++;
    kt_tlb_poll();
    if (t->kind == K_THREAD) {
      if (t->killed) kt_die(t);
      /* Girava para sempre (com yield a cada volta se havia outra thread na
       * fila): com o BKL ocupado por uma leitura de disco, todas as CPUs
       * ficavam a 100% no kt_schedule/bkl_take (notebook: 8 nucleos a 100%
       * por minutos ate desligar; VM: ~1,8 bilhao de trocas em 9 min) e
       * quem tinha trabalho de verdade nao recebia CPU. Agora gira um pouco
       * (o BKL costuma ficar preso por microssegundos) e depois dorme ate
       * quem soltar acordar, ou 2 ms. So com as interrupcoes ligadas em quem
       * chamou. */
      if (++spins >= BKL_SPINS && (fl & 0x200u)) {
        bkl_sleep(t);
        spins = BKL_SPINS / 2;
        continue;
      }
    }
    cpu_pause();
  }
}

static int bkl_drop(struct task* t) {
  uint64_t fl;
  int d;
  if (bkl_owner != t) return 0;
  fl = irq_save();
  d = t->bkl_depth;
  if (t->kind == K_THREAD) mod_thread_leave(&t->msave);
  t->bkl_depth = 0;
  bkl_owner = 0;
  __atomic_store_n(&bkl_lock, 0, __ATOMIC_SEQ_CST);
  irq_restore(fl);
  if (__atomic_load_n(&bkl_sleepers, __ATOMIC_SEQ_CST) > 0) kt_futex_wake((volatile int*)&bkl_lock, 1);
  return d;
}

/* Uma thread de programa nunca pode rodar com as interrupcoes desligadas: sem
 * o tique ela nao e preemptada, e se ficar num laco (o Bun do Claude Code
 * gira chamando k_futex_wake) as outras threads da fila daquela CPU param
 * para sempre (VM 2026-11-20: CPU2 com "trocas" congelado, fila=67, IRR com o
 * vetor do timer pendente, RIP no programa com RFLAGS.IF=0; o Claude ficava
 * em "pensando" sem fim e o teclado morria). Confere na entrada e na saida de
 * todo k_*: religa e avisa no serial qual chamada foi, para achar quem deixa
 * o IF desligado. */
static volatile uint32_t st_if_fix;
static void if_guard2(struct task* t, const char* onde, uint64_t alvo, uint64_t quem);
static void if_guard(struct task* t, const char* onde, uint64_t alvo) { if_guard2(t, onde, alvo, 0); }
static void if_guard2(struct task* t, const char* onde, uint64_t alvo, uint64_t quem) {
  static const char hx[] = "0123456789ABCDEF";
  uint64_t f;
  uint32_t n;
  char b[17];
  int i;
  if (t->kind != K_THREAD) return;
  __asm__ volatile ("pushfq; pop %0" : "=r"(f) :: "memory");
  if (f & 0x200u) return;
  __asm__ volatile ("sti" ::: "memory");
  n = __atomic_add_fetch(&st_if_fix, 1u, __ATOMIC_RELAXED);
  if (n > 40u && (n & 0xFFFu)) return;
  for (i = 0; i < 16; i++) b[i] = hx[(alvo >> (60 - 4 * i)) & 15u];
  b[16] = 0;
  serial_puts("[kt] IF desligado numa thread de programa, religado (");
  serial_puts(onde);
  serial_puts(") alvo=");
  serial_puts(b);
  for (i = 0; i < 16; i++) b[i] = hx[(quem >> (60 - 4 * i)) & 15u];
  serial_puts(" chamador=");
  serial_puts(b);
  serial_puts(" pid=");
  put_dec((uint32_t)t->pid);
  serial_puts(" tid=");
  put_dec((uint32_t)t->tid);
  serial_puts(" vezes=");
  put_dec(n);
  serial_puts("\n");
}
void kt_if_check(const char* onde, uint64_t marca) {
  struct task* t = cur();
  if_guard2(t, onde, t->last_exp, marca);
  if (marca && t->kind == K_THREAD) t->last_exp = marca;
}

void kt_bkl_enter(void) {
  struct task* t = cur();
  if (bkl_owner == t) {
    t->bkl_depth++;
    return;
  }
  if (t->kind == K_THREAD) {
    /* chamado pelo bkl_thunk_common: o alvo (r11) e o ultimo push antes do call */
    /* o alvo que aparece e o k_* ANTERIOR desta thread: quem pode ter
     * deixado o IF desligado */
    {
      /* quadro do bkl_thunk_common: [rbp] = rbp do programa, [rbp+8] = retorno */
      const uint64_t* fa = (const uint64_t*)__builtin_frame_address(0);
      const uint64_t* tb = (const uint64_t*)fa[0];
      if_guard2(t, "entrada de k_, anterior", t->last_exp, tb ? tb[1] : 0);
      t->last_exp = tb ? tb[1] : fa[2];
    }
  }
  bkl_take(t, 1);
}

/* Para codigo do kernel que roda numa kthread (thermal.c, prof_saver do
 * cmd_smp.c): pega o BKL sem olhar o quadro do bkl_thunk_common. O
 * kt_bkl_enter le [rbp] e [rbp+8] supondo que veio de um programa; numa
 * funcao do kernel compilada com -O2 o rbp e um registrador qualquer e a
 * leitura dava #PF (CRASH.LOG de 2026-11-21: CR2=0x5A, rbp=0x52). */
void kt_bkl_enter_kernel(void) {
  struct task* t = cur();
  if (bkl_owner == t) {
    t->bkl_depth++;
    return;
  }
  bkl_take(t, 1);
}

uint64_t kt_bkl_leave(void) {
  struct task* t = cur();
  if (bkl_owner != t) return 0;
  if (t->bkl_depth > 1) {
    t->bkl_depth--;
    return 0;
  }
  bkl_drop(t);
  if (t->kind == K_THREAD) {
    if (t->killed) kt_die(t);
    if (this_cpu()->need_resched) {
      kt_schedule();
      if (t->killed) kt_die(t);
    }
    if_guard(t, "saida de k_", t->last_exp);
  }
  return 0;
}

int kt_bkl_suspend(void) {
  struct task* t = cur();
  if (t->kind != K_WORLD) return 0;
  return bkl_drop(t);
}

void kt_bkl_resume(int depth) {
  struct task* t = cur();
  if (t->kind != K_WORLD) return;
  if (depth <= 0) {
    bkl_drop(t);
    return;
  }
  if (bkl_owner == t) t->bkl_depth = depth;
  else bkl_take(t, depth);
}

int kt_bkl_depth(void) { return world.bkl_depth; }

/* Ponto ocioso do mundo (laco do WIN, espera de tecla, sched_yield): o
 * codigo do kernel que roda ali segura o BKL o tempo todo. Se ha thread
 * esperando, solta por um instante para ela entrar e pega de novo. */
void kt_bkl_breathe(void) {
  struct task* w = &world;
  int d, i;
  if (!bkl_waiters || cur() != w || bkl_owner != w) return;
  d = bkl_drop(w);
  /* quem espera pode estar dormindo (bkl_sleep): acordar e trocar de tarefa
   * leva mais que as 4000 voltas; espera ate alguem pegar, com limite */
  for (i = 0; i < (bkl_sleepers ? 200000 : 4000) && !bkl_lock && bkl_waiters; i++) cpu_pause();
  bkl_take(w, d);
}

void kt_bkl_set_depth(int depth) {
  if (depth < 1) depth = 1;
  if (bkl_owner == &world) world.bkl_depth = depth;
}

int kt_bkl_try_world(void) {
  if (bkl_owner == &world) {
    world.bkl_depth++;
    return 1;
  }
  if (!bkl_lock && !__atomic_exchange_n(&bkl_lock, 1, __ATOMIC_ACQUIRE)) {
    bkl_owner = &world;
    world.bkl_depth = 1;
    return 1;
  }
  return 0;
}

void kt_bkl_drop_world(void) {
  if (bkl_owner != &world) return;
  if (--world.bkl_depth > 0) return;
  world.bkl_depth = 0;
  bkl_owner = 0;
  __atomic_store_n(&bkl_lock, 0, __ATOMIC_RELEASE);
}

/* ---------- consultas ---------- */
int kt_in_thread(void) { return kt_up && cur()->kind == K_THREAD; }
int kt_world_here(void) { return !kt_up || cur() == &world; }
int kt_cpu_index(void) { return this_cpu()->index; }
int kt_stopping(void) { return stop_here(); }
int kt_online_count(void) { return n_online; }

/* Limite termico: so as CPUs 0..n-1 rodam threads (n >= 1). */
void kt_set_cpu_cap(int n) {
  int i;
  if (n < 1) n = 1;
  if (n > KT_CPU_MAX) n = KT_CPU_MAX;
  if (n == cpu_cap) return;
  cpu_cap = n;
  for (i = 1; i < KT_CPU_MAX; i++)       /* as estacionadas saem das threads; */
    if (cpus[i].online) resched_cpu(&cpus[i]);   /* as liberadas puxam trabalho */
}
int kt_cpu_cap(void) { return cpu_cap < n_online ? cpu_cap : n_online; }
uint32_t kt_cur_pid(void) { return cur()->pid; }

int kt_cpu_idle(int idx) {
  if (idx < 0 || idx >= KT_CPU_MAX) return 1;
  return cpus[idx].curr == cpus[idx].idle;
}

void kt_cpu_times(int idx, uint32_t* busy_ms, uint32_t* idle_ms) {
  if (idx < 0 || idx >= KT_CPU_MAX) return;
  if (busy_ms) *busy_ms = cpus[idx].busy_ms;
  if (idle_ms) *idle_ms = cpus[idx].idle_ms;
}

int kt_cpu_threads(int idx) {
  const struct cpu* c;
  if (idx < 0 || idx >= KT_CPU_MAX) return 0;
  c = &cpus[idx];
  return c->nr + (c->curr && c->curr->kind == K_THREAD ? 1 : 0);
}

void kt_dump(void) {
  int i;
  serial_puts("[kt] bkl_spin=");
  put_dec(st_bkl_spin);
  serial_puts(" bkl_sleep=");
  put_dec(st_bkl_sleep);
  serial_puts(" lend=");
  put_dec(st_lend);
  serial_puts(" fx_wait=");
  put_dec(st_fx_wait);
  serial_puts(" fx_timeout=");
  put_dec(st_fx_timeout);
  serial_puts(" mundo_com_bkl_ms=");
  put_dec(st_world_bkl_ms);
  serial_puts("\n");
  for (i = 0; i < KT_CPU_MAX; i++) {
    struct cpu* c = &cpus[i];
    if (!c->online) continue;
    serial_puts("[kt] cpu");
    put_dec((uint32_t)i);
    serial_puts(" trocas=");
    put_dec(c->switches);
    serial_puts(" fila=");
    put_dec((uint32_t)c->nr);
    serial_puts(c->curr == &world ? " (mundo)" : (c->curr == c->idle ? " (ociosa)" : " (thread)"));
    serial_puts("\n");
  }
}

/* ---------- acordar / balancear ---------- */
static struct cpu* find_idle(uint32_t mask, const struct cpu* not_this) {
  int i;
  for (i = 0; i < KT_CPU_MAX; i++) {
    struct cpu* d = &cpus[i];
    if (!d->online || d == not_this || !((mask >> i) & 1u) || parked(d)) continue;
    if (d->curr == d->idle && d->nr == 0) return d;
  }
  return 0;
}

static void kt_wake(struct task* t) {
  uint64_t fl = irq_save();
  struct cpu* c = &cpus[t->cpu];
  struct cpu* dest = c;
  spin_lock(&c->lock);
  if (t->state != T_BLOCKED) {
    spin_unlock(&c->lock);
    irq_restore(fl);
    return;
  }
  t->state = T_READY;
  if (c->curr == t) {               /* ainda nao saiu da CPU: schedule() repoe */
    spin_unlock(&c->lock);
    irq_restore(fl);
    return;
  }
  if (t->kind == K_THREAD && (parked(c) || c->curr != c->idle || c->nr > 0 || !((t->mask >> c->index) & 1u))) {
    struct cpu* d = find_idle(t->mask, c);
    if (d) dest = d;
    else if (!((t->mask >> c->index) & 1u)) dest = &cpus[first_cpu(t->mask)];
    if (t->kind == K_THREAD && parked(dest)) dest = pick_cpu(t->mask);
  }
  if (dest == c) {
    rq_push(c, t);
    spin_unlock(&c->lock);
  } else {
    spin_unlock(&c->lock);
    spin_lock(&dest->lock);
    rq_push(dest, t);
    spin_unlock(&dest->lock);
  }
  if (t->kind == K_WORLD || dest->curr == dest->idle) resched_cpu(dest);
  irq_restore(fl);
}

static int move_one(struct cpu* src, struct cpu* dst) {
  struct cpu* a = src->index < dst->index ? src : dst;
  struct cpu* b = src->index < dst->index ? dst : src;
  struct task* t;
  if (parked(dst)) return 0;
  spin_lock(&a->lock);
  spin_lock(&b->lock);
  for (t = src->qhead; t; t = t->qnext)
    if (t->kind == K_THREAD && !t->on_cpu && ((t->mask >> dst->index) & 1u)) break;
  if (t) {
    rq_remove(src, t);
    rq_push(dst, t);
  }
  spin_unlock(&b->lock);
  spin_unlock(&a->lock);
  return t != 0;
}

/* Puxar: a CPU c pega uma tarefa da fila mais cheia. */
static int kt_pull(struct cpu* c) {
  struct cpu* busiest = 0;
  int bl = 1, i;
  if (n_online < 2 || parked(c)) return 0;
  for (i = 0; i < KT_CPU_MAX; i++) {
    struct cpu* d = &cpus[i];
    int l;
    if (!d->online || d == c || d->nr == 0) continue;
    l = load_of(d);
    if (l > bl) {
      busiest = d;
      bl = l;
    }
  }
  if (!busiest || bl - load_of(c) < 2) return 0;
  return move_one(busiest, c);
}

/* Empurrar: quem tem fila manda uma tarefa para uma CPU parada. */
static void kt_push(struct cpu* c) {
  int i;
  if (n_online < 2) return;
  for (i = 0; i < KT_CPU_MAX && c->nr > 0; i++) {
    struct cpu* d = &cpus[i];
    if (!d->online || d == c || parked(d)) continue;
    if (d->curr != d->idle || d->nr != 0) continue;
    if (move_one(c, d)) resched_cpu(d);
  }
}

/* ---------- futex ---------- */
static void fx_unlink(struct task* t) {
  struct task** pp = &fx_head;
  while (*pp) {
    if (*pp == t) {
      *pp = t->fw_next;
      break;
    }
    pp = &(*pp)->fw_next;
  }
  t->fw_next = 0;
  t->fw_listed = 0;
}

/* fx_lock seguro, IRQs desligadas, t corrente nesta CPU. */
static void block_on(struct task* t, volatile int* addr, uint32_t deadline) {
  struct cpu* c = this_cpu();
  t->fw_addr = addr;
  t->fw_deadline = deadline;
  t->fw_woken = 0;
  t->fw_next = fx_head;
  fx_head = t;
  t->fw_listed = 1;
  spin_lock(&c->lock);
  t->state = T_BLOCKED;
  spin_unlock(&c->lock);
}

static uint32_t deadline_of(int timeout_ms) {
  uint32_t d;
  if (timeout_ms < 0) return 0;
  d = (uint32_t)irq_ticks() + (uint32_t)timeout_ms;
  return d ? d : 1;
}

static int thread_wait(struct task* t, volatile int* addr, int val, int timeout_ms) {
  uint64_t fl;
  int depth = bkl_drop(t);
  int rc;
  fl = irq_save();
  spin_lock(&fx_lock);
  if (*addr != val) {
    spin_unlock(&fx_lock);
    irq_restore(fl);
    bkl_take(t, depth);
    return E_AGAIN;
  }
  if (t->killed) {
    spin_unlock(&fx_lock);
    kt_die(t);
  }
  block_on(t, addr, deadline_of(timeout_ms));
  st_fx_wait++;
  spin_unlock(&fx_lock);
  kt_schedule();
  irq_restore(fl);
  if (t->killed) kt_die(t);
  /* sem prazo nao ha "tempo esgotado": uma volta sem wake e espuria e quem
   * chamou confere a palavra de novo (semantica de futex) */
  rc = t->fw_woken ? 0 : (timeout_ms < 0 ? 0 : E_TIMEDOUT);
  bkl_take(t, depth);
  return rc;
}

/* Dorme ate o BKL ser solto (bkl_drop acorda uma) ou 2 ms. bkl_sleepers
 * sobe ANTES de conferir a trava e quem solta zera a trava ANTES de ler
 * bkl_sleepers (as duas com barreira total): um dos dois ve o outro. */
static void bkl_sleep(struct task* t) {
  uint64_t fl = irq_save();
  __atomic_add_fetch(&bkl_sleepers, 1, __ATOMIC_SEQ_CST);
  spin_lock(&fx_lock);
  if (!__atomic_load_n(&bkl_lock, __ATOMIC_SEQ_CST) || t->killed) {
    spin_unlock(&fx_lock);
    __atomic_sub_fetch(&bkl_sleepers, 1, __ATOMIC_SEQ_CST);
    irq_restore(fl);
    return;
  }
  block_on(t, (volatile int*)&bkl_lock, deadline_of(2));
  st_bkl_sleep++;
  spin_unlock(&fx_lock);
  kt_schedule();
  __atomic_sub_fetch(&bkl_sleepers, 1, __ATOMIC_SEQ_CST);
  irq_restore(fl);
}

/* O mundo empresta a sua CPU as threads por ate 'ms', ou ate *addr mudar. */
static void world_lend(volatile int* addr, int val, uint32_t ms) {
  struct task* w = &world;
  int depth = bkl_drop(w);
  uint64_t fl = irq_save();
  uint32_t d = (uint32_t)irq_ticks() + (ms ? ms : 1u);
  st_lend++;
  spin_lock(&fx_lock);
  if (*addr == val) {
    block_on(w, addr, d ? d : 1);
    spin_unlock(&fx_lock);
    kt_schedule();
  } else {
    spin_unlock(&fx_lock);
  }
  irq_restore(fl);
  bkl_take(w, depth ? depth : 1);
}

/* Laco do WIN sem nada a fazer: empresta a CPU0 (e o BKL) as threads por
 * ate ms, em vez de girar. Fora do mundo nao faz nada. */
static volatile int world_idle_word;
void kt_world_idle(uint32_t ms) {
  if (!kt_up || cur() != &world || bkl_owner != &world) return;
  world_lend(&world_idle_word, 0, ms);
}

static int world_wait(volatile int* addr, int val, int timeout_ms) {
  uint32_t t0 = (uint32_t)irq_ticks();
  for (;;) {
    uint32_t lim = LEND_MS;
    uint32_t el;
    if (*addr != val) return 0;
    el = (uint32_t)irq_ticks() - t0;
    if (timeout_ms >= 0) {
      if (el >= (uint32_t)timeout_ms) return E_TIMEDOUT;
      if ((uint32_t)timeout_ms - el < lim) lim = (uint32_t)timeout_ms - el;
    }
    con_break_poll();                 /* Ctrl+C no console: pode nao voltar */
    /* dentro do WIN: o desktop roda; a tarefa volta quando *addr mudar ou
     * o prazo vencer, sem contar como "calculando" (laco do WIN faz hlt) */
    if (gui_coop_wait(addr, val, lim)) continue;
    /* Tela de texto esperando (WSL exec esperando a VM, por exemplo): as
     * outras telas andam - as de fundo se esta e a visivel (o servico WSL na
     * Ctrl+F10, o DISKSYNC), a visivel se esta e de fundo. Antes o futex
     * prendia o mundo inteiro e o servico nunca subia. Nao no meio de uma
     * operacao de disco (dona da trava do FS), como no sched_yield. */
    if (!fs_held_by_me()) {
      if (vt_bg()) vt_yield_poll();   /* ao fundo: a tela visivel anda */
      else vt_idle();
    }
    if (*addr != val) return 0;
    world_lend(addr, val, lim);
  }
}

int kt_futex_wait(volatile int* addr, int val, int timeout_ms) {
  struct task* t = cur();
  if (!addr) return E_INVAL;
  if (t->kind == K_THREAD) return thread_wait(t, addr, val, timeout_ms);
  return world_wait(addr, val, timeout_ms);
}

int kt_futex_wake(volatile int* addr, int n) {
  uint64_t fl = irq_save();
  struct task** pp;
  int cnt = 0;
  spin_lock(&fx_lock);
  pp = &fx_head;
  while (*pp && (n < 0 || cnt < n)) {
    struct task* t = *pp;
    if (t->fw_addr == addr) {
      *pp = t->fw_next;
      t->fw_next = 0;
      t->fw_listed = 0;
      t->fw_woken = 1;
      kt_wake(t);
      cnt++;
      continue;
    }
    pp = &t->fw_next;
  }
  spin_unlock(&fx_lock);
  irq_restore(fl);
  return cnt;
}

static void fx_timeouts(void) {
  uint32_t now = (uint32_t)irq_ticks();
  struct task** pp;
  if (!fx_head) return;
  spin_lock(&fx_lock);
  pp = &fx_head;
  while (*pp) {
    struct task* t = *pp;
    if (t->fw_deadline && (int32_t)(now - t->fw_deadline) >= 0) {
      *pp = t->fw_next;
      t->fw_next = 0;
      t->fw_listed = 0;
      t->fw_woken = 0;
      st_fx_timeout++;
      kt_wake(t);
      continue;
    }
    pp = &t->fw_next;
  }
  spin_unlock(&fx_lock);
}

/* ---------- vida das threads ---------- */
static void kt_die(struct task* t) {
  if (bkl_owner == t) bkl_drop(t);
  (void)irq_save();
  if (t->fw_listed) {
    spin_lock(&fx_lock);
    if (t->fw_listed) fx_unlink(t);
    spin_unlock(&fx_lock);
  }
  {
    struct cpu* c = this_cpu();
    spin_lock(&c->lock);
    t->state = T_DEAD;
    spin_unlock(&c->lock);
  }
  t->exited = 1;
  kt_futex_wake(&t->exited, -1);
  kt_schedule();
  for (;;) __asm__ volatile ("cli; hlt");
}

static void kt_entry(void) {
  struct cpu* c = this_cpu();
  struct task* t = c->curr;
  gs_load(c, t);
  finish_switch(c);
  fpu_restore(t);
  fs_write(t->fs);
  __asm__ volatile ("sti");
  if (!t->killed) t->exit_code = t->fn(t->arg);
  kt_die(t);
}

static void kt_idle_loop(void) __attribute__((noreturn));
static void kt_idle_loop(void) {
  for (;;) {
    struct cpu* c;
    __asm__ volatile ("cli");
    c = this_cpu();
    if (stop_here()) {
      for (;;) __asm__ volatile ("cli; hlt");
    }
    if (c->nr > 0) {
      kt_schedule();
      continue;
    }
    c->need_resched = 0;
    if (kt_pull(c)) continue;
    __asm__ volatile ("sti; hlt" ::: "memory");
  }
}

static void kt_idle_entry(void) {
  {
    struct cpu* c = this_cpu();
    gs_load(c, c->idle);   /* a tarefa anterior pode ser thread Windows */
    finish_switch(c);
  }
  kt_idle_loop();
}

static void arm_stack(struct task* t, uint8_t* stack, uint32_t size, void (*entry)(void)) {
  uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
  uint64_t* sp = (uint64_t*)top;
  int i;
  /* 6 registros do ctx_switch, o endereco de retorno e alinhamento:
   * na entrada, rsp % 16 == 8 como depois de um call. */
  sp -= 8;
  for (i = 0; i < 8; i++) sp[i] = 0;
  sp[6] = (uint64_t)(uintptr_t)entry;
  t->rsp = (uint64_t)(uintptr_t)sp;
}

static void free_task(struct task* t) {
  while (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) cpu_pause();
  if (t->stack) ufree(t->stack);
  t->stack = 0;
  t->tid = 0;
  t->state = T_FREE;
}

static void reap(void) {
  int i;
  for (i = 0; i < KT_MAX; i++) {
    struct task* t = &kts[i];
    if (t->state == T_DEAD && t->detached && !t->on_cpu) free_task(t);
  }
}

static struct task* find_tid(int tid) {
  int i;
  if (tid <= 0) return 0;
  for (i = 0; i < KT_MAX; i++)
    if (kts[i].state != T_FREE && kts[i].tid == tid) return &kts[i];
  return 0;
}

/* 1 se a thread tid existe e nao terminou (trava do FS: dona morta). */
int kt_thread_alive(int tid) {
  struct task* t = find_tid(tid);
  return t && t->state != T_DEAD;
}

static struct cpu* pick_cpu(uint32_t mask) {
  struct cpu* best = 0;
  int bl = 0x7FFFFFFF, i;
  /* CPU0 por ultimo: e a do mundo (WIN, teclado, rede). */
  for (i = 1; i <= KT_CPU_MAX; i++) {
    int k = i % KT_CPU_MAX;
    struct cpu* d = &cpus[k];
    int l;
    if (!d->online || !((mask >> k) & 1u) || parked(d)) continue;
    l = load_of(d);
    if (l < bl) {
      best = d;
      bl = l;
    }
  }
  if (!best && (mask & 1u)) best = &cpus[0];
  if (!best) {                       /* a afinidade so tem CPUs estacionadas */
    for (i = 1; i < KT_CPU_MAX; i++)
      if (cpus[i].online && ((mask >> i) & 1u)) return &cpus[i];
  }
  return best ? best : &cpus[0];
}

int kt_thread_create(int (*fn)(void*), void* arg, uint32_t stack_size, uint32_t mask) {
  struct task* t = 0;
  struct cpu* dest;
  uint64_t fl;
  int i;
  if (!fn) return E_INVAL;
  reap();
  for (i = 0; i < KT_MAX; i++)
    if (kts[i].state == T_FREE) {
      t = &kts[i];
      break;
    }
  if (!t) return E_AGAIN;
  mask = mask ? (mask & online_mask) : online_mask;
  if (!mask) return E_INVAL;
  if (!stack_size) stack_size = STACK_DEF;
  if (stack_size < STACK_MIN) stack_size = STACK_MIN;
  stack_size = (stack_size + 15u) & ~15u;
  if (!t->xs) {
    uint8_t* raw = (uint8_t*)kmalloc(xs_size + 64u);
    if (!raw) return E_NOMEM;
    t->xs_raw = raw;
    t->xs = (uint8_t*)(((uintptr_t)raw + 63u) & ~(uintptr_t)63u);
  }
  t->pid = mod_current_pid();
  t->stack = (uint8_t*)umalloc_owned(stack_size, t->pid);
  if (!t->stack) return E_NOMEM;
  fpu_init_area(t->xs);
  t->fs = fs_read();
  t->teb = 0;
  t->drive = mod_exe_drive();
  t->kind = K_THREAD;
  t->fn = fn;
  t->arg = arg;
  t->mask = mask;
  t->tid = next_tid++;
  if (next_tid <= 0) next_tid = 1;
  t->on_cpu = 0;
  t->migrate_to = -1;
  t->bkl_depth = 0;
  t->killed = 0;
  t->exited = 0;
  t->exit_code = 0;
  t->detached = 0;
  t->fw_listed = 0;
  t->slice_ms = 0;
  arm_stack(t, t->stack, stack_size, kt_entry);
  dest = pick_cpu(mask);
  fl = irq_save();
  t->state = T_READY;
  spin_lock(&dest->lock);
  rq_push(dest, t);
  spin_unlock(&dest->lock);
  if (dest->curr == dest->idle) resched_cpu(dest);
  irq_restore(fl);
  return t->tid;
}

int kt_thread_join(int tid, int* code) {
  struct task* t = find_tid(tid);
  if (!t || t->kind != K_THREAD || t->detached) return E_SRCH;
  if (t == cur()) return E_DEADLK;
  while (!t->exited) kt_futex_wait(&t->exited, 0, -1);
  if (code) *code = t->exit_code;
  free_task(t);
  return 0;
}

int kt_thread_detach(int tid) {
  struct task* t = find_tid(tid);
  if (!t || t->kind != K_THREAD) return E_SRCH;
  t->detached = 1;
  reap();
  return 0;
}

void kt_thread_exit(int code) {
  struct task* t = cur();
  if (t->kind == K_THREAD) {
    t->exit_code = code;
    kt_die(t);
  }
  mod_exit(code);
  for (;;) __asm__ volatile ("hlt");
}

int kt_thread_self(void) {
  struct task* t = cur();
  return t->kind == K_THREAD ? t->tid : 0;
}

int kt_thread_affinity(int tid, uint32_t mask) {
  struct task* t = tid ? find_tid(tid) : cur();
  if (!t || t->kind != K_THREAD) return E_SRCH;
  mask &= online_mask;
  if (!mask) return E_INVAL;
  t->mask = mask;
  if (t->state == T_RUNNING && !((mask >> t->cpu) & 1u)) resched_cpu(&cpus[t->cpu]);
  if (t == cur() && !((mask >> this_cpu()->index) & 1u)) kt_thread_yield();
  return 0;
}

static void yield_raw(struct task* t) {
  kt_schedule();
  if (t->killed && t->bkl_depth == 0) kt_die(t);
}

void kt_yield_in_thread(void) {
  struct task* t = cur();
  int d;
  if (t->kind != K_THREAD) return;
  d = bkl_drop(t);
  yield_raw(t);
  bkl_take(t, d);
}

void kt_thread_yield(void) {
  if (kt_in_thread()) kt_yield_in_thread();
  else sched_yield();
}

int kt_thread_tls(uint64_t fs) {
  struct task* t = cur();
  t->fs = fs;
  fs_write(fs);
  return 0;
}

/* TEB do Windows da tarefa corrente (WINE.DLL): vira a base do GS dela.
 * 0 desliga (o GS volta para a struct cpu). */
void kt_win_teb_set(uint64_t teb) {
  uint64_t fl = irq_save();
  struct cpu* c = this_cpu();
  struct task* t = cur();
  t->teb = teb;
  gs_load(c, t);
  irq_restore(fl);
}

uint64_t kt_win_teb_get(void) { return cur()->teb; }

void kt_thread_fault_exit(void) {
  struct task* t = cur();
  t->exit_code = -1;
  kt_die(t);
}

/* Fim do programa: as threads dele morrem antes do heap ser liberado. */
/* Slots livres nao guardam a area de FPU: um programa que usou threads e
 * terminou nao deixa nada no heap do kernel. Chamado com o BKL (fim de
 * programa), como o k_thread_create que aloca. */
static void xs_sweep(void) {
  int i;
  for (i = 0; i < KT_MAX; i++) {
    struct task* t = &kts[i];
    if (t->state != T_FREE || !t->xs_raw || t->on_cpu) continue;
    kfree(t->xs_raw);
    t->xs_raw = 0;
    t->xs = 0;
  }
}

void kt_release_pid(uint32_t pid) {
  int i, n = 0;
  uint32_t t0;
  for (i = 0; i < KT_MAX; i++) {
    struct task* t = &kts[i];
    uint64_t fl;
    if (t->state == T_FREE || t->kind != K_THREAD || t->pid != pid) continue;
    n++;
    t->killed = 1;
    fl = irq_save();
    spin_lock(&fx_lock);
    if (t->fw_listed) {
      fx_unlink(t);
      t->fw_woken = 0;
    }
    spin_unlock(&fx_lock);
    kt_wake(t);
    if (t->state == T_RUNNING) resched_cpu(&cpus[t->cpu]);
    irq_restore(fl);
  }
  if (!n) {
    xs_sweep();
    return;
  }
  t0 = (uint32_t)irq_ticks();
  for (i = 0; i < KT_MAX; i++) {
    struct task* t = &kts[i];
    if (t->state == T_FREE || t->kind != K_THREAD || t->pid != pid) continue;
    while (!t->exited || t->on_cpu) {
      if ((uint32_t)irq_ticks() - t0 > 2000u) break;
      if (cur() == &world) world_lend(&t->exited, 0, 2);
      else cpu_pause();
    }
    if (t->exited && !t->on_cpu) {
      free_task(t);
    } else {
      serial_puts("[kt] thread nao morreu: tid=");
      put_dec((uint32_t)t->tid);
      serial_puts("\n");
      t->stack = 0;   /* o ufree_owner leva a pilha; o slot fica perdido */
    }
  }
  serial_puts("[kt] pid ");
  put_dec(pid);
  serial_puts(": threads encerradas=");
  put_dec((uint32_t)n);
  serial_puts("\n");
  xs_sweep();
}

/* ---------- ticks e IPIs ---------- */
static void preempt_check(struct cpu* c) {
  struct task* t = c->curr;
  if (t->kind != K_THREAD || t->bkl_depth) return;
  if (t->killed) kt_die(t);
  if (c->need_resched || (t->slice_ms >= SLICE_MS && c->nr > 0)) {
    yield_raw(t);
  } else if (t->slice_ms >= SLICE_MS) {
    t->slice_ms = 0;
  }
}

static void tick_common(struct cpu* c, int ms) {
  struct task* t = c->curr;
  if (stop_here()) {
    for (;;) __asm__ volatile ("cli; hlt");
  }
  if (t->kind == K_THREAD) t->slice_ms += (uint32_t)ms;
  if (parked(c)) {                   /* limite termico: esvazia esta CPU */
    int i;
    for (i = 0; i < KT_CPU_MAX && c->nr > 0; i++)
      if (cpus[i].online && !parked(&cpus[i]) && &cpus[i] != c && move_one(c, &cpus[i]))
        resched_cpu(&cpus[i]);
    if (t->kind == K_THREAD) c->need_resched = 1;
  }
  if (c->nr > 0) kt_push(c);
  c->bal_ms += (uint32_t)ms;
  if (c->bal_ms >= BAL_MS) {
    c->bal_ms = 0;
    if (c->curr == c->idle || c->nr == 0) kt_pull(c);
  }
  preempt_check(c);
}

void kt_tick_lapic(int ms) {
  struct cpu* c = this_cpu();
  if (c->curr == c->idle) c->idle_ms += (uint32_t)ms;
  else c->busy_ms += (uint32_t)ms;
  tick_common(c, ms);
}

void kt_tick_pit(void) {
  struct cpu* c;
  if (!kt_up) return;
  if (bkl_owner == &world) st_world_bkl_ms++;
  c = this_cpu();
  if (c->curr == c->idle) c->idle_ms++;
  else c->busy_ms++;
  fx_timeouts();
  tick_common(c, 1);
}

void kt_ipi_resched(void) {
  struct cpu* c = this_cpu();
  if (stop_here()) {
    for (;;) __asm__ volatile ("cli; hlt");
  }
  if (c->curr == c->idle) return;   /* o laco ocioso olha a fila */
  preempt_check(c);
}

void kt_ipi_tlb(void) {
  uint64_t cr3;
  __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
  this_cpu()->tlb_ack = tlb_gen;
}

void kt_tlb_shootdown(void) {
  uint32_t g;
  int i;
  struct cpu* me;
  if (n_online < 2) return;
  me = this_cpu();
  g = __atomic_add_fetch(&tlb_gen, 1, __ATOMIC_SEQ_CST);
  smp_ipi_others(0xF4);
  for (i = 0; i < KT_CPU_MAX; i++) {
    struct cpu* d = &cpus[i];
    uint32_t spins = 0;
    if (!d->online || d == me) continue;
    while ((int32_t)(d->tlb_ack - g) < 0 && spins++ < 50000000u) {
      kt_tlb_poll(); /* duas limpezas ao mesmo tempo: atende a da outra */
      cpu_pause();
    }
  }
}

void kt_stop_others(void) {
  if (stopping) return;
  stop_cpu = this_cpu()->index;
  stopping = 1;
  if (n_online > 1) smp_nmi_others();
}

/* ---------- smp run: o mundo muda de CPU ---------- */
void kt_world_migrate(int cpu) {
  struct task* w = &world;
  uint64_t fl;
  if (cur() != w || cpu < 0 || cpu >= KT_CPU_MAX || !cpus[cpu].online) return;
  if (this_cpu()->index == cpu) return;
  fl = irq_save();
  w->mask = 1u << cpu;
  w->migrate_to = cpu;
  kt_schedule();
  irq_restore(fl);
}

/* ---------- inicializacao ---------- */
static void gs_write(uint64_t v) {
  if (fsgs) {
    __asm__ volatile ("wrgsbase %0" :: "r"(v) : "memory");
    return;
  }
  __asm__ volatile ("wrmsr" :: "c"(0xC0000101), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static void set_gs(struct cpu* c) {
  uint64_t v = (uint64_t)(uintptr_t)c;
  c->gs_now = v;
  __asm__ volatile ("wrmsr" :: "c"(0xC0000101), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

void kt_early_init(void) {
  struct cpu* c0 = &cpus[0];
  struct task* i0 = &idle_t[0];
  int i;
  xs_probe();
  for (i = 0; i < KT_CPU_MAX; i++) {
    cpus[i].self = &cpus[i];
    cpus[i].index = i;
    cpus[i].idle = &idle_t[i];
    idle_t[i].kind = K_IDLE;
    idle_t[i].migrate_to = -1;
    idle_t[i].mask = 1u << i;
    idle_t[i].cpu = i;
  }
  world.kind = K_WORLD;
  world.state = T_RUNNING;
  world.on_cpu = 1;
  world.cpu = 0;
  world.mask = 1;
  world.migrate_to = -1;
  world.xs = world_xs;
  world.bkl_depth = 1;
  bkl_owner = &world;
  bkl_lock = 1;

  i0->state = T_READY;
  arm_stack(i0, idle0_stack, sizeof(idle0_stack), kt_idle_entry);

  c0->online = 1;
  c0->curr = &world;
  set_gs(c0);
  kt_up = 1;
  serial_puts("[kt] escalonador: xsave=");
  put_dec((uint32_t)xs_mode);
  serial_puts(" area=");
  put_dec(xs_size);
  serial_puts("\n");
}

void kt_set_online(int idx) {
  if (idx <= 0 || idx >= KT_CPU_MAX || cpus[idx].online) return;
  cpus[idx].online = 1;
  __atomic_or_fetch(&online_mask, 1u << idx, __ATOMIC_SEQ_CST);
  __atomic_add_fetch(&n_online, 1, __ATOMIC_SEQ_CST);
}

/* O AP entra aqui com a pilha de boot (que vira a pilha da tarefa ociosa). */
void kt_ap_enter(int idx) {
  struct cpu* c = &cpus[idx];
  struct task* it = &idle_t[idx];
  set_gs(c);
  it->state = T_RUNNING;
  it->on_cpu = 1;
  c->curr = it;
  kt_set_online(idx);
  smp_timer_start();
  kt_idle_loop();
}
