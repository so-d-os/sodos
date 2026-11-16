/* r8125.c - Realtek RTL8125 2.5GbE (PCI 10EC:8125) por polling.
 *
 * A placa onboard do PC real (ASUS TUF B650: RTL8125B, rev 05). Segue o
 * r8169 do Linux (r8169_main.c, caminho do RTL_GIGA_MAC_VER_61/63):
 * rtl_hw_init_8125, reset, rtl_hw_start_8125_common, aneis de descritores
 * de 16 bytes (formato antigo: o novo e desligado no 0xEB58) e TxPoll_8125.
 * Nao carrega o firmware rtl8125b-2.fw nem os ajustes de PHY do
 * r8169_phy_config.c: a PHY fica como o firmware da placa-mae deixou, so
 * acordada, com 10/100/1000/2500 anunciados e a autonegociacao reiniciada.
 *
 * Sem IRQ (IntrMask = 0): net_poll chama rx; tx espera o descritor livre.
 * A memoria e identidade, entao o endereco do buffer e o fisico. */
#include <stdint.h>
#include "r8125.h"
#include "net.h"
#include "mm.h"
#include "serial.h"

extern void puts_k(const char* s);
extern int pci_mmio_map(uint64_t phys, uint64_t len);

#define VID 0x10ECu
#define DID 0x8125u

/* Registradores (r8169_main.c) */
#define R_MAC0      0x00
#define R_MAC4      0x04
#define R_MAR0      0x08
#define R_TXDESC_LO 0x20
#define R_TXDESC_HI 0x24
#define R_CHIPCMD   0x37  /* 8: StopReq 0x80, Reset 0x10, RxEnb 0x08, TxEnb 0x04 */
#define R_IMR       0x38  /* IntrMask_8125, 32 bits */
#define R_ISR       0x3C  /* IntrStatus_8125, 32 bits */
#define R_TXCFG     0x40
#define R_RXCFG     0x44
#define R_CFG9346   0x50
#define R_CONFIG1   0x52
#define R_CONFIG2   0x53
#define R_CONFIG3   0x54
#define R_CONFIG5   0x56
#define R_PHYSTATUS 0x6C  /* 16 bits */
#define R_PMCH      0x6F
#define R_TXPOLL    0x90  /* TxPoll_8125, 16 bits */
#define R_OCPDR     0xB0  /* OCP da MAC */
#define R_GPHY_OCP  0xB8  /* OCP da PHY */
#define R_MCU       0xD3
#define R_RXMAXSIZE 0xDA
#define R_CPCMD     0xE0
#define R_RXDESC_LO 0xE4
#define R_RXDESC_HI 0xE8
#define R_MISC      0xF0
#define R_MAC0_BKP  0x19E0

#define CMD_STOPREQ 0x80u
#define CMD_RESET   0x10u
#define CMD_RXENB   0x08u
#define CMD_TXENB   0x04u

/* PHYstatus */
#define PS_FULLDUP  0x0001u
#define PS_LINK     0x0002u
#define PS_10       0x0004u
#define PS_100      0x0008u
#define PS_1000F    0x0010u
#define PS_2500F    0x0400u
#define PS_5000F    0x1000u

/* PHY: registradores MII padrao em OCP 0xA400 + 2*n */
#define PHY_BMCR    0xA400
#define PHY_ANAR    0xA408
#define PHY_GBCR    0xA412
#define PHY_ADV2500 0xA5D4  /* MDIO_AN_10GBT_CTRL: bit 7 = 2500BASE-T */
#define PHY_STATE   0xA420  /* bits 2..0 = 3: LAN_ON */
#define PHY_PHYSR   0xA434

/* Descritores (formato de 16 bytes do r8169) */
#define D_OWN   0x80000000u
#define D_EOR   0x40000000u
#define D_FS    0x20000000u
#define D_LS    0x10000000u
#define D_RXRES 0x00200000u  /* resumo de erro na recepcao */

#define NRX   256u
#define NTX   64u
#define BUFSZ 2048u

struct desc {
  volatile uint32_t opts1;
  volatile uint32_t opts2;
  volatile uint64_t addr;
};

