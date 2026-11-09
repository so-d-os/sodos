/*
 * virtgpu.c - VIRTGPU.DLL: driver da placa virtio-gpu (QEMU -device
 * virtio-vga, PCI 1af4:1050), so 2D. Contrato em docs/GPU.md.
 *
 * O back buffer do kernel (gfx_back, contiguo em RAM) vira a memoria do
 * recurso 2D da placa. present = um TRANSFER_TO_HOST_2D por area alterada
 * (o kernel diz quais) + um RESOURCE_FLUSH: quem copia e o QEMU.
 *
 * Virtio 1.0 moderno pela PCI (capabilities 0x09), uma fila (controlq) em
 * polling, sem interrupcao. Toda espera tem timeout (k->ms).
 * suspend = reset do dispositivo (a parte VGA volta a mostrar o modo texto);
 * resume = liga de novo reaproveitando a memoria ja reservada.
 *
 * Roda no contexto do kernel: gcc -m64 -mno-sse -mno-red-zone, sem libc.
 */
#include <stdint.h>
#include "gpu.h"

/* ---------- virtio PCI ---------- */
#define VIRTIO_PCI_CAP_COMMON 1
#define VIRTIO_PCI_CAP_NOTIFY 2

#define ST_ACK 1u
#define ST_DRIVER 2u
#define ST_DRIVER_OK 4u
#define ST_FEATURES_OK 8u

/* virtio_pci_common_cfg */
#define C_DFSEL 0x00
#define C_DF 0x04
#define C_GFSEL 0x08
#define C_GF 0x0C
#define C_NUMQ 0x12
#define C_STATUS 0x14
#define C_QSEL 0x16
#define C_QSIZE 0x18
#define C_QENABLE 0x1C
#define C_QNOFF 0x1E
#define C_QDESC 0x20
#define C_QDRIVER 0x28
#define C_QDEVICE 0x30

#define VQ_N 64u
#define D_NEXT 1u
#define D_WRITE 2u

struct vq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

/* ---------- virtio-gpu ---------- */
#define CMD_GET_DISPLAY_INFO 0x0100u
#define CMD_RESOURCE_CREATE_2D 0x0101u
#define CMD_SET_SCANOUT 0x0103u
#define CMD_RESOURCE_FLUSH 0x0104u
#define CMD_TRANSFER_TO_HOST_2D 0x0105u
#define CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define CMD_UPDATE_CURSOR 0x0300u
#define CMD_MOVE_CURSOR 0x0301u
#define RESP_OK_NODATA 0x1100u
#define RESP_OK_DISPLAY_INFO 0x1101u
#define FMT_B8G8R8A8 1u   /* cursor: palavra 0xAARRGGBB */
#define FMT_B8G8R8X8 2u   /* bytes B,G,R,X: o mesmo do LFB Bochs e de gfx_back */
#define RES_ID 1u
#define RES_CURSOR 2u
#define CUR_LADO 64u      /* a placa usa imagem de cursor 64x64 */

struct hdr {
  uint32_t type, flags;
  uint64_t fence;
  uint32_t ctx;
  uint32_t pad;
};

struct rect {
  uint32_t x, y, w, h;
};

struct c_create {
  struct hdr h;
  uint32_t id, format, width, height;
};

struct c_attach {
  struct hdr h;
  uint32_t id, n;
  uint64_t addr;
  uint32_t len, pad;
};

struct c_scanout {
  struct hdr h;
  struct rect r;
  uint32_t scanout, id;
};

struct c_transfer {
  struct hdr h;
  struct rect r;
  uint64_t offset;
  uint32_t id, pad;
};

struct c_flush {
  struct hdr h;
  struct rect r;
  uint32_t id, pad;
};

struct c_cursor {
  struct hdr h;
  uint32_t scanout, x, y, pad;
  uint32_t id, hot_x, hot_y, pad2;
};

/* ---------- estado ---------- */
static const struct gpu_kapi* k;
static struct gpu_mode m;
static volatile uint8_t* common;

/* Uma fila virtio em uma pagina: desc (0), avail (1024), used (2048). */
struct fila {
  struct vq_desc* desc;
  volatile uint16_t* avail;   /* flags, idx, ring[VQ_N] */
  volatile uint16_t* used;    /* flags, idx, {id u32, len u32}[VQ_N] */
  volatile uint8_t* notify;   /* ja somado o notify_off da fila */
  uint16_t index, qn, avail_idx, used_seen;
};
static struct fila ctl;            /* controlq (0) */
static struct fila cur;            /* cursorq (1) */
static uint8_t* ring;              /* duas paginas: ctl e cur */
static uint8_t* buf;               /* pedidos e respostas */
static uint32_t* cur_img;          /* imagem do cursor, CUR_LADO x CUR_LADO */
static int cur_res_ok;             /* recurso do cursor criado desde o ultimo reset */
static int cur_x, cur_y, cur_hx, cur_hy;
static uint32_t quadros;

