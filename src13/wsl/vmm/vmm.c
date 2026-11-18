/* vmm.c - Nucleo AMD-V (SVM) do WSL.EXE. Ver vmm.h.
 *
 * Fluxo: vmm_run aloca RAM da VM (acima de 4 GB, 2 MB alinhada), monta a
 * paginacao aninhada (NPT, paginas de 2 MB), carrega o bzImage pelo
 * protocolo de boot de 64 bits do Linux (boot_params, e820, linha de
 * comando, GDT e tabelas de pagina em identidade dentro da VM) e sobe uma
 * thread presa numa CPU do SODOS que faz VMRUN em laco. A thread principal
 * fica com o console: imprime a serial da VM e manda o teclado para ela. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vmm.h"
#include "../sodos_api.h"
#include "term.h"

/* ---------------- VMCB ---------------- */
#define C_INTERCEPT_CR 0x000
#define C_INTERCEPT_EXC 0x008
#define C_INTERCEPT1 0x00C
#define C_INTERCEPT2 0x010
#define C_IOPM 0x040
#define C_MSRPM 0x048
#define C_TSC_OFFSET 0x050
#define C_ASID 0x058
#define C_TLB_CTL 0x05C
#define C_VINTR 0x060
#define C_INT_STATE 0x068
#define C_EXITCODE 0x070
#define C_EXITINFO1 0x078
#define C_EXITINFO2 0x080
#define C_EXITINTINFO 0x088
#define C_NP_ENABLE 0x090
#define C_EVENTINJ 0x0A8
#define C_NCR3 0x0B0
#define C_CLEAN 0x0C0
#define C_NRIP 0x0C8
#define C_INSN_LEN 0x0D0
#define C_INSN 0x0D1

#define S_ES 0x400
#define S_CS 0x410
#define S_SS 0x420
#define S_DS 0x430
#define S_FS 0x440
#define S_GS 0x450
#define S_GDTR 0x460
#define S_LDTR 0x470
#define S_IDTR 0x480
#define S_TR 0x490
#define S_CPL 0x4CB
#define S_EFER 0x4D0
#define S_CR4 0x548
#define S_CR3 0x550
#define S_CR0 0x558
#define S_DR7 0x560
#define S_DR6 0x568
#define S_RFLAGS 0x570
#define S_RIP 0x578
#define S_RSP 0x5D8
#define S_RAX 0x5F8
#define S_CR2 0x640
#define S_GPAT 0x668

/* INTERCEPT1 */
#define I1_INTR (1u << 0)
#define I1_NMI (1u << 1)
#define I1_VINTR (1u << 4)
#define I1_CPUID (1u << 18)
#define I1_HLT (1u << 24)
#define I1_INVLPGA (1u << 26)
#define I1_IOIO (1u << 27)
#define I1_MSR (1u << 28)
#define I1_SHUTDOWN (1u << 31)
/* INTERCEPT2 */
#define I2_VMRUN (1u << 0)
#define I2_VMMCALL (1u << 1)
#define I2_VMLOAD (1u << 2)
#define I2_VMSAVE (1u << 3)
#define I2_STGI (1u << 4)
#define I2_CLGI (1u << 5)
#define I2_SKINIT (1u << 6)
#define I2_MONITOR (1u << 10)
#define I2_MWAIT (1u << 11)
#define I2_XSETBV (1u << 13)

#define EX_INTR 0x60
#define EX_NMI 0x61
#define EX_VINTR 0x64
#define EX_CPUID 0x72
#define EX_HLT 0x78
#define EX_INVLPGA 0x7A
#define EX_IOIO 0x7B
#define EX_MSR 0x7C
#define EX_SHUTDOWN 0x7F
#define EX_VMRUN 0x80
#define EX_VMMCALL 0x81
#define EX_VMLOAD 0x82
#define EX_VMSAVE 0x83
#define EX_STGI 0x84
#define EX_CLGI 0x85
#define EX_SKINIT 0x86
#define EX_MONITOR 0x8A
#define EX_MWAIT 0x8B
#define EX_XSETBV 0x8D
#define EX_NPF 0x400

#define MSR_EFER 0xC0000080u
#define MSR_VM_CR 0xC0010114u
#define MSR_HSAVE 0xC0010117u
#define EFER_SVME (1ull << 12)

#define V64(vm, o) (*(volatile uint64_t*)((vm)->vmcb + (o)))
#define V32(vm, o) (*(volatile uint32_t*)((vm)->vmcb + (o)))
#define V16(vm, o) (*(volatile uint16_t*)((vm)->vmcb + (o)))
#define V8(vm, o) (*(volatile uint8_t*)((vm)->vmcb + (o)))

/* ---------------- CPU ---------------- */
static inline void cpuid_(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
  __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}
static inline uint64_t rdmsr_(uint32_t m) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
  return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr_(uint32_t m, uint64_t v) {
  __asm__ volatile("wrmsr" : : "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t read_cr4(void) {
  uint64_t v;
  __asm__ volatile("mov %%cr4, %0" : "=r"(v));
  return v;
}
static inline uint64_t xgetbv0(void) {
  uint32_t lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return ((uint64_t)hi << 32) | lo;
}

/* ---------------- log ---------------- */
void vm_log(struct vm* vm, const char* fmt, ...) {
  char b[1024];
  va_list ap;
  int n;
  (void)vm;
  va_start(ap, fmt);
  n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof b - 1) n = sizeof b - 1;
  k_dbg(b, n);
  printf("%s", b);
}

/* ---------------- memoria ---------------- */
static void* halloc(uint64_t n, uint64_t align) {
  void* p = k_hmalloc(n, align < 4096 ? 4096 : align);
  if (p) memset(p, 0, (size_t)n);
  return p;
}

/* ---------------- checagem ---------------- */
static uint32_t g_svm_feat; /* CPUID 8000000A EDX */

