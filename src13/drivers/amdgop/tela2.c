/* tela2.c - Etapa M da RX 570, parte 2: a segunda tela em espelho.
 * Incluido por amdgop.c com AMD_TELA2 (AMDDUAL.DLL). Feito em 2026-11-18.
 *
 * O GOP liga um monitor so (CRTC 0). Aqui o CRTC 1 passa a mostrar a mesma
 * imagem (o mesmo framebuffer da VRAM) no outro conector com monitor, como
 * o Linux faz num modeset do dce_v11_0 (drm helper: crtc_prepare,
 * encoder_prepare, crtc_mode_set, encoder_mode_set, crtc_commit,
 * encoder_commit), rodando as tabelas do AtomBIOS da placa pelo atom.c:
 *
 *   EnableDispPowerGating(1, liga)  UpdateCRTC_DoubleBufferRegisters(1, trava)
 *   EnableCRTC(1, desliga)          SelectCRTC_Source v1.3 (CRTC 1 -> DIGn)
 *   FMT (8 bits, como o CRTC 0)     SetPixelClock v1.7 (PLL do PHY)
 *   SetCRTC_UsingDTDTiming          GRPH/LB/viewport do CRTC 1 (= CRTC 0)
 *   SetCRTC_OverScan, EnableScaler  buffer de linha, DMIF e marcas d'agua
 *   EnableCRTC(1, liga), BlankCRTC(1, mostra), LUT identidade, destrava
 *   DIGxEncoderControl v1.5: STREAM_SETUP, SETUP_PANEL_MODE
 *   UNIPHYTransmitterControl v1.6: INIT, ENABLE
 *
 * Modo: 1920x1080 a 60 Hz da CEA-861 (148,5 MHz, 2200x1125), que todo
 * monitor HDMI/DVI aceita, em modo DVI (TMDS sem infoframes nem audio).
 * DisplayPort precisa do treino do link (atombios_dp.c): ainda nao.
 *
 * So mexe no CRTC 1, no DIG e no PHY da segunda saida e no PLL desse PHY;
 * o CRTC 0, o DIG e o PHY da tela principal nao sao tocados. Qualquer
 * tabela que aborte desliga o que ja foi ligado. */

#include "atom.c"

#define T2_CRTC 1u
#define T2_GRPH_LUT_10BIT_BYPASS 0x1A02u
#define T2_GRPH_SWAP_CNTL 0x1A03u
#define T2_GRPH_SURF2 0x1A05u
#define T2_GRPH_SURF2_HI 0x1A08u
#define T2_GRPH_OFFSET_X 0x1A09u
#define T2_GRPH_OFFSET_Y 0x1A0Au
#define T2_GRPH_X_START 0x1A0Bu
#define T2_GRPH_Y_START 0x1A0Cu
#define T2_GRPH_FLIP_CONTROL 0x1A12u
#define T2_PRESCALE_GRPH_CONTROL 0x1A2Du
#define T2_INPUT_CSC_CONTROL 0x1A35u
#define T2_OUTPUT_CSC_CONTROL 0x1A3Cu
#define T2_INPUT_GAMMA_CONTROL 0x1A10u
#define T2_DENORM_CONTROL 0x1A50u
#define T2_DEGAMMA_CONTROL 0x1A58u
#define T2_GAMUT_REMAP_CONTROL 0x1A59u
#define T2_DC_LUT_RW_MODE 0x1A78u
#define T2_DC_LUT_RW_INDEX 0x1A79u
#define T2_DC_LUT_30_COLOR 0x1A7Cu
#define T2_DC_LUT_WRITE_EN_MASK 0x1A7Eu
#define T2_DC_LUT_CONTROL 0x1A80u
#define T2_DC_LUT_BLACK_BLUE 0x1A81u
#define T2_DC_LUT_WHITE_BLUE 0x1A84u
#define T2_REGAMMA_CONTROL 0x1AA0u
#define T2_ALPHA_CONTROL 0x1ABCu
#define T2_LB_MEMORY_CTRL 0x1AC1u
#define T2_LB_DESKTOP_HEIGHT 0x1AC3u
#define T2_DPG_WATERMARK_MASK 0x1B32u
#define T2_DPG_URGENCY 0x1B33u
#define T2_VIEWPORT_START 0x1B5Cu
#define T2_VIEWPORT_SIZE 0x1B5Du
#define T2_CRTC_STATUS 0x1BA3u
#define T2_CRTC_FRAME_COUNT 0x1BA6u
#define T2_CRTC_MASTER_UPDATE_MODE 0x1BBEu
#define T2_FMT_BIT_DEPTH_CONTROL 0x1BF2u
#define T2_PIPE0_DMIF_BUFFER_CONTROL 0x0321u
#define T2_D1VGA_CONTROL 0x00CCu
#define T2_D2VGA_CONTROL 0x00CEu
#define T2_DIG_FE_CNTL 0x4A00u       /* DIG0; DIG1..5 a cada 0x100, DIG6 em 0x5400 */
#define T2_DIG_BE_CNTL 0x4A47u
#define T2_DIG_BE_EN_CNTL 0x4A48u

