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
      kbd_watch(b);
      q_push(&kq, b, now);
    }
  }
}

void ps2in_isr(void) {
  ps2_irqs++;
  if (ps2_raw_depth) return;   /* alguem conversa direto com o 8042 */
  lk_take();
  drain_locked();
  lk_drop();
}

/* Byte de teclado que nao veio do 8042 (teclado USB, xhci.c): mesma fila e o
 * mesmo kbd_watch (Ctrl+Alt+Del etc.) que um byte da IRQ1. */
void ps2in_inject(uint8_t b) {
  uint64_t f = irq_save();
  uint32_t now;
  lk_take();
  now = (uint32_t)irq_ticks();
  if (!now) now = 1;
  kbd_watch(b);
  q_push(&kq, b, now);
  lk_drop();
  irq_restore(f);
}

void ps2in_pump(void) {
  uint64_t f;
  /* Teclado USB primeiro, fora do cli: ele pode esperar comandos do xHCI. */
  xhci_kbd_pump();
  if (ps2_raw_depth) return;
  f = irq_save();
  lk_take();
  drain_locked();
  lk_drop();
  irq_restore(f);
}

int ps2in_kbd(uint8_t* b, uint32_t* t) {
  if (q_pop(&kq, b, t)) return 1;
  ps2in_pump();
  return q_pop(&kq, b, t);
}

int ps2in_aux(uint8_t* b, uint32_t* t) {
  if (q_pop(&aq, b, t)) return 1;
  ps2in_pump();
  return q_pop(&aq, b, t);
}

int ps2in_kbd_pending(void) {
  if (q_has(&kq)) return 1;
  ps2in_pump();
  return q_has(&kq);
}

int ps2in_pending(void) {
  if (q_has(&kq) || q_has(&aq)) return 1;
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
