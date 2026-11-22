/* smuprobe.h - o SMU do AMD (gerente de energia/temperatura) esta vivo?
 *
 * No notebook (Ryzen 7 5825U) o Tctl e o contador de energia RAPL ficam
 * parados desde a primeira leitura do SODOS: o SMU parou e ninguem segura a
 * temperatura. Esta sonda roda em cada etapa do boot (efistub e kmain) para
 * achar a primeira etapa em que o contador para.
 *
 * Mede o MSR 0xC001029B (energia do pacote, atualizado pelo SMU ~a cada 1 ms)
 * antes e depois de ~20 ms (20000 leituras da porta 0x80, ~1 us cada; nao
 * depende de timer, serve antes do IDT e no efistub). de == 0 -> parado.
 * Le tambem o Tctl cru (SMN 0x59800 pelo 0x60/0x64 do 0:0.0).
 * So inline: o efistub nao pode chamar nada do kernel. */
#pragma once
#include <stdint.h>

struct smu_probe {
  int ok;          /* 0 = nao e AMD com RAPL */
  uint32_t de;     /* incremento do contador em ~20 ms */
  uint32_t tctl;   /* Tctl cru; 0 se a ponte nao e AMD */
};

static inline void sp_cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
  __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
static inline uint32_t sp_rdmsr_lo(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return lo;
}
static inline void sp_outl(uint16_t p, uint32_t v) { __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint32_t sp_inl(uint16_t p) { uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void sp_io_delay(void) { uint8_t v; __asm__ volatile ("inb $0x80, %0" : "=a"(v)); (void)v; }

static inline void smu_probe_run(struct smu_probe* p) {
  uint32_t a, b, c, d, fam, e0, e1, save, i;
  p->ok = 0;
  p->de = 0;
  p->tctl = 0;
  sp_cpuid(0, &a, &b, &c, &d);
  if (b != 0x68747541u || d != 0x69746E65u || c != 0x444D4163u) return;   /* AuthenticAMD */
  sp_cpuid(1, &a, &b, &c, &d);
  fam = (a >> 8) & 0xF;
  if (fam == 0xF) fam += (a >> 20) & 0xFF;
  if (fam < 0x17) return;
  sp_cpuid(0x80000000u, &a, &b, &c, &d);
  if (a < 0x80000007u) return;
  sp_cpuid(0x80000007u, &a, &b, &c, &d);
  if (!(d & (1u << 14))) return;                 /* sem RAPL: rdmsr daria #GP */
  sp_outl(0xCF8, 0x80000000u);
  if ((sp_inl(0xCFC) & 0xFFFFu) == 0x1022u) {
    sp_outl(0xCF8, 0x80000060u);
    save = sp_inl(0xCFC);
    sp_outl(0xCFC, 0x00059800u);
    sp_outl(0xCF8, 0x80000064u);
    p->tctl = sp_inl(0xCFC);
    sp_outl(0xCF8, 0x80000060u);
    sp_outl(0xCFC, save);
  }
  e0 = sp_rdmsr_lo(0xC001029Bu);
  for (i = 0; i < 20000u; i++) sp_io_delay();
  e1 = sp_rdmsr_lo(0xC001029Bu);
  p->de = e1 - e0;
  p->ok = 1;
}
