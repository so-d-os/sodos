/* smp.c - descobre as CPUs pelo ACPI (MADT), acorda os APs e cuida do LAPIC
 * (timer local por CPU e IPIs).
 *
 * Os APs entram em ap_main com o mesmo CR4/XCR0 da BSP, ligam o LAPIC timer
 * (periodico, SMP_TICK_MS) e caem no escalonador (kthread.c), que roda nelas
 * as threads dos programas. Toda falha (sem ACPI, sem LAPIC, AP que nao
 * sobe) volta para 1 CPU sem derrubar o sistema.
 */
#include <stdint.h>
#include "mm.h"
#include "smp.h"
#include "serial.h"
#include "net.h"
#include "irq.h"
#include "kthread.h"

#define SMP_MAX        8
#define TRAMP_ADDR     0x8000u
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
static volatile uint8_t ap_up[SMP_MAX];
static volatile uint64_t ap_work[SMP_MAX];
static volatile uint32_t ap_go;            /* BSP calibrou o timer: APs entram */
static uint32_t bsp_cr4;
static uint32_t bsp_xcr0_lo, bsp_xcr0_hi;
static uint32_t lapic_per_ms;               /* contagens do timer (div 16) por ms */
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
  /* Boot pelo GRUB (UEFI): copia do RSDP que veio na tag ACPI do Multiboot2
   * (kernel/multiboot_header.asm). No UEFI o RSDP nao fica na area de BIOS. */
  if (mem_eq((const uint8_t*)0x5F00, "RSD PTR ", 8) && sum_ok((const uint8_t*)0x5F00, 20))
    return (const uint8_t*)0x5F00;
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
  lapic_wr(0xF0, 0x1FF);              /* SVR: ligado, espurio 0xFF (sem EOI) */
  lapic_wr(0x80, 0);                  /* TPR: aceita tudo */
  lapic_wr(0xB0, 0);                  /* EOI */
}

static int ipi(uint32_t id, uint32_t cmd) {
  int i, rc = -1;
  uint64_t fl;
  __asm__ volatile ("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
  lapic_wr(0x310, id << 24);          /* destino */
  lapic_wr(0x300, cmd);               /* ICR low: dispara */
  for (i = 0; i < 100000; i++) {
    if (!(lapic_rd(0x300) & (1u << 12))) {  /* delivery idle */
      rc = 0;
      break;
    }
  }
  if (fl & 0x200u) __asm__ volatile ("sti" ::: "memory");
  return rc;
}

/* Todas as CPUs ligadas pelo SODOS menos esta, uma IPI por destino.
 * Antes era o shorthand 11 ("todas menos esta"), que chega tambem as CPUs
 * que o SODOS nao liga (SMP_MAX 8; o Ryzen 7 5825U tem 16 threads, as 8
 * de cima esperando SIPI). No notebook a primeira IPI de TLB depois do
 * smp_init (pci_mmio_map do NVMe) parava o SMU: Tctl e RAPL congelados,
 * sem controle de temperatura/ventoinha, PC desligando/resetando. */
static void ipi_others(uint32_t cmd) {
  uint32_t me = lapic_rd(0x20) >> 24;
  int i;
  for (i = 0; i < ncpu && i < SMP_MAX; i++) {
    if (ids[i] == me) continue;
    if (i > 0 && !ap_up[i]) continue;
    (void)ipi(ids[i], cmd);
  }
}

/* APIC id da CPU (para MSI); 0xFFFFFFFF se ela nao subiu. */
uint32_t smp_apic_id(int cpu) {
  if (cpu < 0 || cpu >= ncpu || (cpu > 0 && !ap_up[cpu])) return 0xFFFFFFFFu;
  return ids[cpu];
}

void smp_send_ipi(int cpu, uint8_t vec) {
  if (cpu < 0 || cpu >= ncpu || (cpu > 0 && !ap_up[cpu])) return;
  ipi(ids[cpu], vec);
}

void smp_ipi_others(uint8_t vec) {
  if (ap_online) ipi_others(vec);
}

void smp_nmi_others(void) {
  if (ap_online) ipi_others(0x400u);  /* delivery mode NMI */
}

static inline uint64_t smp_rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

/* Mede o timer do LAPIC contra o TSC calibrado (irq.c) ou, sem ele (VM),
 * contra o IRQ0 (PIT, 1 ms) da BSP. Pelo TSC um tick perdido no boot nao
 * entra na conta do timer das APs. */
static void lapic_calibrate(void) {
  uint64_t t, hz = irq_clock_hz();
  uint32_t left;
  lapic_wr(0x3E0, 0x3);               /* divide por 16 */
  lapic_wr(0x320, 0x10000u | 0xF2u);  /* mascarado, one-shot */
  if (hz) {
    uint64_t fl, t0, t1;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(fl) : : "memory");
    lapic_wr(0x380, 0xFFFFFFFFu);
    t0 = smp_rdtsc();
    do {
      t1 = smp_rdtsc();
    } while (t1 - t0 < hz / 20u);     /* 50 ms */
    left = lapic_rd(0x390);
    lapic_wr(0x380, 0);
    if (fl & 0x200u) __asm__ volatile ("sti" ::: "memory");
    lapic_per_ms = (uint32_t)((uint64_t)(0xFFFFFFFFu - left) * (hz / 1000u) / (t1 - t0));
    serial_puts("[smp] lapic timer (TSC): 0x");
    serial_hex32(lapic_per_ms);
    serial_puts(" contagens/ms (div 16)\n");
    return;
  }
  t = irq_ticks();
  while (irq_ticks() == t) __asm__ volatile ("pause");
  lapic_wr(0x380, 0xFFFFFFFFu);
  t = irq_ticks();
  while (irq_ticks() - t < 50) __asm__ volatile ("pause");
  left = lapic_rd(0x390);
  lapic_wr(0x380, 0);
  lapic_per_ms = (0xFFFFFFFFu - left) / 50u;
  serial_puts("[smp] lapic timer: 0x");
  serial_hex32(lapic_per_ms);
  serial_puts(" contagens/ms (div 16)\n");
}

