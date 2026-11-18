/*
 * amdgop.c - AMDGOP.DLL: driver de exibicao para placas Radeon sobre o
 * framebuffer GOP. Contrato em docs/GPU.md; a placa entra pelo SODOS.CFG
 * ([video] placa=...). Placas com diagnostico proprio:
 *   - AMD Barcelo (PCI 1002:15E7, notebook, Ryzen 7 5825U): DCN 2.1;
 *   - Radeon RX 570 (PCI 1002:67DF, desktop, Polaris 10): DCE 11.2, GMC 8,
 *     GFX 8 (so leitura, etapa A da RX 570 em docs/GPU.md).
 *
 * Nao liga o motor da GPU: isso exige firmware (PSP/SMU) e e o grosso do
 * amdgpu do Linux. Usa o framebuffer que o UEFI (GOP) ja deixou na tela e
 * faz o que a CPU faz hoje, com uma diferenca: copia para a tela so as areas
 * que o kernel diz que mudaram, em vez do quadro inteiro.
 *
 * Mede tudo e escreve no log (que vai para X:\LOG\GPU.LOG): ciclos de uma
 * copia inteira (o caminho por CPU de hoje), e por quadro os ciclos e os
 * pixels copiados.
 *
 * Roda no contexto do kernel: gcc -m64 -mno-sse -mno-red-zone, sem libc.
 */
#include <stdint.h>
#include "gpu.h"

/* AMDSMC.DLL (-DAMD_SMC): o AMDCUR.DLL mais a etapa C2 da RX 570 (carrega o
 * firmware do SMU). AMDGOP_SIM: teste no host (drivers/amdgop/smcsim.c), os
 * registradores da Polaris passam por sim_r/sim_w em vez do BAR5. */
#ifdef AMD_SMC
#ifndef AMD_CURSOR
#define AMD_CURSOR
#endif
#endif
#ifdef AMDGOP_SIM
uint32_t sim_r(uint32_t reg);
void sim_w(uint32_t reg, uint32_t v);
#endif

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

/* ---------------------------------------------------------------------------
 * Polaris 10 (RX 470/480/570/580, PCI 1002:67DF): registradores no BAR5,
 * endereco em bytes = registrador * 4 (sem segmentos, como no VI do Linux).
 * Offsets do Linux 6.12: dce_11_2_d.h, gmc_8_1_d.h, gfx_8_0_d.h,
 * bif_5_0_d.h; passo entre CRTCs de amdgpu/vid.h. Os registradores GRPH_* e
 * CUR_* de cada CRTC usam o mesmo passo (dce_v11_0.c).
 * Etapa A da RX 570: so LER e registrar no GPU.LOG. Nada aqui escreve na placa.
 * ------------------------------------------------------------------------- */
#define AMD_POLARIS10 0x100267DFu
#define PL_NCRTC 6
static const uint32_t pl_crtc[PL_NCRTC] = { 0x0000u, 0x0200u, 0x0400u, 0x2600u, 0x2800u, 0x2A00u };
/* BIF / MC (gmc 8.1) */
#define PL_CONFIG_MEMSIZE 0x150Au
#define PL_MC_VM_FB_LOCATION 0x0809u
#define PL_MC_VM_AGP_TOP 0x080Au
#define PL_MC_VM_AGP_BOT 0x080Bu
#define PL_MC_VM_AGP_BASE 0x080Cu
#define PL_MC_VM_SYS_LOW 0x080Du
#define PL_MC_VM_SYS_HIGH 0x080Eu
#define PL_MC_VM_MX_L1_TLB_CNTL 0x0819u
#define PL_MC_VM_FB_OFFSET 0x081Au
#define PL_VM_CONTEXT0_CNTL 0x0504u
#define PL_MC_ARB_RAMCFG 0x09D8u
#define PL_MC_SEQ_MISC0 0x0A80u
/* GFX 8 */
#define PL_GRBM_STATUS2 0x2002u
#define PL_GRBM_STATUS 0x2004u
#define PL_CP_MEC_CNTL 0x208Du
#define PL_CP_ME_CNTL 0x21B6u
#define PL_GB_ADDR_CONFIG 0x263Eu
#define PL_CP_RB0_CNTL 0x3041u
#define PL_RLC_CNTL 0xEC00u
/* SMU 7 */
#define PL_SMC_MESSAGE_0 0x0094u
#define PL_SMC_RESP_0 0x0095u
/* DCE 11.2, somar pl_crtc[n] */
#define PL_GRPH_ENABLE 0x1A00u
#define PL_GRPH_CONTROL 0x1A01u
#define PL_GRPH_SURF 0x1A04u
#define PL_GRPH_PITCH 0x1A06u
#define PL_GRPH_SURF_HI 0x1A07u
#define PL_GRPH_X_END 0x1A0Du
#define PL_GRPH_Y_END 0x1A0Eu
#define PL_CUR_CONTROL 0x1A66u
#define PL_CUR_SURF 0x1A67u
#define PL_CUR_SIZE 0x1A68u
#define PL_CUR_SURF_HI 0x1A69u
#define PL_CUR_POSITION 0x1A6Au
#define PL_CUR_HOT_SPOT 0x1A6Bu
#define PL_CRTC_CONTROL 0x1B9Cu
#define PL_CRTC_STATUS_POSITION 0x1BA4u

