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

/* ---------------------------------------------------------------------------
 * DCN 2.1 (Renoir/Barcelo): registradores no BAR5, endereco em bytes =
 * (base do segmento + registrador) * 4. Offsets de dcn_2_1_0_offset.h do
 * Linux; o DCN usa o segmento 2 (DCN_BASE__INST0_SEG2 = 0x34C0).
 * Etapa A do cursor da placa: so LER e registrar no GPU.LOG.
 * ------------------------------------------------------------------------- */
#define AMD_BARCELO 0x100215E7u
#define DCN_SEG2 0x34C0u
#define PIPE_HUBP 0xDCu                /* passo entre HUBP0, HUBP1, ... */
#define PIPE_CNVC 0x16Bu               /* passo entre CNVC_CUR0, CUR1, ... */
#define R_DCN_VM_FB_LOCATION_BASE 0x0493u
#define R_DCN_VM_FB_LOCATION_TOP 0x0494u
#define R_DCN_VM_FB_OFFSET 0x0495u
#define R_DCN_VM_AGP_BOT 0x0496u
#define R_DCN_VM_AGP_TOP 0x0497u
#define R_DCN_VM_AGP_BASE 0x0498u
#define R_HUBP_DCHUBP_CNTL 0x05F3u
#define R_HUBPREQ_SURFACE_PITCH 0x0607u
#define R_HUBPREQ_PRIMARY_ADDR 0x060Au
#define R_HUBPREQ_PRIMARY_ADDR_HI 0x060Bu
#define R_HUBPREQ_CURSOR_SETTINGS 0x065Eu
#define R_CURSOR_CONTROL 0x0678u
#define R_CURSOR_ADDR 0x0679u
#define R_CURSOR_ADDR_HI 0x067Au
#define R_CURSOR_SIZE 0x067Bu
#define R_CURSOR_POSITION 0x067Cu
#define R_CURSOR_HOT_SPOT 0x067Du
#define R_CURSOR_DST_OFFSET 0x067Fu
#define R_CNVC_CUR_CONTROL 0x0CE0u
#define R_CNVC_CUR_COLOR0 0x0CE1u
#define R_CNVC_CUR_COLOR1 0x0CE2u

static volatile uint8_t* mmio;   /* BAR5 */
/* pipe que mostra a tela (superficie != 0 e pitch == largura); -1 = nenhum */
static int tela_pipe = -1;
static uint64_t surf_gpu;        /* endereco GPU do framebuffer GOP */
static uint64_t fb_lo, fb_hi;    /* faixa da memoria de video vista pelo DCN */

static uint32_t dcn_r(uint32_t reg) {
  return *(volatile uint32_t*)(mmio + (DCN_SEG2 + reg) * 4u);
}

static void dcn_w(uint32_t reg, uint32_t v) {
  *(volatile uint32_t*)(mmio + (DCN_SEG2 + reg) * 4u) = v;
}

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

