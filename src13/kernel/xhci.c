/* xhci.c - Controlador USB 3 (xHCI) e mouse USB no protocolo boot.
 *
 * Tudo por polling, como o resto do kernel (sem IRQ):
 *   1. acha os xHCI no PCI (classe 0C0330, qualquer barramento);
 *   2. mapeia o BAR sem cache e toma o controlador do firmware (BIOS handoff);
 *   3. reinicia, monta DCBAA, anel de comandos e anel de eventos;
 *   4. em cada porta raiz conectada: reset, Enable Slot, Address Device,
 *      descritores, Configure Endpoint e SET_PROTOCOL(boot);
 *   5. deixa um Normal TRB no endpoint interrupt IN e le cada relatorio.
 * So um mouse por vez. Hubs nao: o mouse precisa estar numa porta do PC.
 * A memoria e toda de identidade (fisico = virtual) abaixo de 256 MB. */
#include <stdint.h>
#include "xhci.h"
#include "mm.h"
#include "net.h"
#include "serial.h"

#define XC_MAX 4
#define RING_N 32
#define EVT_N  64

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
  volatile uint8_t* cap;
  volatile uint8_t* op;
  volatile uint8_t* rt;
  volatile uint32_t* db;
  uint32_t ctx;            /* 32 ou 64 bytes por contexto (HCCPARAMS1.CSZ) */
  uint32_t ports;
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

static struct xc xcs[XC_MAX];
static int xc_n;

/* O dispositivo em uso (o mouse) ou em enumeracao. 3 paginas:
 * aneis do EP0 e do interrupt, buffers, contexto de entrada e de saida. */
static uint8_t* dev_mem;
static struct ring ep0, irq_in;
static volatile uint8_t* ctl_buf;
static volatile uint8_t* rep_buf;
static volatile uint8_t* ictx;
static volatile uint8_t* octx;

static struct xc* ms_xc;
static int ms_ok;
static int ms_slot;
static int ms_port;
static int ms_dci;
static int ms_len;
static int ms_errs;
static int busy;

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

