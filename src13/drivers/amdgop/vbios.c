/* vbios.c - Etapa M da RX 570 (dois monitores), parte 1: o que a placa tem.
 * Incluido por amdgop.c (com AMD_SMC). Feito em 2026-11-18.
 *
 * Para acender a segunda saida e preciso rodar as tabelas do AtomBIOS desta
 * placa (SetPixelClock, DIGxEncoderControl, UNIPHYTransmitterControl,
 * EnableCRTC...), como o Linux faz em amdgpu_atombios_crtc.c e
 * amdgpu_atombios_encoders.c. Esta parte so le:
 *  - o VBIOS, pela interface de ROM do SMC (vi_read_bios_from_rom do Linux:
 *    SMC_IND_INDEX_11 = ixROM_INDEX, DATA = 0, depois ixROM_DATA e leituras
 *    seguidas, com o indice avancando sozinho);
 *  - a tabela de objetos do AtomBIOS: conectores, I2C (DDC), pino do HPD e
 *    os caminhos conector -> codificador;
 *  - o nivel de cada pino de HPD (DC_GPIO_HPD_Y): monitor ligado ou nao;
 *  - a temporizacao dos CRTCs 0 e 1;
 * e copia o VBIOS para o SERIAL.LOG ([vbios] ...), para o interpretador do
 * AtomBIOS ser testado no PC de desenvolvimento com a ROM desta placa antes
 * de mexer na segunda saida. A unica escrita e o indice da ROM (devolvido). */

#define VB_MAX 0x20000u
#define VB_DUMP_MAX 0x10000u      /* as tabelas do AtomBIOS ficam na imagem legada */
#define VB_DT_GPIO_PIN_LUT 12u    /* ATOM_MASTER_LIST_OF_DATA_TABLES */
#define VB_DT_OBJECT_HEADER 22u
#define PL_DC_GPIO_HPD_A 0x488Du
#define PL_DC_GPIO_HPD_Y 0x488Fu
#define PL_CRTC_H_TOTAL 0x1B80u
#define PL_CRTC_H_BLANK 0x1B81u
#define PL_CRTC_H_SYNC_A 0x1B82u
#define PL_CRTC_V_TOTAL 0x1B87u
#define PL_CRTC_V_BLANK 0x1B8Du
#define PL_CRTC_V_SYNC_A 0x1B8Eu
#define PL_CRTC_BLANK_CONTROL 0x1B9Du

static uint8_t* vb;
static uint32_t vb_tam;

static uint8_t vb8(uint32_t o) { return o < vb_tam ? vb[o] : 0; }
static uint16_t vb16(uint32_t o) { return (uint16_t)(vb8(o) | vb8(o + 1u) << 8); }

/* ixROM_INDEX / ixROM_DATA: 0xC0600010/14 (smu_7_1_1_d.h); o segundo par e
 * o de outras geracoes. So vale o par que devolve 0xAA55 no byte 0. */
static const uint32_t vb_rom_ix[2][2] = {
  { 0xC0600010u, 0xC0600014u }, { 0xC0600028u, 0xC060002Cu } };