/* indices das tabelas de comando (ATOM_MASTER_LIST_OF_COMMAND_TABLES) */
#define AT_DIGxEncoderControl 4
#define AT_SetPixelClock 12
#define AT_EnableDispPowerGating 13
#define AT_EnableScaler 33
#define AT_BlankCRTC 34
#define AT_EnableCRTC 35
#define AT_SetCRTC_OverScan 40
#define AT_SelectCRTC_Source 42
#define AT_UpdateCRTC_DoubleBufferRegisters 44
#define AT_SetCRTC_UsingDTDTiming 49
#define AT_UNIPHYTransmitterControl 76

/* modo da segunda tela: CEA-861 1080p60 */
#define T2_PIX_KHZ 148500u
#define T2_H 1920u
#define T2_H_BLANK 280u
#define T2_H_SYNC_OFF 88u
#define T2_H_SYNC_W 44u
#define T2_V 1080u
#define T2_V_BLANK 45u
#define T2_V_SYNC_OFF 4u
#define T2_V_SYNC_W 5u
#define T2_MODO_DVI 2u
#define T2_BPC_8 2u                  /* PANEL_8BIT_PER_COLOR */

static uint32_t* t2_scratch;
static int t2_ligada;                /* 1 = CRTC 1 e a saida ligados por nos */
static int t2_dig = -1, t2_phy = -1;
static uint32_t t2_conobj;
static uint64_t t2_tsc_ms;           /* ciclos do TSC por ms (calibrado) */
static int t2_estendida;             /* 1 = imagem larga nas duas telas */
static struct gpu_mode t2_m_uma;     /* o modo de uma tela (o do GOP) */
static int t2_m_uma_ok;

static uint32_t t2_dig_reg(uint32_t base, int dig) {
  return dig == 6 ? base + 0x0A00u : base + (uint32_t)dig * 0x100u;
}

static uint32_t t2_rr(struct card_info* c, uint32_t r) {
  (void)c;
  return pl_r(r);
}
static void t2_wr(struct card_info* c, u32 r, uint32_t v) {
  (void)c;
  pl_w(r, v);
}
static uint32_t t2_ms(void) { return k->ms(); }
static void t2_espera_us(uint32_t us) {
#ifdef AMDGOP_SIM
  (void)us;
#else
  uint64_t t0 = tsc(), n;
  if (us > 100000u) us = 100000u;
  n = t2_tsc_ms ? t2_tsc_ms * us / 1000u : (uint64_t)us * 4000u;
  while (tsc() - t0 < n) __asm__ volatile ("pause");
#endif
}
static void t2_msg(const char* s) {
  const char* f = s;
  k->log("[amdtela2] atom: ");
  k->log(s);
  while (*f) f++;
  if (f == s || f[-1] != '\n') k->log("\n");
}

