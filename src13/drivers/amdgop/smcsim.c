/*
 * smcsim.c - teste no host (Linux/WSL) da etapa C2 do AMDSMC.DLL, sem a
 * placa: inclui o amdgop.c com AMDGOP_SIM, e os registradores da Polaris
 * passam por sim_r/sim_w, que imitam o que o driver usa do SMC:
 *
 *   - SMC_IND_INDEX_11/DATA_11 com auto-incremento (SMC_IND_ACCESS_CNTL);
 *   - RAM do SMC (256 KB), SYSCON_RESET_CNTL, SYSCON_CLOCK_CNTL_0, SMC_PC_C,
 *     SMU_STATUS, SMU_FIRMWARE, RCU_UC_EVENTS;
 *   - a ROM de boot: soltar o reset com clock liga RCU INTERRUPTS_ENABLED;
 *     a mensagem Test (0x100) com argumento 0x20000 confere a imagem na RAM
 *     contra o firmware "assinado" (o arquivo) e da SMU_DONE e SMU_PASS;
 *     depois de aceito, o reset seguinte inicia o firmware (FIRMWARE_FLAGS
 *     bit 0 e PC >= 0x20100).
 *
 * Uso: make -C drivers/amdgop sim  (ou: gcc ... smcsim.c && ./smcsim FW)
 * Cada cenario imprime OK/FALHOU; o codigo de saida e o numero de falhas.
 */
#define AMD_SMC
#define AMDGOP_SIM
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amdgop.c"

/* ---------- modelo da placa ---------- */
#define NREG 0x10000u
static uint32_t reg[NREG];
static uint8_t smc_ram[0x40000];
static uint32_t ind_idx;
static uint32_t smc_rst, smc_ck, smc_pc, smu_status, smu_fw, rcu;
static int aceito;            /* a ROM conferiu a imagem */
static int test_visto;        /* chegou PPSMC_MSG_Test com 0x20000 */
static int travado;           /* cenario: a ROM nao liga as interrupcoes */
static uint8_t* fw_ok;        /* arquivo "assinado" esperado */
static long fw_ok_tam;
static long data_escritas;    /* escritas em SMC_IND_DATA_11 */
static long rst_escritas;
/* VBIOS falso (etapa M): ROM_INDEX/ROM_DATA do SMC, com o indice avancando */
#define VROM_TAM 0x1000u              /* a ROM falsa */
static uint8_t vrom[0x20000];         /* cabe a ROM real (etapa M, parte 2) */
static uint32_t vrom_tam = VROM_TAM;
static int sim_traco;                 /* SIM_TRACE: escritas nos registradores */
static uint32_t rom_ix;
static int rom_surda;         /* cenario: a ROM devolve FF */

static uint32_t ram_r(uint32_t a) {
  uint32_t v;
  if (a + 4 > sizeof(smc_ram)) return 0xDEADBEEFu;
  memcpy(&v, smc_ram + a, 4);
  return v;
}

static void ram_w(uint32_t a, uint32_t v) {
  if (a + 4 <= sizeof(smc_ram)) memcpy(smc_ram + a, &v, 4);
}

static uint32_t smc_r(uint32_t a) {
  switch (a) {
    case 0x80000000u: return smc_rst;
    case 0x80000004u: return smc_ck;
    case 0x80000370u: return smc_pc;
    case 0xE0003088u: return smu_status;
    case 0xE00030A4u: return smu_fw;
    case 0xC0000004u: return rcu;
    case 0xC0600010u: return rom_ix;
    case 0xC0600014u: {
      uint32_t v = 0xFFFFFFFFu;
      if (!rom_surda && rom_ix + 4u <= vrom_tam) memcpy(&v, vrom + rom_ix, 4);
      rom_ix += 4u;
      return v;
    }
  }
  return ram_r(a);
}

static void smc_w(uint32_t a, uint32_t v) {
  switch (a) {
    case 0x80000000u: {
      uint32_t antes = smc_rst;
      rst_escritas++;
      smc_rst = v;
      if ((antes & 1u) && !(v & 1u) && !(smc_ck & 1u)) {
        /* reset solto com clock */
        if (aceito) {
          ram_w(0x3F000u, ram_r(0x3F000u) | 1u);   /* FIRMWARE_FLAGS.INTERRUPTS_ENABLED */
          smc_pc = 0x20480u;
        } else if (!travado) {
          rcu |= 0x10000u;
          smc_pc = 0x2B4Cu;
        }
      }
      if (v & 1u) rcu &= ~0x10000u;
      return;
    }
    case 0x80000004u: smc_ck = v; return;
    case 0xE0003088u: smu_status = v; return;
    case 0xE00030A4u: smu_fw = v; return;
    case 0xC0000004u: rcu = v; return;
    case 0xC0600010u: rom_ix = v; return;
  }
  if ((a & 0xFFFFFF00u) == 0xC0600000u) return;   /* outros registradores da ROM */
  ram_w(a, v);
}

/* a ROM: Test com 0x20000 confere a RAM contra o ucode do arquivo */
static void rom_test(void) {
  uint32_t off, tam;
  if (!(rcu & 0x10000u) || !fw_ok) return;
  memcpy(&off, fw_ok + 24, 4);
  memcpy(&tam, fw_ok + 20, 4);
  aceito = memcmp(smc_ram + 0x20000u, fw_ok + off, tam) == 0;
  smu_status = 1u | (aceito ? 2u : 0u);
}

/* ---------- SMU com firmware (mensagens) e SDMA0, so com AMD_SDMA ---------- */
static uint8_t* fb;           /* a VRAM: MC 0xF4_0000_0000 + x = fb + x */
#define VRAM_MC 0xF400000000ull
#define VRAM_TAM 0x04000000u
static uint8_t* sd_ok;        /* polaris10_sdma.bin esperado */
static long sd_ok_tam;
static int smu_recusa_load;   /* cenario: o SMU nao carrega */
static int sdma_surda;        /* cenario: a SDMA nao anda */
static int sdma_ucode;        /* o SMU carregou o ucode da SDMA0 */
static uint64_t drv_dram, smu_dram;
static long sdma_pacotes;

static uint8_t* vram(uint64_t mc, uint32_t n) {
  if (mc < VRAM_MC || mc + n > VRAM_MC + VRAM_TAM) {
    printf("  !! acesso fora da VRAM simulada: %llx\n", (unsigned long long)mc);
    return 0;
  }
  return fb + (mc - VRAM_MC);
}

/* modo PA (SYSTEM_ACCESS_MODE 0, VM desligada): fora da VRAM o endereco e o
 * fisico da RAM; no host, o ponteiro (o k->alloc e o back buffer simulados
 * sao ponteiros do host, como fisico = virtual no SODOS) */