/* So leitura: o estado do DCN que o firmware (GOP) deixou. */
static void dcn_diag(void) {
  uint32_t bar5 = k->pci_read32(m.pci_bus, m.pci_dev, m.pci_fun, 0x24), p;
  /* BAR5 de memoria e 32 bits (no notebook: 0xFCC00000) */
  if ((bar5 & 1u) || ((bar5 >> 1) & 3u) == 2u || (bar5 & ~0xFu) == 0) {
    log_kv("[amdgop] BAR5 inesperado ", bar5);
    k->log(", DCN nao lido\n");
    return;
  }
  mmio = (volatile uint8_t*)(uintptr_t)(bar5 & ~0xFu);
  fb_lo = (uint64_t)(dcn_r(R_DCN_VM_FB_LOCATION_BASE) & 0xFFFFFFu) << 24;
  fb_hi = (uint64_t)(dcn_r(R_DCN_VM_FB_LOCATION_TOP) & 0xFFFFFFu) << 24 | 0xFFFFFFu;
  tela_pipe = -1;
  log_kv("[amdgop] dcn fb_base ", dcn_r(R_DCN_VM_FB_LOCATION_BASE));
  log_kv(" fb_top ", dcn_r(R_DCN_VM_FB_LOCATION_TOP));
  log_kv(" fb_offset ", dcn_r(R_DCN_VM_FB_OFFSET));
  log_kv("\n[amdgop] dcn agp_bot ", dcn_r(R_DCN_VM_AGP_BOT));
  log_kv(" agp_top ", dcn_r(R_DCN_VM_AGP_TOP));
  log_kv(" agp_base ", dcn_r(R_DCN_VM_AGP_BASE));
  k->log("\n");
  for (p = 0; p < 4; p++) {
    uint32_t h = p * PIPE_HUBP, c = p * PIPE_CNVC;
    uint64_t surf = (uint64_t)dcn_r(R_HUBPREQ_PRIMARY_ADDR_HI + h) << 32 | dcn_r(R_HUBPREQ_PRIMARY_ADDR + h);
    if (tela_pipe < 0 && surf && dcn_r(R_HUBPREQ_SURFACE_PITCH + h) == m.w) {
      tela_pipe = (int)p;
      surf_gpu = surf;
    }
    log_kv("[amdgop] pipe ", p);
    log_kv(" hubp_cntl ", dcn_r(R_HUBP_DCHUBP_CNTL + h));
    log_kv(" surf ", dcn_r(R_HUBPREQ_PRIMARY_ADDR_HI + h));
    log_kv(":", dcn_r(R_HUBPREQ_PRIMARY_ADDR + h));
    log_kv(" pitch ", dcn_r(R_HUBPREQ_SURFACE_PITCH + h));
    log_kv("\n[amdgop]   cur_ctl ", dcn_r(R_CURSOR_CONTROL + h));
    log_kv(" cur_addr ", dcn_r(R_CURSOR_ADDR_HI + h));
    log_kv(":", dcn_r(R_CURSOR_ADDR + h));
    log_kv(" size ", dcn_r(R_CURSOR_SIZE + h));
    log_kv(" pos ", dcn_r(R_CURSOR_POSITION + h));
    log_kv(" hot ", dcn_r(R_CURSOR_HOT_SPOT + h));
    log_kv(" dst ", dcn_r(R_CURSOR_DST_OFFSET + h));
    log_kv(" settings ", dcn_r(R_HUBPREQ_CURSOR_SETTINGS + h));
    log_kv("\n[amdgop]   cnvc_cur ", dcn_r(R_CNVC_CUR_CONTROL + c));
    log_kv(" color0 ", dcn_r(R_CNVC_CUR_COLOR0 + c));
    log_kv(" color1 ", dcn_r(R_CNVC_CUR_COLOR1 + c));
    k->log("\n");
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
  if (m.pci_id == AMD_BARCELO) dcn_diag();
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

#ifdef AMD_CURSOR
/* ---------------------------------------------------------------------------
 * Etapa B: cursor do DCN no pipe da tela (AMDCUR.DLL, -DAMD_CURSOR).
 * Sequencia de hubp2_cursor_set_attributes, hubp1_cursor_set_position e
 * dpp2_set_cursor_attributes do amdgpu (Linux 6.12). A imagem 64x64 ARGB
 * fica 16 MB adiante do inicio da VRAM (fora do framebuffer GOP); os 16 KB
 * que estavam la sao guardados e devolvidos no fim.
 * ------------------------------------------------------------------------- */
#define CUR_LADO 64u
#define CUR_VRAM_OFF 0x01000000u
#define CUR_BYTES (CUR_LADO * CUR_LADO * 4u)
#define CUR_MODE_PREMULT 2u   /* CURSOR_MODE_COLOR_PRE_MULTIPLIED_ALPHA */
#define CUR_LPC_8 3u          /* CURSOR_LINE_PER_CHUNK_8: largura 33..64 */
#define CUR_PITCH_64 0u

static uint32_t* cur_salvo;   /* o que havia na VRAM onde a imagem fica */
static int cur_ligado;

static uint32_t hreg(uint32_t r) { return r + (uint32_t)tela_pipe * PIPE_HUBP; }
static uint32_t creg(uint32_t r) { return r + (uint32_t)tela_pipe * PIPE_CNVC; }

static void cur_desliga(void) {
  if (!cur_ligado || tela_pipe < 0) return;
  dcn_w(hreg(R_CURSOR_CONTROL), dcn_r(hreg(R_CURSOR_CONTROL)) & ~1u);
  dcn_w(creg(R_CNVC_CUR_CONTROL), dcn_r(creg(R_CNVC_CUR_CONTROL)) & ~1u);
  cur_ligado = 0;
}

static int a_cursor_set(const uint32_t* argb, int w, int h, int hot_x, int hot_y) {
  volatile uint32_t* img = (volatile uint32_t*)(m.fb + CUR_VRAM_OFF);
  uint64_t addr = surf_gpu + CUR_VRAM_OFF;
  uint32_t i, x, y, v;
  if (tela_pipe < 0 || !mmio || !argb || w < 1 || h < 1 || w > (int)CUR_LADO || h > (int)CUR_LADO) {
    k->log("[amdcur] sem pipe da tela: cursor fica com a CPU\n");
    return -1;
  }
  if (addr + CUR_BYTES - 1u > fb_hi || addr < fb_lo || surf_gpu < fb_lo) {
    log_kv("[amdcur] endereco fora da VRAM do DCN ", (uint32_t)(addr >> 32));
    log_kv(":", (uint32_t)addr);
    k->log("\n");
    return -1;
  }
  if (!cur_salvo) {
    cur_salvo = (uint32_t*)k->alloc(CUR_BYTES, 64);
    if (!cur_salvo) return -1;
    for (i = 0; i < CUR_LADO * CUR_LADO; i++) cur_salvo[i] = img[i];
  }
  for (y = 0; y < CUR_LADO; y++)
    for (x = 0; x < CUR_LADO; x++)
      img[y * CUR_LADO + x] = (int)x < w && (int)y < h ? argb[y * (uint32_t)w + x] : 0u;
  cur_desliga();
  dcn_w(hreg(R_CURSOR_ADDR_HI), (uint32_t)(addr >> 32));
  dcn_w(hreg(R_CURSOR_ADDR), (uint32_t)addr);
  dcn_w(hreg(R_CURSOR_SIZE), CUR_LADO << 16 | CUR_LADO);
  v = dcn_r(hreg(R_CURSOR_CONTROL));
  v &= ~(1u | 1u << 4 | 7u << 8 | 3u << 16 | 0x1Fu << 24);   /* ENABLE 2X MODE PITCH LPC */
  v |= CUR_MODE_PREMULT << 8 | CUR_PITCH_64 << 16 | CUR_LPC_8 << 24;
  dcn_w(hreg(R_CURSOR_CONTROL), v);
  dcn_w(hreg(R_HUBPREQ_CURSOR_SETTINGS), 3u << 8);           /* CHUNK_HDL_ADJUST 3 */
  v = dcn_r(creg(R_CNVC_CUR_CONTROL));
  v &= ~(1u | 1u << 1 | 1u << 3 | 7u << 4);                  /* ENABLE EXP ROM MODE */
  v |= CUR_MODE_PREMULT << 4;
  dcn_w(creg(R_CNVC_CUR_CONTROL), v);
  dcn_w(hreg(R_CURSOR_HOT_SPOT), (uint32_t)hot_x << 16 | (uint32_t)hot_y);
  dcn_w(hreg(R_CURSOR_DST_OFFSET), 0);
  /* liga: HUBP e DPP */
  dcn_w(hreg(R_CURSOR_CONTROL), dcn_r(hreg(R_CURSOR_CONTROL)) | 1u);
  dcn_w(creg(R_CNVC_CUR_CONTROL), dcn_r(creg(R_CNVC_CUR_CONTROL)) | 1u);
  cur_ligado = 1;
  log_kv("[amdcur] pipe ", (uint32_t)tela_pipe);
  log_kv(" imagem gpu ", (uint32_t)(addr >> 32));
  log_kv(":", (uint32_t)addr);
  log_kv(" cur_ctl ", dcn_r(hreg(R_CURSOR_CONTROL)));
  log_kv(" cnvc ", dcn_r(creg(R_CNVC_CUR_CONTROL)));
  log_kv(" size ", dcn_r(hreg(R_CURSOR_SIZE)));
  k->log("\n");
  return 0;
}

static int a_cursor_move(int x, int y) {
  if (!cur_ligado) return -1;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  dcn_w(hreg(R_CURSOR_POSITION), (uint32_t)x << 16 | (uint32_t)y);
  return 0;
}
#endif

/* GOP: no modo texto o fbcon escreve no mesmo framebuffer; nada a parar.
 * Com cursor da placa, ele e desligado (o resume liga de novo). */
static int a_suspend(void) {
#ifdef AMD_CURSOR
  cur_desliga();
#endif
  return 0;
}

static int a_resume(const struct gpu_mode* mode) {
  if (!mode || !mode->fb || !mode->back) return -1;
  m = *mode;
  return 0;
}

static void a_fini(void) {
  if (!k) return;
#ifdef AMD_CURSOR
  cur_desliga();
  if (cur_salvo) {
    volatile uint32_t* img = (volatile uint32_t*)(m.fb + CUR_VRAM_OFF);
    uint32_t i;
    for (i = 0; i < CUR_LADO * CUR_LADO; i++) img[i] = cur_salvo[i];
    k->free(cur_salvo);
    cur_salvo = 0;
  }
#endif
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
#ifdef AMD_CURSOR
  "amdcur (Radeon, GOP, cursor DCN)",
  GPU_CAP_PRESENT | GPU_CAP_DAMAGE | GPU_CAP_CURSOR,
#else
  "amdgop (Radeon, GOP)",
  GPU_CAP_PRESENT | GPU_CAP_DAMAGE,
#endif
  a_init,
  a_selftest,
  a_present,
  a_fini,
  a_suspend,
  a_resume,
#ifdef AMD_CURSOR
  a_cursor_set,
  a_cursor_move,
#else
  0,
  0,
#endif
};

const struct gpu_drv* gpu_driver(void) { return &drv; }
