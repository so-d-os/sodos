/* RTL8852BE: etapa 1 do porte do rtw89 (Linux 6.12, rtw8852b.c, mac.c,
 * efuse.c, core.c), Copyright(c) 2019-2022 Realtek Corporation, usado sob a
 * licenca BSD-3-Clause do rtw89 ("GPL-2.0 OR BSD-3-Clause"). Ver rtw89.h. */
#include "rtw89.h"
#include "rtw89_reg.h"
#include "net.h"
#include "serial.h"
#include "xhci.h"
#include "mm.h"

#define RTW_VID 0x10ECu
#define RTW_DID_8852BE 0xB852u
#define RTW_BAR_LEN 0x100000u
#define CHIP_CAV 0

#define PHY_EFUSE_SIZE 1216
#define LOG_EFUSE_SIZE 2048
#define SEC_CTRL_EFUSE_SIZE 4
/* rtw8852bx_efuse (rtw8852b_common.h), offsets medidos com offsetof. */
#define EF_XTAL 697
#define EF_RFE 714
#define EF_MAC_PCIE 1024

static volatile uint8_t* mmio;
static int chip_cv;
static const char* last_err = "";

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

static uint32_t r32(uint32_t a) { return *(volatile uint32_t*)(mmio + a); }
static uint16_t r16(uint32_t a) { return *(volatile uint16_t*)(mmio + a); }
static uint8_t r8(uint32_t a) { return *(volatile uint8_t*)(mmio + a); }
static void w32(uint32_t a, uint32_t v) { *(volatile uint32_t*)(mmio + a) = v; }
static void w16(uint32_t a, uint16_t v) { *(volatile uint16_t*)(mmio + a) = v; }
static void w8(uint32_t a, uint8_t v) { *(volatile uint8_t*)(mmio + a) = v; }

static void set32(uint32_t a, uint32_t b) { w32(a, r32(a) | b); }
static void clr32(uint32_t a, uint32_t b) { w32(a, r32(a) & ~b); }
static void set16(uint32_t a, uint16_t b) { w16(a, (uint16_t)(r16(a) | b)); }
static void clr16(uint32_t a, uint16_t b) { w16(a, (uint16_t)(r16(a) & ~b)); }
static void set8(uint32_t a, uint8_t b) { w8(a, (uint8_t)(r8(a) | b)); }
static void clr8(uint32_t a, uint8_t b) { w8(a, (uint8_t)(r8(a) & ~b)); }

static int lowbit(uint32_t mask) {
  int s = 0;
  while (mask && !(mask & 1u)) { mask >>= 1; s++; }
  return s;
}

static uint32_t field(uint32_t mask, uint32_t v) { return (v << lowbit(mask)) & mask; }

static uint32_t get32(uint32_t a, uint32_t mask) { return (r32(a) & mask) >> lowbit(mask); }

static void mask32(uint32_t a, uint32_t mask, uint32_t v) {
  w32(a, (r32(a) & ~mask) | field(mask, v));
}

static void mask16(uint32_t a, uint16_t mask, uint16_t v) {
  w16(a, (uint16_t)((r16(a) & ~mask) | field(mask, v)));
}

/* O notebook nao tem COM1: o log tambem fica aqui, e o wifi grava em WIFI.LOG. */
static char logbuf[32768];
static int loglen;

void rtw_log(const char* s) {
  serial_puts(s);
  while (*s && loglen < (int)sizeof(logbuf) - 1) logbuf[loglen++] = *s++;
  logbuf[loglen] = 0;
}

void rtw_log_hex(uint32_t v) {
  char b[9];
  int i;
  for (i = 0; i < 8; i++) b[i] = "0123456789ABCDEF"[(v >> (28 - i * 4)) & 15u];
  b[8] = 0;
  rtw_log(b);
}

const char* rtw_log_text(int* len) {
  if (len) *len = loglen;
  return logbuf;
}

void rtw_log_clear(void) {
  loglen = 0;
  logbuf[0] = 0;
}

static void rlog(const char* s) { rtw_log(s); }

static void rlog_hex(const char* s, uint32_t v) {
  rtw_log(s);
  rtw_log_hex(v);
  rtw_log("\n");
}

/* Primeiros registradores da MAC, para ver o estado quando algo falha. */
static void dump_regs(void) {
  uint32_t a;
  for (a = 0; a < 0x100u; a += 16) {
    rlog_hex("[rtw] reg ", a);
    rtw_log("      ");
    rtw_log_hex(r32(a));
    rtw_log(" ");
    rtw_log_hex(r32(a + 4));
    rtw_log(" ");
    rtw_log_hex(r32(a + 8));
    rtw_log(" ");
    rtw_log_hex(r32(a + 12));
    rtw_log("\n");
  }
}

static int fail(const char* why) {
  last_err = why;
  rtw_log("[rtw] falhou: ");
  rtw_log(why);
  rtw_log("\n");
  if (mmio) dump_regs();
  return -1;
}

const char* rtw_error(void) { return last_err; }

/* Espera em ms com o PIT; sem PIT, conta voltas (aproximado). */
static int late(uint32_t t0, uint32_t* spins, uint32_t ms) {
  __asm__ volatile ("pause");
  if (net_has_clock()) return (net_ms() - t0) >= ms;
  return ++(*spins) >= ms * 100000u;
}

static void msleep(uint32_t ms) {
  uint32_t t0 = net_ms(), spins = 0;
  while (!late(t0, &spins, ms)) { }
}

/* 0 quando (r32(a) & mask) != 0 for igual a 'set' antes de ms. */
static int poll32(uint32_t a, uint32_t mask, int set, uint32_t ms) {
  uint32_t t0 = net_ms(), spins = 0;
  for (;;) {
    int on = (r32(a) & mask) != 0;
    if (on == set) return 0;
    if (late(t0, &spins, ms)) return -1;
  }
}

/* ---------- XTAL SI (registradores do lado analogico) ---------- */

static int xtal_si_write(uint8_t off, uint8_t val, uint8_t mask) {
  w32(R_AX_WLAN_XTAL_SI_CTRL,
      field(B_AX_WL_XTAL_SI_ADDR_MASK, off) | field(B_AX_WL_XTAL_SI_DATA_MASK, val) |
      field(B_AX_WL_XTAL_SI_BITMASK_MASK, mask) |
      field(B_AX_WL_XTAL_SI_MODE_MASK, XTAL_SI_NORMAL_WRITE) | B_AX_WL_XTAL_SI_CMD_POLL);
  if (poll32(R_AX_WLAN_XTAL_SI_CTRL, B_AX_WL_XTAL_SI_CMD_POLL, 0, 50) != 0) {
    rlog_hex("[rtw] xtal si nao respondeu (W) off=", off);
    return -1;
  }
  return 0;
}

static int xtal_si_read(uint8_t off, uint8_t* val) {
  w32(R_AX_WLAN_XTAL_SI_CTRL,
      field(B_AX_WL_XTAL_SI_ADDR_MASK, off) |
      field(B_AX_WL_XTAL_SI_MODE_MASK, XTAL_SI_NORMAL_READ) | B_AX_WL_XTAL_SI_CMD_POLL);
  if (poll32(R_AX_WLAN_XTAL_SI_CTRL, B_AX_WL_XTAL_SI_CMD_POLL, 0, 50) != 0) {
    rlog_hex("[rtw] xtal si nao respondeu (R) off=", off);
    return -1;
  }
  *val = r8(R_AX_WLAN_XTAL_SI_CTRL + 1);
  return 0;
}

/* ---------- energia (rtw8852b_pwr_on_func / pwr_off_func) ---------- */

static int pwr_on_func(void) {
  clr32(R_AX_SYS_PW_CTRL, B_AX_AFSM_WLSUS_EN | B_AX_AFSM_PCIE_SUS_EN);
  set32(R_AX_SYS_PW_CTRL, B_AX_DIS_WLBT_PDNSUSEN_SOPC);
  set32(R_AX_WLLPS_CTRL, B_AX_DIS_WLBT_LPSEN_LOPC);
  clr32(R_AX_SYS_PW_CTRL, B_AX_APDM_HPDN);
  clr32(R_AX_SYS_PW_CTRL, B_AX_APFM_SWLPS);
  if (poll32(R_AX_SYS_PW_CTRL, B_AX_RDY_SYSPWR, 1, 20)) return fail("RDY_SYSPWR");

  set32(R_AX_AFE_LDO_CTRL, B_AX_AON_OFF_PC_EN);
  if (poll32(R_AX_AFE_LDO_CTRL, B_AX_AON_OFF_PC_EN, 1, 20)) return fail("AON_OFF_PC_EN");

  mask32(R_AX_SPS_DIG_OFF_CTRL0, B_AX_C1_L1_MASK, 0x1);
  mask32(R_AX_SPS_DIG_OFF_CTRL0, B_AX_C3_L1_MASK, 0x3);
  set32(R_AX_SYS_PW_CTRL, B_AX_EN_WLON);
  set32(R_AX_SYS_PW_CTRL, B_AX_APFN_ONMAC);
  if (poll32(R_AX_SYS_PW_CTRL, B_AX_APFN_ONMAC, 0, 20)) return fail("APFN_ONMAC");

  set8(R_AX_PLATFORM_ENABLE, B_AX_PLATFORM_EN);
  clr8(R_AX_PLATFORM_ENABLE, B_AX_PLATFORM_EN);
  set8(R_AX_PLATFORM_ENABLE, B_AX_PLATFORM_EN);
  clr8(R_AX_PLATFORM_ENABLE, B_AX_PLATFORM_EN);

  set8(R_AX_PLATFORM_ENABLE, B_AX_PLATFORM_EN);
  clr32(R_AX_SYS_SDIO_CTRL, B_AX_PCIE_CALIB_EN_V1);

  set32(R_AX_SYS_ADIE_PAD_PWR_CTRL, B_AX_SYM_PADPDN_WL_PTA_1P3);
  if (xtal_si_write(XTAL_SI_ANAPAR_WL, XTAL_SI_GND_SHDN_WL, XTAL_SI_GND_SHDN_WL))
    return fail("xtal GND_SHDN_WL");
  set32(R_AX_SYS_ADIE_PAD_PWR_CTRL, B_AX_SYM_PADPDN_WL_RFC_1P3);
  if (xtal_si_write(XTAL_SI_ANAPAR_WL, XTAL_SI_SHDN_WL, XTAL_SI_SHDN_WL) ||
      xtal_si_write(XTAL_SI_ANAPAR_WL, XTAL_SI_OFF_WEI, XTAL_SI_OFF_WEI) ||
      xtal_si_write(XTAL_SI_ANAPAR_WL, XTAL_SI_OFF_EI, XTAL_SI_OFF_EI) ||
      xtal_si_write(XTAL_SI_ANAPAR_WL, 0, XTAL_SI_RFC2RF) ||
      xtal_si_write(XTAL_SI_ANAPAR_WL, XTAL_SI_PON_WEI, XTAL_SI_PON_WEI) ||
      xtal_si_write(XTAL_SI_ANAPAR_WL, XTAL_SI_PON_EI, XTAL_SI_PON_EI) ||
      xtal_si_write(XTAL_SI_ANAPAR_WL, 0, XTAL_SI_SRAM2RFC) ||
      xtal_si_write(XTAL_SI_SRAM_CTRL, 0, XTAL_SI_SRAM_DIS) ||
      xtal_si_write(XTAL_SI_XTAL_XMD_2, 0, XTAL_SI_LDO_LPS) ||
      xtal_si_write(XTAL_SI_XTAL_XMD_4, 0, XTAL_SI_LPS_CAP))
    return fail("xtal si (pwr on)");

  set32(R_AX_PMC_DBG_CTRL2, B_AX_SYSON_DIS_PMCR_AX_WRMSK);
  set32(R_AX_SYS_ISO_CTRL, B_AX_ISO_EB2CORE);
  clr32(R_AX_SYS_ISO_CTRL, B_AX_PWC_EV2EF_B15);
  msleep(1);
  clr32(R_AX_SYS_ISO_CTRL, B_AX_PWC_EV2EF_B14);
  clr32(R_AX_PMC_DBG_CTRL2, B_AX_SYSON_DIS_PMCR_AX_WRMSK);

  /* Primeira vez (efuse ainda nao lido): o Linux pula o ajuste de tensao. */
  set32(R_AX_DMAC_FUNC_EN,
        B_AX_MAC_FUNC_EN | B_AX_DMAC_FUNC_EN | B_AX_MPDU_PROC_EN | B_AX_WD_RLS_EN |
        B_AX_DLE_WDE_EN | B_AX_TXPKT_CTRL_EN | B_AX_STA_SCH_EN | B_AX_DLE_PLE_EN |
        B_AX_PKT_BUF_EN | B_AX_DMAC_TBL_EN | B_AX_PKT_IN_EN | B_AX_DLE_CPUIO_EN |
        B_AX_DISPATCHER_EN | B_AX_BBRPT_EN | B_AX_MAC_SEC_EN | B_AX_DMACREG_GCKEN);
  set32(R_AX_CMAC_FUNC_EN,
        B_AX_CMAC_EN | B_AX_CMAC_TXEN | B_AX_CMAC_RXEN | B_AX_FORCE_CMACREG_GCKEN |
        B_AX_PHYINTF_EN | B_AX_CMAC_DMA_EN | B_AX_PTCLTOP_EN | B_AX_SCHEDULER_EN |
        B_AX_TMAC_EN | B_AX_RMAC_EN);
  mask32(R_AX_EECS_EESK_FUNC_SEL, B_AX_PINMUX_EESK_FUNC_SEL_MASK, PINMUX_EESK_FUNC_SEL_BT_LOG);
  return 0;
}

static int pwr_off_func(void) {
  if (xtal_si_write(XTAL_SI_ANAPAR_WL, XTAL_SI_RFC2RF, XTAL_SI_RFC2RF) ||
      xtal_si_write(XTAL_SI_ANAPAR_WL, 0, XTAL_SI_OFF_EI) ||
      xtal_si_write(XTAL_SI_ANAPAR_WL, 0, XTAL_SI_OFF_WEI) ||
      xtal_si_write(XTAL_SI_WL_RFC_S0, 0, XTAL_SI_RF00) ||
      xtal_si_write(XTAL_SI_WL_RFC_S1, 0, XTAL_SI_RF10) ||
      xtal_si_write(XTAL_SI_ANAPAR_WL, XTAL_SI_SRAM2RFC, XTAL_SI_SRAM2RFC) ||
      xtal_si_write(XTAL_SI_ANAPAR_WL, 0, XTAL_SI_PON_EI) ||
      xtal_si_write(XTAL_SI_ANAPAR_WL, 0, XTAL_SI_PON_WEI))
    return fail("xtal si (pwr off)");
  set32(R_AX_SYS_PW_CTRL, B_AX_EN_WLON);
  clr32(R_AX_WLRF_CTRL, B_AX_AFC_AFEDIG);
  clr8(R_AX_SYS_FUNC_EN, (uint8_t)(B_AX_FEN_BB_GLB_RSTN | B_AX_FEN_BBRSTB));
  clr32(R_AX_SYS_ADIE_PAD_PWR_CTRL, B_AX_SYM_PADPDN_WL_RFC_1P3);
  if (xtal_si_write(XTAL_SI_ANAPAR_WL, 0, XTAL_SI_SHDN_WL)) return fail("xtal SHDN_WL");
  clr32(R_AX_SYS_ADIE_PAD_PWR_CTRL, B_AX_SYM_PADPDN_WL_PTA_1P3);
  if (xtal_si_write(XTAL_SI_ANAPAR_WL, 0, XTAL_SI_GND_SHDN_WL)) return fail("xtal GND_SHDN_WL");
  set32(R_AX_SYS_PW_CTRL, B_AX_APFM_OFFMAC);
  if (poll32(R_AX_SYS_PW_CTRL, B_AX_APFM_OFFMAC, 0, 20)) return fail("APFM_OFFMAC");
  w32(R_AX_WLLPS_CTRL, SW_LPS_OPTION);
  set32(R_AX_SYS_SWR_CTRL1, B_AX_SYM_CTRL_SPS_PWMFREQ);
  mask32(R_AX_SPS_DIG_ON_CTRL0, B_AX_REG_ZCDC_H_MASK, 0x3);
  set32(R_AX_SYS_PW_CTRL, B_AX_APFM_SWLPS);
  return 0;
}

/* rtw89_mac_power_switch: ligada de antes devolve 1 (o chamador desliga e liga). */
static int power_switch(int on) {
  uint32_t st = get32(R_AX_IC_PWR_STATE, B_AX_WLMAC_PWR_STE_MASK);
  int rc;
  if (on && st == 1) return 1;
  rc = on ? pwr_on_func() : pwr_off_func();
  if (rc) return rc;
  w8(R_AX_SCOREBOARD + 3, (uint8_t)(on ? MAC_AX_NOTIFY_TP_MAJOR : MAC_AX_NOTIFY_PWR_MAJOR));
  return 0;
}

/* ---------- efuse (rtw89_dump_physical_efuse_map_ddv e logical) ---------- */