static int ram_lixo;          /* cenario: a placa nao le a RAM direito */
static long ram_leituras;
static long agp_leituras;     /* leituras do back buffer com a abertura AGP ligada */
static uint32_t* back;        /* back buffer do kernel (no SODOS, fisico 0x100000) */
#define W 1920u
#define H 1080u
static const uint8_t* le_mem(uint64_t mc, uint32_t n) {
  static uint8_t lixo[1 << 22];
  if (mc >= VRAM_MC && mc + n <= VRAM_MC + VRAM_TAM) return fb + (mc - VRAM_MC);
  if (mc >= VRAM_MC && mc < VRAM_MC + 0x100000000ull) {
    printf("  !! leitura fora da VRAM simulada: %llx\n", (unsigned long long)mc);
    return 0;
  }
  ram_leituras++;
  /* Na RX 570 o back buffer fica dentro dos 4 MB da abertura AGP que o GOP
   * deixa ligada (BOT = TOP = 0), e por ela a placa le dados velhos. Aqui:
   * abertura ligada (BOT <= TOP) + leitura do back buffer = zeros. */
  if (back && (reg[0x80B] & 0x3FFFFu) <= (reg[0x80A] & 0x3FFFFu) &&
      mc >= (uint64_t)(uintptr_t)back && mc < (uint64_t)(uintptr_t)back + (uint64_t)W * H * 4u) {
    agp_leituras++;
    memset(lixo, 0, n < sizeof lixo ? n : sizeof lixo);
    return lixo;
  }
  if (ram_lixo) {
    memset(lixo, 0x77, n < sizeof lixo ? n : sizeof lixo);
    return lixo;
  }
  return (const uint8_t*)(uintptr_t)mc;
}

#ifdef AMD_SDMA
static uint32_t v32(uint64_t mc) {
  uint32_t v = 0;
  uint8_t* p = vram(mc, 4);
  if (p) memcpy(&v, p, 4);
  return v;
}

static void w32(uint64_t mc, uint32_t v) {
  uint8_t* p = vram(mc, 4);
  if (p) memcpy(p, &v, 4);
}

/* PPSMC_MSG_LoadUcodes: le a TOC (SMU_DRAMData_TOC) e confere a entrada */
static int smu_load(uint32_t mask) {
  uint32_t soft, n, i, off, tam;
  memcpy(&soft, smc_ram + 0x20030u, 4);
  if (smu_recusa_load || !(mask & 2u) || !sd_ok) return 0;
  if (v32(drv_dram) != 1u) return 0;
  n = v32(drv_dram + 4);
  memcpy(&off, sd_ok + 24, 4);
  memcpy(&tam, sd_ok + 20, 4);
  for (i = 0; i < n && i < 16; i++) {
    uint64_t e = drv_dram + 8 + i * 28u;
    uint32_t idv = v32(e), hi = v32(e + 4), lo = v32(e + 8), sz = v32(e + 20);
    uint64_t img = (uint64_t)hi << 32 | lo;
    uint8_t* p;
    if ((idv & 0xFFFFu) != 1u) continue;
    p = vram(img, sz);
    if (!p || sz != tam || memcmp(p, sd_ok + off, tam) != 0) {
      printf("  !! TOC: ucode da SDMA0 errado (img %llx tam %x)\n", (unsigned long long)img, sz);
      return 0;
    }
    sdma_ucode = 1;
    ram_w(soft + 108u, ram_r(soft + 108u) | 2u);
    return 1;
  }
  return 0;
}

/* a SDMA0 executa o anel de rptr ate wptr */
static void sdma_anda(void) {
  uint64_t base = (uint64_t)reg[0x3481] << 8 | (uint64_t)reg[0x3482] << 40;
  uint32_t rsz = 1u << ((reg[0x3480] >> 1) & 0x1F), rptr = reg[0x3483] >> 2, wptr = reg[0x3484] >> 2;
  if (sdma_surda || !sdma_ucode || !(reg[0x3480] & 1u) || (reg[0x3412] & 1u)) return;
#define DW(n) v32(base + ((rptr + (n)) % rsz) * 4u)
  while (rptr != wptr) {
    uint32_t h = DW(0), op = h & 0xFF;
    sdma_pacotes++;
    if (op == 0) {
      rptr = (rptr + 1) % rsz;
    } else if (op == 2) {            /* WRITE linear: addr, count, dados */
      uint64_t a = (uint64_t)DW(2) << 32 | DW(1);
      uint32_t c = DW(3), j;
      for (j = 0; j < c; j++) w32(a + j * 4u, DW(4 + j));
      rptr = (rptr + 4 + c) % rsz;
    } else if (op == 5) {            /* FENCE */
      w32((uint64_t)DW(2) << 32 | DW(1), DW(3));
      rptr = (rptr + 4) % rsz;
    } else if (op == 11) {           /* CONST_FILL: addr, dado, bytes */
      uint64_t a = (uint64_t)DW(2) << 32 | DW(1);
      uint32_t d = DW(3), b = DW(4), j;
      /* como na RX 570 (2026-11-18): fillsize 0 repete o byte baixo; 2 = 32 bits */
      if ((h >> 30) == 0) d = (d & 0xFFu) * 0x01010101u;
      else if ((h >> 30) != 2) printf("  !! CONST_FILL com fillsize %u\n", h >> 30);
      for (j = 0; j < b; j += 4) w32(a + j, d);
      rptr = (rptr + 5) % rsz;
    } else if (op == 1 && ((h >> 8) & 0xFF) == 4) {   /* COPY sub-janela */
      uint32_t es = 1u << ((h >> 29) & 7);
      uint64_t s = (uint64_t)DW(2) << 32 | DW(1), d = (uint64_t)DW(7) << 32 | DW(6);
      uint32_t sx = DW(3) & 0x3FFF, sy = DW(3) >> 16 & 0x3FFF, sp = (DW(4) >> 16 & 0x3FFF) + 1;
      uint32_t ssl = (DW(5) & 0x0FFFFFFF) + 1;
      uint32_t dx = DW(8) & 0x3FFF, dy = DW(8) >> 16 & 0x3FFF, dp = (DW(9) >> 16 & 0x3FFF) + 1;
      uint32_t dsl = (DW(10) & 0x0FFFFFFF) + 1;
      uint32_t w = (DW(11) & 0x3FFF) + 1, hh = (DW(11) >> 16 & 0x3FFF) + 1, y;
      if (es != 4 || ssl < sp * (sy + hh) || dsl < dp * (dy + hh) || sx + w > sp || dx + w > dp)
        printf("  !! sub-janela estranha: es %u sp %u ssl %u dp %u dsl %u ret %ux%u\n", es, sp, ssl, dp,
               dsl, w, hh);
      for (y = 0; y < hh; y++) {
        const uint8_t* ps = le_mem(s + ((uint64_t)(sy + y) * sp + sx) * es, w * es);
        uint8_t* pd = vram(d + ((uint64_t)(dy + y) * dp + dx) * es, w * es);
        if (ps && pd) memcpy(pd, ps, w * es);
      }
      rptr = (rptr + 13) % rsz;
    } else if (op == 1) {            /* COPY linear: bytes, param, src, dst */
      uint32_t b = DW(1);
      uint64_t s = (uint64_t)DW(4) << 32 | DW(3), d = (uint64_t)DW(6) << 32 | DW(5);
      const uint8_t* ps = le_mem(s, b);
      uint8_t* pd = vram(d, b);   /* a placa nunca escreve na RAM */
      if (b > 0x1FFFFFu) printf("  !! COPY maior que 0x1FFFFF: %x\n", b);
      if (ps && pd) memmove(pd, ps, b);
      rptr = (rptr + 7) % rsz;
    } else {
      printf("  !! pacote desconhecido %08x\n", h);
      break;
    }
  }
#undef DW
  reg[0x3483] = rptr << 2;
  if (reg[0x3480] & 0x1000u) w32((uint64_t)reg[0x3488] << 32 | reg[0x3489], rptr << 2);
}
#endif

