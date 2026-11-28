/* i2chid.c - touchpad HID sobre I2C (ver i2chid.h).
 *
 * Caminho:
 *   1. ACPI: em cada DSDT/SSDT, depois de um PNP0C50 (EisaId ou string), o
 *      primeiro I2cSerialBus do _CRS (endereco, velocidade, controlador) e o
 *      GpioInt que vem junto (pino da interrupcao). Varias copias do mesmo
 *      touchpad em controladores diferentes (o _STA escolhe uma pela BIOS)
 *      sao todas candidatas: fica a que responder.
 *   2. Controlador: o Device do ACPI com o nome do I2cSerialBus, a base do
 *      Memory32Fixed dele, conferida pelo IC_COMP_TYPE do DesignWare.
 *   3. HID sobre I2C (especificacao da Microsoft): descritor de 30 bytes no
 *      registrador 0x20 (ou 0x01), report descriptor, SET_POWER ON, RESET.
 *   4. Relatorios: com o pino do GPIO da AMD em nivel baixo ha relatorio; le
 *      wMaxInputLength bytes (2 de tamanho, report ID, dados).
 * Sem IRQ: a RIT olha o pino a cada 1 ms (uma leitura de MMIO).
 *
 * Positivo VAIO VJFE69F11X (Ryzen 5000): ELAN 0911:5289 em 0x2C no I2CD
 * (0xFEDC5000), GPIO 9, descritor em 0x20; coleção de mouse = report ID 2. */
#include <stdint.h>
#include "i2chid.h"
#include "xhci.h"
#include "net.h"
#include "serial.h"

/* ---------- utilidades ---------- */

