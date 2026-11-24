/* irq.c - Envia EOI para IRQ 32..47 */
#include "irq.h"
#include "wine.h"
#include "pic.h"
#include "reboot.h"
#include "kthread.h"
#include "ps2in.h"
#include "serial.h"

/* B1: contador do IRQ0. volatile: quem le nao pode cachear o valor. */
static volatile uint64_t irq_tick_count;
static volatile uint64_t bsp_busy_ticks;
static volatile uint64_t bsp_idle_ticks;
static volatile int bsp_idle_flag;

uint64_t irq_ticks(void) { return irq_tick_count; }

/* ---------- relogio pelo TSC ----------
 * O irq_tick_count era so a contagem de IRQ0 (PIT a 1 kHz). Com a BSP mais
 * de 1 ms sem atender o IRQ0 (cli, handler longo, guest da VM do WSL), o PIC
 * guarda um pedido so e os ticks do meio sumiam: no notebook o k_ms andava
 * 3,5% a 8,4% devagar com carga (DEVELOP\KERNEL-K_MS-FIX).
 * Com TSC invariante e sem hypervisor, o IRQ0 passa a acertar a contagem pelo
 * TSC (calibrado contra o PIT no boot): tick perdido e recuperado no proximo
 * IRQ0 e a contagem nunca volta. Numa VM (bit de hypervisor) fica como era:
 * no QEMU/KVM do WSL2 o PIT reinjeta tick atrasado e o TSC da VM aninhada
 * varia. O comando RELOGIO mostra o estado e liga/desliga. */
#define PIT_HZ 1193182u
#define PIT_DIV 1193u          /* o net.c programa o canal 0 assim (modo 2) */

static volatile int clk_tsc;            /* 1 = o IRQ0 acerta pelo TSC */
static uint64_t clk_hz;                 /* TSC por segundo (0 = nao calibrado) */
static uint64_t clk_per_ms;             /* clk_hz / 1000 */
static uint64_t clk_base;               /* TSC do tick 0 */
static uint32_t clk_spread;             /* desvio das medidas, em ppm */
static volatile uint64_t clk_lost;      /* ticks recuperados (soma) */
static volatile uint32_t clk_stalls;    /* IRQ0 parado >= 2 ms */
static volatile uint32_t clk_max;       /* maior recuperacao de uma vez (ms) */
static volatile uint64_t clk_max_at;    /* irq_tick_count dela */
static volatile uint64_t clk_last_at;   /* irq_tick_count da ultima parada */
static volatile uint64_t clk_held;      /* ticks segurados (PIT 1000,15 Hz adiantado) */
/* Ultimas paradas >= CLK_LOG_MS: quando, quanto e o RIP interrompido pelo
 * IRQ0 que chegou depois (o fim do trecho que segurou o IRQ0: um sti/popf,
 * o retorno do guest da VM...). nm/addr2line no kernel.elf dizem quem foi. */
#define CLK_LOG_N 8
#define CLK_LOG_MS 5u
static struct irq_clock_stall clk_log_v[CLK_LOG_N];
static volatile uint32_t clk_log_n;
static volatile uint64_t clk_max_rip;
static int clk_eligible = -1;           /* TSC invariante e sem hypervisor */
static const char* clk_why = "nao iniciado";

static inline uint64_t clk_rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