static void t2_calibra(void) {
#ifndef AMDGOP_SIM
  uint32_t t = k->ms(), voltas = 0;
  uint64_t a;
  while (k->ms() == t && ++voltas < 50000000u) { }
  t = k->ms();
  a = tsc();
  voltas = 0;
  while (k->ms() - t < 20u && ++voltas < 500000000u) { }
  if (k->ms() - t >= 20u) t2_tsc_ms = (tsc() - a) / (k->ms() - t);
#endif
}

/* roda e loga; 0 = ok */
static int t2_tab(const char* nome, int idx, void* p, uint32_t n) {
  uint8_t fr = 0, cr = 0;
  int r;
  if (!at_tabela(idx, &fr, &cr)) {
    k->log("[amdtela2] tabela ");
    k->log(nome);
    k->log(" nao existe no VBIOS\n");
    return -1;
  }
  r = at_roda(idx, p, n);
  k->log("[amdtela2] ");
  k->log(nome);
  log_kv(" v", (uint32_t)fr << 8 | cr);
  log_kv(r == 0 ? " ok, instrucoes " : " ABORTOU, instrucoes ", at_ops);
  k->log("\n");
  return r;
}

static int t2_crtc_liga(int liga) {
  uint8_t p[4] = { (uint8_t)T2_CRTC, (uint8_t)liga, 0, 0 };
  return t2_tab(liga ? "EnableCRTC(1, liga)" : "EnableCRTC(1, desliga)", AT_EnableCRTC, p, 4);
}

static int t2_transmissor(uint8_t acao, int hpd, const char* nome) {
  uint8_t p[16] = { 0 };
  uint32_t sym = T2_PIX_KHZ / 10u;
  p[0] = (uint8_t)t2_phy;
  p[1] = acao;
  p[2] = (uint8_t)T2_MODO_DVI;
  p[3] = 4;                                   /* single link: 4 pares */
  p[4] = (uint8_t)sym;
  p[5] = (uint8_t)(sym >> 8);
  p[6] = (uint8_t)(sym >> 16);
  p[8] = (uint8_t)(hpd >= 0 ? hpd + 1 : 0);
  p[9] = (uint8_t)(1u << t2_dig);
  p[10] = (uint8_t)t2_conobj;
  return t2_tab(nome, AT_UNIPHYTransmitterControl, p, 16);
}

/* desliga o que ligou (erro ou fim) */
static void t2_desliga(void) {
  uint8_t b[8] = { (uint8_t)T2_CRTC, 1, 0, 0, 0, 0, 0, 0 };
  if (!t2_ligada) return;
  if (pl_espelho == (int)T2_CRTC) {
    pl_espelho = -1;
    pl_w(PL_CUR_CONTROL + pl_crtc[T2_CRTC], pl_r(PL_CUR_CONTROL + pl_crtc[T2_CRTC]) & ~1u);
  }
  if (t2_phy >= 0) (void)t2_transmissor(0, -1, "UNIPHYTransmitterControl(desliga)");
  (void)t2_tab("BlankCRTC(1, apaga)", AT_BlankCRTC, b, 8);
  (void)t2_crtc_liga(0);
  pl_w(PL_GRPH_ENABLE + pl_crtc[T2_CRTC], 0);
  t2_ligada = 0;
}

/* Copia uma marca d'agua (A = 1, B = 2) do CRTC 0 para o CRTC 1. */
static void t2_marca(uint32_t sel) {
  uint32_t o0 = pl_crtc[0], o1 = pl_crtc[T2_CRTC], m0, m1, v;
  m0 = pl_r(T2_DPG_WATERMARK_MASK + o0);
  m1 = pl_r(T2_DPG_WATERMARK_MASK + o1);
  pl_w(T2_DPG_WATERMARK_MASK + o0, (m0 & ~0x700u) | sel << 8);
  pl_w(T2_DPG_WATERMARK_MASK + o1, (m1 & ~0x700u) | sel << 8);
  v = pl_r(T2_DPG_URGENCY + o0);
  pl_w(T2_DPG_URGENCY + o1, v);
  pl_w(T2_DPG_WATERMARK_MASK + o0, m0);
  pl_w(T2_DPG_WATERMARK_MASK + o1, m1);
}

