/* xhci.c - Controlador USB 3 (xHCI): hubs USB 2, mouse e teclado USB
 * (protocolo boot) e pendrive (mass storage bulk-only, comandos SCSI).
 *
 * Teclado: no boot UEFI o firmware para de emular PS/2 quando o SODOS toma os
 * controladores, entao o teclado USB vira scancodes set 1 na mesma fila do
 * PS/2 (ps2in_inject). Shell, keymap e WIN nao mudam. Repeticao por software
 * (500 ms, depois 30/s), como o 8042 faria.
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
 * Webcam (UVC): so sob pedido (comando webcam), ver a secao "webcam".
 * A memoria e toda de identidade (fisico = virtual) abaixo de 4 GB. */
#include "kthread.h"
#include <stdint.h>
#include "xhci.h"
#include "bootinfo.h"
#include "mm.h"
#include "net.h"
#include "serial.h"
#include "ps2in.h"

#define XC_MAX   8   /* AM5: CPU, chipset e controladores extras */
#define UDEV_MAX 16
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
#define UD_KBD   6   /* so teclado; teclado + mouse no mesmo receptor fica UD_MOUSE */

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
  int dci_kb;              /* teclado: interrupt IN no anel rout */
  int kb_len;
  volatile uint8_t* krep;  /* 64 bytes: relatorio do teclado */
  uint8_t ifc[4][3];       /* classe/subclasse/protocolo das 4 primeiras interfaces (USB) */
  int nifc;
  /* Mouse em protocolo report: posicoes (em bits, depois do report ID) que o
   * HID report descriptor deu. m_rep = 0: formato boot (3-4 bytes fixos). */
  int m_rep, m_rid;
  int m_fix, m_errs, m_seen; /* erro a tratar, erros seguidos, relatorios logados */
  int m_boff, m_bn, m_xoff, m_xsz, m_yoff, m_ysz, m_woff, m_wsz;
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
/* Trava do driver: o anel de eventos tem um consumidor por vez. As entradas
 * publicas a pegam (recursiva, por tarefa); a RIT (video/visual.c) so tenta
 * (xl_try) e, se o driver esta ocupado, deixa para a proxima volta. */