uint32_t sim_r(uint32_t r) {
  uint32_t v;
  if (sim_traco > 1 && r != 0x1ACu && r != 0x1ADu && r < NREG) printf("  r %04X -> %08X\n", r, reg[r]);
  if (r == 0x1ADu) {
    v = smc_r(ind_idx);
    if (reg[0x92] & 0x800u) ind_idx += 4;
    return v;
  }
  if (r == 0x1ACu) return ind_idx;
#ifdef AMD_TELA2
  /* DC_COMBOPHYTXREGSn_CMD_BUS_TX_CONTROL_LANEn (0x9nn0): bit 8 = pista
   * pronta (DIG2TransmitterControl, 0xD968, espera antes e depois) */
  if (r >= 0x9000u && r < 0xA000u && (r & 0xFu) == 0u) return reg[r] | 0x100u;
#endif
  return r < NREG ? reg[r] : 0;
}

void sim_w(uint32_t r, uint32_t v) {
  if (sim_traco && r != 0x1ACu && r != 0x1ADu) printf("  w %04X <- %08X\n", r, v);
  if (r == 0x1ACu) {
    ind_idx = v;
    return;
  }
  if (r == 0x1ADu) {
    if ((ind_idx & 0xFFFFFF00u) != 0xC0600000u) data_escritas++;
    smc_w(ind_idx, v);
    if (reg[0x92] & 0x800u) ind_idx += 4;
    return;
  }
  if (r >= NREG) return;
  reg[r] = v;
#ifdef AMD_TELA2
  /* DC_COMBOPHYTXREGSn_CMD_BUS_GLOBAL_FOR_TX_LANEn (0x9nn2): o VBIOS pede
   * pelo bit 12 e espera a resposta no bit 13 (SetPixelClock, 0xB4F0:
   * espera 13 = 0, liga 12, espera 13 = 1); o PHY responde na hora */
  if (r >= 0x9000u && r < 0xA000u && (r & 0xFu) == 2u) reg[r] = (v & 0x1000u) ? (v | 0x2000u) : (v & ~0x2000u);
  /* DC_COMBOPHYPLLREGSn_PLL_WRAP_CNTRL (0x9n3F): bit 14 liga, bit 13 = travado */
  if (r >= 0x9000u && r < 0xA000u && (r & 0xFFu) == 0x3Fu)
    reg[r] = (v & 0x4000u) ? (v | 0x2000u) : (v & ~0x2000u);
  /* 0x9nB1 (48D1 no bloco do PHY): bit 1 liga, bit 2 responde (0xD991) */
  if (r >= 0x9000u && r < 0xA000u && (r & 0xFFu) == 0xB1u)
    reg[r] = (v & 2u) ? (v | 4u) : (v & ~4u);
#endif
  if (r == 0x94u) {                          /* SMC_MESSAGE_0 */
    uint32_t resp = 1;
    if (v == 0x100u && reg[0xA4] == 0x20000u) {
      test_visto = 1;
      rom_test();
    }
#ifdef AMD_SDMA
    if (v >= 0x250u && v <= 0x254u && !(ram_r(0x3F000u) & 1u)) resp = 0xFE;   /* sem firmware */
    else if (v == 0x250u) drv_dram = (drv_dram & 0xFFFFFFFFu) | (uint64_t)reg[0xA4] << 32;
    else if (v == 0x251u) drv_dram = (drv_dram & ~0xFFFFFFFFull) | reg[0xA4];
    else if (v == 0x252u) smu_dram = (smu_dram & 0xFFFFFFFFu) | (uint64_t)reg[0xA4] << 32;
    else if (v == 0x253u) smu_dram = (smu_dram & ~0xFFFFFFFFull) | reg[0xA4];
    else if (v == 0x254u) (void)smu_load(reg[0xA4]);
#endif
    if (rcu & 0x10000u || (ram_r(0x3F000u) & 1u)) reg[0x95] = resp;   /* SMC_RESP_0 */
  }
#ifdef AMD_SDMA
  if (r == 0x3484u) sdma_anda();             /* SDMA0_GFX_RB_WPTR */
#endif
}

/* ---------- gpu_kapi falsa ---------- */
static char logb[1 << 18];
static size_t logn;
static uint32_t relogio;
static const char* fw_arq;
static uint8_t* fw_mem;       /* conteudo servido por fw_read (pode ser alterado) */
static long fw_tam;