static int vb_le(void) {
  uint32_t idx = pl_r(PL_SMC_IND_INDEX_11), acc = pl_r(PL_SMC_IND_ACCESS_CNTL);
  uint32_t w0 = 0, i, c, tam;
  int par = -1;
  if (acc & PL_AUTOINC_11) pl_w(PL_SMC_IND_ACCESS_CNTL, acc & ~PL_AUTOINC_11);
  for (c = 0; c < 2 && par < 0; c++) {
    uint32_t antes = pl_smc_r(vb_rom_ix[c][0]);
    pl_smc_w(vb_rom_ix[c][0], 0);
    pl_w(PL_SMC_IND_INDEX_11, vb_rom_ix[c][1]);
    w0 = pl_r(PL_SMC_IND_DATA_11);
    if ((w0 & 0xFFFFu) == 0xAA55u) par = (int)c;
    else pl_smc_w(vb_rom_ix[c][0], antes);
  }
  if (par < 0) {
    log_kv("[vbios] ROM nao respondeu (palavra 0: ", w0);
    k->log(")\n");
    pl_w(PL_SMC_IND_ACCESS_CNTL, acc);
    pl_w(PL_SMC_IND_INDEX_11, idx);
    return -1;
  }
  tam = ((w0 >> 16) & 0xFFu) * 512u;
  if (tam < 0x1000u || tam > VB_MAX) tam = VB_DUMP_MAX;
  vb = (uint8_t*)k->alloc(tam, 16);
  if (vb) {
    pl_smc_w(vb_rom_ix[par][0], 0);
    pl_w(PL_SMC_IND_INDEX_11, vb_rom_ix[par][1]);
    for (i = 0; i < tam; i += 4) {
      uint32_t v = pl_r(PL_SMC_IND_DATA_11);
      vb[i] = (uint8_t)v;
      vb[i + 1] = (uint8_t)(v >> 8);
      vb[i + 2] = (uint8_t)(v >> 16);
      vb[i + 3] = (uint8_t)(v >> 24);
    }
    vb_tam = tam;
  }
  pl_w(PL_SMC_IND_ACCESS_CNTL, acc);
  pl_w(PL_SMC_IND_INDEX_11, idx);
  log_kv("[vbios] ROM pelo SMC (indice ", vb_rom_ix[par][0]);
  log_kv(") bytes ", tam);
  k->log("\n");
  return vb ? 0 : -1;
}

static int vb_igual(uint32_t o, const char* s) {
  for (; *s; s++, o++)
    if (vb8(o) != (uint8_t)*s) return 0;
  return 1;
}

static const char* vb_conector(uint16_t id) {
  switch (id & 0xFFu) {
    case 0x01: return "DVI-I";
    case 0x02: return "DVI-I dual";
    case 0x03: return "DVI-D";
    case 0x04: return "DVI-D dual";
    case 0x05: return "VGA";
    case 0x0C: return "HDMI";
    case 0x0D: return "HDMI-B";
    case 0x13: return "DisplayPort";
    case 0x14: return "eDP";
    default: return "?";
  }
}

/* Pino do HPD: GPIO_Pin_LUT da o registrador A do pino e o bit; o nivel esta
 * no Y (MASK, A, EN, Y seguidos, como DC_GPIO_HPD_*). -1 = nao achou. */
static int vb_hpd(uint32_t lut, uint8_t gpio, uint32_t* reg, uint32_t* bit) {
  uint32_t fim = lut + vb16(lut), e;
  if (!lut) return -1;
  for (e = lut + 4u; e + 4u <= fim; e += 4u) {
    if (vb8(e + 3u) != gpio) continue;
    *reg = vb16(e);
    *bit = vb8(e + 2u);
    if (*reg < 0x4800u || *reg > 0x49FFu || *bit > 31u) return -1;   /* fora do bloco de GPIO */
    return (int)((pl_r(*reg + 2u) >> *bit) & 1u);
  }
  return -1;
}

static uint32_t vb_saidas_ligadas;   /* conectores com o pino de HPD no nivel de ligado */

/* Conectores da tabela de objetos, para a segunda tela (tela2.c) */
#define VB_MAX_CON 8
struct vb_con {
  uint16_t id;        /* objeto do conector (0x3113 = DP, enum 1) */
  uint16_t enc;       /* codificador do caminho (0x2120 = UNIPHY1, link A) */
  int i2c;            /* ATOM_I2C_ID_CONFIG do DDC/AUX; -1 = nao ha */
  int hpd;            /* HPD1..6 = 0..5, pelo bit do pino em DC_GPIO_HPD; -1 = nao ha */
  int nivel;          /* pino do HPD: 1 = monitor */
};
static struct vb_con vb_con[VB_MAX_CON];
static uint32_t vb_ncon;

/* bit do pino em DC_GPIO_HPD_A -> HPDn (DC_GPIO_HPD_A__DC_GPIO_HPD1..6_A_MASK) */
static int vb_hpd_id(uint32_t reg, uint32_t bit) {
  static const uint8_t b[6] = { 0, 8, 16, 24, 26, 28 };
  int i;
  if (reg != PL_DC_GPIO_HPD_A) return -1;
  for (i = 0; i < 6; i++)
    if (b[i] == bit) return i;
  return -1;
}

