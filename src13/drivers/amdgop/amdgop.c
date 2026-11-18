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
 * firmware do SMU). AMDSDMA.DLL (-DAMD_SDMA): o AMDSMC mais a etapa E (liga
 * o motor de copia SDMA0 e o testa). AMDGOP_SIM: teste no host (drivers/amdgop/smcsim.c), os
 * registradores da Polaris passam por sim_r/sim_w em vez do BAR5. */
/* AMDDUAL.DLL (-DAMD_TELA2): o AMDSDMA mais a segunda tela em espelho (etapa M). */
#ifdef AMD_TELA2
#ifndef AMD_SDMA
#define AMD_SDMA
#endif
#endif
#ifdef AMD_SDMA
#ifndef AMD_SMC
#define AMD_SMC
#endif
#endif
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
    /* 2 pixels por movimento (rep movsq) e o pixel impar no fim (2026-11-18:
     * metade das escritas pela abertura write-combining) */
    uint64_t n = w >> 1, um = w & 1u;
    __asm__ volatile ("rep movsq" : "+S"(src), "+D"(dst), "+c"(n) : : "memory");
    if (um) __asm__ volatile ("rep movsl" : "+S"(src), "+D"(dst), "+c"(um) : : "memory");
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
static int pl_espelho = -1;      /* CRTC em espelho (etapa M): o cursor vai nele tambem */

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

#ifdef AMD_SDMA
/* ---------------------------------------------------------------------------
 * Etapa E da RX 570: o motor de copia SDMA0 (sdma_v3_0 do Linux 6.12).
 *
 * 1. microcodigo: na Polaris o amdgpu carrega pelo SMU (AMDGPU_FW_LOAD_SMU,
 *    smu7_request_smu_load_fw): o ucode do polaris10_sdma.bin vai para a
 *    VRAM, uma tabela SMU_DRAMData_TOC aponta para ele, e as mensagens
 *    SMU_DRAM_ADDR, DRV_DRAM_ADDR e LoadUcodes pedem a carga; o SMU marca o
 *    bit em SoftRegisters.UcodeLoadStatus. So a SDMA0 (mascara 0x2).
 * 2. anel de 4 KB na VRAM, sem doorbell nem polling de wptr (o wptr vai pelo
 *    registrador), rptr escrito pela SDMA na VRAM; VMID 0 com a VM desligada
 *    (etapa A): os enderecos sao os do MC (0xF4_xxxx_xxxx = VRAM).
 * 3. testes: escrita de uma palavra, preenchimento (CONST_FILL) e copia
 *    (COPY linear) de 4 MB, cada um com FENCE; confere pela CPU e mede.
 *
 * Tudo fica em VRAM livre entre 32 e 44 MB (a tela GOP ocupa os primeiros
 * 8 MB e o cursor fica em 16 MB); o kernel mapeia 64 MB do framebuffer.
 * Escritas da CPU na VRAM passam pelo HDP: sfence + HDP flush antes de a
 * placa ler, e HDP_DEBUG0 (invalida) antes de a CPU ler o que a placa
 * escreveu.
 * ------------------------------------------------------------------------- */
#define SD_FW_NOME "P10SDMA.BIN"
#define SD_TOC 0x02000000u
#define SD_UCODE 0x02001000u
#define SD_UCODE_MAX 0x10000u
#define SD_SMUBUF 0x02100000u        /* 200 paginas para o SMU (smu_buffer) */
#define SD_RING 0x02200000u
#define SD_RING_DW 1024u              /* 4 KB: RB_SIZE = log2(1024) = 10 */
#define SD_WB 0x02201000u             /* rptr escrito pela SDMA */
#define SD_TESTE 0x02201080u          /* palavra do teste de escrita */
#define SD_FENCE 0x022010C0u
#define SD_A 0x02400000u
#define SD_B 0x02800000u
#define SD_TAM 0x00400000u            /* 4 MB por teste */
#define SD_FIM 0x02C00000u

/* registradores (oss_3_0_d.h, bif_5_0_d.h) */
#define SD0_UCODE_ADDR 0x3400u
#define SD0_CLK_CTRL 0x3403u
#define SD0_CNTL 0x3404u
#define SD0_CHICKEN_BITS 0x3405u
#define SD0_TILING_CONFIG 0x3406u
#define SD0_SEM_WAIT_FAIL_TIMER_CNTL 0x3409u
#define SD0_STATUS_REG 0x340Du
#define SD0_F32_CNTL 0x3412u
#define SD0_GFX_RB_CNTL 0x3480u
#define SD0_GFX_RB_BASE 0x3481u
#define SD0_GFX_RB_BASE_HI 0x3482u
#define SD0_GFX_RB_RPTR 0x3483u
#define SD0_GFX_RB_WPTR 0x3484u
#define SD0_GFX_RB_WPTR_POLL_CNTL 0x3485u
#define SD0_GFX_RB_WPTR_POLL_ADDR_HI 0x3486u
#define SD0_GFX_RB_WPTR_POLL_ADDR_LO 0x3487u
#define SD0_GFX_RB_RPTR_ADDR_HI 0x3488u
#define SD0_GFX_RB_RPTR_ADDR_LO 0x3489u
#define SD0_GFX_IB_CNTL 0x348Au
#define SD0_GFX_IB_RPTR 0x348Bu
#define SD0_GFX_IB_OFFSET 0x348Cu
#define SD0_GFX_DOORBELL 0x3492u
#define SD0_GFX_VIRTUAL_ADDR 0x34A7u
#define SD0_GFX_APE1_CNTL 0x34A8u
#define SD_HDP_FLUSH_CNTL 0x1520u     /* HDP_MEM_COHERENCY_FLUSH_CNTL */
#define SD_HDP_DEBUG0 0x0BCCu
#define SD_SRBM_STATUS2 0x0393u
/* campos */
#define SD_F32_HALT 1u
#define SD_CNTL_ATC_L1 0x2u
#define SD_CNTL_AUTO_CTXSW 0x40000u
#define SD_RB_ENABLE 1u
#define SD_RB_SIZE_MASK 0x3Eu
#define SD_RB_RPTR_WB 0x1000u
#define SD_IB_ENABLE 1u
#define SD_DOORBELL_EN 0x10000000u
/* pacotes (tonga_sdma_pkt_open.h) */
#define SD_OP_NOP 0u
#define SD_OP_COPY 1u
#define SD_OP_WRITE 2u
#define SD_OP_FENCE 5u
#define SD_OP_CONST_FILL 11u
#define SD_FILL_MAX 0x200000u          /* CONST_FILL: o Linux usa ate 0x3FFFE0 bytes */
/* CONSTANT_FILL_HEADER.fillsize (bits 31:30). Com 0 (como o Linux manda) a
 * RX 570 repete so o byte baixo: 0x5AA5C33C virou 3C3C3C3C (2026-11-18).
 * 2 = palavra de 32 bits. */