static int efuse_dump(uint8_t* map, int size) {
  int addr;
  set8(R_AX_PMC_DBG_CTRL2, (uint8_t)B_AX_SYSON_DIS_PMCR_AX_WRMSK);
  set16(R_AX_SYS_ISO_CTRL, (uint16_t)B_AX_PWC_EV2EF_B14);
  msleep(1);
  set16(R_AX_SYS_ISO_CTRL, (uint16_t)B_AX_PWC_EV2EF_B15);
  clr16(R_AX_SYS_ISO_CTRL, (uint16_t)B_AX_ISO_EB2CORE);
  if (chip_cv == CHIP_CAV) set32(R_AX_EFUSE_CTRL_1_V1, B_AX_EF_BURST);
  for (addr = 0; addr < size; addr++) {
    uint32_t c = field(B_AX_EF_ADDR_MASK, (uint32_t)addr);
    uint32_t t0 = net_ms(), spins = 0;
    w32(R_AX_EFUSE_CTRL, c & ~B_AX_EF_RDY);
    while (!((c = r32(R_AX_EFUSE_CTRL)) & B_AX_EF_RDY))
      if (late(t0, &spins, 100)) return fail("efuse nao respondeu");
    map[addr] = (uint8_t)c;
  }
  if (chip_cv == CHIP_CAV) clr32(R_AX_EFUSE_CTRL_1_V1, B_AX_EF_BURST);
  set16(R_AX_SYS_ISO_CTRL, (uint16_t)B_AX_ISO_EB2CORE);
  clr16(R_AX_SYS_ISO_CTRL, (uint16_t)B_AX_PWC_EV2EF_B15);
  msleep(1);
  clr16(R_AX_SYS_ISO_CTRL, (uint16_t)B_AX_PWC_EV2EF_B14);
  clr8(R_AX_PMC_DBG_CTRL2, (uint8_t)B_AX_SYSON_DIS_PMCR_AX_WRMSK);
  return 0;
}

/* Fisico (blocos com cabecalho de 2 bytes) para logico. */
static int efuse_logical(const uint8_t* phy, uint8_t* log) {
  int pi = SEC_CTRL_EFUSE_SIZE;
  int i;
  for (i = 0; i < LOG_EFUSE_SIZE; i++) log[i] = 0xFF;
  while (pi < PHY_EFUSE_SIZE - SEC_CTRL_EFUSE_SIZE) {
    uint8_t h1 = phy[pi], h2 = phy[pi + 1];
    int blk, we;
    if (h1 == 0xFF || h2 == 0xFF) break;
    blk = ((h2 & 0xF0) >> 4) | ((h1 & 0x0F) << 4);
    we = h2 & 0x0F;
    pi += 2;
    for (i = 0; i < 4; i++) {
      int li;
      if (we & (1 << i)) continue;
      li = (blk << 3) + (i << 1);
      if (pi + 1 > PHY_EFUSE_SIZE - SEC_CTRL_EFUSE_SIZE - 1 || li + 1 > LOG_EFUSE_SIZE)
        return -1;
      log[li] = phy[pi];
      log[li + 1] = phy[pi + 1];
      pi += 2;
    }
  }
  return 0;
}

/* ---------- interface ---------- */

static int pci_cap(int bus, int dev, int fun, uint32_t id) {
  int cap = (int)(pci_rd(bus, dev, fun, 0x34) & 0xFCu), guard;
  for (guard = 0; cap && guard < 48; guard++) {
    uint32_t c = pci_rd(bus, dev, fun, cap);
    if ((c & 0xFFu) == id) return cap;
    cap = (int)((c >> 8) & 0xFCu);
  }
  return 0;
}

/* Desliga ASPM (L0s/L1) e o CLKREQ no Link Control. Com o enlace entrando
 * em L1 o DMA de RX travava (RXDMA_STUCK) uns 15-25 s depois de conectar,
 * mesmo parado; e o disable_aspm_l1 / disable_clkreq do rtw89_pci. */
static void aspm_off(int bus, int dev, int fun, const char* who) {
  int cap = pci_cap(bus, dev, fun, 0x10);
  uint32_t lc;
  if (!cap) return;
  lc = pci_rd(bus, dev, fun, cap + 0x10);
  rlog("[rtw] ASPM ");
  rlog(who);
  rlog_hex(" LNKCTL antes=", lc);
  pci_wr(bus, dev, fun, cap + 0x10, lc & 0xFFFFu & ~(3u | 0x100u));
  rlog_hex("[rtw]   agora=", pci_rd(bus, dev, fun, cap + 0x10));
}

/* Ponte PCIe cujo barramento secundario e o da placa. */
static void aspm_off_all(int bus, int dev, int fun) {
  int b, d, f;
  aspm_off(bus, dev, fun, "placa");
  for (b = 0; b < 256; b++)
    for (d = 0; d < 32; d++)
      for (f = 0; f < 8; f++) {
        uint32_t id = pci_rd(b, d, f, 0);
        if (id == 0xFFFFFFFFu || id == 0) {
          if (f == 0) break;
          continue;
        }
        if (((pci_rd(b, d, f, 0x0C) >> 16) & 0x7Fu) == 1 &&
            ((pci_rd(b, d, f, 0x18) >> 8) & 0xFFu) == (uint32_t)bus) {
          aspm_off(b, d, f, "ponte");
          return;
        }
      }
}

int rtw_probe(struct rtw_info* inf) {
  int bus, dev, fun;
  for (bus = 0; bus < 256; bus++) {
    for (dev = 0; dev < 32; dev++) {
      int nf = 1;
      for (fun = 0; fun < nf; fun++) {
        uint32_t id = pci_rd(bus, dev, fun, 0);
        uint32_t bar, hi, cmd;
        int cap, guard;
        if (id == 0xFFFFFFFFu || id == 0) continue;
        if (fun == 0 && (pci_rd(bus, dev, 0, 0x0C) & 0x00800000u)) nf = 8;
        if ((id & 0xFFFFu) != RTW_VID || (id >> 16) != RTW_DID_8852BE) continue;
        inf->bus = bus;
        inf->dev = dev;
        inf->fun = fun;
        inf->vid = (uint16_t)id;
        inf->did = (uint16_t)(id >> 16);
        /* D0: algum firmware deixa a placa em D3 */
        cap = (int)(pci_rd(bus, dev, fun, 0x34) & 0xFCu);
        for (guard = 0; cap && guard < 48; guard++) {
          uint32_t c = pci_rd(bus, dev, fun, cap);
          if ((c & 0xFFu) == 0x01) {
            uint32_t pm = pci_rd(bus, dev, fun, cap + 4);
            if (pm & 3u) {
              rlog_hex("[rtw] saindo de D", pm & 3u);
              pci_wr(bus, dev, fun, cap + 4, pm & ~3u);
              msleep(10);
            }
            break;
          }
          cap = (int)((c >> 8) & 0xFCu);
        }
        /* BAR2 (o rtw89 usa o 2; o 0 e de I/O) */
        bar = pci_rd(bus, dev, fun, 0x18);
        hi = ((bar & 6u) == 4u) ? pci_rd(bus, dev, fun, 0x1C) : 0;
        inf->bar = ((uint64_t)hi << 32) | (bar & ~0xFu);
        if (!inf->bar || (bar & 1u)) return fail("BAR2 vazio");
        cmd = pci_rd(bus, dev, fun, 0x04);
        /* memoria + bus master, INTx desligado (o driver faz polling) */
        pci_wr(bus, dev, fun, 0x04, (cmd & 0xFFFFu) | 0x406u);
        if (pci_mmio_map(inf->bar, RTW_BAR_LEN) != 0) return fail("BAR fora do alcance");
        aspm_off_all(bus, dev, fun);
        mmio = (volatile uint8_t*)(uintptr_t)inf->bar;
        rlog("[rtw] RTL8852BE em ");
        rtw_log_hex((uint32_t)((bus << 8) | (dev << 3) | fun));
        rlog_hex(" BAR2=", (uint32_t)inf->bar);
        return 0;
      }
    }
  }
  last_err = "placa RTL8852BE nao encontrada";
  return -1;
}

int rtw_power_on(struct rtw_info* inf) {
  static uint8_t phy[PHY_EFUSE_SIZE];
  static uint8_t log[LOG_EFUSE_SIZE];
  uint8_t v;
  int rc, i;
  if (!mmio) return fail("sem placa");
  if (r32(R_AX_SYS_CFG1) == 0xFFFFFFFFu) return fail("MMIO devolve FFFFFFFF");
  chip_cv = (int)get32(R_AX_SYS_CFG1, B_AX_CHIP_VER_MASK);
  inf->cv = chip_cv;
  inf->acv = -1;
  if (xtal_si_read(XTAL_SI_CV, &v) == 0)
    inf->acv = (int)((v & XTAL_SI_ACV_MASK) >> lowbit(XTAL_SI_ACV_MASK));
  rlog_hex("[rtw] cv=", (uint32_t)inf->cv);
  rlog_hex("[rtw] pwr_state=", r32(R_AX_IC_PWR_STATE));

  rc = power_switch(1);
  inf->was_on = rc == 1;
  if (rc != 0) {
    /* rtw89_mac_pwr_on: desliga e tenta de novo */
    rlog("[rtw] desligando antes de ligar\n");
    power_switch(0);
    if (power_switch(1) != 0) return fail(last_err[0] ? last_err : "power on");
  }
  rlog("[rtw] MAC ligada\n");

  inf->efuse_ok = (r16(R_AX_SYS_WL_EFUSE_CTRL) & B_AX_AUTOLOAD_SUS) != 0;
  if (efuse_dump(phy, PHY_EFUSE_SIZE) != 0) return -2;
  if (efuse_logical(phy, log) != 0) {
    fail("efuse logico invalido");
    return -3;
  }
  for (i = 0; i < 6; i++) inf->mac[i] = log[EF_MAC_PCIE + i];
  inf->rfe = log[EF_RFE];
  inf->xtal = log[EF_XTAL];
  rlog_hex("[rtw] rfe=", (uint32_t)inf->rfe);
  rlog("[rtw] mac ");
  for (i = 0; i < 6; i++) {
    rtw_log_hex(inf->mac[i]);
    rtw_log(" ");
  }
  rlog("\n");
  return 0;
}

void rtw_power_off(void) {
  if (mmio) power_switch(0);
}

int rtw_fw_pick(struct rtw_info* inf, const uint8_t* fw, uint32_t len) {
  int n, i, best = -1;
  if (len < 16) return fail("firmware curto");
  if (fw[0] != 0xFF) return fail("firmware sem cabecalho multi (sig FF)");
  n = fw[1];
  inf->fw_major = fw[4];
  inf->fw_minor = fw[5];
  inf->fw_sub = fw[6];
  inf->fw_idx = fw[7];
  if (16u + (uint32_t)n * 16u > len) return fail("firmware cortado");
  /* Normal (1) ou Normal CE (5), cv <= o do chip, o mais perto, sem MP. */
  for (i = 0; i < n; i++) {
    const uint8_t* e = fw + 16 + i * 16;
    if ((e[1] != 1 && e[1] != 5) || e[2] || e[0] > inf->cv) continue;
    if (best < 0 || fw[16 + best * 16] < e[0]) best = i;
  }
  if (best < 0) return fail("firmware sem versao para este cv");
  {
    const uint8_t* e = fw + 16 + best * 16;
    inf->fw_cv = e[0];
    inf->fw_type = e[1];
    inf->fw_off = e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
    inf->fw_size = e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
  }
  if (inf->fw_off > len || inf->fw_size > len - inf->fw_off) return fail("firmware cortado");
  return 0;
}

/* ---------- etapa 2: PCIe, DLE/HFC e envio do firmware ---------- */

/* Aneis de TX que o 8852BE usa (os outros ficam mascarados) e os dois de RX. */
#define TX_RING_N 32
/* 16 enchia em ~1,5 s de beacons enquanto um EXE carregava do pendrive. */
#define RX_RING_N 128
#define RX_BUF_SIZE 11494u
/* Distancia entre buffers: 11494 deixava os impares em endereco 2 mod 4 e o
 * DMA de RX travava (RXDMA_STUCK) sempre num indice impar. O Linux usa skb
 * alinhado; aqui cada buffer comeca em multiplo de 128. */
#define RX_BUF_STRIDE ((RX_BUF_SIZE + 127u) & ~127u)
#define CH12_BUF 2560u
#define TXWD_BODY 24
#define FWDL_PKT 2020u

struct txch {
  uint32_t num, idx, bdram, desa;
  uint8_t sidx, max, min;
};

static const struct txch txchs[] = {
  {R_AX_ACH0_TXBD_NUM, R_AX_ACH0_TXBD_IDX, R_AX_ACH0_BDRAM_CTRL, R_AX_ACH0_TXBD_DESA_L, 0, 5, 2},
  {R_AX_ACH1_TXBD_NUM, R_AX_ACH1_TXBD_IDX, R_AX_ACH1_BDRAM_CTRL, R_AX_ACH1_TXBD_DESA_L, 5, 5, 2},
  {R_AX_ACH2_TXBD_NUM, R_AX_ACH2_TXBD_IDX, R_AX_ACH2_BDRAM_CTRL, R_AX_ACH2_TXBD_DESA_L, 10, 5, 2},
  {R_AX_ACH3_TXBD_NUM, R_AX_ACH3_TXBD_IDX, R_AX_ACH3_BDRAM_CTRL, R_AX_ACH3_TXBD_DESA_L, 15, 5, 2},
  {R_AX_CH8_TXBD_NUM, R_AX_CH8_TXBD_IDX, R_AX_CH8_BDRAM_CTRL, R_AX_CH8_TXBD_DESA_L, 20, 4, 1},
  {R_AX_CH9_TXBD_NUM, R_AX_CH9_TXBD_IDX, R_AX_CH9_BDRAM_CTRL, R_AX_CH9_TXBD_DESA_L, 24, 4, 1},
  {R_AX_CH12_TXBD_NUM, R_AX_CH12_TXBD_IDX, R_AX_CH12_BDRAM_CTRL, R_AX_CH12_TXBD_DESA_L, 28, 4, 1},
};
#define TXCH_N 7
#define TXCH_FWCMD 6

static uint8_t* tx_bd[TXCH_N];  /* TX_RING_N BDs de 8 bytes */
static uint8_t* rx_bd[2];
static uint8_t* rx_buf[2];      /* RX_RING_N buffers de RX_BUF_SIZE, a cada RX_BUF_STRIDE */
static uint8_t* h2c_buf;        /* TX_RING_N buffers de CH12_BUF para o CH12 */
static uint32_t h2c_wp;
static int dma_ready;

