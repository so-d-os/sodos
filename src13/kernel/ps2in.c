/* ps2in.c - filas de entrada PS/2 cheias pela IRQ1/IRQ12 (ver ps2in.h).
 *
 * Duas filas (teclado e mouse) para que o shell possa ler teclas sem
 * tropecar em bytes do mouse, e para que o WIN de prioridade ao teclado.
 * Produtores: a IRQ (CPU0, IF=0) e ps2in_pump (qualquer CPU, com cli),
 * sempre com ps2_lk; isso tambem serializa o par inb(0x64)/inb(0x60) entre
 * CPUs. Consumidor: um so (o mundo), sem lock. */
#include "ps2in.h"
#include "pic.h"
#include "reboot.h"
#include "serial.h"
#include "irq.h"
#include "xhci.h"
#include "hang.h"

#define PQ 256   /* potencia de 2 */

struct pq {
  volatile uint32_t head;   /* escrito pelo produtor */
  volatile uint32_t tail;   /* escrito pelo consumidor */
  uint8_t b[PQ];
  uint32_t t[PQ];
};

static struct pq kq, aq;
static volatile int ps2_lk;
static int ps2_irq_on;      /* ps2in_init ligou as IRQs */
static int ps2_raw_depth;
static volatile uint32_t ps2_irqs, ps2_drops;

static inline uint8_t p_inb(uint16_t port) {
  uint8_t v;
  __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}
static inline void p_outb(uint16_t port, uint8_t v) {
  __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint64_t irq_save(void) {
  uint64_t f;
  __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) : : "memory");
  return f;
}
static inline void irq_restore(uint64_t f) {
  if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
}

static void lk_take(void) {
  while (__atomic_exchange_n(&ps2_lk, 1, __ATOMIC_ACQUIRE))
    while (__atomic_load_n(&ps2_lk, __ATOMIC_RELAXED))
      __builtin_ia32_pause();
}
static void lk_drop(void) {
  __atomic_store_n(&ps2_lk, 0, __ATOMIC_RELEASE);
}

static void q_push(struct pq* q, uint8_t b, uint32_t t) {
  uint32_t h = q->head;
  if (h - __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE) >= PQ) {
    ps2_drops++;
    return;
  }
  q->b[h & (PQ - 1)] = b;
  q->t[h & (PQ - 1)] = t;
  __atomic_store_n(&q->head, h + 1, __ATOMIC_RELEASE);
}

static int q_pop(struct pq* q, uint8_t* b, uint32_t* t) {
  uint32_t tl = q->tail;
  if (__atomic_load_n(&q->head, __ATOMIC_ACQUIRE) == tl) return 0;
  *b = q->b[tl & (PQ - 1)];
  if (t) *t = q->t[tl & (PQ - 1)];
  __atomic_store_n(&q->tail, tl + 1, __ATOMIC_RELEASE);
  return 1;
}

static int q_has(struct pq* q) {
  return __atomic_load_n(&q->head, __ATOMIC_ACQUIRE) != q->tail;
}

/* Byte de teclado para a fila, com ps2_lk: o kbd_watch ve antes e decide.
 * O prefixo E0 fica retido ate o byte seguinte, para que o Del engolido do
 * Ctrl+Alt+Del (E0 53) nao deixe um E0 sozinho na fila (ele grudaria na
 * proxima tecla). */
static int kq_e0;
static void kq_feed(uint8_t b, uint32_t now) {
  int r = kbd_watch(b);
  if (r == KBW_E0) {
    if (kq_e0) q_push(&kq, 0xE0, now);
    kq_e0 = 1;
    return;
  }
  if (r == KBW_SWALLOW) {
    kq_e0 = 0;
    return;
  }
  if (kq_e0) {
    kq_e0 = 0;
    q_push(&kq, 0xE0, now);
  }
  q_push(&kq, b, now);
}

/* Com ps2_lk e IF=0. Limite de bytes por chamada: sem controlador a porta
 * 0x64 pode devolver 0xFF para sempre. */
static void drain_locked(void) {
  uint32_t now = 0;
  int n;
  for (n = 0; n < 32; n++) {
    uint8_t st = p_inb(0x64);
    uint8_t b;
    if (st == 0xFF || !(st & 1)) break;
    b = p_inb(0x60);
    if (!now) {
      now = (uint32_t)irq_ticks();
      if (!now) now = 1;
    }
    if (st & 0x20) {
      q_push(&aq, b, now);
    } else {
      kq_feed(b, now);
    }
  }
}

/* RIT (video/visual.c): chamada na IRQ1/IRQ12 depois de encher a fila. */
void (*volatile ps2in_kick)(void);

