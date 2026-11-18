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
    if (v == 0x100u && reg[0xA4] == 0x20000u) rom_test();
    if (rcu & 0x10000u) reg[0x95] = 1;       /* SMC_RESP_0 */
  }
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
static int s_fw_size(const char* nome) {
  if (!fw_mem || strcmp(nome, "P10KSMC.BIN")) return -1;
  return (int)fw_tam;
}
static int s_fw_read(const char* nome, void* buf, uint32_t max) {
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
static uint8_t* fb;
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
    printf("---- log ----\n%s-------------\n", logb);
  }
}

int main(int argc, char** argv) {
  FILE* f;
  const struct gpu_drv* d;
  uint32_t off, tam;
  uint8_t* ruim;
  fw_arq = argc > 1 ? argv[1] : "../../firmware/amdgpu/polaris10_k_smc.bin";
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
  fb = calloc(1, 0x01000000u + 128u * 128u * 4u + 4096u);
  back = calloc(W * H, 4);

  /* 1. carga normal */
  placa_nova();
  d = liga();
  confere("1 init ok (diag + carga)", d != 0);
  confere("1 carga devolve 0", smc_res == 0);
  confere("1 log diz RODANDO", strstr(logb, "firmware do SMU RODANDO") != 0);
  /* FIRMWARE_FLAGS (0x3F000) fica dentro da area do ucode: o driver zera e o
   * firmware liga o bit 0 depois da carga; o resto tem de ser o arquivo */
  confere("1 RAM do SMC = ucode do arquivo (menos FIRMWARE_FLAGS)",
          memcmp(smc_ram + 0x20000u, fw_ok + off, 0x3F000u - 0x20000u) == 0 &&
              memcmp(smc_ram + 0x3F004u, fw_ok + off + (0x3F004u - 0x20000u),
                     tam - (0x3F004u - 0x20000u)) == 0);
  confere("1 escreveu o ucode inteiro (palavras)", data_escritas >= (long)(tam / 4));
  confere("1 auto-incremento desligado no fim", !(reg[0x92] & 0x800u));
  confere("1 SMC rodando no fim (pc >= 0x20100, ck 0)", smc_pc >= 0x20100u && !(smc_ck & 1u));
  confere("1 Test com arg 0x20000", reg[0xA4] == 0x20000u && reg[0x94] == 0x100u);
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

  printf("\n%d falha(s)\n", falhas);
  return falhas;
}