static int t2_modeset(int dig, int phy, uint32_t enc_obj, int hpd) {
  uint32_t o0 = pl_crtc[0], o1 = pl_crtc[T2_CRTC], i, v, f0;
  uint8_t p[24];
  static const uint8_t asic_dig[7] = { 0x03, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E };
  uint32_t uniphy = enc_obj & 0xFFu, linkb = ((enc_obj >> 8) & 7u) == 2u, pll;
  /* PLL: dce_v11_0_pick_pll da Polaris fora do DP: COMBOPHY_PLL0..5 */
  if (uniphy == 0x1Eu) pll = 20u + linkb;
  else if (uniphy == 0x20u) pll = 22u + linkb;
  else if (uniphy == 0x21u) pll = 24u + linkb;
  else return -1;
  t2_dig = dig;
  t2_phy = phy;
  t2_ligada = 1;

  /* crtc_prepare */
  p[0] = (uint8_t)T2_CRTC; p[1] = 0; p[2] = p[3] = 0;
  if (t2_tab("EnableDispPowerGating(1, liga a energia)", AT_EnableDispPowerGating, p, 4) == -2) return -2;
  p[0] = (uint8_t)T2_CRTC; p[1] = 1;
  if (t2_tab("UpdateCRTC_DoubleBufferRegisters(1, trava)", AT_UpdateCRTC_DoubleBufferRegisters, p, 4)) return -2;
  if (t2_crtc_liga(0)) return -2;

  /* encoder_prepare: CRTC 1 -> DIGn; FMT como o CRTC 0 (8 bits) */
  p[0] = (uint8_t)T2_CRTC; p[1] = asic_dig[dig]; p[2] = (uint8_t)T2_MODO_DVI; p[3] = (uint8_t)T2_BPC_8;
  if (t2_tab("SelectCRTC_Source v3 (CRTC 1 -> DIG)", AT_SelectCRTC_Source, p, 4)) return -2;
  pl_w(T2_FMT_BIT_DEPTH_CONTROL + o1, pl_r(T2_FMT_BIT_DEPTH_CONTROL + o0));

  /* crtc_mode_set: relogio, temporizacao, superficie, overscan, escala */
  for (i = 0; i < 16; i++) p[i] = 0;
  v = T2_PIX_KHZ * 10u;                       /* unidades de 100 Hz */
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
  p[4] = (uint8_t)pll;
  p[5] = (uint8_t)uniphy;
  p[6] = (uint8_t)T2_MODO_DVI;
  p[8] = (uint8_t)T2_CRTC;
  if (t2_tab("SetPixelClock v7 (148,5 MHz)", AT_SetPixelClock, p, 16)) return -2;
  {
    uint16_t d[12] = { T2_H, T2_H_BLANK, T2_V, T2_V_BLANK, T2_H_SYNC_OFF, T2_H_SYNC_W,
                       T2_V_SYNC_OFF, T2_V_SYNC_W, 0, 0, T2_CRTC, 0 };
    if (t2_tab("SetCRTC_UsingDTDTiming (1920x1080)", AT_SetCRTC_UsingDTDTiming, d, 24)) return -2;
  }
  /* do_set_base: a mesma superficie do CRTC 0 */
  pl_w(T2_D2VGA_CONTROL, pl_r(T2_D2VGA_CONTROL) & ~1u);
  pl_w(T2_GRPH_FLIP_CONTROL + o1, pl_r(T2_GRPH_FLIP_CONTROL + o1) & ~1u);
  pl_w(PL_GRPH_SURF_HI + o1, pl_r(PL_GRPH_SURF_HI + o0));
  pl_w(T2_GRPH_SURF2_HI + o1, pl_r(PL_GRPH_SURF_HI + o0));
  pl_w(PL_GRPH_SURF + o1, pl_r(PL_GRPH_SURF + o0));
  pl_w(T2_GRPH_SURF2 + o1, pl_r(PL_GRPH_SURF + o0));
  pl_w(PL_GRPH_CONTROL + o1, pl_r(PL_GRPH_CONTROL + o0));
  pl_w(T2_GRPH_SWAP_CNTL + o1, pl_r(T2_GRPH_SWAP_CNTL + o0));
  pl_w(T2_GRPH_LUT_10BIT_BYPASS + o1, pl_r(T2_GRPH_LUT_10BIT_BYPASS + o0));
  pl_w(T2_GRPH_OFFSET_X + o1, 0);
  pl_w(T2_GRPH_OFFSET_Y + o1, 0);
  pl_w(T2_GRPH_X_START + o1, 0);
  pl_w(T2_GRPH_Y_START + o1, 0);
  pl_w(PL_GRPH_X_END + o1, m.w);
  pl_w(PL_GRPH_Y_END + o1, m.h);
  pl_w(PL_GRPH_PITCH + o1, pl_r(PL_GRPH_PITCH + o0));
  pl_w(PL_GRPH_ENABLE + o1, 1);
  pl_w(T2_LB_DESKTOP_HEIGHT + o1, m.h);
  pl_w(T2_VIEWPORT_START + o1, 0);
  pl_w(T2_VIEWPORT_SIZE + o1, (m.w < T2_H ? m.w : T2_H) << 16 | (((m.h < T2_V ? m.h : T2_V) + 1u) & ~1u));
  pl_w(T2_CRTC_MASTER_UPDATE_MODE + o1, 0);
  for (i = 0; i < 12; i++) p[i] = 0;
  p[8] = (uint8_t)T2_CRTC;
  (void)t2_tab("SetCRTC_OverScan(1, 0)", AT_SetCRTC_OverScan, p, 12);
  p[0] = (uint8_t)T2_CRTC; p[1] = 0; p[2] = p[3] = 0;
  (void)t2_tab("EnableScaler(1, desliga)", AT_EnableScaler, p, 4);
  /* buffer de linha e DMIF (dce_v11_0_line_buffer_adjust, < 2560: 2 e 2) */
  pl_w(T2_LB_MEMORY_CTRL + o1, (pl_r(T2_LB_MEMORY_CTRL + o1) & ~0x300000u) | 2u << 20);
  pl_w(T2_PIPE0_DMIF_BUFFER_CONTROL + T2_CRTC, (pl_r(T2_PIPE0_DMIF_BUFFER_CONTROL + T2_CRTC) & ~7u) | 2u);
  if (pl_espera(0, T2_PIPE0_DMIF_BUFFER_CONTROL + T2_CRTC, 0x10u, 0x10u, 0, 10))
    k->log("[amdtela2] DMIF do CRTC 1 nao confirmou (segue)\n");
  t2_marca(1);
  t2_marca(2);

  /* crtc_commit: liga, mostra, LUT identidade (load_lut), destrava */
  if (t2_crtc_liga(1)) return -2;
  for (i = 0; i < 8; i++) p[i] = 0;
  p[0] = (uint8_t)T2_CRTC;
  if (t2_tab("BlankCRTC(1, mostra)", AT_BlankCRTC, p, 8)) return -2;
  pl_w(T2_INPUT_CSC_CONTROL + o1, pl_r(T2_INPUT_CSC_CONTROL + o1) & ~3u);
  pl_w(T2_PRESCALE_GRPH_CONTROL + o1, pl_r(T2_PRESCALE_GRPH_CONTROL + o1) | 0x10u);
  pl_w(T2_INPUT_GAMMA_CONTROL + o1, pl_r(T2_INPUT_GAMMA_CONTROL + o1) & ~3u);
  pl_w(T2_DC_LUT_CONTROL + o1, 0);
  for (i = 0; i < 3; i++) {
    pl_w(T2_DC_LUT_BLACK_BLUE + i + o1, 0);
    pl_w(T2_DC_LUT_WHITE_BLUE + i + o1, 0xFFFFu);
  }
  pl_w(T2_DC_LUT_RW_MODE + o1, 0);
  pl_w(T2_DC_LUT_WRITE_EN_MASK + o1, 7);
  pl_w(T2_DC_LUT_RW_INDEX + o1, 0);
  for (i = 0; i < 256; i++) {
    uint32_t c = i << 2 | i >> 6;           /* 8 -> 10 bits */
    pl_w(T2_DC_LUT_30_COLOR + o1, c << 20 | c << 10 | c);
  }
  pl_w(T2_DEGAMMA_CONTROL + o1, pl_r(T2_DEGAMMA_CONTROL + o1) & ~0x333u);
  pl_w(T2_GAMUT_REMAP_CONTROL + o1, pl_r(T2_GAMUT_REMAP_CONTROL + o1) & ~3u);
  pl_w(T2_REGAMMA_CONTROL + o1, pl_r(T2_REGAMMA_CONTROL + o1) & ~7u);
  pl_w(T2_OUTPUT_CSC_CONTROL + o1, pl_r(T2_OUTPUT_CSC_CONTROL + o1) & ~7u);
  pl_w(T2_DENORM_CONTROL + o1, 0);
  pl_w(T2_ALPHA_CONTROL + o1, pl_r(T2_ALPHA_CONTROL + o1) | 2u);
  p[0] = (uint8_t)T2_CRTC; p[1] = 0; p[2] = p[3] = 0;
  if (t2_tab("UpdateCRTC_DoubleBufferRegisters(1, destrava)", AT_UpdateCRTC_DoubleBufferRegisters, p, 4))
    return -2;

  /* encoder_commit: setup_dig(ATOM_ENABLE) para TMDS */
  for (i = 0; i < 12; i++) p[i] = 0;
  v = T2_PIX_KHZ / 10u;
  p[0] = (uint8_t)dig; p[1] = 0x0F; p[2] = (uint8_t)T2_MODO_DVI; p[3] = 4;
  p[4] = (uint8_t)v; p[5] = (uint8_t)(v >> 8); p[6] = (uint8_t)(v >> 16);
  p[8] = (uint8_t)T2_BPC_8;
  if (t2_tab("DIGxEncoderControl v5 STREAM_SETUP", AT_DIGxEncoderControl, p, 12)) return -2;
  for (i = 0; i < 12; i++) p[i] = 0;
  p[0] = (uint8_t)dig; p[1] = 0x10;
  (void)t2_tab("DIGxEncoderControl v5 SETUP_PANEL_MODE", AT_DIGxEncoderControl, p, 12);
  if (t2_transmissor(7, hpd, "UNIPHYTransmitterControl v6 INIT")) return -2;
  if (t2_transmissor(1, hpd, "UNIPHYTransmitterControl v6 ENABLE")) return -2;

  /* conferencia: o CRTC 1 varre? (contador de quadros anda) */
  f0 = pl_r(T2_CRTC_FRAME_COUNT + o1);
  (void)pl_espera(0, T2_CRTC_FRAME_COUNT + o1, 0xFFFFFFu, f0 & 0xFFFFFFu, 1, 100);
  log_kv("[amdtela2] CRTC 1: ctl ", pl_r(PL_CRTC_CONTROL + o1));
  log_kv(" quadros ", f0);
  log_kv(" -> ", pl_r(T2_CRTC_FRAME_COUNT + o1));
  log_kv(" DIG fe ", pl_r(t2_dig_reg(T2_DIG_FE_CNTL, dig)));
  log_kv(" be ", pl_r(t2_dig_reg(T2_DIG_BE_CNTL, dig)));
  log_kv(" be_en ", pl_r(t2_dig_reg(T2_DIG_BE_EN_CNTL, dig)));
  k->log("\n");
  return 0;
}