static uint32_t rd16(const volatile uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const volatile uint8_t* p) { return rd16(p) | (rd16(p + 2) << 16); }
static uint64_t rd64(const volatile uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static int mem_eq(const volatile uint8_t* a, const char* b, int n) {
  int i;
  for (i = 0; i < n; i++)
    if (a[i] != (uint8_t)b[i]) return 0;
  return 1;
}

static void ilog(const char* s) { serial_puts(s); }
static void ilog_hex(const char* s, uint32_t v) {
  serial_puts(s);
  serial_hex32(v);
  serial_puts("\n");
}

/* ms para esperas; sem PIT, conta voltas (como o late do xhci.c). */
static uint32_t now_ms(void) { return net_ms_fast(); }
static int late(uint32_t t0, uint32_t* spins, uint32_t ms) {
  __asm__ volatile ("pause");
  if (net_has_clock()) return (now_ms() - t0) >= ms;
  return ++(*spins) >= ms * 100000u;
}
static void isleep(uint32_t ms) {
  uint32_t t0 = now_ms(), sp = 0;
  while (!late(t0, &sp, ms)) { }
}

/* A pagina de a esta mapeada? Anda nas tabelas do kernel (PML4 em 0x1000)
 * sem mexer nelas: as tabelas ACPI ficam na RAM, que o kernel ja mapeia;
 * isto so evita um #PF se alguma ficou de fora. */
static int mapped(uint64_t a) {
  volatile uint64_t* t = (volatile uint64_t*)0x1000;
  uint64_t e;
  e = t[(a >> 39) & 511u];
  if (!(e & 1u)) return 0;
  t = (volatile uint64_t*)(uintptr_t)(e & 0x000FFFFFFFFFF000ull);
  e = t[(a >> 30) & 511u];
  if (!(e & 1u)) return 0;
  if (e & 0x80u) return 1;
  t = (volatile uint64_t*)(uintptr_t)(e & 0x000FFFFFFFFFF000ull);
  e = t[(a >> 21) & 511u];
  if (!(e & 1u)) return 0;
  if (e & 0x80u) return 1;
  t = (volatile uint64_t*)(uintptr_t)(e & 0x000FFFFFFFFFF000ull);
  return (int)(t[(a >> 12) & 511u] & 1u);
}
static int mapped_range(uint64_t a, uint64_t n) {
  uint64_t p;
  for (p = a & ~0xFFFull; p < a + n; p += 0x1000u)
    if (!mapped(p)) return 0;
  return 1;
}

/* ---------- ACPI: tabelas com AML ---------- */

#define NT 24
static const volatile uint8_t* aml_p[NT];
static uint32_t aml_n[NT];
static int ntab;

static int sum_ok(const volatile uint8_t* p, uint32_t n) {
  uint8_t s = 0;
  uint32_t i;
  for (i = 0; i < n; i++) s = (uint8_t)(s + p[i]);
  return s == 0;
}

static const volatile uint8_t* rsdp_find(void) {
  uintptr_t p;
  if (mem_eq((const volatile uint8_t*)0x5F00, "RSD PTR ", 8) && sum_ok((const volatile uint8_t*)0x5F00, 20))
    return (const volatile uint8_t*)0x5F00;
  for (p = 0xE0000; p + 20 <= 0x100000; p += 16)
    if (mem_eq((const volatile uint8_t*)p, "RSD PTR ", 8) && sum_ok((const volatile uint8_t*)p, 20))
      return (const volatile uint8_t*)p;
  return 0;
}

static void add_aml(uint64_t a) {
  const volatile uint8_t* t;
  uint32_t len;
  int i;
  if (!a || ntab >= NT || !mapped_range(a, 36)) return;
  t = (const volatile uint8_t*)(uintptr_t)a;
  len = rd32(t + 4);
  if (len <= 36 || len > 0x400000u || !mapped_range(a, len)) return;
  for (i = 0; i < ntab; i++)
    if (aml_p[i] == t + 36) return;
  aml_p[ntab] = t + 36;
  aml_n[ntab] = len - 36;
  ntab++;
}

static void tab_one(uint64_t a) {
  const volatile uint8_t* t;
  if (!a || !mapped_range(a, 36)) return;
  t = (const volatile uint8_t*)(uintptr_t)a;
  if (mem_eq(t, "SSDT", 4)) {
    add_aml(a);
  } else if (mem_eq(t, "FACP", 4)) {
    uint32_t len = rd32(t + 4);
    uint64_t d = rd32(t + 40);
    if (len >= 148 && mapped_range(a, 148) && rd64(t + 140)) d = rd64(t + 140);
    add_aml(d);
  }
}

static int tables_load(void) {
  const volatile uint8_t* r = rsdp_find();
  const volatile uint8_t* t;
  uint64_t x;
  uint32_t len, i;
  if (ntab) return ntab;
  if (!r) return 0;
  x = (r[15] >= 2) ? rd64(r + 24) : 0;
  if (x && mapped_range(x, 36)) {
    t = (const volatile uint8_t*)(uintptr_t)x;
    len = rd32(t + 4);
    if (len >= 36 && mapped_range(x, len))
      for (i = 0; i < (len - 36) / 8; i++) tab_one(rd64(t + 36 + i * 8));
  }
  if (!ntab) {
    x = rd32(r + 16);
    if (x && mapped_range(x, 36)) {
      t = (const volatile uint8_t*)(uintptr_t)x;
      len = rd32(t + 4);
      if (len >= 36 && mapped_range(x, len))
        for (i = 0; i < (len - 36) / 4; i++) tab_one(rd32(t + 36 + i * 4));
    }
  }
  return ntab;
}

/* Ultimo NameSeg de um caminho ASCII ("\\_SB.I2CD" -> "I2CD"). */
static void last_seg_str(const volatile uint8_t* s, uint32_t max, char out[4]) {
  uint32_t i, st = 0, n;
  for (i = 0; i < max && s[i]; i++)
    if (s[i] == '.' || s[i] == '\\' || s[i] == '^') st = i + 1;
  n = i - st;
  for (i = 0; i < 4; i++) out[i] = (i < n) ? (char)s[st + i] : '_';
}

/* Device (nome) com o ultimo NameSeg seg: base do Memory32Fixed do pacote e
 * se o pacote tem a string hid. 0 se nao achou. */
static uint32_t dev_mem(const char seg[4], const char* hid, int* has_hid) {
  int ti;
  *has_hid = 0;
  for (ti = 0; ti < ntab; ti++) {
    const volatile uint8_t* a = aml_p[ti];
    uint32_t n = aml_n[ti], i;
    for (i = 0; i + 8 < n; i++) {
      uint32_t pl, nb, j, k, end;
      const volatile uint8_t* nm;
      uint32_t segs = 1;
      if (a[i] != 0x5B || a[i + 1] != 0x82) continue;
      nb = a[i + 2] >> 6;
      pl = (nb == 0) ? (a[i + 2] & 0x3Fu) : (a[i + 2] & 0x0Fu);
      for (k = 0; k < nb; k++) pl |= (uint32_t)a[i + 3 + k] << (4 + 8 * k);
      end = i + 2 + pl;
      if (end > n || pl < 5) continue;
      nm = a + i + 3 + nb;
      while (*nm == '\\' || *nm == '^') nm++;
      if (*nm == 0x2E) { segs = 2; nm++; }
      else if (*nm == 0x2F) { segs = nm[1]; nm += 2; }
      if (segs == 0 || segs > 8) continue;
      nm += 4 * (segs - 1);
      if (!mem_eq(nm, seg, 4)) continue;
      {
        uint32_t base = 0;
        int hh = 0, hl = 0;
        while (hid[hl]) hl++;
        for (j = (uint32_t)(nm + 4 - a); j + 12 <= end; j++) {
          if (!base && a[j] == 0x86 && a[j + 1] == 0x09 && a[j + 2] == 0x00) base = rd32(a + j + 4);
          if (!hh && j + (uint32_t)hl <= end && mem_eq(a + j, hid, hl)) hh = 1;
        }
        if (base) {
          *has_hid = hh;
          return base;
        }
      }
    }
  }
  return 0;
}

/* ---------- candidatos ---------- */

struct cand {
  char ctl[4];       /* NameSeg do controlador I2C */
  char gctl[4];      /* NameSeg do controlador GPIO ('\0' = sem GpioInt) */
  uint32_t addr, speed;
  int pin;           /* -1 = sem GpioInt */
};
#define NC 48
static struct cand cands[NC];
static int ncand;

static void cand_add(const struct cand* c) {
  int i;
  for (i = 0; i < ncand; i++)
    if (mem_eq((const volatile uint8_t*)cands[i].ctl, c->ctl, 4) && cands[i].addr == c->addr) return;
  if (ncand < NC) cands[ncand++] = *c;
}

/* I2cSerialBus (0x8E, tipo 1) em q; 1 se valido. */
static int i2c_desc(const volatile uint8_t* q, uint32_t room, struct cand* c, uint32_t* dlen) {
  uint32_t L, tdl, s, e, k;
  if (room < 20 || q[0] != 0x8E) return 0;
  L = rd16(q + 1);
  if (L < 15 || L > 200 || L + 3 > room) return 0;
  if (q[5] != 1) return 0;                  /* I2C */
  if (rd16(q + 7) & 1u) return 0;           /* 10 bits: nao */
  tdl = rd16(q + 10);
  if (tdl < 6) return 0;
  s = 12 + tdl;
  e = 3 + L;
  if (s >= e) return 0;
  for (k = s; k < e && q[k]; k++)
    if (q[k] < 0x20 || q[k] > 0x7E) return 0;
  if (k >= e) return 0;
  c->speed = rd32(q + 12);
  c->addr = rd16(q + 16) & 0x7Fu;
  last_seg_str(q + s, e - s, c->ctl);
  *dlen = e;
  return 1;
}

/* GpioInt (0x8C, tipo 0) em g; pino e controlador. */
static int gpio_desc(const volatile uint8_t* g, uint32_t room, struct cand* c) {
  uint32_t L, po, no;
  if (room < 23 || g[0] != 0x8C) return 0;
  L = rd16(g + 1);
  if (L < 20 || L + 3 > room) return 0;
  if (g[4] != 0) return 0;                  /* interrupcao */
  po = rd16(g + 14);
  no = rd16(g + 17);
  if (po + 2 > L + 3 || no >= L + 3) return 0;
  c->pin = (int)rd16(g + po);
  last_seg_str(g + no, L + 3 - no, c->gctl);
  return 1;
}

static void find_cands(void) {
  int ti;
  ncand = 0;
  for (ti = 0; ti < ntab; ti++) {
    const volatile uint8_t* a = aml_p[ti];
    uint32_t n = aml_n[ti], i, j;
    for (i = 0; i + 8 < n; i++) {
      int hit = 0;
      if (a[i] == 0x0C && a[i + 1] == 0x41 && a[i + 2] == 0xD0 && a[i + 3] == 0x0C && a[i + 4] == 0x50)
        hit = 1;                                  /* EisaId ("PNP0C50") */
      else if (a[i] == 'P' && mem_eq(a + i, "PNP0C50", 7))
        hit = 1;
      else if (a[i] == 'A' && mem_eq(a + i, "ACPI0C50", 8))
        hit = 1;
      if (!hit) continue;
      for (j = i; j < i + 2048 && j + 20 < n; j++) {
        struct cand c;
        uint32_t dl, k;
        c.pin = -1;
        c.gctl[0] = 0;
        if (!i2c_desc(a + j, n - j, &c, &dl)) continue;
        for (k = j + dl; k < j + dl + 64 && k + 23 < n; k++)
          if (gpio_desc(a + k, n - k, &c)) break;
        if (c.pin < 0) c.gctl[0] = 0;
        cand_add(&c);
        break;
      }
      i += 7;
    }
  }
}

/* ---------- DesignWare I2C ---------- */

#define DW_CON      0x00
#define DW_TAR      0x04
#define DW_DATA     0x10
#define DW_SS_HCNT  0x14
#define DW_FS_HCNT  0x1C
#define DW_INTR_MASK 0x30
#define DW_CLR_INTR 0x40
#define DW_CLR_ABRT 0x54
#define DW_ENABLE   0x6C
#define DW_STATUS   0x70
#define DW_RXFLR    0x78
#define DW_ABRT_SRC 0x80
#define DW_EN_STAT  0x9C
#define DW_PARAM1   0xF4
#define DW_TYPE     0xFC

static volatile uint8_t* dw;     /* controlador em uso */
static int dw_depth = 8;         /* comandos de leitura pendentes no maximo */
static uint32_t dw_aborts;

static uint32_t r32(uint32_t off) { return *(volatile uint32_t*)(dw + off); }
static void w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(dw + off) = v; }