static struct {
  volatile uint8_t* bar;
  uint64_t bar_phys;
  int bus, dev, fun;
  uint8_t rev;
  uint32_t xid;
  const char* model;
  uint8_t mac[6];
  struct desc* rx;
  struct desc* tx;
  uint8_t* rxbuf;
  uint8_t* txbuf;
  uint32_t rx_cur, tx_cur;
  int up;           /* placa ligada */
  int link;         /* ultimo link visto pelo tick */
  uint32_t speed;   /* Mb/s */
  int full;
  uint32_t tick_ms;
  uint32_t rx_ok, rx_err, tx_ok, tx_busy, link_changes;
} rt;

/* ---------- E/S ---------- */

static inline uint8_t r8(uint32_t o) { return *(volatile uint8_t*)(rt.bar + o); }
static inline uint16_t r16(uint32_t o) { return *(volatile uint16_t*)(rt.bar + o); }
static inline uint32_t r32(uint32_t o) { return *(volatile uint32_t*)(rt.bar + o); }
static inline void w8(uint32_t o, uint8_t v) { *(volatile uint8_t*)(rt.bar + o) = v; }
static inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t*)(rt.bar + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t*)(rt.bar + o) = v; }
static inline void barrier(void) { __asm__ volatile ("mfence" ::: "memory"); }