void ps2in_isr(void) {
  void (*k)(void);
  ps2_irqs++;
  if (ps2_raw_depth) return;   /* alguem conversa direto com o 8042 */
  lk_take();
  drain_locked();
  lk_drop();
  k = ps2in_kick;
  if (k) k();
}

/* Byte de teclado que nao veio do 8042 (teclado USB, xhci.c): mesma fila e o
 * mesmo kbd_watch (Ctrl+Alt+Del etc.) que um byte da IRQ1. */
void ps2in_inject(uint8_t b) {
  uint64_t f = irq_save();
  uint32_t now;
  lk_take();
  now = (uint32_t)irq_ticks();
  if (!now) now = 1;
  kq_feed(b, now);
  lk_drop();
  irq_restore(f);
}

/* ---------- guardiao ----------
 * O teclado (o USB sempre, o PS/2 fora da IRQ1) so e lido quando o mundo
 * chama ps2in_pump. Com o mundo travado nenhuma tecla chegava, nem
 * Ctrl+Alt+Del. Uma thread do kernel (fora da CPU0) olha a hora do ultimo
 * pump; parado ha 1,5 s, ela mesma bombeia o teclado USB (Ctrl+Alt+Del
 * reinicia pelo kbd_watch). Ctrl+F1 com o mundo parado: solta a trava do
 * sistema de arquivos. Nao reinicia mais sozinho: com o Claude (WINE)
 * pesado o mundo podia so estar lento e o segundo Ctrl+F1 reiniciava o PC.
 * Para reiniciar de verdade: Ctrl+Alt+Del. */
static volatile uint64_t pump_ms;          /* ultimo ps2in_pump do mundo */
static volatile uint64_t emerg_ms;         /* ultimo Ctrl+F1 */
void fs_force_release(void);               /* kexports.c */
int kt_thread_create(int (*fn)(void*), void* arg, uint32_t stack_size, uint32_t mask);
int kt_futex_wait(volatile int* addr, int val, int timeout_ms);
int kt_online_count(void);
/* irq_ticks que anda mesmo com a CPU0 sem IRQ0 (kthread.c): com o mundo
 * preso de interrupcoes desligadas o irq_ticks para e o guardiao nunca via
 * o tempo passar. */
uint64_t kt_ms_robust(void);

void kbd_emergency_note(void) {
  uint64_t t = kt_ms_robust();
  emerg_ms = t ? t : 1;
}

static uint64_t kq_wait_ms(uint64_t now);
/* ms desde a ultima vez que o mundo leu o teclado (ps2in_pump fora de
 * interrupcao), ou ha quanto tempo a tecla mais velha espera na fila: o
 * maior dos dois. */
uint32_t ps2in_world_idle_ms(void) {
  uint64_t now = kt_ms_robust(), p = pump_ms, a = 0, k = kq_wait_ms(now);
  if (p && now > p) a = now - p;
  if (k > a) a = k;
  return (uint32_t)(a > 0xFFFFFFFFu ? 0xFFFFFFFFu : a);
}

/* Le o 8042 de fora da CPU0 (o guardiao, com o mundo parado). So se a trava
 * estiver livre: a CPU0 pode estar parada justamente com ela. */
static void ps2_try_drain(void) {
  uint64_t f;
  if (!ps2_irq_on || ps2_raw_depth) return;
  f = irq_save();
  if (!__atomic_exchange_n(&ps2_lk, 1, __ATOMIC_ACQUIRE)) {
    drain_locked();
    lk_drop();
  }
  irq_restore(f);
}

/* Tecla parada na fila (ninguem lendo) ha quanto tempo? 0 = fila vazia. */
static uint64_t kq_wait_ms(uint64_t now) {
  uint32_t h = kq.head, t = kq.tail;
  uint32_t at;
  if (h == t) return 0;
  at = kq.t[t % PQ];
  return now > at ? now - at : 0;
}