#define SD_FILL_DW (2u << 30)
#define SD_COPY_MAX 0x100000u          /* COPY linear: o Linux usa ate 0x1FFFFF bytes */
/* SMU */
#define SD_MSG_DRV_DRAM_HI 0x250u
#define SD_MSG_DRV_DRAM_LO 0x251u
#define SD_MSG_SMU_DRAM_HI 0x252u
#define SD_MSG_SMU_DRAM_LO 0x253u
#define SD_MSG_LOAD_UCODES 0x254u
#define SD_UCODE_ID_SDMA0 1u
#define SD_UCODE_SDMA0_MASK 0x2u
#define SD_SOFTREGS_PTR 0x20030u      /* SMU74_Firmware_Header.SoftRegisters */
#define SD_UCODE_LOAD_STATUS 108u     /* offsetof(SMU74_SoftRegisters, UcodeLoadStatus) */

static uint64_t sd_mc;                /* endereco MC do inicio da VRAM (fb_lo) */
static volatile uint8_t* sd_cpu;      /* o mesmo para a CPU (m.fb, fb - BAR0 = 0) */
static uint32_t sd_wptr;              /* em palavras */
static uint32_t sd_seq;
static int sd_ligada;

static volatile uint32_t* sd_p(uint32_t off) { return (volatile uint32_t*)(sd_cpu + off); }

static void sd_hdp_flush(void) {
  __asm__ volatile ("sfence" ::: "memory");
  pl_w(SD_HDP_FLUSH_CNTL, 1);
  (void)pl_r(SD_HDP_FLUSH_CNTL);
}

static uint32_t sd_le(uint32_t off) {
  pl_w(SD_HDP_DEBUG0, 1);   /* invalida o cache de leitura do HDP */
  return *sd_p(off);
}

static void sd_put(uint32_t v) {
  *sd_p(SD_RING + (sd_wptr % SD_RING_DW) * 4u) = v;
  sd_wptr++;
}

/* entrega o que foi escrito no anel com um fence no fim; devolve o numero */
static uint32_t sd_envia(void) {
  uint32_t seq = ++sd_seq;
  sd_put(SD_OP_FENCE);
  sd_put((uint32_t)(sd_mc + SD_FENCE));
  sd_put((uint32_t)((sd_mc + SD_FENCE) >> 32));
  sd_put(seq);
  sd_hdp_flush();
  pl_w(SD0_GFX_RB_WPTR, (sd_wptr % SD_RING_DW) << 2);
  return seq;
}

static int sd_espera(uint32_t seq, uint32_t ms) {
  uint32_t t0 = k->ms(), voltas = 0;
  for (;;) {
    if (sd_le(SD_FENCE) == seq) return 0;
    if (k->ms() - t0 > ms || ++voltas > 50000000u) {
      log_kv("[amdsdma] sem fence: esperado ", seq);
      log_kv(" fence ", sd_le(SD_FENCE));
      log_kv(" rptr ", pl_r(SD0_GFX_RB_RPTR));
      log_kv(" wptr ", pl_r(SD0_GFX_RB_WPTR));
      log_kv(" status ", pl_r(SD0_STATUS_REG));
      log_kv(" f32 ", pl_r(SD0_F32_CNTL));
      log_kv(" srbm2 ", pl_r(SD_SRBM_STATUS2));
      k->log("\n");
      return -1;
    }
  }
}

/* entrega e espera */
static int sd_executa(uint32_t ms) { return sd_espera(sd_envia(), ms); }

/* le \SODOS\FW\P10SDMA.BIN e (grava = 1) poe o ucode na VRAM; devolve o
 * tamanho do ucode ou 0 */
static uint32_t sd_ucode_le(uint32_t* versao, int grava) {
  uint8_t* fw;
  int tam, n;
  uint32_t hsize, vmaj, ucode, off, i;
  if (k->ext != GPU_KAPI_EXT || !k->fw_size || !k->fw_read) return 0;
  tam = k->fw_size(SD_FW_NOME);
  if (tam < 0x34 + 4 || tam > 0x20000) {
    log_kv("[amdsdma] sem \\SODOS\\FW\\" SD_FW_NOME " valido, tamanho ", (uint32_t)tam);
    k->log("\n");
    return 0;
  }
  fw = (uint8_t*)k->alloc((uint32_t)tam, 64);
  if (!fw) return 0;
  n = k->fw_read(SD_FW_NOME, fw, (uint32_t)tam);
  /* sdma_firmware_header_v1_0 = common_firmware_header + 4 palavras */
  hsize = le32(fw);
  vmaj = le32(fw + 8) & 0xFFFFu;
  *versao = le32(fw + 16);
  ucode = le32(fw + 20);
  off = le32(fw + 24);
  log_kv("[amdsdma] " SD_FW_NOME " bytes ", (uint32_t)n);
  log_kv(" v", vmaj);
  log_kv(" ucode_versao ", *versao);
  log_kv(" ucode ", ucode);
  log_kv(" em ", off);
  k->log("\n");
  if (n != tam || hsize != (uint32_t)tam || vmaj != 1u || ucode == 0 || (ucode & 3u) ||
      off < 0x34u || off + ucode > (uint32_t)tam || ucode > SD_UCODE_MAX) {
    k->log("[amdsdma] cabecalho invalido\n");
    k->free(fw);
    return 0;
  }
  if (grava)
    for (i = 0; i < ucode; i += 4) *sd_p(SD_UCODE + i) = le32(fw + off + i);
  k->free(fw);
  return ucode;
}