/* codificador do caminho -> DIG (dce_v11_0_pick_dig_encoder) e PHY */
static int t2_dig_de(uint32_t enc, int* phy) {
  uint32_t id = enc & 0xFFu, b = ((enc >> 8) & 7u) == 2u;
  if (id == 0x1Eu) { *phy = (int)b; return (int)b; }            /* UNIPHY: A/B */
  if (id == 0x20u) { *phy = 2 + (int)b; return 2 + (int)b; }    /* UNIPHY1: C/D */
  if (id == 0x21u) { *phy = 4 + (int)b; return 4 + (int)b; }    /* UNIPHY2: E/F */
  if (id == 0x25u) { *phy = 6; return 6; }                      /* UNIPHY3: G */
  return -1;
}

static void polaris_tela2(void) {
  int d, prim = -1, sec = -1, phy = -1, r;
  uint32_t j;
  if (!vb || !mmio || tela_crtc != 0) {
    k->log("[amdtela2] sem VBIOS ou a tela nao esta no CRTC 0: nada a fazer\n");
    return;
  }
  /* qual DIG mostra o CRTC 0 (o GOP) */
  for (d = 0; d < 7; d++) {
    uint32_t fe = pl_r(t2_dig_reg(T2_DIG_FE_CNTL, d)), en = pl_r(t2_dig_reg(T2_DIG_BE_EN_CNTL, d));
    uint32_t be = pl_r(t2_dig_reg(T2_DIG_BE_CNTL, d));
    log_kv("[amdtela2] DIG ", (uint32_t)d);
    log_kv(" fe ", fe);
    log_kv(" be ", be);
    log_kv(" be_en ", en);
    if ((en & 1u) && (fe & 7u) == 0 && (fe & 0x1000000u)) {
      if (prim < 0) prim = d;
      k->log(" <- tela principal (CRTC 0)");
    }
    k->log("\n");
  }
  for (j = 0; j < vb_ncon; j++) {
    int p2 = -1, dj = t2_dig_de(vb_con[j].enc, &p2);
    log_kv("[amdtela2] conector ", j);
    log_kv(" id ", vb_con[j].id);
    log_kv(" dig ", (uint32_t)dj);
    log_kv(" hpd ", (uint32_t)vb_con[j].hpd);
    log_kv(" monitor ", (uint32_t)vb_con[j].nivel);
    k->log("\n");
    if (vb_con[j].nivel == 1 && dj >= 0 && dj != prim && sec < 0) {
      sec = (int)j;
      phy = p2;
    }
  }
  if (prim < 0) {
    k->log("[amdtela2] nenhum DIG ligado ao CRTC 0: nao sei qual e a tela principal\n");
    return;
  }
  if (sec < 0) {
    k->log("[amdtela2] nenhum outro conector com monitor: so uma tela\n");
    return;
  }
  if ((vb_con[sec].id & 0xFFu) == 0x13u || (vb_con[sec].id & 0xFFu) == 0x14u) {
    k->log("[amdtela2] a segunda tela esta num DisplayPort: falta o treino do link (proxima etapa)\n");
    return;
  }
  t2_scratch = (uint32_t*)k->alloc(20u * 1024u, 16);
  if (!t2_scratch) return;
  for (j = 0; j < 20u * 1024u / 4u; j++) t2_scratch[j] = 0;
  if (at_ini(vb, vb_tam, t2_scratch, 20 * 1024, t2_rr, t2_wr) != 0) {
    k->log("[amdtela2] atom: imagem recusada\n");
    return;
  }
  at_relogio = t2_ms;
  at_espera_us = t2_espera_us;
  at_msg = t2_msg;
  t2_calibra();
  log_kv("[amdtela2] TSC por ms ", (uint32_t)t2_tsc_ms);
  t2_m_uma = m;          /* o modo do GOP, para voltar a uma tela */
  t2_m_uma_ok = 1;
  t2_conobj = vb_con[sec].id & 0xFFu;
  log_kv(" segunda tela: conector ", (uint32_t)sec);
  log_kv(" dig ", (uint32_t)t2_dig_de(vb_con[sec].enc, &d));
  log_kv(" phy ", (uint32_t)phy);
  k->log("\n");
  r = t2_modeset(t2_dig_de(vb_con[sec].enc, &d), phy, vb_con[sec].enc, vb_con[sec].hpd);
  if (r) {
    log_kv("[amdtela2] falhou (", (uint32_t)r);
    k->log("): desligando o CRTC 1 e a saida\n");
    t2_desliga();
  } else {
    k->log("[amdtela2] segunda tela LIGADA em espelho (CRTC 1)\n");
    pl_espelho = (int)T2_CRTC;   /* o cursor da placa vai nas duas telas */
  }
}

