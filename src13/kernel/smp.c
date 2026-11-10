/* smp.c - descobre as CPUs pelo ACPI (MADT) e acorda os APs.
 *
 * Fase C1 do SMP: a BSP continua sendo a unica que roda o kernel; os APs
 * entram em ap_main, provam que executam em paralelo (contador proprio) e
 * ficam parados em hlt. Toda falha (sem ACPI, sem LAPIC, AP que nao sobe)
 * volta para 1 CPU sem derrubar o sistema.
 */
#include <stdint.h>
#include "smp.h"
#include "serial.h"
#include "net.h"
#include "irq.h"

#define SMP_MAX        8
/* 0xA000: o stage2 ja terminou.  0x8000/0x9000 NAO: sao os diretorios de
 * pagina de 1..3 GB montados pelo kernel_entry (ver kernel_entry.asm). */
#define TRAMP_ADDR     0xA000u
#define TRAMP_SIZE     0xF80u
#define LAPIC_DEFAULT  0xFEE00000u

/* Blob do trampolim (kernel/ap_tramp.asm via ap_blob.asm). */
extern const uint8_t ap_tramp_blob_start[];
extern const uint8_t ap_tramp_blob_end[];

/* Entrada dos APs, chamada pelo trampolim com a pilha do AP. */
void ap_main(uint32_t cpu);

static uint32_t lapic_base = LAPIC_DEFAULT;
static uint32_t ids[SMP_MAX];
static int ncpu;
static int bsp_id;
static volatile uint32_t ap_online;
static volatile uint32_t ap_parked;
static volatile uint32_t ap_park_ms;      /* quando os APs entraram em hlt */
static volatile uint8_t ap_up[SMP_MAX];
static volatile uint64_t ap_work[SMP_MAX];

/* Fila de tarefa por CPU: uma de cada vez, despachada por IPI (vetor 0xF1).
 * A BSP chama smp_run e fica em hlt; o AP roda e avisa de volta. */
static volatile void (*job_fn)(void*);
static volatile void* job_arg;
static volatile int job_cpu = -1;
static volatile int job_pending;
static volatile int job_done;
static volatile int job_busy;             /* tarefa em andamento (p/ TOP) */
static volatile uint32_t job_t0;
static volatile uint32_t ap_job_ms;       /* tempo da tarefa atual */
static volatile uint32_t ap_busy_ms[SMP_MAX];
static uint8_t ap_stacks[SMP_MAX][16384] __attribute__((aligned(16)));

/* ---------- portas/MSR ---------- */
static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
  __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint32_t lapic_rd(uint32_t off) {
  return *(volatile uint32_t*)((uintptr_t)lapic_base + off);
}
static inline void lapic_wr(uint32_t off, uint32_t v) {
  *(volatile uint32_t*)((uintptr_t)lapic_base + off) = v;
}

static uint32_t apic_id_self(void) {
  uint32_t a, b, c, d;
  __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
  (void)a; (void)c; (void)d;
  return b >> 24;
}

/* ---------- leitura de ACPI ---------- */
static int mem_eq(const uint8_t* p, const char* s, int n) {
  int i;
  for (i = 0; i < n; i++)
    if (p[i] != (uint8_t)s[i]) return 0;
  return 1;
}
static int sum_ok(const uint8_t* p, int n) {
  uint8_t s = 0;
  while (n-- > 0) s = (uint8_t)(s + *p++);
  return s == 0;
}
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static const uint8_t* rsdp_find(void) {
  uintptr_t ebda = ((uintptr_t)(*(const uint16_t*)0x40E)) << 4;
  uintptr_t p;
  if (ebda >= 0x80000 && ebda < 0xA0000) {
    for (p = ebda; p + 20 <= ebda + 1024; p += 16)
      if (mem_eq((const uint8_t*)p, "RSD PTR ", 8) && sum_ok((const uint8_t*)p, 20))
        return (const uint8_t*)p;
  }
  for (p = 0xE0000; p + 20 <= 0x100000; p += 16)
    if (mem_eq((const uint8_t*)p, "RSD PTR ", 8) && sum_ok((const uint8_t*)p, 20))
      return (const uint8_t*)p;
  return 0;
}

