/* xhci.c - Controlador USB 3 (xHCI): hubs USB 2, mouse USB (protocolo boot)
 * e pendrive (mass storage bulk-only, comandos SCSI).
 *
 * Tudo por polling, como o resto do kernel (sem IRQ):
 *   1. acha os xHCI no PCI (classe 0C0330, qualquer barramento);
 *   2. mapeia o BAR sem cache e toma o controlador do firmware (BIOS handoff);
 *   3. reinicia, monta DCBAA, anel de comandos e anel de eventos;
 *   4. em cada porta raiz conectada: reset, Enable Slot, Address Device,
 *      descritores e, conforme a classe da interface, hub, mouse ou disco;
 *   5. hub: liga as portas dele e enumera cada uma (route string e, para
 *      LS/FS atras de hub HS, o TT do hub);
 *   6. mouse: deixa um Normal TRB no interrupt IN e le cada relatorio;
 *   7. disco: CBW, dados e CSW nos dois endpoints bulk (READ(10), WRITE(10)).
 * Um mouse e um disco por vez. Hub USB 3 (SuperSpeed) nao.
 * A memoria e toda de identidade (fisico = virtual) abaixo de 256 MB. */
#include <stdint.h>
#include "xhci.h"
#include "bootinfo.h"
#include "mm.h"
#include "net.h"
#include "serial.h"

#define XC_MAX   4
#define UDEV_MAX 8
#define RING_N   32
#define EVT_N    64

#define TRB_NORMAL   1
#define TRB_SETUP    2
#define TRB_DATA     3
#define TRB_STATUS   4
#define TRB_LINK     6
#define TRB_EN_SLOT  9
#define TRB_DIS_SLOT 10
#define TRB_ADDR     11
#define TRB_CONFIG   12
#define TRB_EVAL     13
#define TRB_RESET_EP 14
#define TRB_SET_DEQ  16
#define EV_XFER      32
#define EV_CMD       33
#define EV_PORT      34

#define CC_OK    1
#define CC_STALL 6
#define CC_SHORT 13

/* PORTSC: bits que se escrevem de volta sem efeito colateral (RO e RWS).
 * PED (bit 1) e os bits de mudanca (17..23) sao RW1C e ficam fora. */
#define PORT_KEEP 0x4E00FFE9u
#define PORT_CCS  (1u << 0)
#define PORT_PED  (1u << 1)
#define PORT_PR   (1u << 4)
#define PORT_PP   (1u << 9)
#define PORT_CSC  (1u << 17)
#define PORT_PRC  (1u << 21)
#define PORT_CHG  (0x7Fu << 17)

/* Tipos em devs[]. UD_OTHER fica enderecado sem driver, so para a porta
 * nao ser enumerada de novo a cada varredura. */
#define UD_FREE  0
#define UD_NEW   1
#define UD_OTHER 2
#define UD_HUB   3
#define UD_MOUSE 4
#define UD_DISK  5

#define DK_RA 32u   /* setores lidos de uma vez (16 KB) */
#define MSQ   8     /* relatorios do mouse guardados entre dois polls */

struct trb {
  volatile uint32_t d[4];
};

struct ring {
  struct trb* t;
  uint32_t idx;
  uint32_t pcs;
};

struct xc {
  int up;
  int bus, dev, fun;
  volatile uint8_t* cap;
  volatile uint8_t* op;
  volatile uint8_t* rt;
  volatile uint32_t* db;
  uint32_t ctx;            /* 32 ou 64 bytes por contexto (HCCPARAMS1.CSZ) */
  uint32_t ports;
  uint32_t pchg;           /* portas raiz (< 32) com evento ainda nao tratado */
  uint8_t* mem;            /* 1 pagina: DCBAA, ERST, anel de comandos e de eventos */
  volatile uint64_t* dcbaa;
  volatile uint32_t* erst;
  struct ring cmd;
  struct trb* evt;
  uint32_t evt_idx;
  uint32_t evt_ccs;
  uint64_t* sp_arr;
  uint8_t* sp_pages;
  uint32_t sp_n;
};

struct udev {
  int kind;
  struct xc* x;
  int slot;
  int port;                /* porta raiz (0-based) */
  uint32_t route;          /* porta em cada hub do caminho, 4 bits por nivel */
  int speed;               /* 1 FS, 2 LS, 3 HS, 4+ SS */
  int parent;              /* hub em devs[] ou -1 */
  int hport;               /* porta no hub (1-based) */
  int tt_slot;             /* hub HS que traduz para este LS/FS (0 = nenhum) */
  int tt_port;
  int nports;              /* hub */
  int iface;
  int dci_in, dci_out;     /* mouse: so dci_in */
  int ep_in, ep_out;       /* enderecos USB dos endpoints bulk (CLEAR_FEATURE) */
  int len;                 /* mouse: bytes do relatorio */
  uint8_t* mem;            /* 3 paginas, nunca liberadas */
  struct ring ep0, rin, rout;
  volatile uint8_t* buf;   /* 256 bytes do EP0 */
  volatile uint8_t* rep;   /* 64 bytes: relatorio do mouse ou CSW */
  volatile uint8_t* cbw;   /* 32 bytes */
  volatile uint8_t* ictx;
  volatile uint8_t* octx;
};

static struct xc xcs[XC_MAX];
static int xc_n;
static struct udev devs[UDEV_MAX];
static int busy;
static int ps2_hint = -1;  /* ultimo ps2_ok de xhci_mouse_init; -1 = nao sabido */

static int ms = -1;        /* devs[] do mouse */
static int ms_errs;
static int ms_fix;         /* erro no interrupt IN: reset no proximo poll */
static int ms_q[MSQ][4];   /* dx, dy, dz, botoes */
static int ms_qh, ms_qn;

static int dk = -1;        /* devs[] do pendrive */
static int dk_fails;
static uint32_t dk_blocks;
static uint32_t dk_gen;
static uint32_t dk_tag;
static volatile uint8_t* dk_data;   /* DK_RA setores: a leitura adiantada */
static volatile uint8_t* dk_wbuf;   /* 1 setor a gravar */
static uint32_t dk_clba, dk_cn;     /* setores validos em dk_data */
static char dk_name[28];

/* ---------- baixo nivel ---------- */

static inline void outl(uint16_t port, uint32_t v) {
  __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
  uint32_t v;
  __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

static uint32_t pci_rd(int bus, int dev, int fun, int off) {
  outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
              ((uint32_t)fun << 8) | ((uint32_t)off & 0xFCu));
  return inl(0xCFC);
}

static void pci_wr(int bus, int dev, int fun, int off, uint32_t v) {
  outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
              ((uint32_t)fun << 8) | ((uint32_t)off & 0xFCu));
  outl(0xCFC, v);
}

static inline uint32_t r32(volatile uint8_t* b, uint32_t off) {
  return *(volatile uint32_t*)(b + off);
}

static inline void w32(volatile uint8_t* b, uint32_t off, uint32_t v) {
  *(volatile uint32_t*)(b + off) = v;
}

static void w64(volatile uint8_t* b, uint32_t off, uint64_t v) {
  w32(b, off, (uint32_t)v);
  w32(b, off + 4, (uint32_t)(v >> 32));
}

static uint64_t pa(const volatile void* p) {
  return (uint64_t)(uintptr_t)p;
}

static void zero(volatile void* p, uint32_t n) {
  volatile uint8_t* b = (volatile uint8_t*)p;
  while (n--) *b++ = 0;
}