static void* volatile xl_owner;
static int xl_depth;
static void xl_enter(void) {
  void* me = kt_cur_task();
  if (__atomic_load_n(&xl_owner, __ATOMIC_ACQUIRE) == me) {
    xl_depth++;
    return;
  }
  for (;;) {
    void* z = 0;
    if (__atomic_compare_exchange_n(&xl_owner, &z, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
    __builtin_ia32_pause();
  }
  xl_depth = 1;
}
static int xl_try(void) {
  void* me = kt_cur_task();
  void* z = 0;
  if (__atomic_load_n(&xl_owner, __ATOMIC_ACQUIRE) == me) {
    xl_depth++;
    return 1;
  }
  if (!__atomic_compare_exchange_n(&xl_owner, &z, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
  xl_depth = 1;
  return 1;
}
static void xl_leave(void) {
  if (--xl_depth == 0) __atomic_store_n(&xl_owner, (void*)0, __ATOMIC_RELEASE);
}
static int ps2_hint = -1;  /* ultimo ps2_ok de xhci_mouse_init; -1 = nao sabido */

/* Mouses: todos os que aparecerem valem. Um teclado gamer ou um receptor
 * costuma ter uma interface "Mouse" (macros) que nunca manda nada; com um
 * mouse so, ela tomava o lugar do mouse de verdade (PC real: X=0, Y=0). */
#define MS_MAX 4
static int msl[MS_MAX];    /* devs[] dos mouses */
static int ms_n;
static int ms_fix;         /* algum mouse com erro no interrupt IN (ver d->m_fix) */
static int ms_q[MSQ][4];   /* dx, dy, dz, botoes */
static int ms_qh, ms_qn;

static int kb = -1;        /* devs[] do teclado */
static int kb_on;          /* xhci_kbd_init rodou: o pump trabalha */
static int kb_errs;
static int kb_fix;
static uint8_t kb_prev[8]; /* ultimo relatorio boot: modificadores, 0, 6 teclas */
static uint8_t kb_rk;      /* tecla que repete (uso HID), 0 = nenhuma */
static uint32_t kb_rt;
static int kb_rn;

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
  if (phys) kt_tlb_shootdown();
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

/* ---------- teclado: uso HID -> scancode set 1 ---------- */

/* Bit 8 = prefixo E0. 0 = sem tecla. ABNT2: 0x87 (/?) = 73, 0x85 (. do
 * teclado numerico) = 7E, 0x64 (\| ao lado do Z) = 56. */
static const uint16_t hid_set1[0x8A] = {
  [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12,
  [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23, [0x0C] = 0x17, [0x0D] = 0x24,
  [0x0E] = 0x25, [0x0F] = 0x26, [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18,
  [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,
  [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D, [0x1C] = 0x15,
  [0x1D] = 0x2C,
  [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06,
  [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
  [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F, [0x2C] = 0x39,
  [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A, [0x30] = 0x1B, [0x31] = 0x2B,
  [0x32] = 0x2B, [0x33] = 0x27, [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33,
  [0x37] = 0x34, [0x38] = 0x35, [0x39] = 0x3A,
  [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F,
  [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44,
  [0x44] = 0x57, [0x45] = 0x58,
  [0x47] = 0x46,
  [0x49] = 0x152, [0x4A] = 0x147, [0x4B] = 0x149, [0x4C] = 0x153, [0x4D] = 0x14F,
  [0x4E] = 0x151, [0x4F] = 0x14D, [0x50] = 0x14B, [0x51] = 0x150, [0x52] = 0x148,
  [0x53] = 0x45, [0x54] = 0x135, [0x55] = 0x37, [0x56] = 0x4A, [0x57] = 0x4E,
  [0x58] = 0x11C, [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B,
  [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49,
  [0x62] = 0x52, [0x63] = 0x53, [0x64] = 0x56, [0x65] = 0x15D,
  [0x85] = 0x7E, [0x87] = 0x73, [0x89] = 0x7D,
};

/* Bits do byte de modificadores: LCtrl LShift LAlt LGui RCtrl RShift RAlt RGui. */
static const uint16_t hid_mod_set1[8] = { 0x1D, 0x2A, 0x38, 0x15B, 0x11D, 0x36, 0x138, 0x15C };

static void kb_code(uint16_t c, int make) {
  if (!c) return;
  if (c & 0x100) ps2in_inject(0xE0);
  ps2in_inject((uint8_t)((c & 0x7Fu) | (make ? 0u : 0x80u)));
}

static void kb_emit(uint8_t u, int make) {
  if (u == 0x46) {  /* Print Screen: E0 2A E0 37 / E0 B7 E0 AA */
    if (make) { kb_code(0x12A, 1); kb_code(0x137, 1); }
    else { kb_code(0x137, 0); kb_code(0x12A, 0); }
    return;
  }
  if (u < sizeof(hid_set1) / sizeof(hid_set1[0])) kb_code(hid_set1[u], make);
}

/* Teclado saiu ou parou: solta o que estava apertado (Shift preso etc.). */
static void kb_release_all(void) {
  int i;
  for (i = 0; i < 8; i++)
    if (kb_prev[0] & (1u << i)) kb_code(hid_mod_set1[i], 0);
  for (i = 2; i < 8; i++)
    if (kb_prev[i] > 3) kb_emit(kb_prev[i], 0);
  for (i = 0; i < 8; i++) kb_prev[i] = 0;
  kb_rk = 0;
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

/* Requisicao de controle no EP0. Os dados ficam em d->buf (ate 1024 bytes).
 * Um STALL (requisicao recusada) para o EP0; o reset deixa ele pronto de novo. */
static int ctrl_to(struct udev* d, uint8_t rtype, uint8_t req, uint16_t val, uint16_t idx,
                   uint16_t len, volatile uint8_t* b) {
  int in = (rtype & 0x80) != 0;
  uint32_t trt = len ? (in ? 3u : 2u) : 0u;
  uint64_t st;
  int rc;
  ring_put(&d->ep0, (uint32_t)rtype | ((uint32_t)req << 8) | ((uint32_t)val << 16),
           (uint32_t)idx | ((uint32_t)len << 16), 8u,
           (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
  if (len)
    ring_put(&d->ep0, (uint32_t)pa(b), (uint32_t)(pa(b) >> 32), len,
             (TRB_DATA << 10) | (in ? (1u << 16) : 0u));
  st = ring_put(&d->ep0, 0, 0, 0,
                (TRB_STATUS << 10) | (1u << 5) | ((len && in) ? 0u : (1u << 16)));
  d->x->db[d->slot] = 1;
  rc = xfer_wait(d->x, d->slot, 1, st, 500, 0);
  if (rc == CC_STALL) ep_reset(d, 1, &d->ep0);
  return rc;
}

static int ctrl(struct udev* d, uint8_t rtype, uint8_t req, uint16_t val, uint16_t idx,
                uint16_t len) {
  return ctrl_to(d, rtype, req, val, idx, len, d->buf);
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
    d->buf = d->mem + 2048;   /* 1024 bytes: descritores (mouse gamer passa de 256) */
    d->rep = d->mem + 1792;   /* 64 bytes */
    d->nifc = 0;
    d->m_rep = 0;
    d->m_rid = 0;
    d->cbw = d->mem + 1856;   /* 32 bytes */
    d->krep = d->mem + 1920;  /* 64 bytes */
    d->dci_kb = 0;
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
  for (j = 0; j < ms_n; j++)
    if (msl[j] == i) {
      int k;
      xlog("[xhci] mouse desconectado\n");
      for (k = j; k + 1 < ms_n; k++) msl[k] = msl[k + 1];
      ms_n--;
      if (!ms_n) ms_qn = 0;
      break;
    }
  if (i == kb) {
    xlog("[xhci] teclado desconectado\n");
    kb_release_all();
    kb = -1;
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

static void kb_arm(struct udev* d) {
  ring_put(&d->rout, (uint32_t)pa(d->krep), (uint32_t)(pa(d->krep) >> 32), (uint32_t)d->kb_len,
           (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));  /* IOC | ISP */
  d->x->db[d->slot] = (uint32_t)d->dci_kb;
}

/* Mouse (m_if >= 0) e/ou teclado (k_if >= 0) no mesmo dispositivo: um
 * receptor sem fio traz os dois. Um Configure Endpoint para os dois IN. */
static int hid_setup(int i, int m_if, int m_ep, int m_mps, int m_ival, int m_boot,
                     int k_if, int k_ep, int k_mps, int k_ival, int cfg) {
  struct udev* d = &devs[i];
  uint32_t add = 0;
  int top = 0, rc;
  if (m_if >= 0) {
    d->dci_in = m_ep * 2 + 1;
    add |= 1u << d->dci_in;
    top = d->dci_in;
  }
  if (k_if >= 0) {
    d->dci_kb = k_ep * 2 + 1;
    add |= 1u << d->dci_kb;
    if (d->dci_kb > top) top = d->dci_kb;
  }
  (void)cfg_begin(d, add, top);
  if (m_if >= 0)
    ep_ctx(d, d->dci_in, 7, m_mps, 0, ep_interval(d->speed, m_ival), &d->rin,
           (uint32_t)m_mps | ((uint32_t)m_mps << 16));                /* Interrupt IN */
  if (k_if >= 0)
    ep_ctx(d, d->dci_kb, 7, k_mps, 0, ep_interval(d->speed, k_ival), &d->rout,
           (uint32_t)k_mps | ((uint32_t)k_mps << 16));
  rc = cfg_run(d);
  if (rc != 0) {
    log_hex("[xhci] Configure Endpoint falhou, cc=", (uint32_t)-rc);
    return -1;
  }
  if (ctrl(d, 0x00, 9, (uint16_t)cfg, 0, 0) != 0) return -1;          /* SET_CONFIGURATION */
  /* Protocolo boot: mouse com botoes, X e Y em 3 bytes fixos (o byte 3, se
   * vier, e a roda); teclado com modificadores e 6 teclas em 8 bytes. O
   * SET_IDLE vai por ultimo: alguns dispositivos respondem STALL. Teclado com
   * idle 0: so manda relatorio quando algo muda (a repeticao e nossa). */
  if (m_if >= 0) {
    /* m_boot < 0: tem boot, mas vai em report (para ter a roda) */
    if (m_boot) (void)ctrl(d, 0x21, 0x0B, m_boot > 0 ? 0 : 1, (uint16_t)m_if, 0);
    (void)ctrl(d, 0x21, 0x0A, 0, (uint16_t)m_if, 0);
  }
  if (k_if >= 0) {
    (void)ctrl(d, 0x21, 0x0B, 0, (uint16_t)k_if, 0);
    (void)ctrl(d, 0x21, 0x0A, 0, (uint16_t)k_if, 0);
  }
  d->kind = (m_if >= 0) ? UD_MOUSE : UD_KBD;
  if (m_if >= 0) {
    d->iface = m_if;
    d->len = m_mps;
    if (ms_n < MS_MAX) msl[ms_n++] = i;
    d->m_errs = 0;
    d->m_fix = 0;
    d->m_seen = 0;
    ms_arm(d);
    xlog("[xhci] mouse USB pronto, porta ");
    xlog_hex((uint32_t)(d->port + 1));
    xlog(m_boot > 0 ? " (boot)\n" : " (report)\n");
  }
  if (k_if >= 0) {
    int j;
    d->kb_len = k_mps > 64 ? 64 : k_mps;
    kb = i;
    kb_errs = 0;
    kb_fix = 0;
    kb_rk = 0;
    for (j = 0; j < 8; j++) kb_prev[j] = 0;
    kb_arm(d);
    xlog("[xhci] teclado USB pronto, porta ");
    xlog_hex((uint32_t)(d->port + 1));
    xlog("\n");
  }
  return 0;
}

static int kb_has(const uint8_t* r, uint8_t u) {
  int i;
  for (i = 2; i < 8; i++)
    if (r[i] == u) return 1;
  return 0;
}

/* Relatorio boot do teclado: compara com o anterior e gera make/break. */
static void kb_report(const uint32_t e[4]) {
  struct udev* d = &devs[kb];
  uint32_t cc = e[2] >> 24, n;
  uint8_t cur[8];
  int i;
  if (cc != CC_OK && cc != CC_SHORT) {
    kb_fix = 1;
    return;
  }
  n = (uint32_t)d->kb_len - (e[2] & 0xFFFFFFu);
  kb_errs = 0;
  for (i = 0; i < 8; i++) cur[i] = (uint32_t)i < n ? d->krep[i] : 0;
  /* ErrorRollOver (teclas demais): as 6 posicoes vem 0x01; ignora. */
  if (n >= 3 && cur[2] != 1) {
    for (i = 0; i < 8; i++)
      if ((cur[0] ^ kb_prev[0]) & (1u << i)) kb_code(hid_mod_set1[i], (cur[0] >> i) & 1);
    for (i = 2; i < 8; i++)
      if (kb_prev[i] > 3 && !kb_has(cur, kb_prev[i])) {
        kb_emit(kb_prev[i], 0);
        if (kb_prev[i] == kb_rk) kb_rk = 0;
      }
    for (i = 2; i < 8; i++)
      if (cur[i] > 3 && !kb_has(kb_prev, cur[i])) {
        kb_emit(cur[i], 1);
        /* Caps, Num e Scroll Lock nao repetem. */
        if (cur[i] != 0x39 && cur[i] != 0x53 && cur[i] != 0x47) {
          kb_rk = cur[i];
          kb_rt = net_ms();
          kb_rn = 0;
        }
      }
    for (i = 0; i < 8; i++) kb_prev[i] = cur[i];
  }
  kb_arm(d);
}

/* HID report descriptor: acha a coleção de aplicacao Mouse (ou Pointer) e
 * onde ficam botoes, X, Y e roda no relatorio de entrada. Muitos mouses (sem
 * fio, gamer) nao tem protocolo boot e so mandam este formato. 1 = achou X e
 * Y; as posicoes vao para d->m_*. */
static int hid_mouse_layout(struct udev* d, const volatile uint8_t* p, int n) {
  static uint16_t off[256];          /* bits ja usados em cada report ID */
  uint32_t upage = 0, rsize = 0, rcount = 0, rid = 0;
  uint32_t us[16], umin = 0, umax = 0;
  int nus = 0, range = 0, depth = 0, mdepth = -1, i;
  int found_x = 0, found_y = 0, found_b = 0, mrid = -1;
  for (i = 0; i < 256; i++) off[i] = 0;
  d->m_bn = d->m_xsz = d->m_ysz = d->m_wsz = 0;
  i = 0;
  while (i < n) {
    uint8_t b = p[i];
    int sz = b & 3, type = (b >> 2) & 3, tag = b >> 4, j;
    uint32_t v = 0;
    if (b == 0xFE) {                 /* item longo: pula */
      if (i + 2 >= n) break;
      i += 3 + p[i + 1];
      continue;
    }
    if (sz == 3) sz = 4;
    if (i + 1 + sz > n) break;
    for (j = 0; j < sz; j++) v |= (uint32_t)p[i + 1 + j] << (8 * j);
    i += 1 + sz;
    if (type == 1) {                 /* global */
      if (tag == 0) upage = v;
      else if (tag == 7) rsize = v;
      else if (tag == 8) rid = v & 0xFFu;
      else if (tag == 9) rcount = v;
    } else if (type == 2) {          /* local */
      if (tag == 0 && nus < 16) us[nus++] = (sz == 4) ? v : (upage << 16) | (v & 0xFFFFu);
      else if (tag == 1) { umin = (sz == 4) ? v : (upage << 16) | v; range = 1; }
      else if (tag == 2) umax = (sz == 4) ? v : (upage << 16) | v;
    } else if (type == 0) {          /* main */
      if (tag == 10) {               /* Collection */
        /* So Mouse (01/02): um Pointer solto costuma ser tablet (absoluto). */
        if (v == 1 && mdepth < 0 && nus > 0 && us[nus - 1] == 0x00010002u)
          mdepth = depth;
        depth++;
      } else if (tag == 12) {        /* End Collection */
        if (depth > 0) depth--;
        if (mdepth >= 0 && depth <= mdepth) {
          if (found_x && found_y) break;   /* primeira coleção de mouse basta */
          mdepth = -1;
        }
      } else if (tag == 8) {         /* Input */
        uint32_t k;
        if (mdepth >= 0 && !(v & 1) && (v & 2) && rsize && rsize <= 32) {
          for (k = 0; k < rcount && k < 64u; k++) {
            uint32_t u = nus ? us[k < (uint32_t)nus ? k : (uint32_t)nus - 1u]
                             : range ? umin + k : 0;
            uint32_t bit = off[rid] + k * rsize;
            if (mrid >= 0 && (int)rid != mrid) break;
            if ((u >> 16) == 9 && !found_b) {
              found_b = 1;
              d->m_boff = (int)bit;
              d->m_bn = (int)(rcount > 8 ? 8 : rcount);
              mrid = (int)rid;
            } else if (u == 0x00010030u && !found_x && (v & 4)) {   /* X relativo */
              found_x = 1; d->m_xoff = (int)bit; d->m_xsz = (int)rsize; mrid = (int)rid;
            } else if (u == 0x00010031u && !found_y && (v & 4)) {   /* Y relativo */
              found_y = 1; d->m_yoff = (int)bit; d->m_ysz = (int)rsize; mrid = (int)rid;
            } else if (u == 0x00010038u && !d->m_wsz) {
              d->m_woff = (int)bit; d->m_wsz = (int)rsize;
            }
          }
        }
        off[rid] = (uint16_t)(off[rid] + rsize * rcount);
      }
      nus = 0;
      range = 0;
      (void)umax;
    }
  }
  if (!found_x || !found_y) return 0;
  d->m_rid = mrid > 0 ? mrid : 0;
  if (!found_b) d->m_bn = 0;
  return 1;
}

static uint32_t rep_bits(const uint8_t* r, uint32_t nbytes, int bit, int size) {
  uint32_t v = 0;
  int k;
  for (k = 0; k < size && k < 32; k++) {
    uint32_t b = (uint32_t)(bit + k);
    if ((b >> 3) < nbytes && (r[b >> 3] >> (b & 7)) & 1u) v |= 1u << k;
  }
  return v;
}

static int rep_sbits(const uint8_t* r, uint32_t nbytes, int bit, int size) {
  uint32_t v;
  if (size <= 0) return 0;
  v = rep_bits(r, nbytes, bit, size);
  if (size < 32 && (v & (1u << (size - 1)))) v |= ~0u << size;
  return (int)v;
}

/* "[xhci] <s> 05 01 09 02 ...\n" (ate max bytes) na serial / SERIAL.LOG. */
static void log_bytes(const char* s, const volatile uint8_t* p, uint32_t n, uint32_t max) {
  static const char hx[] = "0123456789ABCDEF";
  char b[4];
  uint32_t i;
  xlog(s);
  for (i = 0; i < n && i < max; i++) {
    b[0] = ' ';
    b[1] = hx[p[i] >> 4];
    b[2] = hx[p[i] & 15u];
    b[3] = 0;
    xlog(b);
  }
  if (n > max) xlog(" ...");
  xlog("\n");
}

static void ms_report(int mi, const uint32_t e[4]) {
  struct udev* d = &devs[mi];
  uint32_t cc = e[2] >> 24, n;
  if (cc != CC_OK && cc != CC_SHORT) {
    if (d->m_seen < 8) {
      d->m_seen++;
      log_hex("[xhci] mouse: erro no interrupt IN, cc=", cc);
    }
    d->m_fix = 1;
    ms_fix = 1;
    return;
  }
  n = (uint32_t)d->len - (e[2] & 0xFFFFFFu);
  d->m_errs = 0;
  if (d->m_seen < 8) {  /* os primeiros relatorios de cada mouse, para o log */
    d->m_seen++;
    xlog("[xhci] mouse porta ");
    xlog_hex((uint32_t)(d->port + 1));
    log_bytes(" relatorio:", d->rep, n, 16);
  }
  if (d->m_rep) {
    /* Protocolo report: so o report ID do mouse; os outros (teclas de
     * midia, configuracao do mouse gamer) ficam de fora. */
    uint8_t r[64];
    uint32_t k, nb = n > 64u ? 64u : n;
    const uint8_t* q0 = r;
    for (k = 0; k < nb; k++) r[k] = d->rep[k];
    if (d->m_rid) {
      if (nb < 1 || r[0] != (uint8_t)d->m_rid) {
        ms_arm(d);
        return;
      }
      q0 = r + 1;
      nb--;
    }
    {
      int dx = rep_sbits(q0, nb, d->m_xoff, d->m_xsz);
      int dy = rep_sbits(q0, nb, d->m_yoff, d->m_ysz);
      int dz = rep_sbits(q0, nb, d->m_woff, d->m_wsz);
      int b = (int)(rep_bits(q0, nb, d->m_boff, d->m_bn > 3 ? 3 : d->m_bn) & 7u);
      int* q;
      if (ms_qn < MSQ) {
        q = ms_q[(ms_qh + ms_qn) % MSQ];
        ms_qn++;
        q[0] = q[1] = q[2] = 0;
      } else {
        q = ms_q[(ms_qh + MSQ - 1) % MSQ];
      }
      q[0] += dx;
      q[1] += dy;
      q[2] += dz;
      q[3] = b;
    }
    ms_arm(d);
    return;
  }
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
  int m_if = -1, m_ep = -1, m_mps = 0, m_ival = 0, m_boot = 0, m_rlen = 0;
  int k_if = -1, k_ep = -1, k_mps = 0, k_ival = 0;
  int s_if = -1, s_in = -1, s_out = -1, s_mps = 0, s_burst = 0;
  /* Interfaces HID sem protocolo boot de mouse/teclado: podem ser um mouse
   * em protocolo report (le-se o report descriptor depois). */
  int h_if[4], h_ep[4], h_mps[4], h_ival[4], h_proto[4], h_rlen[4], nh = 0, cur_rlen = 0;
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
  if (total > 1024) total = 1024;
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
      cur_rlen = 0;
      if (cur_cls == 9 && cur_alt == 0) hub = 1;
      if (cur_alt == 0 && d->nifc < 4) {
        d->ifc[d->nifc][0] = (uint8_t)cur_cls;
        d->ifc[d->nifc][1] = (uint8_t)cur_sub;
        d->ifc[d->nifc][2] = (uint8_t)cur_proto;
        d->nifc++;
      }
      xlog("[xhci] interface classe ");
      xlog_hex((uint32_t)((cur_cls << 16) | (cur_sub << 8) | cur_proto));
      xlog("\n");
    } else if (t == 5 && l >= 7 && cur_alt == 0) {
      int addr = d->buf[k + 2];
      int attr = d->buf[k + 3] & 3;
      int mps = ((int)d->buf[k + 4] | ((int)d->buf[k + 5] << 8)) & 0x7FF;
      last_bulk = 0;
#ifdef XHCI_TEST_REPORT_MOUSE
      /* Teste no QEMU (o usb-mouse dele e boot): forca o caminho do report descriptor. */
      if (0) {
#else
      if (cur_cls == 3 && cur_sub == 1 && cur_proto == 2 && m_ep < 0 && (addr & 0x80) &&
          attr == 3) {
#endif
        /* Mouse com protocolo boot (subclasse 1, protocolo 2). */
        m_if = cur_if;
        m_boot = 1;
        m_rlen = cur_rlen;
        m_ep = addr & 0x0F;
        m_mps = mps;
        m_ival = d->buf[k + 6];
      } else if (cur_cls == 3 && !(cur_sub == 1 && cur_proto == 1) && (addr & 0x80) && attr == 3 &&
                 nh < 4) {
        h_if[nh] = cur_if;
        h_ep[nh] = addr & 0x0F;
        h_mps[nh] = mps;
        h_ival[nh] = d->buf[k + 6];
        h_proto[nh] = cur_proto;
        h_rlen[nh] = cur_rlen;
        nh++;
      } else if (cur_cls == 3 && cur_sub == 1 && cur_proto == 1 && k_ep < 0 && (addr & 0x80) &&
                 attr == 3) {
        /* Teclado com protocolo boot (subclasse 1, protocolo 1). */
        k_if = cur_if;
        k_ep = addr & 0x0F;
        k_mps = mps;
        k_ival = d->buf[k + 6];
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
    } else if (t == 0x21 && l >= 9 && d->buf[k + 6] == 0x22) {
      cur_rlen = (int)d->buf[k + 7] | ((int)d->buf[k + 8] << 8);  /* report descriptor */
    }
    k += l;
  }

  /* Mouse com protocolo boot: o relatorio boot tem so botoes, X e Y (muitos
   * mouses nao mandam a roda nele). Se o report descriptor descreve a roda,
   * usa o protocolo report, como o usbhid do Linux. Sem roda ou sem descritor
   * util, fica no boot. */
  if (m_boot > 0 && m_rlen > 0 && ctrl(d, 0x00, 9, (uint16_t)cfg, 0, 0) == 0) {
    int len = m_rlen > 1024 ? 1024 : m_rlen;
    zero(d->buf, 1024u);
    if (ctrl(d, 0x81, 6, 0x2200, (uint16_t)m_if, (uint16_t)len) == 0 &&
        hid_mouse_layout(d, d->buf, len) && d->m_wsz > 0) {
      m_boot = -1;
      d->m_rep = 1;
      xlog("[xhci] mouse boot com roda: protocolo report, id ");
      xlog_hex((uint32_t)d->m_rid);
      xlog(" roda ");
      xlog_hex((uint32_t)((d->m_woff << 8) | d->m_wsz));
      xlog("\n");
    } else {
      xlog("[xhci] mouse boot sem roda no report descriptor: fica no boot\n");
    }
  }

  /* Mouse sem protocolo boot: le o report descriptor de cada interface HID
   * candidata (precisa da configuracao ativa) e usa a primeira que descreve
   * um mouse. Sem descritor util, um protocolo 2 ainda vai no formato boot. */
  if (m_ep < 0 && ms_n < MS_MAX && nh > 0) {
    int h;
    if (ctrl(d, 0x00, 9, (uint16_t)cfg, 0, 0) == 0) {
      for (h = 0; h < nh && m_ep < 0; h++) {
        int len = h_rlen[h];
        if (len <= 0) continue;
        if (len > 1024) len = 1024;
        zero(d->buf, 1024u);
        if (ctrl(d, 0x81, 6, 0x2200, (uint16_t)h_if[h], (uint16_t)len) != 0) {
          log_hex("[xhci] report descriptor recusado, interface ", (uint32_t)h_if[h]);
          continue;
        }
        log_hex("[xhci] report descriptor, interface ", (uint32_t)h_if[h]);
        log_bytes("[xhci]  ", d->buf, (uint32_t)len, 128);
        if (hid_mouse_layout(d, d->buf, len)) {
          xlog("[xhci] mouse: id ");
          xlog_hex((uint32_t)d->m_rid);
          xlog(" botoes ");
          xlog_hex((uint32_t)((d->m_boff << 8) | d->m_bn));
          xlog(" x ");
          xlog_hex((uint32_t)((d->m_xoff << 8) | d->m_xsz));
          xlog(" y ");
          xlog_hex((uint32_t)((d->m_yoff << 8) | d->m_ysz));
          xlog("\n");
          m_if = h_if[h];
          m_ep = h_ep[h];
          m_mps = h_mps[h];
          m_ival = h_ival[h];
          m_boot = 0;
          d->m_rep = 1;
          xlog("[xhci] mouse em protocolo report\n");
        }
      }
    }
    for (h = 0; h < nh && m_ep < 0; h++)
      if (h_proto[h] == 2) {
        m_if = h_if[h];
        m_ep = h_ep[h];
        m_mps = h_mps[h];
        m_ival = h_ival[h];
        m_boot = 0;
      }
  }

  if (hub) {
    if (hub_setup(i, cfg) != 0) goto other;
    return i;
  }
  if (s_in >= 0 && s_out >= 0 && s_mps >= 8 && dk < 0) {
    if (disk_setup(i, s_if, s_in, s_out, s_mps, s_burst, cfg) != 0) goto fail;
    return i;
  }
  {
    int use_m = m_ep >= 0 && m_mps >= 3 && ms_n < MS_MAX;
    int use_k = k_ep >= 0 && k_mps >= 3 && kb < 0;
    if (use_m || use_k) {
      if (hid_setup(i, use_m ? m_if : -1, m_ep, m_mps > 64 ? 64 : m_mps, m_ival, m_boot,
                    use_k ? k_if : -1, k_ep, k_mps, k_ival, cfg) != 0)
        goto fail;
      return i;
    }
  }
  if (m_ep < 0 && k_ep < 0 && s_in < 0) xlog("[xhci] porta sem hub, mouse, teclado ou disco\n");
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
  if (type == EV_XFER) {
    int k;
    for (k = 0; k < ms_n; k++) {
      struct udev* m = &devs[msl[k]];
      if (m->x == x && (int)(e[3] >> 24) == m->slot && (int)((e[3] >> 16) & 0x1Fu) == m->dci_in) {
        ms_report(msl[k], e);
        return;
      }
    }
    if (kb >= 0 && devs[kb].x == x && (int)(e[3] >> 24) == devs[kb].slot &&
        (int)((e[3] >> 16) & 0x1Fu) == devs[kb].dci_kb)
      kb_report(e);
  }
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

static int xhci_mouse_init_u(int ps2_ok) {
  ps2_hint = ps2_ok;
  if (ms_n) return 0;
  if (busy || !xc_up(ps2_ok)) return -1;
  scan_all();
  if (!ms_n) xlog("[xhci] nenhum mouse USB\n");
  return ms_n ? 0 : -1;
}

static int xhci_mouse_poll_u(int* dx, int* dy, int* dz, int* btn) {
  static uint32_t scan_ms;
  int i;
  uint32_t e[4];
  if (busy || !xc_n) return 0;
  for (i = 0; i < xc_n; i++)
    if (xcs[i].up)
      while (evt_take(&xcs[i], e)) evt_other(&xcs[i], e);
  if (ms_fix) {
    /* Endpoint parado (STALL ou erro de transacao): reset e volta ao anel. */
    int k;
    ms_fix = 0;
    for (k = 0; k < ms_n; k++) {
      struct udev* d = &devs[msl[k]];
      if (!d->m_fix) continue;
      d->m_fix = 0;
      if (++d->m_errs > 8) {
        int j;
        xlog("[xhci] mouse com erro, desligado\n");
        if (d->kind == UD_MOUSE) d->kind = UD_OTHER;
        for (j = k; j + 1 < ms_n; j++) msl[j] = msl[j + 1];
        ms_n--;
        k--;
      } else {
        ep_reset(d, d->dci_in, &d->rin);
        ms_arm(d);
      }
    }
  }
  /* Reserva para um evento de porta perdido: a cada 500 ms procura uma
   * conexao nova (CSC ainda ligado) enquanto nao ha mouse. */
  if (!ms_n && net_has_clock() && net_ms() - scan_ms >= 500u) {
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

static int irqs_on(void) {
  uint64_t f;
  __asm__ volatile ("pushfq; popq %0" : "=r"(f));
  return (f & 0x200u) != 0;
}

/* Boot UEFI: liga os controladores (o firmware deixa de emular PS/2 quando
 * eles sao nossos) e procura o teclado. 0 = teclado pronto. */
static int xhci_kbd_init_u(void) {
  kb_on = 1;
  if (kb >= 0) return 0;
  if (busy || !xc_up(0)) return -1;
  scan_all();
  if (kb < 0) xlog("[xhci] nenhum teclado USB\n");
  return kb >= 0 ? 0 : -1;
}

int xhci_kbd_ready(void) {
  return kb >= 0;
}

/* Chamado por ps2in_pump (todo leitor de teclado passa por la): recolhe os
 * relatorios, repete a tecla segurada e, com interrupcoes ligadas, trata
 * teclado ligado depois do boot. Uma CPU por vez; nada se o driver ja esta
 * no meio de outra coisa (busy). */
static void xhci_kbd_pump_u(void) {
  static volatile int lk;
  static uint32_t scan_ms;
  uint32_t e[4];
  int i, on;
  if (!kb_on || busy || !xc_n) return;
  if (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) return;
  on = irqs_on();
  for (i = 0; i < xc_n; i++)
    if (xcs[i].up)
      while (evt_take(&xcs[i], e)) evt_other(&xcs[i], e);
  /* O ep_reset espera o comando pelo relogio (IRQ0): so com IF ligado. */
  if (kb_fix && kb >= 0 && on) {
    struct udev* d = &devs[kb];
    kb_fix = 0;
    if (++kb_errs > 8) {
      xlog("[xhci] teclado com erro, desligado\n");
      kb_release_all();
      kb = -1;
    } else {
      ep_reset(d, d->dci_kb, &d->rout);
      kb_arm(d);
    }
  }
  if (kb_rk && net_has_clock()) {
    uint32_t now = net_ms();
    if (now - kb_rt >= (kb_rn ? 33u : 500u)) {
      kb_emit(kb_rk, 1);
      kb_rt = now;
      kb_rn = 1;
    }
  }
  /* Teclado ligado depois (ou evento de porta perdido): a cada 500 ms. */
  if (on && kb < 0 && net_has_clock() && net_ms() - scan_ms >= 500u) {
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
  if (on) port_work();
  __atomic_store_n(&lk, 0, __ATOMIC_RELEASE);
}

static int xhci_disk_init_u(void) {
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

static int xhci_disk_read_u(uint32_t lba, void* buf) {
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

static int xhci_disk_write_u(uint32_t lba, const void* buf) {
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

/* ---------- webcam (UVC) ----------
 *
 * Classe de video (0E): uma interface VideoControl (0E/01) e uma
 * VideoStreaming (0E/02). A VS tem os formatos (MJPEG ou YUY2) e os tamanhos
 * na alternativa 0, e um endpoint isocrono IN em cada alternativa > 0, cada
 * uma com uma banda. Para um quadro:
 *   1. SET_CONFIGURATION e VS na alternativa 0;
 *   2. PROBE/COMMIT (SET_CUR) com formato, tamanho e intervalo; a camera
 *      devolve o tamanho maximo do quadro e quantos bytes manda por intervalo;
 *   3. Configure Endpoint (isocrono IN) e SET_INTERFACE na menor alternativa
 *      que tenha essa banda;
 *   4. um TD (Isoch TRB) por intervalo, ISO_Q na fila; cada microquadro
 *      comeca com o cabecalho UVC (tamanho, bits FID/EOF/ERR) e o resto e o
 *      quadro; os eventos vao para o interruptor 1 e a thread da camera os le
 *      (ver "webcam: stream");
 *   5. Stop Endpoint, alternativa 0 e o endpoint sai do contexto.
 * Programas: k_cam_modes, k_cam_open, k_cam_frame, k_cam_close. O comando
 * webcam usa o mesmo stream (descarta os primeiros quadros, para a exposicao
 * automatica). Hub USB 3 e camera SuperSpeed nao. */

static int put_s(char* out, int pos, int max, const char* s);
static int put_u(char* out, int pos, int max, uint32_t v);
static int put_hex2(char* out, int pos, int max, uint32_t v);
static int put_path(char* out, int pos, int max, const struct udev* d);

#define CAM_FMT_MAX 8
#define CAM_FRM_MAX 32
#define CAM_ALT_MAX 16
#define ISO_N 256              /* TRBs no anel isocrono (o ultimo e o Link) */
#define ISO_Q 200              /* TDs na fila: 25 ms com um por microquadro */
#define CEV_N 1024             /* anel de eventos do interruptor 1 (so a camera) */
#define ISO_SLOT 4096u         /* buffer de cada TD (ate 3 x 1024 bytes) */
#define TRB_ISOCH 5
#define TRB_STOP_EP 15

struct cam_frm {
  uint8_t kind, fmt, idx;
  uint16_t w, h;
  uint32_t ival, maxbuf;
};

struct cam_alt {
  uint8_t alt, ep, mult, ival;
  uint16_t mps;
};

/* Estado da captura em andamento. */
struct cam_cap {
  uint8_t* fb;
  uint32_t cap, len;
  int have, fid, bad, started, kind;
  uint32_t frames;
  uint32_t want_len;           /* YUY2: w * h * 2 */
  uint32_t pkts, errs, lost;
  uint32_t noeoi, ncc[4];      /* sem EOI, cc 15/23/4/outros */
};

/* Stream (thread da camera e quem le os quadros). */
struct cam_strm {
  volatile int run;            /* 1 = transmitindo; so a thread desliga */
  volatile int stop_req;
  volatile int seq;            /* quadros publicados (palavra do futex) */
  volatile int lk;
  volatile int wake;           /* futex da thread (sempre 0) */
  int tid;
  int ready, reading, wr;      /* buffers: pronto, sendo lido, sendo escrito */
  uint8_t* buf[3];
  uint32_t blen[3], bcap;
  uint32_t owner;              /* pid de quem ligou */
  volatile uint32_t last_read;
  int kind, w, h, dci;
  uint32_t tdlen, mult, next;
  struct ring r;
  struct xc* x;
  struct udev* d;
  struct trb* ev;              /* interruptor 1 */
  volatile uint32_t* erst;
  uint32_t ev_idx, ev_ccs;
  uint8_t* shot;               /* comando webcam: copia do quadro */
  uint32_t shot_cap;
  int want_kind;               /* k_cam_open: formato pedido (0 = MJPEG antes) */
};

static struct {
  uint32_t g0;
  int dev;                     /* devs[] da webcam, -1 = nenhuma */
  volatile uint8_t* cfg;       /* descritor de configuracao inteiro */
  int cfg_len, cfg_val;
  int vc_if, vs_if, uvc;
  int nfmt;
  uint8_t fmt_kind[CAM_FMT_MAX], fmt_idx[CAM_FMT_MAX];
  int nfrm;
  struct cam_frm frm[CAM_FRM_MAX];
  int nalt;
  struct cam_alt alt[CAM_ALT_MAX];
  struct trb* iso;             /* ISO_N TRBs */
  uint8_t* pk;                 /* ISO_N * ISO_SLOT */
  struct cam_cap c;
  struct cam_strm s;
  uint32_t g1;
} cam = { .g0 = 0x43414D30u, .dev = -1, .s = { .ready = -1, .reading = -1 }, .g1 = 0x43414D31u };

static uint32_t le16(const volatile uint8_t* b) {
  return b[0] | ((uint32_t)b[1] << 8);
}

static int cam_find(void) {
  int i, q;
  for (i = 0; i < UDEV_MAX; i++) {
    if (devs[i].kind != UD_OTHER || !devs[i].slot) continue;
    for (q = 0; q < devs[i].nifc; q++)
      if (devs[i].ifc[q][0] == 0x0E) return i;
  }
  return -1;
}

/* Le o descritor de configuracao inteiro e acha interfaces, formatos,
 * tamanhos e alternativas. 0 = ok. */
static int cam_load(struct udev* d) {
  const volatile uint8_t* b;
  int n, k, l, t, total;
  int cif = -1, calt = 0, ccls = -1, csub = -1, fkind = 0, fidx = 0;
  if (!cam.cfg) cam.cfg = (volatile uint8_t*)xmem(4096u, 64u);
  if (!cam.cfg) return -1;
  n = ctrl_to(d, 0x80, 6, 0x0200, 0, 9, cam.cfg);
  if (n != 0) {
    log_hex("[xhci] webcam: GET_DESCRIPTOR cc=", (uint32_t)n);
    return -1;
  }
  total = (int)le16(cam.cfg + 2);
  if (total < 9) total = 9;
  if (total > 4096) total = 4096;
  if (ctrl_to(d, 0x80, 6, 0x0200, 0, (uint16_t)total, cam.cfg) != 0) return -1;
  b = cam.cfg;
  n = total;
  cam.cfg_len = total;
  cam.cfg_val = b[5];
  cam.vc_if = cam.vs_if = -1;
  cam.uvc = 0x100;
  cam.nfmt = cam.nfrm = cam.nalt = 0;
  for (k = 0; k + 1 < n; k += l) {
    l = b[k];
    t = b[k + 1];
    if (l < 2 || k + l > n) break;
    if (t == 4 && l >= 9) {
      cif = b[k + 2];
      calt = b[k + 3];
      ccls = b[k + 5];
      csub = b[k + 6];
      if (ccls == 0x0E && csub == 1 && cam.vc_if < 0) cam.vc_if = cif;
      if (ccls == 0x0E && csub == 2 && cam.vs_if < 0) cam.vs_if = cif;
      continue;
    }
    if (ccls != 0x0E) continue;
    if (t == 0x24 && csub == 1 && cif == cam.vc_if && l >= 5 && b[k + 2] == 1)
      cam.uvc = (int)le16(b + k + 3);                                  /* VC_HEADER */
    if (t == 0x24 && csub == 2 && cif == cam.vs_if && calt == 0) {
      int st = b[k + 2];
      if ((st == 6 && l >= 11) || (st == 4 && l >= 27)) {               /* VS_FORMAT_* */
        fidx = b[k + 3];
        fkind = 0;
        if (st == 6) fkind = 1;
        else if (b[k + 5] == 'Y' && b[k + 6] == 'U' && b[k + 7] == 'Y' && b[k + 8] == '2') fkind = 2;
        if (fkind && cam.nfmt < CAM_FMT_MAX) {
          cam.fmt_kind[cam.nfmt] = (uint8_t)fkind;
          cam.fmt_idx[cam.nfmt] = (uint8_t)fidx;
          cam.nfmt++;
        }
      } else if ((st == 7 || st == 5) && l >= 26 && fkind &&
                 (st == 7) == (fkind == 1) && cam.nfrm < CAM_FRM_MAX) {  /* VS_FRAME_* */
        struct cam_frm* f = &cam.frm[cam.nfrm++];
        f->kind = (uint8_t)fkind;
        f->fmt = (uint8_t)fidx;
        f->idx = b[k + 3];
        f->w = (uint16_t)le16(b + k + 5);
        f->h = (uint16_t)le16(b + k + 7);
        f->maxbuf = le32(b + k + 17);
        f->ival = le32(b + k + 21);
      } else if (st == 4 || st == 6 || st == 0x10 || st == 0x0C) {
        fkind = 0;                                                     /* formato sem suporte */
      }
    }
    if (t == 5 && l >= 7 && cif == cam.vs_if && calt > 0 && (b[k + 2] & 0x80) &&
        (b[k + 3] & 3) == 1 && cam.nalt < CAM_ALT_MAX) {
      uint32_t raw = le16(b + k + 4);
      struct cam_alt* a = &cam.alt[cam.nalt++];
      a->alt = (uint8_t)calt;
      a->ep = b[k + 2];
      a->mps = (uint16_t)(raw & 0x7FFu);
      a->mult = (uint8_t)(((raw >> 11) & 3u) + 1u);
      a->ival = b[k + 6];
    }
  }
  if (cam.vs_if < 0) return -1;
  return 0;
}

/* Controlador parado (HCH, HSE ou HCE): esquece os dispositivos dele, reinicia
 * e enumera de novo. */
static void xc_recover(struct xc* x) {
  int j;
  log_hex("[xhci] controlador parado, reiniciando; USBSTS ", r32(x->op, 0x04));
  for (j = 0; j < UDEV_MAX; j++)
    if (devs[j].kind != UD_FREE && devs[j].x == x) {
      devs[j].slot = 0;          /* sem Disable Slot: o controlador nao responde */
      dev_drop(j);
    }
  x->up = 0;
  busy = 0;
  (void)xc_up(0);
  scan_all();
}

static int cam_open(void) {
  int i = cam_find();
  cam.dev = -1;
  if (i >= 0 && (r32(devs[i].x->op, 0x04) & 0x1005u)) {
    xc_recover(devs[i].x);
    i = cam_find();
  }
  if (i < 0) return -1;
  if (cam_load(&devs[i]) != 0) {
    xlog("[xhci] webcam: descritores ilegiveis\n");
    return -2;
  }
  cam.dev = i;
  return 0;
}

static int cam_describe_u(char* out, int max) {
  static const char* const kn[] = { "?", "MJPEG", "YUY2" };
  struct udev* d;
  int pos = 0, i;
  if (!xc_n) (void)xc_up(0);
  if (busy) return put_s(out, 0, max, "USB ocupado.\n");
  /* Transmitindo: os descritores ja estao lidos (e o EP0 fica em paz). */
  if (!cam.s.run && cam_open() != 0)
    return put_s(out, 0, max, "Nenhuma webcam USB (classe de video 0E).\n");
  d = &devs[cam.dev];
  pos = put_s(out, pos, max, "Webcam na porta ");
  pos = put_path(out, pos, max, d);
  pos = put_s(out, pos, max, ", UVC ");
  pos = put_u(out, pos, max, (uint32_t)(cam.uvc >> 8));
  pos = put_s(out, pos, max, ".");
  pos = put_hex2(out, pos, max, (uint32_t)cam.uvc & 0xFFu);
  pos = put_s(out, pos, max, ", interface de video ");
  pos = put_u(out, pos, max, (uint32_t)cam.vs_if);
  pos = put_s(out, pos, max, "\n");
  for (i = 0; i < cam.nfrm; i++) {
    const struct cam_frm* f = &cam.frm[i];
    pos = put_s(out, pos, max, "  ");
    pos = put_s(out, pos, max, kn[f->kind < 3 ? f->kind : 0]);
    pos = put_s(out, pos, max, " ");
    pos = put_u(out, pos, max, f->w);
    pos = put_s(out, pos, max, "x");
    pos = put_u(out, pos, max, f->h);
    if (f->ival) {
      pos = put_s(out, pos, max, "  ");
      pos = put_u(out, pos, max, (10000000u + f->ival / 2u) / f->ival);
      pos = put_s(out, pos, max, " q/s");
    }
    pos = put_s(out, pos, max, "\n");
  }
  if (!cam.nfrm) pos = put_s(out, pos, max, "  nenhum formato MJPEG ou YUY2\n");
  pos = put_s(out, pos, max, "  banda (bytes por intervalo):");
  for (i = 0; i < cam.nalt; i++) {
    pos = put_s(out, pos, max, " ");
    pos = put_u(out, pos, max, (uint32_t)cam.alt[i].mps * cam.alt[i].mult);
  }
  pos = put_s(out, pos, max, "\n");
  return pos;
}

/* ---------- webcam: stream ----------
 *
 * Os eventos dos TDs isocronos vao para o interruptor 1 (anel de eventos
 * proprio, CEV_N entradas): a thread da camera (cam_thread, tempo real numa
 * CPU que nao e a da RIT) le e repoe os TDs sem a trava do driver. A trava so
 * entra para ligar e desligar (comandos e EP0). Cada quadro inteiro vai para um
 * de 3 buffers; quem le (k_cam_frame) copia o mais novo. Sem ninguem lendo por
 * 5 s, a thread desliga a camera sozinha (programa que fechou sem k_cam_close). */

static void cs_lock(void) {
  while (__atomic_exchange_n(&cam.s.lk, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
}

static void cs_unlock(void) {
  __atomic_store_n(&cam.s.lk, 0, __ATOMIC_RELEASE);
}

/* Quadro inteiro em c->fb: passa a ser o "pronto" e a escrita vai para um
 * buffer que ninguem esta lendo. */
static void cam_publish(struct cam_cap* c) {
  int i;
  cs_lock();
  cam.s.blen[cam.s.wr] = c->len;
  cam.s.ready = cam.s.wr;
  for (i = 0; i < 3; i++)
    if (i != cam.s.ready && i != cam.s.reading) break;
  cam.s.wr = i;
  cam.s.seq++;
  cs_unlock();
  c->fb = cam.s.buf[i];
  (void)kt_futex_wake(&cam.s.seq, 64);
}

static void cam_end(struct cam_cap* c) {
  if (c->started && !c->bad && c->len > 0) {
    int ok;
    if (c->kind == 1) {
      /* SOI no comeco e EOI no fim (algumas cameras completam com zeros):
       * sem o EOI o quadro veio cortado. */
      uint32_t k, lo = c->len > 64u ? c->len - 64u : 2u;
      ok = 0;
      if (c->len > 4 && c->fb[0] == 0xFF && c->fb[1] == 0xD8)
        for (k = c->len - 1; k > lo; k--)
          if (c->fb[k] == 0xD9 && c->fb[k - 1] == 0xFF) {
            c->len = k + 1;
            ok = 1;
            break;
          }
      if (!ok) c->noeoi++;
    } else {
      ok = c->len == c->want_len;
    }
    if (ok) {
      c->frames++;
      cam_publish(c);
    }
  }
  c->started = 1;
  c->len = 0;
  c->bad = 0;
}

/* Um microquadro: cabecalho UVC (um por payload transfer, mesmo com 2 ou 3
 * transacoes de alta banda, como no uvcvideo do Linux) e um pedaco do quadro. */
static void cam_packet(struct cam_cap* c, const volatile uint8_t* p, uint32_t n) {
  uint32_t hl, bfh, i;
  if (n < 2) return;
  hl = p[0];
  bfh = p[1];
  if (hl < 2 || hl > n) return;
  c->pkts++;
  if (c->have && (int)(bfh & 1u) != c->fid) cam_end(c);  /* quadro novo sem EOF */
  c->have = 1;
  c->fid = (int)(bfh & 1u);
  if (bfh & 0x40u) {
    c->bad = 1;
    c->errs++;
  }
  if (c->len + (n - hl) > c->cap) c->bad = 1;
  else
    for (i = hl; i < n; i++) c->fb[c->len++] = p[i];
  if (bfh & 2u) cam_end(c);
}

static void iso_put(struct ring* r, uint64_t buf, uint32_t len, uint32_t tlbpc) {
  struct trb* t = &r->t[r->idx];
  t->d[0] = (uint32_t)buf;
  t->d[1] = (uint32_t)(buf >> 32);
  t->d[2] = len | (1u << 22);                     /* interruptor 1 */
  __asm__ volatile ("" : : : "memory");
  t->d[3] = (TRB_ISOCH << 10) | (1u << 5) | (1u << 2) | (tlbpc << 16) | (1u << 31) | r->pcs;
  if (++r->idx == ISO_N - 1) {
    struct trb* l = &r->t[ISO_N - 1];
    l->d[3] = (TRB_LINK << 10) | 2u | r->pcs;
    r->idx = 0;
    r->pcs ^= 1u;
  }
}

static void cam_td(void) {
  struct ring* r = &cam.s.r;
  iso_put(r, pa(cam.pk + (uint32_t)r->idx * ISO_SLOT), cam.s.tdlen, cam.s.mult - 1u);
}

/* Interruptor 1: anel de eventos so da camera, sem interrupcao (polling). */
static volatile uint8_t* cam_ir(void) {
  return cam.s.x->rt + 0x20 + 32;
}

static int ev1_take(uint32_t e[4]) {
  struct trb* t = &cam.s.ev[cam.s.ev_idx];
  if ((t->d[3] & 1u) != cam.s.ev_ccs) return 0;
  __asm__ volatile ("" : : : "memory");
  e[0] = t->d[0];
  e[1] = t->d[1];
  e[2] = t->d[2];
  e[3] = t->d[3];
  if (++cam.s.ev_idx == CEV_N) {
    cam.s.ev_idx = 0;
    cam.s.ev_ccs ^= 1u;
  }
  w64(cam_ir(), 0x18, pa(&cam.s.ev[cam.s.ev_idx]) | 8u);  /* ERDP, limpa EHB */
  return 1;
}

static int cam_ir_setup(struct xc* x) {
  uint32_t maxi = (r32(x->cap, 0x04) >> 8) & 0x7FFu;
  volatile uint8_t* ir;
  if (maxi < 2) return -1;
  if (!cam.s.ev) {
    uint8_t* raw = (uint8_t*)umalloc_owned(CEV_N * 16u * 2u, 0);
    if (!raw) return -2;
    cam.s.ev = (struct trb*)(uintptr_t)((pa(raw) + CEV_N * 16u - 1u) & ~(uint64_t)(CEV_N * 16u - 1u));
  }
  if (!cam.s.erst) cam.s.erst = (volatile uint32_t*)xmem(64u, 64u);
  if (!cam.s.erst) return -2;
  zero(cam.s.ev, CEV_N * 16u);
  cam.s.ev_idx = 0;
  cam.s.ev_ccs = 1;
  cam.s.erst[0] = (uint32_t)pa(cam.s.ev);
  cam.s.erst[1] = (uint32_t)(pa(cam.s.ev) >> 32);
  cam.s.erst[2] = CEV_N;
  cam.s.erst[3] = 0;
  cam.s.x = x;
  ir = cam_ir();
  w32(ir, 0x00, 1u);                         /* IMAN: limpa IP, IE = 0 */
  w32(ir, 0x04, 0);
  w32(ir, 0x08, 1u);                         /* ERSTSZ */
  w64(ir, 0x18, pa(cam.s.ev));               /* ERDP */
  w64(ir, 0x10, pa(cam.s.erst));             /* ERSTBA por ultimo */
  return 0;
}

/* Escolhe o tamanho: o maior ate want_w de largura (0 = ate 640), ou o
 * menor de todos. MJPEG antes de YUY2. Com a altura nos 16 bits de cima,
 * so o tamanho exato. */
static int cam_pick(int want_w) {
  int kind, i, best;
  uint32_t lim = want_w > 0 ? (uint32_t)want_w & 0xFFFFu : 640u;
  uint32_t eh = want_w > 0 ? (uint32_t)want_w >> 16 : 0u;
  if (eh) {
    for (kind = 1; kind <= 2; kind++)
      for (i = 0; i < cam.nfrm; i++)
        if (cam.frm[i].kind == kind && cam.frm[i].w == lim && cam.frm[i].h == eh &&
            (!cam.s.want_kind || cam.s.want_kind == kind))
          return i;
    return -1;
  }
  for (kind = 1; kind <= 2; kind++) {
    best = -1;
    for (i = 0; i < cam.nfrm; i++)
      if (cam.frm[i].kind == kind && cam.frm[i].w <= lim &&
          (best < 0 || cam.frm[i].w > cam.frm[best].w))
        best = i;
    if (best >= 0) return best;
  }
  best = -1;
  for (i = 0; i < cam.nfrm; i++)
    if (best < 0 || cam.frm[i].w < cam.frm[best].w ||
        (cam.frm[i].w == cam.frm[best].w && cam.frm[i].kind < cam.frm[best].kind))
      best = i;
  return best;
}

static int cam_fail(char* err, int max, const char* s) {
  if (err && max > 0) (void)put_s(err, 0, max, s);
  xlog("[xhci] webcam: ");
  xlog(s);
  xlog("\n");
  return -1;
}

/* Desliga a camera (com a trava): Stop Endpoint, alternativa 0, endpoint fora
 * do contexto. Quem espera quadro acorda e ve run = 0. */
static void cam_hw_stop(void) {
  struct udev* d = cam.s.d;
  struct xc* x = cam.s.x;
  struct cam_cap* c = &cam.c;
  int dci = cam.s.dci;
  uint32_t e[4];
  busy = 1;
  if (!(r32(x->op, 0x04) & 0x1005u)) {
    (void)cmd_run(x, 0, 0, (TRB_STOP_EP << 10) | ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24), 0);
    (void)ctrl(d, 0x01, 11, 0, (uint16_t)cam.vs_if, 0);
    (void)cfg_begin(d, 0, 0);
    ictx_at(d, 0)[0] = 1u << dci;                                     /* Drop */
    (void)cfg_run(d);
    while (evt_take(x, e)) evt_other(x, e);
  }
  busy = 0;
  cam.s.run = 0;
  cam.s.stop_req = 0;
  xlog("[xhci] webcam: parou; quadros ");
  xlog_hex(c->frames);
  xlog(" pacotes ");
  xlog_hex(c->pkts);
  xlog(" erros ");
  xlog_hex(c->errs);
  xlog(" perdidos ");
  xlog_hex(c->lost);
  log_hex(" sem EOI ", c->noeoi);
  cam.s.seq++;
  (void)kt_futex_wake(&cam.s.seq, 64);
}

/* Uma volta da thread: eventos do interruptor 1, TDs repostos. */
__attribute__((noinline)) static void cam_step(void) {
  struct cam_cap* c = &cam.c;
  struct xc* x = cam.s.x;
  uint32_t e[4];
  int ring = 0;
  if (r32(x->op, 0x04) & 0x1005u) {
    log_hex("[xhci] webcam: controlador parou, USBSTS ", r32(x->op, 0x04));
    xl_enter();
    cam_hw_stop();
    xl_leave();
    return;
  }
  if (cam.s.stop_req || net_ms() - cam.s.last_read > 5000u) {
    if (!cam.s.stop_req) xlog("[xhci] webcam: ninguem lendo ha 5 s, desligando\n");
    xl_enter();
    cam_hw_stop();
    xl_leave();
    return;
  }
  while (ev1_take(e)) {
    uint64_t p;
    uint32_t code, s, nd;
    if (((e[3] >> 10) & 0x3Fu) != EV_XFER || (int)(e[3] >> 24) != cam.s.d->slot ||
        (int)((e[3] >> 16) & 0x1Fu) != cam.s.dci)
      continue;
    p = e[0] | ((uint64_t)e[1] << 32);
    code = e[2] >> 24;
    if (code != CC_OK && code != CC_SHORT)
      c->ncc[code == 15 ? 0 : code == 23 ? 1 : code == 4 ? 2 : 3]++;
    if (p < pa(cam.iso) || p >= pa(cam.iso) + (ISO_N - 1) * 16u) {
      c->bad = 1;                                 /* Missed Service etc. sem TD */
      c->lost++;
      if (code == 14 || code == 15) ring = 1;
      continue;
    }
    s = (uint32_t)((p - pa(cam.iso)) / 16u);
    nd = (s + (ISO_N - 1) - cam.s.next) % (ISO_N - 1) + 1;  /* TDs que terminaram */
    if (nd > 1) {
      c->bad = 1;
      c->lost += nd - 1;
    }
    cam.s.next = (s + 1) % (ISO_N - 1);
    if (code == CC_OK || code == CC_SHORT) {
      uint32_t got = cam.s.tdlen - (e[2] & 0xFFFFFFu);
      if (got > cam.s.tdlen) got = 0;
      cam_packet(c, cam.pk + s * ISO_SLOT, got);
    } else {
      c->bad = 1;
      c->lost++;
    }
    while (nd--) cam_td();
    ring = 1;
  }
  if (ring) x->db[cam.s.d->slot] = (uint32_t)cam.s.dci;
}

static int cam_thread(void* arg) {
  (void)arg;
  for (;;) {
    if (!cam.s.run) {
      (void)kt_futex_wait(&cam.s.wake, 0, 200);
      continue;
    }
    cam_step();
    (void)kt_futex_wait(&cam.s.wake, 0, 2);
  }
  return 0;
}

/* Stream ligado sem a thread (morreu): desliga a camera daqui. Sem trava. */
static void cam_check_thread(void) {
  if (!cam.s.run || cam.s.tid <= 0 || kt_thread_alive(cam.s.tid)) return;
  xlog("[xhci] webcam: a thread da camera morreu; desligando\n");
  xl_enter();
  if (cam.s.run) cam_hw_stop();
  xl_leave();
  cam.s.tid = 0;
}

/* Liga a camera e o stream (com a trava). want_w como em cam_pick. */
static int cam_start_u(int want_w, int* kind, int* w, int* h, uint32_t* maxlen, char* err,
                       int emax) {
  struct udev* d;
  struct xc* x;
  const struct cam_frm* f;
  const struct cam_alt* a = 0;
  volatile uint32_t* c;
  struct cam_cap* cc = &cam.c;
  uint32_t plen, maxframe, payload, need, pid = kt_cur_pid();
  int fi, i, dci, rc, out;
  cam_check_thread();
  if (cam.s.run) {
    if (cam.s.owner != pid) return cam_fail(err, emax, "webcam em uso por outro programa") - 1;
    *kind = cam.s.kind;
    *w = cam.s.w;
    *h = cam.s.h;
    *maxlen = cam.s.bcap;
    cam.s.last_read = net_ms();
    return 0;
  }
  if (!xc_n) (void)xc_up(0);
  if (busy) return cam_fail(err, emax, "USB ocupado");
  if (cam_open() != 0) return cam_fail(err, emax, "nenhuma webcam USB");
  d = &devs[cam.dev];
  x = d->x;
  if (d->speed != 3 && d->speed != 1) return cam_fail(err, emax, "so webcam USB 2 (HS ou FS)");
  fi = cam_pick(want_w);
  if (fi < 0)
    return cam_fail(err, emax, ((uint32_t)want_w >> 16) ? "a webcam nao tem esse tamanho (webcam /L)"
                                                         : "a webcam nao tem MJPEG nem YUY2");
  f = &cam.frm[fi];
  if (cam_ir_setup(x) != 0) return cam_fail(err, emax, "controlador sem interruptor secundario");
  busy = 1;

  if (ctrl(d, 0x00, 9, (uint16_t)cam.cfg_val, 0, 0) != 0) {          /* SET_CONFIGURATION */
    busy = 0;
    return cam_fail(err, emax, "SET_CONFIGURATION recusado");
  }
  (void)ctrl(d, 0x01, 11, 0, (uint16_t)cam.vs_if, 0);                 /* SET_INTERFACE alt 0 */

  /* PROBE: formato, tamanho e intervalo; a camera completa o resto. */
  plen = cam.uvc >= 0x150 ? 48u : cam.uvc >= 0x110 ? 34u : 26u;
  for (;;) {
    zero(d->buf, 64u);
    d->buf[0] = 1;                                                    /* bmHint: intervalo */
    d->buf[2] = f->fmt;
    d->buf[3] = f->idx;
    put_le32(d->buf + 4, f->ival);
    rc = ctrl(d, 0x21, 0x01, 0x0100, (uint16_t)cam.vs_if, (uint16_t)plen);
    if (rc == 0 || plen == 26u) break;
    plen = plen == 48u ? 34u : 26u;
  }
  if (rc != 0 || ctrl(d, 0xA1, 0x81, 0x0100, (uint16_t)cam.vs_if, (uint16_t)plen) != 0) {
    busy = 0;
    return cam_fail(err, emax, "PROBE recusado");
  }
  maxframe = le32(d->buf + 18);
  payload = le32(d->buf + 22);
  if (ctrl(d, 0x21, 0x01, 0x0200, (uint16_t)cam.vs_if, (uint16_t)plen) != 0) {  /* COMMIT */
    busy = 0;
    return cam_fail(err, emax, "COMMIT recusado");
  }

  /* Menor alternativa com a banda pedida (ou a maior que houver). */
  for (i = 0; i < cam.nalt; i++) {
    uint32_t bw = (uint32_t)cam.alt[i].mps * cam.alt[i].mult;
    if (bw >= payload && (!a || bw < (uint32_t)a->mps * a->mult)) a = &cam.alt[i];
  }
  if (!a)
    for (i = 0; i < cam.nalt; i++)
      if (!a || (uint32_t)cam.alt[i].mps * cam.alt[i].mult > (uint32_t)a->mps * a->mult)
        a = &cam.alt[i];
  if (!a) {
    busy = 0;
    return cam_fail(err, emax, "sem endpoint isocrono");
  }
  dci = (a->ep & 0x0F) * 2 + 1;

  /* Buffers: anel, pacotes e 3 quadros (o kmalloc nao da blocos grandes:
   * vem do umalloc, que e memoria em identidade). */
  need = maxframe;
  if (need < f->maxbuf) need = f->maxbuf;
  if (need < (uint32_t)f->w * f->h * 2u) need = (uint32_t)f->w * f->h * 2u;
  need += 65536u;
  if (!cam.iso) cam.iso = (struct trb*)xmem(ISO_N * 16u, 4096u);
  if (!cam.pk) {
    uint8_t* raw = (uint8_t*)umalloc_owned(ISO_N * ISO_SLOT + 4096u, 0);
    if (raw) cam.pk = (uint8_t*)(uintptr_t)((pa(raw) + 4095u) & ~4095ull);
  }
  if (cam.s.bcap < need) {
    for (i = 0; i < 3; i++) {
      if (cam.s.buf[i]) ufree(cam.s.buf[i]);
      cam.s.buf[i] = (uint8_t*)umalloc_owned(need, 0);
    }
    cam.s.bcap = need;
    for (i = 0; i < 3; i++)
      if (!cam.s.buf[i]) cam.s.bcap = 0;
  }
  if (!cam.iso || !cam.pk || !cam.s.bcap) {
    busy = 0;
    return cam_fail(err, emax, "sem memoria");
  }
  zero(cam.iso, ISO_N * 16u);
  cam.s.r.t = cam.iso;
  cam.s.r.idx = 0;
  cam.s.r.pcs = 1;
  cam.iso[ISO_N - 1].d[0] = (uint32_t)pa(cam.iso);
  cam.iso[ISO_N - 1].d[1] = (uint32_t)(pa(cam.iso) >> 32);
  cam.iso[ISO_N - 1].d[3] = (TRB_LINK << 10) | 2u;

  /* Configure Endpoint: isocrono IN (CErr 0, Max Burst = transacoes extras). */
  (void)cfg_begin(d, 1u << dci, dci);
  c = ictx_at(d, dci + 1);
  c[0] = (d->speed == 3 ? (uint32_t)(a->ival ? a->ival - 1 : 0) : (uint32_t)(a->ival ? a->ival + 2 : 3)) << 16;
  c[1] = (5u << 3) | ((uint32_t)(a->mult - 1) << 8) | ((uint32_t)a->mps << 16);
  c[2] = (uint32_t)pa(cam.iso) | 1u;
  c[3] = (uint32_t)(pa(cam.iso) >> 32);
  c[4] = ((uint32_t)a->mps * a->mult) | (((uint32_t)a->mps * a->mult) << 16);
  rc = cfg_run(d);
  if (rc != 0) {
    log_hex("[xhci] webcam: Configure Endpoint cc=", (uint32_t)-rc);
    busy = 0;
    return cam_fail(err, emax, "Configure Endpoint recusado (banda?)");
  }
  if (ctrl(d, 0x01, 11, a->alt, (uint16_t)cam.vs_if, 0) != 0) {      /* SET_INTERFACE */
    (void)cfg_begin(d, 0, 0);
    ictx_at(d, 0)[0] = 1u << dci;
    (void)cfg_run(d);
    busy = 0;
    return cam_fail(err, emax, "SET_INTERFACE recusado");
  }

  cs_lock();
  cam.s.ready = -1;
  cam.s.reading = -1;
  cam.s.wr = 0;
  cs_unlock();
  cc->fb = cam.s.buf[0];
  cc->cap = cam.s.bcap;
  cc->len = 0;
  cc->have = cc->fid = cc->bad = cc->started = cc->frames = 0;
  cc->kind = f->kind;
  cc->want_len = (uint32_t)f->w * f->h * 2u;
  cc->pkts = cc->errs = cc->lost = cc->noeoi = 0;
  cc->ncc[0] = cc->ncc[1] = cc->ncc[2] = cc->ncc[3] = 0;
  cam.s.d = d;
  cam.s.dci = dci;
  cam.s.tdlen = (uint32_t)a->mps * a->mult;
  cam.s.mult = a->mult;
  cam.s.next = 0;
  cam.s.kind = f->kind;
  cam.s.w = f->w;
  cam.s.h = f->h;
  cam.s.owner = pid;
  cam.s.last_read = net_ms();
  cam.s.stop_req = 0;
  for (out = 0; out < ISO_Q; out++) cam_td();
  x->db[d->slot] = (uint32_t)dci;
  cam.s.run = 1;
  busy = 0;

  xlog("[xhci] webcam: stream ");
  xlog_hex(((uint32_t)f->w << 16) | f->h);
  xlog(f->kind == 1 ? " MJPEG" : " YUY2");
  xlog(" alt ");
  xlog_hex(a->alt);
  xlog(" banda ");
  log_hex("", cam.s.tdlen);

  if (cam.s.tid <= 0 || !kt_thread_alive(cam.s.tid)) {
    int n = kt_online_count();
    int cpu = n >= 3 ? n - 2 : n - 1;
    cam.s.tid = kt_thread_create_rt(cam_thread, 0, 16384, cpu < 0 ? 0 : cpu, 0);
    if (cam.s.tid <= 0) {
      xlog("[xhci] webcam: thread nao criada\n");
      cam_hw_stop();
      return cam_fail(err, emax, "thread da camera nao criada");
    }
    log_hex("[xhci] webcam: thread na CPU ", (uint32_t)cpu);
  }
  (void)kt_futex_wake(&cam.s.wake, 1);
  *kind = f->kind;
  *w = f->w;
  *h = f->h;
  *maxlen = cam.s.bcap;
  return 0;
}

/* Copia o quadro mais novo depois de *seq. Sem a trava do driver. */
static int cam_frame_u(void* buf, int max, int* seq, int timeout_ms) {
  uint32_t t0 = net_ms(), n, i;
  int r;
  uint8_t* o = (uint8_t*)buf;
  const uint8_t* src;
  if (!buf || !seq) return -1;
  for (;;) {
    int cur = cam.s.seq;
    uint32_t el;
    cam_check_thread();
    if (!cam.s.run) return -1;
    cam.s.last_read = net_ms();
    if (cur != *seq && cam.s.ready >= 0) break;
    el = net_ms() - t0;
    if (timeout_ms <= 0 || el >= (uint32_t)timeout_ms) return 0;
    (void)kt_futex_wait(&cam.s.seq, cur, (int)((uint32_t)timeout_ms - el));
  }
  cs_lock();
  r = cam.s.ready;
  if (r < 0) {
    cs_unlock();
    return 0;
  }
  cam.s.reading = r;
  n = cam.s.blen[r];
  *seq = cam.s.seq;
  cs_unlock();
  src = cam.s.buf[r];
  if (n > (uint32_t)max) n = (uint32_t)max;
  for (i = 0; i + 8 <= n; i += 8) *(uint64_t*)(o + i) = *(const uint64_t*)(src + i);
  for (; i < n; i++) o[i] = src[i];
  cs_lock();
  cam.s.reading = -1;
  cs_unlock();
  return (int)n;
}

/* Pede para a thread desligar e espera (ate 1,5 s). */
static int cam_close_u(void) {
  uint32_t t0 = net_ms();
  cam_check_thread();
  if (!cam.s.run) return 0;
  cam.s.stop_req = 1;
  (void)kt_futex_wake(&cam.s.wake, 1);
  while (cam.s.run && net_ms() - t0 < 1500u) {
    int cur = cam.s.seq;
    (void)kt_futex_wait(&cam.s.seq, cur, 10);
  }
  return cam.s.run ? -1 : 0;
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

static int xhci_describe_u(char* out, int max) {
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
        pos = put_s(out, pos, max, j == kb ? "mouse + teclado" : "mouse");
        if (d->m_rep) pos = put_s(out, pos, max, " (report)");
      } else if (d->kind == UD_KBD) {
        pos = put_s(out, pos, max, "teclado");
      } else if (d->kind == UD_DISK) {
        pos = put_s(out, pos, max, "pendrive ");
        pos = put_s(out, pos, max, dk_name);
        pos = put_s(out, pos, max, ", ");
        pos = put_u(out, pos, max, dk_blocks / 2048u);
        pos = put_s(out, pos, max, " MB");
      } else if (d->nifc && d->ifc[0][0] == 0x0E) {
        pos = put_s(out, pos, max, "webcam (comando webcam)");
      } else {
        int q;
        pos = put_s(out, pos, max, "outro (sem driver:");
        /* classe/subclasse/protocolo de cada interface: 03/01/02 = mouse
         * boot, 03/00/00 = HID report, 08 = disco, 09 = hub, E0 = Bluetooth */
        for (q = 0; q < d->nifc; q++) {
          pos = put_s(out, pos, max, " ");
          pos = put_hex2(out, pos, max, d->ifc[q][0]);
          pos = put_s(out, pos, max, "/");
          pos = put_hex2(out, pos, max, d->ifc[q][1]);
          pos = put_s(out, pos, max, "/");
          pos = put_hex2(out, pos, max, d->ifc[q][2]);
        }
        pos = put_s(out, pos, max, ")");
      }
      pos = put_s(out, pos, max, "\n");
    }
  }
  return pos;
}

/* ---------- entradas publicas (com a trava do driver) ---------- */
int xhci_mouse_init(int ps2_ok) {
  int r;
  xl_enter();
  r = xhci_mouse_init_u(ps2_ok);
  xl_leave();
  return r;
}
int xhci_mouse_poll(int* dx, int* dy, int* dz, int* btn) {
  int r;
  xl_enter();
  r = xhci_mouse_poll_u(dx, dy, dz, btn);
  xl_leave();
  return r;
}
int xhci_kbd_init(void) {
  int r;
  xl_enter();
  r = xhci_kbd_init_u();
  xl_leave();
  return r;
}
/* Sem esperar a trava: o pump tambem roda na IRQ0 (gui_preempt_tick ->
 * ps2in_pending) e na CPU de quem segura a trava giraria para sempre. Se a
 * RIT ou um disco esta no driver, os eventos ficam com eles. */
void xhci_kbd_pump(void) {
  if (!xl_try()) return;
  xhci_kbd_pump_u();
  xl_leave();
}
int xhci_disk_init(void) {
  int r;
  xl_enter();
  r = xhci_disk_init_u();
  xl_leave();
  return r;
}
int xhci_disk_read(uint32_t lba, void* buf) {
  int r;
  xl_enter();
  r = xhci_disk_read_u(lba, buf);
  xl_leave();
  return r;
}
int xhci_disk_write(uint32_t lba, const void* buf) {
  int r;
  xl_enter();
  r = xhci_disk_write_u(lba, buf);
  xl_leave();
  return r;
}
int xhci_describe(char* out, int max) {
  int r;
  xl_enter();
  r = xhci_describe_u(out, max);
  xl_leave();
  return r;
}
int xhci_cam_describe(char* out, int max) {
  int r;
  if (!out || max < 2) return 0;
  out[0] = 0;
  xl_enter();
  r = cam_describe_u(out, max);
  xl_leave();
  return r;
}
int xhci_cam_grab(int want_w, int skip, const uint8_t** data, uint32_t* len, int* kind, int* w,
                  int* h, char* err, int emax) {
  int r;
  static char dummy[64];
  if (!err || emax < 2) {
    err = dummy;
    emax = (int)sizeof(dummy);
  }
  err[0] = 0;
  {
    uint32_t maxlen = 0;
    int seq, n = 0, i, was = cam.s.run;
    xl_enter();
    r = cam_start_u(want_w, kind, w, h, &maxlen, err, emax);
    xl_leave();
    if (r != 0) {
      if (r == -2 && cam.s.run) {
        /* Outro programa transmitindo: tira a foto do stream dele. */
        err[0] = 0;
        *kind = cam.s.kind;
        *w = cam.s.w;
        *h = cam.s.h;
        maxlen = cam.s.bcap;
        was = 1;
        skip = 0;
      } else {
        return r;
      }
    }
    if (cam.s.shot_cap < maxlen) {
      if (cam.s.shot) ufree(cam.s.shot);
      cam.s.shot = (uint8_t*)umalloc_owned(maxlen, 0);
      cam.s.shot_cap = cam.s.shot ? maxlen : 0;
    }
    if (!cam.s.shot) {
      if (!was) (void)cam_close_u();
      (void)put_s(err, 0, emax, "sem memoria");
      return -1;
    }
    seq = cam.s.seq;
    for (i = 0; i <= skip; i++) {
      n = cam_frame_u(cam.s.shot, (int)cam.s.shot_cap, &seq, 3000);
      if (n <= 0) break;
    }
    if (!was) (void)cam_close_u();
    if (n <= 0) {
      (void)put_s(err, 0, emax, n == 0 ? "nenhum quadro inteiro em 3 s" : "a webcam parou");
      return -1;
    }
    *data = cam.s.shot;
    *len = (uint32_t)n;
    return 0;
  }
}

/* Para programas (k_cam_*, kexports.c). */
/* Ponteiro de programa: no heap alto (programas, DLLs, pilhas) ou acima de
 * 4 GB. Abaixo de um_start() fica o kernel (kts[], world, cpus...): tres
 * quedas de 26/11 tinham listas de modos da camera escritas ali. Recusa e
 * registra quem chamou, em vez de corromper o kernel. */
static int cam_user_ptr(const void* p, uint32_t len, const char* what) {
  uint64_t a = (uint64_t)(uintptr_t)p;
  if (a >= um_start() && a + len >= a) return 1;
  xlog("[xhci] webcam: ponteiro no kernel em ");
  xlog(what);
  xlog(" ");
  xlog_hex((uint32_t)(a >> 32));
  xlog_hex((uint32_t)a);
  log_hex(" pid ", kt_cur_pid());
  return 0;
}

int xhci_cam_modes(uint32_t* out, int max) {
  if (max > 0 && !cam_user_ptr(out, (uint32_t)max * 16u, "k_cam_modes")) return -1;
  xlog("[xhci] webcam: k_cam_modes pid ");
  xlog_hex(kt_cur_pid());
  xlog(" buffer ");
  xlog_hex((uint32_t)((uint64_t)(uintptr_t)out >> 32));
  log_hex("", (uint32_t)(uintptr_t)out);
  int i, n = 0;
  if (!out || max < 0) return -1;
  xl_enter();
  if (!xc_n) (void)xc_up(0);
  if (!cam.s.run && (busy || cam_open() != 0)) {
    xl_leave();
    return -1;
  }
  for (i = 0; i < cam.nfrm && n < max; i++, n++) {
    out[n * 4 + 0] = cam.frm[i].kind;
    out[n * 4 + 1] = cam.frm[i].w;
    out[n * 4 + 2] = cam.frm[i].h;
    out[n * 4 + 3] = cam.frm[i].ival;
  }
  xl_leave();
  return n;
}

int xhci_cam_open(int kind, int w, int h, int* maxlen) {
  int k = 0, ww = 0, hh = 0, r;
  uint32_t ml = 0;
  char err[64];
  int want = w | (h << 16);
  if (maxlen && !cam_user_ptr(maxlen, 4, "k_cam_open")) return -1;
  xl_enter();
  cam.s.want_kind = kind;          /* cam_pick: so esse formato no tamanho exato */
  r = cam_start_u(want, &k, &ww, &hh, &ml, err, (int)sizeof(err));
  cam.s.want_kind = 0;
  xl_leave();
  if (r == 0 && ((kind && k != kind) || ((w || h) && (ww != w || hh != h)))) {
    if (!cam.s.run || cam.s.owner == kt_cur_pid()) (void)cam_close_u();
    return -3;
  }
  if (maxlen) *maxlen = (int)ml;
  return r;
}

int xhci_cam_frame(void* buf, int max, int* seq, int timeout_ms) {
  if (!cam_user_ptr(buf, max > 0 ? (uint32_t)max : 0, "k_cam_frame") || !cam_user_ptr(seq, 4, "k_cam_frame seq"))
    return -1;
  return cam_frame_u(buf, max, seq, timeout_ms);
}

int xhci_cam_close(void) {
  if (cam.s.run && cam.s.owner != kt_cur_pid()) return -2;
  return cam_close_u();
}

/* ---------- RIT (video/visual.c) ---------- */

/* Sem esperar nada: recolhe os eventos (teclado vai para a fila do PS/2,
 * mouse para ms_q), repete a tecla segurada e devolve um relatorio do mouse.
 * 0 = nada (ou o driver esta ocupado: a RIT tenta de novo na proxima volta).
 * Reset de endpoint, enumeracao e portas ficam com o mundo
 * (xhci_mouse_maint / xhci_kbd_pump), que podem esperar comandos. */
int xhci_rit_poll(int* dx, int* dy, int* dz, int* btn) {
  uint64_t f;
  uint32_t e[4];
  int i, got = 0;
  if (!xc_n) return 0;
  __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
  if (!xl_try()) goto out;
  if (busy) {
    xl_leave();
    goto out;
  }
  for (i = 0; i < xc_n; i++)
    if (xcs[i].up)
      while (evt_take(&xcs[i], e)) evt_other(&xcs[i], e);
  if (kb_on && kb_rk && net_has_clock()) {
    uint32_t now = net_ms();
    if (now - kb_rt >= (kb_rn ? 33u : 500u)) {
      kb_emit(kb_rk, 1);
      kb_rt = now;
      kb_rn = 1;
    }
  }
  if (ms_qn) {
    *dx = ms_q[ms_qh][0];
    *dy = ms_q[ms_qh][1];
    *dz = ms_q[ms_qh][2];
    *btn = ms_q[ms_qh][3];
    ms_qh = (ms_qh + 1) % MSQ;
    ms_qn--;
    got = 1;
  }
  xl_leave();
out:
  if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
  return got;
}

/* Com a RIT lendo os relatorios: o que xhci_mouse_poll fazia alem disso
 * (endpoint parado, mouse ligado depois, portas). O mundo chama de vez em
 * quando, com IF ligado. */
void xhci_mouse_maint(void) {
  static uint32_t scan_ms;
  int i;
  if (!xc_n) return;
  xl_enter();
  if (busy) {
    xl_leave();
    return;
  }
  if (ms_fix) {
    int k;
    ms_fix = 0;
    for (k = 0; k < ms_n; k++) {
      struct udev* d = &devs[msl[k]];
      if (!d->m_fix) continue;
      d->m_fix = 0;
      if (++d->m_errs > 8) {
        int j;
        xlog("[xhci] mouse com erro, desligado\n");
        if (d->kind == UD_MOUSE) d->kind = UD_OTHER;
        for (j = k; j + 1 < ms_n; j++) msl[j] = msl[j + 1];
        ms_n--;
        k--;
      } else {
        ep_reset(d, d->dci_in, &d->rin);
        ms_arm(d);
      }
    }
  }
  if (!ms_n && net_has_clock() && net_ms() - scan_ms >= 500u) {
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
  xl_leave();
}

/* 1 se ha mouse ou teclado USB (a RIT precisa olhar o anel). */
int xhci_rit_wanted(void) {
  return xc_n > 0 && (ms_n > 0 || kb >= 0);
}

/* Interrupcao por MSI: cada evento novo manda o vetor vec ao LAPIC apic.
 * Devolve quantos controladores ficaram com MSI (0 = so polling). */
static uint8_t xc_msi[XC_MAX];
int xhci_rit_irq(uint32_t apic, uint8_t vec) {
  int i, n = 0;
  xl_enter();
  for (i = 0; i < xc_n; i++) {
    struct xc* x = &xcs[i];
    uint32_t st = pci_rd(x->bus, x->dev, x->fun, 0x04);
    int ptr, guard = 0, msi = 0, msix = 0;
    if (!x->up || !(st & (1u << 20))) continue;          /* sem lista de capacidades */
    ptr = (int)(pci_rd(x->bus, x->dev, x->fun, 0x34) & 0xFCu);
    while (ptr >= 0x40 && guard++ < 48) {
      uint32_t h = pci_rd(x->bus, x->dev, x->fun, ptr);
      if ((h & 0xFFu) == 0x05 && !msi) msi = ptr;
      if ((h & 0xFFu) == 0x11 && !msix) msix = ptr;
      ptr = (int)((h >> 8) & 0xFCu);
    }
    if (!msi) {
      xlog("[xhci] sem MSI: a RIT le por polling\n");
      continue;
    }
    if (msix) {                                           /* MSI-X desligado */
      uint32_t h = pci_rd(x->bus, x->dev, x->fun, msix);
      pci_wr(x->bus, x->dev, x->fun, msix, h & ~(1u << 31));
    }
    {
      uint32_t h = pci_rd(x->bus, x->dev, x->fun, msi);
      uint32_t ctl = h >> 16;
      int is64 = (ctl & 0x80u) != 0;
      pci_wr(x->bus, x->dev, x->fun, msi + 4, 0xFEE00000u | ((apic & 0xFFu) << 12));
      if (is64) {
        pci_wr(x->bus, x->dev, x->fun, msi + 8, 0);
        pci_wr(x->bus, x->dev, x->fun, msi + 12, vec);
      } else {
        pci_wr(x->bus, x->dev, x->fun, msi + 8, vec);
      }
      ctl = (ctl & ~0x70u) | 1u;                          /* 1 vetor, ligado */
      pci_wr(x->bus, x->dev, x->fun, msi, (h & 0xFFFFu) | (ctl << 16));
    }
    pci_wr(x->bus, x->dev, x->fun, 0x04, (st & 0xFFFFu) | (1u << 10));   /* sem INTx */
    w32(x->rt, 0x20 + 0x04, 160u);                        /* IMOD: 40 us */
    w32(x->rt, 0x20 + 0x00, 3u);                          /* IMAN: limpa IP, IE = 1 */
    w32(x->op, 0x00, r32(x->op, 0x00) | 4u);              /* USBCMD.INTE */
    xc_msi[i] = 1;
    n++;
    xlog("[xhci] MSI ligado: eventos acordam a RIT\n");
  }
  xl_leave();
  return n;
}

/* Na interrupcao (qualquer CPU): limpa IMAN.IP. Os eventos a RIT le. */
void xhci_rit_ack(void) {
  int i;
  for (i = 0; i < xc_n; i++)
    if (xc_msi[i] && xcs[i].up) w32(xcs[i].rt, 0x20 + 0x00, 3u);
}