static void vb_objetos(void) {
  uint32_t rom = vb16(0x48), dat, obj, lut, con, cam, n, j, p;
  dat = vb16(rom + 0x20u);
  obj = vb16(dat + 4u + VB_DT_OBJECT_HEADER * 2u);
  lut = vb16(dat + 4u + VB_DT_GPIO_PIN_LUT * 2u);
  log_kv("[vbios] tabelas: comandos ", vb16(rom + 0x1Eu));
  log_kv(" dados ", dat);
  log_kv(" objetos ", obj);
  log_kv(" (versao ", (uint32_t)vb8(obj + 2u) << 8 | vb8(obj + 3u));
  log_kv(") gpio_lut ", lut);
  k->log("\n");
  if (!obj || obj >= vb_tam) return;
  con = obj + vb16(obj + 6u);
  cam = obj + vb16(obj + 14u);
  n = vb8(con);
  vb_saidas_ligadas = 0;
  vb_ncon = 0;
  for (j = 0; j < n && j < 16u; j++) {
    uint32_t e = con + 4u + j * 8u, r = obj + vb16(e + 4u), voltas;
    uint16_t id = vb16(e);
    int i2c = -1, gpio = -1, estado = 0, nivel = -1;
    uint32_t reg = 0, bit = 0;
    for (voltas = 0; voltas < 32u && vb8(r) != 0xFFu && vb8(r + 1u) != 0; voltas++) {
      if (vb8(r) == 1u) i2c = vb8(r + 2u);                   /* ATOM_I2C_RECORD */
      if (vb8(r) == 2u) { gpio = vb8(r + 2u); estado = vb8(r + 3u); }   /* ATOM_HPD_INT_RECORD */
      r += vb8(r + 1u);
    }
    if (gpio >= 0) nivel = vb_hpd(lut, (uint8_t)gpio, &reg, &bit);
    if (vb_ncon < VB_MAX_CON) {
      vb_con[vb_ncon].id = id;
      vb_con[vb_ncon].enc = 0;
      vb_con[vb_ncon].i2c = i2c;
      vb_con[vb_ncon].hpd = gpio >= 0 ? vb_hpd_id(reg, bit) : -1;
      vb_con[vb_ncon].nivel = nivel;
      vb_ncon++;
    }
    log_kv("[vbios] conector ", j);
    log_kv(" id ", id);
    k->log(" ");
    k->log(vb_conector(id));
    log_kv(" i2c ", (uint32_t)i2c);
    log_kv(" hpd_gpio ", (uint32_t)gpio);
    log_kv(" pino ", reg);
    log_kv(".", bit);
    log_kv(" nivel ", (uint32_t)nivel);
    log_kv(" (ligado = ", (uint32_t)estado);
    k->log(")");
    /* HPD e ativo em 1 (o monitor puxa o pino para cima). O ucPlugged_PinState
     * da RX 570 e 0 em todos os conectores: nao serve (2026-11-18, primeiro
     * log: so os pinos do DP 3 e do HDMI em 1, com os dois monitores neles) */
    if (nivel == 1) {
      vb_saidas_ligadas |= 1u << j;
      k->log(" MONITOR");
    }
    k->log("\n");
  }
  n = vb8(cam);
  p = cam + 4u;
  for (j = 0; j < n && j < 16u; j++) {
    uint32_t t = vb16(p + 2u), g;
    if (t < 8u) break;
    log_kv("[vbios] caminho ", j);
    log_kv(" disp ", vb16(p));
    log_kv(" conector ", vb16(p + 4u));
    log_kv(" gpu ", vb16(p + 6u));
    k->log(" codificadores");
    for (g = 8u; g + 2u <= t; g += 2u) log_kv(" ", vb16(p + g));
    for (g = 0; g < vb_ncon; g++)
      if (vb_con[g].id == vb16(p + 4u) && t >= 10u) vb_con[g].enc = vb16(p + 8u);
    k->log("\n");
    p += t;
  }
  log_kv("[vbios] conectores com monitor (mascara) ", vb_saidas_ligadas);
  log_kv(" pinos HPD a ", pl_r(PL_DC_GPIO_HPD_A));
  log_kv(" y ", pl_r(PL_DC_GPIO_HPD_Y));
  k->log("\n");
}