static const uint8_t* acpi_table(const uint8_t* rsdp, const char* sig) {
  uint8_t rev = rsdp[15];
  uint32_t rsdt = rd32(rsdp + 16);
  uint64_t xsdt = (rev >= 2) ? rd64(rsdp + 24) : 0;
  uint32_t i, n;
  if (rsdt) {
    const uint8_t* t = (const uint8_t*)(uintptr_t)rsdt;
    uint32_t len = rd32(t + 4);
    if (len < 36) return 0;
    n = (len - 36) / 4;
    for (i = 0; i < n; i++) {
      const uint8_t* e = (const uint8_t*)(uintptr_t)rd32(t + 36 + i * 4);
      if (e && rd32(e + 4) >= 36 && mem_eq(e, sig, 4)) return e;
    }
  }
  if (xsdt) {
    const uint8_t* t = (const uint8_t*)(uintptr_t)xsdt;
    uint32_t len = rd32(t + 4);
    if (len < 36) return 0;
    n = (len - 36) / 8;
    for (i = 0; i < n; i++) {
      const uint8_t* e = (const uint8_t*)(uintptr_t)rd64(t + 36 + i * 8);
      if (e && rd32(e + 4) >= 36 && mem_eq(e, sig, 4)) return e;
    }
  }
  return 0;
}

static void add_id(uint32_t id) {
  int i;
  if (id >= 256) return;              /* so xAPIC (8 bits) */
  for (i = 0; i < ncpu; i++)
    if (ids[i] == id) return;
  if (ncpu >= SMP_MAX) return;
  ids[ncpu++] = id;
}

static void madt_parse(const uint8_t* m) {
  uint32_t len = rd32(m + 4);
  uint32_t off;
  if (len < 44) return;
  if (rd32(m + 36)) lapic_base = rd32(m + 36);
  off = 44;
  while (off + 2 <= len) {
    uint8_t type = m[off];
    uint8_t elen = m[off + 1];
    if (elen < 2) break;
    if (type == 0 && elen >= 8) {
      if (m[off + 4] & 1) add_id(m[off + 3]);
    } else if (type == 9 && elen >= 12) {
      if (rd32(m + off + 8) & 1) add_id(rd32(m + off + 4) & 0xFF);
    }
    off += elen;
  }
}

/* ---------- LAPIC / IPI ---------- */
static void lapic_enable(void) {
  uint64_t b = rdmsr(0x1B);
  b |= 1u << 11;                      /* APIC enable */
  b &= ~(1u << 10);                   /* xAPIC (nao x2APIC) */
  wrmsr(0x1B, b);
  lapic_wr(0xF0, 0x1F1);              /* SVR: ligado, espurio 0xF1 (tem gate) */
  lapic_wr(0x80, 0);                  /* TPR: aceita tudo */
  lapic_wr(0xB0, 0);                  /* EOI */
}

static int ipi(uint32_t id, uint32_t cmd) {
  int i;
  lapic_wr(0x310, id << 24);          /* destino */
  lapic_wr(0x300, cmd);               /* ICR low: dispara */
  for (i = 0; i < 100000; i++) {
    if (!(lapic_rd(0x300) & (1u << 12))) return 0;  /* delivery idle */
  }
  return -1;
}

static void write_params(int cpu) {
  uint64_t cr3;
  struct desc_ptr { uint16_t limit; uint64_t base; } __attribute__((packed));
  struct desc_ptr gdtr, idtr;
  uintptr_t top = (uintptr_t)ap_stacks[cpu] + sizeof(ap_stacks[cpu]) - 16;
  int i;

  for (i = 0; i < (int)TRAMP_SIZE; i += 8) {
    *(volatile uint64_t*)(TRAMP_ADDR + (uint32_t)i) =
        *(const uint64_t*)(ap_tramp_blob_start + i);
  }
  __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
  *(volatile uint32_t*)(TRAMP_ADDR + 0xF40) = (uint32_t)cr3;
  *(volatile uint64_t*)(TRAMP_ADDR + 0xF48) = (uint64_t)top;
  *(volatile uint64_t*)(TRAMP_ADDR + 0xF50) = (uint64_t)(uintptr_t)ap_main;
  *(volatile uint32_t*)(TRAMP_ADDR + 0xF58) = (uint32_t)cpu;
  __asm__ volatile ("sgdt %0" : "=m"(gdtr));
  __asm__ volatile ("sidt %0" : "=m"(idtr));
  for (i = 0; i < 10; i++) {
    ((volatile uint8_t*)(TRAMP_ADDR + 0xF60))[i] = ((const uint8_t*)&gdtr)[i];
    ((volatile uint8_t*)(TRAMP_ADDR + 0xF70))[i] = ((const uint8_t*)&idtr)[i];
  }
}