static uint32_t be32(const volatile uint8_t* b) {
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static uint32_t le32(const volatile uint8_t* b) {
  return b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void put_le32(volatile uint8_t* b, uint32_t v) {
  b[0] = (uint8_t)v;
  b[1] = (uint8_t)(v >> 8);
  b[2] = (uint8_t)(v >> 16);
  b[3] = (uint8_t)(v >> 24);
}

/* Bloco zerado e alinhado. Nunca e liberado (fica para a proxima tentativa). */
static void* xmem(uint32_t n, uint32_t align) {
  uint8_t* raw;
  uint64_t a;
  if (!mm_ready()) return 0;
  raw = (uint8_t*)kmalloc(n + align);
  if (!raw) return 0;
  a = (pa(raw) + align - 1u) & ~(uint64_t)(align - 1u);
  zero((void*)(uintptr_t)a, n);
  return (void*)(uintptr_t)a;
}

/* 1 quando passou 'ms' desde t0. Sem PIT, conta voltas. */
static int late(uint32_t t0, uint32_t* spins, uint32_t ms) {
  __asm__ volatile ("pause");
  if (net_has_clock()) return (net_ms() - t0) >= ms;
  return ++(*spins) >= ms * 100000u;
}

static void xsleep(uint32_t ms) {
  uint32_t t0 = net_ms(), spins = 0;
  while (!late(t0, &spins, ms)) { }
}

static int xwait(volatile uint8_t* b, uint32_t off, uint32_t mask, uint32_t want, uint32_t ms) {
  uint32_t t0 = net_ms(), spins = 0;
  while ((r32(b, off) & mask) != want)
    if (late(t0, &spins, ms)) return -1;
  return 0;
}

/* Proxima tabela de paginas a partir da entrada e (cria uma vazia se faltar). */
static volatile uint64_t* pt_next(volatile uint64_t* e) {
  if (!(*e & 1u)) {
    void* t = xmem(4096u, 4096u);
    if (!t) return 0;
    *e = pa(t) | 3u;
  }
  return (volatile uint64_t*)(uintptr_t)(*e & 0x000FFFFFFFFFF000ull);
}

/* Mapeia [phys, phys+len) em identidade, sem cache (PCD|PWT), em paginas de
 * 2 MB. O BAR pode estar acima de 4 GB (o OVMF poe em 0xE000000000). */
static int mmio_map(uint64_t phys, uint64_t len) {
  volatile uint64_t* pml4 = (volatile uint64_t*)0x1000;  /* de kernel_entry */
  uint64_t a = phys & ~0x1FFFFFull;
  uint64_t end = phys + len;
  if (end > (1ull << 47)) return -1;
  for (; a < end; a += 0x200000ull) {
    volatile uint64_t* pdpt = pt_next(&pml4[(a >> 39) & 511u]);
    volatile uint64_t* pd;
    if (!pdpt) return -1;
    if (pdpt[(a >> 30) & 511u] & 0x80u) continue;  /* pagina de 1 GB: ja mapeado */
    pd = pt_next(&pdpt[(a >> 30) & 511u]);
    if (!pd) return -1;
    pd[(a >> 21) & 511u] = a | 0x9Bu;  /* P | RW | PWT | PCD | PS */
    __asm__ volatile ("invlpg (%0)" : : "r"((uintptr_t)a) : "memory");
  }
  return 0;
}

int pci_mmio_map(uint64_t phys, uint64_t len) {
  return mmio_map(phys, len);
}

/* Log do driver na COM1, com os prefixos [xhci]. */
static void xlog(const char* s) {
  serial_puts(s);
}

static void xlog_hex(uint32_t v) {
  serial_hex32(v);
}

static void log_hex(const char* s, uint32_t v) {
  xlog(s);
  xlog_hex(v);
  xlog("\n");
}

/* ---------- aneis ---------- */

static void ring_init(struct ring* r, struct trb* t) {
  zero(t, RING_N * sizeof(struct trb));
  r->t = t;
  r->idx = 0;
  r->pcs = 1;
  t[RING_N - 1].d[0] = (uint32_t)pa(t);
  t[RING_N - 1].d[1] = (uint32_t)(pa(t) >> 32);
  t[RING_N - 1].d[3] = (TRB_LINK << 10) | 2u;  /* Toggle Cycle */
}

/* Escreve um TRB com o ciclo do produtor (o bit de ciclo por ultimo). */
static uint64_t ring_put(struct ring* r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
  struct trb* t = &r->t[r->idx];
  uint64_t p = pa(t);
  t->d[0] = d0;
  t->d[1] = d1;
  t->d[2] = d2;
  __asm__ volatile ("" : : : "memory");
  t->d[3] = (d3 & ~1u) | r->pcs;
  if (++r->idx == RING_N - 1) {
    struct trb* l = &r->t[RING_N - 1];
    l->d[3] = (l->d[3] & ~1u) | r->pcs;
    r->idx = 0;
    r->pcs ^= 1u;
  }
  return p;
}

static int evt_take(struct xc* x, uint32_t e[4]) {
  struct trb* t = &x->evt[x->evt_idx];
  if ((t->d[3] & 1u) != x->evt_ccs) return 0;
  __asm__ volatile ("" : : : "memory");
  e[0] = t->d[0];
  e[1] = t->d[1];
  e[2] = t->d[2];
  e[3] = t->d[3];
  if (++x->evt_idx == EVT_N) {
    x->evt_idx = 0;
    x->evt_ccs ^= 1u;
  }
  w64(x->rt, 0x20 + 0x18, pa(&x->evt[x->evt_idx]) | 8u);  /* ERDP, limpa EHB */
  return 1;
}

static void evt_other(struct xc* x, const uint32_t e[4]);

static int cmd_run(struct xc* x, uint32_t d0, uint32_t d1, uint32_t d3, int* slot) {
  uint64_t p = ring_put(&x->cmd, d0, d1, 0, d3);
  uint32_t e[4], t0, spins = 0;
  x->db[0] = 0;
  t0 = net_ms();
  for (;;) {
    if (evt_take(x, e)) {
      if (((e[3] >> 10) & 0x3Fu) == EV_CMD && e[0] == (uint32_t)p && e[1] == (uint32_t)(p >> 32)) {
        if (slot) *slot = (int)(e[3] >> 24);
        return (e[2] >> 24) == CC_OK ? 0 : -(int)(e[2] >> 24);
      }
      evt_other(x, e);
      continue;
    }
    if (late(t0, &spins, 500)) return -1000;
  }
}

/* Espera o fim do TRB p no endpoint dci do slot; os outros eventos vao para
 * evt_other. 0 = completo ou curto (*left = bytes que faltaram), >0 = codigo
 * de conclusao do erro, -1 = tempo esgotado. */
static int xfer_wait(struct xc* x, int slot, int dci, uint64_t p, uint32_t ms_max, uint32_t* left) {
  uint32_t e[4], t0 = net_ms(), spins = 0;
  for (;;) {
    if (evt_take(x, e)) {
      if (((e[3] >> 10) & 0x3Fu) == EV_XFER && (int)(e[3] >> 24) == slot &&
          (int)((e[3] >> 16) & 0x1Fu) == dci) {
        uint32_t cc = e[2] >> 24;
        if (cc != CC_OK && cc != CC_SHORT) return (int)cc;
        if (e[0] == (uint32_t)p && e[1] == (uint32_t)(p >> 32)) {
          if (left) *left = e[2] & 0xFFFFFFu;
          return 0;
        }
        continue;
      }
      evt_other(x, e);
      continue;
    }
    if (late(t0, &spins, ms_max)) return -1;
  }
}

/* Endpoint parado (STALL ou erro): volta a rodar e o anel recomeca no
 * proximo TRB livre (os TRBs que sobraram da transferencia ficam para tras). */
static void ep_reset(struct udev* d, int dci, struct ring* r) {
  uint64_t q = pa(&r->t[r->idx]) | r->pcs;
  (void)cmd_run(d->x, 0, 0, (TRB_RESET_EP << 10) | ((uint32_t)dci << 16) |
                            ((uint32_t)d->slot << 24), 0);
  (void)cmd_run(d->x, (uint32_t)q, (uint32_t)(q >> 32), (TRB_SET_DEQ << 10) |
                ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24), 0);
}

/* Requisicao de controle no EP0. Os dados ficam em d->buf (ate 256 bytes).
 * Um STALL (requisicao recusada) para o EP0; o reset deixa ele pronto de novo. */
static int ctrl(struct udev* d, uint8_t rtype, uint8_t req, uint16_t val, uint16_t idx,
                uint16_t len) {
  int in = (rtype & 0x80) != 0;
  uint32_t trt = len ? (in ? 3u : 2u) : 0u;
  uint64_t st;
  int rc;
  ring_put(&d->ep0, (uint32_t)rtype | ((uint32_t)req << 8) | ((uint32_t)val << 16),
           (uint32_t)idx | ((uint32_t)len << 16), 8u,
           (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
  if (len)
    ring_put(&d->ep0, (uint32_t)pa(d->buf), (uint32_t)(pa(d->buf) >> 32), len,
             (TRB_DATA << 10) | (in ? (1u << 16) : 0u));
  st = ring_put(&d->ep0, 0, 0, 0,
                (TRB_STATUS << 10) | (1u << 5) | ((len && in) ? 0u : (1u << 16)));
  d->x->db[d->slot] = 1;
  rc = xfer_wait(d->x, d->slot, 1, st, 500, 0);
  if (rc == CC_STALL) ep_reset(d, 1, &d->ep0);
  return rc;
}

/* ---------- controlador ---------- */

/* 1 se o firmware ainda e o dono do controlador (USB Legacy Support, capacidade 1). */
static int bios_owned(struct xc* x) {
  uint32_t off = (r32(x->cap, 0x10) >> 16) << 2;
  int n = 0;
  while (off && n++ < 64) {
    uint32_t v = r32(x->cap, off);
    uint32_t nx;
    if ((v & 0xFFu) == 1u) return (v >> 16) & 1u;
    nx = (v >> 8) & 0xFFu;
    if (!nx) break;
    off += nx << 2;
  }
  return 0;
}

/* Pede o controlador ao firmware e desliga os SMIs dele. */
static void handoff(struct xc* x) {
  uint32_t off = (r32(x->cap, 0x10) >> 16) << 2;
  int n = 0;
  while (off && n++ < 64) {
    uint32_t v = r32(x->cap, off);
    uint32_t nx;
    if ((v & 0xFFu) == 1u) {
      w32(x->cap, off, v | (1u << 24));
      if (xwait(x->cap, off, 1u << 16, 0, 1000) != 0) {
        xlog("[xhci] firmware nao soltou o controlador, forcando\n");
        w32(x->cap, off, (r32(x->cap, off) & ~(1u << 16)) | (1u << 24));
      }
      /* Mantem so os bits reservados e limpa os eventos SMI (RW1C). */
      w32(x->cap, off + 4, (r32(x->cap, off + 4) & 0x000E1FEEu) | 0xE0000000u);
      return;
    }
    nx = (v >> 8) & 0xFFu;
    if (!nx) break;
    off += nx << 2;
  }
}

static int xc_start(struct xc* x) {
  uint32_t hcs1 = r32(x->cap, 0x04);
  uint32_t hcs2 = r32(x->cap, 0x08);
  uint32_t slots = hcs1 & 0xFFu;
  uint32_t sp = ((hcs2 >> 27) & 0x1Fu) | (((hcs2 >> 21) & 0x1Fu) << 5);
  uint32_t i;
  x->ports = (hcs1 >> 24) & 0xFFu;
  x->ctx = (r32(x->cap, 0x10) & 4u) ? 64u : 32u;
  if (slots > 255u) slots = 255u;

  handoff(x);
  if (xwait(x->op, 0x04, 1u << 11, 0, 1000) != 0) return -1;     /* CNR */
  w32(x->op, 0x00, r32(x->op, 0x00) & ~1u);                      /* R/S = 0 */
  if (xwait(x->op, 0x04, 1u, 1u, 100) != 0) return -2;           /* HCH */
  w32(x->op, 0x00, 2u);                                          /* HCRST */
  if (xwait(x->op, 0x00, 2u, 0, 1000) != 0) return -3;
  if (xwait(x->op, 0x04, 1u << 11, 0, 1000) != 0) return -4;
  xsleep(2);

  if (!x->mem) x->mem = (uint8_t*)xmem(4096u, 4096u);
  if (!x->mem) return -5;
  zero(x->mem, 4096u);
  x->dcbaa = (volatile uint64_t*)x->mem;                 /* 256 entradas */
  x->erst = (volatile uint32_t*)(x->mem + 2048);
  ring_init(&x->cmd, (struct trb*)(x->mem + 2560));
  x->evt = (struct trb*)(x->mem + 3072);                 /* EVT_N = 64 */
  x->evt_idx = 0;
  x->evt_ccs = 1;
  x->pchg = 0;

  if (sp) {
    if (sp > x->sp_n) {
      x->sp_arr = (uint64_t*)xmem(sp * 8u, 64u);
      x->sp_pages = (uint8_t*)umalloc_owned(sp * 4096u + 4096u, 0);
      if (!x->sp_arr || !x->sp_pages) return -6;
      x->sp_pages = (uint8_t*)(uintptr_t)((pa(x->sp_pages) + 4095u) & ~4095ull);
      x->sp_n = sp;
    }
    for (i = 0; i < sp; i++) x->sp_arr[i] = pa(x->sp_pages + i * 4096u);
    x->dcbaa[0] = pa(x->sp_arr);
  }

  w32(x->op, 0x38, slots);                                       /* CONFIG */
  w64(x->op, 0x30, pa(x->dcbaa));                                /* DCBAAP */
  w64(x->op, 0x18, pa(x->cmd.t) | 1u);                           /* CRCR, RCS */
  x->erst[0] = (uint32_t)pa(x->evt);
  x->erst[1] = (uint32_t)(pa(x->evt) >> 32);
  x->erst[2] = EVT_N;
  x->erst[3] = 0;
  w32(x->rt, 0x20 + 0x00, 1u);                                   /* IMAN: limpa IP, IE = 0 */
  w32(x->rt, 0x20 + 0x08, 1u);                                   /* ERSTSZ */
  w64(x->rt, 0x20 + 0x18, pa(x->evt));                           /* ERDP */
  w64(x->rt, 0x20 + 0x10, pa(x->erst));                          /* ERSTBA */
  w32(x->op, 0x00, 1u);                                          /* R/S, sem INTE */
  if (xwait(x->op, 0x04, 1u, 0, 100) != 0) return -7;

  for (i = 0; i < x->ports; i++) {
    uint32_t sc = r32(x->op, 0x400 + i * 0x10);
    if (!(sc & PORT_PP)) w32(x->op, 0x400 + i * 0x10, (sc & PORT_KEEP) | PORT_PP);
  }
  return 0;
}

/* ---------- dispositivos ---------- */

static volatile uint32_t* ictx_at(struct udev* d, int i) {
  return (volatile uint32_t*)(d->ictx + (uint32_t)i * d->x->ctx);
}

/* Entrada livre de devs[], com aneis e contextos zerados. */
static int dev_new(void) {
  int i;
  for (i = 0; i < UDEV_MAX; i++) {
    struct udev* d = &devs[i];
    if (d->kind != UD_FREE) continue;
    if (!d->mem) d->mem = (uint8_t*)xmem(3u * 4096u, 4096u);
    if (!d->mem) return -1;
    zero(d->mem, 3u * 4096u);
    ring_init(&d->ep0, (struct trb*)d->mem);
    ring_init(&d->rin, (struct trb*)(d->mem + 512));
    ring_init(&d->rout, (struct trb*)(d->mem + 1024));
    d->buf = d->mem + 1536;   /* 256 bytes */
    d->rep = d->mem + 1792;   /* 64 bytes */
    d->cbw = d->mem + 1856;   /* 32 bytes */
    d->ictx = d->mem + 4096;  /* 33 contextos */
    d->octx = d->mem + 8192;  /* 32 contextos */
    d->kind = UD_NEW;
    d->slot = 0;
    return i;
  }
  return -1;
}

static void slot_drop(struct xc* x, int slot) {
  if (slot <= 0) return;
  (void)cmd_run(x, 0, 0, (TRB_DIS_SLOT << 10) | ((uint32_t)slot << 24), 0);
  x->dcbaa[slot] = 0;
}

/* Tira o dispositivo e, se for hub, o que estiver atras dele. */
static void dev_drop(int i) {
  struct udev* d;
  int j;
  if (i < 0 || i >= UDEV_MAX) return;
  d = &devs[i];
  if (d->kind == UD_FREE) return;
  for (j = 0; j < UDEV_MAX; j++)
    if (j != i && devs[j].kind != UD_FREE && devs[j].parent == i) dev_drop(j);
  if (i == ms) {
    xlog("[xhci] mouse desconectado\n");
    ms = -1;
    ms_qn = 0;
  }
  if (i == dk) {
    xlog("[xhci] pendrive desconectado\n");
    dk = -1;
    dk_cn = 0;
  }
  slot_drop(d->x, d->slot);
  d->slot = 0;
  d->kind = UD_FREE;
}

static int dev_root(struct xc* x, int p) {
  int i;
  for (i = 0; i < UDEV_MAX; i++)
    if (devs[i].kind != UD_FREE && devs[i].x == x && devs[i].parent < 0 && devs[i].port == p)
      return i;
  return -1;
}

static int dev_child(int h, int hp) {
  int i;
  for (i = 0; i < UDEV_MAX; i++)
    if (devs[i].kind != UD_FREE && devs[i].parent == h && devs[i].hport == hp) return i;
  return -1;
}

/* Hubs entre a porta raiz e o dispositivo (niveis ocupados na route string). */
static int tier(uint32_t route) {
  int n = 0;
  while (n < 5 && (route >> (4 * n))) n++;
  return n;
}

/* Reset da porta raiz USB 2 (a USB 3 ja vem habilitada). Devolve a velocidade
 * (1 FS, 2 LS, 3 HS, 4+ SS) ou -1. */
static int port_reset(struct xc* x, int p) {
  uint32_t off = 0x400 + (uint32_t)p * 0x10;
  uint32_t sc = r32(x->op, off);
  if (!(sc & PORT_CCS)) return -1;
  if (!(sc & PORT_PED)) {
    w32(x->op, off, (sc & PORT_KEEP) | PORT_PR);
    if (xwait(x->op, off, PORT_PRC, PORT_PRC, 500) != 0) return -1;
    xsleep(10);
  }
  sc = r32(x->op, off);
  w32(x->op, off, (sc & PORT_KEEP) | PORT_CHG);
  if (!(sc & PORT_PED) || !(sc & PORT_CCS)) return -1;
  return (int)((sc >> 10) & 0xFu);
}

/* Intervalo do contexto em unidades de 2^n * 125 us. */
static uint32_t ep_interval(int speed, int b) {
  uint32_t v = 0, f;
  if (speed >= 3) {
    if (b < 1) b = 1;
    if (b > 16) b = 16;
    return (uint32_t)(b - 1);
  }
  if (b < 1) b = 1;
  f = (uint32_t)b * 8u;
  while (f > 1u) {
    f >>= 1;
    v++;
  }
  if (v < 3) v = 3;
  if (v > 10) v = 10;
  return v;
}

/* Contexto de entrada do Configure Endpoint, com o slot copiado do de saida. */
static volatile uint32_t* cfg_begin(struct udev* d, uint32_t add, int entries) {
  volatile uint32_t* c;
  int i;
  zero(d->ictx, 33u * d->x->ctx);
  ictx_at(d, 0)[1] = add | 1u;
  c = ictx_at(d, 1);
  for (i = 0; i < 8; i++) c[i] = ((volatile uint32_t*)d->octx)[i];
  if (entries) c[0] = (c[0] & ~(0x1Fu << 27)) | ((uint32_t)entries << 27);
  c[3] = 0;
  return c;
}

static void ep_ctx(struct udev* d, int dci, uint32_t type, int mps, int burst, uint32_t ival,
                   struct ring* r, uint32_t avg) {
  volatile uint32_t* c = ictx_at(d, dci + 1);
  c[0] = ival << 16;
  c[1] = (3u << 1) | (type << 3) | ((uint32_t)burst << 8) | ((uint32_t)mps << 16);
  c[2] = (uint32_t)pa(r->t) | 1u;
  c[3] = (uint32_t)(pa(r->t) >> 32);
  c[4] = avg;
}

static int cfg_run(struct udev* d) {
  return cmd_run(d->x, (uint32_t)pa(d->ictx), (uint32_t)(pa(d->ictx) >> 32),
                 (TRB_CONFIG << 10) | ((uint32_t)d->slot << 24), 0);
}

static int enumerate(struct xc* x, int port, uint32_t route, int speed, int parent, int hport);

/* ---------- hub USB 2 ---------- */

/* Reset da porta hp do hub. Devolve a velocidade como port_reset ou -1. */
static int hub_port_reset(struct udev* d, int hp) {
  uint32_t t0, spins = 0, st = 0, ch = 0;
  if (ctrl(d, 0x23, 3, 4, (uint16_t)hp, 0) != 0) return -1;          /* SET_FEATURE(PORT_RESET) */
  t0 = net_ms();
  for (;;) {
    xsleep(10);
    if (ctrl(d, 0xA3, 0, 0, (uint16_t)hp, 4) != 0) return -1;         /* GET_STATUS */
    st = d->buf[0] | ((uint32_t)d->buf[1] << 8);
    ch = d->buf[2] | ((uint32_t)d->buf[3] << 8);
    if (!(st & (1u << 4)) && (ch & (1u << 4))) break;
    if (late(t0, &spins, 500)) return -1;
  }
  (void)ctrl(d, 0x23, 1, 20, (uint16_t)hp, 0);                         /* C_PORT_RESET */
  xsleep(10);                                                         /* recuperacao */
  if (!(st & 1u) || !(st & 2u)) return -1;
  if (st & (1u << 9)) return 2;
  if (st & (1u << 10)) return 3;
  return 1;
}

/* Le cada porta do hub: tira o que saiu, enumera o que chegou. */
static void hub_scan(int h) {
  struct udev* d = &devs[h];
  int hp;
  for (hp = 1; hp <= d->nports && d->kind == UD_HUB; hp++) {
    uint32_t st, ch;
    int c, speed;
    if (ctrl(d, 0xA3, 0, 0, (uint16_t)hp, 4) != 0) continue;
    st = d->buf[0] | ((uint32_t)d->buf[1] << 8);
    ch = d->buf[2] | ((uint32_t)d->buf[3] << 8);
    if (ch & 1u) (void)ctrl(d, 0x23, 1, 16, (uint16_t)hp, 0);          /* C_PORT_CONNECTION */
    c = dev_child(h, hp);
    if (c >= 0) {
      if ((st & 1u) && !(ch & 1u)) continue;
      dev_drop(c);
    }
    if (!(st & 1u) || tier(d->route) >= 5) continue;
    xsleep(100);  /* debounce da conexao */
    speed = hub_port_reset(d, hp);
    if (speed < 0) continue;
    (void)enumerate(d->x, d->port, d->route | ((uint32_t)hp << (4 * tier(d->route))), speed,
                    h, hp);
  }
}

static int hub_setup(int i, int cfg) {
  struct udev* d = &devs[i];
  volatile uint32_t* c;
  uint32_t chars;
  int n, pg, hp;
  if (d->speed >= 4) {
    xlog("[xhci] hub USB 3 nao suportado\n");
    return -1;
  }
  if (ctrl(d, 0x00, 9, (uint16_t)cfg, 0, 0) != 0) return -1;          /* SET_CONFIGURATION */
  if (ctrl(d, 0xA0, 6, 0x2900, 0, 9) != 0) return -1;                 /* descritor do hub */
  n = d->buf[2];
  chars = d->buf[3] | ((uint32_t)d->buf[4] << 8);
  pg = d->buf[5];
  if (n > 15) n = 15;
  c = cfg_begin(d, 0, 0);
  c[0] |= 1u << 26;                                                   /* Hub */
  c[1] = (c[1] & 0x00FFFFFFu) | ((uint32_t)n << 24);
  if (d->speed == 3) c[2] = (c[2] & ~(3u << 16)) | (((chars >> 5) & 3u) << 16);  /* TTT */
  /* Sem esses campos o controlador ainda roteia pela route string; so o TT
   * (mouse LS/FS atras do hub) depende deles. */
  if (cfg_run(d) != 0) xlog("[xhci] hub: Configure Endpoint recusado, seguindo\n");
  d->nports = n;
  d->kind = UD_HUB;
  for (hp = 1; hp <= n; hp++) (void)ctrl(d, 0x23, 3, 8, (uint16_t)hp, 0);  /* PORT_POWER */
  xsleep((uint32_t)pg * 2u + 100u);
  log_hex("[xhci] hub USB 2, portas ", (uint32_t)n);
  hub_scan(i);
  return 0;
}

/* ---------- mouse ---------- */

static void ms_arm(struct udev* d) {
  ring_put(&d->rin, (uint32_t)pa(d->rep), (uint32_t)(pa(d->rep) >> 32), (uint32_t)d->len,
           (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));  /* IOC | ISP */
  d->x->db[d->slot] = (uint32_t)d->dci_in;
}

static int mouse_setup(int i, int iface, int ep, int mps, int ival, int boot, int cfg) {
  struct udev* d = &devs[i];
  int rc;
  d->dci_in = ep * 2 + 1;
  (void)cfg_begin(d, 1u << d->dci_in, d->dci_in);
  ep_ctx(d, d->dci_in, 7, mps, 0, ep_interval(d->speed, ival), &d->rin,
         (uint32_t)mps | ((uint32_t)mps << 16));                      /* Interrupt IN */
  rc = cfg_run(d);
  if (rc != 0) {
    log_hex("[xhci] Configure Endpoint falhou, cc=", (uint32_t)-rc);
    return -1;
  }
  if (ctrl(d, 0x00, 9, (uint16_t)cfg, 0, 0) != 0) return -1;          /* SET_CONFIGURATION */
  /* Protocolo boot: botoes, X e Y em 3 bytes fixos (o byte 3, se vier, e a roda).
   * O SET_IDLE vai por ultimo: alguns mouses respondem STALL. */
  if (boot) (void)ctrl(d, 0x21, 0x0B, 0, (uint16_t)iface, 0);
  (void)ctrl(d, 0x21, 0x0A, 0, (uint16_t)iface, 0);
  d->iface = iface;
  d->len = mps;
  d->kind = UD_MOUSE;
  ms = i;
  ms_errs = 0;
  ms_fix = 0;
  ms_qn = 0;
  ms_arm(d);
  xlog("[xhci] mouse USB pronto, porta ");
  xlog_hex((uint32_t)(d->port + 1));
  xlog(boot ? " (boot)\n" : " (report)\n");
  return 0;
}

static void ms_report(const uint32_t e[4]) {
  struct udev* d = &devs[ms];
  uint32_t cc = e[2] >> 24, n;
  if (cc != CC_OK && cc != CC_SHORT) {
    ms_fix = 1;
    return;
  }
  n = (uint32_t)d->len - (e[2] & 0xFFFFFFu);
  ms_errs = 0;
  if (n >= 3) {
    int dx = (int8_t)d->rep[1], dy = (int8_t)d->rep[2];
    int dz = (n >= 4) ? (int8_t)d->rep[3] : 0, b = d->rep[0] & 7;
    int* q;
    if (ms_qn < MSQ) {
      q = ms_q[(ms_qh + ms_qn) % MSQ];
      ms_qn++;
      q[0] = q[1] = q[2] = 0;
    } else {
      q = ms_q[(ms_qh + MSQ - 1) % MSQ];  /* fila cheia: soma no ultimo */
    }
    q[0] += dx;
    q[1] += dy;
    q[2] += dz;
    q[3] = b;
  }
  ms_arm(d);
}

/* ---------- pendrive ---------- */

static void clear_halt(struct udev* d, int dci, struct ring* r, int ep) {
  ep_reset(d, dci, r);
  (void)ctrl(d, 0x02, 1, 0, (uint16_t)ep, 0);                         /* CLEAR_FEATURE(HALT) */
}

/* Bulk-Only Mass Storage Reset: o disco volta a esperar um CBW. */
static void bot_reset(struct udev* d) {
  (void)ctrl(d, 0x21, 0xFF, 0, (uint16_t)d->iface, 0);
  clear_halt(d, d->dci_in, &d->rin, d->ep_in);
  clear_halt(d, d->dci_out, &d->rout, d->ep_out);
}

static int bulk(struct udev* d, int in, volatile uint8_t* p, uint32_t len, uint32_t ms_max,
                uint32_t* left) {
  struct ring* r = in ? &d->rin : &d->rout;
  int dci = in ? d->dci_in : d->dci_out;
  uint64_t t = ring_put(r, (uint32_t)pa(p), (uint32_t)(pa(p) >> 32), len,
                        (TRB_NORMAL << 10) | (1u << 5) | (in ? (1u << 2) : 0u));
  d->x->db[d->slot] = (uint32_t)dci;
  return xfer_wait(d->x, d->slot, dci, t, ms_max, left);
}

/* Um comando SCSI pelo bulk-only: CBW, dados (ate DK_RA setores) e CSW.
 * 0 = ok, 1 = o disco recusou (o motivo vem no REQUEST SENSE), -1 = falha
 * de transporte, -2 = o disco nao respondeu. */
static int bot(struct udev* d, const uint8_t* cdb, int n, volatile uint8_t* data, uint32_t len,
               int in) {
  volatile uint8_t* w = d->cbw;
  volatile uint8_t* s = d->rep;
  uint32_t tag = ++dk_tag, left = 0;
  int i, rc;
  zero(w, 31);
  w[0] = 'U';
  w[1] = 'S';
  w[2] = 'B';
  w[3] = 'C';
  put_le32(w + 4, tag);
  put_le32(w + 8, len);
  w[12] = in ? 0x80 : 0;
  w[14] = (uint8_t)n;
  for (i = 0; i < n; i++) w[15 + i] = cdb[i];
  rc = bulk(d, 0, w, 31, 2000, 0);
  if (rc < 0) return -2;
  if (rc != 0) {
    bot_reset(d);
    return -1;
  }
  if (len) {
    rc = bulk(d, in, data, len, 5000, &left);
    if (rc < 0) return -2;
    if (rc == CC_STALL)
      clear_halt(d, in ? d->dci_in : d->dci_out, in ? &d->rin : &d->rout,
                 in ? d->ep_in : d->ep_out);
    else if (rc != 0) {
      bot_reset(d);
      return -1;
    }
  }
  zero(s, 13);
  rc = bulk(d, 1, s, 13, 5000, 0);
  if (rc == CC_STALL) {
    clear_halt(d, d->dci_in, &d->rin, d->ep_in);
    rc = bulk(d, 1, s, 13, 5000, 0);
  }
  if (rc < 0) return -2;
  if (rc != 0 || s[0] != 'U' || s[1] != 'S' || s[2] != 'B' || s[3] != 'S' || le32(s + 4) != tag) {
    bot_reset(d);
    return -1;
  }
  if (s[12] == 0) return 0;
  if (s[12] == 1) return 1;
  bot_reset(d);  /* phase error */
  return -1;
}

static int scsi_sense(struct udev* d) {
  static const uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
  return bot(d, cdb, 6, d->buf, 18, 1);
}

/* "Fabricante Produto" do INQUIRY, sem os espacos de sobra. */
static void inquiry_name(const volatile uint8_t* b) {
  int k = 0, i;
  for (i = 8; i < 32 && k < (int)sizeof(dk_name) - 1; i++) {
    char c = (char)b[i];
    if (c < 32 || c > 126) c = ' ';
    if (i == 16 && k && dk_name[k - 1] != ' ') dk_name[k++] = ' ';
    if (c == ' ' && (k == 0 || dk_name[k - 1] == ' ')) continue;
    dk_name[k++] = c;
  }
  while (k && dk_name[k - 1] == ' ') k--;
  dk_name[k] = 0;
}

static int scsi_init(struct udev* d) {
  static const uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
  static const uint8_t tur[6] = { 0, 0, 0, 0, 0, 0 };
  static const uint8_t cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  uint32_t bs = 0;
  int i, rc = -1;
  if (!dk_data) dk_data = (volatile uint8_t*)xmem(DK_RA * 512u, DK_RA * 512u);
  if (!dk_wbuf) dk_wbuf = (volatile uint8_t*)xmem(512u, 64u);
  if (!dk_data || !dk_wbuf) return -1;
  if (bot(d, inq, 6, d->buf, 36, 1) != 0) {
    xlog("[xhci] pendrive: INQUIRY falhou\n");
    return -1;
  }
  inquiry_name(d->buf);
  /* O primeiro comando costuma voltar com UNIT ATTENTION; alguns pendrives
   * levam um tempo para ficar prontos. */
  for (i = 0; i < 25; i++) {
    rc = bot(d, tur, 6, 0, 0, 0);
    if (rc == 0) break;
    if (rc < 0) return -1;
    (void)scsi_sense(d);
    xsleep(200);
  }
  if (rc != 0) {
    xlog("[xhci] pendrive nao ficou pronto\n");
    return -1;
  }
  for (i = 0; i < 3; i++) {
    rc = bot(d, cap, 10, d->buf, 8, 1);
    if (rc == 0) break;
    if (rc < 0) return -1;
    (void)scsi_sense(d);
  }
  if (rc != 0) return -1;
  dk_blocks = be32(d->buf) + 1u;
  bs = be32(d->buf + 4);
  if (!dk_blocks) dk_blocks = 0xFFFFFFFFu;  /* mais de 2 TB: so os primeiros */
  if (bs != 512u) {
    log_hex("[xhci] pendrive com setor de tamanho nao suportado: ", bs);
    return -1;
  }
  dk_cn = 0;
  dk_fails = 0;
  dk_gen++;
  xlog("[xhci] pendrive pronto: ");
  xlog(dk_name);
  log_hex(", setores ", dk_blocks);
  return 0;
}

static int disk_setup(int i, int iface, int ein, int eout, int mps, int burst, int cfg) {
  struct udev* d = &devs[i];
  int top;
  d->ep_in = ein;
  d->ep_out = eout;
  d->dci_in = (ein & 0x0F) * 2 + 1;
  d->dci_out = (eout & 0x0F) * 2;
  d->iface = iface;
  top = d->dci_in > d->dci_out ? d->dci_in : d->dci_out;
  (void)cfg_begin(d, (1u << d->dci_in) | (1u << d->dci_out), top);
  ep_ctx(d, d->dci_in, 6, mps, burst, 0, &d->rin, 3072u);             /* Bulk IN */
  ep_ctx(d, d->dci_out, 2, mps, burst, 0, &d->rout, 3072u);           /* Bulk OUT */
  if (cfg_run(d) != 0) {
    xlog("[xhci] pendrive: Configure Endpoint falhou\n");
    return -1;
  }
  if (ctrl(d, 0x00, 9, (uint16_t)cfg, 0, 0) != 0) return -1;          /* SET_CONFIGURATION */
  xsleep(20);  /* alguns pendrives so respondem ao primeiro CBW depois de um tempo */
  if (scsi_init(d) != 0) return -1;
  d->kind = UD_DISK;
  dk = i;
  return 0;
}

/* READ(10) ou WRITE(10) de n setores a partir de lba, com dados em buf. */
static int disk_io(int write, uint32_t lba, uint32_t n, volatile uint8_t* buf) {
  uint8_t cdb[10];
  cdb[0] = write ? 0x2A : 0x28;
  cdb[1] = 0;
  cdb[2] = (uint8_t)(lba >> 24);
  cdb[3] = (uint8_t)(lba >> 16);
  cdb[4] = (uint8_t)(lba >> 8);
  cdb[5] = (uint8_t)lba;
  cdb[6] = 0;
  cdb[7] = (uint8_t)(n >> 8);
  cdb[8] = (uint8_t)n;
  cdb[9] = 0;
  return bot(&devs[dk], cdb, 10, buf, n * 512u, !write);
}

/* Uma tentativa a mais quando o disco recusa. Tres falhas de transporte
 * seguidas (ou um disco que nao responde) e ele e dado como removido. */
static int disk_try(int write, uint32_t lba, uint32_t n, volatile uint8_t* buf) {
  int rc = disk_io(write, lba, n, buf);
  if (rc == 1) {
    (void)scsi_sense(&devs[dk]);
    rc = disk_io(write, lba, n, buf);
  }
  if (rc == 0) {
    dk_fails = 0;
    return 0;
  }
  dk_cn = 0;
  if (rc == -2 || (rc < 0 && ++dk_fails >= 3)) dev_drop(dk);
  return -1;
}

/* ---------- enumeracao ---------- */

/* Enumera o dispositivo da porta raiz port (0-based), ou da porta hport do
 * hub parent (route ja inclui essa porta). Devolve o indice em devs[] ou -1. */
static int enumerate(struct xc* x, int port, uint32_t route, int speed, int parent, int hport) {
  int i, slot = 0, rc, total, k, cfg, hub = 0;
  int cur_cls = -1, cur_sub = 0, cur_proto = -1, cur_alt = 0, cur_if = -1, last_bulk = 0;
  int m_if = -1, m_ep = -1, m_mps = 0, m_ival = 0, m_boot = 0;
  int s_if = -1, s_in = -1, s_out = -1, s_mps = 0, s_burst = 0;
  uint32_t mps0;
  volatile uint32_t* c;
  struct udev* d;

  i = dev_new();
  if (i < 0) return -1;
  d = &devs[i];
  d->x = x;
  d->port = port;
  d->route = route;
  d->speed = speed;
  d->parent = parent;
  d->hport = hport;
  d->tt_slot = 0;
  d->tt_port = 0;
  d->nports = 0;
  if (parent >= 0 && speed < 3) {
    struct udev* h = &devs[parent];
    if (h->speed == 3) {
      d->tt_slot = h->slot;
      d->tt_port = hport;
    } else {
      d->tt_slot = h->tt_slot;
      d->tt_port = h->tt_port;
    }
  }

  if (cmd_run(x, 0, 0, TRB_EN_SLOT << 10, &slot) != 0 || slot <= 0) {
    xlog("[xhci] Enable Slot falhou\n");
    d->kind = UD_FREE;
    return -1;
  }
  d->slot = slot;
  x->dcbaa[slot] = pa(d->octx);
  mps0 = (speed >= 4) ? 512u : (speed == 3) ? 64u : 8u;

  ictx_at(d, 0)[1] = 3u;                                         /* A0 | A1 */
  c = ictx_at(d, 1);                                             /* slot */
  c[0] = (1u << 27) | ((uint32_t)speed << 20) | (route & 0xFFFFFu);
  c[1] = (uint32_t)(port + 1) << 16;
  c[2] = (uint32_t)d->tt_slot | ((uint32_t)d->tt_port << 8);
  c = ictx_at(d, 2);                                             /* EP0 */
  c[1] = (3u << 1) | (4u << 3) | (mps0 << 16);
  c[2] = (uint32_t)pa(d->ep0.t) | 1u;
  c[3] = (uint32_t)(pa(d->ep0.t) >> 32);
  c[4] = 8u;
  rc = cmd_run(x, (uint32_t)pa(d->ictx), (uint32_t)(pa(d->ictx) >> 32),
               (TRB_ADDR << 10) | ((uint32_t)slot << 24), 0);
  if (rc != 0) {
    log_hex("[xhci] Address Device falhou, cc=", (uint32_t)-rc);
    goto fail;
  }
  xsleep(2);

  if (ctrl(d, 0x80, 6, 0x0100, 0, 8) != 0) goto fail;
  if (speed == 1 && d->buf[7] && d->buf[7] != mps0) {
    /* Full speed: o EP0 pode ter 8..64 bytes; corrige com Evaluate Context. */
    mps0 = d->buf[7];
    zero(d->ictx, 3u * x->ctx);
    ictx_at(d, 0)[1] = 2u;
    c = ictx_at(d, 2);
    c[1] = (3u << 1) | (4u << 3) | (mps0 << 16);
    if (cmd_run(x, (uint32_t)pa(d->ictx), (uint32_t)(pa(d->ictx) >> 32),
                (TRB_EVAL << 10) | ((uint32_t)slot << 24), 0) != 0)
      goto fail;
  }

  if (ctrl(d, 0x80, 6, 0x0200, 0, 9) != 0) goto fail;
  total = (int)d->buf[2] | ((int)d->buf[3] << 8);
  if (total < 9) total = 9;
  if (total > 256) total = 256;
  if (ctrl(d, 0x80, 6, 0x0200, 0, (uint16_t)total) != 0) goto fail;
  cfg = d->buf[5];

  for (k = 0; k + 1 < total;) {
    int l = d->buf[k];
    int t = d->buf[k + 1];
    if (l < 2 || k + l > total) break;
    if (t == 4 && l >= 9) {
      cur_if = d->buf[k + 2];
      cur_alt = d->buf[k + 3];
      cur_cls = d->buf[k + 5];
      cur_sub = d->buf[k + 6];
      cur_proto = d->buf[k + 7];
      last_bulk = 0;
      if (cur_cls == 9 && cur_alt == 0) hub = 1;
      xlog("[xhci] interface classe ");
      xlog_hex((uint32_t)((cur_cls << 16) | (cur_sub << 8) | cur_proto));
      xlog("\n");
    } else if (t == 5 && l >= 7 && cur_alt == 0) {
      int addr = d->buf[k + 2];
      int attr = d->buf[k + 3] & 3;
      int mps = ((int)d->buf[k + 4] | ((int)d->buf[k + 5] << 8)) & 0x7FF;
      last_bulk = 0;
      if (cur_cls == 3 && cur_proto == 2 && m_ep < 0 && (addr & 0x80) && attr == 3) {
        m_if = cur_if;
        m_boot = cur_sub == 1;
        m_ep = addr & 0x0F;
        m_mps = mps;
        m_ival = d->buf[k + 6];
      } else if (cur_cls == 8 && cur_proto == 0x50 && attr == 2 && (s_if < 0 || s_if == cur_if)) {
        s_if = cur_if;
        if ((addr & 0x80) && s_in < 0) {
          s_in = addr;
          s_mps = mps;
          last_bulk = 1;
        } else if (!(addr & 0x80) && s_out < 0) {
          s_out = addr;
          last_bulk = 1;
        }
      }
    } else if (t == 0x30 && l >= 6 && last_bulk) {
      s_burst = d->buf[k + 2];  /* SuperSpeed: bMaxBurst do companheiro */
    }
    k += l;
  }

  if (hub) {
    if (hub_setup(i, cfg) != 0) goto other;
    return i;
  }
  if (s_in >= 0 && s_out >= 0 && s_mps >= 8 && dk < 0) {
    if (disk_setup(i, s_if, s_in, s_out, s_mps, s_burst, cfg) != 0) goto fail;
    return i;
  }
  if (m_ep >= 0 && m_mps >= 3 && ms < 0) {
    if (mouse_setup(i, m_if, m_ep, m_mps > 64 ? 64 : m_mps, m_ival, m_boot, cfg) != 0) goto fail;
    return i;
  }
  if (m_ep < 0 && s_in < 0) xlog("[xhci] porta sem hub, mouse ou disco\n");
other:
  d->kind = UD_OTHER;
  return i;

fail:
  slot_drop(x, slot);
  d->slot = 0;
  d->kind = UD_FREE;
  return -1;
}

/* Enumera o que estiver ligado e ainda sem dispositivo: as portas raiz de
 * todos os controladores e as portas de cada hub. */
static void scan_all(void) {
  int i, p, h;
  uint32_t e[4];
  busy = 1;
  for (i = 0; i < xc_n; i++) {
    struct xc* x = &xcs[i];
    if (!x->up) continue;
    while (evt_take(x, e)) evt_other(x, e);
    x->pchg = 0;
    for (p = 0; (uint32_t)p < x->ports; p++) {
      uint32_t off = 0x400 + (uint32_t)p * 0x10;
      uint32_t sc = r32(x->op, off);
      int cur = dev_root(x, p);
      int speed;
      if (sc & PORT_CHG) w32(x->op, off, (sc & PORT_KEEP) | PORT_CHG);
      if (cur >= 0) {
        if ((sc & PORT_CCS) && !(sc & PORT_CSC)) continue;
        dev_drop(cur);
      }
      if (!(sc & PORT_CCS)) continue;
      if (cur >= 0) xsleep(100);  /* debounce: reconectado */
      speed = port_reset(x, p);
      if (speed > 0) (void)enumerate(x, p, 0, speed, -1, 0);
    }
  }
  for (h = 0; h < UDEV_MAX; h++)
    if (devs[h].kind == UD_HUB) hub_scan(h);
  busy = 0;
}

/* Eventos que chegam no meio de uma espera ou do polling. Relatorio do mouse
 * entra na fila; mudanca de porta raiz fica marcada para port_work. */
static void evt_other(struct xc* x, const uint32_t e[4]) {
  uint32_t type = (e[3] >> 10) & 0x3Fu;
  if (type == EV_PORT) {
    int p = (int)(e[0] >> 24) - 1;
    if (p >= 0 && p < 32) x->pchg |= 1u << p;
    return;
  }
  if (type == EV_XFER && ms >= 0 && devs[ms].x == x && (int)(e[3] >> 24) == devs[ms].slot &&
      (int)((e[3] >> 16) & 0x1Fu) == devs[ms].dci_in)
    ms_report(e);
}

/* Trata as portas raiz marcadas por evt_other (fora de qualquer espera). */
static void port_work(void) {
  int i;
  if (busy) return;
  for (i = 0; i < xc_n; i++)
    if (xcs[i].up && xcs[i].pchg) {
      scan_all();
      return;
    }
}

/* ---------- interface ---------- */

static int xc_scan(void) {
  int bus, dev, fun;
  xc_n = 0;
  for (bus = 0; bus < 256; bus++) {
    for (dev = 0; dev < 32; dev++) {
      int nf = 1;
      for (fun = 0; fun < nf; fun++) {
        uint32_t id = pci_rd(bus, dev, fun, 0);
        uint32_t cls, bar0, bar1, cmd;
        uint64_t base;
        if (id == 0xFFFFFFFFu || id == 0) continue;
        if (fun == 0 && (pci_rd(bus, dev, 0, 0x0C) & 0x00800000u)) nf = 8;
        cls = pci_rd(bus, dev, fun, 0x08) >> 8;
        if (cls != 0x0C0330u || xc_n >= XC_MAX) continue;
        bar0 = pci_rd(bus, dev, fun, 0x10);
        bar1 = ((bar0 & 6u) == 4u) ? pci_rd(bus, dev, fun, 0x14) : 0;
        base = ((uint64_t)bar1 << 32) | (bar0 & ~0xFu);
        if (!base || (bar0 & 1u)) continue;
        cmd = pci_rd(bus, dev, fun, 0x04);
        pci_wr(bus, dev, fun, 0x04, (cmd & 0xFFFFu) | 0x6u);  /* memoria + bus master */
        if (mmio_map(base, 0x100000u) != 0) {
          log_hex("[xhci] BAR fora do alcance, alto=", bar1);
          continue;
        }
        xcs[xc_n].cap = (volatile uint8_t*)(uintptr_t)base;
        xcs[xc_n].bus = bus;
        xcs[xc_n].dev = dev;
        xcs[xc_n].fun = fun;
        xc_n++;
        xlog("[xhci] controlador ");
        xlog_hex((uint32_t)((bus << 8) | (dev << 3) | fun));
        xlog(" em ");
        if (bar1) xlog_hex(bar1);
        log_hex("", (uint32_t)base);
      }
    }
  }
  return xc_n;
}

/* Liga os controladores que ainda estao parados. ps2_ok != 0: um controlador
 * que o firmware ainda usa fica com ele (emulacao PS/2 de teclado USB).
 * Devolve quantos estao rodando. */
static int xc_up(int ps2_ok) {
  int i, rc, up = 0, fresh = 0;
  if (!xc_n && xc_scan() == 0) {
    xlog("[xhci] nenhum controlador\n");
    return 0;
  }
  for (i = 0; i < xc_n; i++) {
    struct xc* x = &xcs[i];
    if (x->up) {
      up++;
      continue;
    }
    x->op = x->cap + (r32(x->cap, 0) & 0xFFu);
    x->rt = x->cap + (r32(x->cap, 0x18) & ~0x1Fu);
    x->db = (volatile uint32_t*)(x->cap + (r32(x->cap, 0x14) & ~3u));
    if (ps2_ok && bios_owned(x)) {
      xlog("[xhci] firmware usa o controlador e ha mouse PS/2, nao mexe\n");
      continue;
    }
    rc = xc_start(x);
    if (rc != 0) {
      log_hex("[xhci] controlador nao iniciou, erro ", (uint32_t)-rc);
      continue;
    }
    x->up = 1;
    up++;
    fresh++;
  }
  /* As portas voltam a conectar logo depois do reset. */
  if (fresh) xsleep(150);
  return up;
}

int xhci_mouse_init(int ps2_ok) {
  ps2_hint = ps2_ok;
  if (ms >= 0) return 0;
  if (busy || !xc_up(ps2_ok)) return -1;
  scan_all();
  if (ms < 0) xlog("[xhci] nenhum mouse USB\n");
  return ms >= 0 ? 0 : -1;
}

int xhci_mouse_poll(int* dx, int* dy, int* dz, int* btn) {
  static uint32_t scan_ms;
  int i;
  uint32_t e[4];
  if (busy || !xc_n) return 0;
  for (i = 0; i < xc_n; i++)
    if (xcs[i].up)
      while (evt_take(&xcs[i], e)) evt_other(&xcs[i], e);
  if (ms_fix && ms >= 0) {
    /* Endpoint parado (STALL ou erro de transacao): reset e volta ao anel. */
    struct udev* d = &devs[ms];
    ms_fix = 0;
    if (++ms_errs > 8) {
      xlog("[xhci] mouse com erro, desligado\n");
      d->kind = UD_OTHER;
      ms = -1;
    } else {
      ep_reset(d, d->dci_in, &d->rin);
      ms_arm(d);
    }
  }
  /* Reserva para um evento de porta perdido: a cada 500 ms procura uma
   * conexao nova (CSC ainda ligado) enquanto nao ha mouse. */
  if (ms < 0 && net_has_clock() && net_ms() - scan_ms >= 500u) {
    scan_ms = net_ms();
    for (i = 0; i < xc_n; i++) {
      uint32_t p;
      if (!xcs[i].up) continue;
      for (p = 0; p < xcs[i].ports && p < 32; p++) {
        uint32_t sc = r32(xcs[i].op, 0x400 + p * 0x10);
        if ((sc & PORT_CCS) && (sc & PORT_CSC)) xcs[i].pchg |= 1u << p;
      }
    }
  }
  port_work();
  if (!ms_qn) return 0;
  *dx = ms_q[ms_qh][0];
  *dy = ms_q[ms_qh][1];
  *dz = ms_q[ms_qh][2];
  *btn = ms_q[ms_qh][3];
  ms_qh = (ms_qh + 1) % MSQ;
  ms_qn--;
  return 1;
}

int xhci_disk_init(void) {
  const struct BootInfo* bi = bootinfo_get();
  int ps2_ok;
  if (dk >= 0) return 0;
  if (busy) return -1;
  /* No boot UEFI o firmware nao emula PS/2 depois do ExitBootServices: o
   * controlador e nosso mesmo com touchpad PS/2. No BIOS vale o teste do
   * mouse PS/2; sem ele, na duvida, o controlador que o firmware usa fica. */
  if (bi && bi->firmware == BOOTINFO_FIRMWARE_UEFI) ps2_ok = 0;
  else ps2_ok = ps2_hint >= 0 ? ps2_hint : 1;
  if (!xc_up(ps2_ok)) return -1;
  scan_all();
  if (dk < 0) xlog("[xhci] nenhum pendrive\n");
  return dk >= 0 ? 0 : -1;
}

uint32_t xhci_disk_sectors(void) {
  return dk >= 0 ? dk_blocks : 0;
}

uint32_t xhci_disk_gen(void) {
  return dk_gen;
}

const char* xhci_disk_name(void) {
  return dk >= 0 && dk_name[0] ? dk_name : "pendrive";
}

int xhci_disk_read(uint32_t lba, void* buf) {
  uint8_t* out = (uint8_t*)buf;
  volatile uint8_t* src;
  uint32_t i;
  if (busy) return -1;
  port_work();
  if (dk < 0 || lba >= dk_blocks) return -1;
  if (!dk_cn || lba < dk_clba || lba >= dk_clba + dk_cn) {
    /* Le DK_RA setores alinhados: a FAT e os diretorios vem em sequencia. */
    uint32_t start = lba - lba % DK_RA;
    uint32_t n = DK_RA;
    if (n > dk_blocks - start) n = dk_blocks - start;
    dk_cn = 0;
    if (disk_try(0, start, n, dk_data) != 0) {
      if (dk < 0 || disk_try(0, lba, 1, dk_data) != 0) return -1;
      start = lba;
      n = 1;
    }
    dk_clba = start;
    dk_cn = n;
  }
  src = dk_data + (lba - dk_clba) * 512u;
  for (i = 0; i < 512u; i++) out[i] = src[i];
  return 0;
}

int xhci_disk_write(uint32_t lba, const void* buf) {
  const uint8_t* in = (const uint8_t*)buf;
  uint32_t i;
  if (busy) return -1;
  port_work();
  if (dk < 0 || lba >= dk_blocks) return -1;
  for (i = 0; i < 512u; i++) dk_wbuf[i] = in[i];
  if (disk_try(1, lba, 1, dk_wbuf) != 0) return -1;
  if (dk_cn && lba >= dk_clba && lba < dk_clba + dk_cn) {
    volatile uint8_t* dst = dk_data + (lba - dk_clba) * 512u;
    for (i = 0; i < 512u; i++) dst[i] = in[i];
  }
  return 0;
}

/* ---------- comando usb ---------- */

static int put_s(char* out, int pos, int max, const char* s) {
  while (*s && pos < max - 1) out[pos++] = *s++;
  out[pos] = 0;
  return pos;
}

static int put_u(char* out, int pos, int max, uint32_t v) {
  char t[12];
  int n = 0;
  do {
    t[n++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v && n < 11);
  while (n && pos < max - 1) out[pos++] = t[--n];
  out[pos] = 0;
  return pos;
}

static int put_hex2(char* out, int pos, int max, uint32_t v) {
  static const char hx[] = "0123456789ABCDEF";
  if (pos < max - 2) {
    out[pos++] = hx[(v >> 4) & 15u];
    out[pos++] = hx[v & 15u];
  }
  out[pos] = 0;
  return pos;
}

/* Caminho como o Linux mostra: porta raiz e as portas de cada hub (4.1). */
static int put_path(char* out, int pos, int max, const struct udev* d) {
  int n = tier(d->route), t;
  pos = put_u(out, pos, max, (uint32_t)(d->port + 1));
  for (t = 0; t < n; t++) {
    pos = put_s(out, pos, max, ".");
    pos = put_u(out, pos, max, (d->route >> (4 * t)) & 15u);
  }
  return pos;
}

int xhci_describe(char* out, int max) {
  static const char* const spd[] = { "?", "FS", "LS", "HS", "SS", "SS+" };
  int i, j, pos = 0;
  if (!out || max < 2) return 0;
  out[0] = 0;
  if (!xc_n) return put_s(out, 0, max, "Nenhum controlador xHCI.\n");
  for (i = 0; i < xc_n; i++) {
    struct xc* x = &xcs[i];
    pos = put_s(out, pos, max, "xHCI ");
    pos = put_hex2(out, pos, max, (uint32_t)x->bus);
    pos = put_s(out, pos, max, ":");
    pos = put_hex2(out, pos, max, (uint32_t)x->dev);
    pos = put_s(out, pos, max, ".");
    pos = put_u(out, pos, max, (uint32_t)x->fun);
    if (!x->up) {
      pos = put_s(out, pos, max, "  parado (fica com o firmware)\n");
      continue;
    }
    pos = put_s(out, pos, max, "  ");
    pos = put_u(out, pos, max, x->ports);
    pos = put_s(out, pos, max, " portas\n");
    for (j = 0; j < UDEV_MAX; j++) {
      const struct udev* d = &devs[j];
      int s = d->speed;
      if (d->kind < UD_OTHER || d->x != x) continue;
      pos = put_s(out, pos, max, "  porta ");
      pos = put_path(out, pos, max, d);
      pos = put_s(out, pos, max, "  ");
      pos = put_s(out, pos, max, spd[s < 0 || s > 5 ? 0 : s]);
      pos = put_s(out, pos, max, "  ");
      if (d->kind == UD_HUB) {
        pos = put_s(out, pos, max, "hub, ");
        pos = put_u(out, pos, max, (uint32_t)d->nports);
        pos = put_s(out, pos, max, " portas");
      } else if (d->kind == UD_MOUSE) {
        pos = put_s(out, pos, max, "mouse");
      } else if (d->kind == UD_DISK) {
        pos = put_s(out, pos, max, "pendrive ");
        pos = put_s(out, pos, max, dk_name);
        pos = put_s(out, pos, max, ", ");
        pos = put_u(out, pos, max, dk_blocks / 2048u);
        pos = put_s(out, pos, max, " MB");
      } else {
        pos = put_s(out, pos, max, "outro (sem driver)");
      }
      pos = put_s(out, pos, max, "\n");
    }
  }
  return pos;
}