static void vb_crtcs(void) {
  uint32_t n;
  for (n = 0; n < 2u; n++) {
    uint32_t o = pl_crtc[n];
    log_kv("[vbios] crtc ", n);
    log_kv(" h_total ", pl_r(PL_CRTC_H_TOTAL + o));
    log_kv(" h_blank ", pl_r(PL_CRTC_H_BLANK + o));
    log_kv(" h_sync ", pl_r(PL_CRTC_H_SYNC_A + o));
    log_kv(" v_total ", pl_r(PL_CRTC_V_TOTAL + o));
    log_kv(" v_blank ", pl_r(PL_CRTC_V_BLANK + o));
    log_kv(" v_sync ", pl_r(PL_CRTC_V_SYNC_A + o));
    log_kv(" blank_ctl ", pl_r(PL_CRTC_BLANK_CONTROL + o));
    k->log("\n");
  }
}

/* FNV-1a da imagem, sempre: diz se o VBIOS e o mesmo da copia ja tirada.
 * Com -DAMD_VBIOS_COPIA, tambem a copia: "[vbios] OOOOO <64 bytes em hex>"
 * por linha; linhas so de FF ou so de 00 viram "[vbios] OOOOO =FF" (o
 * extrator repete o byte). A copia da RX 570 ja foi tirada (2026-11-18,
 * 59.904 bytes, fnv1a A98AC119) e enchia o SERIAL.LOG: desligada. */
static void vb_copia_log(void) {
  uint32_t tam = vb_tam < VB_DUMP_MAX ? vb_tam : VB_DUMP_MAX, o, i, h = 2166136261u;
#ifdef AMD_VBIOS_COPIA
  static const char hx[] = "0123456789ABCDEF";
  char l[16 + 128 + 2];
#endif
  for (o = 0; o < tam; o += 64u) {
#ifdef AMD_VBIOS_COPIA
    int p = 0, todos = 1;
    uint32_t d;
    for (i = 1; i < 64u; i++)
      if (vb[o + i] != vb[o]) { todos = 0; break; }
#endif
    for (i = 0; i < 64u; i++) h = (h ^ vb[o + i]) * 16777619u;
#ifdef AMD_VBIOS_COPIA
    for (i = 0; "[vbios] "[i]; i++) l[p++] = "[vbios] "[i];
    for (d = 0; d < 5u; d++) l[p++] = hx[(o >> (16u - 4u * d)) & 15u];
    l[p++] = ' ';
    if (todos && (vb[o] == 0xFFu || vb[o] == 0)) {
      l[p++] = '=';
      l[p++] = hx[vb[o] >> 4];
      l[p++] = hx[vb[o] & 15u];
    } else {
      for (i = 0; i < 64u; i++) {
        l[p++] = hx[vb[o + i] >> 4];
        l[p++] = hx[vb[o + i] & 15u];
      }
    }
    l[p++] = '\n';
    l[p] = 0;
    k->log(l);
#endif
  }
#ifdef AMD_VBIOS_COPIA
  log_kv("[vbios] fim da copia: bytes ", tam);
#else
  log_kv("[vbios] imagem: bytes ", tam);
#endif
  log_kv(" fnv1a ", h);
  k->log("\n");
}

/* Etapa M, parte 1. Uma vez por carga do DLL. */
static void polaris_saidas(void) {
  if (vb) return;
  if (vb_le() != 0) return;
  if (vb16(0) != 0xAA55u || !vb_igual(0x30u, " 761295520") || !vb_igual(vb16(0x48) + 4u, "ATOM")) {
    log_kv("[vbios] imagem sem as assinaturas do AtomBIOS (bytes 0: ", (uint32_t)vb16(0));
    k->log(")\n");
    return;
  }
  vb_objetos();
  vb_crtcs();
  vb_copia_log();
}