static int start_ap(int cpu) {
  uint32_t id = ids[cpu];
  uint32_t base = ap_online;
  int i;
  write_params(cpu);
  if (ipi(id, 0x00004500) < 0) return -1;   /* INIT (edge, delivery 5) */
  net_delay(10);
  if (ipi(id, 0x00004600 | (TRAMP_ADDR >> 12)) < 0) return -1;   /* SIPI -> TRAMP_ADDR */
  for (i = 0; i < 100; i++) {
    if (ap_online > base) return 0;
    net_delay(1);
  }
  /* So reenvia se o AP nao subiu: um SIPI a mais reinicia o AP em modo real. */
  ipi(id, 0x00004608);
  for (i = 0; i < 100; i++) {
    if (ap_online > base) return 0;
    net_delay(1);
  }
  return -1;
}

void smp_ipi_ack(void) { lapic_wr(0xB0, 0); }

/* Depois da prova de paralelismo: acorda por IPI, roda a tarefa da vez e
 * avisa a BSP (IPI de volta). Fica em hlt quando nao ha tarefa. */
static void ap_idle(uint32_t cpu) {
  for (;;) {
    if (job_pending && job_cpu == (int)cpu) {
      void (*fn)(void*) = (void(*)(void*))job_fn;
      void* arg = (void*)job_arg;
      job_pending = 0;
      fn(arg);
      job_done = 1;
      ipi(ids[0], 0x000000F1);
    }
    __asm__ volatile ("sti; hlt");
  }
}

/* Corpo dos APs: nunca volta (o trampolim chamou sem return address). */
void ap_main(uint32_t cpu) {
  if (cpu >= (uint32_t)ncpu) {              /* parametro de um AP que reiniciou */
    for (;;) __asm__ volatile ("cli; hlt");
  }
  if (cpu < SMP_MAX) {
    lapic_enable();
    if (!ap_up[cpu]) {                      /* reentrada por SIPI nao conta 2x */
      ap_up[cpu] = 1;
      __sync_fetch_and_add(&ap_online, 1);
    }
    for (;;) {
      if (ap_parked) ap_idle(cpu);          /* nao volta */
      ap_work[cpu]++;
      __asm__ volatile ("pause");
    }
  }
  for (;;) __asm__ volatile ("cli; hlt");
}

int smp_cpu_count(void) { return ncpu; }

int smp_run(int cpu, void (*fn)(void*), void* arg) {
  uint32_t dt;
  if (cpu <= 0 || cpu >= ncpu || cpu >= SMP_MAX || !ap_up[cpu] || !fn) return -1;
  job_fn = fn;
  job_arg = arg;
  job_cpu = cpu;
  job_t0 = (uint32_t)irq_ticks();
  ap_job_ms = 0;
  job_done = 0;
  job_busy = 1;
  job_pending = 1;
  if (ipi(ids[cpu], 0x000000F1) < 0) {
    job_pending = 0;
    job_busy = 0;
    return -1;
  }
  cpu_idle(1);                       /* o BSP fica ocioso enquanto o AP roda */
  while (!job_done) {
    ap_job_ms = (uint32_t)(irq_ticks() - job_t0);   /* uso ao vivo no TOP */
    __asm__ volatile ("sti; hlt");
  }
  cpu_idle(0);
  dt = (uint32_t)(irq_ticks() - job_t0);
  ap_busy_ms[cpu] += dt;
  ap_job_ms = 0;
  job_busy = 0;
  job_done = 0;
  serial_puts("[smp] cpu ");
  serial_hex32((uint32_t)cpu);
  serial_puts(" tarefa: ");
  serial_hex32(dt);
  serial_puts(" ms\n");
  return 0;
}