static int dw_setup(volatile uint8_t* b, uint32_t addr, uint32_t speed) {
  uint32_t t0, sp = 0, con, p1;
  dw = b;
  w32(DW_ENABLE, 0);
  t0 = now_ms();
  while (r32(DW_EN_STAT) & 1u)
    if (late(t0, &sp, 50)) return -1;
  /* Os tempos de SCL ficam os do firmware; fast mode se ele os deixou. */
  con = 0x61u;                                   /* master, restart, slave off, standard */
  if (speed >= 400000u && r32(DW_FS_HCNT)) con = 0x63u;
  else if (!r32(DW_SS_HCNT) && r32(DW_FS_HCNT)) con = 0x63u;
  w32(DW_CON, con);
  w32(DW_TAR, addr);
  w32(DW_INTR_MASK, 0);
  (void)r32(DW_CLR_INTR);
  p1 = r32(DW_PARAM1);
  {
    int rx = (int)((p1 >> 8) & 0xFFu) + 1, tx = (int)((p1 >> 16) & 0xFFu) + 1;
    dw_depth = rx < tx ? rx : tx;
    if (dw_depth > 32) dw_depth = 32;
    if (dw_depth < 2) dw_depth = 2;
  }
  w32(DW_ENABLE, 1);
  return 0;
}