static void s_log(const char* s) {
  size_t n = strlen(s);
  if (logn + n < sizeof(logb)) {
    memcpy(logb + logn, s, n);
    logn += n;
    logb[logn] = 0;
  }
}
static void s_hex(uint32_t v) {
  char t[12];
  snprintf(t, sizeof t, "%08X", v);
  s_log(t);
}
static uint32_t s_pci(uint32_t b, uint32_t d, uint32_t f, uint32_t off) {
  (void)b; (void)d; (void)f;
  switch (off) {
    case 0x08: return 0x030000EFu;
    case 0x10: return 0x0000000Cu;
    case 0x14: return 0x000000FBu;
    case 0x18: return 0x0000000Cu;
    case 0x1C: return 0x000000FCu;
    case 0x20: return 0x0000F001u;
    case 0x24: return 0xF6F00000u;
  }
  return 0;
}
static void s_pciw(uint32_t b, uint32_t d, uint32_t f, uint32_t o, uint32_t v) {
  (void)b; (void)d; (void)f; (void)o; (void)v;
}
static void* s_alloc(uint32_t n, uint32_t a) {
  void* p = 0;
  if (a < 16) a = 16;
  if (posix_memalign(&p, a, n ? n : 1)) return 0;
  return p;
}
static void s_free(void* p) { free(p); }
static uint32_t s_ms(void) { return relogio++; }
static uint8_t* sd_mem;       /* conteudo servido como P10SDMA.BIN (0 = nao ha) */
static long sd_mem_tam;
static int s_fw_size(const char* nome) {
  if (!strcmp(nome, "P10SDMA.BIN")) return sd_mem ? (int)sd_mem_tam : -1;
  if (!fw_mem || strcmp(nome, "P10KSMC.BIN")) return -1;
  return (int)fw_tam;
}
static int s_fw_read(const char* nome, void* buf, uint32_t max) {
  if (!strcmp(nome, "P10SDMA.BIN")) {
    if (!sd_mem) return -1;
    if (max > (uint32_t)sd_mem_tam) max = (uint32_t)sd_mem_tam;
    memcpy(buf, sd_mem, max);
    return (int)max;
  }
  if (!fw_mem || strcmp(nome, "P10KSMC.BIN")) return -1;
  if (max > (uint32_t)fw_tam) max = (uint32_t)fw_tam;
  memcpy(buf, fw_mem, max);
  return (int)max;
}

static struct gpu_kapi kp = {
  GPU_ABI, s_log, s_hex, s_pci, s_pciw, s_alloc, s_free, s_ms,
  GPU_KAPI_EXT, s_fw_size, s_fw_read
};

/* ---------- cenarios ---------- */
static int falhas;

static void vw16(uint32_t o, uint16_t v) { vrom[o] = (uint8_t)v; vrom[o + 1] = (uint8_t)(v >> 8); }

/* Um AtomBIOS minimo: cabecalhos, GPIO_Pin_LUT, conectores DP e HDMI (HPD nos
 * bits 0 e 8 de DC_GPIO_HPD) e dois caminhos. */
static void rom_monta(void) {
  uint32_t i;
  memset(vrom, 0, sizeof vrom);
  vrom_tam = VROM_TAM;
  vw16(0, 0xAA55u);
  vrom[2] = VROM_TAM / 512u;
  memcpy(vrom + 0x30, " 761295520", 10);
  vw16(0x48, 0x100);
  memcpy(vrom + 0x104, "ATOM", 4);
  vw16(0x100 + 0x1E, 0x180);                 /* comandos */
  vw16(0x100 + 0x20, 0x200);                 /* dados */
  vw16(0x200 + 4 + 12 * 2, 0x300);           /* GPIO_Pin_LUT */
  vw16(0x200 + 4 + 22 * 2, 0x400);           /* Object_Header */
  vw16(0x300, 4 + 3 * 4);
  vw16(0x304, 0x488Du); vrom[0x306] = 0; vrom[0x307] = 0x10;
  vw16(0x308, 0x488Du); vrom[0x30A] = 8; vrom[0x30B] = 0x11;
  vw16(0x30C, 0x1234u); vrom[0x30E] = 3; vrom[0x30F] = 0x12;   /* fora do bloco: ignorado */
  vrom[0x402] = 1; vrom[0x403] = 3;
  vw16(0x406, 0x20);                         /* conectores em 0x420 */
  vw16(0x40E, 0x80);                         /* caminhos em 0x480 */
  vrom[0x420] = 2;
  vw16(0x424, 0x3113u); vw16(0x428, 0x40);
  vw16(0x42C, 0x310Cu); vw16(0x430, 0x50);
  vrom[0x440] = 1; vrom[0x441] = 4; vrom[0x442] = 0x90; vrom[0x443] = 0xA0;
  vrom[0x444] = 2; vrom[0x445] = 4; vrom[0x446] = 0x10; vrom[0x447] = 1;
  vrom[0x448] = 0xFF;
  vrom[0x450] = 2; vrom[0x451] = 4; vrom[0x452] = 0x11; vrom[0x453] = 1;
  vrom[0x454] = 0xFF;
  vrom[0x480] = 2;
  vw16(0x484, 0x0008); vw16(0x486, 10); vw16(0x488, 0x3113u); vw16(0x48A, 0x1100); vw16(0x48C, 0x211Eu);
  vw16(0x48E, 0x0080); vw16(0x490, 10); vw16(0x492, 0x310Cu); vw16(0x494, 0x1100); vw16(0x496, 0x221Eu);
  for (i = 0x800; i < VROM_TAM; i++) vrom[i] = 0xFF;
}

static uint32_t rom_fnv(void) {
  uint32_t h = 2166136261u, i;
  for (i = 0; i < VROM_TAM; i++) h = (h ^ vrom[i]) * 16777619u;
  return h;
}

static void placa_nova(void) {
  memset(reg, 0, sizeof reg);
  for (size_t i = 0; i < sizeof smc_ram; i++) smc_ram[i] = (uint8_t)(i * 2654435761u >> 24);
  ind_idx = 0;
  smc_rst = 0;
  smc_ck = 0x01000000u;     /* como no log real: ck_disable 0 */
  smc_pc = 0x2B4Cu;         /* ROM de boot */
  smu_status = 0;
  smu_fw = 0x00030006u;     /* modo protegido, chave de producao */
  rcu = 0x10000u;
  aceito = 0;
  test_visto = 0;
  travado = 0;
  data_escritas = rst_escritas = 0;
  reg[0x150A] = 0x1000;                  /* CONFIG_MEMSIZE */
  reg[0x0809] = 0xF4FFF400u;             /* MC_VM_FB_LOCATION */
  reg[0x95] = 1;                         /* SMC_RESP_0 como no log */
  reg[0x1B9C] = 0x80410311u;             /* CRTC0 ligado */
  reg[0x1A00] = 1;
  reg[0x1A01] = 0x0A10800Au;
  reg[0x1A07] = 0xF4u;
  reg[0x1A06] = W;
  reg[0x1A66] = 0x04000010u;
  rom_monta();
  rom_ix = 0x55u;
  rom_surda = 0;
  reg[0x488F] = 0x1u;        /* DC_GPIO_HPD_Y: monitor no DP, nada no HDMI */
  logn = 0;
  logb[0] = 0;
  relogio = 0;
  fw_mem = fw_ok;
  fw_tam = fw_ok_tam;
  sd_mem = sd_ok;
  sd_mem_tam = sd_ok_tam;
  smu_recusa_load = sdma_surda = sdma_ucode = 0;
  ram_lixo = 0;
  ram_leituras = 0;
  agp_leituras = 0;
  /* MC_VM_AGP_TOP/BOT/BASE como o GOP da RX 570 deixa: abertura ligada (0..4 MB) */
  reg[0x80A] = reg[0x80B] = reg[0x80C] = 0;
  drv_dram = smu_dram = 0;
  sdma_pacotes = 0;
  memset(fb + 0x02000000u, 0, 0x01000000u);   /* area da SDMA */
}