static void t2_layout(int larga);

static void polaris_tela2_fim(void) {
  if (t2_estendida) t2_layout(0);   /* o console de texto usa o fb do GOP */
  if (t2_ligada) {
    t2_desliga();
    k->log("[amdtela2] segunda tela desligada (fim)\n");
  }
  if (t2_scratch) {
    k->free(t2_scratch);
    t2_scratch = 0;
  }
}

/* ---------------------------------------------------------------------------
 * Tela estendida (2026-11-18). As duas telas leem a mesma imagem larga na
 * VRAM, comecando no endereco do GOP (VRAM 0): 2*w x h, pitch 2*w pixels
 * (3840x1080 = 16.588.800 bytes, abaixo do cursor em 16 MB). O CRTC 0 mostra
 * a metade esquerda (viewport em 0) e o CRTC 1 a direita (viewport em w). A
 * posicao do cursor e na imagem (como o dce_v11_0_cursor_move_locked, que
 * soma crtc->x): a mesma nos dois CRTCs, cada um mostra se cair nele.
 * ------------------------------------------------------------------------- */

/* pitch e viewports: larga = 1 -> imagem larga; 0 -> a do GOP */
static void t2_layout(int larga) {
  uint32_t w = t2_m_uma.w, h = t2_m_uma.h, o0 = pl_crtc[0], o1 = pl_crtc[T2_CRTC];
  uint32_t pitch = larga ? 2u * w : t2_m_uma.fb_pitch / 4u, vw = w < T2_H ? w : T2_H;
  uint32_t vh = ((h < T2_V ? h : T2_V) + 1u) & ~1u;
  pl_w(PL_GRPH_PITCH + o0, pitch);
  pl_w(PL_GRPH_X_END + o0, larga ? 2u * w : w);
  pl_w(T2_VIEWPORT_START + o0, 0);
  if (t2_ligada) {
    pl_w(PL_GRPH_PITCH + o1, pitch);
    pl_w(PL_GRPH_X_END + o1, larga ? 2u * w : w);
    pl_w(T2_VIEWPORT_START + o1, larga ? w << 16 : 0);
    pl_w(T2_VIEWPORT_SIZE + o1, vw << 16 | vh);
  }
  t2_estendida = larga;
}