int smp_cpu_stat(int i, struct k_cpu_stat* out) {
  if (!out || i < 0 || i >= ncpu) return -1;
  out->id = ids[i];
  out->index = (unsigned)i;
  out->online = (i == 0) ? 1u : (ap_up[i] ? 1u : 0u);
  out->state = (i == 0) ? 1u : (ap_parked ? 0u : (ap_up[i] ? 1u : 0u));
  if (i == 0) {
    out->busy_ms = (unsigned)irq_busy();
    out->idle_ms = (unsigned)irq_idle();
  } else {
    uint32_t busy = ap_busy_ms[i];
    uint32_t up = ap_parked ? (uint32_t)(irq_ticks() - ap_park_ms) : 0u;
    if (job_busy && job_cpu == i) busy += ap_job_ms;
    out->busy_ms = busy;
    out->idle_ms = up > busy ? up - busy : 0u;
  }
  out->pad[0] = 0;
  out->pad[1] = 0;
  return 0;
}

void smp_init(void) {
  const uint8_t* rsdp;
  const uint8_t* madt;
  uint64_t w0[SMP_MAX];
  int i, started = 0;

  rsdp = rsdp_find();
  if (!rsdp) {
    serial_puts("[smp] sem ACPI: 1 cpu\n");
    return;
  }
  madt = acpi_table(rsdp, "APIC");
  if (!madt) {
    serial_puts("[smp] sem MADT: 1 cpu\n");
    return;
  }
  madt_parse(madt);
  if (ncpu < 1) {
    serial_puts("[smp] MADT sem CPU: 1 cpu\n");
    return;
  }

  serial_puts("[smp] cpus=0x");
  serial_hex32((uint32_t)ncpu);
  serial_puts(" lapic=0x");
  serial_hex32(lapic_base);
  serial_puts("\n");
  if (ncpu == 1) return;

  lapic_enable();
  bsp_id = (int)apic_id_self();
  serial_puts("[smp] bsp apic=0x");
  serial_hex32((uint32_t)bsp_id);
  serial_puts("\n");

  /* ids[0] = BSP, para o TOP mostrar CPU0 como a BSP. */
  for (i = 0; i < ncpu; i++) {
    if ((int)ids[i] == bsp_id) {
      uint32_t t = ids[0];
      ids[0] = ids[i];
      ids[i] = t;
      break;
    }
  }

  for (i = 0; i < ncpu; i++) {
    if ((int)ids[i] == bsp_id) continue;
    serial_puts("[smp] acordando ap ");
    serial_hex32(ids[i]);
    serial_puts("\n");
    if (start_ap(i) != 0) {
      serial_puts("[smp] ap ");
      serial_hex32(ids[i]);
      serial_puts(" nao subiu; parando o bring-up\n");
      break;
    }
    started++;
  }

  if (started < ncpu - 1) {
    serial_puts("[smp] APs online=0x");
    serial_hex32((uint32_t)ap_online);
    serial_puts(" de 0x");
    serial_hex32((uint32_t)(ncpu - 1));
    serial_puts("\n");
  }
  if (started == 0 && ap_online == 0) return;

  /* Prova de paralelismo: cada AP conta no proprio contador por 200 ms. */
  for (i = 0; i < ncpu; i++) w0[i] = ap_work[i];
  net_delay(200);
  for (i = 0; i < ncpu; i++) {
    uint64_t d = ap_work[i] - w0[i];
    if (!d) continue;
    serial_puts("[smp] ap ");
    serial_hex32(ids[i]);
    serial_puts(" rodou 0x");
    serial_hex32((uint32_t)d);
    serial_puts(" voltas em 200 ms\n");
  }
  ap_park_ms = (uint32_t)irq_ticks();
  ap_parked = 1;                    /* APs ficam em hlt ate a fase seguinte */
  net_delay(5);
  serial_puts("[smp] APs online=0x");
  serial_hex32((uint32_t)ap_online);
  serial_puts(" (parados)\n");
}
