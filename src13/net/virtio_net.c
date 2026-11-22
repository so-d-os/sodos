/* virtio_net.c - virtio-net legada (PCI 1AF4:1000) em polling.
 *
 * Registradores (BAR0, portas de E/S, sem MSI-X):
 *   00 recursos da placa   04 recursos aceitos   08 PFN da fila
 *   0C tamanho da fila     0E fila escolhida     10 aviso (notify)
 *   12 estado              13 ISR                14 MAC (6 bytes)
 * Fila 0 = recepcao, fila 1 = envio. Cada fila e um vring legado alinhado
 * em 4 KB: descritores, anel "avail" (nosso) e, na pagina seguinte, o anel
 * "used" (do QEMU). Sem recursos negociados alem do MAC, cada quadro vem
 * atras de um cabecalho virtio_net_hdr de 10 bytes, no mesmo buffer.
 *
 * Sem IRQ: avail.flags = NO_INTERRUPT e INTx desligado no PCI. O rx da pilha
 * (nic_poll) le o anel used; os buffers voltam ao avail e o aviso ao QEMU sai
 * em lote (cada outw e uma saida da VM). */
#include <stdint.h>
#include "virtio_net.h"
#include "net.h"
#include "mm.h"
#include "serial.h"

extern void puts_k(const char* s);

#define VN_HDR   10u
#define VN_BUF   2048u
#define VN_QMAX  1024u

#define R_FEAT_DEV   0x00
#define R_FEAT_DRV   0x04
#define R_QPFN       0x08
#define R_QSIZE      0x0C
#define R_QSEL       0x0E
#define R_QNOTIFY    0x10
#define R_STATUS     0x12
#define R_ISR        0x13
#define R_MAC        0x14

#define ST_ACK       1u
#define ST_DRIVER    2u
#define ST_DRIVER_OK 4u
#define ST_FAILED    0x80u

#define F_MAC        (1u << 5)

#define D_NEXT       1u
#define D_WRITE      2u

#define AVAIL_NO_INT 1u
#define USED_NO_NOTIFY 1u

struct vdesc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct vq {
  uint16_t n;                 /* tamanho (da placa) */
  volatile struct vdesc* desc;
  volatile uint16_t* avail;   /* [0] flags [1] idx [2..] ring */
  volatile uint16_t* used;    /* [0] flags [1] idx, depois {u32 id, u32 len} */
  uint8_t* buf;               /* n buffers de VN_BUF */
  uint16_t avail_idx;         /* proximo avail.idx */
  uint16_t used_seen;         /* ultimo used.idx consumido */
  uint16_t pending;           /* buffers devolvidos ainda sem aviso */
};

static struct {
  uint16_t io;
  int bus, dev, fun;
  uint8_t mac[6];
  struct vq rx, tx;
  int up;
  uint32_t rx_ok, rx_err, tx_ok, tx_busy, kicks;
} vn;

/* ---------- E/S ---------- */