/* Escreve w[0..nw) e le nr bytes (com restart entre os dois). Devolve os bytes
 * lidos, -1 se o dispositivo nao respondeu (NAK), -2 tempo esgotado. */
static int dw_xfer(const uint8_t* w, int nw, uint8_t* r, int nr, uint32_t ms) {
  int i, got = 0, sent = 0, tot = nw + nr;
  uint32_t t0 = now_ms(), sp = 0, ab;
  if (tot <= 0) return 0;
  while (r32(DW_RXFLR)) (void)r32(DW_DATA);     /* sobra de antes */
  (void)r32(DW_CLR_ABRT);
  for (i = 0; i < nw; i++) {
    uint32_t c = w[i];
    if (i == tot - 1) c |= 0x200u;               /* STOP */
    while (!(r32(DW_STATUS) & 2u))               /* TFNF */
      if (late(t0, &sp, ms)) return -2;
    w32(DW_DATA, c);
  }
  while (got < nr) {
    while (sent < nr && sent - got < dw_depth && (r32(DW_STATUS) & 2u)) {
      uint32_t c = 0x100u;                       /* READ */
      if (sent == 0 && nw) c |= 0x400u;          /* RESTART */
      if (sent == nr - 1) c |= 0x200u;           /* STOP */
      w32(DW_DATA, c);
      sent++;
    }
    while (got < nr && r32(DW_RXFLR)) r[got++] = (uint8_t)r32(DW_DATA);
    ab = r32(DW_ABRT_SRC);
    if (ab) {
      (void)r32(DW_CLR_ABRT);
      dw_aborts++;
      return -1;
    }
    if (got < nr && late(t0, &sp, ms)) return -2;
  }
  /* Fim da escrita sem leitura (ou do STOP): fila vazia e mestre parado. */
  while ((r32(DW_STATUS) & 0x24u) != 0x04u)      /* TFE e !MST_ACTIVITY */
    if (late(t0, &sp, ms)) break;
  ab = r32(DW_ABRT_SRC);
  if (ab) {
    (void)r32(DW_CLR_ABRT);
    dw_aborts++;
    return -1;
  }
  return got;
}

/* ---------- HID ---------- */

static volatile uint8_t* gpio_reg;   /* registrador do pino (GPIO da AMD), 0 = sem */
static uint32_t in_reg, max_in, cmd_reg, data_reg, rd_len, rd_reg, vid, pid;
static int ready, tried, errs;
static volatile int lk;
static uint32_t poll_t;              /* sem GPIO: ultima leitura */
static int seen;

/* Coleção de mouse: posicoes em bits depois do report ID. */
static int m_rid, m_boff, m_bn, m_xoff, m_xsz, m_yoff, m_ysz, m_woff, m_wsz;

static uint8_t rdesc[4096];
static uint8_t ib[64];

/* Mesma ideia do hid_mouse_layout do xhci.c: primeira coleção Mouse (01/02)
 * com X e Y relativos. */