int vmm_check(char* why, int max) {
  uint32_t r[4];
  cpuid_(0x80000000u, 0, r);
  if (r[0] < 0x8000000Au) {
    snprintf(why, (size_t)max, "CPU sem as folhas CPUID de SVM");
    return -1;
  }
  cpuid_(0x80000001u, 0, r);
  if (!(r[2] & (1u << 2))) {
    snprintf(why, (size_t)max, "a CPU nao tem AMD-V (SVM); Intel VT-x ainda nao e suportado");
    return -1;
  }
  if (rdmsr_(MSR_VM_CR) & (1u << 4)) {
    snprintf(why, (size_t)max, "AMD-V desligado no BIOS (SVM Mode / SVMDIS)");
    return -1;
  }
  cpuid_(0x8000000Au, 0, r);
  g_svm_feat = r[3];
  if (!(r[3] & 1u)) {
    snprintf(why, (size_t)max, "a CPU nao tem paginacao aninhada (NPT)");
    return -1;
  }
  if (!(r[3] & (1u << 3))) {
    snprintf(why, (size_t)max, "a CPU nao tem NRIP save");
    return -1;
  }
  return 0;
}

/* SVM ligado e area de salvamento do host na CPU corrente. */
static uint8_t* g_hsave[256];
static int svm_enable_here(void) {
  int c = k_thread_cpu();
  if (c < 0 || c >= 256) return -1;
  if (!g_hsave[c]) {
    g_hsave[c] = (uint8_t*)halloc(4096, 4096);
    if (!g_hsave[c]) return -1;
  }
  wrmsr_(MSR_EFER, rdmsr_(MSR_EFER) | EFER_SVME);
  wrmsr_(MSR_HSAVE, (uint64_t)(uintptr_t)g_hsave[c]);
  return c;
}

/* ---------------- carga do Linux ---------------- */
#define G_GDT 0x500u
#define G_STACK 0x6F00u
#define G_BOOTP 0x7000u
#define G_PML4 0x9000u
#define G_CMDLINE 0x20000u

static void e820_add(uint8_t* bp, uint64_t addr, uint64_t size, uint32_t type) {
  uint8_t n = bp[0x1E8];
  uint8_t* e = bp + 0x2D0 + n * 20;
  memcpy(e, &addr, 8);
  memcpy(e + 8, &size, 8);
  memcpy(e + 16, &type, 4);
  bp[0x1E8] = (uint8_t)(n + 1);
}

static int load_linux(struct vm* vm, const vmm_config* c, uint64_t* entry) {
  const uint8_t* k = c->kernel;
  uint32_t len = c->kernel_len, setup, pm_off, init_size, cl_max, hdr_end, i;
  uint16_t ver;
  uint64_t load = 0x1000000, pref;
  uint8_t* bp;
  uint64_t* pml4;
  if (len < 0x1000 || rd32(k + 0x202) != 0x53726448u) {
    vm_log(vm, "WSL: o kernel nao e um bzImage\n");
    return -1;
  }
  ver = rd16(k + 0x206);
  if (ver < 0x20C || !(k[0x236] & 1)) {
    vm_log(vm, "WSL: bzImage sem entrada de 64 bits (protocolo %x)\n", ver);
    return -1;
  }
  setup = k[0x1F1] ? k[0x1F1] : 4;
  pm_off = (setup + 1) * 512;
  pref = rd64(k + 0x258);
  init_size = rd32(k + 0x260);
  if (pref >= 0x100000 && pref + init_size < vm->ram_size) load = pref;
  if (pm_off >= len || load + init_size >= vm->ram_size || load + (len - pm_off) >= vm->ram_size) {
    vm_log(vm, "WSL: RAM da VM pequena para o kernel\n");
    return -1;
  }
  memcpy(vm->ram + load, k + pm_off, len - pm_off);

  bp = vm->ram + G_BOOTP;
  memset(bp, 0, 4096);
  hdr_end = 0x202 + k[0x201];
  if (hdr_end > 0x290) hdr_end = 0x290;
  memcpy(bp + 0x1F1, k + 0x1F1, hdr_end - 0x1F1);
  bp[0x210] = 0xFF;                /* type_of_loader: indefinido */
  bp[0x211] |= 0x01 | 0x80;         /* LOADED_HIGH | CAN_USE_HEAP */
  *(uint16_t*)(bp + 0x224) = 0xFE00;
  *(uint32_t*)(bp + 0x214) = (uint32_t)load;
  *(uint32_t*)(bp + 0x228) = G_CMDLINE;
  *(uint32_t*)(bp + 0x218) = 0; /* sem initrd */
  *(uint32_t*)(bp + 0x21C) = 0;
  cl_max = rd32(k + 0x238);
  if (!cl_max || cl_max > 4095) cl_max = 4095;
  {
    size_t n = strlen(c->cmdline);
    if (n > cl_max) n = cl_max;
    memcpy(vm->ram + G_CMDLINE, c->cmdline, n);
    vm->ram[G_CMDLINE + n] = 0;
  }
  e820_add(bp, 0, 0x9FC00, 1);
  e820_add(bp, 0x9FC00, 0x400, 2);
  e820_add(bp, 0xF0000, 0x10000, 2);
  e820_add(bp, 0x100000, vm->ram_size - 0x100000, 1);

  /* GDT: 0x10 codigo 64, 0x18 dados */
  {
    uint64_t gdt[4] = {0, 0, 0x00AF9A000000FFFFull, 0x00CF92000000FFFFull};
    memcpy(vm->ram + G_GDT, gdt, sizeof gdt);
  }
  /* paginas: 4 GB em identidade, 2 MB */
  pml4 = (uint64_t*)(vm->ram + G_PML4);
  memset(pml4, 0, 6 * 4096);
  pml4[0] = (G_PML4 + 0x1000) | 3;
  for (i = 0; i < 4; i++) {
    uint64_t* pdpt = (uint64_t*)(vm->ram + G_PML4 + 0x1000);
    uint64_t* pd = (uint64_t*)(vm->ram + G_PML4 + 0x2000 + i * 0x1000);
    uint32_t j;
    pdpt[i] = (G_PML4 + 0x2000 + i * 0x1000) | 3;
    for (j = 0; j < 512; j++) pd[j] = ((uint64_t)(i * 512 + j) << 21) | 0x83;
  }
  *entry = load + 0x200;
  vm_log(vm, "WSL: kernel %u KB carregado em %llx, protocolo %x\n", len >> 10,
         (unsigned long long)load, ver);
  return 0;
}

