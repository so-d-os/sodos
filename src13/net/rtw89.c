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
static char logbuf[8192];
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
#define RX_RING_N 16
#define RX_BUF_SIZE 11494u
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
static uint8_t* rx_buf[2];      /* RX_RING_N buffers de RX_BUF_SIZE */
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
    rx_buf[i] = dma_alloc(RX_RING_N * RX_BUF_SIZE, 4096);
    if (!rx_bd[i] || !rx_buf[i]) return -1;
    for (j = 0; j < RX_RING_N; j++) {
      uint8_t* bd = rx_bd[i] + j * 8;
      put16(bd, RX_BUF_SIZE);
      put16(bd + 2, 0);
      put32(bd + 4, pa32(rx_buf[i] + (uint32_t)j * RX_BUF_SIZE));
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