static int mouse_layout(const uint8_t* p, int n) {
  static uint16_t off[256];
  uint32_t upage = 0, rsize = 0, rcount = 0, rid = 0;
  uint32_t us[16], umin = 0;
  int nus = 0, range = 0, depth = 0, mdepth = -1, i;
  int found_x = 0, found_y = 0, found_b = 0, mrid = -1;
  for (i = 0; i < 256; i++) off[i] = 0;
  m_bn = m_xsz = m_ysz = m_wsz = 0;
  i = 0;
  while (i < n) {
    uint8_t b = p[i];
    int sz = b & 3, type = (b >> 2) & 3, tag = b >> 4, j;
    uint32_t v = 0;
    if (b == 0xFE) {
      if (i + 2 >= n) break;
      i += 3 + p[i + 1];
      continue;
    }
    if (sz == 3) sz = 4;
    if (i + 1 + sz > n) break;
    for (j = 0; j < sz; j++) v |= (uint32_t)p[i + 1 + j] << (8 * j);
    i += 1 + sz;
    if (type == 1) {
      if (tag == 0) upage = v;
      else if (tag == 7) rsize = v;
      else if (tag == 8) rid = v & 0xFFu;
      else if (tag == 9) rcount = v;
    } else if (type == 2) {
      if (tag == 0 && nus < 16) us[nus++] = (sz == 4) ? v : (upage << 16) | (v & 0xFFFFu);
      else if (tag == 1) { umin = (sz == 4) ? v : (upage << 16) | v; range = 1; }
    } else if (type == 0) {
      if (tag == 10) {
        if (v == 1 && mdepth < 0 && nus > 0 && us[nus - 1] == 0x00010002u) mdepth = depth;
        depth++;
      } else if (tag == 12) {
        if (depth > 0) depth--;
        if (mdepth >= 0 && depth <= mdepth) {
          if (found_x && found_y) break;
          mdepth = -1;
        }
      } else if (tag == 8) {
        uint32_t k;
        if (mdepth >= 0 && !(v & 1) && (v & 2) && rsize && rsize <= 32) {
          for (k = 0; k < rcount && k < 64u; k++) {
            uint32_t u = nus ? us[k < (uint32_t)nus ? k : (uint32_t)nus - 1u] : range ? umin + k : 0;
            uint32_t bit = off[rid] + k * rsize;
            if (mrid >= 0 && (int)rid != mrid) break;
            if ((u >> 16) == 9 && !found_b) {
              found_b = 1; m_boff = (int)bit; m_bn = (int)(rcount > 8 ? 8 : rcount); mrid = (int)rid;
            } else if (u == 0x00010030u && !found_x && (v & 4)) {
              found_x = 1; m_xoff = (int)bit; m_xsz = (int)rsize; mrid = (int)rid;
            } else if (u == 0x00010031u && !found_y && (v & 4)) {
              found_y = 1; m_yoff = (int)bit; m_ysz = (int)rsize; mrid = (int)rid;
            } else if (u == 0x00010038u && !m_wsz) {
              m_woff = (int)bit; m_wsz = (int)rsize;
            }
          }
        }
        off[rid] = (uint16_t)(off[rid] + rsize * rcount);
      }
      nus = 0;
      range = 0;
    }
  }
  if (!found_x || !found_y) return 0;
  m_rid = mrid > 0 ? mrid : 0;
  if (!found_b) m_bn = 0;
  return 1;
}

/* Precision Touchpad (coleção 0D/05): o primeiro dedo (tip, X, Y absolutos)
 * e o botao 1, todos no mesmo report ID; e o report ID do Input Mode (0D/52,
 * feature). Serve quando o touchpad so manda o relatorio multitoque. */
static int p_rid, p_tip, p_xoff, p_xsz, p_yoff, p_ysz, p_boff = -1, im_rid;
static int ptp_layout(const uint8_t* p, int n) {
  static uint16_t off[256];
  uint32_t upage = 0, rsize = 0, rcount = 0, rid = 0;
  uint32_t us[16], umin = 0;
  int nus = 0, range = 0, depth = 0, tdepth = -1, fdepth = -1, fingers = 0, i;
  p_rid = 0; p_tip = p_xsz = p_ysz = 0; p_boff = -1; im_rid = 0;
  for (i = 0; i < 256; i++) off[i] = 0;
  i = 0;
  while (i < n) {
    uint8_t b = p[i];
    int sz = b & 3, type = (b >> 2) & 3, tag = b >> 4, j;
    uint32_t v = 0;
    if (b == 0xFE) { if (i + 2 >= n) break; i += 3 + p[i + 1]; continue; }
    if (sz == 3) sz = 4;
    if (i + 1 + sz > n) break;
    for (j = 0; j < sz; j++) v |= (uint32_t)p[i + 1 + j] << (8 * j);
    i += 1 + sz;
    if (type == 1) {
      if (tag == 0) upage = v;
      else if (tag == 7) rsize = v;
      else if (tag == 8) rid = v & 0xFFu;
      else if (tag == 9) rcount = v;
    } else if (type == 2) {
      if (tag == 0 && nus < 16) us[nus++] = (sz == 4) ? v : (upage << 16) | (v & 0xFFFFu);
      else if (tag == 1) { umin = (sz == 4) ? v : (upage << 16) | v; range = 1; }
    } else if (type == 0) {
      if (tag == 10) {
        uint32_t u = nus ? us[nus - 1] : 0;
        if (v == 1 && tdepth < 0 && u == 0x000D0005u) tdepth = depth;
        else if (tdepth >= 0 && v == 2 && u == 0x000D0022u && fdepth < 0) { fdepth = depth; fingers++; }
        depth++;
      } else if (tag == 12) {
        if (depth > 0) depth--;
        if (fdepth >= 0 && depth <= fdepth) fdepth = -1;
        if (tdepth >= 0 && depth <= tdepth) tdepth = -2;   /* so a primeira */
      } else if (tag == 8 || tag == 11) {
        uint32_t k;
        for (k = 0; k < rcount && k < 64u && rsize && rsize <= 32; k++) {
          uint32_t u = nus ? us[k < (uint32_t)nus ? k : (uint32_t)nus - 1u] : range ? umin + k : 0;
          int bit = (int)(off[rid] + k * rsize);
          if (tag == 11) { if (u == 0x000D0052u && !im_rid) im_rid = (int)rid; continue; }
          if (tdepth < 0 || (v & 1)) continue;
          if (fdepth >= 0 && fingers == 1) {
            if (u == 0x000D0042u && !p_tip) { p_tip = bit + 1; p_rid = (int)rid; }
            else if (u == 0x00010030u && !p_xsz) { p_xoff = bit; p_xsz = (int)rsize; }
            else if (u == 0x00010031u && !p_ysz) { p_yoff = bit; p_ysz = (int)rsize; }
          } else if (fdepth < 0 && u == 0x00090001u && p_boff < 0 && (int)rid == p_rid) {
            p_boff = bit;
          }
        }
        if (tag == 8) off[rid] = (uint16_t)(off[rid] + rsize * rcount);
      }
      nus = 0;
      range = 0;
    }
  }
  return p_tip && p_xsz && p_ysz && p_rid;
}