static int tela_crtc = -1;       /* CRTC que mostra a tela; -1 = nenhum */

static uint32_t pl_r(uint32_t reg) {
#ifdef AMDGOP_SIM
  return sim_r(reg);
#else
  return *(volatile uint32_t*)(mmio + reg * 4u);
#endif
}

static void pl_w(uint32_t reg, uint32_t v) {
#ifdef AMDGOP_SIM
  sim_w(reg, v);
#else
  *(volatile uint32_t*)(mmio + reg * 4u) = v;
#endif
}

/* ---------------------------------------------------------------------------
 * Etapa C da RX 570: estado do SMC (o processador do SMU7). Leitura indireta
 * como vi_smc_rreg do Linux: endereco em SMC_IND_INDEX_11, valor em
 * SMC_IND_DATA_11 (smu_7_1_3_d.h). A unica escrita e o indice, devolvido ao
 * valor de antes no fim. smu7_is_smc_ram_running: ck_disable = 0 e
 * SMC_PC_C >= 0x20100.
 * ------------------------------------------------------------------------- */
#define PL_SMC_IND_INDEX_11 0x01ACu
#define PL_SMC_IND_DATA_11 0x01ADu
#define PL_IX_SYSCON_RESET_CNTL 0x80000000u    /* bit 0 rst_reg */
#define PL_IX_SYSCON_CLOCK_CNTL_0 0x80000004u  /* bit 0 ck_disable */
#define PL_IX_SMC_PC_C 0x80000370u
#define PL_IX_SMU_STATUS 0xE0003088u           /* bit 0 SMU_DONE */
#define PL_IX_SMU_FIRMWARE 0xE00030A4u         /* bit 16 SMU_MODE (protegido), 17 SMU_SEL */
#define PL_IX_FIRMWARE_FLAGS 0x0003F000u       /* bit 0 INTERRUPTS_ENABLED */
#define PL_IX_FEATURE_STATUS 0x0003F010u       /* bit 13 VOLTAGE_CONTROLLER_ON (DPM) */
#define PL_IX_FW_HEADER 0x00020000u            /* SMU7_Firmware_Header (smu7.h) */

static uint32_t pl_smc_r(uint32_t ix) {
  pl_w(PL_SMC_IND_INDEX_11, ix);
  return pl_r(PL_SMC_IND_DATA_11);
}

static void polaris_smc_diag(void) {
  uint32_t idx = pl_r(PL_SMC_IND_INDEX_11);
  uint32_t ck = pl_smc_r(PL_IX_SYSCON_CLOCK_CNTL_0), pc = pl_smc_r(PL_IX_SMC_PC_C);
  uint32_t fw = pl_smc_r(PL_IX_SMU_FIRMWARE);
  log_kv("[amdgop] smc rst ", pl_smc_r(PL_IX_SYSCON_RESET_CNTL));
  log_kv(" ck ", ck);
  log_kv(" pc ", pc);
  log_kv(" smu_status ", pl_smc_r(PL_IX_SMU_STATUS));
  log_kv(" smu_fw ", fw);
  log_kv("\n[amdgop] smc fw_flags ", pl_smc_r(PL_IX_FIRMWARE_FLAGS));
  log_kv(" feature_status ", pl_smc_r(PL_IX_FEATURE_STATUS));
  /* cabecalho: Digest[5], Version, HeaderSize, Flags, EntryPoint, CodeSize */
  log_kv(" hdr versao ", pl_smc_r(PL_IX_FW_HEADER + 20u));
  log_kv(" tam ", pl_smc_r(PL_IX_FW_HEADER + 24u));
  log_kv(" flags ", pl_smc_r(PL_IX_FW_HEADER + 28u));
  log_kv(" entrada ", pl_smc_r(PL_IX_FW_HEADER + 32u));
  log_kv(" codigo ", pl_smc_r(PL_IX_FW_HEADER + 36u));
  pl_w(PL_SMC_IND_INDEX_11, idx);
  k->log("\n[amdgop] smc ");
  k->log(!(ck & 1u) && pc >= 0x20100u ? "RODANDO (firmware do SMU na RAM do SMC)" : "PARADO (sem firmware rodando)");
  k->log(fw & (1u << 16) ? ", modo protegido\n" : ", modo nao protegido\n");
}