/* ---------------- NPT ---------------- */
static uint64_t build_npt(struct vm* vm) {
  /* 4 GB fisicos da VM; so a RAM e mapeada (o resto vira NPF = MMIO) */
  uint64_t* pml4 = (uint64_t*)halloc(4096, 4096);
  uint64_t* pdpt = (uint64_t*)halloc(4096, 4096);
  uint64_t gpa;
  if (!pml4 || !pdpt) return 0;
  pml4[0] = (uint64_t)(uintptr_t)pdpt | 7;
  for (gpa = 0; gpa < vm->ram_size; gpa += 0x200000) {
    uint32_t i = (uint32_t)(gpa >> 30), j = (uint32_t)((gpa >> 21) & 511);
    uint64_t* pd;
    if (!(pdpt[i] & 1)) {
      pd = (uint64_t*)halloc(4096, 4096);
      if (!pd) return 0;
      pdpt[i] = (uint64_t)(uintptr_t)pd | 7;
    }
    pd = (uint64_t*)(uintptr_t)(pdpt[i] & ~0xFFFull);
    pd[j] = ((uint64_t)(uintptr_t)vm->ram + gpa) | 0x87;
  }
  return (uint64_t)(uintptr_t)pml4;
}

static void msrpm_pass(uint8_t* pm, uint32_t msr) {
  uint32_t base;
  if (msr < 0x2000) base = 0;
  else if (msr >= 0xC0000000u && msr < 0xC0002000u) base = 0x800, msr -= 0xC0000000u;
  else if (msr >= 0xC0010000u && msr < 0xC0012000u) base = 0x1000, msr -= 0xC0010000u;
  else return;
  pm[base + msr / 4] &= (uint8_t)~(3u << ((msr % 4) * 2));
}

static void seg(struct vm* vm, uint32_t off, uint16_t sel, uint16_t attr, uint32_t limit, uint64_t base) {
  V16(vm, off) = sel;
  V16(vm, off + 2) = attr;
  V32(vm, off + 4) = limit;
  V64(vm, off + 8) = base;
}

/* ---------------- CPUID ---------------- */
static uint64_t g_host_xcr0;
static int g_use_xsave;

static uint32_t xsave_size_for(uint64_t xcr0) {
  uint32_t r[4], sz = 576, i;
  for (i = 2; i < 64; i++) {
    if (!(xcr0 & (1ull << i))) continue;
    cpuid_(0xD, i, r);
    if (r[0] && r[1] + r[0] > sz) sz = r[1] + r[0];
  }
  return sz;
}

static void do_cpuid(struct vm* vm, uint32_t leaf, uint32_t sub, uint32_t r[4]) {
  uint32_t m[4];
  uint32_t maxstd, maxext;
  uint64_t cr4 = V64(vm, S_CR4);
  cpuid_(0, 0, m);
  maxstd = m[0] < 0xD ? m[0] : 0xD;
  cpuid_(0x80000000u, 0, m);
  maxext = m[0] < 0x80000008u ? m[0] : 0x80000008u;
  r[0] = r[1] = r[2] = r[3] = 0;
  if (leaf >= 0x40000000u && leaf < 0x80000000u) return; /* sem assinatura de hipervisor */
  if (leaf < 0x40000000u && leaf > maxstd) return;
  if (leaf >= 0x80000000u && leaf > maxext) return;
  cpuid_(leaf, sub, r);
  switch (leaf) {
  case 0:
    r[0] = maxstd;
    break;
  case 1:
    r[1] &= 0x0000FFFFu; /* APIC id 0, 1 processador logico */
    r[2] &= ~((1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 15) |
              (1u << 18) | (1u << 21) | (1u << 24) | (1u << 27));
    if (!g_use_xsave) r[2] &= ~((1u << 26) | (1u << 28) | (1u << 12) | (1u << 29));
    if (g_use_xsave && (cr4 & (1u << 18))) r[2] |= 1u << 27; /* OSXSAVE da VM */
    r[2] |= 1u << 31;                                         /* hipervisor */
    r[3] &= ~((1u << 7) | (1u << 9) | (1u << 12) | (1u << 14) | (1u << 18) | (1u << 21) |
              (1u << 22) | (1u << 27) | (1u << 28) | (1u << 29) | (1u << 31));
    break;
  case 2: case 3:
    break;
  case 7:
    if (sub == 0) {
      r[1] &= ~((1u << 2) | (1u << 4) | (1u << 11) | (1u << 14));
      r[2] &= ~((1u << 4) | (1u << 5) | (1u << 7) | (1u << 16));
      if (cr4 & (1u << 22)) r[2] |= 1u << 4; /* OSPKE */
      r[3] &= ~((1u << 20) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | (1u << 31));
      if (!g_use_xsave) r[1] &= ~((1u << 5) | (1u << 16) | (1u << 17) | (1u << 28) | (1u << 30) | (1u << 31));
    } else if (sub != 1) {
      r[0] = r[1] = r[2] = r[3] = 0;
    }
    break;
  case 0xD:
    if (!g_use_xsave) {
      r[0] = r[1] = r[2] = r[3] = 0;
    } else if (sub == 0) {
      r[0] &= (uint32_t)g_host_xcr0;
      r[3] &= (uint32_t)(g_host_xcr0 >> 32);
      r[1] = xsave_size_for(vm->regs.guest_xcr0);
      r[2] = xsave_size_for(g_host_xcr0);
    } else if (sub == 1) {
      r[0] &= ~(1u << 3); /* sem XSAVES */
      r[1] = r[2] = r[3] = 0;
    } else if (sub >= 64 || !(g_host_xcr0 & (1ull << sub))) {
      r[0] = r[1] = r[2] = r[3] = 0;
    }
    break;
  case 0x80000000u:
    r[0] = maxext;
    break;
  case 0x80000001u:
    r[2] &= ~((1u << 1) | (1u << 2) | (1u << 3) | (1u << 9) | (1u << 10) | (1u << 12) |
              (1u << 13) | (1u << 15) | (1u << 22) | (1u << 23) | (1u << 24) | (1u << 26) |
              (1u << 27) | (1u << 28) | (1u << 29));
    r[3] &= ~((1u << 7) | (1u << 9) | (1u << 12) | (1u << 14));
    break;
  case 0x80000002u: case 0x80000003u: case 0x80000004u:
  case 0x80000005u: case 0x80000006u:
    break;
  case 0x80000007u:
    r[0] = r[1] = r[2] = 0;
    r[3] &= 1u << 8; /* TSC invariante */
    break;
  case 0x80000008u:
    r[1] = 0;
    r[2] = 0; /* 1 nucleo */
    r[3] = 0;
    break;
  default:
    r[0] = r[1] = r[2] = r[3] = 0;
  }
}