/* Requisicao de controle no EP0 do dispositivo em enumeracao. */
static int ctrl(struct xc* x, int slot, uint8_t rtype, uint8_t req, uint16_t val,
                uint16_t idx, uint16_t len) {
  int in = (rtype & 0x80) != 0;
  uint32_t trt = len ? (in ? 3u : 2u) : 0u;
  uint32_t e[4], t0, spins = 0;
  uint64_t st;
  ring_put(&ep0, (uint32_t)rtype | ((uint32_t)req << 8) | ((uint32_t)val << 16),
           (uint32_t)idx | ((uint32_t)len << 16), 8u,
           (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
  if (len)
    ring_put(&ep0, (uint32_t)pa(ctl_buf), (uint32_t)(pa(ctl_buf) >> 32), len,
             (TRB_DATA << 10) | (in ? (1u << 16) : 0u));
  st = ring_put(&ep0, 0, 0, 0,
                (TRB_STATUS << 10) | (1u << 5) | ((len && in) ? 0u : (1u << 16)));
  x->db[slot] = 1;
  t0 = net_ms();
  for (;;) {
    if (evt_take(x, e)) {
      if (((e[3] >> 10) & 0x3Fu) == EV_XFER && (int)(e[3] >> 24) == slot &&
          ((e[3] >> 16) & 0x1Fu) == 1u) {
        uint32_t cc = e[2] >> 24;
        if (cc != CC_OK && cc != CC_SHORT) return -(int)cc;
        if (e[0] == (uint32_t)st && e[1] == (uint32_t)(st >> 32)) return 0;
        continue;
      }
      evt_other(x, e);
      continue;
    }
    if (late(t0, &spins, 500)) return -1000;
  }
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

/* ---------- dispositivo ---------- */

static void dev_layout(void) {
  zero(dev_mem, 3u * 4096u);
  ring_init(&ep0, (struct trb*)dev_mem);
  ring_init(&irq_in, (struct trb*)(dev_mem + 512));
  ctl_buf = dev_mem + 1024;   /* 256 bytes */
  rep_buf = dev_mem + 1280;   /* 64 bytes */
  ictx = dev_mem + 4096;      /* 33 contextos */
  octx = dev_mem + 8192;      /* 32 contextos */
}

static volatile uint32_t* ictx_at(struct xc* x, int i) {
  return (volatile uint32_t*)(ictx + (uint32_t)i * x->ctx);
}

/* Reset da porta USB 2 (a USB 3 ja vem habilitada). Devolve a velocidade
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

static void slot_drop(struct xc* x, int slot) {
  if (slot <= 0) return;
  (void)cmd_run(x, 0, 0, (TRB_DIS_SLOT << 10) | ((uint32_t)slot << 24), 0);
  x->dcbaa[slot] = 0;
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

static void irq_arm(void) {
  ring_put(&irq_in, (uint32_t)pa(rep_buf), (uint32_t)(pa(rep_buf) >> 32), (uint32_t)ms_len,
           (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));  /* IOC | ISP */
  ms_xc->db[ms_slot] = (uint32_t)ms_dci;
}

/* Enumera o que estiver na porta p (0-based). 0 = mouse ligado. */
static int enumerate(struct xc* x, int p) {
  int speed, slot = 0, rc, total, i;
  int iface = -1, ep = -1, mps = 0, ival = 0, boot = 0, cfg;
  int cur_cls = -1, cur_proto = -1, cur_alt = 0, cur_if = -1, cur_sub = 0;
  uint32_t mps0;
  volatile uint32_t* c;

  if (!dev_mem) dev_mem = (uint8_t*)xmem(3u * 4096u, 4096u);
  if (!dev_mem) return -1;
  speed = port_reset(x, p);
  if (speed < 0) return -1;
  dev_layout();

  if (cmd_run(x, 0, 0, TRB_EN_SLOT << 10, &slot) != 0 || slot <= 0) {
    xlog("[xhci] Enable Slot falhou\n");
    return -1;
  }
  x->dcbaa[slot] = pa(octx);
  mps0 = (speed >= 4) ? 512u : (speed == 3) ? 64u : 8u;

  ictx_at(x, 0)[1] = 3u;                                         /* A0 | A1 */
  c = ictx_at(x, 1);                                             /* slot */
  c[0] = (1u << 27) | ((uint32_t)speed << 20);
  c[1] = (uint32_t)(p + 1) << 16;
  c = ictx_at(x, 2);                                             /* EP0 */
  c[1] = (3u << 1) | (4u << 3) | (mps0 << 16);
  c[2] = (uint32_t)pa(ep0.t) | 1u;
  c[3] = (uint32_t)(pa(ep0.t) >> 32);
  c[4] = 8u;
  rc = cmd_run(x, (uint32_t)pa(ictx), (uint32_t)(pa(ictx) >> 32),
               (TRB_ADDR << 10) | ((uint32_t)slot << 24), 0);
  if (rc != 0) {
    log_hex("[xhci] Address Device falhou, cc=", (uint32_t)-rc);
    goto fail;
  }
  xsleep(2);

  if (ctrl(x, slot, 0x80, 6, 0x0100, 0, 8) != 0) goto fail;
  if (speed == 1 && ctl_buf[7] && ctl_buf[7] != mps0) {
    /* Full speed: o EP0 pode ter 8..64 bytes; corrige com Evaluate Context. */
    mps0 = ctl_buf[7];
    zero(ictx, 3u * x->ctx);
    ictx_at(x, 0)[1] = 2u;
    c = ictx_at(x, 2);
    c[1] = (3u << 1) | (4u << 3) | (mps0 << 16);
    if (cmd_run(x, (uint32_t)pa(ictx), (uint32_t)(pa(ictx) >> 32),
                (TRB_EVAL << 10) | ((uint32_t)slot << 24), 0) != 0)
      goto fail;
  }

  if (ctrl(x, slot, 0x80, 6, 0x0200, 0, 9) != 0) goto fail;
  total = (int)ctl_buf[2] | ((int)ctl_buf[3] << 8);
  if (total < 9) total = 9;
  if (total > 256) total = 256;
  if (ctrl(x, slot, 0x80, 6, 0x0200, 0, (uint16_t)total) != 0) goto fail;
  cfg = ctl_buf[5];

  for (i = 0; i + 1 < total;) {
    int l = ctl_buf[i];
    int t = ctl_buf[i + 1];
    if (l < 2 || i + l > total) break;
    if (t == 4 && l >= 9) {
      cur_if = ctl_buf[i + 2];
      cur_alt = ctl_buf[i + 3];
      cur_cls = ctl_buf[i + 5];
      cur_sub = ctl_buf[i + 6];
      cur_proto = ctl_buf[i + 7];
      xlog("[xhci] interface classe ");
      xlog_hex((uint32_t)((cur_cls << 16) | (cur_sub << 8) | cur_proto));
      xlog("\n");
    } else if (t == 5 && l >= 7 && ep < 0 && cur_cls == 3 && cur_proto == 2 && cur_alt == 0 &&
               (ctl_buf[i + 2] & 0x80) && (ctl_buf[i + 3] & 3) == 3) {
      iface = cur_if;
      boot = cur_sub == 1;
      ep = ctl_buf[i + 2] & 0x0F;
      mps = ((int)ctl_buf[i + 4] | ((int)ctl_buf[i + 5] << 8)) & 0x7FF;
      ival = ctl_buf[i + 6];
    }
    i += l;
  }
  if (ep < 0 || mps < 3) {
    xlog("[xhci] porta sem mouse HID\n");
    goto fail;
  }
  if (mps > 64) mps = 64;

  ms_dci = ep * 2 + 1;
  zero(ictx, 33u * x->ctx);
  ictx_at(x, 0)[1] = 1u | (1u << ms_dci);
  c = ictx_at(x, 1);
  for (i = 0; i < 8; i++) c[i] = ((volatile uint32_t*)octx)[i];
  c[0] = (c[0] & ~(0x1Fu << 27)) | ((uint32_t)ms_dci << 27);
  c[3] = 0;
  c = ictx_at(x, ms_dci + 1);
  c[0] = ep_interval(speed, ival) << 16;
  c[1] = (3u << 1) | (7u << 3) | ((uint32_t)mps << 16);         /* Interrupt IN */
  c[2] = (uint32_t)pa(irq_in.t) | 1u;
  c[3] = (uint32_t)(pa(irq_in.t) >> 32);
  c[4] = (uint32_t)mps | ((uint32_t)mps << 16);
  rc = cmd_run(x, (uint32_t)pa(ictx), (uint32_t)(pa(ictx) >> 32),
               (TRB_CONFIG << 10) | ((uint32_t)slot << 24), 0);
  if (rc != 0) {
    log_hex("[xhci] Configure Endpoint falhou, cc=", (uint32_t)-rc);
    goto fail;
  }
  if (ctrl(x, slot, 0x00, 9, (uint16_t)cfg, 0, 0) != 0) goto fail;  /* SET_CONFIGURATION */
  /* Protocolo boot: botoes, X e Y em 3 bytes fixos (o byte 3, se vier, e a roda).
   * O SET_IDLE vai por ultimo: alguns mouses respondem STALL e param o EP0. */
  if (boot) (void)ctrl(x, slot, 0x21, 0x0B, 0, (uint16_t)iface, 0);
  (void)ctrl(x, slot, 0x21, 0x0A, 0, (uint16_t)iface, 0);

  ms_xc = x;
  ms_slot = slot;
  ms_port = p;
  ms_len = mps;
  ms_errs = 0;
  ms_ok = 1;
  irq_arm();
  xlog("[xhci] mouse USB pronto, porta ");
  xlog_hex((uint32_t)(p + 1));
  xlog(boot ? " (boot)\n" : " (report)\n");
  return 0;

fail:
  slot_drop(x, slot);
  return -1;
}

/* Porta mudou: mouse tirado ou algo conectado. Chamar com busy = 1. */
static void port_event(struct xc* x, int p) {
  uint32_t off = 0x400 + (uint32_t)p * 0x10;
  uint32_t sc = r32(x->op, off);
  w32(x->op, off, (sc & PORT_KEEP) | PORT_CHG);
  if (ms_ok && ms_xc == x && ms_port == p) {
    if (!(sc & PORT_CCS)) {
      xlog("[xhci] mouse desconectado\n");
      ms_ok = 0;
      slot_drop(x, ms_slot);
    }
    return;
  }
  if (!ms_ok && (sc & PORT_CCS) && (sc & PORT_CSC)) {
    xsleep(100);  /* debounce da conexao */
    (void)enumerate(x, p);
  }
}

/* Eventos que chegam no meio de uma espera ou do polling. Durante uma
 * enumeracao (busy) o evento de porta fica para a varredura de reserva. */
static void evt_other(struct xc* x, const uint32_t e[4]) {
  int p;
  if (((e[3] >> 10) & 0x3Fu) != EV_PORT || !x->up || busy) return;
  p = (int)(e[0] >> 24) - 1;
  if (p < 0 || (uint32_t)p >= x->ports) return;
  busy = 1;
  port_event(x, p);
  busy = 0;
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

int xhci_mouse_init(int ps2_ok) {
  int i, p, rc, up = 0;
  if (ms_ok) return 0;
  if (!xc_n && xc_scan() == 0) {
    xlog("[xhci] nenhum controlador\n");
    return -1;
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
      /* Emulacao PS/2 do firmware ativa: tirar o xHCI dele derruba o teclado USB. */
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
  }
  if (!up) return -1;
  /* As portas voltam a conectar logo depois do reset. */
  xsleep(150);
  busy = 1;
  for (i = 0; i < xc_n && !ms_ok; i++) {
    struct xc* x = &xcs[i];
    uint32_t e[4];
    if (!x->up) continue;
    while (evt_take(x, e)) { }  /* eventos de porta do reset; as portas sao lidas abaixo */
    for (p = 0; (uint32_t)p < x->ports && !ms_ok; p++)
      if (r32(x->op, 0x400 + (uint32_t)p * 0x10) & PORT_CCS) (void)enumerate(x, p);
  }
  busy = 0;
  if (!ms_ok) xlog("[xhci] nenhum mouse USB\n");
  return ms_ok ? 0 : -1;
}

int xhci_mouse_poll(int* dx, int* dy, int* dz, int* btn) {
  static uint32_t scan_ms;
  int i, got = 0;
  uint32_t e[4];
  /* Reserva para um evento de porta perdido: a cada 500 ms procura uma
   * conexao nova (CSC ainda ligado) enquanto nao ha mouse. */
  if (!ms_ok && !busy && xc_n && net_has_clock() && net_ms() - scan_ms >= 500u) {
    scan_ms = net_ms();
    busy = 1;
    for (i = 0; i < xc_n && !ms_ok; i++) {
      uint32_t p;
      if (!xcs[i].up) continue;
      for (p = 0; p < xcs[i].ports && !ms_ok; p++) {
        uint32_t sc = r32(xcs[i].op, 0x400 + p * 0x10);
        if ((sc & PORT_CCS) && (sc & PORT_CSC)) port_event(&xcs[i], (int)p);
      }
    }
    busy = 0;
  }
  for (i = 0; i < xc_n; i++) {
    struct xc* x = &xcs[i];
    if (!x->up) continue;
    while (!got && evt_take(x, e)) {
      uint32_t cc, n;
      if (((e[3] >> 10) & 0x3Fu) != EV_XFER || !ms_ok || x != ms_xc ||
          (int)(e[3] >> 24) != ms_slot || (int)((e[3] >> 16) & 0x1Fu) != ms_dci) {
        evt_other(x, e);
        continue;
      }
      cc = e[2] >> 24;
      if (cc != CC_OK && cc != CC_SHORT) {
        /* Endpoint parado (STALL ou erro de transacao): reset e volta ao anel. */
        uint64_t q = pa(&irq_in.t[irq_in.idx]) | irq_in.pcs;
        if (++ms_errs > 8) {
          log_hex("[xhci] mouse com erro, desligado, cc=", cc);
          ms_ok = 0;
          continue;
        }
        (void)cmd_run(x, 0, 0, (TRB_RESET_EP << 10) | ((uint32_t)ms_dci << 16) |
                               ((uint32_t)ms_slot << 24), 0);
        (void)cmd_run(x, (uint32_t)q, (uint32_t)(q >> 32), (TRB_SET_DEQ << 10) |
                      ((uint32_t)ms_dci << 16) | ((uint32_t)ms_slot << 24), 0);
        if (ms_ok) irq_arm();  /* o cmd_run pode ter visto o mouse sair */
        continue;
      }
      n = (uint32_t)ms_len - (e[2] & 0xFFFFFFu);
      ms_errs = 0;
      if (n >= 3) {
        *btn = rep_buf[0] & 7;
        *dx = (int8_t)rep_buf[1];
        *dy = (int8_t)rep_buf[2];
        *dz = (n >= 4) ? (int8_t)rep_buf[3] : 0;
        got = 1;
      }
      irq_arm();
    }
  }
  return got;
}