/* buf: N_SLOT pedidos de 64 bytes, cada um com a resposta de 64 logo depois;
 * no fim, uma resposta grande para GET_DISPLAY_INFO (408 bytes). */
#define N_SLOT 32u
#define SLOT_REQ(i) ((i) * 128u)
#define SLOT_RSP(i) ((i) * 128u + 64u)
#define BUF_BIG (N_SLOT * 128u)
#define BUF_CUR (BUF_BIG + 512u)   /* pedido da fila do cursor */
#define BUF_SIZE (BUF_CUR + 64u)

/* Areas alteradas (GPU_CAP_DAMAGE) vem do kernel; mais que MAX_RECTS vira
 * um retangulo so (a uniao). */
#define MAX_RECTS 12u

static void barreira(void) { __asm__ volatile ("lock; addl $0,(%%rsp)" : : : "memory"); }

static uint8_t r8(volatile uint8_t* b, uint32_t o) { return *(volatile uint8_t*)(b + o); }
static uint16_t r16(volatile uint8_t* b, uint32_t o) { return *(volatile uint16_t*)(b + o); }
static uint32_t r32(volatile uint8_t* b, uint32_t o) { return *(volatile uint32_t*)(b + o); }
static void w8(volatile uint8_t* b, uint32_t o, uint8_t v) { *(volatile uint8_t*)(b + o) = v; }
static void w16(volatile uint8_t* b, uint32_t o, uint16_t v) { *(volatile uint16_t*)(b + o) = v; }
static void w32(volatile uint8_t* b, uint32_t o, uint32_t v) { *(volatile uint32_t*)(b + o) = v; }
static void w64(volatile uint8_t* b, uint32_t o, uint64_t v) {
  w32(b, o, (uint32_t)v);
  w32(b, o + 4, (uint32_t)(v >> 32));
}

static void zera(void* p, uint32_t n) {
  uint8_t* d = (uint8_t*)p;
  while (n--) *d++ = 0;
}

static void erro(const char* s) {
  k->log("[virtgpu] ");
  k->log(s);
  k->log("\n");
}

static uint8_t cfg8(uint32_t off) {
  return (uint8_t)(k->pci_read32(m.pci_bus, m.pci_dev, m.pci_fun, off & ~3u) >> ((off & 3u) * 8u));
}

static uint32_t cfg32(uint32_t off) { return k->pci_read32(m.pci_bus, m.pci_dev, m.pci_fun, off); }

/* Endereco de um BAR de memoria; 0 se for de I/O ou acima de 4 GB. */
static uintptr_t bar_addr(uint32_t bar) {
  uint32_t lo = cfg32(0x10 + bar * 4u);
  uint32_t hi = 0;
  if (lo & 1u) return 0;
  if (((lo >> 1) & 3u) == 2u) hi = cfg32(0x14 + bar * 4u);
  if (hi) return 0;   /* o kernel so mapeia ate 4 GB */
  return lo & ~0xFu;
}

/* Acha as capabilities virtio (comum e notificacao). */
static int acha_caps(uint32_t* notify_mult, uintptr_t* notify_base) {
  uint32_t p;
  int n = 0;
  common = 0;
  *notify_base = 0;
  if (!(cfg32(0x04) >> 16 & 0x10u)) return -1;   /* sem lista de capabilities */
  for (p = cfg8(0x34) & 0xFCu; p && n < 48; p = cfg8(p + 1) & 0xFCu, n++) {
    uint8_t type;
    uintptr_t base;
    if (cfg8(p) != 0x09) continue;
    type = cfg8(p + 3);
    base = bar_addr(cfg8(p + 4));
    if (!base) continue;
    base += cfg32(p + 8);
    if (type == VIRTIO_PCI_CAP_COMMON && !common) common = (volatile uint8_t*)base;
    if (type == VIRTIO_PCI_CAP_NOTIFY && !*notify_base) {
      *notify_base = base;
      *notify_mult = cfg32(p + 16);
    }
  }
  return common && *notify_base ? 0 : -1;
}