static int sd_msg(uint32_t msg, uint32_t arg, const char* nome) {
  int r = pl_smc_msg(msg, arg);
  if (r != 1) {
    k->log("[amdsdma] SMU recusou ");
    k->log(nome);
    log_kv(": resp ", (uint32_t)r);
    k->log("\n");
    return -1;
  }
  return 0;
}

/* 1. microcodigo da SDMA0 pelo SMU */
static int sd_carrega_ucode(void) {
  uint32_t soft, st, versao = 0, tam, idx, t0, voltas = 0;
  uint64_t a;
  volatile uint32_t* toc = sd_p(SD_TOC);
  int i;
  idx = pl_r(PL_SMC_IND_INDEX_11);
  soft = pl_smc_r(SD_SOFTREGS_PTR);
  /* SMU74_SoftRegisters tem 120 bytes; no polaris10_k_smc.bin fica em 0x3FA24 */
  if (soft < 0x20000u || soft + 0x80u > 0x40000u || (soft & 3u)) {
    log_kv("[amdsdma] SoftRegisters fora da RAM do SMC: ", soft);
    k->log("\n");
    pl_w(PL_SMC_IND_INDEX_11, idx);
    return -1;
  }
  st = pl_smc_r(soft + SD_UCODE_LOAD_STATUS);
  pl_w(PL_SMC_IND_INDEX_11, idx);
  tam = sd_ucode_le(&versao, 1);
  if (!tam) return -1;
  /* SMU_DRAMData_TOC: versao 1, uma entrada (SMU_Entry, 28 bytes) */
  for (i = 0; i < 1024; i++) toc[i] = 0;
  a = sd_mc + SD_UCODE;
  toc[0] = 1;                                           /* structure_version */
  toc[1] = 1;                                           /* num_entries */
  toc[2] = SD_UCODE_ID_SDMA0 | (versao & 0xFFFFu) << 16; /* id, version */
  toc[3] = (uint32_t)(a >> 32);                         /* image_addr_high */
  toc[4] = (uint32_t)a;                                 /* image_addr_low */
  toc[5] = 0;                                           /* meta_data_addr_high */
  toc[6] = 0;                                           /* meta_data_addr_low */
  toc[7] = tam;                                         /* data_size_byte */
  toc[8] = 0;                                           /* flags, num_register_entries */
  sd_hdp_flush();
  idx = pl_r(PL_SMC_IND_INDEX_11);
  pl_smc_w(soft + SD_UCODE_LOAD_STATUS, 0);
  pl_w(PL_SMC_IND_INDEX_11, idx);
  log_kv("[amdsdma] soft_regs ", soft);
  log_kv(" status antes ", st);
  log_kv(" toc ", (uint32_t)((sd_mc + SD_TOC) >> 32));
  log_kv(":", (uint32_t)(sd_mc + SD_TOC));
  k->log("\n");
  /* como smu7_request_smu_load_fw: as respostas so vao para o log; quem diz
   * se carregou e o UcodeLoadStatus */
  (void)sd_msg(SD_MSG_SMU_DRAM_HI, (uint32_t)((sd_mc + SD_SMUBUF) >> 32), "SMU_DRAM_ADDR_HI");
  (void)sd_msg(SD_MSG_SMU_DRAM_LO, (uint32_t)(sd_mc + SD_SMUBUF), "SMU_DRAM_ADDR_LO");
  (void)sd_msg(SD_MSG_DRV_DRAM_HI, (uint32_t)((sd_mc + SD_TOC) >> 32), "DRV_DRAM_ADDR_HI");
  (void)sd_msg(SD_MSG_DRV_DRAM_LO, (uint32_t)(sd_mc + SD_TOC), "DRV_DRAM_ADDR_LO");
  (void)sd_msg(SD_MSG_LOAD_UCODES, SD_UCODE_SDMA0_MASK, "LoadUcodes");
  t0 = k->ms();
  for (;;) {
    idx = pl_r(PL_SMC_IND_INDEX_11);
    st = pl_smc_r(soft + SD_UCODE_LOAD_STATUS);
    pl_w(PL_SMC_IND_INDEX_11, idx);
    if ((st & SD_UCODE_SDMA0_MASK) == SD_UCODE_SDMA0_MASK) break;
    if (k->ms() - t0 > 2000u || ++voltas > 20000000u) {
      log_kv("[amdsdma] SMU nao carregou a SDMA0: UcodeLoadStatus ", st);
      k->log("\n");
      return -3;
    }
  }
  log_kv("[amdsdma] ucode da SDMA0 carregado pelo SMU: versao ", versao);
  log_kv(" bytes ", tam);
  log_kv(" status ", st);
  k->log("\n");
  return 0;
}

static void sd_para(void) {
  pl_w(SD0_GFX_RB_CNTL, pl_r(SD0_GFX_RB_CNTL) & ~SD_RB_ENABLE);
  pl_w(SD0_GFX_IB_CNTL, pl_r(SD0_GFX_IB_CNTL) & ~SD_IB_ENABLE);
  pl_w(SD0_F32_CNTL, pl_r(SD0_F32_CNTL) | SD_F32_HALT);
}

