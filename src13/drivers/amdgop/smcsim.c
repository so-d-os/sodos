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
  }
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
static const uint8_t* le_mem(uint64_t mc, uint32_t n) {
  static uint8_t lixo[1 << 22];
  if (mc >= VRAM_MC && mc + n <= VRAM_MC + VRAM_TAM) return fb + (mc - VRAM_MC);
  if (mc >= VRAM_MC && mc < VRAM_MC + 0x100000000ull) {
    printf("  !! leitura fora da VRAM simulada: %llx\n", (unsigned long long)mc);
    return 0;
  }
  ram_leituras++;
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
  if (r == 0x1ADu) {
    v = smc_r(ind_idx);
    if (reg[0x92] & 0x800u) ind_idx += 4;
    return v;
  }
  if (r == 0x1ACu) return ind_idx;
  return r < NREG ? reg[r] : 0;
}

void sim_w(uint32_t r, uint32_t v) {
  if (r == 0x1ACu) {
    ind_idx = v;
    return;
  }
  if (r == 0x1ADu) {
    data_escritas++;
    smc_w(ind_idx, v);
    if (reg[0x92] & 0x800u) ind_idx += 4;
    return;
  }
  if (r >= NREG) return;
  reg[r] = v;
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
static char logb[1 << 16];
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
#define W 1920u
#define H 1080u
static uint32_t* back;
static int falhas;

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
      confere("14 nada fora das areas", fora);
      memset(fb, 0, W * H * 4u);
      d->present(0, 0);
      ok = 1;
      for (y = 0; y < H; y += 17)
        for (x = 0; x < W; x += 13)
          if (tela[y * W + x] != back[y * W + x]) ok = 0;
      confere("14 quadro inteiro pela SDMA", ok && sd_quadros == 2);
      /* a SDMA para no meio da sessao: a CPU assume o quadro */
      sdma_surda = 1;
      memset(fb, 0, W * H * 4u);
      d->present(rr, 1);
      ok = 1;
      for (y = 20; y < 220; y += 3)
        for (x = 10; x < 310; x += 5)
          if (tela[y * W + x] != back[y * W + x]) ok = 0;
      confere("14 SDMA parou: a CPU copiou o quadro", ok);
      confere("14 caminho da SDMA desligado e SDMA em HALT",
              sd_present_ok == 0 && (reg[0x3412] & 1u) && strstr(logb, "present pela SDMA falhou") != 0);
      d->fini();
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

  printf("\n%d falha(s)\n", falhas);
  return falhas;
}