static inline void outl_(uint16_t port, uint32_t v) {
  __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t inl_(uint16_t port) {
  uint32_t v;
  __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}
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

/* ~1 us por escrita na porta 0x80 (nao depende do PIT). */
static void udelay(uint32_t us) {
  while (us--) __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}
static void mdelay(uint32_t ms) { udelay(ms * 1000u); }

/* ---------- log (tela e COM1, como o nlog do nvme.c) ---------- */

static void lg(const char* s) {
  serial_puts(s);
  puts_k(s);
}

static void fmt_u(char* b, uint32_t v) {
  char t[12];
  int n = 0, i = 0;
  do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
  while (n) b[i++] = t[--n];
  b[i] = 0;
}

static void lg_u(uint32_t v) {
  char b[12];
  fmt_u(b, v);
  lg(b);
}

static void lg_hex(uint32_t v, int digits) {
  static const char hx[] = "0123456789ABCDEF";
  char b[9];
  int i;
  if (digits > 8) digits = 8;
  for (i = 0; i < digits; i++) b[i] = hx[(v >> (4 * (digits - 1 - i))) & 0xFu];
  b[digits] = 0;
  lg(b);
}

static void lg_mac(const uint8_t* m) {
  int i;
  for (i = 0; i < 6; i++) {
    lg_hex(m[i], 2);
    if (i != 5) lg(":");
  }
}

static void lg_ip(uint32_t ip) {
  int i;
  for (i = 3; i >= 0; i--) {
    lg_u((ip >> (i * 8)) & 0xFFu);
    if (i) lg(".");
  }
}

/* ---------- OCP (MAC e PHY) ---------- */

static uint16_t mac_ocp_rd(uint32_t reg) {
  w32(R_OCPDR, reg << 15);
  return (uint16_t)r32(R_OCPDR);
}
static void mac_ocp_wr(uint32_t reg, uint16_t v) {
  w32(R_OCPDR, 0x80000000u | (reg << 15) | v);
}
static void mac_ocp_mod(uint32_t reg, uint16_t mask, uint16_t set) {
  mac_ocp_wr(reg, (uint16_t)((mac_ocp_rd(reg) & ~mask) | set));
}

/* -1 se a PHY nao respondeu */
static int phy_rd(uint32_t reg) {
  int i;
  w32(R_GPHY_OCP, reg << 15);
  for (i = 0; i < 100; i++) {
    udelay(10);
    if (r32(R_GPHY_OCP) & 0x80000000u) return (int)(r32(R_GPHY_OCP) & 0xFFFFu);
  }
  return -1;
}
static void phy_wr(uint32_t reg, uint16_t v) {
  int i;
  w32(R_GPHY_OCP, 0x80000000u | (reg << 15) | v);
  for (i = 0; i < 100; i++) {
    udelay(10);
    if (!(r32(R_GPHY_OCP) & 0x80000000u)) return;
  }
}

static void unlock_cfg(void) { w8(R_CFG9346, 0xC0); }
static void lock_cfg(void) { w8(R_CFG9346, 0x00); }

/* Espera (r8(reg) & bit) virar want; 0 = ok. */
static int wait8(uint32_t reg, uint8_t bit, int want, uint32_t us) {
  while (us--) {
    if (((r8(reg) & bit) != 0) == want) return 0;
    udelay(1);
  }
  return -1;
}

/* ---------- inicializacao ---------- */

/* rtl_enable_rxdvgate + rtl_wait_txrx_fifo_empty (VER_61+) */
static void rxdv_gate_on(void) {
  w32(R_MISC, r32(R_MISC) | (1u << 19));
  mdelay(2);
  w8(R_CHIPCMD, (uint8_t)(r8(R_CHIPCMD) | CMD_STOPREQ));
  udelay(4200);
}

/* rtl_hw_init_8125 */
static void hw_init_8125(void) {
  rxdv_gate_on();
  w8(R_CHIPCMD, (uint8_t)(r8(R_CHIPCMD) & ~(CMD_TXENB | CMD_RXENB)));
  mdelay(1);
  w8(R_MCU, (uint8_t)(r8(R_MCU) & ~0x80u));  /* NOW_IS_OOB */
  mac_ocp_mod(0xE8DE, 1u << 14, 0);
  (void)wait8(R_MCU, 0x02, 1, 4200);         /* LINK_LIST_RDY */
  mac_ocp_wr(0xC0AA, 0x07D0);
  mac_ocp_wr(0xC0A6, 0x0150);
  mac_ocp_wr(0xC01E, 0x5555);
  (void)wait8(R_MCU, 0x02, 1, 4200);
}

/* rtl8169_cleanup + rtl_hw_reset */
static int hw_reset(void) {
  w32(R_IMR, 0);
  w32(R_ISR, 0xFFFFFFFFu);
  w32(R_RXCFG, r32(R_RXCFG) & ~0x3Fu);  /* rtl_rx_close */
  rxdv_gate_on();
  w8(R_CHIPCMD, CMD_RESET);
  return wait8(R_CHIPCMD, CMD_RESET, 0, 100000);
}

/* rtl_hw_start_8125_common (sem o firmware e sem EPHY) */
static void hw_start_8125(void) {
  int b = rt.xid == 0x641u;  /* RTL8125B = RTL_GIGA_MAC_VER_63 */
  uint32_t i;
  w8(R_CONFIG3, (uint8_t)(r8(R_CONFIG3) & ~0x02u));  /* Rdy_to_L23 */
  w16(0x382, 0x221B);
  w8(0x4500, 0);  /* RSS desligado */
  w16(0x4800, 0);
  mac_ocp_mod(0xD40A, 0x0010, 0x0000);  /* UPS desligado */
  w8(R_CONFIG1, (uint8_t)(r8(R_CONFIG1) & ~0x10u));
  mac_ocp_wr(0xC140, 0xFFFF);
  mac_ocp_wr(0xC142, 0xFFFF);
  mac_ocp_mod(0xD3E2, 0x0FFF, 0x03A9);
  mac_ocp_mod(0xD3E4, 0x00FF, 0x0000);
  mac_ocp_mod(0xE860, 0x0000, 0x0080);
  mac_ocp_mod(0xEB58, 0x0001, 0x0000);  /* descritor de TX no formato antigo */
  mac_ocp_mod(0xE614, 0x0700, b ? 0x0200 : 0x0400);
  mac_ocp_mod(0xE63E, 0x0C30, b ? 0x0000 : 0x0020);
  mac_ocp_mod(0xC0B4, 0x0000, 0x000C);
  mac_ocp_mod(0xEB6A, 0x00FF, 0x0033);
  mac_ocp_mod(0xEB50, 0x03E0, 0x0040);
  mac_ocp_mod(0xE056, 0x00F0, 0x0030);
  mac_ocp_mod(0xE040, 0x1000, 0x0000);
  mac_ocp_mod(0xEA1C, 0x0003, 0x0001);
  mac_ocp_mod(0xE0C0, 0x4F0F, 0x4403);
  mac_ocp_mod(0xE052, 0x0080, 0x0068);
  mac_ocp_mod(0xD430, 0x0FFF, 0x047F);
  mac_ocp_mod(0xEA1C, 0x0004, 0x0000);
  mac_ocp_mod(0xEB54, 0x0000, 0x0001);
  udelay(1);
  mac_ocp_mod(0xEB54, 0x0001, 0x0000);
  w16(0x1880, (uint16_t)(r16(0x1880) & ~0x0030u));
  mac_ocp_wr(0xE098, 0xC302);
  for (i = 0; i < 1000 && (mac_ocp_rd(0xE00E) & (1u << 13)); i++) udelay(10);
  /* rtl_enable_tx_lpi(true): EEE na MAC */
  mac_ocp_mod(0xE040, 0, 0x0003);
  if (rt.xid == 0x609u) mac_ocp_mod(0xEB62, 0, 0x0006);
  w32(R_MISC, r32(R_MISC) & ~(1u << 19));  /* rtl_disable_rxdvgate */
}

static void* dma_alloc(uint32_t n) {
  uint8_t* raw = (uint8_t*)umalloc_owned(n + 4096u, 0);
  uint8_t* a;
  uint32_t i;
  if (!raw) return 0;
  a = (uint8_t*)(uintptr_t)(((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u);
  for (i = 0; i < n; i++) a[i] = 0;
  return a;
}

static void rx_give(uint32_t i) {
  rt.rx[i].opts2 = 0;
  rt.rx[i].addr = (uint64_t)(uintptr_t)(rt.rxbuf + i * BUFSZ);
  barrier();
  rt.rx[i].opts1 = D_OWN | (i == NRX - 1u ? D_EOR : 0) | BUFSZ;
}

static void rings_init(void) {
  uint32_t i;
  for (i = 0; i < NRX; i++) rx_give(i);
  for (i = 0; i < NTX; i++) {
    rt.tx[i].opts1 = i == NTX - 1u ? D_EOR : 0;
    rt.tx[i].opts2 = 0;
    rt.tx[i].addr = (uint64_t)(uintptr_t)(rt.txbuf + i * BUFSZ);
  }
  rt.rx_cur = 0;
  rt.tx_cur = 0;
  barrier();
}

/* rtl_hw_start para o 8125 */
static void hw_start(void) {
  uint64_t txp = (uint64_t)(uintptr_t)rt.tx;
  uint64_t rxp = (uint64_t)(uintptr_t)rt.rx;
  unlock_cfg();
  /* ASPM e CLKREQ desligados: em polling nao vale a pena o L1 */
  w8(R_CONFIG5, (uint8_t)(r8(R_CONFIG5) & ~0x01u));
  w8(R_CONFIG2, (uint8_t)(r8(R_CONFIG2) & ~0x80u));
  /* sem VLAN nem checksum na placa: a pilha confere sozinha */
  w16(R_CPCMD, (uint16_t)(r16(R_CPCMD) & 0x2003u));
  hw_start_8125();
  mac_ocp_mod(0xC0AC, 0, 0x1F80);  /* rtl_enable_exit_l1 */
  w16(R_RXMAXSIZE, (uint16_t)BUFSZ);
  /* o r8169 escreve a parte alta primeiro */
  w32(R_TXDESC_HI, (uint32_t)(txp >> 32));
  w32(R_TXDESC_LO, (uint32_t)txp);
  w32(R_RXDESC_HI, (uint32_t)(rxp >> 32));
  w32(R_RXDESC_LO, (uint32_t)rxp);
  lock_cfg();
  (void)r8(R_CHIPCMD);
  w8(R_CHIPCMD, CMD_TXENB | CMD_RXENB);
  /* rtl_init_rxcfg: RX_FETCH_DFLT_8125 | RX_DMA_BURST (| RX_PAUSE_SLOT_ON no B) */
  w32(R_RXCFG, (8u << 27) | (7u << 8) | (rt.xid == 0x641u ? (1u << 11) : 0));
  /* rtl_set_tx_config_registers: DMA burst 7, IFG 3, TXCFG_AUTO_FIFO */
  w32(R_TXCFG, (7u << 8) | (3u << 24) | (1u << 7));
  /* rtl_set_rx_mode: broadcast + o nosso MAC + multicast (filtro todo aberto) */
  w32(R_MAR0 + 4, 0xFFFFFFFFu);
  w32(R_MAR0, 0xFFFFFFFFu);
  w32(R_RXCFG, (r32(R_RXCFG) & ~0x3Fu) | 0x0Eu);
  w32(R_IMR, 0);  /* polling */
  w32(R_ISR, 0xFFFFFFFFu);
}

/* rtl_rar_set */
static void mac_set(const uint8_t* a) {
  unlock_cfg();
  w32(R_MAC4, (uint32_t)a[4] | ((uint32_t)a[5] << 8));
  (void)r8(R_CHIPCMD);
  w32(R_MAC0, (uint32_t)a[0] | ((uint32_t)a[1] << 8) | ((uint32_t)a[2] << 16) |
              ((uint32_t)a[3] << 24));
  (void)r8(R_CHIPCMD);
  lock_cfg();
}

static int mac_valid(const uint8_t* m) {
  int i, z = 1;
  for (i = 0; i < 6; i++) if (m[i]) z = 0;
  return !z && !(m[0] & 1u);
}

/* rtl_read_mac_address: no 8125 o firmware guarda o MAC em MAC0_BKP */
static void mac_read(void) {
  int i;
  for (i = 0; i < 6; i++) rt.mac[i] = r8(R_MAC0_BKP + (uint32_t)i);
  if (!mac_valid(rt.mac))
    for (i = 0; i < 6; i++) rt.mac[i] = r8(R_MAC0 + (uint32_t)i);
}

/* Acorda a PHY e reinicia a autonegociacao com tudo anunciado
 * (rtl_pll_power_up, genphy_resume, rtl822x_config_aneg, genphy_soft_reset). */
static void phy_up(void) {
  int st = -1, v, i;
  w8(R_PMCH, (uint8_t)(r8(R_PMCH) | 0x80u));
  for (i = 0; i < 100; i++) {
    st = phy_rd(PHY_STATE);
    if (st >= 0 && (st & 7) == 3) break;
    mdelay(1);
  }
  serial_puts("[r8125] phy state ");
  serial_hex32((uint32_t)st);
  serial_puts("\n");
  v = phy_rd(PHY_BMCR);
  if (v >= 0 && (v & 0x0C00))  /* PDOWN ou ISOLATE */
    phy_wr(PHY_BMCR, (uint16_t)(v & ~0x0C00));
  v = phy_rd(PHY_ANAR);
  if (v >= 0) phy_wr(PHY_ANAR, (uint16_t)((v & ~0x001Fu) | 0x01E1u));  /* 10/100 HD/FD */
  v = phy_rd(PHY_GBCR);
  if (v >= 0) phy_wr(PHY_GBCR, (uint16_t)((v & ~0x0100u) | 0x0200u));  /* 1000 FD */
  v = phy_rd(PHY_ADV2500);
  if (v >= 0) phy_wr(PHY_ADV2500, (uint16_t)(v | 0x0080u));            /* 2500 FD */
  phy_wr(PHY_BMCR, 0x1200);  /* ANENABLE | ANRESTART */
}

static void link_read(void) {
  uint16_t s = r16(R_PHYSTATUS);
  rt.link = (s & PS_LINK) != 0;
  rt.full = (s & PS_FULLDUP) != 0;
  rt.speed = (s & PS_5000F) ? 5000u : (s & PS_2500F) ? 2500u : (s & PS_1000F) ? 1000u
           : (s & PS_100) ? 100u : (s & PS_10) ? 10u : 0u;
  if (s & (PS_1000F | PS_2500F | PS_5000F)) rt.full = 1;
  if (rt.link) {
    /* O Linux tira a velocidade da PHY (rtlgen_decode_physr), nao do
     * PHYstatus: bits 10..9 e 5..4 do PHYSR, duplex no bit 3. */
    int v = phy_rd(PHY_PHYSR);
    if (v >= 0 && (v & 0x0004)) {
      switch (v & 0x0630) {
        case 0x0000: rt.speed = 10u; break;
        case 0x0010: rt.speed = 100u; break;
        case 0x0020: rt.speed = 1000u; break;
        case 0x0210: rt.speed = 2500u; break;
        case 0x0220: rt.speed = 5000u; break;
        case 0x0200: rt.speed = 10000u; break;
        default: break;
      }
      rt.full = (v & 0x0008) != 0;
    }
  }
}

/* ---------- quadros ---------- */

static int rt_tx(const uint8_t* frame, int len) {
  struct desc* d;
  uint8_t* b;
  uint32_t i, n, spins;
  if (!rt.up || len < 14) return -1;
  if (len > 1514) len = 1514;
  d = &rt.tx[rt.tx_cur];
  for (spins = 0; (d->opts1 & D_OWN) && spins < 20000u; spins++) udelay(1);
  if (d->opts1 & D_OWN) {
    rt.tx_busy++;
    return -1;
  }
  b = rt.txbuf + rt.tx_cur * BUFSZ;
  for (i = 0; i < (uint32_t)len; i++) b[i] = frame[i];
  n = len < 60 ? 60u : (uint32_t)len;
  for (; i < n; i++) b[i] = 0;
  d->opts2 = 0;
  d->addr = (uint64_t)(uintptr_t)b;
  barrier();
  d->opts1 = D_OWN | D_FS | D_LS | (rt.tx_cur == NTX - 1u ? D_EOR : 0) | n;
  barrier();
  w16(R_TXPOLL, 1);
  rt.tx_cur = (rt.tx_cur + 1u) % NTX;
  rt.tx_ok++;
  return 0;
}

static int rt_rx(uint8_t* frame, int cap) {
  int tries;
  if (!rt.up) return 0;
  for (tries = 0; tries < 8; tries++) {
    uint32_t i = rt.rx_cur;
    uint32_t st = rt.rx[i].opts1;
    int len, k;
    const uint8_t* b;
    if (st & D_OWN) {
      uint32_t isr = r32(R_ISR);
      if (isr) w32(R_ISR, isr);  /* sem IRQ, mas o RDU/FOVW fica limpo */
      return 0;
    }
    __asm__ volatile ("lfence" ::: "memory");
    rt.rx_cur = (i + 1u) % NRX;
    len = (int)(st & 0x3FFFu) - 4;  /* tira o FCS */
    if ((st & D_RXRES) || (st & (D_FS | D_LS)) != (D_FS | D_LS) || len < 14) {
      rt.rx_err++;
      rx_give(i);
      continue;
    }
    if (len > cap) len = cap;
    b = rt.rxbuf + i * BUFSZ;
    for (k = 0; k < len; k++) frame[k] = b[k];
    rx_give(i);
    rt.rx_ok++;
    return len;
  }
  return 0;
}

static void lg_link(void) {
  if (!rt.link) {
    lg("sem link");
    return;
  }
  lg("link ");
  lg_u(rt.speed);
  lg(rt.full ? " Mb/s full duplex" : " Mb/s half duplex");
}

static void put_addr(void) {
  uint32_t ip, mask, gw, dns;
  net_get_addr(&ip, &mask, &gw, &dns);
  lg("ip ");
  lg_ip(ip);
  lg("  gateway ");
  lg_ip(gw);
  lg("  dns ");
  lg_ip(dns);
  lg("\n");
}

/* Roda a cada net_poll (fora das esperas da pilha). Vigia o link: ao subir
 * de novo, pede DHCP (o roteador pode ter mudado). So COM1: a tela pode
 * estar no desktop. */
static void rt_tick(void) {
  int was = rt.link;
  uint32_t now = net_ms();
  if (!rt.up || now - rt.tick_ms < 500u) return;
  rt.tick_ms = now;
  link_read();
  if (rt.link == was) return;
  rt.link_changes++;
  serial_puts(rt.link ? "[r8125] link subiu\n" : "[r8125] link caiu\n");
  if (rt.link && net_dhcp() == 0) serial_puts("[r8125] DHCP ok\n");
}

static const struct net_link rt_link = {"RTL8125", rt.mac, rt_tx, rt_rx, rt_tick};

/* Liga D0 pela capacidade de energia (o firmware pode ter deixado em D3). */
static void pci_d0(int bus, int dev, int fun) {
  uint32_t p;
  int n = 0;
  if (!(pci_rd(bus, dev, fun, 0x04) & 0x00100000u)) return;
  p = pci_rd(bus, dev, fun, 0x34) & 0xFCu;
  while (p && n++ < 48) {
    uint32_t c = pci_rd(bus, dev, fun, (int)p);
    if ((c & 0xFFu) == 0x01u) {
      uint32_t cs = pci_rd(bus, dev, fun, (int)p + 4);
      if (cs & 3u) {
        pci_wr(bus, dev, fun, (int)p + 4, cs & ~3u);
        mdelay(10);
        serial_puts("[r8125] D3 -> D0\n");
      }
    } else if ((c & 0xFFu) == 0x10u) {
      /* PCIe: ASPM desligado no Link Control */
      uint32_t lc = pci_rd(bus, dev, fun, (int)p + 0x10);
      if (lc & 3u) pci_wr(bus, dev, fun, (int)p + 0x10, lc & ~3u);
    }
    p = (c >> 8) & 0xFCu;
  }
}

static int find(void) {
  int bus, dev, fun;
  for (bus = 0; bus < 256; bus++) {
    for (dev = 0; dev < 32; dev++) {
      int nf = 1;
      for (fun = 0; fun < nf; fun++) {
        uint32_t id = pci_rd(bus, dev, fun, 0);
        if (id == 0xFFFFFFFFu || id == 0) continue;
        if (fun == 0 && (pci_rd(bus, dev, 0, 0x0C) & 0x00800000u)) nf = 8;
        if ((id & 0xFFFFu) != VID || (id >> 16) != DID) continue;
        rt.bus = bus;
        rt.dev = dev;
        rt.fun = fun;
        rt.rev = (uint8_t)pci_rd(bus, dev, fun, 0x08);
        return 0;
      }
    }
  }
  return -1;
}

/* Primeiro BAR de memoria (no 8125: BAR0 e I/O, BAR2 e MMIO de 64 bits). */
static int bar_map(void) {
  int off;
  for (off = 0x10; off <= 0x24; off += 4) {
    uint32_t lo = pci_rd(rt.bus, rt.dev, rt.fun, off);
    uint32_t hi = 0;
    if (lo & 1u) continue;  /* I/O */
    if ((lo & 6u) == 4u) hi = pci_rd(rt.bus, rt.dev, rt.fun, off + 4);
    rt.bar_phys = ((uint64_t)hi << 32) | (lo & ~0xFu);
    if (!rt.bar_phys) {
      if ((lo & 6u) == 4u) off += 4;
      continue;
    }
    if (pci_mmio_map(rt.bar_phys, 0x10000u) != 0) return -2;
    rt.bar = (volatile uint8_t*)(uintptr_t)rt.bar_phys;
    return 0;
  }
  return -1;
}

/* rtl_chip_infos do r8169 (mascara 0x7CF) */
static const char* model_of(uint32_t xid) {
  switch (xid & 0x7CFu) {
    case 0x609u: return "RTL8125A";
    case 0x641u: return "RTL8125B";
    case 0x688u: case 0x689u: return "RTL8125D";
    case 0x68Au: return "RTL8125K";
    case 0x681u: return "RTL8125BP";
    case 0x708u: return "RTL8125CP";
    default: return "RTL8125";
  }
}

int r8125_present(void) { return rt.up; }

int r8125_start(void) {
  uint32_t cmd, t0;
  int rc;
  if (rt.up) return 0;
  if (find() != 0) return -1;
  lg("Rede: Realtek RTL8125 em PCI ");
  lg_hex((uint32_t)rt.bus, 2);
  lg(":");
  lg_hex((uint32_t)rt.dev, 2);
  lg(".");
  lg_u((uint32_t)rt.fun);
  lg(", rev ");
  lg_hex(rt.rev, 2);
  lg("\n");
  pci_d0(rt.bus, rt.dev, rt.fun);
  cmd = pci_rd(rt.bus, rt.dev, rt.fun, 0x04);
  /* memoria + bus master, INTx desligado (so polling) */
  pci_wr(rt.bus, rt.dev, rt.fun, 0x04, (cmd & 0xFFFFu) | 0x6u | 0x400u);
  rc = bar_map();
  if (rc != 0) {
    lg(rc == -2 ? "  RTL8125: BAR fora do alcance\n" : "  RTL8125: sem BAR de memoria\n");
    return -1;
  }
  if (r32(R_TXCFG) == 0xFFFFFFFFu) {
    lg("  RTL8125: a placa nao responde (BAR ");
    lg_hex((uint32_t)(rt.bar_phys >> 32), 8);
    lg_hex((uint32_t)rt.bar_phys, 8);
    lg(")\n");
    return -1;
  }
  rt.xid = (r32(R_TXCFG) >> 20) & 0xFCFu;
  rt.model = model_of(rt.xid);
  rt.rx = (struct desc*)dma_alloc(NRX * 16u);
  rt.tx = (struct desc*)dma_alloc(NTX * 16u);
  rt.rxbuf = (uint8_t*)dma_alloc(NRX * BUFSZ);
  rt.txbuf = (uint8_t*)dma_alloc(NTX * BUFSZ);
  if (!rt.rx || !rt.tx || !rt.rxbuf || !rt.txbuf) {
    lg("  RTL8125: sem memoria para os aneis\n");
    return -1;
  }

  /* rtl_init_rxcfg antes do reset, como no rtl_init_one */
  w32(R_RXCFG, (8u << 27) | (7u << 8));
  if (hw_reset() != 0) lg("  RTL8125: o reset nao terminou (seguindo assim mesmo)\n");
  mac_read();
  hw_init_8125();
  mac_set(rt.mac);
  phy_up();
  rings_init();
  (void)hw_reset();  /* rtl8169_cleanup, como no rtl_reset_work do open */
  mac_set(rt.mac);
  hw_start();
  rt.up = 1;

  lg("  ");
  lg(rt.model);
  lg(" (XID ");
  lg_hex(rt.xid, 3);
  lg(")  MAC ");
  lg_mac(rt.mac);
  lg("\n");
  net_attach_base(&rt_link);

  /* Autonegociacao em 2,5 G leva uns 3 s; ate 8 s, sem depender do PIT. */
  lg("  esperando o link...");
  for (t0 = 0, link_read(); !rt.link && t0 < 80u; t0++) {
    mdelay(100);
    if (t0 % 10u == 9u) lg(".");
    link_read();
  }
  lg(" ");
  lg_link();
  lg("\n");
  rt.tick_ms = net_ms();
  if (!rt.link) {
    lg("  sem cabo? Com o cabo ligado o DHCP roda sozinho (ou: net dhcp)\n");
    return 0;
  }
  lg("  DHCP...");
  rc = net_dhcp();
  if (rc != 0) {
    lg(" ninguem respondeu (tente: net dhcp)\n");
    return 0;
  }
  lg(" ");
  put_addr();
  return 0;
}

void r8125_status(void) {
  if (!rt.up) {
    lg("Ethernet: RTL8125 nao encontrada\n");
    return;
  }
  link_read();
  lg("Ethernet: ");
  lg(rt.model);
  lg("  MAC ");
  lg_mac(rt.mac);
  lg("  ");
  lg_link();
  lg("\n  PHYstatus ");
  lg_hex(r16(R_PHYSTATUS), 4);
  {
    int v = phy_rd(PHY_PHYSR);
    lg("  PHYSR ");
    lg_hex(v < 0 ? 0xFFFFu : (uint32_t)v, 4);
  }
  lg("  quedas/subidas ");
  lg_u(rt.link_changes);
  lg("\n  rx ");
  lg_u(rt.rx_ok);
  lg(" (erros ");
  lg_u(rt.rx_err);
  lg(")  tx ");
  lg_u(rt.tx_ok);
  lg(" (fila cheia ");
  lg_u(rt.tx_busy);
  lg(")\n");
}