/* Espera o dispositivo devolver 'quantos' pedidos. */
static int espera_em(struct fila* q, uint16_t quantos) {
  uint32_t t0 = k->ms();
  uint16_t alvo = (uint16_t)(q->used_seen + quantos);
  for (;;) {
    barreira();
    if ((uint16_t)(q->used[1] - alvo) < 0x8000u) break;
    if (k->ms() - t0 > 1000u) {
      erro("timeout na fila");
      return -1;
    }
    __asm__ volatile ("pause");
  }
  q->used_seen = alvo;
  return 0;
}

static int espera(uint16_t quantos) { return espera_em(&ctl, quantos); }

/* Poe o pedido (req, rlen) + resposta na fila, a partir do descritor d. */
static void poe_r(uint16_t d, uint32_t req, uint32_t rlen, uint32_t rsp, uint32_t rsplen) {
  struct vq_desc* desc = ctl.desc;
  desc[d].addr = (uint64_t)(uintptr_t)(buf + req);
  desc[d].len = rlen;
  desc[d].flags = D_NEXT;
  desc[d].next = (uint16_t)(d + 1);
  desc[d + 1].addr = (uint64_t)(uintptr_t)(buf + rsp);
  desc[d + 1].len = rsplen;
  desc[d + 1].flags = D_WRITE;
  desc[d + 1].next = 0;
  zera(buf + rsp, rsplen);
  ctl.avail[2 + (ctl.avail_idx % ctl.qn)] = d;
  ctl.avail_idx++;
}

static void poe(uint16_t d, uint32_t req, uint32_t rlen, uint32_t rsp) {
  poe_r(d, req, rlen, rsp, 64);
}

static void manda_em(struct fila* q) {
  barreira();
  q->avail[1] = q->avail_idx;
  barreira();
  w16(q->notify, 0, q->index);
}

static void manda(void) { manda_em(&ctl); }

/* Fila do cursor: um descritor so, sem resposta (o dispositivo so devolve). */
static int cursor_cmd(const struct c_cursor* c) {
  const uint8_t* s = (const uint8_t*)c;
  uint32_t i;
  for (i = 0; i < sizeof *c; i++) buf[BUF_CUR + i] = s[i];
  cur.desc[0].addr = (uint64_t)(uintptr_t)(buf + BUF_CUR);
  cur.desc[0].len = sizeof *c;
  cur.desc[0].flags = 0;
  cur.desc[0].next = 0;
  cur.avail[2 + (cur.avail_idx % cur.qn)] = 0;
  cur.avail_idx++;
  manda_em(&cur);
  return espera_em(&cur, 1);
}

static int resp_ok(uint32_t rsp, uint32_t esperado) {
  uint32_t t = ((struct hdr*)(buf + rsp))->type;
  if (t == esperado) return 0;
  k->log("[virtgpu] resposta ");
  k->log_hex(t);
  k->log("\n");
  return -1;
}

/* Um comando sincrono com resposta OK_NODATA (ou 'esperado'). */
static int cmd(const void* req, uint32_t len, uint32_t esperado) {
  const uint8_t* s = (const uint8_t*)req;
  uint32_t i;
  for (i = 0; i < len; i++) buf[SLOT_REQ(0) + i] = s[i];
  poe_r(0, SLOT_REQ(0), len, BUF_BIG, 512);
  manda();
  if (espera(1) != 0) return -1;
  return resp_ok(BUF_BIG, esperado);
}

static int liga_fila(struct fila* q, uint16_t idx, uint8_t* mem, uintptr_t nbase, uint32_t mult) {
  uint16_t qmax;
  w16(common, C_QSEL, idx);
  qmax = r16(common, C_QSIZE);
  if (qmax == 0) return -1;
  q->index = idx;
  q->qn = qmax < VQ_N ? qmax : (uint16_t)VQ_N;
  w16(common, C_QSIZE, q->qn);
  zera(mem, 4096);
  q->desc = (struct vq_desc*)mem;
  q->avail = (volatile uint16_t*)(mem + 1024);
  q->used = (volatile uint16_t*)(mem + 2048);
  q->avail_idx = q->used_seen = 0;
  w64(common, C_QDESC, (uint64_t)(uintptr_t)q->desc);
  w64(common, C_QDRIVER, (uint64_t)(uintptr_t)q->avail);
  w64(common, C_QDEVICE, (uint64_t)(uintptr_t)q->used);
  q->notify = (volatile uint8_t*)(nbase + (uintptr_t)r16(common, C_QNOFF) * mult);
  w16(common, C_QENABLE, 1);
  return 0;
}