static const struct gpu_drv* liga(void) {
  struct gpu_mode md = { W, H, W * 4u, back, (volatile uint8_t*)fb, W * 4u, AMD_POLARIS10, 1, 0, 0 };
  const struct gpu_drv* d = gpu_driver();
  smc_res = 1;
  cur_ligado = 0;
  if (d->init(&kp, &md) != 0) return 0;
  return d;
}

static void confere(const char* nome, int cond) {
  printf("%-58s %s\n", nome, cond ? "OK" : "FALHOU");
  if (!cond) {
    falhas++;
    printf("---- fim do log ----\n%s-------------\n", logn > 1200 ? logb + logn - 1200 : logb);
  }
}

int main(int argc, char** argv) {
  FILE* f;
  const struct gpu_drv* d;
  uint32_t off, tam;
  uint8_t* ruim;
  fw_arq = argc > 1 ? argv[1] : "../../firmware/amdgpu/polaris10_k_smc.bin";
  if (argc > 2) {
    FILE* g = fopen(argv[2], "rb");
    if (!g) {
      perror(argv[2]);
      return 97;
    }
    fseek(g, 0, SEEK_END);
    sd_ok_tam = ftell(g);
    fseek(g, 0, SEEK_SET);
    sd_ok = malloc((size_t)sd_ok_tam);
    if (fread(sd_ok, 1, (size_t)sd_ok_tam, g) != (size_t)sd_ok_tam) return 96;
    fclose(g);
  }
  f = fopen(fw_arq, "rb");
  if (!f) {
    perror(fw_arq);
    return 99;
  }
  fseek(f, 0, SEEK_END);
  fw_ok_tam = ftell(f);
  fseek(f, 0, SEEK_SET);
  fw_ok = malloc((size_t)fw_ok_tam);
  if (fread(fw_ok, 1, (size_t)fw_ok_tam, f) != (size_t)fw_ok_tam) return 98;
  fclose(f);
  memcpy(&off, fw_ok + 24, 4);
  memcpy(&tam, fw_ok + 20, 4);
  fb = calloc(1, VRAM_TAM);
  back = calloc(W * H, 4);

  /* 1. carga normal */
  placa_nova();
  d = liga();
  confere("1 init ok (diag + carga)", d != 0);
  confere("1 carga devolve 0", smc_res == 0);
  confere("1 log diz RODANDO", strstr(logb, "firmware do SMU RODANDO") != 0);
  /* FIRMWARE_FLAGS (0x3F000) e os SoftRegisters (0x3FA24, 120 bytes) ficam
   * dentro da area do ucode: o driver e o firmware escrevem neles depois da
   * carga (com AMD_SDMA, o UcodeLoadStatus); o resto tem de ser o arquivo */
  confere("1 RAM do SMC = ucode do arquivo (menos flags e soft regs)",
          memcmp(smc_ram + 0x20000u, fw_ok + off, 0x3F000u - 0x20000u) == 0 &&
              memcmp(smc_ram + 0x3F004u, fw_ok + off + (0x3F004u - 0x20000u),
                     0x3FA24u - 0x3F004u) == 0 &&
              memcmp(smc_ram + 0x3FAA0u, fw_ok + off + (0x3FAA0u - 0x20000u),
                     tam - (0x3FAA0u - 0x20000u)) == 0);
  confere("1 escreveu o ucode inteiro (palavras)", data_escritas >= (long)(tam / 4));
  confere("1 auto-incremento desligado no fim", !(reg[0x92] & 0x800u));
  confere("1 SMC rodando no fim (pc >= 0x20100, ck 0)", smc_pc >= 0x20100u && !(smc_ck & 1u));
  confere("1 Test com arg 0x20000", test_visto);
  confere("1 FIRMWARE_FLAGS bit 0", ram_r(0x3F000u) & 1u);
  confere("1 reset solto no fim", !(smc_rst & 1u));
  confere("1 tela achada (CRTC 0)", tela_crtc == 0);
  /* cursor continua funcionando depois da carga */
  {
    uint32_t seta[16 * 16];
    for (int i = 0; i < 256; i++) seta[i] = 0xFFFFFFFFu;
    confere("1 cursor_set depois da carga", d->cursor_set(seta, 16, 16, 0, 0) == 0);
    confere("1 cursor ligado (CUR_CONTROL bit 0)", reg[0x1A66] & 1u);
    confere("1 cursor_move", d->cursor_move(100, 200) == 0 && reg[0x1A6A] == (100u << 16 | 200u));
    d->fini();
    confere("1 fini desliga o cursor", !(reg[0x1A66] & 1u));
  }

  /* 2. firmware ja rodando (segunda entrada no WIN) */
  {
    long antes;
    placa_nova();
    smc_pc = 0x20480u;
    antes = data_escritas;
    d = liga();
    confere("2 ja rodando: devolve 0", d && smc_res == 0);
    confere("2 ja rodando: nao escreve na RAM do SMC", data_escritas == antes && rst_escritas == 0);
    confere("2 log 'ja roda'", strstr(logb, "ja roda") != 0);
    if (d) d->fini();
  }

  /* 3. imagem adulterada: a ROM recusa */
  placa_nova();
  ruim = malloc((size_t)fw_ok_tam);
  memcpy(ruim, fw_ok, (size_t)fw_ok_tam);
  ruim[off + 0x1000] ^= 0x5A;
  fw_mem = ruim;
  d = liga();
  confere("3 adulterado: SMU_PASS 0 -> -4", d && smc_res == -4);
  confere("3 log 'recusou'", strstr(logb, "recusou") != 0);
  confere("3 SMC nao fica rodando", smc_pc < 0x20100u);
  if (d) d->fini();

  /* 4. sem arquivo */
  placa_nova();
  fw_mem = 0;
  d = liga();
  confere("4 sem arquivo: -1 e nao mexe no SMC", d && smc_res == -1 && rst_escritas == 0);
  if (d) d->fini();

  /* 5. modo nao protegido: nao carrega */
  placa_nova();
  smu_fw = 0x00020006u;
  d = liga();
  confere("5 nao protegido: -1 e nao mexe no SMC", d && smc_res == -1 && rst_escritas == 0);
  if (d) d->fini();

  /* 6. ROM nao liga as interrupcoes: espera com limite */
  placa_nova();
  travado = 1;
  rcu = 0;
  d = liga();
  confere("6 travado: -2 dentro do limite", d && smc_res == -2);
  confere("6 relogio andou (limite em ms)", relogio > 1000u);
  if (d) d->fini();

  /* 7. cabecalho invalido (tamanho errado no cabecalho) */
  placa_nova();
  memcpy(ruim, fw_ok, (size_t)fw_ok_tam);
  ruim[0] ^= 0x10;
  fw_mem = ruim;
  d = liga();
  confere("7 cabecalho invalido: -1 e nao mexe no SMC", d && smc_res == -1 && rst_escritas == 0);
  if (d) d->fini();

  /* 8. kernel antigo (sem a extensao da gpu_kapi) */
  placa_nova();
  kp.ext = 0;
  d = liga();
  confere("8 gpu_kapi antiga: -1 e nao mexe no SMC", d && smc_res == -1 && rst_escritas == 0);
  kp.ext = GPU_KAPI_EXT;
  if (d) d->fini();

  /* 9. indice do SMC devolvido ao valor de antes */
  placa_nova();
  ind_idx = 0x12340u;
  d = liga();
  confere("9 SMC_IND_INDEX_11 devolvido", d && ind_idx == 0x12340u);
  if (d) d->fini();

#ifdef AMD_SDMA
  /* ---- etapa E: SDMA0 ---- */
  if (!sd_ok) {
    printf("sem polaris10_sdma.bin (2o argumento): cenarios da SDMA pulados\n");
    falhas++;
  } else {
    uint32_t i, ruins = 0;
    /* 10. carga do ucode pelo SMU, anel e testes */
    placa_nova();
    d = liga();
    confere("10 SMC carregado e SDMA ligada", d && smc_res == 0 && sd_res == 0);
    confere("10 log 'SDMA0 FUNCIONANDO'", strstr(logb, "SDMA0 FUNCIONANDO") != 0);
    confere("10 SMU recebeu TOC e SMU_DRAM na VRAM",
            drv_dram == VRAM_MC + 0x02000000u && smu_dram == VRAM_MC + 0x02100000u);
    confere("10 ucode da SDMA0 carregado (UcodeLoadStatus)", sdma_ucode);
    confere("10 anel na VRAM (RB_BASE) e tamanho 4 KB",
            ((uint64_t)reg[0x3481] << 8) == VRAM_MC + 0x02200000u && ((reg[0x3480] >> 1) & 0x1F) == 10);
    confere("10 F32 solto, anel e IB ligados",
            !(reg[0x3412] & 1u) && (reg[0x3480] & 1u) && (reg[0x348A] & 1u));
    confere("10 rptr alcancou wptr", reg[0x3483] == reg[0x3484]);
    /* depois da medida da CPU (que zera B) vem o teste da RAM, que zera de novo
     * os primeiros 2 MB de B pela SDMA e poe a sub-janela: o fim de B fica 0 */
    for (i = 0x200000u; i < 0x400000u; i += 4096u) {
      uint32_t a, b;
      memcpy(&a, fb + 0x02800000u + i, 4);
      memcpy(&b, fb + 0x02800000u + i + 4092u, 4);
      if (a != 0 || b != 0) ruins++;
    }
    confere("10 fim da area B zerado pela CPU (medida)", ruins == 0);
    confere("10 pacotes executados (escrita, 2 fill, 4 copias, 3 fences)", sdma_pacotes >= 10);
    if (d) d->fini();
    confere("10 fini para a SDMA (F32 HALT)", (reg[0x3412] & 1u) && !(reg[0x3480] & 1u));

    /* 11. SMU nao carrega: SDMA nao liga */
    placa_nova();
    smu_recusa_load = 1;
    d = liga();
    confere("11 SMU nao carrega: sd_res -3, anel desligado", d && sd_res == -3 && !(reg[0x3480] & 1u));
    if (d) d->fini();

    /* 12. SDMA nao anda: fence nao vem, SDMA parada */
    placa_nova();
    sdma_surda = 1;
    d = liga();
    confere("12 SDMA parada: sd_res -11 dentro do limite", d && sd_res == -11);
    confere("12 log 'sem fence'", strstr(logb, "sem fence") != 0);
    confere("12 SDMA deixada em HALT", (reg[0x3412] & 1u) && !(reg[0x3480] & 1u));
    if (d) d->fini();

    /* 13. sem o arquivo da SDMA */
    placa_nova();
    sd_mem = 0;
    d = liga();
    confere("13 sem P10SDMA.BIN: sd_res -1, anel desligado", d && sd_res == -1 && !(reg[0x3480] & 1u));
    if (d) d->fini();

    /* 14. present pela SDMA lendo o back buffer na RAM (modo PA) */
    {
      struct gpu_rect rr[2] = { { 10, 20, 300, 200 }, { 1500, 900, 600, 400 } };   /* o 2o passa da borda */
      uint32_t x, y, ok = 1, fora = 1;
      uint32_t* tela = (uint32_t*)fb;   /* framebuffer = inicio da VRAM, pitch W */
      placa_nova();
      d = liga();
      confere("14 teste da RAM passou: present pela placa", d && sd_res == 0 && sd_present_ok == 1);
      confere("14 log 'present passa a ser pela placa'", strstr(logb, "present passa a ser pela placa") != 0);
      confere("14 a placa leu a RAM (modo PA)", ram_leituras > 0);
      confere("14 abertura AGP desligada antes de ler a RAM (BOT > TOP)",
              (reg[0x80B] & 0x3FFFFu) > (reg[0x80A] & 0x3FFFFu) && agp_leituras == 0);
      confere("14 teste do proprio back buffer no log", strstr(logb, "back buffer (fisico") != 0);
      for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) back[y * W + x] = 0x00100000u * (x & 0xF) + y * 7u + x;
      memset(fb, 0, W * H * 4u);
      d->present(rr, 2);
      for (y = 20; y < 220; y += 3)
        for (x = 10; x < 310; x += 5)
          if (tela[y * W + x] != back[y * W + x]) ok = 0;
      for (y = 900; y < H; y += 3)
        for (x = 1500; x < W; x += 5)
          if (tela[y * W + x] != back[y * W + x]) ok = 0;
      if (tela[19 * W + 10] || tela[20 * W + 9] || tela[20 * W + 310] || tela[220 * W + 10] ||
          tela[899 * W + 1500])
        fora = 0;
      confere("14 areas copiadas pela SDMA (recortadas na borda)", ok && sd_quadros == 1);
      confere("14 quadro dividido: parte pela SDMA, parte pela CPU", sd_px_sdma > 0 && sd_px_cpu > 0);
      confere("14 nada fora das areas", fora);
      memset(fb, 0, W * H * 4u);
      d->present(0, 0);
      ok = 1;
      for (y = 0; y < H; y += 17)
        for (x = 0; x < W; x += 13)
          if (tela[y * W + x] != back[y * W + x]) ok = 0;
      confere("14 quadro inteiro pela SDMA", ok && sd_quadros == 2);
      /* a SDMA para no meio da sessao: a CPU assume o quadro */
      /* area pequena: so a CPU (abaixo de SD_MIN_PX) */
      {
        uint32_t q = sd_quadros;
        uint64_t ps = sd_px_sdma;
        struct gpu_rect pq = { 40, 40, 101, 100 };   /* largura impar: rep movsq + 1 pixel */
        memset(fb, 0, W * H * 4u);
        d->present(&pq, 1);
        confere("14 area pequena: so a CPU, certa (largura impar, ultimo pixel)",
                sd_quadros == q && sd_px_sdma == ps && tela[90 * W + 90] == back[90 * W + 90] &&
                    tela[90 * W + 140] == back[90 * W + 140] && tela[90 * W + 141] == 0);
      }
      sdma_surda = 1;
      memset(fb, 0, W * H * 4u);
      d->present(0, 0);
      ok = 1;
      for (y = 0; y < H; y += 17)
        for (x = 0; x < W; x += 13)
          if (tela[y * W + x] != back[y * W + x]) ok = 0;
      confere("14 SDMA parou: a CPU copiou o quadro inteiro", ok);
      confere("14 caminho da SDMA desligado e SDMA em HALT",
              sd_present_ok == 0 && (reg[0x3412] & 1u) && strstr(logb, "present pela SDMA falhou") != 0);
      d->fini();
      confere("14 fini devolve a abertura AGP do GOP", reg[0x80A] == 0 && reg[0x80B] == 0);
    }

    /* 15. a placa le lixo da RAM: o present continua pela CPU, SDMA ligada */
    placa_nova();
    ram_lixo = 1;
    d = liga();
    confere("15 RAM ilegivel: sd_res 0, present pela CPU", d && sd_res == 0 && sd_present_ok == 0);
    confere("15 log 'nao leu a RAM'", strstr(logb, "nao leu a RAM") != 0);
    if (d) d->fini();
  }