static int guard_main(void* arg) {
  static volatile int z;
  uint64_t first = 0;   /* primeiro Ctrl+F1 tratado com o mundo parado */
  uint64_t seen = 0;    /* ultimo Ctrl+F1 ja olhado */
  int noted = 0;
  (void)arg;
  for (;;) {
    uint64_t now;
    int stuck;
    kt_futex_wait(&z, 0, 50);
    /* o teclado USB anda mesmo se o mundo nao chama o pump: Ctrl+Alt+Del
     * chega ao kbd_watch; o pump tem trava propria e respeita o driver
     * ocupado. O PS/2 tambem: com a CPU0 presa de interrupcoes desligadas a
     * IRQ1 nao entra, e so daqui o Ctrl+Alt+Del seria visto. */
    now = kt_ms_robust();
    if (now - pump_ms > 1000u) {   /* o mundo nao esta lendo: nao disputa com ele */
      xhci_kbd_pump();
      ps2_try_drain();
    }
    stuck = kq_wait_ms(now) > 1500u;
    if (!stuck) {
      noted = 0;
      first = 0;
      seen = emerg_ms;
      continue;
    }
    if (!noted) {
      noted = 1;
      serial_puts("[guarda] teclas sem leitor ha 1,5 s: o mundo parou (Ctrl+F1 destrava; Ctrl+Alt+Del reinicia)\n");
    }
    if (emerg_ms && emerg_ms != seen) {
      seen = emerg_ms;
      if (!first) {
        first = now;
        serial_puts("[guarda] Ctrl+F1 com o mundo parado: soltando a trava do disco\n");
        fs_force_release();
      } else if (now - first > 3000u) {
        serial_puts("[guarda] Ctrl+F1 de novo e o mundo segue parado (Ctrl+Alt+Del reinicia)\n");
      }
    }
  }
  return 0;
}

void guard_start(void) {
  static int started;
  int tid;
  if (started || kt_online_count() < 2) return;
  started = 1;
  pump_ms = kt_ms_robust();
  tid = kt_thread_create(guard_main, 0, 16384, ~1u); /* longe da CPU0 (o mundo) */
  serial_puts(tid > 0 ? "[guarda] ligado\n" : "[guarda] nao foi possivel criar a thread\n");
  hang_start();   /* Ctrl+Alt+Del: tela 1, HANG.LOG (hang.c) */
}

void ps2in_pump(void) {
  uint64_t f;
  /* So conta como "o mundo leu" fora de interrupcao: o gancho do IRQ0 do
   * WIN (gui_preempt_tick -> ps2in_pending) bombeia a cada tique, e com o
   * WIN preso o mundo parecia vivo (o Ctrl+Alt+Del nao gravava o HANG.LOG). */
  __asm__ volatile ("pushfq; popq %0" : "=r"(f));
  if (f & 0x200u) {
    uint64_t t = kt_ms_robust();
    pump_ms = t ? t : 1;
  }
  /* Teclado USB primeiro, fora do cli: ele pode esperar comandos do xHCI. */
  xhci_kbd_pump();
  if (ps2_raw_depth) return;
  /* Com IRQ1/IRQ12 ligadas a interrupcao ja enche as filas: ler a porta 0x64
   * aqui e so rede de seguranca (IRQ perdida), no maximo a cada 20 ms. Antes
   * cada "tem tecla?" do laco do WIN lia o 8042; numa VM cada inb e uma saida
   * para o QEMU, e a CPU0 passava ~70% do tempo nisso segurando o BKL. */
  if (ps2_irq_on) {
    static uint32_t last;
    uint32_t t = (uint32_t)irq_ticks();
    if (t - last < 20) return;
    last = t;
  }
  f = irq_save();
  lk_take();
  drain_locked();
  lk_drop();
  irq_restore(f);
}

/* ---------- teclas por comando (TECLA, API) ----------
 * Bytes set 1, como os do 8042, que entram na fila do teclado um por vez e
 * so quando quem le pede (ps2in_kbd com a fila vazia). O kbd_watch (Shift,
 * Ctrl, AltGr, Ctrl+Alt+Del, Ctrl+F1...) ve cada byte na hora em que ele e
 * lido, como numa tecla de verdade: com o grupo todo de uma vez o Shift ja
 * estaria solto quando o leitor pedisse o caractere. Daqui em diante e o
 * mesmo caminho de uma tecla apertada (PrtSc, Alt+Tab, Ctrl+F12...). */
#define SQ 4096   /* potencia de 2 */
static uint8_t sq[SQ];
static volatile uint32_t sq_h, sq_t;

int ps2in_type(const uint8_t* b, int n) {
  uint64_t f;
  int i;
  if (n <= 0) return 0;
  f = irq_save();
  lk_take();
  if ((uint32_t)n > SQ - (sq_h - sq_t)) {
    lk_drop();
    irq_restore(f);
    return -1;
  }
  for (i = 0; i < n; i++) sq[(sq_h + (uint32_t)i) & (SQ - 1)] = b[i];
  __atomic_store_n(&sq_h, sq_h + (uint32_t)n, __ATOMIC_RELEASE);
  lk_drop();
  irq_restore(f);
  return 0;
}

int ps2in_type_left(void) {
  return (int)(sq_h - sq_t);
}

/* Um byte da fila de comando para a do teclado, se ela estiver vazia. */
static void syn_feed(void) {
  uint64_t f;
  if (__atomic_load_n(&sq_h, __ATOMIC_ACQUIRE) == sq_t) return;
  f = irq_save();
  lk_take();
  if (sq_h != sq_t && (!q_has(&kq) || kq_e0)) {
    uint8_t b = sq[sq_t & (SQ - 1)];
    uint32_t now = (uint32_t)irq_ticks();
    sq_t++;
    if (!now) now = 1;
    kq_feed(b, now);
  }
  lk_drop();
  irq_restore(f);
}