/* 2. anel (sdma_v3_0_start / gfx_resume, so a SDMA0 e a VMID 0) */
static int sd_liga(void) {
  uint32_t v;
  uint64_t ring = sd_mc + SD_RING, wb = sd_mc + SD_WB;
  int i;
  /* golden_settings_polaris10_a11 da SDMA0 */
  pl_w(SD0_CHICKEN_BITS, (pl_r(SD0_CHICKEN_BITS) & ~0xFC910007u) | 0x00810007u);
  pl_w(SD0_CLK_CTRL, pl_r(SD0_CLK_CTRL) & ~0xFF000FFFu);
  pl_w(SD0_GFX_IB_CNTL, (pl_r(SD0_GFX_IB_CNTL) & ~0x800F0111u) | 0x00000100u);
  /* parada antes de programar */
  pl_w(SD0_CNTL, (pl_r(SD0_CNTL) & ~SD_CNTL_AUTO_CTXSW) | SD_CNTL_ATC_L1);
  sd_para();
  for (i = 0; i < (int)SD_RING_DW; i++) *sd_p(SD_RING + (uint32_t)i * 4u) = 0;
  *sd_p(SD_WB) = 0;
  *sd_p(SD_FENCE) = 0;
  sd_hdp_flush();
  pl_w(SD0_GFX_VIRTUAL_ADDR, 0);
  pl_w(SD0_GFX_APE1_CNTL, 0);
  pl_w(SD0_TILING_CONFIG, pl_r(PL_GB_ADDR_CONFIG) & 0x70u);
  pl_w(SD0_SEM_WAIT_FAIL_TIMER_CNTL, 0);
  v = (pl_r(SD0_GFX_RB_CNTL) & ~(SD_RB_SIZE_MASK | SD_RB_ENABLE)) | 10u << 1;
  pl_w(SD0_GFX_RB_CNTL, v);
  sd_wptr = 0;
  pl_w(SD0_GFX_RB_RPTR, 0);
  pl_w(SD0_GFX_RB_WPTR, 0);
  pl_w(SD0_GFX_IB_RPTR, 0);
  pl_w(SD0_GFX_IB_OFFSET, 0);
  pl_w(SD0_GFX_RB_RPTR_ADDR_HI, (uint32_t)(wb >> 32));
  pl_w(SD0_GFX_RB_RPTR_ADDR_LO, (uint32_t)wb & 0xFFFFFFFCu);
  v |= SD_RB_RPTR_WB;
  pl_w(SD0_GFX_RB_BASE, (uint32_t)(ring >> 8));
  pl_w(SD0_GFX_RB_BASE_HI, (uint32_t)(ring >> 40));
  pl_w(SD0_GFX_DOORBELL, pl_r(SD0_GFX_DOORBELL) & ~SD_DOORBELL_EN);
  pl_w(SD0_GFX_RB_WPTR_POLL_ADDR_LO, (uint32_t)(wb + 0x40u));
  pl_w(SD0_GFX_RB_WPTR_POLL_ADDR_HI, (uint32_t)((wb + 0x40u) >> 32));
  pl_w(SD0_GFX_RB_WPTR_POLL_CNTL, pl_r(SD0_GFX_RB_WPTR_POLL_CNTL) & ~1u);
  pl_w(SD0_GFX_RB_CNTL, v | SD_RB_ENABLE);
  pl_w(SD0_GFX_IB_CNTL, pl_r(SD0_GFX_IB_CNTL) | SD_IB_ENABLE);
  /* solta o F32 e liga a troca de contexto */
  pl_w(SD0_F32_CNTL, pl_r(SD0_F32_CNTL) & ~SD_F32_HALT);
  pl_w(SD0_CNTL, pl_r(SD0_CNTL) | SD_CNTL_AUTO_CTXSW | SD_CNTL_ATC_L1);
  sd_ligada = 1;
  log_kv("[amdsdma] anel ", (uint32_t)(ring >> 32));
  log_kv(":", (uint32_t)ring);
  log_kv(" rb_cntl ", pl_r(SD0_GFX_RB_CNTL));
  log_kv(" f32 ", pl_r(SD0_F32_CNTL));
  log_kv(" status ", pl_r(SD0_STATUS_REG));
  k->log("\n");
  return 0;
}