/* ---------------- MSR ---------------- */
#define NMSR 64
static uint32_t g_msr_id[NMSR];
static uint64_t g_msr_val[NMSR];
static int g_nmsr;

static uint64_t msr_read(struct vm* vm, uint32_t m) {
  int i;
  switch (m) {
  case 0x10: return rdtsc_();
  case MSR_EFER: return V64(vm, S_EFER) & ~EFER_SVME;
  case 0x277: return V64(vm, S_GPAT);
  }
  for (i = 0; i < g_nmsr; i++)
    if (g_msr_id[i] == m) return g_msr_val[i];
  return 0;
}

static void msr_write(struct vm* vm, uint32_t m, uint64_t v) {
  int i;
  switch (m) {
  case 0x10: return;
  case MSR_EFER: {
    uint64_t old = V64(vm, S_EFER);
    V64(vm, S_EFER) = (v & ~(1ull << 10)) | (old & (1ull << 10)) | EFER_SVME;
    return;
  }
  case 0x277: V64(vm, S_GPAT) = v; return;
  }
  for (i = 0; i < g_nmsr; i++)
    if (g_msr_id[i] == m) {
      g_msr_val[i] = v;
      return;
    }
  if (g_nmsr < NMSR) {
    g_msr_id[g_nmsr] = m;
    g_msr_val[g_nmsr++] = v;
  }
}

/* ---------------- memoria da VM vista pela VM ---------------- */
/* gva -> gpa pela paginacao da VM. ~0 se nao mapeado. */
static uint64_t gva_to_gpa(struct vm* vm, uint64_t gva) {
  uint64_t cr0 = V64(vm, S_CR0), cr3 = V64(vm, S_CR3), e = 0;
  int lv;
  if (!(cr0 & (1ull << 31))) return gva;
  e = cr3 & 0x000FFFFFFFFFF000ull;
  for (lv = 3; lv >= 0; lv--) {
    uint64_t idx = (gva >> (12 + 9 * lv)) & 511;
    uint8_t* p = gpa_ptr(vm, e + idx * 8, 8);
    uint64_t pte;
    if (!p) return ~0ull;
    memcpy(&pte, p, 8);
    if (!(pte & 1)) return ~0ull;
    if (lv > 0 && lv < 3 && (pte & 0x80)) {
      uint64_t mask = (1ull << (12 + 9 * lv)) - 1;
      return (pte & 0x000FFFFFFFFFF000ull & ~mask) | (gva & mask);
    }
    e = pte & 0x000FFFFFFFFFF000ull;
  }
  return e | (gva & 0xFFF);
}

static int fetch_insn(struct vm* vm, uint8_t* out) {
  int n = V8(vm, C_INSN_LEN) & 0xF, i;
  uint64_t rip;
  if (n > 0) {
    for (i = 0; i < n; i++) out[i] = V8(vm, C_INSN + i);
    for (; i < 15; i++) out[i] = 0;
    return n;
  }
  rip = V64(vm, S_RIP) + V64(vm, S_CS + 8);
  for (i = 0; i < 15; i++) {
    uint64_t g = gva_to_gpa(vm, rip + i);
    uint8_t* p = g == ~0ull ? 0 : gpa_ptr(vm, g, 1);
    if (!p) break;
    out[i] = *p;
  }
  return i;
}

/* ---------------- decodificador de MMIO ---------------- */
struct insn {
  int len;
  int size;  /* bytes acessados */
  int dsize; /* tamanho do destino (movzx) */
  int reg;   /* registrador (-1 = imediato) */
  int hibyte; /* ah..bh */
  int write;
  int zext, sext;
  uint64_t imm;
};

static int decode(const uint8_t* b, int n, struct insn* d) {
  int i = 0, rex = 0, o16 = 0, op, modrm, mod, rm;
  memset(d, 0, sizeof *d);
  d->reg = -1;
  while (i < n) {
    uint8_t p = b[i];
    if (p == 0x66) o16 = 1;
    else if (p == 0x67 || p == 0x2E || p == 0x3E || p == 0x26 || p == 0x36 || p == 0x64 ||
             p == 0x65 || p == 0xF0 || p == 0xF2 || p == 0xF3) {
    } else break;
    i++;
  }
  if (i < n && (b[i] & 0xF0) == 0x40) rex = b[i++];
  if (i >= n) return -1;
  op = b[i++];
  if (op == 0x0F) {
    if (i >= n) return -1;
    op = 0x0F00 | b[i++];
  }
  switch (op) {
  case 0x88: d->write = 1; d->size = 1; break;
  case 0x89: d->write = 1; d->size = (rex & 8) ? 8 : o16 ? 2 : 4; break;
  case 0x8A: d->size = 1; break;
  case 0x8B: d->size = (rex & 8) ? 8 : o16 ? 2 : 4; break;
  case 0xC6: d->write = 1; d->size = 1; break;
  case 0xC7: d->write = 1; d->size = (rex & 8) ? 8 : o16 ? 2 : 4; break;
  case 0x87: d->write = 2; d->size = (rex & 8) ? 8 : o16 ? 2 : 4; break; /* xchg */
  case 0x0FB6: d->size = 1; d->zext = 1; break;
  case 0x0FB7: d->size = 2; d->zext = 1; break;
  case 0x0FBE: d->size = 1; d->sext = 1; break;
  case 0x0FBF: d->size = 2; d->sext = 1; break;
  default: return -1;
  }
  d->dsize = (d->zext || d->sext) ? ((rex & 8) ? 8 : o16 ? 2 : 4) : d->size;
  if (i >= n) return -1;
  modrm = b[i++];
  mod = modrm >> 6;
  rm = modrm & 7;
  if (mod == 3) return -1;
  if (op != 0xC6 && op != 0xC7) {
    d->reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
    if (d->size == 1 && !rex && d->reg >= 4 && d->reg < 8 && !d->zext && !d->sext) {
      d->hibyte = 1;
      d->reg -= 4;
    }
  }
  if (rm == 4) {
    if (i >= n) return -1;
    if (mod == 0 && (b[i] & 7) == 5) i += 4;
    i++;
  } else if (mod == 0 && rm == 5) {
    i += 4;
  }
  if (mod == 1) i += 1;
  else if (mod == 2) i += 4;
  if (op == 0xC6) {
    d->imm = b[i];
    i += 1;
  } else if (op == 0xC7) {
    if (o16 && !(rex & 8)) {
      d->imm = rd16(b + i);
      i += 2;
    } else {
      d->imm = (uint64_t)(int64_t)(int32_t)rd32(b + i);
      i += 4;
    }
  }
  if (i > n) return -1;
  d->len = i;
  return 0;
}