#ifdef AMD_SMC
/* ---------------------------------------------------------------------------
 * Etapa C2 da RX 570: carrega o firmware do SMU (polaris10_k_smc.bin do
 * linux-firmware, em X:\SODOS\FW\P10KSMC.BIN) na RAM do SMC e o inicia em
 * modo protegido. Sequencia de polaris10_start_smu_in_protection_mode +
 * smu7_upload_smu_firmware_image + smu7_send_msg_to_smc_offset (Linux 6.12,
 * pm/powerplay/smumgr). O SMC confere a assinatura da AMD: imagem errada da
 * SMU_PASS = 0 e nada roda. Toda espera tem limite (ms e numero de voltas).
 * ------------------------------------------------------------------------- */
#define PL_FW_NOME "P10KSMC.BIN"
#define PL_SMC_IND_ACCESS_CNTL 0x0092u
#define PL_AUTOINC_11 0x800u                 /* AUTO_INCREMENT_IND_11 */
#define PL_SMC_MSG_ARG_0 0x00A4u
#define PL_IX_RCU_UC_EVENTS 0xC0000004u
#define PL_RCU_INT_EN 0x10000u               /* RCU_UC_EVENTS.INTERRUPTS_ENABLED */
#define PL_SMU_DONE 1u
#define PL_SMU_PASS 2u
#define PL_MSG_TEST 0x100u                   /* PPSMC_MSG_Test */
#define PL_SMC_RAM 0x20000u                  /* inicio do firmware na RAM do SMC */
#define PL_SMC_RAM_MAX 0x20000u              /* bytes que cabem ate 0x40000 */

static int smc_res = 1;                      /* resultado da carga (1 = nao tentou) */

static void pl_smc_w(uint32_t ix, uint32_t v) {
  pl_w(PL_SMC_IND_INDEX_11, ix);
  pl_w(PL_SMC_IND_DATA_11, v);
}

static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Espera (valor & mask) == want (ou != want com diferente). 0 = ok. */
static int pl_espera(int indireto, uint32_t reg, uint32_t mask, uint32_t want, int diferente,
                     uint32_t ms) {
  uint32_t t0 = k->ms(), voltas = 0, v;
  for (;;) {
    v = (indireto ? pl_smc_r(reg) : pl_r(reg)) & mask;
    if (diferente ? v != want : v == want) return 0;
    if (k->ms() - t0 > ms || ++voltas > 20000000u) return -1;
  }
}

static int pl_smc_rodando(void) {
  return !(pl_smc_r(PL_IX_SYSCON_CLOCK_CNTL_0) & 1u) && pl_smc_r(PL_IX_SMC_PC_C) >= 0x20100u;
}

/* Mensagem ao SMU com argumento, como smu7_send_msg_to_smc_with_parameter.
 * Devolve SMC_RESP (1 = ok) ou -1 se nao respondeu. */
static int pl_smc_msg(uint32_t msg, uint32_t arg) {
  (void)pl_espera(0, PL_SMC_RESP_0, 0xFFFFu, 0, 1, 100);   /* resposta anterior (pode nao haver) */
  pl_w(PL_SMC_MSG_ARG_0, arg);
  pl_w(PL_SMC_RESP_0, 0);
  pl_w(PL_SMC_MESSAGE_0, msg);
  if (pl_espera(0, PL_SMC_RESP_0, 0xFFFFu, 0, 1, 1000) != 0) return -1;
  return (int)(pl_r(PL_SMC_RESP_0) & 0xFFFFu);
}

static int pl_smc_falha(int cod, uint32_t idx) {
  pl_w(PL_SMC_IND_INDEX_11, idx);
  return cod;
}

/* Le e confere \SODOS\FW\P10KSMC.BIN (so o arquivo, nada na placa). Devolve o
 * buffer (k->free depois) com off, ucode e versao preenchidos, ou 0. */