/* 3. testes */
static int sd_testes(void) {
  uint32_t i, ruins = 0;
  uint64_t t;
  /* escrita de uma palavra (sdma_v3_0_ring_test_ring) */
  *sd_p(SD_TESTE) = 0xCAFEDEADu;
  sd_put(SD_OP_WRITE);
  sd_put((uint32_t)(sd_mc + SD_TESTE));
  sd_put((uint32_t)((sd_mc + SD_TESTE) >> 32));
  sd_put(1);
  sd_put(0xDEADBEEFu);
  if (sd_executa(1000) != 0) return -1;
  if (sd_le(SD_TESTE) != 0xDEADBEEFu) {
    log_kv("[amdsdma] teste de escrita: lido ", sd_le(SD_TESTE));
    k->log("\n");
    return -2;
  }
  k->log("[amdsdma] escrita pela SDMA ok\n");
  /* preenchimento de 4 MB */
  /* como sdma_v3_0_emit_fill_buffer, mas com fillsize 2 (32 bits); contagem
   * em bytes */
  t = tsc();
  for (i = 0; i < SD_TAM; i += SD_FILL_MAX) {
    sd_put(SD_OP_CONST_FILL | SD_FILL_DW);
    sd_put((uint32_t)(sd_mc + SD_A + i));
    sd_put((uint32_t)((sd_mc + SD_A + i) >> 32));
    sd_put(0x5AA5C33Cu);
    sd_put(SD_FILL_MAX);
  }
  if (sd_executa(2000) != 0) return -3;
  t = tsc() - t;
  for (i = 0; i < SD_TAM; i += 4096u)
    if (sd_le(SD_A + i) != 0x5AA5C33Cu || sd_le(SD_A + i + 4092u) != 0x5AA5C33Cu) ruins++;
  log_kv("[amdsdma] preenchimento de 4 MB: ciclos ", (uint32_t)t);
  log_kv(" paginas erradas ", ruins);
  log_kv(" (primeira palavra ", sd_le(SD_A));
  k->log(")");
  k->log("\n");
  if (ruins) return -4;
  /* copia de 4 MB (A com um padrao por palavra -> B) */
  for (i = 0; i < SD_TAM; i += 4096u) *sd_p(SD_A + i) = i ^ 0x13572468u;
  sd_hdp_flush();
  t = tsc();
  for (i = 0; i < SD_TAM; i += SD_COPY_MAX) {   /* sdma_v3_0_emit_copy_buffer */
    sd_put(SD_OP_COPY);                            /* sub_op 0 = linear */
    sd_put(SD_COPY_MAX);
    sd_put(0);
    sd_put((uint32_t)(sd_mc + SD_A + i));
    sd_put((uint32_t)((sd_mc + SD_A + i) >> 32));
    sd_put((uint32_t)(sd_mc + SD_B + i));
    sd_put((uint32_t)((sd_mc + SD_B + i) >> 32));
  }
  if (sd_executa(2000) != 0) return -5;
  t = tsc() - t;
  ruins = 0;
  for (i = 0; i < SD_TAM; i += 4096u)
    if (sd_le(SD_B + i) != (i ^ 0x13572468u) || sd_le(SD_B + i + 4092u) != 0x5AA5C33Cu) ruins++;
  log_kv("[amdsdma] copia de 4 MB: ciclos ", (uint32_t)t);
  log_kv(" paginas erradas ", ruins);
  k->log("\n");
  if (ruins) return -6;
  /* para comparar: a CPU preenche 4 MB pela abertura (write-combining) */
  t = tsc();
  for (i = 0; i < SD_TAM; i += 4) *sd_p(SD_B + i) = 0;
  __asm__ volatile ("sfence" ::: "memory");
  t = tsc() - t;
  log_kv("[amdsdma] (CPU preenchendo os mesmos 4 MB: ciclos ", (uint32_t)t);
  k->log(")\n");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Etapa D (primeira parte): a SDMA lendo a RAM do PC, sem GART.
 *
 * O MC da RX 570 esta como o GOP deixou: VM_CONTEXT0 desligado e
 * MC_VM_MX_L1_TLB_CNTL.SYSTEM_ACCESS_MODE = 0 ("PA only", etapa A). Nesse
 * modo um endereco fora da VRAM (MC_VM_FB_LOCATION) deveria ir direto para a
 * RAM, com o endereco fisico. O back buffer do kernel fica em 0x100000
 * (fisico = virtual) e o k->alloc tambem devolve fisico = virtual.
 *
 * Antes de usar, testa: copia linear RAM -> VRAM e copia de um retangulo
 * (COPY sub-janela, como o cik_sdma do Mesa: elementsize log2(4), pitch e
 * tamanho - 1) conferidos pela CPU. So LE a RAM: nada aqui escreve na RAM
 * pela placa. Passou: o present usa a SDMA (uma sub-janela por area, um
 * fence no fim); qualquer erro volta para a CPU e desliga o caminho.
 * ------------------------------------------------------------------------- */
#define SD_SUBWIN (SD_OP_COPY | 4u << 8 | 2u << 29)   /* COPY_LINEAR_SUB_WIND, 4 bytes */
#define SD_RAM_TAM 0x10000u                          /* 64 KB: 256 x 64 pixels */
#define SD_RAM_LARG 256u

static int sd_present_ok;
static uint32_t sd_falhas_present;

/* Abertura AGP do MC (MC_VM_AGP_BOT/TOP/BASE, em 4 MB). O GOP da RX 570 deixa
 * BOT = TOP = BASE = 0: uma abertura ATIVA cobrindo os primeiros 4 MB fisicos.
 * O back buffer do kernel comeca em 1 MB, e as linhas que caiam ali eram lidas
 * por esse caminho, que nao ve o que ainda esta na cache da CPU: no primeiro
 * boot com o present pela SDMA (2026-11-18) a parte de cima da tela comecou
 * preta e ganhou riscos curtos (linhas de cache velhas). O teste da RAM tinha
 * passado porque o buffer dele ficava acima de 4 MB. O Linux desliga a
 * abertura (amdgpu_gmc_set_agp_default: BOT = 0x3FFFF > TOP = 0); aqui o mesmo,
 * e o valor do GOP volta no fim. */
#define SD_MC_VM_AGP_TOP 0x080Au
#define SD_MC_VM_AGP_BOT 0x080Bu
#define SD_MC_VM_AGP_BASE 0x080Cu
static uint32_t sd_agp_top, sd_agp_bot, sd_agp_base;
static int sd_agp_mexeu;

static void sd_agp_desliga(void) {
  sd_agp_top = pl_r(SD_MC_VM_AGP_TOP) & 0x3FFFFu;
  sd_agp_bot = pl_r(SD_MC_VM_AGP_BOT) & 0x3FFFFu;
  sd_agp_base = pl_r(SD_MC_VM_AGP_BASE) & 0x3FFFFu;
  if (sd_agp_bot > sd_agp_top) return;   /* ja desligada */
  log_kv("[amdsdma] abertura AGP ligada: bot ", sd_agp_bot);
  log_kv(" top ", sd_agp_top);
  log_kv(" base ", sd_agp_base);
  log_kv(" (fisico ", sd_agp_bot << 2);
  log_kv(" a ", (sd_agp_top << 2) + 3u);
  k->log(" MB): desligada, como no Linux\n");
  pl_w(SD_MC_VM_AGP_BOT, 0x3FFFFu);   /* primeiro o BOT: BOT > TOP ja desliga */
  pl_w(SD_MC_VM_AGP_TOP, 0);
  sd_agp_mexeu = 1;
}

static void sd_agp_volta(void) {
  if (!sd_agp_mexeu) return;
  pl_w(SD_MC_VM_AGP_TOP, sd_agp_top);
  pl_w(SD_MC_VM_AGP_BOT, sd_agp_bot);
  pl_w(SD_MC_VM_AGP_BASE, sd_agp_base);
  sd_agp_mexeu = 0;
}

/* Testa a leitura do proprio back buffer (as primeiras linhas, as que a
 * abertura AGP pegava): a CPU escreve um padrao (fica na cache), a SDMA copia
 * na hora, a CPU confere e o conteudo antigo volta. */
static int sd_teste_back(void) {
  uint32_t* b = (uint32_t*)m.back;
  uint32_t* salvo = (uint32_t*)k->alloc(SD_RAM_TAM, 64);
  uint64_t fis = (uint64_t)(uintptr_t)m.back;
  uint32_t i, ruins = 0;
  if (!salvo || !b) return -1;
  for (i = 0; i < SD_RAM_TAM / 4u; i++) salvo[i] = b[i];
  for (i = 0; i < SD_RAM_TAM / 4u; i++) b[i] = 0x3C000000u ^ i * 2246822519u;
  sd_put(SD_OP_COPY);
  sd_put(SD_RAM_TAM);
  sd_put(0);
  sd_put((uint32_t)fis);
  sd_put((uint32_t)(fis >> 32));
  sd_put((uint32_t)(sd_mc + SD_B));
  sd_put((uint32_t)((sd_mc + SD_B) >> 32));
  if (sd_executa(1000) != 0) {
    for (i = 0; i < SD_RAM_TAM / 4u; i++) b[i] = salvo[i];
    k->free(salvo);
    return -6;
  }
  for (i = 0; i < SD_RAM_TAM / 4u; i++)
    if (sd_le(SD_B + i * 4u) != b[i]) ruins++;
  log_kv("[amdsdma] back buffer (fisico ", (uint32_t)fis);
  log_kv(") -> VRAM 64 KB: erradas ", ruins);
  log_kv(" (palavra 0: ", sd_le(SD_B));
  log_kv(" esperado ", b[0]);
  k->log(")\n");
  for (i = 0; i < SD_RAM_TAM / 4u; i++) b[i] = salvo[i];
  k->free(salvo);
  return ruins ? -7 : 0;
}

/* sub-janela de w x h pixels de 32 bits (Mesa cik_sdma_copy, GFX8) */
static void sd_subjanela(uint64_t src, uint32_t sx, uint32_t sy, uint32_t spitch, uint32_t salt,
                         uint64_t dst, uint32_t dx, uint32_t dy, uint32_t dpitch, uint32_t dalt,
                         uint32_t w, uint32_t h) {
  sd_put(SD_SUBWIN);
  sd_put((uint32_t)src);
  sd_put((uint32_t)(src >> 32));
  sd_put(sx | sy << 16);
  sd_put((spitch - 1u) << 16);
  sd_put(spitch * salt - 1u);
  sd_put((uint32_t)dst);
  sd_put((uint32_t)(dst >> 32));
  sd_put(dx | dy << 16);
  sd_put((dpitch - 1u) << 16);
  sd_put(dpitch * dalt - 1u);
  sd_put((w - 1u) | (h - 1u) << 16);
  sd_put(0);
}

static int sd_teste_ram(void) {
  uint32_t* buf = (uint32_t*)k->alloc(SD_RAM_TAM, 4096);
  uint64_t fis = (uint64_t)(uintptr_t)buf, t;
  uint32_t i, x, y, ruins = 0, fora = 0;
  const uint32_t rx = 16, ry = 8, rw = 64, rh = 32, dx = 100, dy = 10, dp = 1920;
  if (!buf) return -1;
  if (fis >= fb_lo && fis <= fb_hi) {
    k->log("[amdsdma] buffer de teste cai na faixa da VRAM do MC: sem teste da RAM\n");
    k->free(buf);
    return -1;
  }
  for (i = 0; i < SD_RAM_TAM / 4u; i++) buf[i] = 0xA5000000u ^ i * 2654435761u;
  __asm__ volatile ("mfence" ::: "memory");
  /* 1. linear: 64 KB da RAM para a area B */
  t = tsc();
  sd_put(SD_OP_COPY);
  sd_put(SD_RAM_TAM);
  sd_put(0);
  sd_put((uint32_t)fis);
  sd_put((uint32_t)(fis >> 32));
  sd_put((uint32_t)(sd_mc + SD_B));
  sd_put((uint32_t)((sd_mc + SD_B) >> 32));
  if (sd_executa(1000) != 0) {
    k->free(buf);
    return -2;
  }
  t = tsc() - t;
  for (i = 0; i < SD_RAM_TAM / 4u; i += 61u)
    if (sd_le(SD_B + i * 4u) != buf[i]) ruins++;
  log_kv("[amdsdma] RAM -> VRAM linear 64 KB: ciclos ", (uint32_t)t);
  log_kv(" erradas ", ruins);
  log_kv(" (palavra 0: ", sd_le(SD_B));
  log_kv(" esperado ", buf[0]);
  k->log(")\n");
  if (ruins) {
    k->free(buf);
    return -3;
  }
  /* 2. sub-janela: (16,8) 64x32 de uma imagem 256x64 -> (100,10) de uma
   * imagem 1920 de largura, sobre um fundo zerado pela SDMA */
  sd_put(SD_OP_CONST_FILL | SD_FILL_DW);
  sd_put((uint32_t)(sd_mc + SD_B));
  sd_put((uint32_t)((sd_mc + SD_B) >> 32));
  sd_put(0);
  sd_put(SD_FILL_MAX);
  sd_subjanela(fis, rx, ry, SD_RAM_LARG, SD_RAM_TAM / 4u / SD_RAM_LARG, sd_mc + SD_B, dx, dy, dp, 64,
               rw, rh);
  if (sd_executa(1000) != 0) {
    k->free(buf);
    return -4;
  }
  for (y = 0; y < rh; y++)
    for (x = 0; x < rw; x += 7u)
      if (sd_le(SD_B + ((dy + y) * dp + dx + x) * 4u) != buf[(ry + y) * SD_RAM_LARG + rx + x]) ruins++;
  /* em volta do retangulo continua zero */
  if (sd_le(SD_B + ((dy - 1u) * dp + dx) * 4u) || sd_le(SD_B + (dy * dp + dx - 1u) * 4u) ||
      sd_le(SD_B + (dy * dp + dx + rw) * 4u) || sd_le(SD_B + ((dy + rh) * dp + dx) * 4u))
    fora++;
  log_kv("[amdsdma] RAM -> VRAM sub-janela 64x32: erradas ", ruins);
  log_kv(" fora do retangulo ", fora);
  log_kv(" (pixel 0: ", sd_le(SD_B + (dy * dp + dx) * 4u));
  log_kv(" esperado ", buf[ry * SD_RAM_LARG + rx]);
  k->log(")\n");
  k->free(buf);
  if (ruins || fora) return -5;
  return sd_teste_back();
}

/* ---------------------------------------------------------------------------
 * Present dividido entre a SDMA e a CPU (2026-11-18).
 *
 * Medido na placa: a SDMA (com o SMU sem DPM, clocks provavelmente no minimo)
 * copia RAM -> VRAM a ~3,4 ciclos por pixel; a CPU, pela abertura com
 * write-combining, a ~2,5. Sozinha a SDMA perde. Mas as duas trabalham ao
 * mesmo tempo: cada area e cortada na horizontal, a SDMA recebe as linhas de
 * baixo e a CPU copia as de cima enquanto isso; no fim espera o fence.
 * Somadas (~0,30 + ~0,40 pixel por ciclo) dao ~1,4 ciclos por pixel.
 *
 * A fatia da SDMA (sd_parte, em 1/1024) se ajusta a cada quadro: se o fence
 * ja tinha chegado quando a CPU acabou, a SDMA estava folgada (fatia +16); se
 * a espera passou de 1/8 do tempo da CPU, ela atrasou o quadro (fatia -16).
 * Quadros pequenos (menos de SD_MIN_PX pixels) ficam so com a CPU: ali o
 * custo fixo do fence e do HDP pesa mais que a copia.
 * ------------------------------------------------------------------------- */
#define SD_MIN_PX 65536u
#define SD_MAX_AREAS 40
#define SD_BLOCO 64u                          /* linhas da CPU entre olhadas no fence */
static uint32_t sd_parte = 440;              /* fatia inicial: 0,30 / (0,30 + 0,40) */
static uint64_t sd_px_sdma, sd_px_cpu;       /* pixels de cada lado (para o log) */
static uint32_t sd_folgas, sd_atrasos;

struct sd_area { uint32_t x, y, w, h; };

static int sd_present(const struct gpu_rect* r, int n) {
  uint64_t src = (uint64_t)(uintptr_t)m.back, dst = surf_gpu, t0, t1, t2, te, ts, px = 0, pxs = 0;
  uint32_t sp = m.pitch / 4u, dp = m.fb_pitch / 4u, seq;
  struct sd_area a[SD_MAX_AREAS];
  int i, na = 0, pronto;
  if (!sd_ligada || (m.pitch & 3u) || (m.fb_pitch & 3u) || sp > 16384u || dp > 16384u) return -1;
  if (!r || n <= 0 || n > SD_MAX_AREAS) {
    a[0].x = a[0].y = 0;
    a[0].w = m.w;
    a[0].h = m.h;
    na = 1;
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
      a[na].x = (uint32_t)x;
      a[na].y = (uint32_t)y;
      a[na].w = (uint32_t)w;
      a[na].h = (uint32_t)h;
      na++;
    }
  }
  for (i = 0; i < na; i++) px += (uint64_t)a[i].w * a[i].h;
  if (!na) return 0;
  if (px < SD_MIN_PX) return -1;   /* a CPU faz sozinha */
  /* 1. a SDMA recebe as linhas de baixo de cada area */
  for (i = 0; i < na; i++) {
    uint32_t hs = (uint32_t)(((uint64_t)a[i].h * sd_parte) >> 10);
    if (!hs) continue;
    sd_subjanela(src, a[i].x, a[i].y + a[i].h - hs, sp, m.h, dst, a[i].x, a[i].y + a[i].h - hs, dp, m.h,
                 a[i].w, hs);
    pxs += (uint64_t)a[i].w * hs;
  }
  seq = pxs ? sd_envia() : 0;
  te = tsc();
  /* 2. a CPU copia as de cima ao mesmo tempo, em blocos de SD_BLOCO linhas;
   * entre um bloco e outro olha o fence, para saber quando a SDMA acabou */
  t0 = tsc();
  ts = 0;
  {
    /* a SDMA acabou entre a ultima olhada sem fence e a primeira com ele:
     * vale o meio (2026-11-18: usar a primeira olhada atrasava a SDMA em ate
     * um bloco, e a fatia ficava em ~0,43 com a SDMA acabando antes da CPU
     * em 99,8% dos quadros) */
    uint64_t tult = te;
    for (i = 0; i < na; i++) {
      uint32_t hs = (uint32_t)(((uint64_t)a[i].h * sd_parte) >> 10), y, hc = a[i].h - hs;
      for (y = 0; y < hc; y += SD_BLOCO) {
        copia(a[i].x, a[i].y + y, a[i].w, hc - y > SD_BLOCO ? SD_BLOCO : hc - y);
        if (pxs && !ts) {
          uint64_t agora = tsc();
          if (sd_le(SD_FENCE) == seq) ts = tult + (agora - tult) / 2u;
          else tult = agora;
        }
      }
    }
  }
  t1 = tsc();
  /* 3. espera a SDMA */
  if (pxs) {
    pronto = ts != 0;
    if (!pronto) {
      if (sd_espera(seq, 200) != 0) {
        sd_falhas_present++;
        sd_present_ok = 0;
        sd_para();
        sd_ligada = 0;
        k->log("[amdsdma] present pela SDMA falhou: SDMA parada, a CPU volta a copiar\n");
        return -1;   /* a CPU refaz o quadro inteiro */
      }
      ts = tsc();
    }
    t2 = ts;
    /* 4. ajusta a fatia pelas velocidades medidas neste quadro:
     * SDMA pxs pixels em (t2 - te), CPU (px - pxs) em (t1 - t0); a fatia ideal
     * e pxs/Ts / (pxs/Ts + pxc/Tc) = pxs*Tc / (pxs*Tc + pxc*Ts). Media com o
     * valor anterior (3:1) para nao oscilar. */
    if (pronto) sd_folgas++;
    else sd_atrasos++;
    if (px > pxs && t1 > t0 && t2 > te) {
      uint64_t a1 = pxs * (t1 - t0), a2 = (px - pxs) * (t2 - te);
      uint32_t f = a1 + a2 ? (uint32_t)((a1 << 10) / (a1 + a2)) : sd_parte;
      sd_parte = (3u * sd_parte + f) / 4u;
      if (sd_parte < 64u) sd_parte = 64u;
      if (sd_parte > 960u) sd_parte = 960u;
    }
  }
  sd_px_sdma += pxs;
  sd_px_cpu += px - pxs;
  pixels += px;
  return 0;
}