/* Reset + negociacao + filas 0 e 1. Reaproveita ring/buf se ja existem. */
static int liga(void) {
  uint32_t mult = 0, t0;
  uintptr_t nbase;
  if (acha_caps(&mult, &nbase) != 0) {
    erro("capabilities virtio nao achadas");
    return -1;
  }
  /* memoria e bus master ligados */
  k->pci_write32(m.pci_bus, m.pci_dev, m.pci_fun, 0x04, (cfg32(0x04) & 0xFFFFu) | 0x6u);
  w8(common, C_STATUS, 0);
  t0 = k->ms();
  while (r8(common, C_STATUS) != 0)
    if (k->ms() - t0 > 500u) {
      erro("reset nao terminou");
      return -1;
    }
  w8(common, C_STATUS, ST_ACK);
  w8(common, C_STATUS, ST_ACK | ST_DRIVER);
  w32(common, C_DFSEL, 1);
  if (!(r32(common, C_DF) & 1u)) {
    erro("sem VIRTIO_F_VERSION_1");
    return -1;
  }
  w32(common, C_GFSEL, 0);
  w32(common, C_GF, 0);            /* sem virgl, sem EDID */
  w32(common, C_GFSEL, 1);
  w32(common, C_GF, 1);            /* VERSION_1 */
  w8(common, C_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
  if (!(r8(common, C_STATUS) & ST_FEATURES_OK)) {
    erro("features recusadas");
    return -1;
  }
  if (!ring) ring = (uint8_t*)k->alloc(8192, 4096);
  if (!buf) buf = (uint8_t*)k->alloc(BUF_SIZE, 64);
  if (!ring || !buf) {
    erro("sem memoria");
    return -1;
  }
  if (liga_fila(&ctl, 0, ring, nbase, mult) != 0) return -1;
  /* sem fila de cursor o driver ainda serve; so o cursor fica com a CPU */
  cur.qn = 0;
  if (r16(common, C_NUMQ) >= 2) liga_fila(&cur, 1, ring + 4096, nbase, mult);
  cur_res_ok = 0;
  w8(common, C_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
  return 0;
}

/* Recurso 2D com a memoria do back buffer, na tela 0. */
static int monta_tela(void) {
  struct c_create cc;
  struct c_attach ca;
  struct c_scanout cs;
  zera(&cc, sizeof cc);
  cc.h.type = CMD_RESOURCE_CREATE_2D;
  cc.id = RES_ID;
  cc.format = FMT_B8G8R8X8;
  cc.width = m.w;
  cc.height = m.h;
  if (cmd(&cc, sizeof cc, RESP_OK_NODATA) != 0) return -1;
  zera(&ca, sizeof ca);
  ca.h.type = CMD_RESOURCE_ATTACH_BACKING;
  ca.id = RES_ID;
  ca.n = 1;
  ca.addr = (uint64_t)(uintptr_t)m.back;
  ca.len = m.pitch * m.h;
  if (cmd(&ca, sizeof ca, RESP_OK_NODATA) != 0) return -1;
  zera(&cs, sizeof cs);
  cs.h.type = CMD_SET_SCANOUT;
  cs.r.w = m.w;
  cs.r.h = m.h;
  cs.scanout = 0;
  cs.id = RES_ID;
  return cmd(&cs, sizeof cs, RESP_OK_NODATA);
}

static int v_init(const struct gpu_kapi* kapi, const struct gpu_mode* mode) {
  k = kapi;
  if (!k || k->abi != GPU_ABI || !mode || !mode->back) return -1;
  m = *mode;
  if (m.pitch != m.w * 4u) {
    erro("back buffer com pitch diferente da largura");
    return -1;
  }
  if (liga() != 0 || monta_tela() != 0) {
    if (common) w8(common, C_STATUS, 0);
    return -1;
  }
  quadros = 0;
  k->log("[virtgpu] tela ");
  k->log_hex(m.w);
  k->log("x");
  k->log_hex(m.h);
  k->log(" em recurso 2D\n");
  return 0;
}

/* Imagem do cursor num recurso 64x64 e UPDATE_CURSOR na fila do cursor. */
static int v_cursor_set(const uint32_t* argb, int w, int h, int hot_x, int hot_y) {
  struct c_transfer t;
  struct c_cursor c;
  int x, y;
  if (!cur.qn || !argb || w < 1 || h < 1 || w > (int)CUR_LADO || h > (int)CUR_LADO) return -1;
  if (!cur_img) cur_img = (uint32_t*)k->alloc(CUR_LADO * CUR_LADO * 4u, 64);
  if (!cur_img) return -1;
  for (y = 0; y < (int)CUR_LADO; y++)
    for (x = 0; x < (int)CUR_LADO; x++)
      cur_img[y * CUR_LADO + x] = x < w && y < h ? argb[y * w + x] : 0u;
  if (!cur_res_ok) {
    struct c_create cc;
    struct c_attach ca;
    zera(&cc, sizeof cc);
    cc.h.type = CMD_RESOURCE_CREATE_2D;
    cc.id = RES_CURSOR;
    cc.format = FMT_B8G8R8A8;
    cc.width = cc.height = CUR_LADO;
    if (cmd(&cc, sizeof cc, RESP_OK_NODATA) != 0) return -1;
    zera(&ca, sizeof ca);
    ca.h.type = CMD_RESOURCE_ATTACH_BACKING;
    ca.id = RES_CURSOR;
    ca.n = 1;
    ca.addr = (uint64_t)(uintptr_t)cur_img;
    ca.len = CUR_LADO * CUR_LADO * 4u;
    if (cmd(&ca, sizeof ca, RESP_OK_NODATA) != 0) return -1;
    cur_res_ok = 1;
  }
  zera(&t, sizeof t);
  t.h.type = CMD_TRANSFER_TO_HOST_2D;
  t.r.w = t.r.h = CUR_LADO;
  t.id = RES_CURSOR;
  if (cmd(&t, sizeof t, RESP_OK_NODATA) != 0) return -1;
  cur_hx = hot_x;
  cur_hy = hot_y;
  zera(&c, sizeof c);
  c.h.type = CMD_UPDATE_CURSOR;
  c.x = (uint32_t)cur_x;
  c.y = (uint32_t)cur_y;
  c.id = RES_CURSOR;
  c.hot_x = (uint32_t)hot_x;
  c.hot_y = (uint32_t)hot_y;
  return cursor_cmd(&c);
}

static int v_cursor_move(int x, int y) {
  struct c_cursor c;
  if (!cur.qn || !cur_res_ok) return -1;
  cur_x = x < 0 ? 0 : x;
  cur_y = y < 0 ? 0 : y;
  zera(&c, sizeof c);
  c.h.type = CMD_MOVE_CURSOR;
  c.x = (uint32_t)cur_x;
  c.y = (uint32_t)cur_y;
  c.id = RES_CURSOR;
  c.hot_x = (uint32_t)cur_hx;
  c.hot_y = (uint32_t)cur_hy;
  return cursor_cmd(&c);
}

/* GET_DISPLAY_INFO prova que a fila anda; o primeiro present vem depois. */
static int v_selftest(void) {
  struct hdr h;
  zera(&h, sizeof h);
  h.type = CMD_GET_DISPLAY_INFO;
  return cmd(&h, sizeof h, RESP_OK_DISPLAY_INFO);
}

/* Um TRANSFER por retangulo e um FLUSH com a uniao, numa notificacao so. */
static int manda_rects(const struct rect* r, int n) {
  struct c_flush* f;
  uint32_t x0 = m.w, y0 = m.h, x1 = 0, y1 = 0;
  int i;
  for (i = 0; i < n; i++) {
    struct c_transfer* t = (struct c_transfer*)(buf + SLOT_REQ((uint32_t)i));
    zera(t, sizeof *t);
    t->h.type = CMD_TRANSFER_TO_HOST_2D;
    t->r = r[i];
    t->offset = (uint64_t)r[i].y * m.pitch + (uint64_t)r[i].x * 4u;
    t->id = RES_ID;
    poe((uint16_t)(i * 2), SLOT_REQ((uint32_t)i), sizeof *t, SLOT_RSP((uint32_t)i));
    if (r[i].x < x0) x0 = r[i].x;
    if (r[i].y < y0) y0 = r[i].y;
    if (r[i].x + r[i].w > x1) x1 = r[i].x + r[i].w;
    if (r[i].y + r[i].h > y1) y1 = r[i].y + r[i].h;
  }
  f = (struct c_flush*)(buf + SLOT_REQ((uint32_t)n));
  zera(f, sizeof *f);
  f->h.type = CMD_RESOURCE_FLUSH;
  f->r.x = x0;
  f->r.y = y0;
  f->r.w = x1 - x0;
  f->r.h = y1 - y0;
  f->id = RES_ID;
  poe((uint16_t)(n * 2), SLOT_REQ((uint32_t)n), sizeof *f, SLOT_RSP((uint32_t)n));
  manda();
  if (espera((uint16_t)(n + 1)) != 0) return -1;
  for (i = 0; i <= n; i++)
    if (resp_ok(SLOT_RSP((uint32_t)i), RESP_OK_NODATA) != 0) return -1;
  return 0;
}

#ifdef VG_MEDIR
/* -DVG_MEDIR: ciclos medios do present a cada 64 quadros, no log serial. */
static uint64_t med_soma;
static uint32_t med_n, med_px;
static uint64_t tsc(void) {
  uint32_t lo, hi;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return (uint64_t)hi << 32 | lo;
}
#endif

static int present(const struct gpu_rect* rr, int nn);

static int v_present(const struct gpu_rect* rr, int nn) {
#ifdef VG_MEDIR
  uint64_t t0 = tsc();
  int rc = present(rr, nn);
  med_soma += tsc() - t0;
  if (++med_n == 64) {
    k->log("[virtgpu] ciclos/quadro ");
    k->log_hex((uint32_t)(med_soma / 64u));
    k->log(" pixels/quadro ");
    k->log_hex(med_px / 64u);
    k->log("\n");
    med_soma = 0;
    med_n = med_px = 0;
  }
  return rc;
#else
  return present(rr, nn);
#endif
}

static int present(const struct gpu_rect* rr, int nn) {
  struct rect r[MAX_RECTS];
  int n = 0, i;
  /* recorta na tela; o kernel ja recorta, mas o driver nao confia */
  for (i = 0; rr && i < nn; i++) {
    int32_t x = rr[i].x, y = rr[i].y, w = rr[i].w, h = rr[i].h;
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
    if (n == (int)MAX_RECTS || 2 * (n + 2) > (int)ctl.qn) {
      /* demais: a uniao de tudo num retangulo so */
      uint32_t x1 = (uint32_t)(x + w), y1 = (uint32_t)(y + h);
      int j;
      for (j = 0; j < n; j++) {
        if (r[j].x < (uint32_t)x) x = (int32_t)r[j].x;
        if (r[j].y < (uint32_t)y) y = (int32_t)r[j].y;
        if (r[j].x + r[j].w > x1) x1 = r[j].x + r[j].w;
        if (r[j].y + r[j].h > y1) y1 = r[j].y + r[j].h;
      }
      r[0].x = (uint32_t)x;
      r[0].y = (uint32_t)y;
      r[0].w = x1 - (uint32_t)x;
      r[0].h = y1 - (uint32_t)y;
      n = 1;
      continue;
    }
    r[n].x = (uint32_t)x;
    r[n].y = (uint32_t)y;
    r[n].w = (uint32_t)w;
    r[n].h = (uint32_t)h;
    n++;
  }
  if (n == 0) {
    /* sem areas (ou todas fora da tela): a tela inteira */
    r[0].x = r[0].y = 0;
    r[0].w = m.w;
    r[0].h = m.h;
    n = 1;
  }
  if (manda_rects(r, n) != 0) return -1;
#ifdef VG_MEDIR
  for (i = 0; i < n; i++) med_px += r[i].w * r[i].h;
#endif
  quadros++;
  return 0;
}

/* Reset: a parte VGA do virtio-vga volta a mostrar a tela de texto. */
static int v_suspend(void) {
  w8(common, C_STATUS, 0);
  return 0;
}

static int v_resume(const struct gpu_mode* mode) {
  if (!mode || !mode->back || mode->pitch != mode->w * 4u) return -1;
  m = *mode;
  if (liga() != 0 || monta_tela() != 0) {
    w8(common, C_STATUS, 0);
    return -1;
  }
  return v_selftest();
}

static void v_fini(void) {
  if (common) w8(common, C_STATUS, 0);
  if (k) {
    k->log("[virtgpu] fim, quadros ");
    k->log_hex(quadros);
    k->log("\n");
    k->free(ring);
    k->free(buf);
    k->free(cur_img);
  }
  ring = buf = 0;
  cur_img = 0;
  common = 0;
}

static const struct gpu_drv drv = {
  GPU_ABI,
  "virtio-gpu",
  GPU_CAP_PRESENT | GPU_CAP_DAMAGE | GPU_CAP_CURSOR,
  v_init,
  v_selftest,
  v_present,
  v_fini,
  v_suspend,
  v_resume,
  v_cursor_set,
  v_cursor_move,
};

const struct gpu_drv* gpu_driver(void) { return &drv; }