static inline void clk_cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
  __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline void clk_outb(uint16_t port, uint8_t v) {
  __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t clk_inb(uint16_t port) {
  uint8_t v;
  __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

static inline uint64_t clk_irq_save(void) {
  uint64_t fl;
  __asm__ volatile ("pushfq; pop %0; cli" : "=r"(fl) : : "memory");
  return fl;
}

static inline void clk_irq_restore(uint64_t fl) {
  if (fl & 0x200u) __asm__ volatile ("sti" ::: "memory");
}

/* Contador do canal 0 (desce de PIT_DIV ate 1 e recarrega). */
static uint32_t clk_pit(void) {
  uint32_t lo, hi;
  clk_outb(0x43, 0x00);
  lo = clk_inb(0x40);
  hi = clk_inb(0x40);
  return lo | (hi << 8);
}

/* Uma medida: TSC contra ~ms milissegundos do PIT, contando as voltas do
 * canal 0 por consulta (sem interrupcao). Leituras com menos de 1 ms entre
 * si nunca perdem volta; se uma demorou mais (SMI), a medida e descartada.
 * 0 = falhou. */
static uint64_t clk_measure(uint32_t ms) {
  uint64_t fl, t0, t1, prev_t, gap = 0, hz;
  uint32_t c0, c1, prev, wraps = 0, want = ms, n = 0;
  fl = clk_irq_save();
  (void)clk_pit();   /* a primeira leitura costuma demorar mais */
  /* O TSC antes do latch (outb 0x43): fica junto do instante da contagem;
   * depois dele viriam os dois inb, que variam de uma leitura para outra. */
  t0 = clk_rdtsc();
  c0 = clk_pit();
  prev = c0;
  prev_t = t0;
  for (;;) {
    uint64_t t = clk_rdtsc();
    uint32_t c = clk_pit();
    if (t - prev_t > gap) gap = t - prev_t;
    prev_t = t;
    if (c > prev) wraps++;
    prev = c;
    if (wraps >= want) {
      c1 = c;
      t1 = t;
      break;
    }
    if (++n > 50000000u) {  /* PIT parado: desiste */
      clk_irq_restore(fl);
      return 0;
    }
  }
  clk_irq_restore(fl);
  {
    uint64_t counts = (uint64_t)wraps * PIT_DIV + c0 - c1;
    if (c0 < 1 || c0 > PIT_DIV || c1 < 1 || c1 > PIT_DIV || counts < PIT_DIV) return 0;
    hz = (t1 - t0) * (uint64_t)PIT_HZ / counts;
  }
  /* 0,8 ms entre duas leituras: pode ter perdido uma volta. */
  if (!hz || gap * 1250u > hz) return 0;
  return hz;
}

static void clk_check(void) {
  uint32_t a, b, c, d, max_ext;
  if (clk_eligible >= 0) return;
  clk_eligible = 0;
  clk_cpuid(0, &a, &b, &c, &d);
  if (a < 1) {
    clk_why = "sem CPUID 1";
    return;
  }
  clk_cpuid(1, &a, &b, &c, &d);
  if (!(d & (1u << 4))) {
    clk_why = "CPU sem TSC";
    return;
  }
  if (c & (1u << 31)) {
    clk_why = "hypervisor (VM): TSC da VM nao serve";
    return;
  }
  clk_cpuid(0x80000000u, &max_ext, &b, &c, &d);
  if (max_ext < 0x80000007u) {
    clk_why = "TSC nao e invariante (sem CPUID 0x80000007)";
    return;
  }
  clk_cpuid(0x80000007u, &a, &b, &c, &d);
  if (!(d & (1u << 8))) {
    clk_why = "TSC nao e invariante";
    return;
  }
  clk_eligible = 1;
}

/* Tres medidas de 40 ms, fica a do meio. 0 = calibrou. */
static int clk_calibrate(void) {
  uint64_t m[3], t;
  int i, n = 0;
  for (i = 0; i < 3; i++) {
    uint64_t hz = clk_measure(40);
    if (hz) m[n++] = hz;
  }
  if (n < 2) {
    clk_why = "calibracao do TSC contra o PIT falhou";
    return -1;
  }
  if (n == 3) {
    if (m[0] > m[1]) { t = m[0]; m[0] = m[1]; m[1] = t; }
    if (m[1] > m[2]) { t = m[1]; m[1] = m[2]; m[2] = t; }
    if (m[0] > m[1]) { t = m[0]; m[0] = m[1]; m[1] = t; }
    clk_hz = m[1];
    clk_spread = (uint32_t)((m[2] - m[0]) * 1000000u / m[1]);
  } else {
    if (m[0] > m[1]) { t = m[0]; m[0] = m[1]; m[1] = t; }
    clk_hz = m[0] + (m[1] - m[0]) / 2;
    clk_spread = (uint32_t)((m[1] - m[0]) * 1000000u / clk_hz);
  }
  /* Medidas que nao batem (PIT estranho, SMI em todas) ou frequencia
   * absurda: fica com a contagem do IRQ0. */
  if (clk_spread > 2000u || clk_hz < 100000000ull || clk_hz > 10000000000ull) {
    clk_why = "calibracao do TSC instavel";
    clk_hz = 0;
    return -1;
  }
  clk_per_ms = clk_hz / 1000u;
  return clk_per_ms ? 0 : -1;
}

/* Liga a recuperacao a partir da contagem de agora (sem salto). */
static void clk_start(void) {
  uint64_t fl = clk_irq_save();
  clk_base = clk_rdtsc() - irq_tick_count * clk_per_ms;
  __asm__ volatile ("" ::: "memory");   /* base antes do clk_tsc */
  clk_tsc = 1;
  clk_irq_restore(fl);
}

static void clk_log(void) {
  serial_puts("[relogio] ");
  if (clk_tsc) {
    serial_puts("TSC 0x");
    serial_hex32((uint32_t)(clk_hz / 1000u));
    serial_puts(" kHz (desvio 0x");
    serial_hex32(clk_spread);
    serial_puts(" ppm): o IRQ0 recupera ticks perdidos\n");
  } else {
    serial_puts("so a contagem do IRQ0 (");
    serial_puts(clk_why);
    serial_puts(")\n");
  }
}

void irq_clock_init(void) {
  clk_check();
  if (clk_eligible == 1 && clk_calibrate() == 0) {
    clk_why = "TSC invariante";
    clk_start();
  }
  clk_log();
}

int irq_clock_set(int on) {
  if (!on) {
    clk_tsc = 0;
    clk_why = "desligado pelo comando RELOGIO";
    clk_log();
    return 0;
  }
  if (clk_tsc) return 0;
  clk_check();
  if (!clk_per_ms && clk_calibrate() != 0) {
    clk_log();
    return -1;
  }
  clk_why = clk_eligible == 1 ? "TSC invariante" : "TSC forcado pelo comando RELOGIO";
  clk_start();
  clk_log();
  return 0;
}

uint64_t irq_clock_hz(void) { return clk_hz; }

void irq_clock_info(struct irq_clock* out) {
  out->tsc = clk_tsc;
  out->eligible = clk_eligible;
  out->hz = clk_hz;
  out->spread_ppm = clk_spread;
  out->lost = clk_lost;
  out->stalls = clk_stalls;
  out->max_ms = clk_max;
  out->max_at = clk_max_at;
  out->last_at = clk_last_at;
  out->held = clk_held;
  out->now = irq_tick_count;
  out->why = clk_why;
  out->max_rip = clk_max_rip;
  out->nlog = clk_log_n < CLK_LOG_N ? clk_log_n : CLK_LOG_N;
  {
    uint32_t i;
    /* mais nova primeiro */
    for (i = 0; i < out->nlog; i++) out->log[i] = clk_log_v[(clk_log_n - 1u - i) % CLK_LOG_N];
  }
}

/* Segura o IRQ0 (cli) por ms pelo TSC e diz quantos ticks andaram ate o
 * primeiro IRQ0 depois. Com o TSC ligado deve dar ~ms; sem, ~1. */
int irq_clock_test(uint32_t ms, uint64_t* ticks) {
  uint64_t t0, fl, s, w, per;
  per = clk_per_ms;
  if (!per && clk_calibrate() != 0) return -1;
  per = clk_per_ms;
  t0 = irq_tick_count;
  fl = clk_irq_save();
  s = clk_rdtsc();
  while (clk_rdtsc() - s < ms * per) __asm__ volatile ("pause");
  __asm__ volatile ("sti" ::: "memory");
  /* o IRQ0 pendente entra logo depois do sti; espera a contagem mudar */
  w = clk_rdtsc();
  while (irq_tick_count == t0 && clk_rdtsc() - w < 20u * per) __asm__ volatile ("pause");
  *ticks = irq_tick_count - t0;
  if (!(fl & 0x200u)) __asm__ volatile ("cli" ::: "memory");
  return 0;
}
uint64_t irq_busy(void) { return bsp_busy_ticks; }
uint64_t irq_idle(void) { return bsp_idle_ticks; }

/* Marca o BSP como ocioso (esperando tecla/tarefa) para o TOP. */
void cpu_idle(int on) { bsp_idle_flag = on ? 1 : 0; }

/* Preempcao do WIN: chamado no IRQ0, depois do EOI, com o RIP interrompido. */
static void (*tick_hook)(uint64_t rip);
void irq_set_tick_hook(void (*fn)(uint64_t rip)) { tick_hook = fn; }

void irq_handle(isr_frame_t* f) {
  uint32_t intno = f->int_no;
  if (intno >= 32 && intno <= 47) {
    uint8_t irq = (uint8_t)(intno - 32);
    if (irq == 0) {
      /* So conta (e acerta pelo TSC): nenhum outro estado do kernel e
       * tocado aqui. */
      uint64_t n = irq_tick_count + 1;
      if (clk_tsc) {
        uint64_t d = clk_rdtsc() - clk_base, alvo;
        if ((int64_t)d < 0) d = 0;
        alvo = d / clk_per_ms;
        if (alvo > n) {
          /* IRQ0 ficou parado: os ticks do meio se perderam no PIC */
          uint64_t j = alvo - n;
          clk_lost += j;
          if (j >= 2) {
            clk_stalls++;
            clk_last_at = alvo;
          }
          if (j > clk_max) {
            clk_max = (uint32_t)(j > 0xFFFFFFFFu ? 0xFFFFFFFFu : j);
            clk_max_at = alvo;
            clk_max_rip = f->eip;
          }
          if (j + 1u >= CLK_LOG_MS) {
            struct irq_clock_stall* e = &clk_log_v[clk_log_n % CLK_LOG_N];
            e->at = alvo;
            e->ms = (uint32_t)(j + 1u > 0xFFFFFFFFu ? 0xFFFFFFFFu : j + 1u);
            e->rip = f->eip;
            clk_log_n++;
          }
          n = alvo;
        } else if (n > alvo + 1) {
          /* o PIT (1000,15 Hz) chegou antes: este tick fica para o proximo */
          n--;
          clk_held++;
        }
      }
      irq_tick_count = n;
      /* relogio dos programas Windows; so existe com um EXE deles rodando */
      if (wine_kuser) wine_tick(irq_tick_count);
      if (bsp_idle_flag || kt_cpu_idle(0)) bsp_idle_ticks++;
      else bsp_busy_ticks++;
    } else if (irq == 1 || irq == 12) {
      /* Teclado/mouse: so tira o byte do 8042 e poe na fila (ps2in.c). */
      ps2in_isr();
    }
    pic_send_eoi(irq);
    /* Depois do EOI: o gancho pode trocar de tarefa e so voltar bem depois.
     * So com o mundo (WIN) nesta CPU, e so se o BKL estiver livre ou com
     * ele: com uma thread de outra CPU no kernel, fica para o proximo tick. */
    if (irq == 0) {
      if (tick_hook && kt_world_here() && kt_bkl_try_world()) {
        tick_hook(f->eip);
        kt_bkl_drop_world();
      }
      kt_tick_pit();
    }
  }
}
