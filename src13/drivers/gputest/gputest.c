/*
 * gputest.c - Driver de placa de TESTE (GPUTEST.DLL). Nao acelera nada:
 * copia o back buffer para o framebuffer pela CPU, como gfx_copy_back.
 * Serve para exercitar o carregador da etapa 1 (docs/GPU.md): carga,
 * selftest, present, memoria contada no TOP e desligamento seguro.
 *
 * So entra com gpudrv=GPUTEST.DLL no D:\SODOS\VIDEO.CFG (nenhuma placa
 * aponta para ele). Compilado com -DGPU_FALHA=N vira GPUFAIL.DLL, que
 * devolve erro no quadro N para testar a volta ao caminho por CPU.
 *
 * Roda no contexto do kernel: gcc -m64 -mno-sse -mno-red-zone, sem libc.
 */
#include <stdint.h>
#include "gpu.h"

static const struct gpu_kapi* k;
static struct gpu_mode m;
static void* fila;           /* so para o TOP mostrar memoria do driver */
static uint32_t quadros;

static int t_init(const struct gpu_kapi* kapi, const struct gpu_mode* mode) {
  k = kapi;
  if (!k || k->abi != GPU_ABI || !mode || !mode->fb || !mode->back) return -1;
  m = *mode;
  fila = k->alloc(64u * 1024u, 4096u);
  if (!fila) return -1;
  quadros = 0;
  k->log("[gputest] init ");
  k->log_hex(m.w);
  k->log("x");
  k->log_hex(m.h);
  k->log(" pci ");
  k->log_hex(m.pci_id);
  k->log("\n");
  return 0;
}

/* Escreve um padrao na primeira linha do LFB, le de volta e restaura. */
static int t_selftest(void) {
  volatile uint32_t* p = (volatile uint32_t*)m.fb;
  uint32_t salvo[16];
  int i, ok = 1;
  for (i = 0; i < 16; i++) salvo[i] = p[i];
  for (i = 0; i < 16; i++) p[i] = 0x00A5C300u + (uint32_t)i;
  for (i = 0; i < 16; i++)
    if (p[i] != 0x00A5C300u + (uint32_t)i) ok = 0;
  for (i = 0; i < 16; i++) p[i] = salvo[i];
  return ok ? 0 : -1;
}

static int t_present(const struct gpu_rect* r, int n) {
  uint32_t y;
  (void)r;
  (void)n;
  quadros++;
#ifdef GPU_FALHA
  if (quadros >= GPU_FALHA) return -1;
#endif
  for (y = 0; y < m.h; y++) {
    const uint32_t* src = (const uint32_t*)((const uint8_t*)m.back + y * m.pitch);
    volatile uint32_t* dst = (volatile uint32_t*)(m.fb + y * m.fb_pitch);
    uint32_t c = m.w;
    __asm__ volatile ("rep movsl" : "+S"(src), "+D"(dst), "+c"(c) : : "memory");
  }
  return 0;
}

/* O LFB nao muda de lugar; o modo novo pode ter outro tamanho. */
static int t_suspend(void) {
  k->log("[gputest] suspenso\n");
  return 0;
}

static int t_resume(const struct gpu_mode* mode) {
  if (!mode || !mode->fb || !mode->back) return -1;
  m = *mode;
  k->log("[gputest] resume\n");
  return t_selftest();
}

static void t_fini(void) {
  if (k) {
    k->log("[gputest] fini, quadros ");
    k->log_hex(quadros);
    k->log("\n");
    k->free(fila);
  }
  fila = 0;
}

static const struct gpu_drv drv = {
  GPU_ABI,
#ifdef GPU_FALHA
  "gpufail (teste)",
#else
  "gputest (teste)",
#endif
  GPU_CAP_PRESENT,
  t_init,
  t_selftest,
  t_present,
  t_fini,
  t_suspend,
  t_resume,
};

const struct gpu_drv* gpu_driver(void) { return &drv; }