static uint64_t reg_get(struct vm* vm, int r) { return vm->regs.gpr[r]; }

static void reg_set_sized(struct vm* vm, int r, int size, int hibyte, uint64_t v) {
  uint64_t* g = &vm->regs.gpr[r];
  if (hibyte) *g = (*g & ~0xFF00ull) | ((v & 0xFF) << 8);
  else if (size == 1) *g = (*g & ~0xFFull) | (v & 0xFF);
  else if (size == 2) *g = (*g & ~0xFFFFull) | (v & 0xFFFF);
  else if (size == 4) *g = v & 0xFFFFFFFFull;
  else *g = v;
}

static int handle_npf(struct vm* vm) {
  uint64_t gpa = V64(vm, C_EXITINFO2);
  uint8_t ib[16];
  struct insn d;
  int n;
  uint64_t val = 0;
  if (gpa < vm->ram_size) {
    vm_log(vm, "WSL: NPF dentro da RAM em %llx (info %llx)\n", (unsigned long long)gpa,
           (unsigned long long)V64(vm, C_EXITINFO1));
    return -1;
  }
  n = fetch_insn(vm, ib);
  if (n <= 0 || decode(ib, n, &d)) {
    vm_log(vm, "WSL: MMIO em %llx com instrucao que nao sei decodificar (rip %llx: %02x %02x %02x %02x)\n",
           (unsigned long long)gpa, (unsigned long long)V64(vm, S_RIP), ib[0], ib[1], ib[2], ib[3]);
    return -1;
  }
  if (d.write) {
    uint64_t old = 0;
    if (d.reg >= 0) {
      val = reg_get(vm, d.reg);
      if (d.hibyte) val >>= 8;
    } else {
      val = d.imm;
    }
    if (d.size < 8) val &= (1ull << (d.size * 8)) - 1;
    if (d.write == 2) dev_mmio(vm, gpa, d.size, 0, &old); /* xchg: le antes */
    dev_mmio(vm, gpa, d.size, 1, &val);
    if (d.write == 2) reg_set_sized(vm, d.reg, d.size, d.hibyte, old);
  } else {
    dev_mmio(vm, gpa, d.size, 0, &val);
    if (d.size < 8) val &= (1ull << (d.size * 8)) - 1;
    if (d.sext) {
      if (d.size == 1) val = (uint64_t)(int64_t)(int8_t)val;
      else val = (uint64_t)(int64_t)(int16_t)val;
    }
    reg_set_sized(vm, d.reg, d.dsize, d.hibyte, val);
  }
  V64(vm, S_RIP) += (uint64_t)d.len;
  return 0;
}

/* ---------------- excecoes e interrupcoes ---------------- */
static void inject_exc(struct vm* vm, int vec, int has_err, uint32_t err) {
  uint64_t e = (uint64_t)vec | (3ull << 8) | (1ull << 31);
  if (has_err) e |= (1ull << 11) | ((uint64_t)err << 32);
  V64(vm, C_EVENTINJ) = e;
}

/* Pende interrupcao do PIC; 1 se a VM pode recebe-la agora. */
static int prepare_irq(struct vm* vm) {
  uint64_t eii = V64(vm, C_EXITINTINFO);
  int vec;
  if (V64(vm, C_EVENTINJ) & (1ull << 31)) return 0; /* ja tem algo (excecao) */
  if (eii & (1ull << 31)) {
    /* evento que a saida interrompeu: entrega de novo */
    V64(vm, C_EVENTINJ) = eii;
    return 0;
  }
  vec = pic_pending_vector(vm);
  if (vec < 0) {
    V64(vm, C_VINTR) &= ~(1ull << 8);
    V32(vm, C_INTERCEPT1) &= ~I1_VINTR;
    return 0;
  }
  if ((V64(vm, S_RFLAGS) & (1u << 9)) && !(V64(vm, C_INT_STATE) & 1)) {
    V64(vm, C_EVENTINJ) = (uint64_t)vec | (1ull << 31);
    pic_ack(vm, vec);
    V64(vm, C_VINTR) &= ~(1ull << 8);
    V32(vm, C_INTERCEPT1) &= ~I1_VINTR;
    return 1;
  }
  /* janela de interrupcao: V_IRQ + intercepta VINTR */
  V64(vm, C_VINTR) |= (1ull << 8) | (0xFull << 16) | (1ull << 20);
  V32(vm, C_INTERCEPT1) |= I1_VINTR;
  return 0;
}

/* ---------------- laco da VM ---------------- */
struct run_ctx {
  struct vm* vm;
  const vmm_config* cfg;
  int cpu;
};

static volatile int g_wake;