static uint32_t bits(const uint8_t* r, uint32_t nb, int bit, int size) {
  uint32_t v = 0;
  int k;
  for (k = 0; k < size && k < 32; k++) {
    uint32_t b = (uint32_t)(bit + k);
    if ((b >> 3) < nb && ((r[b >> 3] >> (b & 7)) & 1u)) v |= 1u << k;
  }
  return v;
}
static int sbits(const uint8_t* r, uint32_t nb, int bit, int size) {
  uint32_t v;
  if (size <= 0) return 0;
  v = bits(r, nb, bit, size);
  if (size < 32 && (v & (1u << (size - 1)))) v |= ~0u << size;
  return (int)v;
}

static int gpio_low(void) {
  return gpio_reg ? !((*(volatile uint32_t*)gpio_reg) & 0x10000u) : 1;
}

static int hid_cmd(uint8_t op) {
  uint8_t w[4];
  w[0] = (uint8_t)cmd_reg;
  w[1] = (uint8_t)(cmd_reg >> 8);
  w[2] = 0x00;                       /* report type/ID 0; SET_POWER: ON */
  w[3] = op;
  return dw_xfer(w, 4, 0, 0, 50);
}

/* Tenta o candidato c: 0 se o touchpad respondeu e tem coleção de mouse. */
static int try_cand(const struct cand* c) {
  static const uint16_t regs[2] = { 0x0020, 0x0001 };
  uint8_t d[30], w[2];
  uint32_t base, t0, sp = 0;
  int hh, k, n;
  base = dev_mem(c->ctl, "AMDI0010", &hh);
  ilog("[i2chid] candidato ");
  { char s[5] = { c->ctl[0], c->ctl[1], c->ctl[2], c->ctl[3], 0 }; ilog(s); }
  ilog_hex(" endereco ", c->addr);
  if (!base) { ilog("[i2chid]   controlador sem Memory32Fixed no ACPI\n"); return -1; }
  if (pci_mmio_map(base & ~0xFFFu, 0x1000) != 0) return -1;
  if (*(volatile uint32_t*)(uintptr_t)(base + DW_TYPE) != 0x44570140u) {
    ilog_hex("[i2chid]   nao e DesignWare: ", base);
    return -1;
  }
  if (dw_setup((volatile uint8_t*)(uintptr_t)base, c->addr, c->speed) != 0) {
    ilog_hex("[i2chid]   controlador nao desliga: ", base);
    return -1;
  }
  for (k = 0; k < 2; k++) {
    w[0] = (uint8_t)regs[k];
    w[1] = (uint8_t)(regs[k] >> 8);
    n = dw_xfer(w, 2, d, 30, 50);
    if (n == 30 && rd16(d) == 30 && rd16(d + 2) == 0x0100) break;
    if (n == -1) { ilog_hex("[i2chid]   sem resposta (NAK) em ", base); return -1; }
  }
  if (k == 2) { ilog("[i2chid]   descritor HID invalido\n"); return -1; }
  rd_len = rd16(d + 4);
  rd_reg = rd16(d + 6);
  in_reg = rd16(d + 8);
  max_in = rd16(d + 10);
  cmd_reg = rd16(d + 16);
  data_reg = rd16(d + 18);
  vid = rd16(d + 20);
  pid = rd16(d + 22);
  ilog_hex("[i2chid]   controlador ", base);
  ilog_hex("[i2chid]   VID:PID ", (vid << 16) | pid);
  if (rd_len == 0 || rd_len > sizeof rdesc || max_in < 3) {
    ilog_hex("[i2chid]   report descriptor grande demais: ", rd_len);
    return -1;
  }
  w[0] = (uint8_t)rd_reg;
  w[1] = (uint8_t)(rd_reg >> 8);
  n = dw_xfer(w, 2, rdesc, (int)rd_len, 200);
  if (n != (int)rd_len) { ilog_hex("[i2chid]   report descriptor nao veio: ", (uint32_t)n); return -1; }
  if (!mouse_layout(rdesc, n)) { ilog("[i2chid]   sem coleção de mouse no report descriptor\n"); return -1; }
  ilog_hex("[i2chid]   mouse: report ID ", (uint32_t)m_rid);
  if (ptp_layout(rdesc, n)) {
    ilog_hex("[i2chid]   multitoque: report ID ", (uint32_t)p_rid);
    ilog_hex("[i2chid]   dedo 1 tip/x/y bits ", ((uint32_t)(p_tip - 1) << 20) | ((uint32_t)p_xoff << 10) | (uint32_t)p_yoff);
    ilog_hex("[i2chid]   botao bit ", (uint32_t)p_boff);
  }
  if (im_rid) ilog_hex("[i2chid]   Input Mode: report ID ", (uint32_t)im_rid);
  /* GPIO da AMD: registrador do pino = base + 4 * pino, bit 16 = nivel. */
  gpio_reg = 0;
  if (c->pin >= 0 && c->gctl[0]) {
    uint32_t gb = dev_mem(c->gctl, "AMDI0030", &hh);
    if (gb && hh && pci_mmio_map(gb & ~0xFFFu, 0x2000) == 0) {
      gpio_reg = (volatile uint8_t*)(uintptr_t)(gb + 4u * (uint32_t)c->pin);
      ilog_hex("[i2chid]   GPIO do pino ", gb + 4u * (uint32_t)c->pin);
    }
  }
  if (!gpio_reg) ilog("[i2chid]   sem GPIO da AMD: le a cada 8 ms\n");
  (void)hid_cmd(0x08);               /* SET_POWER ON */
  isleep(2);
  (void)hid_cmd(0x01);               /* RESET */
  /* Resposta do reset: relatorio de tamanho 0 com a interrupcao. */
  t0 = now_ms();
  while (!gpio_low() || !gpio_reg) {
    if (late(t0, &sp, gpio_reg ? 1000 : 100)) break;
    if (!gpio_reg) continue;
  }
  ilog_hex("[i2chid]   reset: GPIO baixo depois de ms ", now_ms() - t0);
  for (k = 0; k < 4 && gpio_low(); k++) {
    n = dw_xfer(0, 0, ib, (int)(max_in > sizeof ib ? sizeof ib : max_in), 20);
    if (!gpio_reg) break;
  }
  /* Input Mode = 0 (mouse): o relatorio do report ID do mouse. A Microsoft
   * diz que e o modo depois do reset, mas confirmar nao custa. */
  if (im_rid && im_rid < 15 && data_reg) {
    uint8_t s[10];
    s[0] = (uint8_t)cmd_reg; s[1] = (uint8_t)(cmd_reg >> 8);
    s[2] = (uint8_t)(0x30u | (uint32_t)im_rid);   /* feature, report ID */
    s[3] = 0x03;                                   /* SET_REPORT */
    s[4] = (uint8_t)data_reg; s[5] = (uint8_t)(data_reg >> 8);
    s[6] = 4; s[7] = 0;                            /* tamanho: 2 + ID + 1 */
    s[8] = (uint8_t)im_rid;
    s[9] = 0;                                      /* modo mouse */
    ilog_hex("[i2chid]   SET_REPORT Input Mode=0: ", (uint32_t)dw_xfer(s, 10, 0, 0, 50));
  }
  return 0;
}