static int sd_res = 1;
static uint32_t sd_quadros;   /* quadros que a SDMA copiou */

static int polaris_sdma(void) {
  int r;
  sd_mc = fb_lo;
  sd_cpu = m.fb;
  if (surf_gpu != fb_lo || sd_mc + SD_FIM - 1u > fb_hi) {
    k->log("[amdsdma] tela nao comeca na VRAM do MC: SDMA nao liga\n");
    return -1;
  }
  sd_para();   /* parada enquanto o SMU troca o microcodigo */
  r = sd_carrega_ucode();
  if (r) return r;
  r = sd_liga();
  if (r) return r;
  r = sd_testes();
  if (r) {
    log_kv("[amdsdma] testes falharam: ", (uint32_t)(-r));
    k->log(" (SDMA parada)\n");
    sd_para();
    sd_ligada = 0;
    return -10 + r;
  }
  k->log("[amdsdma] SDMA0 FUNCIONANDO\n");
  sd_agp_desliga();
  r = sd_teste_ram();
  if (r == 0) {
    sd_present_ok = 1;
    k->log("[amdsdma] a SDMA le a RAM (modo PA, sem GART): o present passa a ser pela placa\n");
  } else {
    log_kv("[amdsdma] SDMA nao leu a RAM direito: ", (uint32_t)(-r));
    k->log(" (present continua pela CPU)\n");
  }
  return 0;
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

#ifdef AMD_SMC
#include "vbios.c"   /* etapa M: VBIOS, saidas e HPD (dois monitores) */
#endif
#ifdef AMD_TELA2
#include "tela2.c"   /* etapa M, parte 2: CRTC 1 em espelho pelo AtomBIOS */
#endif

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
#ifdef AMD_SDMA
    if (smc_res == 0) sd_res = polaris_sdma();
#endif
    polaris_saidas();   /* por ultimo: a copia do VBIOS enche o log */
#ifdef AMD_TELA2
    polaris_tela2();
#endif
  } else {
    /* outra placa (QEMU): so confere o arquivo, para testar a leitura */
    uint32_t o, u, v;
    uint8_t* fw = smc_fw_le(&o, &u, &v);
    if (fw) {
      k->log("[amdsmc] firmware conferido; placa nao e a RX 570: nada carregado\n");
      k->free(fw);
    }
#ifdef AMD_SDMA
    if (sd_ucode_le(&v, 0)) k->log("[amdsdma] ucode conferido; placa nao e a RX 570: SDMA nao liga\n");
#endif
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
#ifdef AMD_SDMA
  if (sd_present_ok && sd_present(r, n) == 0) {
    ciclos += tsc() - t0;
    quadros++;
    sd_quadros++;
    return 0;
  }
#endif
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

/* escreve no CRTC da tela e no do espelho (segunda tela), se houver */
static void pl_cw(uint32_t r, uint32_t v) {
  pl_w(pl_creg(r), v);
  if (pl_espelho >= 0 && pl_espelho != tela_crtc) pl_w(r + pl_crtc[pl_espelho], v);
}

static void pl_lock(int on) {
  uint32_t v = pl_r(pl_creg(PL_CUR_UPDATE));
  pl_cw(PL_CUR_UPDATE, on ? v | PL_CUR_LOCK : v & ~PL_CUR_LOCK);
}

static void pl_cur_desliga(void) {
  if (!cur_ligado || tela_crtc < 0 || !mmio) return;
  pl_lock(1);
  pl_cw(PL_CUR_CONTROL, pl_r(pl_creg(PL_CUR_CONTROL)) & ~PL_CUR_EN);
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
  pl_cw(PL_CUR_POSITION, ((uint32_t)x & 0x3FFFu) << 16 | ((uint32_t)y & 0x3FFFu));
  pl_cw(PL_CUR_HOT_SPOT, (uint32_t)ox << 16 | (uint32_t)oy);
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
  pl_cw(PL_CUR_SURF_HI, (uint32_t)(addr >> 32));
  pl_cw(PL_CUR_SURF, (uint32_t)addr);
  pl_cw(PL_CUR_SIZE, (PL_LADO - 1u) << 16 | (PL_LADO - 1u));
  if (!cur_ligado) pl_pos((int)m.w / 2, (int)m.h / 2);
  v = pl_r(pl_creg(PL_CUR_CONTROL));
  v &= ~(PL_CUR_MODE_MASK | PL_CUR_2X);
  v |= PL_CUR_EN | CUR_MODE_PREMULT << 8;
  pl_cw(PL_CUR_CONTROL, v);
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
#ifdef AMD_TELA2
  if (t2_estendida) t2_layout(0);   /* a tela de texto usa o fb do GOP */
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
#ifdef AMD_SDMA
  if (sd_ligada) {
    sd_para();
    sd_ligada = 0;
    k->log("[amdsdma] SDMA0 parada (fim)\n");
  }
  sd_agp_volta();
  log_kv("[amdsdma] quadros pela SDMA ", sd_quadros);
  log_kv(" falhas ", sd_falhas_present);
  log_kv(" fatia da SDMA /1024 ", sd_parte);
  /* em milhares de pixels (1024): em 32 bits a contagem em pixels estoura numa
   * sessao de alguns minutos (4.859 quadros de 1,47 M pixels = 7,1 G) */
  log_kv(" kpixels SDMA ", (uint32_t)(sd_px_sdma >> 10));
  log_kv(" CPU ", (uint32_t)(sd_px_cpu >> 10));
  log_kv(" folgas ", sd_folgas);
  log_kv(" atrasos ", sd_atrasos);
  k->log("\n");
  sd_quadros = 0;
  sd_px_sdma = sd_px_cpu = 0;
  sd_folgas = sd_atrasos = 0;
  sd_present_ok = 0;
#endif
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
#ifdef AMD_TELA2
  polaris_tela2_fim();
#endif
#ifdef AMD_SMC
  if (vb) {
    k->free(vb);
    vb = 0;
    vb_tam = 0;
  }
#endif
}

static const struct gpu_drv drv = {
  GPU_ABI,
#if defined(AMD_TELA2)
  "amddual (Radeon, GOP, cursor, SMU, SDMA, 2 telas)",
  GPU_CAP_PRESENT | GPU_CAP_DAMAGE | GPU_CAP_CURSOR | GPU_CAP_TELAS,
#elif defined(AMD_SDMA)
  "amdsdma (Radeon, GOP, cursor, SMU, SDMA)",
  GPU_CAP_PRESENT | GPU_CAP_DAMAGE | GPU_CAP_CURSOR,
#elif defined(AMD_SMC)
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
#ifdef AMD_TELA2
  a_telas,
#endif
};

const struct gpu_drv* gpu_driver(void) { return &drv; }