static int vcpu_thread(void* arg) {
  struct run_ctx* rc = (struct run_ctx*)arg;
  struct vm* vm = rc->vm;
  int cur = -1, halted = 0;
  unsigned t0 = k_ms();
  uint64_t nexits = 0;
  while (!vm->stop) {
    uint64_t code;
    int c = k_thread_cpu();
    if (c != cur) {
      if (svm_enable_here() < 0) {
        snprintf(vm->stop_why, sizeof vm->stop_why, "falhou ao ligar SVM na CPU %d", c);
        vm->exit_code = 1;
        vm->stop = 1;
        break;
      }
      if (cur >= 0) V8(vm, C_TLB_CTL) = 1;
      cur = c;
    }
    if (rc->cfg->timeout_s && k_ms() - t0 > rc->cfg->timeout_s * 1000u) {
      snprintf(vm->stop_why, sizeof vm->stop_why, "tempo limite (%u s)", rc->cfg->timeout_s);
      vm->stop = 1;
      break;
    }
    dev_poll(vm, rdtsc_());
    if (halted) {
      if (!(V64(vm, S_RFLAGS) & (1u << 9))) {
        snprintf(vm->stop_why, sizeof vm->stop_why, "a VM parou (hlt com interrupcoes desligadas)");
        vm->stop = 1;
        break;
      }
      if (pic_pending_vector(vm) < 0) {
        uint64_t now = rdtsc_(), next = dev_next_event(vm);
        uint64_t ms2 = vm->tsc_hz / 500;
        if (next > now + ms2) k_futex_wait(&g_wake, 0, 1);
        else __asm__ volatile("pause");
        continue;
      }
      halted = 0;
    }
    prepare_irq(vm);
    V64(vm, S_RAX) = vm->regs.gpr[0];
    V64(vm, S_RSP) = vm->regs.gpr[4];
    svm_enter(&vm->regs);
    vm->regs.gpr[0] = V64(vm, S_RAX);
    vm->regs.gpr[4] = V64(vm, S_RSP);
    V8(vm, C_TLB_CTL) = 0;
    nexits++;
    code = V64(vm, C_EXITCODE);
    /* evento injetado foi entregue (ou volta em EXITINTINFO) */
    V64(vm, C_EVENTINJ) = 0;
    switch (code) {
    case EX_INTR:
    case EX_NMI:
      break;
    case EX_VINTR:
      V64(vm, C_VINTR) &= ~(1ull << 8);
      V32(vm, C_INTERCEPT1) &= ~I1_VINTR;
      break;
    case EX_CPUID: {
      uint32_t r[4];
      do_cpuid(vm, (uint32_t)vm->regs.gpr[0], (uint32_t)vm->regs.gpr[1], r);
      vm->regs.gpr[0] = r[0];
      vm->regs.gpr[3] = r[1];
      vm->regs.gpr[1] = r[2];
      vm->regs.gpr[2] = r[3];
      V64(vm, S_RIP) = V64(vm, C_NRIP);
      break;
    }
    case EX_HLT:
      V64(vm, S_RIP) = V64(vm, C_NRIP);
      halted = 1;
      break;
    case EX_IOIO: {
      uint64_t i1 = V64(vm, C_EXITINFO1);
      uint16_t port = (uint16_t)(i1 >> 16);
      int size = (i1 & 0x10) ? 1 : (i1 & 0x20) ? 2 : 4;
      if (i1 & 4) {
        static int warned;
        if (!warned++) vm_log(vm, "WSL: I/O de string na porta %x ignorado\n", port);
      } else if (i1 & 1) {
        uint32_t v = dev_in(vm, port, size);
        if (size == 4) vm->regs.gpr[0] = v;
        else reg_set_sized(vm, 0, size, 0, v);
      } else {
        uint32_t v = (uint32_t)vm->regs.gpr[0];
        if (size == 1) v &= 0xFF;
        else if (size == 2) v &= 0xFFFF;
        dev_out(vm, port, size, v);
      }
      V64(vm, S_RIP) = V64(vm, C_EXITINFO2);
      break;
    }
    case EX_MSR: {
      uint32_t m = (uint32_t)vm->regs.gpr[1];
      if (V64(vm, C_EXITINFO1) & 1) {
        msr_write(vm, m, (vm->regs.gpr[0] & 0xFFFFFFFFull) | (vm->regs.gpr[2] << 32));
      } else {
        uint64_t v = msr_read(vm, m);
        vm->regs.gpr[0] = v & 0xFFFFFFFFull;
        vm->regs.gpr[2] = v >> 32;
      }
      V64(vm, S_RIP) = V64(vm, C_NRIP);
      break;
    }
    case EX_XSETBV: {
      uint64_t v = (vm->regs.gpr[0] & 0xFFFFFFFFull) | (vm->regs.gpr[2] << 32);
      if ((uint32_t)vm->regs.gpr[1] != 0 || !(v & 1) || (v & ~g_host_xcr0) ||
          ((v & 4) && !(v & 2))) {
        inject_exc(vm, 13, 1, 0);
      } else {
        vm->regs.guest_xcr0 = v;
        V64(vm, S_RIP) = V64(vm, C_NRIP);
      }
      break;
    }
    case EX_NPF:
      if (handle_npf(vm)) {
        snprintf(vm->stop_why, sizeof vm->stop_why, "acesso a memoria nao tratado em %llx",
                 (unsigned long long)V64(vm, C_EXITINFO2));
        vm->exit_code = 1;
        vm->stop = 1;
      }
      break;
    case EX_MONITOR:
    case EX_MWAIT:
      V64(vm, S_RIP) = V64(vm, C_NRIP);
      break;
    case EX_VMRUN: case EX_VMMCALL: case EX_VMLOAD: case EX_VMSAVE:
    case EX_STGI: case EX_CLGI: case EX_SKINIT: case EX_INVLPGA:
      inject_exc(vm, 6, 0, 0); /* #UD: sem SVM aninhado */
      break;
    case EX_SHUTDOWN:
      snprintf(vm->stop_why, sizeof vm->stop_why, "a VM reiniciou (triple fault, rip %llx)",
               (unsigned long long)V64(vm, S_RIP));
      vm->stop = 1;
      break;
    default:
      snprintf(vm->stop_why, sizeof vm->stop_why, "saida %llx inesperada (info %llx %llx, rip %llx)",
               (unsigned long long)code, (unsigned long long)V64(vm, C_EXITINFO1),
               (unsigned long long)V64(vm, C_EXITINFO2), (unsigned long long)V64(vm, S_RIP));
      vm->exit_code = 1;
      vm->stop = 1;
    }
  }
  if (vm->verbose) vm_log(vm, "WSL: %llu saidas da VM\n", (unsigned long long)nexits);
  return 0;
}