static char st[96];
static void st_set(const char* s) {
  int i;
  for (i = 0; s[i] && i < (int)sizeof st - 1; i++) st[i] = s[i];
  st[i] = 0;
}

int i2chid_init(void) {
  int i;
  if (ready) return 0;
  if (tried) return -1;
  tried = 1;
  st_set("touchpad I2C: nao achado");
  if (!tables_load()) { ilog("[i2chid] sem tabelas ACPI\n"); return -1; }
  find_cands();
  if (!ncand) { ilog("[i2chid] nenhum PNP0C50 com I2cSerialBus no ACPI\n"); return -1; }
  for (i = 0; i < ncand; i++) {
    if (try_cand(&cands[i]) == 0) {
      static const char hx[] = "0123456789ABCDEF";
      char s[] = "touchpad I2C: VVVV:PPPP em XXXX, GPIO ";
      int k;
      for (k = 0; k < 4; k++) {
        s[14 + k] = hx[(vid >> (12 - 4 * k)) & 15u];
        s[19 + k] = hx[(pid >> (12 - 4 * k)) & 15u];
        s[27 + k] = cands[i].ctl[k];
      }
      st_set(s);
      {
        int l = 0;
        while (st[l]) l++;
        if (gpio_reg && cands[i].pin >= 0 && cands[i].pin < 1000 && l + 4 < (int)sizeof st) {
          int p = cands[i].pin;
          if (p >= 100) st[l++] = (char)('0' + p / 100);
          if (p >= 10) st[l++] = (char)('0' + (p / 10) % 10);
          st[l++] = (char)('0' + p % 10);
        } else {
          const char* x = "nao (polling)";
          while (*x && l < (int)sizeof st - 1) st[l++] = *x++;
        }
        st[l] = 0;
      }
      ilog("[i2chid] ");
      ilog(st);
      ilog("\n");
      errs = 0;
      ready = 1;
      return 0;
    }
  }
  dw = 0;
  return -1;
}

