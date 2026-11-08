/*
 * amdgop.c - AMDGOP.DLL: driver de exibicao para a Radeon integrada do
 * notebook (AMD Barcelo, PCI 1002:15E7, Ryzen 7 5825U). Contrato em
 * docs/GPU.md; a placa entra pelo SODOS.CFG ([video] placa=1002:15E7).
 *
 * Nao liga o motor da GPU: isso exige firmware (PSP/SMU) e e o grosso do
 * amdgpu do Linux. Usa o framebuffer que o UEFI (GOP) ja deixou na tela e
 * faz o que a CPU faz hoje, com uma diferenca: copia para a tela so as areas
 * que o kernel diz que mudaram, em vez do quadro inteiro.
 *
 * Mede tudo e escreve no log (que vai para U:\SODOS\GPU.LOG): ciclos de uma
 * copia inteira (o caminho por CPU de hoje), e por quadro os ciclos e os
 * pixels copiados.
 *
 * Roda no contexto do kernel: gcc -m64 -mno-sse -mno-red-zone, sem libc.
 */
#include <stdint.h>
#include "gpu.h"

static const struct gpu_kapi* k;
static struct gpu_mode m;

/* medidas */
static uint32_t quadros;
static uint64_t ciclos, pixels;
static uint64_t ciclos_inteira;   /* uma copia do quadro inteiro, no selftest */

static uint64_t tsc(void) {
  uint32_t lo, hi;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return (uint64_t)hi << 32 | lo;
}

static void log_kv(const char* nome, uint32_t v) {
  k->log(nome);
  k->log_hex(v);
}

/* Copia um retangulo do back buffer para o framebuffer, linha a linha. */
static void copia(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  uint32_t r;
  for (r = 0; r < h; r++) {
    const uint8_t* src = (const uint8_t*)m.back + (y + r) * m.pitch + x * 4u;
    volatile uint8_t* dst = m.fb + (y + r) * m.fb_pitch + x * 4u;
    uint64_t n = w;
    __asm__ volatile ("rep movsl" : "+S"(src), "+D"(dst), "+c"(n) : : "memory");
  }
}

static int a_init(const struct gpu_kapi* kapi, const struct gpu_mode* mode) {
  uint32_t b;
  k = kapi;
  if (!k || k->abi != GPU_ABI || !mode || !mode->fb || !mode->back) return -1;
  m = *mode;
  if (m.w == 0 || m.h == 0 || m.fb_pitch < m.w * 4u) return -1;
  quadros = 0;
  ciclos = pixels = 0;
  /* o que a placa e, para o log: revisao, classe e os BARs */
  log_kv("[amdgop] pci ", m.pci_id);
  log_kv(" classe/rev ", k->pci_read32(m.pci_bus, m.pci_dev, m.pci_fun, 0x08));
  k->log("\n[amdgop] BARs");
  for (b = 0; b < 6; b++) log_kv(" ", k->pci_read32(m.pci_bus, m.pci_dev, m.pci_fun, 0x10 + b * 4u));
  log_kv("\n[amdgop] tela ", m.w);
  log_kv("x", m.h);
  log_kv(" fb ", (uint32_t)(uintptr_t)m.fb);
  log_kv(" pitch ", m.fb_pitch);
  k->log("\n");
  return 0;
}

/* Escreve um padrao no inicio do framebuffer, le de volta e restaura; depois
 * mede uma copia do quadro inteiro (o custo do caminho por CPU de hoje). */
static int a_selftest(void) {
  volatile uint32_t* p = (volatile uint32_t*)m.fb;
  uint32_t salvo[16];
  uint64_t t0;
  int i, ok = 1;
  for (i = 0; i < 16; i++) salvo[i] = p[i];
  for (i = 0; i < 16; i++) p[i] = 0x00A5C300u + (uint32_t)i;
  for (i = 0; i < 16; i++)
    if (p[i] != 0x00A5C300u + (uint32_t)i) ok = 0;
  for (i = 0; i < 16; i++) p[i] = salvo[i];
  if (!ok) {
    k->log("[amdgop] framebuffer nao guardou o padrao\n");
    return -1;
  }
  t0 = tsc();
  copia(0, 0, m.w, m.h);
  ciclos_inteira = tsc() - t0;
  log_kv("[amdgop] copia do quadro inteiro: ciclos ", (uint32_t)ciclos_inteira);
  k->log("\n");
  return 0;
}

static int a_present(const struct gpu_rect* r, int n) {
  uint64_t t0 = tsc();
  int i;
  if (!r || n <= 0) {
    copia(0, 0, m.w, m.h);
    pixels += (uint64_t)m.w * m.h;
  } else {
    for (i = 0; i < n; i++) {
      int32_t x = r[i].x, y = r[i].y, w = r[i].w, h = r[i].h;
      if (x < 0) {
        w += x;
        x = 0;
      }
      if (y < 0) {
        h += y;
        y = 0;
      }
      if (x + w > (int32_t)m.w) w = (int32_t)m.w - x;
      if (y + h > (int32_t)m.h) h = (int32_t)m.h - y;
      if (w <= 0 || h <= 0) continue;
      copia((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
      pixels += (uint64_t)w * (uint32_t)h;
    }
  }
  ciclos += tsc() - t0;
  quadros++;
  return 0;
}

/* GOP: no modo texto o fbcon escreve no mesmo framebuffer; nada a parar. */
static int a_suspend(void) { return 0; }

static int a_resume(const struct gpu_mode* mode) {
  if (!mode || !mode->fb || !mode->back) return -1;
  m = *mode;
  return 0;
}

static void a_fini(void) {
  if (!k) return;
  log_kv("[amdgop] fim: quadros ", quadros);
  if (quadros) {
    log_kv(" ciclos/quadro ", (uint32_t)(ciclos / quadros));
    log_kv(" pixels/quadro ", (uint32_t)(pixels / quadros));
  }
  log_kv(" (quadro inteiro: ciclos ", (uint32_t)ciclos_inteira);
  log_kv(" pixels ", m.w * m.h);
  k->log(")\n");
}

static const struct gpu_drv drv = {
  GPU_ABI,
  "amdgop (Radeon, GOP)",
  GPU_CAP_PRESENT | GPU_CAP_DAMAGE,
  a_init,
  a_selftest,
  a_present,
  a_fini,
  a_suspend,
  a_resume,
  0,
  0,
};

const struct gpu_drv* gpu_driver(void) { return &drv; }