static uint8_t* smc_fw_le(uint32_t* off, uint32_t* ucode, uint32_t* versao) {
  uint8_t* fw;
  int tam, n;
  uint32_t hsize, hdr, vmaj, inicio;
  if (k->ext != GPU_KAPI_EXT || !k->fw_size || !k->fw_read) {
    k->log("[amdsmc] kernel sem leitura de firmware (gpu_kapi antiga)\n");
    return 0;
  }
  tam = k->fw_size(PL_FW_NOME);
  if (tam < 0x24 + 4 || tam > 0x40000) {
    log_kv("[amdsmc] sem \\SODOS\\FW\\" PL_FW_NOME " valido, tamanho ", (uint32_t)tam);
    k->log("\n");
    return 0;
  }
  fw = (uint8_t*)k->alloc((uint32_t)tam, 64);
  if (!fw) return 0;
  n = k->fw_read(PL_FW_NOME, fw, (uint32_t)tam);
  /* smc_firmware_header_v1_0: size_bytes, header_size_bytes, versao do
   * cabecalho (16+16), versao do IP (16+16), ucode_version, ucode_size_bytes,
   * ucode_array_offset_bytes, crc32, ucode_start_addr */
  hsize = le32(fw);
  hdr = le32(fw + 4);
  vmaj = le32(fw + 8) & 0xFFFFu;
  *versao = le32(fw + 16);
  *ucode = le32(fw + 20);
  *off = le32(fw + 24);
  inicio = le32(fw + 32);
  log_kv("[amdsmc] " PL_FW_NOME " bytes ", (uint32_t)n);
  log_kv(" cabecalho ", hdr);
  log_kv(" v", vmaj);
  log_kv(" ucode_versao ", *versao);
  log_kv(" ucode ", *ucode);
  log_kv(" em ", *off);
  log_kv(" inicio ", inicio);
  k->log("\n");
  if (n != tam || hsize != (uint32_t)tam || vmaj != 1u || hdr < 0x24u || *off < hdr ||
      *ucode == 0 || (*ucode & 3u) || *off + *ucode > (uint32_t)tam || *ucode > PL_SMC_RAM_MAX ||
      inicio != PL_SMC_RAM) {
    k->log("[amdsmc] cabecalho do firmware invalido: nao carrega\n");
    k->free(fw);
    return 0;
  }
  return fw;
}

/* 0 = firmware rodando (ja estava ou carregou); <0 = nao carregou (a tela e
 * o cursor seguem iguais: o DCE nao depende do SMU). */
static int polaris_smc_carrega(void) {
  uint8_t* fw;
  const uint8_t* img;
  int resp;
  uint32_t ucode, off, versao, i, idx, fwreg, rst, ck;
  idx = pl_r(PL_SMC_IND_INDEX_11);
  if (pl_smc_rodando()) {
    log_kv("[amdsmc] firmware do SMU ja roda: pc ", pl_smc_r(PL_IX_SMC_PC_C));
    log_kv(" flags ", pl_smc_r(PL_IX_FIRMWARE_FLAGS));
    k->log(" (nada a carregar)\n");
    return pl_smc_falha(0, idx);
  }
  fwreg = pl_smc_r(PL_IX_SMU_FIRMWARE);
  if (!(fwreg & (1u << 16)) || !(fwreg & (1u << 17))) {
    /* modo nao protegido ou chave SK: outra imagem e outra sequencia */
    log_kv("[amdsmc] SMU_FIRMWARE ", fwreg);
    k->log(" nao e modo protegido com chave de producao: nao carrega\n");
    return pl_smc_falha(-1, idx);
  }
  pl_w(PL_SMC_IND_INDEX_11, idx);
  fw = smc_fw_le(&off, &ucode, &versao);
  if (!fw) return -1;
  img = fw + off;
  idx = pl_r(PL_SMC_IND_INDEX_11);
  /* 1. reset do SMC */
  rst = pl_smc_r(PL_IX_SYSCON_RESET_CNTL);
  pl_smc_w(PL_IX_SYSCON_RESET_CNTL, rst | 1u);
  /* 2. imagem na RAM do SMC a partir de 0x20000, indice com auto-incremento */
  pl_w(PL_SMC_IND_INDEX_11, PL_SMC_RAM);
  pl_w(PL_SMC_IND_ACCESS_CNTL, pl_r(PL_SMC_IND_ACCESS_CNTL) | PL_AUTOINC_11);
  for (i = 0; i < ucode; i += 4) pl_w(PL_SMC_IND_DATA_11, le32(img + i));
  pl_w(PL_SMC_IND_ACCESS_CNTL, pl_r(PL_SMC_IND_ACCESS_CNTL) & ~PL_AUTOINC_11);
  k->free(fw);
  /* 3. status limpo, clock ligado, reset solto */
  pl_smc_w(PL_IX_SMU_STATUS, 0);
  ck = pl_smc_r(PL_IX_SYSCON_CLOCK_CNTL_0);
  pl_smc_w(PL_IX_SYSCON_CLOCK_CNTL_0, ck & ~1u);
  pl_smc_w(PL_IX_SYSCON_RESET_CNTL, rst & ~1u);
  if (pl_espera(1, PL_IX_RCU_UC_EVENTS, PL_RCU_INT_EN, PL_RCU_INT_EN, 0, 1000) != 0) {
    log_kv("[amdsmc] SMC nao ligou as interrupcoes: rcu ", pl_smc_r(PL_IX_RCU_UC_EVENTS));
    log_kv(" pc ", pl_smc_r(PL_IX_SMC_PC_C));
    k->log("\n");
    return pl_smc_falha(-2, idx);
  }
  /* 4. "Test" com 0x20000: a ROM confere a imagem e a inicia */
  resp = pl_smc_msg(PL_MSG_TEST, PL_SMC_RAM);
  if (pl_espera(1, PL_IX_SMU_STATUS, PL_SMU_DONE, 0, 1, 2000) != 0) {
    log_kv("[amdsmc] SMU_DONE nao veio: status ", pl_smc_r(PL_IX_SMU_STATUS));
    log_kv(" resp ", (uint32_t)resp);
    k->log("\n");
    return pl_smc_falha(-3, idx);
  }
  if (!(pl_smc_r(PL_IX_SMU_STATUS) & PL_SMU_PASS)) {
    log_kv("[amdsmc] SMC recusou o firmware (SMU_PASS 0): status ", pl_smc_r(PL_IX_SMU_STATUS));
    k->log("\n");
    return pl_smc_falha(-4, idx);
  }
  /* 5. reinicia ja com o firmware e espera ele terminar de iniciar */
  pl_smc_w(PL_IX_FIRMWARE_FLAGS, 0);
  pl_smc_w(PL_IX_SYSCON_RESET_CNTL, pl_smc_r(PL_IX_SYSCON_RESET_CNTL) | 1u);
  pl_smc_w(PL_IX_SYSCON_RESET_CNTL, pl_smc_r(PL_IX_SYSCON_RESET_CNTL) & ~1u);
  if (pl_espera(1, PL_IX_FIRMWARE_FLAGS, 1u, 1u, 0, 2000) != 0) {
    log_kv("[amdsmc] firmware aceito, mas nao iniciou: flags ", pl_smc_r(PL_IX_FIRMWARE_FLAGS));
    log_kv(" pc ", pl_smc_r(PL_IX_SMC_PC_C));
    k->log("\n");
    return pl_smc_falha(-5, idx);
  }
  log_kv("[amdsmc] firmware do SMU RODANDO: versao ", versao);
  log_kv(" pc ", pl_smc_r(PL_IX_SMC_PC_C));
  log_kv(" flags ", pl_smc_r(PL_IX_FIRMWARE_FLAGS));
  log_kv(" resp_test ", (uint32_t)resp);
  k->log("\n");
  return pl_smc_falha(0, idx);
}
#endif