#endif

  /* 16. etapa M: VBIOS pela ROM do SMC, conectores, HPD e copia no log */
  {
    char esperado[64];
    placa_nova();
    ind_idx = 0x1234u;
    d = liga();
    confere("16 init ok com a leitura do VBIOS", d != 0);
    confere("16 conector 0: DisplayPort, i2c 90, MONITOR",
            strstr(logb, "conector 00000000 id 00003113 DisplayPort i2c 00000090 hpd_gpio 00000010 "
                         "pino 0000488D.00000000 nivel 00000001 (ligado = 00000001) MONITOR") != 0);
    confere("16 conector 1: HDMI sem monitor (bit 8 em 0)",
            strstr(logb, "conector 00000001 id 0000310C HDMI i2c FFFFFFFF hpd_gpio 00000011 "
                         "pino 0000488D.00000008 nivel 00000000 (ligado = 00000001)\n") != 0);
    confere("16 mascara de monitores = 1", strstr(logb, "com monitor (mascara) 00000001") != 0);
    confere("16 caminhos com os codificadores",
            strstr(logb, "caminho 00000001 disp 00000080 conector 0000310C gpu 00001100 "
                         "codificadores 0000221E") != 0);
    snprintf(esperado, sizeof esperado, "bytes 00001000 fnv1a %08X", rom_fnv());
    confere("16 copia inteira no log (fnv confere)", strstr(logb, esperado) != 0);
#ifdef AMD_VBIOS_COPIA
    confere("16 linhas so de FF resumidas", strstr(logb, "[vbios] 00800 =FF") != 0 &&
                                                strstr(logb, "[vbios] 00000 55AA08") != 0);
#else
    confere("16 sem a copia no log (so o fnv)", strstr(logb, "[vbios] 00000 ") == 0);
#endif
    confere("16 indice do SMC e auto-incremento devolvidos",
            ind_idx == 0x1234u && !(reg[0x92] & 0x800u));
    if (getenv("SIM_LOG")) {   /* para testar o vbios-extrai.py */
      FILE* g = fopen(getenv("SIM_LOG"), "wb");
      if (g) {
        fwrite(logb, 1, logn, g);
        fclose(g);
      }
      g = fopen("/tmp/sodos-vrom.bin", "wb");
      if (g) {
        fwrite(vrom, 1, VROM_TAM, g);
        fclose(g);
      }
    }
    if (d) d->fini();
    placa_nova();
    reg[0x488F] = 0x101u;
    d = liga();
    confere("16 dois monitores: mascara 3", d && strstr(logb, "com monitor (mascara) 00000003") != 0);
    if (d) d->fini();
    placa_nova();
    rom_surda = 1;
    d = liga();
    confere("16 ROM surda: init ok e log 'nao respondeu'",
            d && strstr(logb, "ROM nao respondeu") != 0 && strstr(logb, "conector") == 0);
    if (d) d->fini();
  }

