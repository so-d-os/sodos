/*
 * virtgpu.c - VIRTGPU.DLL: driver da placa virtio-gpu (QEMU -device
 * virtio-vga, PCI 1af4:1050), so 2D. Contrato em docs/GPU.md.
 *
 * O back buffer do kernel (gfx_back, contiguo em RAM) vira a memoria do
 * recurso 2D da placa. present = TRANSFER_TO_HOST_2D + RESOURCE_FLUSH: quem
 * copia o quadro e o QEMU, nao a CPU do SODOS.
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

#define VQ_N 16u
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
#define RESP_OK_NODATA 0x1100u
#define RESP_OK_DISPLAY_INFO 0x1101u
#define FMT_B8G8R8X8 2u   /* bytes B,G,R,X: o mesmo do LFB Bochs e de gfx_back */
#define RES_ID 1u

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

/* ---------- estado ---------- */
static const struct gpu_kapi* k;
static struct gpu_mode m;
static volatile uint8_t* common;
static volatile uint8_t* notify;   /* ja somado o notify_off da fila 0 */
static uint8_t* ring;              /* desc + avail + used, uma pagina */
static struct vq_desc* desc;
static volatile uint16_t* avail;   /* flags, idx, ring[VQ_N] */
static volatile uint16_t* used;    /* flags, idx, {id u32, len u32}[VQ_N] */
static uint16_t qn, avail_idx, used_seen;
static uint8_t* buf;               /* pedidos e respostas */
static uint32_t quadros;

#define BUF_REQ0 0
#define BUF_RSP0 512
#define BUF_REQ1 1024
#define BUF_RSP1 1536
#define BUF_SIZE 2048

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
static int espera(uint16_t quantos) {
  uint32_t t0 = k->ms();
  uint16_t alvo = (uint16_t)(used_seen + quantos);
  for (;;) {
    barreira();
    if ((uint16_t)(used[1] - alvo) < 0x8000u) break;
    if (k->ms() - t0 > 1000u) {
      erro("timeout na fila");
      return -1;
    }
    __asm__ volatile ("pause");
  }
  used_seen = alvo;
  return 0;
}

/* Poe o pedido (req, rlen) + resposta na fila, a partir do descritor d. */
static void poe(uint16_t d, uint32_t req, uint32_t rlen, uint32_t rsp) {
  desc[d].addr = (uint64_t)(uintptr_t)(buf + req);
  desc[d].len = rlen;
  desc[d].flags = D_NEXT;
  desc[d].next = (uint16_t)(d + 1);
  desc[d + 1].addr = (uint64_t)(uintptr_t)(buf + rsp);
  desc[d + 1].len = 64;
  desc[d + 1].flags = D_WRITE;
  desc[d + 1].next = 0;
  zera(buf + rsp, 64);
  avail[2 + (avail_idx % qn)] = d;
  avail_idx++;
}

static void manda(void) {
  barreira();
  avail[1] = avail_idx;
  barreira();
  w16(notify, 0, 0);
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
  for (i = 0; i < len; i++) buf[BUF_REQ0 + i] = s[i];
  poe(0, BUF_REQ0, len, BUF_RSP0);
  manda();
  if (espera(1) != 0) return -1;
  return resp_ok(BUF_RSP0, esperado);
}

/* Reset + negociacao + fila 0. Reaproveita ring/buf se ja existem. */
static int liga(void) {
  uint32_t mult = 0, t0;
  uintptr_t nbase;
  uint16_t qmax, noff;
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
  if (r16(common, C_NUMQ) < 1) return -1;
  w16(common, C_QSEL, 0);
  qmax = r16(common, C_QSIZE);
  if (qmax == 0) return -1;
  qn = qmax < VQ_N ? qmax : (uint16_t)VQ_N;
  w16(common, C_QSIZE, qn);
  if (!ring) ring = (uint8_t*)k->alloc(4096, 4096);
  if (!buf) buf = (uint8_t*)k->alloc(BUF_SIZE, 64);
  if (!ring || !buf) {
    erro("sem memoria");
    return -1;
  }
  zera(ring, 4096);
  desc = (struct vq_desc*)ring;
  avail = (volatile uint16_t*)(ring + 1024);
  used = (volatile uint16_t*)(ring + 2048);
  avail_idx = used_seen = 0;
  w64(common, C_QDESC, (uint64_t)(uintptr_t)desc);
  w64(common, C_QDRIVER, (uint64_t)(uintptr_t)avail);
  w64(common, C_QDEVICE, (uint64_t)(uintptr_t)used);
  noff = r16(common, C_QNOFF);
  notify = (volatile uint8_t*)(nbase + (uintptr_t)noff * mult);
  w16(common, C_QENABLE, 1);
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

/* GET_DISPLAY_INFO prova que a fila anda; o primeiro present vem depois. */
static int v_selftest(void) {
  struct hdr h;
  zera(&h, sizeof h);
  h.type = CMD_GET_DISPLAY_INFO;
  return cmd(&h, sizeof h, RESP_OK_DISPLAY_INFO);
}

static int v_present(const struct gpu_rect* r, int n) {
  struct c_transfer* t = (struct c_transfer*)(buf + BUF_REQ0);
  struct c_flush* f = (struct c_flush*)(buf + BUF_REQ1);
  (void)r;
  (void)n;
  zera(t, sizeof *t);
  t->h.type = CMD_TRANSFER_TO_HOST_2D;
  t->r.w = m.w;
  t->r.h = m.h;
  t->id = RES_ID;
  zera(f, sizeof *f);
  f->h.type = CMD_RESOURCE_FLUSH;
  f->r.w = m.w;
  f->r.h = m.h;
  f->id = RES_ID;
  /* os dois na mesma notificacao; a fila processa em ordem */
  poe(0, BUF_REQ0, sizeof *t, BUF_RSP0);
  poe(2, BUF_REQ1, sizeof *f, BUF_RSP1);
  manda();
  if (espera(2) != 0) return -1;
  if (resp_ok(BUF_RSP0, RESP_OK_NODATA) != 0 || resp_ok(BUF_RSP1, RESP_OK_NODATA) != 0) return -1;
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
  }
  ring = buf = 0;
  common = 0;
}

static const struct gpu_drv drv = {
  GPU_ABI,
  "virtio-gpu",
  GPU_CAP_PRESENT,
  v_init,
  v_selftest,
  v_present,
  v_fini,
  v_suspend,
  v_resume,
};

const struct gpu_drv* gpu_driver(void) { return &drv; }