static uint64_t measure_tsc(void) {
  unsigned t0 = k_ms(), t1;
  uint64_t a, b;
  while ((t1 = k_ms()) == t0) {
  }
  a = rdtsc_();
  while (k_ms() - t1 < 200) k_yield();
  b = rdtsc_();
  t0 = k_ms();
  return (b - a) * 1000ull / (uint64_t)(t0 - t1);
}

/* ---------------- console ---------------- */
static void rx_push(struct vm* vm, const char* s, int n) {
  int i;
  for (i = 0; i < n; i++) {
    uint32_t h = vm->rx_head;
    if (h - vm->rx_tail >= sizeof vm->rx) return;
    vm->rx[h % sizeof vm->rx] = (uint8_t)s[i];
    vm->rx_head = h + 1;
  }
}

static void term_reply(void* arg, const char* s) { rx_push((struct vm*)arg, s, (int)strlen(s)); }

/* Teclas especiais do SODOS (shell.h) -> sequencias do TERM=linux */
static const char* key_seq(int ch) {
  switch (ch) {
  case 0x100: return "\033[A";
  case 0x101: return "\033[B";
  case 0x102: return "\033[D";
  case 0x103: return "\033[C";
  case 0x104: return "\033[5~";
  case 0x105: return "\033[6~";
  case 0x106: return "\033[1~";
  case 0x107: return "\033[4~";
  case 0x108: return "\033[3~";
  case 0x109: return "\033[2~";
  case 0x141: return "\033[[A";
  case 0x142: return "\033[[B";
  case 0x143: return "\033[[C";
  case 0x144: return "\033[[D";
  case 0x145: return "\033[[E";
  case 0x146: return "\033[17~";
  case 0x147: return "\033[18~";
  case 0x148: return "\033[19~";
  case 0x149: return "\033[20~";
  case 0x14A: return "\033[21~";
  case 0x14B: return "\033[23~";
  case 0x14C: return "\033[24~";
  }
  return 0;
}

static void console_loop(struct vm* vm, const vmm_config* cfg) {
  char line[512];
  int ln = 0;
  size_t want = cfg->input_after ? strlen(cfg->input_after) : 0;
  size_t matched = 0;
  int typed = cfg->input ? 0 : 1;
  const char* inp = cfg->input;
  term_init(term_reply, vm);
  k_kbd_raw(1); /* Ctrl+C e Ctrl+letra vao para o Linux */
  while (!vm->stop || vm->tx_tail != vm->tx_head) {
    int any = 0, ch;
    while (vm->tx_tail != vm->tx_head) {
      uint8_t c = vm->tx[vm->tx_tail % UART_RING];
      vm->tx_tail++;
      any = 1;
      term_write(&c, 1);
      line[ln++] = (char)c;
      if (c == '\n' || ln == (int)sizeof line) {
        k_dbg(line, ln);
        ln = 0;
      }
      if (!typed && want) {
        if (c == (uint8_t)cfg->input_after[matched]) {
          if (++matched == want) {
            typed = -1; /* pode digitar */
          }
        } else {
          matched = c == (uint8_t)cfg->input_after[0] ? 1 : 0;
        }
      }
    }
    if (typed == -1 || (!typed && !want)) {
      /* digita a entrada pedida (-x) */
      while (*inp) {
        uint32_t h = vm->rx_head;
        if (h - vm->rx_tail >= sizeof vm->rx) break;
        vm->rx[h % sizeof vm->rx] = (uint8_t)(*inp == '\n' ? '\r' : *inp);
        vm->rx_head = h + 1;
        inp++;
      }
      if (!*inp) typed = 1;
    }
    if (any) term_flush();
    while ((ch = k_trygetc()) > 0) {
      const char* seq = key_seq(ch);
      if (ch == 0x1D) { /* Ctrl+] */
        snprintf(vm->stop_why, sizeof vm->stop_why, "Ctrl+] no console");
        vm->stop = 1;
        break;
      }
      if (seq) rx_push(vm, seq, (int)strlen(seq));
      else if (ch < 0x100) {
        char b = (char)ch;
        if (ch == '\n') b = '\r';
        else if (ch == 8) b = 0x7F;
        rx_push(vm, &b, 1);
      }
      any = 1;
    }
    if (!any) k_futex_wait(&g_wake, 0, 10);
  }
  k_kbd_raw(0);
  term_end();
  if (ln) k_dbg(line, ln);
}