void smp_timer_start(void) {
  if (!lapic_per_ms) return;
  lapic_wr(0x3E0, 0x3);
  lapic_wr(0x320, 0x20000u | 0xF2u);  /* periodico, vetor 0xF2 */
  lapic_wr(0x380, lapic_per_ms * SMP_TICK_MS);
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
  if (ipi(id, 0x00004608) < 0) return -1;   /* SIPI, vetor 8 -> 0x8000 */
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

/* Mesmo CR4 (OSXSAVE, FSGSBASE...) e XCR0 da BSP: as threads usam SSE/AVX e
 * rdfsbase/wrfsbase em qualquer CPU. */
static void ap_cpu_setup(void) {
  uint64_t cr4 = bsp_cr4;
  __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4) : "memory");
  if (cr4 & (1u << 18))
    __asm__ volatile ("xsetbv" :: "c"(0), "a"(bsp_xcr0_lo), "d"(bsp_xcr0_hi));
  __asm__ volatile ("fninit");
  mm_mtrr_ap();   /* mesmo tipo de cache da BSP na RAM acima de 4 GB */
}

/* Corpo dos APs: nunca volta (o trampolim chamou sem return address). */
void ap_main(uint32_t cpu) {
  if (cpu >= (uint32_t)ncpu || cpu >= SMP_MAX) {   /* AP que reiniciou */
    for (;;) __asm__ volatile ("cli; hlt");
  }
  ap_cpu_setup();
  lapic_enable();
  if (!ap_up[cpu]) {                      /* reentrada por SIPI nao conta 2x */
    ap_up[cpu] = 1;
    __sync_fetch_and_add(&ap_online, 1);
  }
  while (!ap_go) {
    ap_work[cpu]++;
    __asm__ volatile ("pause");
  }
  kt_ap_enter((int)cpu);                  /* liga o timer e vira ociosa */
  for (;;) __asm__ volatile ("cli; hlt");
}

int smp_cpu_count(void) { return ncpu; }

int smp_run(int cpu, void (*fn)(void*), void* arg) {
  uint32_t t0, dt;
  if (cpu <= 0 || cpu >= ncpu || cpu >= SMP_MAX || !ap_up[cpu] || !fn) return -1;
  if (!kt_world_here()) return -1;
  t0 = (uint32_t)irq_ticks();
  kt_world_migrate(cpu);
  if (kt_cpu_index() != cpu) return -1;
  fn(arg);
  kt_world_migrate(0);
  dt = (uint32_t)(irq_ticks() - t0);
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
  out->state = kt_cpu_idle(i) ? 0u : 1u;
  if (i == 0) {
    out->busy_ms = (unsigned)irq_busy();
    out->idle_ms = (unsigned)irq_idle();
  } else {
    uint32_t b = 0, d = 0;
    kt_cpu_times(i, &b, &d);
    out->busy_ms = b;
    out->idle_ms = d;
  }
  out->pad[0] = (unsigned)kt_cpu_threads(i);
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

  {
    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    bsp_cr4 = (uint32_t)cr4;
    if (cr4 & (1u << 18))
      __asm__ volatile ("xgetbv" : "=a"(bsp_xcr0_lo), "=d"(bsp_xcr0_hi) : "c"(0));
  }
  lapic_enable();
  lapic_calibrate();
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

  /* Prova de paralelismo: cada AP conta no proprio contador por 100 ms;
   * depois todos entram no escalonador. */
  for (i = 0; i < ncpu; i++) w0[i] = ap_work[i];
  net_delay(100);
  for (i = 0; i < ncpu; i++) {
    uint64_t d = ap_work[i] - w0[i];
    if (!d) continue;
    serial_puts("[smp] ap ");
    serial_hex32(ids[i]);
    serial_puts(" rodou 0x");
    serial_hex32((uint32_t)d);
    serial_puts(" voltas em 100 ms\n");
  }
  ap_go = 1;
  for (i = 0; i < 100 && kt_online_count() < 1 + (int)ap_online; i++) net_delay(1);
  serial_puts("[smp] APs online=0x");
  serial_hex32((uint32_t)ap_online);
  serial_puts(" no escalonador=0x");
  serial_hex32((uint32_t)(kt_online_count() - 1));
  serial_puts("\n");
}