int i2chid_wanted(void) { return ready; }

const char* i2chid_status(void) {
  if (!st[0]) return "touchpad I2C: nao procurado";
  return st;
}

static uint32_t st_t, st_n, c_low, c_rd, c_zero, c_mouse, c_ptp, c_other;

/* Relatorio multitoque -> deslocamento relativo do primeiro dedo. A escala
 * (2/3) deixa ~1 cm de dedo em ~150 pixels neste touchpad (2632 unidades
 * em 12 cm); o resto fica para o proximo relatorio. */
static int pt_on, pt_x, pt_y, pt_rx, pt_ry, pt_b;
static int ptp_rel(const uint8_t* r, uint32_t nb, int* dx, int* dy, int* dz, int* btn) {
  int tip = (int)bits(r, nb, p_tip - 1, 1);
  int x = (int)bits(r, nb, p_xoff, p_xsz), y = (int)bits(r, nb, p_yoff, p_ysz);
  int b = p_boff >= 0 ? (int)bits(r, nb, p_boff, 1) : 0;
  int ddx = 0, ddy = 0;
  if (tip && pt_on) {
    pt_rx += (x - pt_x) * 2;
    pt_ry += (y - pt_y) * 2;
    ddx = pt_rx / 3; pt_rx -= ddx * 3;
    ddy = pt_ry / 3; pt_ry -= ddy * 3;
  } else {
    pt_rx = pt_ry = 0;
  }
  pt_on = tip;
  pt_x = x;
  pt_y = y;
  if (!ddx && !ddy && b == pt_b) return 0;
  pt_b = b;
  *dx = ddx;
  *dy = ddy;
  *dz = 0;
  *btn = b;
  return 1;
}

int i2chid_poll(int* dx, int* dy, int* dz, int* btn) {
  int n, got = 0;
  uint32_t len, nb;
  const uint8_t* r;
  int low;
  uint32_t t;
  if (!ready) return 0;
  if (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) return 0;
  t = now_ms();
  /* Resumo no serial a cada 5 s no primeiro minuto (diagnostico). */
  if (!st_t) st_t = t;
  if (st_n < 12 && t - st_t >= 5000u) {
    st_t = t;
    st_n++;
    ilog_hex("[i2chid] estado: GPIO ", gpio_reg ? *(volatile uint32_t*)gpio_reg : 0xFFFFFFFFu);
    ilog_hex("[i2chid]   baixo/leituras ", (c_low << 16) | (c_rd & 0xFFFFu));
    ilog_hex("[i2chid]   vazios/mouse/multi/outros ", (c_zero << 24) | ((c_mouse & 0xFFu) << 16) | ((c_ptp & 0xFFu) << 8) | (c_other & 0xFFu));
  }
  /* Le com o pino baixo; e, a cada 8 ms, mesmo com ele alto (pino que nao
   * sinaliza: le e o touchpad devolve tamanho 0 quando nao tem nada). */
  low = gpio_reg ? gpio_low() : 0;
  if (low) c_low++;
  if (!low) {
    if (t - poll_t < 8u) goto out;
  }
  poll_t = t;
  c_rd++;
  n = dw_xfer(0, 0, ib, (int)(max_in > sizeof ib ? sizeof ib : max_in), 5);
  if (n < 2) {
    if (++errs <= 3 || errs % 1000 == 0) ilog_hex("[i2chid] erro na leitura: ", (uint32_t)n);
    goto out;
  }
  errs = 0;
  len = rd16(ib);
  if (len < 3 || len > (uint32_t)n) { c_zero++; goto out; }  /* 0 = reset / nada */
  if (seen < 10) {
    static const char hx[] = "0123456789ABCDEF";
    char s[4];
    uint32_t i;
    seen++;
    ilog(low ? "[i2chid] relatorio:" : "[i2chid] relatorio (pino alto):");
    for (i = 0; i < len && i < 20; i++) {
      s[0] = ' '; s[1] = hx[ib[i] >> 4]; s[2] = hx[ib[i] & 15u]; s[3] = 0;
      ilog(s);
    }
    ilog("\n");
  }
  r = ib + 2;
  nb = len - 2;
  if (p_rid && r[0] == (uint8_t)p_rid && p_rid != m_rid) {
    c_ptp++;
    got = ptp_rel(r + 1, nb - 1, dx, dy, dz, btn);
    goto out;
  }
  if (m_rid) {
    if (r[0] != (uint8_t)m_rid) { c_other++; goto out; }   /* outra coleção */
    r++;
    nb--;
  }
  c_mouse++;
  *btn = m_bn ? (int)(bits(r, nb, m_boff, m_bn > 3 ? 3 : m_bn)) : 0;
  *dx = sbits(r, nb, m_xoff, m_xsz);
  *dy = sbits(r, nb, m_yoff, m_ysz);
  *dz = m_wsz ? sbits(r, nb, m_woff, m_wsz) : 0;
  got = 1;
out:
  __atomic_store_n(&lk, 0, __ATOMIC_RELEASE);
  return got;
}