int vmm_run(const vmm_config* cfg) {
  struct vm* vm;
  char why[128];
  uint64_t entry, npt;
  uint8_t *iopm, *msrpm;
  struct run_ctx rc;
  int tid, ncpu;
  if (vmm_check(why, sizeof why)) {
    printf("WSL: %s\n", why);
    return 1;
  }
  vm = (struct vm*)halloc(sizeof *vm, 4096);
  if (!vm) {
    printf("WSL: sem memoria (SET HIMEM?)\n");
    return 1;
  }
  vm->verbose = cfg->verbose;
  vm->ram_size = (uint64_t)cfg->ram_mb << 20;
  if (vm->ram_size < (128ull << 20) || vm->ram_size > (3ull << 30)) {
    printf("WSL: RAM da VM deve ficar entre 128 MB e 3072 MB\n");
    return 1;
  }
  vm->ram = (uint8_t*)k_hmalloc(vm->ram_size, 0x200000);
  if (!vm->ram) {
    printf("WSL: nao consegui %u MB acima de 4 GB para a VM (use -m menor ou SET HIMEM=)\n",
           cfg->ram_mb);
    return 1;
  }
  memset(vm->ram, 0, (size_t)vm->ram_size);
  vm->disk = cfg->disk;
  vm->disk_bytes = cfg->disk.size;
  vm->cow_max_chunks = cfg->cow_mb * 256u;

  g_use_xsave = (read_cr4() & (1u << 18)) != 0;
  g_host_xcr0 = g_use_xsave ? xgetbv0() : 1;
  vm->regs.use_xsave = (uint64_t)g_use_xsave;
  vm->regs.host_xcr0 = g_host_xcr0;
  vm->regs.guest_xcr0 = 1;
  vm->regs.guest_fpu = halloc(16384, 4096);
  vm->regs.host_fpu = halloc(16384, 4096);
  vm->vmcb = (uint8_t*)halloc(4096, 4096);
  {
    uint8_t* hs = (uint8_t*)halloc(4096, 4096);
    iopm = (uint8_t*)halloc(12288, 4096);
    msrpm = (uint8_t*)halloc(8192, 4096);
    if (!vm->regs.guest_fpu || !vm->regs.host_fpu || !vm->vmcb || !hs || !iopm || !msrpm) {
      printf("WSL: sem memoria para o VMCB\n");
      return 1;
    }
    vm->regs.host_save_pa = (uint64_t)(uintptr_t)hs;
  }
  /* FPU da VM no estado inicial: FCW 0x37F, MXCSR 0x1F80 */
  *(uint16_t*)vm->regs.guest_fpu = 0x37F;
  *(uint32_t*)((uint8_t*)vm->regs.guest_fpu + 24) = 0x1F80;
  vm->regs.vmcb_pa = (uint64_t)(uintptr_t)vm->vmcb;

  vm->tsc_hz = measure_tsc();
  vm_log(vm, "WSL: AMD-V ok (decode assists %s), TSC %llu MHz, %s\n",
         (g_svm_feat & (1u << 7)) ? "sim" : "nao", (unsigned long long)(vm->tsc_hz / 1000000),
         g_use_xsave ? "XSAVE" : "FXSAVE");

  if (load_linux(vm, cfg, &entry)) return 1;
  npt = build_npt(vm);
  if (!npt) {
    printf("WSL: sem memoria para a NPT\n");
    return 1;
  }
  memset(iopm, 0xFF, 12288);
  memset(msrpm, 0xFF, 8192);
  {
    static const uint32_t pass[] = {0xC0000081u, 0xC0000082u, 0xC0000083u, 0xC0000084u,
                                    0xC0000100u, 0xC0000101u, 0xC0000102u, 0x174, 0x175, 0x176};
    unsigned i;
    for (i = 0; i < sizeof pass / sizeof pass[0]; i++) msrpm_pass(msrpm, pass[i]);
  }

  /* controle */
  V32(vm, C_INTERCEPT1) = I1_INTR | I1_NMI | I1_CPUID | I1_HLT | I1_INVLPGA | I1_IOIO | I1_MSR |
                          I1_SHUTDOWN;
  V32(vm, C_INTERCEPT2) = I2_VMRUN | I2_VMMCALL | I2_VMLOAD | I2_VMSAVE | I2_STGI | I2_CLGI |
                          I2_SKINIT | I2_MONITOR | I2_MWAIT | I2_XSETBV;
  V64(vm, C_IOPM) = (uint64_t)(uintptr_t)iopm;
  V64(vm, C_MSRPM) = (uint64_t)(uintptr_t)msrpm;
  V64(vm, C_TSC_OFFSET) = 0;
  V32(vm, C_ASID) = 1;
  V8(vm, C_TLB_CTL) = 1;
  V64(vm, C_VINTR) = 1ull << 24; /* V_INTR_MASKING */
  V64(vm, C_NP_ENABLE) = 1;
  V64(vm, C_NCR3) = npt;
  V32(vm, C_CLEAN) = 0;

  /* estado da VM: modo longo, paginas em identidade */
  seg(vm, S_CS, 0x10, 0xA9B, 0xFFFFFFFF, 0);
  seg(vm, S_DS, 0x18, 0xC93, 0xFFFFFFFF, 0);
  seg(vm, S_ES, 0x18, 0xC93, 0xFFFFFFFF, 0);
  seg(vm, S_SS, 0x18, 0xC93, 0xFFFFFFFF, 0);
  seg(vm, S_FS, 0x18, 0xC93, 0xFFFFFFFF, 0);
  seg(vm, S_GS, 0x18, 0xC93, 0xFFFFFFFF, 0);
  seg(vm, S_GDTR, 0, 0, 0x1F, G_GDT);
  seg(vm, S_IDTR, 0, 0, 0, 0);
  seg(vm, S_LDTR, 0, 0x82, 0, 0);
  seg(vm, S_TR, 0, 0x8B, 0x67, 0);
  V8(vm, S_CPL) = 0;
  V64(vm, S_EFER) = (1ull << 8) | (1ull << 10) | EFER_SVME;
  V64(vm, S_CR0) = 0x80000031ull;
  V64(vm, S_CR3) = G_PML4;
  V64(vm, S_CR4) = 0x20;
  V64(vm, S_DR6) = 0xFFFF0FF0ull;
  V64(vm, S_DR7) = 0x400;
  V64(vm, S_RFLAGS) = 2;
  V64(vm, S_RIP) = entry;
  V64(vm, S_GPAT) = 0x0007040600070406ull;
  vm->regs.gpr[4] = G_STACK;
  vm->regs.gpr[6] = G_BOOTP; /* rsi = boot_params */

  dev_init(vm);
  if (vm->dev && cfg->net) dev_net_enable(vm);
  if (!vm->dev) {
    printf("WSL: sem memoria para os dispositivos\n");
    return 1;
  }

  ncpu = k_cpu_count();
  rc.vm = vm;
  rc.cfg = cfg;
  rc.cpu = cfg->cpu >= 0 && cfg->cpu < ncpu ? cfg->cpu : ncpu - 1;
  vm_log(vm, "WSL: VM com %u MB, disco %llu MB, CPU %d do SODOS; Ctrl+] encerra\n", cfg->ram_mb,
         (unsigned long long)(vm->disk_bytes >> 20), rc.cpu);
  vm_log(vm, "WSL: linha de comando: %s\n", cfg->cmdline);
  tid = k_thread_create(vcpu_thread, &rc, 1u << 20, 1u << rc.cpu);
  if (tid <= 0) {
    printf("WSL: nao consegui criar a thread da VM\n");
    return 1;
  }
  console_loop(vm, cfg);
  k_thread_join(tid, 0);
  vm_log(vm, "\nWSL: VM encerrada: %s\n", vm->stop_why[0] ? vm->stop_why : "?");
  /* a RAM da VM fica com o programa e e solta quando ele sai */
  return vm->exit_code;
}