/* So leitura: memoria, motores e a parte de exibicao que o GOP deixou. */
static void polaris_diag(void) {
  uint32_t bar0 = k->pci_read32(m.pci_bus, m.pci_dev, m.pci_fun, 0x10);
  uint32_t bar1 = k->pci_read32(m.pci_bus, m.pci_dev, m.pci_fun, 0x14);
  uint32_t bar5 = k->pci_read32(m.pci_bus, m.pci_dev, m.pci_fun, 0x24);
  uint32_t memsize, fbloc, n;
  uint64_t vram_cpu, fb_cpu = (uint64_t)(uintptr_t)m.fb;
  /* BAR5: memoria, 32 bits (no desktop: 0xF6F00000) */
  if ((bar5 & 1u) || ((bar5 >> 1) & 3u) == 2u || (bar5 & ~0xFu) == 0) {
    log_kv("[amdgop] BAR5 inesperado ", bar5);
    k->log(", Polaris nao lida\n");
    return;
  }
  mmio = (volatile uint8_t*)(uintptr_t)(bar5 & ~0xFu);
  /* sanidade: tamanho da VRAM em MB; 0 ou FFFFFFFF = BAR errado ou placa desligada */
  memsize = pl_r(PL_CONFIG_MEMSIZE);
  log_kv("[amdgop] polaris vram MB ", memsize);
  if (memsize == 0 || memsize == 0xFFFFFFFFu) {
    k->log(" (invalido: nada mais e lido)\n");
    mmio = 0;
    return;
  }
  /* abertura da VRAM para a CPU: BAR0 de 64 bits */
  vram_cpu = ((bar0 >> 1) & 3u) == 2u ? ((uint64_t)bar1 << 32 | (bar0 & ~0xFu)) : (bar0 & ~0xFu);
  log_kv(" bar0 ", (uint32_t)(vram_cpu >> 32));
  log_kv(":", (uint32_t)vram_cpu);
  log_kv(" fb-bar0 ", (uint32_t)(fb_cpu - vram_cpu));
  /* MC: onde a GPU ve a VRAM (base/top em 16 MB: bits 15:0 base, 31:16 top) */
  fbloc = pl_r(PL_MC_VM_FB_LOCATION);
  fb_lo = (uint64_t)(fbloc & 0xFFFFu) << 24;
  fb_hi = (uint64_t)(fbloc >> 16) << 24 | 0xFFFFFFu;
  log_kv("\n[amdgop] mc fb_location ", fbloc);
  log_kv(" fb_offset ", pl_r(PL_MC_VM_FB_OFFSET));
  log_kv(" agp_bot ", pl_r(PL_MC_VM_AGP_BOT));
  log_kv(" agp_top ", pl_r(PL_MC_VM_AGP_TOP));
  log_kv(" agp_base ", pl_r(PL_MC_VM_AGP_BASE));
  log_kv("\n[amdgop] mc sys_low ", pl_r(PL_MC_VM_SYS_LOW));
  log_kv(" sys_high ", pl_r(PL_MC_VM_SYS_HIGH));
  log_kv(" l1_tlb ", pl_r(PL_MC_VM_MX_L1_TLB_CNTL));
  log_kv(" vm_ctx0 ", pl_r(PL_VM_CONTEXT0_CNTL));
  log_kv(" ramcfg ", pl_r(PL_MC_ARB_RAMCFG));
  log_kv(" seq_misc0 ", pl_r(PL_MC_SEQ_MISC0));
  /* GFX: o firmware deixou os motores parados? (CP_ME_CNTL bits 24/26/28 = halt) */
  log_kv("\n[amdgop] gfx grbm ", pl_r(PL_GRBM_STATUS));
  log_kv(" grbm2 ", pl_r(PL_GRBM_STATUS2));
  log_kv(" cp_me ", pl_r(PL_CP_ME_CNTL));
  log_kv(" cp_mec ", pl_r(PL_CP_MEC_CNTL));
  log_kv(" rb0 ", pl_r(PL_CP_RB0_CNTL));
  log_kv(" rlc ", pl_r(PL_RLC_CNTL));
  log_kv(" gb_addr ", pl_r(PL_GB_ADDR_CONFIG));
  log_kv("\n[amdgop] smc msg ", pl_r(PL_SMC_MESSAGE_0));
  log_kv(" resp ", pl_r(PL_SMC_RESP_0));
  k->log("\n");
  polaris_smc_diag();
  /* DCE: cada CRTC; o da tela tem CRTC e GRPH ligados e pitch = largura */
  tela_crtc = -1;
  for (n = 0; n < PL_NCRTC; n++) {
    uint32_t o = pl_crtc[n];
    uint32_t crtc = pl_r(PL_CRTC_CONTROL + o), en = pl_r(PL_GRPH_ENABLE + o);
    uint64_t surf = (uint64_t)pl_r(PL_GRPH_SURF_HI + o) << 32 | pl_r(PL_GRPH_SURF + o);
    if (tela_crtc < 0 && (crtc & 1u) && (en & 1u) && surf && pl_r(PL_GRPH_PITCH + o) == m.w) {
      tela_crtc = (int)n;
      surf_gpu = surf;
    }
    log_kv("[amdgop] crtc ", n);
    log_kv(" ctl ", crtc);
    log_kv(" grph_en ", en);
    log_kv(" grph_ctl ", pl_r(PL_GRPH_CONTROL + o));
    log_kv(" surf ", (uint32_t)(surf >> 32));
    log_kv(":", (uint32_t)surf);
    log_kv(" pitch ", pl_r(PL_GRPH_PITCH + o));
    log_kv(" fim ", pl_r(PL_GRPH_X_END + o));
    log_kv("x", pl_r(PL_GRPH_Y_END + o));
    log_kv(" pos ", pl_r(PL_CRTC_STATUS_POSITION + o));
    log_kv("\n[amdgop]   cur_ctl ", pl_r(PL_CUR_CONTROL + o));
    log_kv(" cur_surf ", pl_r(PL_CUR_SURF_HI + o));
    log_kv(":", pl_r(PL_CUR_SURF + o));
    log_kv(" size ", pl_r(PL_CUR_SIZE + o));
    log_kv(" pos ", pl_r(PL_CUR_POSITION + o));
    log_kv(" hot ", pl_r(PL_CUR_HOT_SPOT + o));
    k->log("\n");
  }
  log_kv("[amdgop] crtc da tela ", (uint32_t)tela_crtc);
  if (tela_crtc >= 0) {
    log_kv(" surf-mc_base ", (uint32_t)(surf_gpu - fb_lo));
    k->log(surf_gpu >= fb_lo && surf_gpu <= fb_hi ? " (dentro da VRAM do MC)" : " (FORA da VRAM do MC)");
  }
  k->log("\n");
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
  /* o framebuffer pode estar acima de 4 GB (RX 570: BAR0 em 0xFB_0000_0000) */
  log_kv(" fb ", (uint32_t)((uint64_t)(uintptr_t)m.fb >> 32));
  log_kv(":", (uint32_t)(uintptr_t)m.fb);
  log_kv(" pitch ", m.fb_pitch);
  k->log("\n");
  if (m.pci_id == AMD_BARCELO) dcn_diag();
  if (m.pci_id == AMD_POLARIS10) polaris_diag();
#ifdef AMD_SMC
  /* uma vez por carga do DLL; se ja roda (outra sessao do WIN), so confere */
  if (m.pci_id == AMD_POLARIS10 && mmio) {
    smc_res = polaris_smc_carrega();
  } else {
    /* outra placa (QEMU): so confere o arquivo, para testar a leitura */
    uint32_t o, u, v;
    uint8_t* fw = smc_fw_le(&o, &u, &v);
    if (fw) {
      k->log("[amdsmc] firmware conferido; placa nao e a RX 570: nada carregado\n");
      k->free(fw);
    }
  }
#endif
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
static uint32_t cur_salvo_n;  /* palavras guardadas em cur_salvo */
static int cur_ligado;

static uint32_t hreg(uint32_t r) { return r + (uint32_t)tela_pipe * PIPE_HUBP; }
static uint32_t creg(uint32_t r) { return r + (uint32_t)tela_pipe * PIPE_CNVC; }

/* ---------------------------------------------------------------------------
 * Cursor do DCE 11.2 (Polaris), no CRTC da tela. Sequencia de
 * dce_v11_0_lock_cursor / show_cursor / hide_cursor / cursor_move_locked do
 * amdgpu (Linux 6.12): tudo entre CURSOR_UPDATE_LOCK 1 e 0, a troca vale no
 * proximo quadro. CUR_POSITION e o canto de cima da imagem (o DCE nao subtrai
 * o hot spot: CUR_HOT_SPOT so recorta quando a imagem passa da borda).
 * Imagem ARGB pre-multiplicado, 16 MB adiante do inicio da VRAM, como na
 * Barcelo, mas **128x128**: o DCE 11 le o cursor com pitch de 128 pixels
 * (o tamanho maximo), qualquer que seja o CUR_SIZE. Com 64x64 (pitch 64) a
 * placa mostrava a seta com metade da altura e, abaixo dela, lixo da VRAM
 * (1o boot, 2026-11-17). O Linux usa 128x128 (max_cursor_width) sempre.
 * ------------------------------------------------------------------------- */
#define PL_LADO 128u
#define PL_BYTES (PL_LADO * PL_LADO * 4u)
#define PL_CUR_UPDATE 0x1A6Eu
#define PL_CUR_LOCK (1u << 16)          /* CUR_UPDATE.CURSOR_UPDATE_LOCK */
#define PL_CUR_EN 1u                    /* CUR_CONTROL.CURSOR_EN */
#define PL_CUR_MODE_MASK (3u << 8)      /* CUR_CONTROL.CURSOR_MODE */
#define PL_CUR_2X (1u << 16)            /* CUR_CONTROL.CURSOR_2X_MAGNIFY */

static int pl_hot_x, pl_hot_y;

static uint32_t pl_creg(uint32_t r) { return r + pl_crtc[tela_crtc]; }

static void pl_lock(int on) {
  uint32_t v = pl_r(pl_creg(PL_CUR_UPDATE));
  pl_w(pl_creg(PL_CUR_UPDATE), on ? v | PL_CUR_LOCK : v & ~PL_CUR_LOCK);
}

static void pl_cur_desliga(void) {
  if (!cur_ligado || tela_crtc < 0 || !mmio) return;
  pl_lock(1);
  pl_w(pl_creg(PL_CUR_CONTROL), pl_r(pl_creg(PL_CUR_CONTROL)) & ~PL_CUR_EN);
  pl_lock(0);
  cur_ligado = 0;
}

/* canto de cima = ponta - hot; o que passa da borda esquerda/de cima vira a
 * origem recortada (CUR_HOT_SPOT), como no cursor_move_locked */
static void pl_pos(int x, int y) {
  int ox = 0, oy = 0;
  x -= pl_hot_x;
  y -= pl_hot_y;
  if (x < 0) {
    ox = -x > (int)PL_LADO - 1 ? (int)PL_LADO - 1 : -x;
    x = 0;
  }
  if (y < 0) {
    oy = -y > (int)PL_LADO - 1 ? (int)PL_LADO - 1 : -y;
    y = 0;
  }
  pl_w(pl_creg(PL_CUR_POSITION), ((uint32_t)x & 0x3FFFu) << 16 | ((uint32_t)y & 0x3FFFu));
  pl_w(pl_creg(PL_CUR_HOT_SPOT), (uint32_t)ox << 16 | (uint32_t)oy);
}

static int pl_cursor_set(const uint32_t* argb, int w, int h, int hot_x, int hot_y) {
  volatile uint32_t* img = (volatile uint32_t*)(m.fb + CUR_VRAM_OFF);
  uint64_t addr = surf_gpu + CUR_VRAM_OFF;
  uint32_t i, x, y, v;
  if (tela_crtc < 0 || !mmio || !argb || w < 1 || h < 1 || w > (int)PL_LADO || h > (int)PL_LADO ||
      hot_x < 0 || hot_y < 0 || hot_x >= w || hot_y >= h) {
    k->log("[amdcur] polaris: sem CRTC da tela ou imagem invalida: cursor fica com a CPU\n");
    return -1;
  }
  if (addr + PL_BYTES - 1u > fb_hi || addr < fb_lo || surf_gpu < fb_lo) {
    log_kv("[amdcur] endereco fora da VRAM do MC ", (uint32_t)(addr >> 32));
    log_kv(":", (uint32_t)addr);
    k->log("\n");
    return -1;
  }
  if (!cur_salvo) {
    cur_salvo = (uint32_t*)k->alloc(PL_BYTES, 64);
    if (!cur_salvo) return -1;
    cur_salvo_n = PL_LADO * PL_LADO;
    for (i = 0; i < cur_salvo_n; i++) cur_salvo[i] = img[i];
  }
  for (y = 0; y < PL_LADO; y++)
    for (x = 0; x < PL_LADO; x++)
      img[y * PL_LADO + x] = (int)x < w && (int)y < h ? argb[y * (uint32_t)w + x] : 0u;
  pl_hot_x = hot_x;
  pl_hot_y = hot_y;
  pl_lock(1);
  pl_w(pl_creg(PL_CUR_SURF_HI), (uint32_t)(addr >> 32));
  pl_w(pl_creg(PL_CUR_SURF), (uint32_t)addr);
  pl_w(pl_creg(PL_CUR_SIZE), (PL_LADO - 1u) << 16 | (PL_LADO - 1u));
  if (!cur_ligado) pl_pos((int)m.w / 2, (int)m.h / 2);
  v = pl_r(pl_creg(PL_CUR_CONTROL));
  v &= ~(PL_CUR_MODE_MASK | PL_CUR_2X);
  v |= PL_CUR_EN | CUR_MODE_PREMULT << 8;
  pl_w(pl_creg(PL_CUR_CONTROL), v);
  pl_lock(0);
  cur_ligado = 1;
  log_kv("[amdcur] polaris crtc ", (uint32_t)tela_crtc);
  log_kv(" imagem gpu ", (uint32_t)(addr >> 32));
  log_kv(":", (uint32_t)addr);
  log_kv(" cur_ctl ", pl_r(pl_creg(PL_CUR_CONTROL)));
  log_kv(" size ", pl_r(pl_creg(PL_CUR_SIZE)));
  log_kv(" update ", pl_r(pl_creg(PL_CUR_UPDATE)));
  k->log("\n");
  return 0;
}

static int pl_cursor_move(int x, int y) {
  if (!cur_ligado || tela_crtc < 0) return -1;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  pl_lock(1);
  pl_pos(x, y);
  pl_lock(0);
  return 0;
}

static void cur_desliga(void) {
  if (m.pci_id == AMD_POLARIS10) {
    pl_cur_desliga();
    return;
  }
  if (!cur_ligado || tela_pipe < 0) return;
  dcn_w(hreg(R_CURSOR_CONTROL), dcn_r(hreg(R_CURSOR_CONTROL)) & ~1u);
  dcn_w(creg(R_CNVC_CUR_CONTROL), dcn_r(creg(R_CNVC_CUR_CONTROL)) & ~1u);
  cur_ligado = 0;
}

static int a_cursor_set(const uint32_t* argb, int w, int h, int hot_x, int hot_y) {
  volatile uint32_t* img = (volatile uint32_t*)(m.fb + CUR_VRAM_OFF);
  uint64_t addr = surf_gpu + CUR_VRAM_OFF;
  uint32_t i, x, y, v;
  if (m.pci_id == AMD_POLARIS10) return pl_cursor_set(argb, w, h, hot_x, hot_y);
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
    cur_salvo_n = CUR_LADO * CUR_LADO;
    for (i = 0; i < cur_salvo_n; i++) cur_salvo[i] = img[i];
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
  if (m.pci_id == AMD_POLARIS10) return pl_cursor_move(x, y);
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
    for (i = 0; i < cur_salvo_n; i++) img[i] = cur_salvo[i];
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
#if defined(AMD_SMC)
  "amdsmc (Radeon, GOP, cursor, SMU)",
  GPU_CAP_PRESENT | GPU_CAP_DAMAGE | GPU_CAP_CURSOR,
#elif defined(AMD_CURSOR)
  "amdcur (Radeon, GOP, cursor da placa)",
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