static void put16(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t le32(const uint8_t* p) {
  return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void bzero(void* d, uint32_t n) {
  uint8_t* p = (uint8_t*)d;
  while (n--) *p++ = 0;
}

static void bcopy(void* d, const void* s, uint32_t n) {
  uint8_t* a = (uint8_t*)d;
  const uint8_t* b = (const uint8_t*)s;
  while (n--) *a++ = *b++;
}

/* Memoria para DMA: identidade, abaixo de 4 GB, alinhada. Nunca liberada. */
static uint8_t* dma_alloc(uint32_t n, uint32_t align) {
  uint8_t* raw = (uint8_t*)umalloc_owned(n + align, 0);
  uintptr_t a;
  if (!raw) return 0;
  a = ((uintptr_t)raw + align - 1u) & ~(uintptr_t)(align - 1u);
  if ((uint64_t)a + n > 0xFFFFFFFFull) return 0;
  bzero((void*)a, n);
  return (uint8_t*)a;
}

static uint32_t pa32(const void* p) { return (uint32_t)(uintptr_t)p; }

static int dma_mem(void) {
  int i, j;
  if (dma_ready) return 0;
  for (i = 0; i < TXCH_N; i++)
    if (!(tx_bd[i] = dma_alloc(TX_RING_N * 8u, 4096))) return -1;
  h2c_buf = dma_alloc(TX_RING_N * CH12_BUF, 4096);
  if (!h2c_buf) return -1;
  for (i = 0; i < 2; i++) {
    rx_bd[i] = dma_alloc(RX_RING_N * 8u, 4096);
    rx_buf[i] = dma_alloc(RX_RING_N * RX_BUF_STRIDE, 4096);
    if (!rx_bd[i] || !rx_buf[i]) return -1;
    for (j = 0; j < RX_RING_N; j++) {
      uint8_t* bd = rx_bd[i] + j * 8;
      put16(bd, RX_BUF_SIZE);
      put16(bd + 2, 0);
      put32(bd + 4, pa32(rx_buf[i] + (uint32_t)j * RX_BUF_STRIDE));
    }
  }
  dma_ready = 1;
  return 0;
}

/* rtw89_mac_dmac_pre_init: funcoes da DMAC, DLE no modo DLFW e HFC so do H2C. */
static int dmac_pre_init(void) {
  uint32_t v;
  set32(R_AX_HCI_FUNC_EN, B_AX_HCI_TXDMA_EN | B_AX_HCI_RXDMA_EN);
  w32(R_AX_DMAC_FUNC_EN, B_AX_MAC_FUNC_EN | B_AX_DMAC_FUNC_EN | B_AX_DISPATCHER_EN |
                         B_AX_PKT_BUF_EN);
  w32(R_AX_DMAC_CLK_EN, B_AX_DISPATCHER_CLK_EN);

  /* DLE (rtw8852b_dle_mem_pcie[DLFW]: wde_size9, ple_size8, wde_qt4, ple_qt13) */
  if ((r32(R_AX_DMAC_FUNC_EN) & (B_AX_MAC_FUNC_EN | B_AX_DMAC_FUNC_EN)) !=
      (B_AX_MAC_FUNC_EN | B_AX_DMAC_FUNC_EN))
    return fail("DMAC desligada");
  clr32(R_AX_DMAC_FUNC_EN, B_AX_DLE_WDE_EN | B_AX_DLE_PLE_EN);
  set32(R_AX_DMAC_CLK_EN, B_AX_DLE_WDE_CLK_EN | B_AX_DLE_PLE_CLK_EN);
  v = r32(R_AX_WDE_PKTBUF_CFG);
  v = (v & ~B_AX_WDE_PAGE_SEL_MASK) | field(B_AX_WDE_PAGE_SEL_MASK, S_AX_WDE_PAGE_SEL_64);
  v = (v & ~B_AX_WDE_START_BOUND_MASK) | field(B_AX_WDE_START_BOUND_MASK, 0);
  v = (v & ~B_AX_WDE_FREE_PAGE_NUM_MASK) | field(B_AX_WDE_FREE_PAGE_NUM_MASK, 0);
  w32(R_AX_WDE_PKTBUF_CFG, v);
  v = r32(R_AX_PLE_PKTBUF_CFG);
  v = (v & ~B_AX_PLE_PAGE_SEL_MASK) | field(B_AX_PLE_PAGE_SEL_MASK, S_AX_PLE_PAGE_SEL_128);
  /* (0 + 1024 paginas WDE) * 64 / 8 KB */
  v = (v & ~B_AX_PLE_START_BOUND_MASK) | field(B_AX_PLE_START_BOUND_MASK, 8);
  v = (v & ~B_AX_PLE_FREE_PAGE_NUM_MASK) | field(B_AX_PLE_FREE_PAGE_NUM_MASK, 64);
  w32(R_AX_PLE_PKTBUF_CFG, v);
#define QTA(reg, mod, mn, mx) \
  w32(reg, field(B_AX_##mod##_MIN_SIZE_MASK, mn) | field(B_AX_##mod##_MAX_SIZE_MASK, mx))
  QTA(R_AX_WDE_QTA0_CFG, WDE, 0, 0);
  QTA(R_AX_WDE_QTA1_CFG, WDE, 48, 0);  /* min do wcpu vem do modo SCC (wde_qt7) */
  QTA(R_AX_WDE_QTA3_CFG, WDE, 0, 0);
  QTA(R_AX_WDE_QTA4_CFG, WDE, 0, 0);
  QTA(R_AX_PLE_QTA0_CFG, PLE, 0, 0);
  QTA(R_AX_PLE_QTA1_CFG, PLE, 0, 0);
  QTA(R_AX_PLE_QTA2_CFG, PLE, 16, 16);  /* c2h */
  QTA(R_AX_PLE_QTA3_CFG, PLE, 48, 48);  /* h2c */
  QTA(R_AX_PLE_QTA4_CFG, PLE, 0, 0);
  QTA(R_AX_PLE_QTA5_CFG, PLE, 0, 0);
  QTA(R_AX_PLE_QTA6_CFG, PLE, 0, 0);
  QTA(R_AX_PLE_QTA7_CFG, PLE, 0, 0);
  QTA(R_AX_PLE_QTA8_CFG, PLE, 0, 0);
  QTA(R_AX_PLE_QTA9_CFG, PLE, 0, 0);
  QTA(R_AX_PLE_QTA10_CFG, PLE, 0, 0);
#undef QTA
  set32(R_AX_DMAC_FUNC_EN, B_AX_DLE_WDE_EN | B_AX_DLE_PLE_EN);
  if (poll32(R_AX_WDE_INI_STATUS, WDE_MGN_INI_RDY, 1, 10) ||
      (r32(R_AX_WDE_INI_STATUS) & WDE_MGN_INI_RDY) != WDE_MGN_INI_RDY)
    return fail("WDE nao ficou pronta");
  if (poll32(R_AX_PLE_INI_STATUS, PLE_MGN_INI_RDY, 1, 10) ||
      (r32(R_AX_PLE_INI_STATUS) & PLE_MGN_INI_RDY) != PLE_MGN_INI_RDY)
    return fail("PLE nao ficou pronta");

  /* HFC: so o canal do H2C (hfc_preccfg_pcie: h2c_prec 40, full_cond 0) */
  clr32(R_AX_HCI_FC_CTRL, B_AX_HCI_FC_EN | B_AX_HCI_FC_CH12_EN);
  w32(R_AX_CH_PAGE_CTRL, field(B_AX_PREC_PAGE_CH12_MASK, 40));
  mask32(R_AX_HCI_FC_CTRL, B_AX_HCI_FC_CH12_FULL_COND_MASK, 0);
  set32(R_AX_HCI_FC_CTRL, B_AX_HCI_FC_CH12_EN);
  return 0;
}

/* rtw89_pci_ops_mac_pre_init_ax, sem os ajustes de PHY por MDIO e a
 * calibracao de clock (o link ja vem treinado do firmware do PC). */
static int pci_pre_init(void) {
  uint32_t lbc;
  int i;
  clr32(R_AX_PCIE_PS_CTRL, B_AX_L1OFF_PWR_OFF_EN);
  clr32(R_AX_SYS_PW_CTRL, B_AX_PSUS_OFF_CAPC_EN);
  set32(R_AX_SYS_SDIO_CTRL, B_AX_PCIE_DIS_L2_CTRL_LDO_HCI);
  clr32(R_AX_SYS_SDIO_CTRL, B_AX_PCIE_DIS_WLSUS_AFT_PDN);
  set32(R_AX_HCI_OPT_CTRL, BIT_WAKE_CTRL);
  clr32(R_AX_PCIE_EXP_CTRL, B_AX_SIC_EN_FORCE_CLKREQ);
  lbc = r32(R_AX_LBC_WATCHDOG);
  lbc = (lbc & ~B_AX_LBC_TIMER) | field(B_AX_LBC_TIMER, MAC_AX_LBC_TMR_2MS);
  lbc |= B_AX_LBC_FLAG | B_AX_LBC_EN;
  w32(R_AX_LBC_WATCHDOG, lbc);
  set32(R_AX_LBC_WATCHDOG, lbc);
  set32(R_AX_PCIE_DBG_CTRL, B_AX_ASFF_FULL_NO_STK | B_AX_EN_STUCK_DBG);
  set32(R_AX_PCIE_INIT_CFG1, B_AX_PCIE_TXRST_KEEP_REG | B_AX_PCIE_RXRST_KEEP_REG);

  set32(R_AX_PCIE_DMA_STOP1, B_AX_STOP_WPDMA);
  /* para o DMA */
  set32(R_AX_PCIE_DMA_STOP1, B_AX_STOP_PCIEIO);
  clr32(R_AX_PCIE_INIT_CFG1, B_AX_RXHCI_EN | B_AX_TXHCI_EN);
  if (poll32(R_AX_PCIE_DMA_BUSY1, DMA_BUSY1_CHECK_V1, 0, 10)) return fail("TX DMA ocupado");
  if (poll32(R_AX_PCIE_DMA_BUSY1, B_AX_RXQ_BUSY | B_AX_RPQ_BUSY, 0, 10))
    return fail("RX DMA ocupado");

  set32(R_AX_TXBD_RWPTR_CLR1, B_AX_CLR_ACH0_IDX | B_AX_CLR_ACH1_IDX | B_AX_CLR_ACH2_IDX |
                              B_AX_CLR_ACH3_IDX | B_AX_CLR_CH8_IDX | B_AX_CLR_CH9_IDX |
                              B_AX_CLR_CH12_IDX);
  set32(R_AX_RXBD_RWPTR_CLR, B_AX_CLR_RXQ_IDX | B_AX_CLR_RPQ_IDX);

  /* rtw89_pci_mode_op: BD truncado, RXBD por pacote, varias tags */
  clr32(R_AX_PCIE_INIT_CFG1, B_AX_RXBD_MODE);
  mask32(R_AX_PCIE_INIT_CFG1, B_AX_PCIE_MAX_TXDMA_MASK, MAC_AX_TX_BURST_2048B);
  mask32(R_AX_PCIE_INIT_CFG1, B_AX_PCIE_MAX_RXDMA_MASK, MAC_AX_RX_BURST_128B);
  set32(R_AX_PCIE_INIT_CFG1, B_AX_LATENCY_CONTROL);
  mask32(R_AX_PCIE_EXP_CTRL, B_AX_MAX_TAG_NUM, MAC_AX_TAG_NUM_8);
  mask32(R_AX_PCIE_INIT_CFG2, B_AX_WD_ITVL_IDLE, MAC_AX_WD_DMA_INTVL_256NS);
  mask32(R_AX_PCIE_INIT_CFG2, B_AX_WD_ITVL_ACT, MAC_AX_WD_DMA_INTVL_256NS);
  set32(R_AX_TX_ADDRESS_INFO_MODE_SETTING, B_AX_HOST_ADDR_INFO_8B_SEL);
  clr32(R_AX_PKTIN_SETTING, B_AX_WD_ADDR_INFO_LENGTH);

  /* rtw89_pci_reset_trx_rings */
  for (i = 0; i < TXCH_N; i++) {
    const struct txch* c = &txchs[i];
    bzero(tx_bd[i], TX_RING_N * 8u);
    w16(c->num, TX_RING_N);
    w32(c->bdram, field(BDRAM_SIDX_MASK, c->sidx) | field(BDRAM_MAX_MASK, c->max) |
                  field(BDRAM_MIN_MASK, c->min));
    w32(c->desa, pa32(tx_bd[i]));
    w32(c->desa + 4, 0);
  }
  h2c_wp = 0;
  w16(R_AX_RXQ_RXBD_NUM, RX_RING_N);
  w32(R_AX_RXQ_RXBD_DESA_L, pa32(rx_bd[0]));
  w32(R_AX_RXQ_RXBD_DESA_L + 4, 0);
  w16(R_AX_RPQ_RXBD_NUM, RX_RING_N);
  w32(R_AX_RPQ_RXBD_DESA_L, pa32(rx_bd[1]));
  w32(R_AX_RPQ_RXBD_DESA_L + 4, 0);

  set32(R_AX_PCIE_INIT_CFG1, B_AX_RST_BDRAM);
  if (poll32(R_AX_PCIE_INIT_CFG1, B_AX_RST_BDRAM, 0, 10)) return fail("BDRAM nao resetou");

  /* so o canal do firmware (CH12) liga agora */
  set32(R_AX_PCIE_DMA_STOP1, B_AX_TX_STOP1_MASK_V1);
  clr32(R_AX_PCIE_DMA_STOP1, B_AX_STOP_CH12);
  clr32(R_AX_PCIE_DMA_STOP1, B_AX_STOP_PCIEIO);
  set32(R_AX_PCIE_INIT_CFG1, B_AX_RXHCI_EN | B_AX_TXHCI_EN);
  return 0;
}

/* Um pacote no CH12: TXWD de 24 bytes + dados. fwdl = pedaco de firmware. */
static int h2c_send(const uint8_t* data, uint32_t len, int fwdl) {
  uint8_t* buf;
  uint8_t* bd;
  uint32_t next = (h2c_wp + 1u) % TX_RING_N;
  uint32_t t0 = net_ms(), spins = 0;
  uint32_t seq;
  if (len + TXWD_BODY > CH12_BUF) return fail("H2C grande demais");
  /* espera a placa liberar o slot (indice de leitura do HW nos bits 27:16) */
  while (((r32(R_AX_CH12_TXBD_IDX) >> 16) & 0xFFFu) == next)
    if (late(t0, &spins, 100)) return fail("CH12 cheio (a placa nao le o anel)");
  buf = h2c_buf + h2c_wp * CH12_BUF;
  bzero(buf, TXWD_BODY);
  /* rtw89_core_fill_txdesc para FWCMD: CH 12, FW_DL, tamanho; o Linux le um
   * "seq" de bytes do proprio pacote (como se fosse cabecalho 802.11). */
  seq = len >= 24 ? (((uint32_t)data[22] | ((uint32_t)data[23] << 8)) & 0xFFF0u) >> 4 : 0;
  put32(buf, (12u << 16) | (fwdl ? (1u << 20) : 0));
  put32(buf + 8, len & 0x3FFFu);
  put32(buf + 12, seq & 0xFFFu);
  bcopy(buf + TXWD_BODY, data, len);
  bd = tx_bd[TXCH_FWCMD] + h2c_wp * 8u;
  put16(bd, len + TXWD_BODY);
  put16(bd + 2, 1u << 14);  /* LS; DMA_HI = 0 (abaixo de 4 GB) */
  put32(bd + 4, pa32(buf));
  __asm__ volatile ("mfence" ::: "memory");
  h2c_wp = next;
  w16(R_AX_CH12_TXBD_IDX, (uint16_t)h2c_wp);
  return 0;
}

static void fw_pc_dump(void) {
  int i;
  w32(R_AX_DBG_CTRL, field(B_AX_DBG_SEL0, FW_PROG_CNTR_DBG_SEL) |
                     field(B_AX_DBG_SEL1, FW_PROG_CNTR_DBG_SEL));
  mask32(R_AX_SYS_STATUS1, B_AX_SEL_0XC0_MASK, MAC_DBG_SEL);
  for (i = 0; i < 4; i++) rlog_hex("[rtw] fw PC=", r32(R_AX_DBG_PORT_SEL));
}

static int fw_status(void) {
  return (int)((r8(R_AX_WCPU_FW_CTRL) & B_AX_WCPU_FWDL_STS_MASK) >> lowbit(B_AX_WCPU_FWDL_STS_MASK));
}

/* __rtw89_fw_download para o 8852B (cabecalho v0, sem BB MCU). */
static int fw_download_once(const uint8_t* fw, uint32_t size) {
  uint32_t w3, w6, w7, base, hdr_len, n, i, off;
  uint8_t hdr[512];
  uint32_t t0, spins;
  int st;

  /* disable_cpu */
  clr32(R_AX_PLATFORM_ENABLE, B_AX_WCPU_EN);
  clr32(R_AX_WCPU_FW_CTRL, B_AX_WCPU_FWDL_EN | B_AX_H2C_PATH_RDY | B_AX_FWDL_PATH_RDY);
  clr32(R_AX_SYS_CLK_CTRL, B_AX_CPU_CLK_EN);
  clr32(R_AX_PLATFORM_ENABLE, B_AX_APB_WRAP_EN);
  set32(R_AX_PLATFORM_ENABLE, B_AX_APB_WRAP_EN);
  clr32(R_AX_PLATFORM_ENABLE, B_AX_PLATFORM_EN);
  set32(R_AX_PLATFORM_ENABLE, B_AX_PLATFORM_EN);

  /* enable_cpu(boot_reason 0, dlfw) */
  if (r32(R_AX_PLATFORM_ENABLE) & B_AX_WCPU_EN) return fail("WCPU ja ligada");
  w32(R_AX_UDM1, 0);
  w32(R_AX_UDM2, 0);
  w32(R_AX_HALT_H2C_CTRL, 0);
  w32(R_AX_HALT_C2H_CTRL, 0);
  w32(R_AX_HALT_H2C, 0);
  w32(R_AX_HALT_C2H, 0);
  set32(R_AX_SYS_CLK_CTRL, B_AX_CPU_CLK_EN);
  {
    uint32_t v = r32(R_AX_WCPU_FW_CTRL);
    v &= ~(B_AX_WCPU_FWDL_EN | B_AX_H2C_PATH_RDY | B_AX_FWDL_PATH_RDY | B_AX_WCPU_FWDL_STS_MASK);
    v |= B_AX_WCPU_FWDL_EN;
    w32(R_AX_WCPU_FW_CTRL, v);
  }
  mask32(R_AX_SEC_CTRL, B_AX_SEC_IDMEM_SIZE_CONFIG_MASK, 0x2);
  mask16(R_AX_BOOT_REASON, (uint16_t)B_AX_BOOT_REASON_MASK, 0);
  set32(R_AX_PLATFORM_ENABLE, B_AX_WCPU_EN);

  /* cabecalho (rtw89_fw_hdr_parser_v0) */
  w3 = le32(fw + 12);
  w6 = le32(fw + 24);
  w7 = le32(fw + 28);
  if ((w3 >> 24) != 0) return fail("firmware com cabecalho v1 (nao suportado)");
  n = (w6 >> 8) & 0xFFu;
  base = 32u + 16u * n;
  hdr_len = (w7 & (1u << 16)) ? ((w3 >> 16) & 0xFFu) : base;
  if (n == 0 || n > 8 || base > sizeof(hdr) - 8 || hdr_len > size) return fail("cabecalho do firmware invalido");

  if (poll32(R_AX_WCPU_FW_CTRL, B_AX_H2C_PATH_RDY, 1, 400)) return fail("H2C path nao ficou pronto");

  /* H2C com o cabecalho (sem a parte dinamica), PART_SIZE = 2020 */
  bcopy(hdr + 8, fw, base);
  put32(hdr + 8 + 28, (w7 & ~0xFFFFu) | FWDL_PKT);
  put32(hdr, (0u << 16) | 1u | (3u << 2) | (0u << 8) | (0u << 24));  /* H2C MAC FWDL FWHDR_DL */
  put32(hdr + 4, (base + 8u) & 0x3FFFu);
  if (h2c_send(hdr, base + 8u, 0)) return -1;
  if (poll32(R_AX_WCPU_FW_CTRL, B_AX_FWDL_PATH_RDY, 1, 400)) return fail("FWDL path nao ficou pronto");
  w32(R_AX_HALT_H2C_CTRL, 0);
  w32(R_AX_HALT_C2H_CTRL, 0);
  rlog_hex("[rtw] cabecalho aceito, secoes=", n);

  /* secoes, em pedacos de 2020 bytes */
  off = hdr_len;
  for (i = 0; i < n; i++) {
    uint32_t sw1 = le32(fw + 32 + 16 * i + 4);
    uint32_t len = sw1 & 0xFFFFFFu;
    if (sw1 & (1u << 28)) len += 8;
    if (off + len > size) return fail("secao do firmware fora do arquivo");
    rlog_hex("[rtw] secao bytes=", len);
    while (len) {
      uint32_t k = len > FWDL_PKT ? FWDL_PKT : len;
      if (h2c_send(fw + off, k, 1)) return -1;
      off += k;
      len -= k;
    }
  }

  /* espera o firmware subir (FWDL_STS = 7) */
  msleep(5);
  t0 = net_ms();
  spins = 0;
  while ((st = fw_status()) != 7)
    if (late(t0, &spins, 400)) break;
  rlog_hex("[rtw] FWDL_STS=", (uint32_t)st);
  if (st == 7) return 0;
  fw_pc_dump();
  if (st == 2) return fail("firmware: checksum errado");
  if (st == 3) return fail("firmware: falha de seguranca");
  if (st == 4) return fail("firmware: cv nao confere");
  return fail("firmware nao subiu");
}

int rtw_fw_load(struct rtw_info* inf, const uint8_t* file) {
  int tries, rc = -1;
  if (!mmio) return fail("sem placa");
  if (dma_mem() != 0) return fail("sem memoria para DMA");
  if (dmac_pre_init() != 0) return -1;
  if (pci_pre_init() != 0) return -1;
  rlog("[rtw] DLE/HFC e aneis PCIe prontos\n");
  for (tries = 0; tries < 5 && rc != 0; tries++) {
    rc = fw_download_once(file + inf->fw_off, inf->fw_size);
    if (rc) rlog_hex("[rtw] tentativa falhou: ", (uint32_t)tries + 1u);
  }
  if (rc == 0) rlog("[rtw] firmware rodando\n");
  return rc;
}

/* ---------- etapa 3: resto da MAC, RX e o primeiro H2C com resposta ---------- */

static uint32_t rx_wp[2];  /* proximo buffer a ler em cada anel (RXQ, RPQ) */
static int rx_held = -1;   /* anel cujo buffer o chamador ainda esta lendo */
static uint32_t rx_maxfill[2], rx_last_ms[2];  /* para o log de travamento */
static uint8_t h2c_seq;

/* rtw8852bx_mac_enable_bb_rf */
static int enable_bb_rf(void) {
  set8(R_AX_SYS_FUNC_EN, (uint8_t)(B_AX_FEN_BBRSTB | B_AX_FEN_BB_GLB_RSTN));
  mask32(R_AX_SPS_DIG_ON_CTRL0, B_AX_REG_ZCDC_H_MASK, 0x1);
  set32(R_AX_WLRF_CTRL, B_AX_AFC_AFEDIG);
  clr32(R_AX_WLRF_CTRL, B_AX_AFC_AFEDIG);
  set32(R_AX_WLRF_CTRL, B_AX_AFC_AFEDIG);
  if (xtal_si_write(XTAL_SI_WL_RFC_S0, 0xC7, 0xFF) || xtal_si_write(XTAL_SI_WL_RFC_S1, 0xC7, 0xFF))
    return fail("xtal si (enable_bb_rf)");
  w8(R_AX_PHYREG_SET, PHYREG_SET_XYN_CYCLE);
  return 0;
}

/* sys_init_ax: dmac_func_en, cmac_func_en(0), chip_func_en */
static void sys_init(void) {
  w32(R_AX_DMAC_FUNC_EN, B_AX_MAC_FUNC_EN | B_AX_DMAC_FUNC_EN | B_AX_MAC_SEC_EN |
                         B_AX_DISPATCHER_EN | B_AX_DLE_CPUIO_EN | B_AX_PKT_IN_EN |
                         B_AX_DMAC_TBL_EN | B_AX_PKT_BUF_EN | B_AX_STA_SCH_EN |
                         B_AX_TXPKT_CTRL_EN | B_AX_WD_RLS_EN | B_AX_MPDU_PROC_EN |
                         B_AX_DMAC_CRPRT);
  w32(R_AX_DMAC_CLK_EN, B_AX_MAC_SEC_CLK_EN | B_AX_DISPATCHER_CLK_EN | B_AX_DLE_CPUIO_CLK_EN |
                        B_AX_PKT_IN_CLK_EN | B_AX_STA_SCH_CLK_EN | B_AX_TXPKT_CTRL_CLK_EN |
                        B_AX_WD_RLS_CLK_EN | B_AX_BBRPT_CLK_EN);
  set32(R_AX_CK_EN, B_AX_CMAC_CKEN | B_AX_PHYINTF_CKEN | B_AX_CMAC_DMA_CKEN |
                    B_AX_PTCLTOP_CKEN | B_AX_SCHEDULER_CKEN | B_AX_TMAC_CKEN | B_AX_RMAC_CKEN);
  set32(R_AX_CMAC_FUNC_EN, B_AX_CMAC_EN | B_AX_CMAC_TXEN | B_AX_CMAC_RXEN | B_AX_PHYINTF_EN |
                           B_AX_CMAC_DMA_EN | B_AX_PTCLTOP_EN | B_AX_SCHEDULER_EN |
                           B_AX_TMAC_EN | B_AX_RMAC_EN | B_AX_CMAC_CRPRT);
  set32(R_AX_SPS_DIG_ON_CTRL0, B_AX_OCP_L1_MASK);
}

/* DLE no modo SCC (rtw8852b_dle_mem_pcie[SCC]: wde_size7, ple_size6, wde_qt7,
 * ple_qt18 minimo e ple_qt58 maximo). */
static int dle_init_scc(void) {
  static const uint16_t ple_min[11] = {147, 0, 16, 20, 17, 13, 89, 0, 32, 14, 8};
  static const uint16_t ple_max[11] = {147, 0, 16, 20, 157, 13, 229, 0, 172, 14, 24};
  static const uint32_t ple_reg[11] = {
    R_AX_PLE_QTA0_CFG, R_AX_PLE_QTA1_CFG, R_AX_PLE_QTA2_CFG, R_AX_PLE_QTA3_CFG,
    R_AX_PLE_QTA4_CFG, R_AX_PLE_QTA5_CFG, R_AX_PLE_QTA6_CFG, R_AX_PLE_QTA7_CFG,
    R_AX_PLE_QTA8_CFG, R_AX_PLE_QTA9_CFG, R_AX_PLE_QTA10_CFG};
  uint32_t v;
  int i;
  clr32(R_AX_DMAC_FUNC_EN, B_AX_DLE_WDE_EN | B_AX_DLE_PLE_EN);
  set32(R_AX_DMAC_CLK_EN, B_AX_DLE_WDE_CLK_EN | B_AX_DLE_PLE_CLK_EN);
  v = r32(R_AX_WDE_PKTBUF_CFG);
  v = (v & ~B_AX_WDE_PAGE_SEL_MASK) | field(B_AX_WDE_PAGE_SEL_MASK, S_AX_WDE_PAGE_SEL_64);
  v = (v & ~B_AX_WDE_START_BOUND_MASK) | field(B_AX_WDE_START_BOUND_MASK, 0);
  v = (v & ~B_AX_WDE_FREE_PAGE_NUM_MASK) | field(B_AX_WDE_FREE_PAGE_NUM_MASK, 510);
  w32(R_AX_WDE_PKTBUF_CFG, v);
  v = r32(R_AX_PLE_PKTBUF_CFG);
  v = (v & ~B_AX_PLE_PAGE_SEL_MASK) | field(B_AX_PLE_PAGE_SEL_MASK, S_AX_PLE_PAGE_SEL_128);
  /* (510 + 2 paginas WDE) * 64 / 8 KB */
  v = (v & ~B_AX_PLE_START_BOUND_MASK) | field(B_AX_PLE_START_BOUND_MASK, 4);
  v = (v & ~B_AX_PLE_FREE_PAGE_NUM_MASK) | field(B_AX_PLE_FREE_PAGE_NUM_MASK, 496);
  w32(R_AX_PLE_PKTBUF_CFG, v);
#define WQ(reg, mn, mx) w32(reg, field(B_AX_WDE_MIN_SIZE_MASK, mn) | field(B_AX_WDE_MAX_SIZE_MASK, mx))
  WQ(R_AX_WDE_QTA0_CFG, 446, 446);
  WQ(R_AX_WDE_QTA1_CFG, 48, 48);
  WQ(R_AX_WDE_QTA3_CFG, 0, 0);
  WQ(R_AX_WDE_QTA4_CFG, 16, 16);
#undef WQ
  for (i = 0; i < 11; i++)
    w32(ple_reg[i], field(B_AX_PLE_MIN_SIZE_MASK, ple_min[i]) | field(B_AX_PLE_MAX_SIZE_MASK, ple_max[i]));
  set32(R_AX_DMAC_FUNC_EN, B_AX_DLE_WDE_EN | B_AX_DLE_PLE_EN);
  if (poll32(R_AX_WDE_INI_STATUS, WDE_MGN_INI_RDY, 1, 10) ||
      (r32(R_AX_WDE_INI_STATUS) & WDE_MGN_INI_RDY) != WDE_MGN_INI_RDY)
    return fail("WDE (SCC) nao ficou pronta");
  if (poll32(R_AX_PLE_INI_STATUS, PLE_MGN_INI_RDY, 1, 10) ||
      (r32(R_AX_PLE_INI_STATUS) & PLE_MGN_INI_RDY) != PLE_MGN_INI_RDY)
    return fail("PLE (SCC) nao ficou pronta");
  return 0;
}

/* HFC completo (rtw8852b_hfc_chcfg_pcie, pubcfg {446, 0, 446, 0}, modo POH). */
static void hfc_init_full(void) {
  static const uint16_t ch_min[12] = {5, 5, 4, 4, 0, 0, 0, 0, 4, 4, 0, 0};
  static const uint16_t ch_max[12] = {341, 341, 342, 342, 0, 0, 0, 0, 342, 342, 0, 0};
  uint32_t v;
  int ch;
  clr32(R_AX_HCI_FC_CTRL, B_AX_HCI_FC_EN | B_AX_HCI_FC_CH12_EN);
  for (ch = 0; ch < 12; ch++) {
    if ((ch >= 4 && ch <= 7) || ch >= 10) continue;  /* dma_ch_mask do 8852B */
    w32(R_AX_ACH0_PAGE_CTRL + (uint32_t)ch * 4u,
        field(B_AX_MIN_PG_MASK, ch_min[ch]) | field(B_AX_MAX_PG_MASK, ch_max[ch]));
  }
  w32(R_AX_PUB_PAGE_CTRL1, field(B_AX_PUBPG_G0_MASK, 446) | field(B_AX_PUBPG_G1_MASK, 0));
  w32(R_AX_WP_PAGE_CTRL2, field(B_AX_WP_THRD_MASK, 0));
  /* hfc_mix_cfg_ax com hfc_preccfg_pcie {2, 40, 0, 0, 1, 0, 0, 0} */
  w32(R_AX_CH_PAGE_CTRL, field(B_AX_PREC_PAGE_CH011_MASK, 2) | field(B_AX_PREC_PAGE_CH12_MASK, 40));
  w32(R_AX_PUB_PAGE_CTRL2, field(B_AX_PUBPG_ALL_MASK, 446));
  w32(R_AX_WP_PAGE_CTRL1, 0);
  v = r32(R_AX_HCI_FC_CTRL);
  v = (v & ~B_AX_HCI_FC_MODE_MASK) | field(B_AX_HCI_FC_MODE_MASK, 0);
  v = (v & ~B_AX_HCI_FC_WD_FULL_COND_MASK) | field(B_AX_HCI_FC_WD_FULL_COND_MASK, 1);
  v &= ~(B_AX_HCI_FC_CH12_FULL_COND_MASK | B_AX_HCI_FC_WP_CH07_FULL_COND_MASK |
         B_AX_HCI_FC_WP_CH811_FULL_COND_MASK);
  w32(R_AX_HCI_FC_CTRL, v);
  set32(R_AX_HCI_FC_CTRL, B_AX_HCI_FC_EN | B_AX_HCI_FC_CH12_EN);
}

/* dmac_init_ax: DLE, HFC, agendador de estacoes, MPDU e seguranca. */
static int dmac_init(void) {
  uint32_t v;
  if (dle_init_scc()) return -1;
  hfc_init_full();
  set8(R_AX_SS_CTRL, (uint8_t)B_AX_SS_EN);
  if (poll32(R_AX_SS_CTRL, B_AX_SS_INIT_DONE_1, 1, 10)) return fail("agendador de estacoes");
  set32(R_AX_SS_CTRL, B_AX_SS_WARM_INIT_FLG);
  clr32(R_AX_SS_CTRL, B_AX_SS_NONEMPTY_SS2FINFO_EN);
  w32(R_AX_ACTION_FWD0, TRXCFG_MPDU_PROC_ACT_FRWD);
  w32(R_AX_TF_FWD, TRXCFG_MPDU_PROC_TF_FRWD);
  set32(R_AX_MPDU_PROC, B_AX_APPEND_FCS | B_AX_A_ICV_ERR);
  w32(R_AX_CUT_AMSDU_CTRL, TRXCFG_MPDU_PROC_CUT_CTRL);
  v = r32(R_AX_SEC_ENG_CTRL);
  v |= B_AX_CLK_EN_CGCMP | B_AX_CLK_EN_WAPI | B_AX_CLK_EN_WEP_TKIP | B_AX_SEC_TX_ENC |
       B_AX_SEC_RX_DEC | B_AX_MC_DEC | B_AX_BC_DEC;
  v &= ~B_AX_TX_PARTIAL_MODE;
  w32(R_AX_SEC_ENG_CTRL, v);
  set32(R_AX_SEC_MPDU_PROC, B_AX_APPEND_ICV | B_AX_APPEND_MIC);
  return 0;
}

/* cmac_init_ax para a CMAC 0. */
static int cmac_init(void) {
  uint32_t v;
  uint16_t h;
  /* scheduler */
  mask32(R_AX_PREBKF_CFG_1, B_AX_SIFS_MACTXEN_T1_MASK, SIFS_MACTXEN_T1);
  set32(R_AX_SCH_EXT_CTRL, B_AX_PORT_RST_TSF_ADV);
  clr32(R_AX_CCA_CFG_0, B_AX_BTCCA_EN);
  mask32(R_AX_PREBKF_CFG_0, B_AX_PREBKF_TIME_MASK, SCH_PREBKF_24US);
  /* addr cam */
  set32(R_AX_ADDR_CAM_CTRL, field(B_AX_ADDR_CAM_RANGE_MASK, 0x7F) | B_AX_ADDR_CAM_CLR | B_AX_ADDR_CAM_EN);
  if (poll32(R_AX_ADDR_CAM_CTRL, B_AX_ADDR_CAM_CLR, 0, 10)) return fail("ADDR_CAM nao limpou");
  /* rx filter: tudo para o host; filtro padrao do rtw89 */
  w32(R_AX_MGNT_FLTR, RX_FLTR_FRAME_TO_HOST);
  w32(R_AX_CTRL_FLTR, RX_FLTR_FRAME_TO_HOST);
  w32(R_AX_DATA_FLTR, RX_FLTR_FRAME_TO_HOST);
  w32(R_AX_RX_FLTR_OPT, B_AX_A_A1_MATCH | B_AX_A_BC | B_AX_A_MC | B_AX_A_UC_CAM_MATCH |
                        B_AX_A_BC_CAM_MATCH | B_AX_A_PWR_MGNT | B_AX_A_FTM_REQ |
                        field(B_AX_UID_FILTER_MASK, 3) | B_AX_A_BCN_CHK_EN);
  w16(R_AX_PLCP_HDR_FLTR, (uint16_t)(B_AX_CCK_CRC_CHK | B_AX_CCK_SIG_CHK | B_AX_LSIG_PARITY_CHK_EN |
                                     B_AX_SIGA_CRC_CHK | B_AX_VHT_SU_SIGB_CRC_CHK |
                                     B_AX_VHT_MU_SIGB_CRC_CHK | B_AX_HE_SIGB_CRC_CHK));
  /* cca */
  v = r32(R_AX_CCA_CONTROL);
  v |= B_AX_TB_CHK_BASIC_NAV | B_AX_TB_CHK_BTCCA | B_AX_TB_CHK_EDCCA | B_AX_TB_CHK_CCA_P20 |
       B_AX_SIFS_CHK_BTCCA | B_AX_SIFS_CHK_CCA_P20 | B_AX_CTN_CHK_INTRA_NAV |
       B_AX_CTN_CHK_BASIC_NAV | B_AX_CTN_CHK_BTCCA | B_AX_CTN_CHK_EDCCA |
       B_AX_CTN_CHK_CCA_S80 | B_AX_CTN_CHK_CCA_S40 | B_AX_CTN_CHK_CCA_S20 | B_AX_CTN_CHK_CCA_P20;
  v &= ~(B_AX_TB_CHK_TX_NAV | B_AX_TB_CHK_CCA_S80 | B_AX_TB_CHK_CCA_S40 | B_AX_TB_CHK_CCA_S20 |
         B_AX_SIFS_CHK_CCA_S80 | B_AX_SIFS_CHK_CCA_S40 | B_AX_SIFS_CHK_CCA_S20 |
         B_AX_CTN_CHK_TXNAV | B_AX_SIFS_CHK_EDCCA);
  w32(R_AX_CCA_CONTROL, v);
  clr32(R_AX_RSP_CHK_SIG, B_AX_RSP_CHK_TXNAV | B_AX_RSP_CHK_INTRA_NAV | B_AX_RSP_CHK_BASIC_NAV);
  clr32(R_AX_TRXPTCL_RESP_0, B_AX_RSP_CHK_SEC_CCA_80 | B_AX_RSP_CHK_SEC_CCA_40 |
                             B_AX_RSP_CHK_SEC_CCA_20 | B_AX_RSP_CHK_BTCCA |
                             B_AX_RSP_CHK_EDCCA | B_AX_RSP_CHK_CCA);
  /* nav */
  set32(R_AX_WMAC_NAV_CTL, B_AX_WMAC_PLCP_UP_NAV_EN | B_AX_WMAC_TF_UP_NAV_EN | B_AX_WMAC_NAV_UPPER_EN);
  mask32(R_AX_WMAC_NAV_CTL, B_AX_WMAC_NAV_UPPER_MASK, NAV_25MS);
  /* spatial reuse */
  clr8(R_AX_RX_SR_CTRL, (uint8_t)B_AX_SR_EN);
  set8(R_AX_BSSID_SRC_CTRL, (uint8_t)B_AX_PLCP_SRC_EN);
  /* tmac */
  clr32(R_AX_MAC_LOOPBACK, B_AX_MACLBK_EN);
  mask32(R_AX_TCR0, B_AX_TCR_UDF_THSD_MASK, TCR_UDF_THSD);
  mask32(R_AX_TXD_FIFO_CTRL, B_AX_TXDFIFO_HIGH_MCS_THRE_MASK, TXDFIFO_HIGH_MCS_THRE);
  mask32(R_AX_TXD_FIFO_CTRL, B_AX_TXDFIFO_LOW_MCS_THRE_MASK, TXDFIFO_LOW_MCS_THRE);
  /* trxptcl */
  v = r32(R_AX_TRXPTCL_RESP_0);
  v = (v & ~B_AX_WMAC_SPEC_SIFS_CCK_MASK) | field(B_AX_WMAC_SPEC_SIFS_CCK_MASK, WMAC_SPEC_SIFS_CCK);
  v = (v & ~B_AX_WMAC_SPEC_SIFS_OFDM_MASK) | field(B_AX_WMAC_SPEC_SIFS_OFDM_MASK, WMAC_SPEC_SIFS_OFDM_52B);
  w32(R_AX_TRXPTCL_RESP_0, v);
  set32(R_AX_RXTRIG_TEST_USER_2, B_AX_RXTRIG_FCSCHK_EN);
  mask32(R_AX_TRXPTCL_RRSR_CTL_0, B_AX_WMAC_RESP_REF_RATE_SEL, 0);
  mask32(R_AX_TRXPTCL_RRSR_CTL_0, B_AX_WMAC_RESP_RSC_MASK, 2);
  /* rmac */
  mask32(R_AX_RESPBA_CAM_CTRL, B_AX_BACAM_RST_MASK, S_AX_BACAM_RST_ALL);
  if (poll32(R_AX_RESPBA_CAM_CTRL, B_AX_BACAM_RST_MASK, 0, 10)) rlog("[rtw] BA CAM nao resetou\n");
  set8(R_AX_RESPBA_CAM_CTRL, (uint8_t)B_AX_SSN_SEL);
  h = r16(R_AX_DLK_PROTECT_CTL);
  h = (uint16_t)((h & ~B_AX_RX_DLK_DATA_TIME_MASK) | field(B_AX_RX_DLK_DATA_TIME_MASK, 15));
  h = (uint16_t)((h & ~B_AX_RX_DLK_CCA_TIME_MASK) | field(B_AX_RX_DLK_CCA_TIME_MASK, 32));
  w16(R_AX_DLK_PROTECT_CTL, h);
  w8(R_AX_RCR, (uint8_t)((r8(R_AX_RCR) & ~B_AX_CH_EN_MASK) | field(B_AX_CH_EN_MASK, 1)));
  /* cma0_dma 89 paginas de 128 bytes = 11392, em unidades de 512 */
  mask32(R_AX_RX_FLTR_OPT, B_AX_RX_MPDU_MAX_LEN_MASK, 22);
  clr8(R_AX_PLCP_HDR_FLTR, (uint8_t)B_AX_VHT_SU_SIGB_CRC_CHK);
  /* cmac_com */
  v = r32(R_AX_TX_SUB_CARRIER_VALUE);
  v &= ~(B_AX_TXSC_20M_MASK | B_AX_TXSC_40M_MASK | B_AX_TXSC_80M_MASK);
  w32(R_AX_TX_SUB_CARRIER_VALUE, v);
  mask32(R_AX_PTCL_RRSR1, B_AX_RRSR_RATE_EN_MASK, RRSR_OFDM_CCK_EN);
  /* ptcl */
  v = r32(R_AX_SIFS_SETTING);
  v = (v & ~B_AX_HW_CTS2SELF_PKT_LEN_TH_MASK) | field(B_AX_HW_CTS2SELF_PKT_LEN_TH_MASK, S_AX_CTS2S_TH_1K);
  v = (v & ~B_AX_HW_CTS2SELF_PKT_LEN_TH_TWW_MASK) |
      field(B_AX_HW_CTS2SELF_PKT_LEN_TH_TWW_MASK, S_AX_CTS2S_TH_SEC_256B);
  w32(R_AX_SIFS_SETTING, v | B_AX_HW_CTS2SELF_EN);
  v = r32(R_AX_PTCL_FSM_MON);
  v = (v & ~B_AX_PTCL_TX_ARB_TO_THR_MASK) | field(B_AX_PTCL_TX_ARB_TO_THR_MASK, S_AX_PTCL_TO_2MS);
  w32(R_AX_PTCL_FSM_MON, v & ~B_AX_PTCL_TX_ARB_TO_MODE);
  set8(R_AX_PTCL_COMMON_SETTING_0, (uint8_t)(B_AX_CMAC_TX_MODE_0 | B_AX_CMAC_TX_MODE_1));
  clr8(R_AX_PTCL_COMMON_SETTING_0, (uint8_t)(B_AX_PTCL_TRIGGER_SS_EN_0 | B_AX_PTCL_TRIGGER_SS_EN_1 |
                                             B_AX_PTCL_TRIGGER_SS_EN_UL));
  w8(R_AX_PTCLRPT_FULL_HDL, (uint8_t)((r8(R_AX_PTCLRPT_FULL_HDL) & ~B_AX_SPE_RPT_PATH_MASK) |
                                      field(B_AX_SPE_RPT_PATH_MASK, FWD_TO_WLCPU)));
  mask32(R_AX_AGG_LEN_VHT_0, B_AX_AMPDU_MAX_LEN_VHT_MASK, 0x3FF80);
  /* cmac_dma */
  clr8(R_AX_RXDMA_CTRL_0, (uint8_t)RX_FULL_MODE);
  return 0;
}

/* rtw89_pci_ops_mac_post_init_ax (sem o LTR): libera todos os canais de TX. */
static void pci_post_init(void) {
  set32(R_AX_TX_ADDRESS_INFO_MODE_SETTING, B_AX_HOST_ADDR_INFO_8B_SEL);
  clr32(R_AX_PKTIN_SETTING, B_AX_WD_ADDR_INFO_LENGTH);
  clr32(R_AX_PCIE_DMA_STOP1, B_AX_TX_STOP1_MASK_V1);
  clr32(R_AX_PCIE_DMA_STOP1, B_AX_STOP_WPDMA | B_AX_STOP_PCIEIO);
}

/* H2C normal: cabecalho de 8 bytes + dados. Seq multiplo de 4 sempre pede REC_ACK. */
static int h2c_cmd(int cat, int cls, int func, int rack, int dack, const uint8_t* data, uint32_t len) {
  static uint8_t pkt[2048 + 8];
  if (len + 8u > sizeof(pkt)) return fail("H2C grande demais");
  if ((h2c_seq % 4u) == 0) rack = 1;
  put32(pkt, (uint32_t)cat | ((uint32_t)cls << 2) | ((uint32_t)func << 8) | ((uint32_t)h2c_seq << 24));
  put32(pkt + 4, ((len + 8u) & 0x3FFFu) | (rack ? 1u << 14 : 0) | (dack ? 1u << 15 : 0));
  bcopy(pkt + 8, data, len);
  h2c_seq++;
  return h2c_send(pkt, len + 8u, 0);
}

int rtw_rx_poll(struct rtw_rx* out) {
  static const uint32_t idx_reg[2] = {R_AX_RXQ_RXBD_IDX, R_AX_RPQ_RXBD_IDX};
  int q;
  if (rx_held >= 0) {
    bzero(rx_buf[rx_held] + rx_wp[rx_held] * RX_BUF_STRIDE, 8);
    rx_wp[rx_held] = (rx_wp[rx_held] + 1u) % RX_RING_N;
    w16(idx_reg[rx_held], (uint16_t)rx_wp[rx_held]);
    rx_held = -1;
  }
  for (q = 0; q < 2; q++) {
    uint32_t hw = (r32(idx_reg[q]) >> 16) & 0xFFFu;
    uint8_t* b;
    uint32_t info, d0, dlen, off;
    if (hw == rx_wp[q]) continue;
    if ((hw + RX_RING_N - rx_wp[q]) % RX_RING_N > rx_maxfill[q])
      rx_maxfill[q] = (hw + RX_RING_N - rx_wp[q]) % RX_RING_N;
    rx_last_ms[q] = net_ms();
    b = rx_buf[q] + rx_wp[q] * RX_BUF_STRIDE;
    __asm__ volatile ("mfence" ::: "memory");
    info = le32(b);
    d0 = le32(b + 4);
    out->ring = q;
    out->fs = (info >> 15) & 1u;
    out->ls = (info >> 14) & 1u;
    out->type = (int)((d0 >> 24) & 0xFu);
    dlen = d0 & 0x3FFFu;
    off = 4u + ((d0 & (1u << 31)) ? 32u : 16u) + ((d0 >> 28) & 7u) * 8u;
    if (off + dlen > RX_BUF_SIZE) dlen = off < RX_BUF_SIZE ? RX_BUF_SIZE - off : 0;
    out->len = dlen;
    out->desc = b + 4;
    out->data = b + off;
    /* o buffer volta para a placa so na proxima chamada */
    rx_held = q;
    return 1;
  }
  return 0;
}

/* Espera um C2H de ack (classe 0 = info, func 0 REC_ACK / 1 DONE_ACK). */
static int wait_c2h_ack(uint32_t ms) {
  struct rtw_rx rx;
  uint32_t t0 = net_ms(), spins = 0;
  int got = 0;
  for (;;) {
    while (rtw_rx_poll(&rx)) {
      rlog_hex("[rtw] rx anel=", (uint32_t)rx.ring);
      rlog_hex("[rtw]   tipo=", (uint32_t)rx.type);
      rlog_hex("[rtw]   bytes=", rx.len);
      if (rx.type == 10 && rx.len >= 8) {
        uint32_t w0 = le32(rx.data);
        rlog_hex("[rtw]   c2h w0=", w0);
        if ((w0 & 3u) == 1 && ((w0 >> 2) & 0x3Fu) == 0 && ((w0 >> 8) & 0xFFu) <= 1) got++;
      }
    }
    if (got >= 1) return 0;
    if (late(t0, &spins, ms)) return -1;
  }
}

/* Uma IMR: limpa 'clr' e liga 'set' (rtw89_write32_clr + _set). */
static void imr(uint32_t a, uint32_t clr, uint32_t set) { w32(a, (r32(a) & ~clr) | set); }

/* enable_imr_ax (DMAC e CMAC0) e err_imr_ctrl_ax(true), com os valores do
 * rtw8852b_imr_info. Sem isto PTCL_IMR0 e RMAC ficam no reset (quase tudo
 * ligado), um evento comum da CMAC vira erro L0, o firmware sobe para L1
 * (HALT_C2H 0x10) e para o DMA de TX e RX esperando o driver. */
static void enable_imr(void) {
  /* DMAC */
  imr(0x9430, 0x00003337u, 0x00003327u);  /* WDRLS_ERR_IMR */
  imr(0x9D1C, 0, 0x00000008u);            /* SEC_DEBUG: IMR_ERROR */
  imr(0x9BF4, 0x0000003Eu, 0);            /* MPDU_TX_ERR_IMR */
  imr(0x9CF4, 0x0000000Bu, 0);            /* MPDU_RX_ERR_IMR */
  imr(0x9EF0, 0x00000007u, 0x00000007u);  /* STA_SCHEDULER_ERR_IMR */
  imr(0x9F1C, 0x0000030Fu, 0x00000101u);  /* TXPKTCTL_ERR_IMR_ISR */
  imr(0x9F2C, 0x0000030Fu, 0x00000303u);  /* TXPKTCTL_ERR_IMR_ISR_B1 */
  imr(0x8C38, 0x070FF0FFu, 0x070FF0FFu);  /* WDE_ERR_IMR */
  imr(0x9038, 0x070FF0FFu, 0x070FF0DFu);  /* PLE_ERR_IMR */
  imr(0x9A20, 0, 0x00000001u);            /* PKTIN_ERR_IMR */
  imr(0x8850, 0xFF0FFFFFu, 0x0C000161u);  /* HOST_DISPATCHER_ERR_IMR */
  imr(0x8854, 0xFF07FFFFu, 0x04000062u);  /* CPU_DISPATCHER_ERR_IMR */
  imr(0x8858, 0x3F031F1Fu, 0);            /* OTHER_DISPATCHER_ERR_IMR */
  imr(0x9840, 0x00001111u, 0x00001111u);  /* CPUIO_ERR_IMR */
  imr(0x960C, 0, 0x00000001u);            /* BBRPT_COM_ERR_IMR_ISR */
  imr(0x962C, 0x000000FFu, 0);            /* BBRPT_CHINFO_ERR_IMR_ISR */
  imr(0x963C, 0, 0x00000001u);            /* BBRPT_DFS_ERR_IMR_ISR */
  imr(0x966C, 0, 0x00000001u);            /* LA_ERRFLAG */
  /* CMAC0 */
  imr(0xC3E8, 0x00000003u, 0x00000001u);  /* SCHEDULE_ERR_IMR */
  imr(0xC6C0, 0xFFFFFFFFu, 0x10800001u);  /* PTCL_IMR0 */
  imr(0xC800, 0x0080C000u, 0x0000C000u);  /* DLE_CTRL */
  imr(0xCEF4, 0x000FF000u, 0x000E4000u);  /* RMAC_ERR_ISR */
  imr(0xCCEC, 0x00000780u, 0x00000780u);  /* TMAC_ERR_IMR_ISR */
  w32(0x8520, 0xFFFFFFFFu);               /* DMAC_ERR_IMR */
  w32(0xC160, 0xFFFFFFFFu);               /* CMAC_ERR_IMR */
}

int rtw_mac_init(void) {
  static const uint8_t ofld_cfg[8] = {0x09, 0x00, 0x00, 0x00, 0x5E, 0x00, 0x00, 0x00};
  if (!mmio || !dma_ready) return fail("firmware nao carregado");
  /* pci_pre_init zerou os indices da placa */
  rx_wp[0] = rx_wp[1] = 0;
  rx_held = -1;
  if (enable_bb_rf()) return -1;
  sys_init();
  if (dmac_init()) return -1;
  if (cmac_init()) return -1;
  enable_imr();
  /* host rpr (PCIe: POH) */
  mask32(R_AX_WDRLS_CFG, B_AX_WDRLS_MODE_MASK, RTW89_RPR_MODE_POH);
  set32(R_AX_RLSRPT0_CFG0, B_AX_RLSRPT0_FLTR_MAP_MASK);
  mask32(R_AX_RLSRPT0_CFG1, B_AX_RLSRPT0_AGGNUM_MASK, 30);
  mask32(R_AX_RLSRPT0_CFG1, B_AX_RLSRPT0_TO_MASK, 255);
  pci_post_init();
  rlog("[rtw] MAC inicializada\n");
  rlog_hex("[rtw] RXQ idx=", r32(R_AX_RXQ_RXBD_IDX));
  rlog_hex("[rtw] RPQ idx=", r32(R_AX_RPQ_RXBD_IDX));
  /* rtw89_fw_h2c_set_ofld_cfg (MAC, FW_OFLD, OFLD_CFG), com REC_ACK e DONE_ACK */
  h2c_seq = 0;
  if (h2c_cmd(1, 0x9, 0x14, 1, 1, ofld_cfg, sizeof(ofld_cfg))) return -1;
  if (wait_c2h_ack(500)) {
    rlog_hex("[rtw] CH12 idx=", r32(R_AX_CH12_TXBD_IDX));
    rlog_hex("[rtw] RXQ idx=", r32(R_AX_RXQ_RXBD_IDX));
    return fail("firmware nao respondeu ao H2C");
  }
  rlog("[rtw] firmware respondeu (C2H)\n");
  return 0;
}

/* ---------- etapa 4: tabelas BB/RF, canal e escuta de beacons ---------- */

#include "rtw8852b_tbl.h"

#define PHY_BASE 0x10000u
#define RFREG_MASK 0xFFFFFu
static const uint32_t rf_base[2] = {0xE000, 0xF000};

static uint32_t pr32(uint32_t a) { return r32(PHY_BASE + a); }
static void pw32(uint32_t a, uint32_t v) { w32(PHY_BASE + a, v); }
static void pmask(uint32_t a, uint32_t mask, uint32_t v) { mask32(PHY_BASE + a, mask, v); }
static uint32_t pget(uint32_t a, uint32_t mask) { return get32(PHY_BASE + a, mask); }
static void pset(uint32_t a, uint32_t b) { set32(PHY_BASE + a, b); }
static void pclr(uint32_t a, uint32_t b) { clr32(PHY_BASE + a, b); }

static void udelay_spin(uint32_t us) {
  /* sem relogio fino: ~1 us por leitura de registrador no PCIe */
  while (us--) (void)r32(R_AX_SYS_CFG1);
}

static int swsi_wait(void) {
  int i;
  for (i = 0; i < 1000; i++) {
    if (!(pr32(R_SWSI_V1) & (B_SWSI_W_BUSY_V1 | B_SWSI_R_BUSY_V1))) return 0;
    udelay_spin(1);
  }
  return -1;
}

/* rtw89_phy_write_rf_v1: bit 16 do endereco = acesso direto, senao SWSI. */
static void rf_write(int path, uint32_t addr, uint32_t mask, uint32_t data) {
  int en = 0;
  mask &= RFREG_MASK;
  if (addr & 0x10000u) {
    pmask(rf_base[path] + ((addr & 0xFFu) << 2), mask, data);
    udelay_spin(1);
    return;
  }
  if (swsi_wait()) {
    rlog_hex("[rtw] SWSI ocupado (W) addr=", addr);
    return;
  }
  data &= RFREG_MASK;
  if (mask != RFREG_MASK) {
    en = 1;
    pmask(R_SWSI_BIT_MASK_V1, RFREG_MASK, mask);
    data = (data << lowbit(mask)) & RFREG_MASK;
  }
  pw32(R_SWSI_DATA_V1, field(B_SWSI_DATA_BIT_MASK_EN_V1, (uint32_t)en) |
                       field(B_SWSI_DATA_PATH_V1, (uint32_t)path) |
                       field(B_SWSI_DATA_ADDR_V1, addr & 0xFFu) |
                       field(B_SWSI_DATA_VAL_V1, data));
}

static uint32_t rf_read(int path, uint32_t addr, uint32_t mask) {
  int i;
  mask &= RFREG_MASK;
  if (addr & 0x10000u) return pget(rf_base[path] + ((addr & 0xFFu) << 2), mask);
  if (swsi_wait()) return 0xFFFFFFFFu;
  pmask(R_SWSI_READ_ADDR_V1, B_SWSI_READ_ADDR_V1,
        field(B_SWSI_READ_ADDR_PATH_V1, (uint32_t)path) | field(B_SWSI_READ_ADDR_ADDR_V1, addr & 0xFFu));
  udelay_spin(2);
  for (i = 0; i < 1000 && !(pr32(R_SWSI_V1) & B_SWSI_R_DATA_DONE_V1); i++) udelay_spin(1);
  if (i == 1000) return 0xFFFFFFFFu;
  return pget(R_SWSI_V1, mask);
}

/* rtw89_phy_config_bb_reg: 0xF9..0xFE sao esperas. */
static void bb_table(const uint32_t (*t)[2], int n) {
  int i;
  for (i = 0; i < n; i++) {
    uint32_t a = t[i][0], d = t[i][1];
    if (a == 0xFE) msleep(50);
    else if (a == 0xFD) msleep(5);
    else if (a == 0xFC) msleep(1);
    else if (a == 0xFB) udelay_spin(50);
    else if (a == 0xFA) udelay_spin(5);
    else if (a == 0xF9) udelay_spin(1);
    else if (d == 0xBABECAFEu) continue;
    else pw32(a, d);
  }
}

/* Tabela de RF de um caminho, mais a copia que o firmware guarda (H2C OUTSRC
 * RF_REG_A/B, paginas de 500 entradas). */
static int rf_table(int path, const uint32_t (*t)[2], int n) {
  static uint32_t store[1500];
  uint8_t page[2000];
  int i, cnt = 0, pg;
  for (i = 0; i < n; i++) {
    rf_write(path, t[i][0], RFREG_MASK, t[i][1]);
    if (t[i][0] < 0x100u) continue;
    if (cnt < (int)(sizeof(store) / sizeof(store[0])))
      store[cnt++] = (t[i][0] << 20) | t[i][1];
  }
  for (pg = 0; pg * 500 < cnt; pg++) {
    int k = cnt - pg * 500 > 500 ? 500 : cnt - pg * 500;
    for (i = 0; i < k; i++) put32(page + i * 4, store[pg * 500 + i]);
    if (h2c_cmd(H2C_CAT_OUTSRC, path ? H2C_CL_OUTSRC_RF_REG_B : H2C_CL_OUTSRC_RF_REG_A,
                pg, 0, 0, page, (uint32_t)k * 4u))
      return -1;
  }
  rlog_hex("[rtw] RF regs para o firmware: ", (uint32_t)cnt);
  return 0;
}

static void bb_reset_all(void) {
  pmask(R_S0_HW_SI_DIS, B_S0_HW_SI_DIS_W_R_TRIG, 0x7);
  pmask(R_S1_HW_SI_DIS, B_S1_HW_SI_DIS_W_R_TRIG, 0x7);
  udelay_spin(1);
  pmask(R_RSTB_ASYNC, B_RSTB_ASYNC_ALL, 1);
  pmask(R_RSTB_ASYNC, B_RSTB_ASYNC_ALL, 0);
  pmask(R_S0_HW_SI_DIS, B_S0_HW_SI_DIS_W_R_TRIG, 0x0);
  pmask(R_S1_HW_SI_DIS, B_S1_HW_SI_DIS_W_R_TRIG, 0x0);
  pmask(R_RSTB_ASYNC, B_RSTB_ASYNC_ALL, 1);
}

static void bb_reset(void) {
  pset(R_P0_TXPW_RSTB, B_P0_TXPW_RSTB_MANON);
  pset(R_P0_TSSI_TRK, B_P0_TSSI_TRK_EN);
  pset(R_P1_TXPW_RSTB, B_P1_TXPW_RSTB_MANON);
  pset(R_P1_TSSI_TRK, B_P1_TSSI_TRK_EN);
  bb_reset_all();
  pclr(R_P0_TXPW_RSTB, B_P0_TXPW_RSTB_MANON);
  pclr(R_P0_TSSI_TRK, B_P0_TSSI_TRK_EN);
  pclr(R_P1_TXPW_RSTB, B_P1_TXPW_RSTB_MANON);
  pclr(R_P1_TSSI_TRK, B_P1_TSSI_TRK_EN);
}

static void bb_reset_en(int en, int is_2g) {
  if (en) {
    pmask(R_S0_HW_SI_DIS, B_S0_HW_SI_DIS_W_R_TRIG, 0x0);
    pmask(R_S1_HW_SI_DIS, B_S1_HW_SI_DIS_W_R_TRIG, 0x0);
    pmask(R_RSTB_ASYNC, B_RSTB_ASYNC_ALL, 1);
    if (is_2g) pmask(R_RXCCA, B_RXCCA_DIS, 0x0);
    pmask(R_PD_CTRL, B_PD_HIT_DIS, 0x0);
  } else {
    pmask(R_RXCCA, B_RXCCA_DIS, 0x1);
    pmask(R_PD_CTRL, B_PD_HIT_DIS, 0x1);
    pmask(R_S0_HW_SI_DIS, B_S0_HW_SI_DIS_W_R_TRIG, 0x7);
    pmask(R_S1_HW_SI_DIS, B_S1_HW_SI_DIS_W_R_TRIG, 0x7);
    udelay_spin(1);
    pmask(R_RSTB_ASYNC, B_RSTB_ASYNC_ALL, 0);
  }
}

/* rtw8852bx_mac_disable_bb_rf + enable_bb_rf (rtw89_chip_reset_bb_rf). */
static int reset_bb_rf(void) {
  uint8_t v;
  clr32(R_AX_WLRF_CTRL, B_AX_AFC_AFEDIG);
  clr8(R_AX_SYS_FUNC_EN, (uint8_t)(B_AX_FEN_BBRSTB | B_AX_FEN_BB_GLB_RSTN));
  if (xtal_si_read(XTAL_SI_WL_RFC_S0, &v) ||
      xtal_si_write(XTAL_SI_WL_RFC_S0, (uint8_t)(v & ~XTAL_SI_RF00S_EN), 0xFF) ||
      xtal_si_read(XTAL_SI_WL_RFC_S1, &v) ||
      xtal_si_write(XTAL_SI_WL_RFC_S1, (uint8_t)(v & ~XTAL_SI_RF10S_EN), 0xFF))
    return fail("xtal si (disable_bb_rf)");
  return enable_bb_rf();
}

/* rtw8852bx_bb_cfg_txrx_path com os dois caminhos (RF_AB, 2 fluxos). */
static void cfg_txrx_path(void) {
  pmask(R_CHBW_MOD_V1, B_ANT_RX_SEG0, 3);
  pmask(R_FC0_BW_V1, B_ANT_RX_1RCCA_SEG0, 3);
  pmask(R_FC0_BW_V1, B_ANT_RX_1RCCA_SEG1, 3);
  pmask(R_RXHT_MCS_LIMIT, B_RXHT_MCS_LIMIT, 1);
  pmask(R_RXVHT_MCS_LIMIT, B_RXVHT_MCS_LIMIT, 1);
  pmask(R_RXHE, B_RXHE_USER_MAX, 4);
  pmask(R_RXHE, B_RXHE_MAX_NSS, 1);
  pmask(R_RXHE, B_RXHETB_MAX_NSS, 1);
  pmask(R_P1_TXPW_RSTB, B_P1_TXPW_RSTB_MANON | B_P1_TXPW_RSTB_TSSI, 1);
  pmask(R_P1_TXPW_RSTB, B_P1_TXPW_RSTB_MANON | B_P1_TXPW_RSTB_TSSI, 3);
  pmask(R_P0_RFMODE, B_P0_RFMODE_ORI_TXRX_FTM_TX, 0x1233312);
  pmask(R_P0_RFMODE_FTM_RX, B_P0_RFMODE_FTM_RX, 0x333);
  pmask(R_P1_RFMODE, B_P1_RFMODE_ORI_TXRX_FTM_TX, 0x1233312);
  pmask(R_P1_RFMODE_FTM_RX, B_P1_RFMODE_FTM_RX, 0x333);
  pmask(R_RXHT_MCS_LIMIT, B_RXHT_MCS_LIMIT, 1);
  pmask(R_RXVHT_MCS_LIMIT, B_RXVHT_MCS_LIMIT, 1);
  pmask(R_RXHE, B_RXHE_MAX_NSS, 1);
  pmask(R_RXHE, B_RXHETB_MAX_NSS, 1);
  pmask(R_MAC_SEL, B_MAC_SEL_MOD, 0x0);
}

/* Registrador 18 do RF (canal, banda, largura), nos dois caminhos e nas duas copias. */
static uint32_t cfgch_value(uint32_t v, int ch) {
  v &= ~(RR_CFGCH_BAND1 | RR_CFGCH_POW_LCK | RR_CFGCH_TRX_AH | RR_CFGCH_BCN |
         RR_CFGCH_BAND0 | RR_CFGCH_CH);
  v |= field(RR_CFGCH_CH, (uint32_t)ch);
  if (ch > 14) v |= field(RR_CFGCH_BAND1, CFGCH_BAND1_5G) | field(RR_CFGCH_BAND0, CFGCH_BAND0_5G);
  v &= ~(RR_CFGCH_POW_LCK | RR_CFGCH_TRX_AH | RR_CFGCH_BCN | RR_CFGCH_BW2) & RFREG_MASK;
  return v | RR_CFGCH_BW2;
}

static int set_s0_arfc18(uint32_t val) {
  uint32_t bak = rf_read(0, RR_LDO, RFREG_MASK);
  int i;
  rf_write(0, RR_LDO, RR_LDO_SEL, 0x1);
  rf_write(0, RR_CFGCH, RFREG_MASK, val);
  for (i = 0; i < 1000 && rf_read(0, RR_LPF, RR_LPF_BUSY) != 0; i++) udelay_spin(1);
  rf_write(0, RR_LDO, RFREG_MASK, bak);
  return i == 1000;
}

static void lck_check(void) {
  uint32_t tmp;
  if (rf_read(0, RR_SYNFB, RR_SYNFB_LK) == 0) {
    rlog("[rtw] LCK: SYN MMD reset\n");
    rf_write(0, RR_MMD, RR_MMD_RST_EN, 0x1);
    rf_write(0, RR_MMD, RR_MMD_RST_SYN, 0x0);
    rf_write(0, RR_MMD, RR_MMD_RST_SYN, 0x1);
    rf_write(0, RR_MMD, RR_MMD_RST_EN, 0x0);
  }
  udelay_spin(10);
  if (rf_read(0, RR_SYNFB, RR_SYNFB_LK) == 0) {
    rlog("[rtw] LCK: RF 0x18 de novo\n");
    rf_write(0, RR_LCK_TRG, RR_LCK_TRGSEL, 0x1);
    tmp = rf_read(0, RR_CFGCH, RFREG_MASK);
    set_s0_arfc18(tmp);
    rf_write(0, RR_LCK_TRG, RR_LCK_TRGSEL, 0x0);
  }
  if (rf_read(0, RR_SYNFB, RR_SYNFB_LK) == 0) {
    rlog("[rtw] LCK: SYN off/on\n");
    tmp = rf_read(0, RR_POW, RFREG_MASK);
    rf_write(0, RR_POW, RFREG_MASK, tmp);
    tmp = rf_read(0, RR_SX, RFREG_MASK);
    rf_write(0, RR_SX, RFREG_MASK, tmp);
    rf_write(0, RR_SYNLUT, RR_SYNLUT_MOD, 0x1);
    rf_write(0, RR_POW, RR_POW_SYN, 0x0);
    rf_write(0, RR_POW, RR_POW_SYN, 0x3);
    rf_write(0, RR_SYNLUT, RR_SYNLUT_MOD, 0x0);
    rf_write(0, RR_LCK_TRG, RR_LCK_TRGSEL, 0x1);
    tmp = rf_read(0, RR_CFGCH, RFREG_MASK);
    set_s0_arfc18(tmp);
    rf_write(0, RR_LCK_TRG, RR_LCK_TRGSEL, 0x0);
  }
  rlog_hex("[rtw] RF SYNFB=", rf_read(0, RR_SYNFB, RFREG_MASK));
}

/* rtw8852b_ctrl_bw_ch com 20 MHz. */
static void rf_set_channel(int ch) {
  int dav, path;
  for (dav = 1; dav >= 0; dav--) {
    for (path = 0; path < 2; path++) {
      uint32_t reg = dav ? RR_CFGCH : RR_CFGCH_V1;
      uint32_t v = cfgch_value(rf_read(path, reg, RFREG_MASK), ch);
      if (path == 0 && dav) {
        if (!set_s0_arfc18(v)) lck_check();
        else rlog("[rtw] LCK timeout\n");
      } else {
        rf_write(path, reg, RFREG_MASK, v);
      }
      rf_write(path, RR_LCKST, RR_LCKST_BIN, 0);
      rf_write(path, RR_LCKST, RR_LCKST_BIN, 1);
    }
  }
  for (dav = 1; dav >= 0; dav--) {
    for (path = 0; path < 2; path++) {
      uint32_t reg = dav ? RR_CFGCH : RR_CFGCH_V1;
      uint32_t v = rf_read(path, reg, RFREG_MASK);
      if (v == 0xFFFFFFFFu) continue;
      v = (v & ~RR_CFGCH_BW) | field(RR_CFGCH_BW, CFGCH_BW_20M);
      v &= ~(RR_CFGCH_POW_LCK | RR_CFGCH_TRX_AH | RR_CFGCH_BCN | RR_CFGCH_BW2) & RFREG_MASK;
      rf_write(path, reg, RFREG_MASK, v | RR_CFGCH_BW2);
    }
  }
  for (path = 0; path < 2; path++) {
    rf_write(path, RR_LUTWE2, RR_LUTWE2_RTXBW, 0x1);
    rf_write(path, RR_LUTWA, RR_LUTWA_M2, 0x12);
    rf_write(path, RR_LUTWD0, RR_LUTWD0_LB, 0x1B);
    rf_write(path, RR_LUTWE2, RR_LUTWE2_RTXBW, 0x0);
  }
  rlog_hex("[rtw] RF 0x18 A=", rf_read(0, RR_CFGCH, RFREG_MASK));
  rlog_hex("[rtw] RF 0x18 B=", rf_read(1, RR_CFGCH, RFREG_MASK));
}

static uint8_t sco_mapping(int ch) {
  if (ch == 1) return 109;
  if (ch >= 2 && ch <= 6) return 108;
  if (ch >= 7 && ch <= 10) return 107;
  if (ch >= 11 && ch <= 14) return 106;
  if (ch == 36 || ch == 38) return 51;
  if (ch >= 40 && ch <= 58) return 50;
  if (ch >= 60 && ch <= 64) return 49;
  if (ch == 100 || ch == 102) return 48;
  if (ch >= 104 && ch <= 126) return 47;
  if (ch >= 128 && ch <= 151) return 46;
  if (ch >= 153 && ch <= 177) return 45;
  return 0;
}

static uint8_t chan_idx(int ch) {
  static const uint8_t base5[4] = {36, 100, 132, 149};
  int i;
  if (ch <= 14) return (uint8_t)ch;  /* base 0 */
  for (i = 3; i > 0 && ch < base5[i]; i--) { }
  return (uint8_t)(((i + 2) << 4) | (((ch - base5[i]) >> 1) & 0xF));
}

/* rtw8852b_set_channel (MAC, BB, RF) entre set_channel_help(enter/exit), 20 MHz. */
static void set_channel(int ch) {
  int is_2g = ch <= 14;
  int path;
  /* enter */
  pmask(R_ADC_FIFO, B_ADC_FIFO_RST, 0xF);
  udelay_spin(40);
  bb_reset_en(0, is_2g);
  /* MAC */
  clr8(R_AX_WMAC_RFMOD, (uint8_t)B_AX_WMAC_RFMOD_MASK);
  w32(R_AX_TX_SUB_CARRIER_VALUE, 0);
  if (!is_2g) {
    clr8(R_AX_TXRATE_CHK, (uint8_t)B_AX_BAND_MODE);
    set8(R_AX_TXRATE_CHK, (uint8_t)(B_AX_CHECK_CCK_EN | B_AX_RTS_LIMIT_IN_OFDM6));
  } else {
    set8(R_AX_TXRATE_CHK, (uint8_t)B_AX_BAND_MODE);
    clr8(R_AX_TXRATE_CHK, (uint8_t)(B_AX_CHECK_CCK_EN | B_AX_RTS_LIMIT_IN_OFDM6));
  }
  /* BB: banda, SCO e coeficientes de CCK (sem os ganhos de efuse ainda) */
  pmask(R_PATH0_BAND_SEL_V1, B_PATH0_BAND_SEL_MSK_V1, is_2g ? 1 : 0);
  pmask(R_PATH1_BAND_SEL_V1, B_PATH1_BAND_SEL_MSK_V1, is_2g ? 1 : 0);
  pmask(R_FC0_BW_V1, B_FC0_BW_INV, sco_mapping(ch));
  pmask(R_TXFIR0, B_TXFIR_C01, ch == 14 ? 0x3b13ff : 0x3d23ff);
  pmask(R_TXFIR2, B_TXFIR_C23, ch == 14 ? 0x1c42de : 0x29b354);
  pmask(R_TXFIR4, B_TXFIR_C45, ch == 14 ? 0xfdb0ad : 0xfc1c8);
  pmask(R_TXFIR6, B_TXFIR_C67, ch == 14 ? 0xf60f6e : 0xfdb053);
  pmask(R_TXFIR8, B_TXFIR_C89, ch == 14 ? 0xfd8f92 : 0xf86f9a);
  pmask(R_TXFIRA, B_TXFIR_CAB, ch == 14 ? 0x2d011 : 0xfaef92);
  pmask(R_TXFIRC, B_TXFIR_CCD, ch == 14 ? 0x1c02c : 0xfe5fcc);
  pmask(R_TXFIRE, B_TXFIR_CEF, ch == 14 ? 0xfff00a : 0xffdff5);
  /* largura 20 MHz */
  {
    uint32_t rx_path_0 = pget(R_CHBW_MOD_V1, B_ANT_RX_SEG0);
    pmask(R_FC0_BW_V1, B_FC0_BW_SET, 0x0);
    pmask(R_CHBW_MOD_V1, B_CHBW_MOD_SBW, 0x0);
    pmask(R_CHBW_MOD_V1, B_CHBW_MOD_PRICH, 0x0);
    pmask(R_P0_RFMODE_ORI_RX, B_P0_RFMODE_ORI_RX_ALL, 0x333);
    pmask(R_P1_RFMODE_ORI_RX, B_P1_RFMODE_ORI_RX_ALL, 0x333);
    for (path = 0; path < 2; path++) {
      pmask(path ? 0xC1EC : 0xC0EC, 0x6000, 0x0);
      pmask(path ? 0xC1E4 : 0xC0E4, 0x30, 0x2);
    }
    if (rx_path_0 == 0x1) pmask(R_P1_RFMODE_ORI_RX, B_P1_RFMODE_ORI_RX_ALL, 0x111);
    else if (rx_path_0 == 0x2) pmask(R_P0_RFMODE_ORI_RX, B_P0_RFMODE_ORI_RX_ALL, 0x111);
  }
  pmask(R_UPD_CLK_ADC, B_ENABLE_CCK, is_2g ? 1 : 0);
  pmask(R_RXCCA, B_RXCCA_DIS, is_2g ? 0 : 1);
  if (!is_2g) {
    pmask(R_PATH0_BT_SHARE_V1, B_PATH0_BT_SHARE_V1, 0x0);
    pmask(R_PATH0_BTG_PATH_V1, B_PATH0_BTG_PATH_V1, 0x0);
    pmask(R_PATH1_BT_SHARE_V1, B_PATH1_BT_SHARE_V1, 0x0);
    pmask(R_PATH1_BTG_PATH_V1, B_PATH1_BTG_PATH_V1, 0x0);
    pmask(R_CHBW_MOD_V1, B_BT_SHARE, 0x0);
    pmask(R_FC0_BW_V1, B_ANT_RX_BT_SEG0, 0x0);
    pmask(R_BT_DYN_DC_EST_EN_V1, B_BT_DYN_DC_EST_EN_MSK, 0x0);
    pmask(R_GNT_BT_WGT_EN, B_GNT_BT_WGT_EN, 0x0);
  }
  pmask(R_MAC_PIN_SEL, B_CH_IDX_SEG0, chan_idx(ch));
  pmask(R_PATH0_5MDET_V1, B_PATH0_5MDET_EN, 0x0);
  pmask(R_PATH1_5MDET_V1, B_PATH1_5MDET_EN, 0x0);
  pmask(R_ASSIGN_SBD_OPT_V1, B_ASSIGN_SBD_OPT_EN_V1, 0x0);
  bb_reset_all();
  /* RF */
  rf_set_channel(ch);
  /* exit */
  pmask(R_ADC_FIFO, B_ADC_FIFO_RST, 0x0);
  bb_reset_en(1, is_2g);
}

int rtw_radio_init(int channel) {
  uint32_t v;
  int i;
  if (!mmio || !dma_ready) return fail("MAC nao inicializada");
  if (reset_bb_rf()) return -1;
  bb_table(tbl_bb, (int)(sizeof(tbl_bb) / sizeof(tbl_bb[0])));
  bb_reset();
  rlog_hex("[rtw] tabela BB aplicada, regs=", (uint32_t)(sizeof(tbl_bb) / sizeof(tbl_bb[0])));
  if (rf_table(0, tbl_radioa, (int)(sizeof(tbl_radioa) / sizeof(tbl_radioa[0]))) ||
      rf_table(1, tbl_radiob, (int)(sizeof(tbl_radiob) / sizeof(tbl_radiob[0]))))
    return -1;
  rlog_hex("[rtw] RF A 0x00=", rf_read(0, 0x0, RFREG_MASK));
  rlog_hex("[rtw] RF B 0x00=", rf_read(1, 0x0, RFREG_MASK));
  /* phy_dm_init: so o que a recepcao precisa */
  pclr(R_P0_EN_SOUND_WO_NDP, B_P0_EN_SOUND_WO_NDP);
  pclr(R_P1_EN_SOUND_WO_NDP, B_P1_EN_SOUND_WO_NDP);
  for (v = R_AX_PWR_MACID_LMT_TABLE0; v <= R_AX_PWR_MACID_LMT_TABLE127; v += 4) w32(v, 0);
  /* nctl */
  pset(R_IOQ_IQK_DPK, 0x3);
  pset(R_GNT_BT_WGT_EN, 0x1);
  pset(R_P0_PATH_RST, 0x8000000);
  pset(R_P1_PATH_RST, 0x8000000);
  pset(R_IOQ_IQK_DPK, 0x2);
  pw32(R_NCTL_CFG, 0x8);
  for (i = 0; i < 100; i++) {
    pw32(0x8080, 0x4);
    udelay_spin(1);
    if (pr32(0x8080) == 0x4) break;
    udelay_spin(10);
  }
  if (i == 100) rlog("[rtw] bloco NCTL nao respondeu\n");
  bb_table(tbl_nctl, (int)(sizeof(tbl_nctl) / sizeof(tbl_nctl[0])));
  rtw_rfk_init();
  cfg_txrx_path();
  set_channel(channel);
  /* recebe beacons de qualquer rede (como no scan do rtw89) */
  clr32(R_AX_RX_FLTR_OPT, B_AX_A_A1_MATCH | B_AX_A_BC | B_AX_A_BCN_CHK_EN);
  rlog_hex("[rtw] radio no canal ", (uint32_t)channel);
  return 0;
}

/* ---------- etapa 5: estacao no firmware, potencia e TX de gerencia ---------- */

#define TXCH_B0MG 4      /* indice de CH8 em txchs */
#define TX_DATA_BUF 2304u
#define TXWD_LEN 64u     /* body 24 + info 24 + wp_info 8 + addr_info 8 */

static uint8_t* wd_buf[TXCH_N];
static uint8_t* txdata_buf[TXCH_N];
static uint32_t tx_wp[TXCH_N];

/* Porta 0 como estacao sem link (rtw89_mac_port_update, net_type NO_LINK). */
static void port_init(void) {
  uint32_t v = r32(R_AX_PORT_CFG_P0);
  v &= ~(B_AX_PORT_FUNC_EN | B_AX_BRK_SETUP | B_AX_TBTT_PROHIB_EN | B_AX_RX_BSSID_FIT_EN |
         B_AX_TSF_UDT_EN | B_AX_BCNTX_EN | B_AX_NET_TYPE_MASK);
  w32(R_AX_PORT_CFG_P0, v | B_AX_TSFTR_RST);
  v = r32(R_AX_MBSSID_DROP_0);
  v &= ~(field(B_AX_PORT_DROP_4_0_MASK, 1) | 1u);
  w32(R_AX_MBSSID_DROP_0, v);
  set32(R_AX_PORT_CFG_P0, B_AX_PORT_FUNC_EN);
}

/* Potencia por taxa (0xD2C0..0xD2E8), limites (0xD2EC..0xD338) e limites por
 * RU (0xD33C..0xD368) como o rtw89 deixa no canal 56 (5 GHz, 20 MHz),
 * copiados de um dump da placa no Linux. O registrador usa 0,5 dB por
 * unidade (a tabela do rtw89 usa 0,25 dB e divide por 2 ao gravar). */
static const uint32_t txpwr_ch56[43] = {
  0x28282828, 0x28282828, 0x24262828, 0x28282828,
  0x22242628, 0x1A1C1E20, 0x28282828, 0x28282828,
  0x22242628, 0x1A1C1E20, 0x28282828, 0x00000000,
  0x001A001A, 0x001A001A, 0x0000001A, 0x00000000,
  0x001A0000, 0x0000001A, 0x001A0000, 0x00000000,
  0x0000001A, 0x00000000, 0x0E140014, 0x0E140E14,
  0x00000E14, 0x00000000, 0x0E140000, 0x00000E14,
  0x0E140000, 0x00000000, 0x00000E14, 0x0C0C0C0C,
  0x00000000, 0x11111111, 0x00000000, 0x17171818,
  0x00000000, 0x06060606, 0x00000000, 0x0B0B0A0A,
  0x00000000, 0x12121111, 0x00000000,
};

static void txpwr_init(void) {
  uint32_t a;
  int i = 0;
  w32(R_AX_PWR_UL_CTRL2, 0x07763333);
  w32(R_AX_PWR_COEXT_CTRL, 0x01ebf000);
  w32(R_AX_PWR_UL_CTRL0, 0x0002f8ff);
  mask32(R_AX_PWR_RATE_CTRL, B_AX_PWR_REF, 0);
  for (a = R_AX_PWR_BY_RATE_TABLE0; a <= R_AX_PWR_RU_LMT_TABLE11; a += 4) w32(a, txpwr_ch56[i++]);
  /* rtw8852bx_set_txpwr_ref com ref 0: TSSI_CW 172, PWR_CW 312 (OFDM e CCK, 2 caminhos) */
  {
    const uint32_t m = B_DPD_TSSI_CW | B_DPD_PWR_CW | B_DPD_REF;
    const uint32_t v = field(B_DPD_TSSI_CW, 172) | field(B_DPD_PWR_CW, 312) | field(B_DPD_REF, 0);
    rlog_hex("[rtw] txpwr ref antes 0x5804=", pr32(0x5804));
    rlog_hex("[rtw] txpwr ref antes 0x7804=", pr32(0x7804));
    pmask(0x5804, m, v);
    pmask(0x7804, m, v);
    pmask(0x5808, m, v);
    pmask(0x7808, m, v);
  }
}

/* rtw89_fw_h2c_cam: addr CAM 0 (nosso MAC como SMA, o AP como TMA) e BSSID CAM 0. */
static int cam_update(const uint8_t* mac, const uint8_t* bssid, int net_type, uint32_t aid) {
  uint8_t c[60];
  uint8_t sh = 0, th = 0;
  int i;
  bzero(c, sizeof(c));
  for (i = 0; i < 6; i++) {
    sh ^= mac[i];
    th ^= bssid[i];
  }
  put32(c + 4, (0u << 0) | (0u << 8) | (0x40u << 16));                  /* idx, offset, len */
  put32(c + 8, 1u | ((uint32_t)net_type << 1) | ((uint32_t)sh << 16) | ((uint32_t)th << 24));
  put32(c + 12, 0);                                                     /* BSSID CAM 0 */
  put32(c + 16, mac[0] | (mac[1] << 8) | (mac[2] << 16) | ((uint32_t)mac[3] << 24));
  put32(c + 20, mac[4] | (mac[5] << 8) | (bssid[0] << 16) | ((uint32_t)bssid[1] << 24));
  put32(c + 24, bssid[2] | (bssid[3] << 8) | (bssid[4] << 16) | ((uint32_t)bssid[5] << 24));
  put32(c + 32, 0);                                                     /* macid 0, porta 0 */
  put32(c + 36, aid & 0xFFFu);                                          /* AID12 */
  put32(c + 48, (0u << 0) | (0u << 8) | (8u << 16));                    /* BSSID idx, offset, len */
  put32(c + 52, 1u | (0x3Fu << 2) | ((uint32_t)bssid[0] << 16) | ((uint32_t)bssid[1] << 24));
  put32(c + 56, bssid[2] | (bssid[3] << 8) | (bssid[4] << 16) | ((uint32_t)bssid[5] << 24));
  return h2c_cmd(1, 0x6, 0x0, 0, 1, c, sizeof(c));
}

/* Depois da associacao: CAM e join info como INFRA (net_type 2) com o AID. */
int rtw_sta_connect(const uint8_t* mac, const uint8_t* bssid, uint32_t aid) {
  uint8_t p[4];
  uint32_t v;
  if (cam_update(mac, bssid, 2, aid)) return -1;
  put32(p, 0u | (0u << 8) | (2u << 24) | (1u << 26));
  if (h2c_cmd(1, 0x8, 0x0, 0, 1, p, 4)) return -1;
  v = r32(R_AX_PORT_CFG_P0);
  v = (v & ~B_AX_NET_TYPE_MASK) | field(B_AX_NET_TYPE_MASK, 2);
  w32(R_AX_PORT_CFG_P0, v);
  rlog_hex("[rtw] conectado, AID=", aid);
  return 0;
}

/* rtw89_mac_write_lte / read_lte: registradores do PTA (coexistencia com o BT). */
static int lte_wait(void) {
  int i;
  for (i = 0; i < 1000; i++) {
    if (r8(R_AX_LTE_CTRL + 3) & (1u << 5)) return 0;
    udelay_spin(50);
  }
  return -1;
}

static uint32_t lte_read(uint32_t off) {
  if (lte_wait()) rlog("[rtw] LTE nao pronto (R)\n");
  w32(R_AX_LTE_CTRL, 0x800F0000u | off);
  return r32(R_AX_LTE_RDATA);
}

static void lte_write(uint32_t off, uint32_t v) {
  if (lte_wait()) rlog("[rtw] LTE nao pronto (W)\n");
  w32(R_AX_LTE_WDATA, v);
  w32(R_AX_LTE_CTRL, 0xC00F0000u | off);
}

/* A antena e dividida com o BT: o WiFi so transmite com GNT_WL. Como o rtw89
 * faz sem BT ativo: caminho de controle no WiFi, GNT_WL = 1 e GNT_BT = 0 por
 * software nos dois caminhos, debug de GNT no RF desligado. */
static void coex_wl_only(void) {
  const uint32_t gnt = 0x77007700u;
  rlog_hex("[rtw] PTA LTE_SW_CFG_1 antes=", lte_read(R_AX_LTE_SW_CFG_1));
  w8(R_AX_SYS_SDIO_CTRL + 3, (uint8_t)(r8(R_AX_SYS_SDIO_CTRL + 3) | (1u << 2)));
  set8(R_AX_BTC_FUNC_EN, (uint8_t)B_AX_PTA_WL_TX_EN);
  lte_write(R_AX_LTE_SW_CFG_1, gnt);
  rf_write(0, RR_WLSEL, RFREG_MASK, 0x0);
  rf_write(1, RR_WLSEL, RFREG_MASK, 0x0);
  rlog_hex("[rtw] PTA LTE_SW_CFG_1 agora=", lte_read(R_AX_LTE_SW_CFG_1));
}

int rtw_sta_init(const uint8_t* mac, const uint8_t* bssid) {
  uint8_t p[68];
  int i;
  if (!dma_ready) return fail("radio nao ligado");
  for (i = 0; i < TXCH_N; i++) {
    if (!wd_buf[i]) wd_buf[i] = dma_alloc(TX_RING_N * TXWD_LEN, 4096);
    if (!txdata_buf[i]) txdata_buf[i] = dma_alloc(TX_RING_N * TX_DATA_BUF, 4096);
    if (!wd_buf[i] || !txdata_buf[i]) return fail("sem memoria para TX");
    tx_wp[i] = 0;
  }
  port_init();
  coex_wl_only();
  txpwr_init();
  /* role maintain: macid 0, cliente, criar, estacao */
  bzero(p, sizeof(p));
  put32(p, (0u << 0) | (0u << 8) | (0u << 10) | (1u << 13));
  if (h2c_cmd(1, 0x8, 0x4, 0, 1, p, 4)) return -1;
  /* join info desconectado: macid 0, porta 0, NO_LINK, estacao, cliente */
  put32(p, 0u | (1u << 8) | (1u << 26));
  if (h2c_cmd(1, 0x8, 0x0, 0, 1, p, 4)) return -1;
  /* macid 0 sem pausa (mask_grp[0] bit 0, pause 0) */
  bzero(p, 32);
  put32(p + 16, 1u);
  if (h2c_cmd(1, 0x9, 0x8, 1, 0, p, 32)) return -1;
  /* cmac table padrao do macid 0 (rtw89_fw_h2c_default_cmac_tbl) */
  bzero(p, sizeof(p));
  put32(p, 0u | (1u << 7));
  put32(p + 6 * 4, 2u << 16);                            /* NTX_PATH_EN = RF_B */
  put32(p + 13 * 4, 7u << 9);                            /* mascara TXPWR_MODE */
  put32(p + 14 * 4, (0xFu << 16) | (3u << 20) | (3u << 22) | (3u << 24) | (3u << 26) |
                    (1u << 28) | (1u << 29) | (1u << 30) | (1u << 31));
  put32(p + 15 * 4, (3u << 18) | (0xFu << 24));
  if (h2c_cmd(1, 0x5, 0x2, 0, 1, p, 68)) return -1;
  if (cam_update(mac, bssid, 0, 0)) return -1;
  rlog("[rtw] estacao criada no firmware (macid 0, CAM com MAC e BSSID)\n");
  return 0;
}

/* Um quadro 802.11 completo (sem FCS; a placa poe o FCS) a OFDM 6 Mbps.
 * ch = indice em txchs, ch_dma = canal no TXWD, qsel = fila da MAC,
 * hw_seq = a placa numera a sequencia (gerencia) ou fica a do quadro (dados). */
static int tx_frame(int ch, uint32_t ch_dma, uint32_t qsel, int hw_seq,
                    const uint8_t* frame, uint32_t len) {
  uint32_t slot = tx_wp[ch], next = (slot + 1u) % TX_RING_N;
  uint8_t* wd = wd_buf[ch] + slot * TXWD_LEN;
  uint8_t* data = txdata_buf[ch] + slot * TX_DATA_BUF;
  uint8_t* bd = tx_bd[ch] + slot * 8u;
  uint32_t seq;
  if (!wd_buf[ch]) return fail("TX nao preparado");
  if (len > TX_DATA_BUF || len < 24) return fail("quadro de TX invalido");
  if (((r32(txchs[ch].idx) >> 16) & 0xFFFu) == next) return fail("fila de TX cheia");
  bcopy(data, frame, len);
  seq = (((uint32_t)frame[22] | ((uint32_t)frame[23] << 8)) & 0xFFF0u) >> 4;
  bzero(wd, TXWD_LEN);
  /* body: WD_INFO, canal, WD_PAGE e (gerencia) SSN pela placa */
  put32(wd, (1u << 22) | (ch_dma << 16) | (1u << 7) | (hw_seq ? (1u << 2) | 1u : 0));
  put32(wd + 8, (0u << 24) | (qsel << 17) | (len & 0x3FFFu));   /* macid 0 */
  put32(wd + 12, seq & 0xFFFu);
  /* info: USE_RATE, OFDM6, DISDATAFB; RTS so para unicast */
  put32(wd + 24, (1u << 30) | (4u << 16) | (1u << 10));
  put32(wd + 24 + 16, (1u << 31) | ((frame[4] & 1u) ? 0 : (1u << 27)));
  /* wp_info + addr_info */
  put16(wd + 48, (slot & 0x7FFFu) | 0x8000u);
  put16(wd + 56, len);
  put16(wd + 58, (1u << 15) | 1u);
  put32(wd + 60, pa32(data));
  put16(bd, TXWD_LEN);
  put16(bd + 2, 1u << 14);
  put32(bd + 4, pa32(wd));
  __asm__ volatile ("mfence" ::: "memory");
  tx_wp[ch] = next;
  w16(txchs[ch].idx, (uint16_t)next);
  return 0;
}

/* Gerencia pela fila B0MG (CH8, QSEL 0x12). */
int rtw_tx_mgmt(const uint8_t* frame, uint32_t len) {
  return tx_frame(TXCH_B0MG, 8u, 0x12u, 1, frame, len);
}

/* Dados pela ACH0 (melhor esforco, QSEL BE_0). */
int rtw_tx_data(const uint8_t* frame, uint32_t len) {
  return tx_frame(0, 0u, 0x00u, 0, frame, len);
}

/* 1 quando a placa ja leu todos os BDs de gerencia enviados. */
int rtw_tx_mgmt_done(void) {
  return ((r32(txchs[TXCH_B0MG].idx) >> 16) & 0xFFFu) == tx_wp[TXCH_B0MG];
}

/* ---------- calibracoes de inicio: RCK e DACK (rtw8852b_rfk.c) ---------- */

/* rtw89_rfk_parser: {flag, caminho, endereco, mascara, valor}. */
static void rfk_run(const uint32_t (*t)[5], int n) {
  int i;
  for (i = 0; i < n; i++) {
    const uint32_t* r = t[i];
    if (r[0] == 0) rf_write((int)r[1], r[2], r[3], r[4]);
    else if (r[0] == 1) pmask(r[2], r[3], r[4]);
    else if (r[0] == 2) pset(r[2], r[3]);
    else if (r[0] == 3) pclr(r[2], r[3]);
    else udelay_spin(r[4]);
  }
}
#define RFK(t) rfk_run(t, (int)(sizeof(t) / sizeof(t[0])))

static void rck(int path) {
  uint32_t reg5 = rf_read(path, RR_RSV1, RFREG_MASK), v;
  int i;
  rf_write(path, RR_RSV1, RR_RSV1_RST, 0x0);
  rf_write(path, RR_MOD, RR_MOD_MASK, RR_MOD_V_RX);
  rf_write(path, RR_RCKC, RFREG_MASK, 0x00240);
  for (i = 0; i < 30 && !rf_read(path, RR_RCKS, 1u << 3); i++) udelay_spin(2);
  v = rf_read(path, RR_RCKC, RR_RCKC_CA);
  rf_write(path, RR_RCKC, RFREG_MASK, v);
  rf_write(path, RR_RSV1, RFREG_MASK, reg5);
  rlog_hex("[rtw] RCK ", ((uint32_t)path << 28) | v | (i == 30 ? 0x08000000u : 0));
}

static int poll_phy(uint32_t a, uint32_t mask, uint32_t us) {
  while (us--) {
    if (pget(a, mask)) return 0;
    udelay_spin(1);
  }
  return -1;
}

static void check_addc(int path) {
  int i;
  if (path == 0) RFK(rfk_check_addc_a);
  else RFK(rfk_check_addc_b);
  for (i = 0; i < 100; i++) (void)pr32(R_DBG32_D);
}

static void drck(void) {
  uint32_t v;
  pmask(R_DRCK_V1, B_DRCK_V1_KICK, 0x1);
  if (poll_phy(R_DRCK_RS, B_DRCK_RS_DONE, 10000)) rlog("[rtw] DRCK timeout\n");
  pmask(R_DRCK_V1, B_DRCK_V1_KICK, 0x0);
  pmask(R_DRCK_FH, B_DRCK_LAT, 0x1);
  udelay_spin(1);
  pmask(R_DRCK_FH, B_DRCK_LAT, 0x0);
  v = pget(R_DRCK_RS, B_DRCK_RS_LPS);
  pmask(R_DRCK_V1, B_DRCK_V1_SEL, 0x0);
  pmask(R_DRCK_V1, B_DRCK_V1_CV, v);
}

static void addck(void) {
  uint32_t a0, a1;
  /* S0 */
  pmask(R_ADDCK0, B_ADDCK0_MAN, 0x0);
  pmask(R_PATH1_SAMPL_DLY_T_V1, 0x30, 0x0);
  pmask(R_P0_NRBW, B_P0_NRBW_DBG, 0x1);
  pmask(R_ANAPAR, B_ANAPAR_ADCCLK, 0x0);
  pmask(R_ANAPAR, B_ANAPAR_FLTRST, 0x0);
  pmask(R_ANAPAR, B_ANAPAR_FLTRST, 0x1);
  pmask(R_ANAPAR_PW15, B_ANAPAR_PW15_H, 0xf);
  pmask(R_ANAPAR, B_ANAPAR_EN, 0x0);
  pmask(R_PATH0_SAMPL_DLY_T_V1, 1u << 1, 0x1);
  pmask(R_ANAPAR_PW15, B_ANAPAR_PW15_H, 0x3);
  check_addc(0);
  pmask(R_ADDCK0, B_ADDCK0_TRG, 0x1);
  pmask(R_ADDCK0, B_ADDCK0_TRG, 0x0);
  udelay_spin(1);
  pmask(R_ADDCK0, B_ADDCK0, 0x1);
  if (poll_phy(R_ADDCKR0, 1u, 10000)) rlog("[rtw] ADDCK S0 timeout\n");
  check_addc(0);
  pmask(R_PATH0_SAMPL_DLY_T_V1, 1u << 1, 0x0);
  pmask(R_ANAPAR, B_ANAPAR_EN, 0x1);
  pmask(R_ANAPAR_PW15, B_ANAPAR_PW15_H, 0xc);
  pmask(R_ANAPAR, B_ANAPAR_ADCCLK, 0x1);
  pmask(R_P0_NRBW, B_P0_NRBW_DBG, 0x0);
  /* S1 */
  pmask(R_P1_DBGMOD, B_P1_DBGMOD_ON, 0x1);
  pmask(R_ANAPAR, B_ANAPAR_ADCCLK, 0x0);
  pmask(R_ANAPAR, B_ANAPAR_FLTRST, 0x0);
  pmask(R_ANAPAR, B_ANAPAR_FLTRST, 0x1);
  pmask(R_ANAPAR_PW15, B_ANAPAR_PW15_H, 0xf);
  pmask(R_ANAPAR, B_ANAPAR_EN, 0x0);
  pmask(R_PATH1_SAMPL_DLY_T_V1, 1u << 1, 0x1);
  pmask(R_ANAPAR_PW15, B_ANAPAR_PW15_H, 0x3);
  check_addc(1);
  pmask(R_ADDCK1, B_ADDCK1_TRG, 0x1);
  pmask(R_ADDCK1, B_ADDCK1_TRG, 0x0);
  udelay_spin(1);
  pmask(R_ADDCK1, B_ADDCK1, 0x1);
  if (poll_phy(R_ADDCKR1, 1u, 10000)) rlog("[rtw] ADDCK S1 timeout\n");
  check_addc(1);
  pmask(R_PATH1_SAMPL_DLY_T_V1, 1u << 1, 0x0);
  pmask(R_ANAPAR, B_ANAPAR_EN, 0x1);
  pmask(R_ANAPAR_PW15, B_ANAPAR_PW15_H, 0xc);
  pmask(R_ANAPAR, B_ANAPAR_ADCCLK, 0x1);
  pmask(R_P1_DBGMOD, B_P1_DBGMOD_ON, 0x0);
  /* backup + reload (_addck_backup/_addck_reload) */
  pmask(R_ADDCK0, B_ADDCK0, 0x0);
  a0 = pget(R_ADDCKR0, B_ADDCKR0_A0);
  a1 = pget(R_ADDCKR0, B_ADDCKR0_A1);
  pmask(R_ADDCK0D, B_ADDCK0D_VAL, a0);
  pmask(R_ADDCK0, B_ADDCK0_VAL, a1 >> 6);
  pmask(R_ADDCK0D, B_ADDCK0D_VAL2, a1 & 0x3f);
  pmask(R_ADDCK0, B_ADDCK0_MAN, 0x3);
  pmask(R_ADDCK1, B_ADDCK1, 0x0);
  a0 = pget(R_ADDCKR1, B_ADDCKR1_A0);
  a1 = pget(R_ADDCKR1, B_ADDCKR1_A1);
  pmask(R_ADDCK1D, B_ADDCK1D_VAL, a0);
  pmask(R_ADDCK1, B_ADDCK0_VAL, a1 >> 6);
  pmask(R_ADDCK1D, B_ADDCK1D_VAL2, a1 & 0x3f);
  pmask(R_ADDCK1, B_ADDCK1_MAN, 0x3);
}

static int dack_s0_done(int part1) {
  if (part1) return pget(R_DACK_S0P0, B_DACK_S0P0_OK) && pget(R_DACK_S0P1, B_DACK_S0P1_OK);
  return pget(R_DACK_S0P2, B_DACK_S0P2_OK) && pget(R_DACK_S0P3, B_DACK_S0P3_OK);
}

static int dack_s1_done(int part1) {
  if (part1) return pget(R_DACK_S1P0, B_DACK_S1P0_OK) || pget(R_DACK_S1P1, B_DACK_S1P1_OK);
  return pget(R_DACK10S, B_DACK_S1P2_OK) || pget(R_DACK11S, B_DACK_S1P3_OK);
}

static void wait_dack(int (*done)(int), int part1, const char* what) {
  int i;
  for (i = 0; i < 10000 && !done(part1); i++) udelay_spin(1);
  if (i == 10000) {
    rlog("[rtw] DACK timeout: ");
    rlog(what);
    rlog("\n");
  }
}

/* _dac_cal: AFE, DRCK, ADDCK e DACK dos dois caminhos. */
static void dack(void) {
  uint32_t rf0 = rf_read(0, RR_MOD, RFREG_MASK), rf1 = rf_read(1, RR_MOD, RFREG_MASK);
  w32(R_AX_PHYREG_SET, 0xf);
  RFK(rfk_afe_init);
  drck();
  rf_write(0, RR_RSV1, RR_RSV1_RST, 0x0);
  rf_write(1, RR_RSV1, RR_RSV1_RST, 0x0);
  rf_write(0, RR_MOD, RFREG_MASK, 0x337e1);
  rf_write(1, RR_MOD, RFREG_MASK, 0x337e1);
  addck();
  rf_write(0, RR_MODOPT, RFREG_MASK, 0x0);
  rf_write(1, RR_MODOPT, RFREG_MASK, 0x0);
  RFK(rfk_dack_s0_1);
  wait_dack(dack_s0_done, 1, "S0 MSBK");
  RFK(rfk_dack_s0_2);
  wait_dack(dack_s0_done, 0, "S0 DADCK");
  RFK(rfk_dack_s0_3);
  pmask(R_P0_NRBW, B_P0_NRBW_DBG, 0x0);
  RFK(rfk_dack_s1_1);
  wait_dack(dack_s1_done, 1, "S1 MSBK");
  RFK(rfk_dack_s1_2);
  wait_dack(dack_s1_done, 0, "S1 DADCK");
  RFK(rfk_dack_s1_3);
  RFK(rfk_check_dadc_en_b);
  check_addc(1);
  RFK(rfk_check_dadc_dis_b);
  pmask(R_P1_DBGMOD, B_P1_DBGMOD_ON, 0x0);
  rf_write(0, RR_MOD, RFREG_MASK, rf0);
  rf_write(1, RR_MOD, RFREG_MASK, rf1);
  rf_write(0, RR_RSV1, RR_RSV1_RST, 0x1);
  rf_write(1, RR_RSV1, RR_RSV1_RST, 0x1);
  rlog("[rtw] DACK feito\n");
}

void rtw_rfk_init(void) {
  rck(0);
  rck(1);
  dack();
}

/* Copia 0x0000-0x1FFFF do BAR (como o mac_reg_dump do rtw89), para comparar
 * com o Linux (tools/rtw89_dump.py e tools/rtw89_regdiff.py). */
void rtw_dump_regs(uint8_t* out) {
  uint32_t a;
  if (!mmio) return;
  for (a = 0; a < RTW_DUMP_SIZE; a += 4) put32(out + a, r32(a));
}

/* Estado de erro da placa quando a rede para: pedido de SER do firmware
 * (HALT_C2H), ISRs, DLE e quanto os aneis de RX chegaram a encher. */
void rtw_log_stall(void) {
  uint32_t now = net_ms();
  int q;
  if (!mmio) return;
  rtw_log("      halt_c2h ");
  rtw_log_hex(r32(R_AX_HALT_C2H_CTRL));
  rtw_log(" ");
  rtw_log_hex(r32(R_AX_HALT_C2H));
  rtw_log(" hisr0 ");
  rtw_log_hex(r32(0x01A4u));
  rtw_log(" pcie_hisr00 ");
  rtw_log_hex(r32(0x10B4u));
  rtw_log(" dmac_err ");
  rtw_log_hex(r32(0x8524u));
  rtw_log(" cmac_err ");
  rtw_log_hex(r32(0xC164u));
  rtw_log("\n      ptcl_isr ");
  rtw_log_hex(r32(0xC6C4u));
  rtw_log(" sch_isr ");
  rtw_log_hex(r32(0xC3ECu));
  rtw_log(" rmac ");
  rtw_log_hex(r32(0xCEF4u));
  rtw_log(" tmac ");
  rtw_log_hex(r32(0xCCECu));
  rtw_log(" ptcl_imr ");
  rtw_log_hex(r32(0xC6C0u));
  rtw_log("\n      dle_empty ");
  rtw_log_hex(r32(0x8430u));
  rtw_log(" ");
  rtw_log_hex(r32(0x8434u));
  rtw_log(" dma_busy1 ");
  rtw_log_hex(r32(R_AX_PCIE_DMA_BUSY1));
  for (q = 0; q < 2; q++) {
    const uint8_t* b = rx_buf[q] + rx_wp[q] * RX_BUF_STRIDE;
    rtw_log(q ? " | RPQ" : " | RXQ");
    rtw_log(" max ");
    rtw_log_hex(rx_maxfill[q]);
    rtw_log(" ms sem rx ");
    rtw_log_hex(now - rx_last_ms[q]);
    rtw_log(" prox ");
    rtw_log_hex(le32(b + 4));
  }
  rtw_log("\n");
}

uint32_t rtw_reg(uint32_t a) { return mmio ? r32(a) : 0; }

/* Indices crus (placa << 16 | nosso) de RXQ, RPQ e ACH0. */
void rtw_ring_state(uint32_t* rxq, uint32_t* rpq, uint32_t* ach0) {
  *rxq = mmio ? r32(R_AX_RXQ_RXBD_IDX) : 0;
  *rpq = mmio ? r32(R_AX_RPQ_RXBD_IDX) : 0;
  *ach0 = mmio ? r32(txchs[0].idx) : 0;
}