static int a_telas(int op, struct gpu_telas* t, const struct gpu_mode* mode) {
  uint32_t w, h;
  if (!t || !t2_m_uma_ok) return -1;
  w = t2_m_uma.w;
  h = t2_m_uma.h;
  if (op == GPU_TELAS_CONSULTA) {
    t->n = t2_ligada ? 2u : 1u;
    t->w = 2u * w;
    t->h = h;
    t->x2 = w;
    t->fb = t2_m_uma.fb;
    t->fb_pitch = 2u * w * 4u;
    /* a imagem larga tem de caber antes do cursor (CUR_VRAM_OFF) */
    if (!t2_ligada || (uint64_t)t->fb_pitch * h > 0x01000000u) return -1;
    return 0;
  }
  if (op == GPU_TELAS_ESTENDE) {
    if (!t2_ligada || !mode || mode->w != 2u * w || mode->h != h || mode->fb_pitch != 2u * w * 4u ||
        !mode->back || !mode->fb)
      return -1;
    m = *mode;
    t2_layout(1);
    k->log("[amdtela2] tela ESTENDIDA: ");
    log_kv("", m.w);
    log_kv("x", m.h);
    log_kv(", segunda tela em x ", w);
    k->log("\n");
    return 0;
  }
  if (op == GPU_TELAS_UMA) {
    if (t2_estendida) {
      t2_layout(0);
      k->log("[amdtela2] volta a uma tela (espelho)\n");
    }
    if (mode) m = *mode;
    return 0;
  }
  return -1;
}