static int syn_has(void) {
  return __atomic_load_n(&sq_h, __ATOMIC_ACQUIRE) != sq_t;
}

int ps2in_kbd(uint8_t* b, uint32_t* t) {
  if (q_pop(&kq, b, t)) return 1;
  ps2in_pump();
  if (q_pop(&kq, b, t)) return 1;
  syn_feed();
  return q_pop(&kq, b, t);
}

/* So as teclas de verdade: quem so procura o Ctrl+C no meio de um comando
 * (shell_break, con_break_poll) nao pode puxar as do TECLA. Ele as guardava
 * para "digitar adiante" e no WIN esse buffer e jogado fora no fim do
 * comando: um "TECLA ver{ENTER}dir{ENTER}" perdia o dir. */
int ps2in_kbd_hw(uint8_t* b, uint32_t* t) {
  if (q_pop(&kq, b, t)) return 1;
  ps2in_pump();
  return q_pop(&kq, b, t);
}

int ps2in_aux(uint8_t* b, uint32_t* t) {
  if (q_pop(&aq, b, t)) return 1;
  ps2in_pump();
  return q_pop(&aq, b, t);
}

/* Para a RIT: so a fila, sem ps2in_pump (que pode esperar o xHCI). */
int ps2in_aux_pop(uint8_t* b, uint32_t* t) {
  return q_pop(&aq, b, t);
}
int ps2in_kbd_has(void) {
  return q_has(&kq) || syn_has();
}

int ps2in_kbd_pending(void) {
  if (q_has(&kq) || syn_has()) return 1;
  ps2in_pump();
  return q_has(&kq);
}

int ps2in_pending(void) {
  if (q_has(&kq) || q_has(&aq) || syn_has()) return 1;
  ps2in_pump();
  return q_has(&kq) || q_has(&aq);
}

void ps2in_flush(void) {
  ps2in_pump();
  __atomic_store_n(&kq.tail, __atomic_load_n(&kq.head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
  __atomic_store_n(&aq.tail, __atomic_load_n(&aq.head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

static void irqs_set(int on) {
  if (on) {
    pic_unmask(2);    /* cascata: a IRQ12 vem pelo PIC escravo */
    pic_unmask(12);
    pic_unmask(1);
  } else {
    pic_mask_irq(1);
    pic_mask_irq(12);
  }
}

void ps2in_raw(int on) {
  uint64_t f = irq_save();
  if (on) {
    if (ps2_raw_depth++ == 0 && ps2_irq_on) irqs_set(0);
  } else if (ps2_raw_depth > 0) {
    if (--ps2_raw_depth == 0 && ps2_irq_on) irqs_set(1);
  }
  irq_restore(f);
}

static int wait_write(void) {
  int n;
  for (n = 0; n < 100000; n++)
    if (!(p_inb(0x64) & 2)) return 1;
  return 0;
}

static int wait_read(void) {
  int n;
  for (n = 0; n < 100000; n++)
    if (p_inb(0x64) & 1) return 1;
  return 0;
}

void ps2in_init(void) {
  uint64_t f;
  uint8_t cfg;
  if (p_inb(0x64) == 0xFF) {
    serial_puts("[ps2in] sem controlador 8042: so polling\n");
    return;
  }
  f = irq_save();
  lk_take();
  drain_locked();
  /* Byte de configuracao: bit 0 = IRQ1, bit 1 = IRQ12. O resto (traducao,
   * relogios das portas) fica como o firmware deixou. */
  if (!wait_write()) goto fail;
  p_outb(0x64, 0x20);
  if (!wait_read()) goto fail;
  cfg = p_inb(0x60);
  cfg |= 0x03;
  if (!wait_write()) goto fail;
  p_outb(0x64, 0x60);
  if (!wait_write()) goto fail;
  p_outb(0x60, cfg);
  drain_locked();
  lk_drop();
  ps2_irq_on = 1;
  if (!ps2_raw_depth) irqs_set(1);
  irq_restore(f);
  serial_puts("[ps2in] IRQ1/IRQ12 ligadas, filas de entrada ativas\n");
  return;
fail:
  lk_drop();
  irq_restore(f);
  serial_puts("[ps2in] 8042 nao respondeu: so polling\n");
}

void ps2in_stats(uint32_t* irqs, uint32_t* drops) {
  if (irqs) *irqs = ps2_irqs;
  if (drops) *drops = ps2_drops;
}