#ifdef AMD_TELA2
  /* 17. etapa M, parte 2: as tabelas do AtomBIOS da RX 570 de verdade (a ROM
   * tirada da placa, argv[3]) sobre os registradores do simulador. A tela
   * principal esta no DIG2 (DP 3, UNIPHY1 link A); a segunda, no HDMI. */
  if (argc > 3) {
    FILE* g = fopen(argv[3], "rb");
    long n;
    if (!g) {
      perror(argv[3]);
      return 95;
    }
    placa_nova();
    n = (long)fread(vrom, 1, sizeof vrom, g);
    fclose(g);
    vrom_tam = (uint32_t)n;
    reg[0x488F] = 0x04000001u;            /* HPD: DP 3 (bit 0) e HDMI (bit 26) */
    reg[0x4C00] = 0x01000000u;            /* DIG2 FE: CRTC 0, SYMCLK_FE_ON */
    reg[0x4C48] = 0x00000101u;            /* DIG2 BE: ligado */
    reg[0x4C47] = 0x00000000u;            /* DIG2 BE: modo DP */
    reg[0x0321] = reg[0x0322] = 0x12u;    /* DMIF: alocacao concluida */
    sim_traco = getenv("SIM_TRACE") ? atoi(getenv("SIM_TRACE")) : 0;
    d = liga();
    sim_traco = 0;
    confere("17 init ok com a ROM real", d != 0);
    confere("17 conectores da ROM real: 4, DP 3 e HDMI com monitor",
            vb_ncon == 4 && vb_con[2].nivel == 1 && vb_con[3].nivel == 1 && vb_con[3].hpd == 4);
    confere("17 tela principal: DIG2", strstr(logb, "DIG 00000002 fe 01000000") != 0 &&
                                         strstr(logb, "<- tela principal") != 0);
    confere("17 segunda tela: conector 3 (HDMI), DIG3, PHY D",
            strstr(logb, "segunda tela: conector 00000003 dig 00000003 phy 00000003") != 0);
    confere("17 nenhuma tabela abortou", strstr(logb, "ABORTOU") == 0);
    confere("17 todas as tabelas rodaram",
            strstr(logb, "SetPixelClock v7 (148,5 MHz) v00000107 ok") != 0 &&
                strstr(logb, "SetCRTC_UsingDTDTiming (1920x1080) v") != 0 &&
                strstr(logb, "UNIPHYTransmitterControl v6 ENABLE v00000106 ok") != 0);
    confere("17 log 'segunda tela LIGADA'", strstr(logb, "segunda tela LIGADA") != 0);
    confere("17 CRTC 1 com a superficie do CRTC 0",
            reg[0x1A04 + 0x200] == reg[0x1A04] && reg[0x1A07 + 0x200] == reg[0x1A07] &&
                reg[0x1A06 + 0x200] == reg[0x1A06] && (reg[0x1A00 + 0x200] & 1u));
    confere("17 CRTC 0 e DIG2 intactos",
            reg[0x1B9C] == 0x80410311u && reg[0x4C00] == 0x01000000u && reg[0x4C48] == 0x101u);
    if (d) {
      uint32_t seta[16 * 16];
      for (int i = 0; i < 256; i++) seta[i] = 0xFFFFFFFFu;
      confere("17 cursor nas duas telas",
              d->cursor_set(seta, 16, 16, 0, 0) == 0 && d->cursor_move(300, 400) == 0 &&
                  (reg[0x1A66] & 1u) && (reg[0x1A66 + 0x200] & 1u) &&
                  reg[0x1A6A + 0x200] == reg[0x1A6A] && reg[0x1A67 + 0x200] == reg[0x1A67]);
    }
    /* tela estendida: imagem 3840x1080 na VRAM, CRTC 0 na esquerda e CRTC 1
     * na direita; o present copia o quadro largo; UMA e o suspend voltam */
    if (d) {
      struct gpu_telas t;
      struct gpu_mode mw;
      uint32_t* larga = calloc(2u * W * H, 4);
      uint32_t* tela = (uint32_t*)fb;
      int ok = 1;
      uint32_t x, y;
      confere("17 telas: consulta 2 telas, 3840x1080, segunda em 1920",
              d->telas && (d->caps & GPU_CAP_TELAS) && d->telas(GPU_TELAS_CONSULTA, &t, 0) == 0 &&
                  t.n == 2 && t.w == 2u * W && t.h == H && t.x2 == W && t.fb_pitch == 2u * W * 4u);
      mw.w = 2u * W;
      mw.h = H;
      mw.pitch = 2u * W * 4u;
      mw.back = larga;
      mw.fb = t.fb;
      mw.fb_pitch = t.fb_pitch;
      mw.pci_id = AMD_POLARIS10;
      mw.pci_bus = 1;
      mw.pci_dev = mw.pci_fun = 0;
      confere("17 telas: estende", d->telas(GPU_TELAS_ESTENDE, &t, &mw) == 0);
      confere("17 telas: CRTC 0 e 1 com pitch 3840, CRTC 1 a partir de x 1920",
              reg[0x1A06] == 2u * W && reg[0x1A06 + 0x200] == 2u * W && reg[0x1B5C] == 0 &&
                  reg[0x1B5C + 0x200] == (W << 16) && reg[0x1A0D + 0x200] == 2u * W);
      for (y = 0; y < H; y++)
        for (x = 0; x < 2u * W; x++) larga[y * 2u * W + x] = x * 3u + y * 7919u;
      memset(fb, 0, 2u * W * H * 4u);
      d->present(0, 0);
      for (y = 0; y < H; y += 7)
        for (x = 0; x < 2u * W; x += 11)
          if (tela[y * 2u * W + x] != larga[y * 2u * W + x]) ok = 0;
      confere("17 telas: quadro largo inteiro na VRAM (SDMA + CPU)", ok);
      ok = 1;
      memset(fb, 0, 2u * W * H * 4u);
      {
        struct gpu_rect r = { 2000, 100, 1500, 600 };   /* so na segunda tela */
        d->present(&r, 1);
      }
      for (y = 100; y < 700; y += 5)
        for (x = 2000; x < 3500; x += 9)
          if (tela[y * 2u * W + x] != larga[y * 2u * W + x]) ok = 0;
      confere("17 telas: area na segunda tela", ok && tela[99 * 2u * W + 2000] == 0);
      confere("17 telas: suspend volta o pitch do GOP (tela de texto)",
              d->suspend() == 0 && reg[0x1A06] == W && reg[0x1B5C + 0x200] == 0);
      confere("17 telas: resume + estende de novo", d->resume(&mw) == 0 &&
                                                       d->telas(GPU_TELAS_ESTENDE, &t, &mw) == 0 &&
                                                       reg[0x1A06] == 2u * W);
      confere("17 telas: UMA volta a uma tela", d->telas(GPU_TELAS_UMA, &t, 0) == 0 &&
                                                    reg[0x1A06] == W && reg[0x1A06 + 0x200] == W);
      d->telas(GPU_TELAS_ESTENDE, &t, &mw);
      d->fini();
      confere("17 telas: fini volta o pitch do GOP", reg[0x1A06] == W);
      free(larga);
      d = 0;
    }
    if (getenv("SIM_LOG17")) {
      FILE* h = fopen(getenv("SIM_LOG17"), "wb");
      if (h) {
        fwrite(logb, 1, logn, h);
        fclose(h);
      }
    }
    if (d) d->fini();
    confere("17 fini desliga a segunda tela", strstr(logb, "segunda tela desligada (fim)") != 0);
  } else {
    printf("17 (sem a ROM da RX 570 em argv[3]: pulado)\n");
  }
#endif

  printf("\n%d falha(s)\n", falhas);
  return falhas;
}