static inline void outb_(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outw_(uint16_t p, uint16_t v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outl_(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t inb_(uint16_t p) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw_(uint16_t p) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl_(uint16_t p) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void mb(void) { __asm__ volatile("mfence" ::: "memory"); }
static inline void cb(void) { __asm__ volatile("" ::: "memory"); }

static uint32_t pci_rd(int bus, int dev, int fun, int off) {
  outl_(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
               ((uint32_t)fun << 8) | ((uint32_t)off & 0xFCu));
  return inl_(0xCFC);
}
static void pci_wr(int bus, int dev, int fun, int off, uint32_t v) {
  outl_(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
               ((uint32_t)fun << 8) | ((uint32_t)off & 0xFCu));
  outl_(0xCFC, v);
}

/* ---------- log ---------- */

static void lg(const char* s) {
  puts_k(s);
  serial_puts(s);
}
static void lg_u(uint32_t v) {
  char b[12];
  int i = 11;
  b[i] = 0;
  do {
    b[--i] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v && i > 0);
  lg(b + i);
}
static void lg_hex2(uint8_t v) {
  char b[3];
  const char* h = "0123456789ABCDEF";
  b[0] = h[v >> 4];
  b[1] = h[v & 15];
  b[2] = 0;
  lg(b);
}
static void lg_mac(const uint8_t* m) {
  int i;
  for (i = 0; i < 6; i++) {
    lg_hex2(m[i]);
    if (i != 5) lg(":");
  }
}

/* ---------- memoria (identidade: endereco virtual = fisico) ---------- */

static void* dma_alloc(uint32_t n) {
  uint8_t* raw = (uint8_t*)umalloc_owned(n + 4096u, 0);
  uint8_t* a;
  uint32_t i;
  if (!raw) return 0;
  a = (uint8_t*)(uintptr_t)(((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u);
  for (i = 0; i < n; i++) a[i] = 0;
  return a;
}

static uint32_t align4k(uint32_t v) { return (v + 4095u) & ~4095u; }

/* Monta a fila q (0 rx, 1 tx). 0 = ok. */
static int vq_setup(struct vq* q, uint16_t idx) {
  uint32_t n, avail_off, used_off, size;
  uint8_t* mem;
  uint64_t phys;
  outw_((uint16_t)(vn.io + R_QSEL), idx);
  n = inw_((uint16_t)(vn.io + R_QSIZE));
  if (n == 0 || n > VN_QMAX || (n & (n - 1u))) return -1;
  avail_off = 16u * n;
  used_off = align4k(avail_off + 6u + 2u * n);
  size = used_off + align4k(6u + 8u * n);
  mem = (uint8_t*)dma_alloc(size);
  q->buf = (uint8_t*)dma_alloc(n * VN_BUF);
  if (!mem || !q->buf) return -2;
  phys = (uint64_t)(uintptr_t)mem;
  if ((phys >> 12) > 0xFFFFFFFFull) return -2;
  q->n = (uint16_t)n;
  q->desc = (volatile struct vdesc*)mem;
  q->avail = (volatile uint16_t*)(mem + avail_off);
  q->used = (volatile uint16_t*)(mem + used_off);
  q->avail_idx = 0;
  q->used_seen = 0;
  q->pending = 0;
  q->avail[0] = AVAIL_NO_INT;
  outl_((uint16_t)(vn.io + R_QPFN), (uint32_t)(phys >> 12));
  return 0;
}

static void vq_kick(struct vq* q, uint16_t idx) {
  mb();
  q->pending = 0;
  if (q->used[0] & USED_NO_NOTIFY) return;
  outw_((uint16_t)(vn.io + R_QNOTIFY), idx);
  vn.kicks++;
}

/* Devolve o buffer id da recepcao ao anel avail. */
static void rx_give(uint16_t id) {
  struct vq* q = &vn.rx;
  q->desc[id].addr = (uint64_t)(uintptr_t)(q->buf + (uint32_t)id * VN_BUF);
  q->desc[id].len = VN_BUF;
  q->desc[id].flags = D_WRITE;
  q->desc[id].next = 0;
  q->avail[2 + (q->avail_idx % q->n)] = id;
  cb();
  q->avail_idx++;
  q->avail[1] = q->avail_idx;
  q->pending++;
}

/* ---------- quadros ---------- */

static int vn_tx(const uint8_t* frame, int len) {
  struct vq* q = &vn.tx;
  uint16_t slot;
  uint8_t* b;
  uint32_t i, spins;
  if (!vn.up || len < 14) return -1;
  if (len > 1514) len = 1514;
  /* anel cheio: espera o QEMU consumir (ele processa no aviso) */
  for (spins = 0; (uint16_t)(q->avail_idx - q->used[1]) >= q->n && spins < 2000000u; spins++)
    __asm__ volatile("pause");
  if ((uint16_t)(q->avail_idx - q->used[1]) >= q->n) {
    vn.tx_busy++;
    return -1;
  }
  q->used_seen = q->used[1];
  slot = (uint16_t)(q->avail_idx % q->n);
  b = q->buf + (uint32_t)slot * VN_BUF;
  for (i = 0; i < VN_HDR; i++) b[i] = 0;
  for (i = 0; i < (uint32_t)len; i++) b[VN_HDR + i] = frame[i];
  q->desc[slot].addr = (uint64_t)(uintptr_t)b;
  q->desc[slot].len = VN_HDR + (uint32_t)len;
  q->desc[slot].flags = 0;
  q->desc[slot].next = 0;
  q->avail[2 + slot] = slot;
  cb();
  q->avail_idx++;
  q->avail[1] = q->avail_idx;
  vq_kick(q, 1);
  vn.tx_ok++;
  return 0;
}

static int vn_rx(uint8_t* frame, int cap) {
  struct vq* q = &vn.rx;
  if (!vn.up) return 0;
  for (;;) {
    uint16_t at, id;
    uint32_t len, k;
    volatile uint32_t* e;
    const uint8_t* b;
    if (q->used_seen == q->used[1]) {
      /* nada novo: avisa o QEMU dos buffers devolvidos */
      if (q->pending) vq_kick(q, 0);
      return 0;
    }
    __asm__ volatile("lfence" ::: "memory");
    at = (uint16_t)(q->used_seen % q->n);
    e = (volatile uint32_t*)(q->used + 2) + 2u * at;
    id = (uint16_t)e[0];
    len = e[1];
    q->used_seen++;
    if (id >= q->n || len < VN_HDR + 14u || len > VN_BUF) {
      vn.rx_err++;
      if (id < q->n) rx_give(id);
      continue;
    }
    len -= VN_HDR;
    if (len > (uint32_t)cap) len = (uint32_t)cap;
    b = q->buf + (uint32_t)id * VN_BUF + VN_HDR;
    for (k = 0; k < len; k++) frame[k] = b[k];
    rx_give(id);
    /* com rajada grande, avisa antes de esgotar os buffers da placa */
    if (q->pending >= q->n / 4u) vq_kick(q, 0);
    vn.rx_ok++;
    return (int)len;
  }
}

static void vn_tick(void) {
  if (vn.up && vn.rx.pending) vq_kick(&vn.rx, 0);
}

static const struct net_link vn_link = {"virtio-net", vn.mac, vn_tx, vn_rx, vn_tick};

/* ---------- PCI ---------- */

static int find(void) {
  int bus, dev, fun;
  for (bus = 0; bus < 8; bus++)
    for (dev = 0; dev < 32; dev++) {
      int nf = 1;
      for (fun = 0; fun < nf; fun++) {
        uint32_t id = pci_rd(bus, dev, fun, 0);
        if (id == 0xFFFFFFFFu) continue;
        if (fun == 0 && (pci_rd(bus, dev, 0, 0x0C) & 0x00800000u)) nf = 8;
        /* 1000 = virtio-net transicional (com a interface legada);
         * subsistema 1 = rede */
        if (id != 0x10001AF4u) continue;
        if ((pci_rd(bus, dev, fun, 0x2C) >> 16) != 1u) continue;
        vn.bus = bus;
        vn.dev = dev;
        vn.fun = fun;
        return 0;
      }
    }
  return -1;
}

int virtio_net_present(void) { return vn.up; }

int virtio_net_start(void) {
  uint32_t bar, cmd, feat;
  uint16_t i;
  if (vn.up) return 0;
  if (find() != 0) return -1;
  bar = pci_rd(vn.bus, vn.dev, vn.fun, 0x10);
  if ((bar & 1u) == 0 || (bar & ~3u) == 0) {
    serial_puts("[virtio-net] BAR0 sem portas de E/S (disable-legacy?)\n");
    return -1;
  }
  vn.io = (uint16_t)(bar & ~3u);
  cmd = pci_rd(vn.bus, vn.dev, vn.fun, 0x04);
  /* portas + bus master, INTx desligado (so polling) */
  pci_wr(vn.bus, vn.dev, vn.fun, 0x04, (cmd & 0xFFFFu) | 0x1u | 0x4u | 0x400u);

  outb_((uint16_t)(vn.io + R_STATUS), 0);  /* reset */
  outb_((uint16_t)(vn.io + R_STATUS), ST_ACK);
  outb_((uint16_t)(vn.io + R_STATUS), ST_ACK | ST_DRIVER);
  feat = inl_((uint16_t)(vn.io + R_FEAT_DEV));
  outl_((uint16_t)(vn.io + R_FEAT_DRV), feat & F_MAC);
  for (i = 0; i < 6; i++)
    vn.mac[i] = (feat & F_MAC) ? inb_((uint16_t)(vn.io + R_MAC + i)) : (uint8_t)(i == 0 ? 0x52 : 0x10 + i);

  if (vq_setup(&vn.rx, 0) != 0 || vq_setup(&vn.tx, 1) != 0) {
    outb_((uint16_t)(vn.io + R_STATUS), ST_FAILED);
    lg("Rede: virtio-net sem memoria para as filas\n");
    return -1;
  }
  for (i = 0; i < vn.rx.n; i++) rx_give(i);
  vn.tx.avail[0] = AVAIL_NO_INT;
  outb_((uint16_t)(vn.io + R_STATUS), ST_ACK | ST_DRIVER | ST_DRIVER_OK);
  (void)inb_((uint16_t)(vn.io + R_ISR));
  vn.up = 1;
  vq_kick(&vn.rx, 0);

  lg("Rede: virtio-net (filas ");
  lg_u(vn.rx.n);
  lg("/");
  lg_u(vn.tx.n);
  lg(")  MAC ");
  lg_mac(vn.mac);
  lg("\n");
  net_attach_base(&vn_link);
  return 0;
}

void virtio_net_status(void) {
  if (!vn.up) {
    lg("virtio-net: nao encontrada\n");
    return;
  }
  lg("virtio-net: MAC ");
  lg_mac(vn.mac);
  lg("  filas ");
  lg_u(vn.rx.n);
  lg("/");
  lg_u(vn.tx.n);
  lg("\n  rx ");
  lg_u(vn.rx_ok);
  lg(" (erros ");
  lg_u(vn.rx_err);
  lg(")  tx ");
  lg_u(vn.tx_ok);
  lg(" (fila cheia ");
  lg_u(vn.tx_busy);
  lg(")  avisos ");
  lg_u(vn.kicks);
  lg("\n");
}
