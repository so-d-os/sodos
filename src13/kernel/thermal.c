/* thermal.c - temperatura da CPU no log.
 *
 * O SODOS nao faz gestao termica (sem ACPI _TZ, sem P-states): com carga
 * pesada (WINE + Claude nas 8 CPUs) o PC ja desligou sem CRASH.LOG, o que
 * parece corte por temperatura (THERMTRIP) ou de energia. Esta thread le o
 * sensor a cada 10 s, escreve "[temp]" na serial (vai para o SERIAL.LOG) e
 * grava D:\LOG\TEMP.LOG a cada 60 s (a cada 2 s quando passa de TEMPMAX).
 * O arquivo e acrescentado, nao sobrescrito: depois de um desligamento, o
 * boot seguinte ainda mostra as ultimas leituras do anterior.
 *
 * Junto: temperatura do SSD (NVMe, log SMART), carga das CPUs e memoria
 * livre, na mesma linha. thermal_stat() da tudo ao TOP (aba Temp).
 *
 * Sensores:
 *  - AMD Zen (familias 17h/19h/1Ah): registrador SMN 0x59800 (THM_TCON_CUR_TMP)
 *    lido pelo indice/dado 0x60/0x64 da ponte 0:0.0 - so quando a ponte e
 *    AMD (vendor 0x1022): no Q35 do QEMU o 0x60 e o PCIEXBAR.
 *  - Intel com DTS (CPUID 6 EAX bit 0): MSR 0x1B1 (pacote) ou 0x19C (nucleo),
 *    TjMax do MSR 0x1A2.
 * Sem sensor da CPU a linha diz "CPU sem sensor"; SET TEMPLOG=0 desliga. */
#include <stdint.h>
#include "serial.h"
#include "kthread.h"
#include "drives.h"
#include "env.h"
#include "kexports.h"
#include "thermal.h"
#include "nvme.h"
#include "mm.h"
#include "smp.h"
#include "xhci.h"   /* pci_mmio_map */
#include "smuprobe.h"

int rtc_time(void);
int serial_log_save(void);
uint32_t net_ms(void);

enum { TS_NONE, TS_AMD_SMN, TS_INTEL_PKG, TS_INTEL_CORE };
static int ts_kind;
static int ts_tjmax = 100;

static inline void cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
  __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}
static inline void outl_t(uint16_t p, uint32_t v) { __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl_t(uint16_t p) { uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

/* Config PCI do 0:0.0 (mecanismo 1). Chamado com o BKL: nvme/xhci usam o
 * mesmo par 0xCF8/0xCFC. */
static uint32_t pci00_read(uint8_t off) {
  outl_t(0xCF8, 0x80000000u | (off & 0xFCu));
  return inl_t(0xCFC);
}
static void pci00_write(uint8_t off, uint32_t v) {
  outl_t(0xCF8, 0x80000000u | (off & 0xFCu));
  outl_t(0xCFC, v);
}

static uint32_t ts_fam, ts_model, ts_bridge;
/* AMD: sensores por CCD (como o k10temp), ultimo valor cru e "parado". */
static uint32_t ts_ccd_base, ts_ccd_mask;
static uint32_t ts_raw, ts_raw_ccd;       /* Tctl cru; soma dos CCD crus */
static int ts_tctl = HW_NONE, ts_ccd = HW_NONE;
static uint32_t ts_change_ms;             /* ultima vez que a leitura mudou */
static int ts_seen;
static int ts_stale;                      /* 1 = sensor parado (nao confiar) */

/* Registrador SMN pelo indice/dado 0x60/0x64 do 0:0.0 (com o BKL). */
static uint32_t smn_read(uint32_t addr) {
  uint32_t save = pci00_read(0x60), v;
  pci00_write(0x60, addr);
  v = pci00_read(0x64);
  pci00_write(0x60, save);
  return v;
}
static char ts_vendor[13];

static void ts_detect(void) {
  uint32_t a, b, c, d, maxleaf, fam, vb, vc, vd;
  /* o nome do fabricante fica em vb/vd/vc: o CPUID 1 abaixo usa b/c/d */
  cpuid(0, &maxleaf, &vb, &vc, &vd);
  cpuid(1, &a, &b, &c, &d);
  { int i; for (i = 0; i < 4; i++) { ts_vendor[i] = (char)(vb >> (8 * i)); ts_vendor[4 + i] = (char)(vd >> (8 * i)); ts_vendor[8 + i] = (char)(vc >> (8 * i)); } ts_vendor[12] = 0; }
  fam = (a >> 8) & 0xF;
  ts_model = (a >> 4) & 0xF;
  if (fam == 0xF) {
    fam += (a >> 20) & 0xFF;
    ts_model |= ((a >> 16) & 0xF) << 4;
  }
  ts_fam = fam;
  ts_bridge = pci00_read(0);
  if (vb == 0x68747541u && vd == 0x69746E65u && vc == 0x444D4163u) {   /* AuthenticAMD */
    if (fam >= 0x17 && (ts_bridge & 0xFFFFu) == 0x1022u) {
      uint32_t m = ts_model, i;
      ts_kind = TS_AMD_SMN;
      /* deslocamento dos CCD como no k10temp (Zen2/3: 0x154; Zen4+: 0x300) */
      if (fam == 0x17) ts_ccd_base = (m >= 0x30 && m <= 0x3F) || m == 0x71 || m == 0x01 || m == 0x08 || m == 0x11 || m == 0x18 ? 0x59954u : 0;
      else if (fam == 0x19) {
        if ((m >= 0x10 && m <= 0x1F) || (m >= 0x60 && m <= 0x7F) || (m >= 0xA0 && m <= 0xAF))
          ts_ccd_base = 0x59800u + (m >= 0x60 && m <= 0x7F ? 0x308u : 0x300u);
        else ts_ccd_base = 0x59954u;            /* Zen3: 5825U (modelo 0x50) etc. */
      } else if (fam == 0x1A) ts_ccd_base = 0x59B08u;
      for (i = 0; ts_ccd_base && i < 8; i++)
        if (smn_read(ts_ccd_base + 4u * i) & 0x800u) ts_ccd_mask |= 1u << i;
    }
    return;
  }
  if (vb == 0x756E6547u && vd == 0x49656E69u && vc == 0x6C65746Eu && maxleaf >= 6) {  /* GenuineIntel */
    cpuid(6, &a, &b, &c, &d);
    if (!(a & 1u)) return;                       /* sem DTS: rdmsr daria #GP */
    ts_kind = (a & (1u << 6)) ? TS_INTEL_PKG : TS_INTEL_CORE;
    {
      int tj = (int)((rdmsr(0x1A2) >> 16) & 0xFF);
      if (tj >= 70 && tj <= 130) ts_tjmax = tj;
    }
  }
}

/* Graus C (inteiro); -1000 se nao deu para ler. Com o BKL. */
static int ts_read(void) {
  if (ts_kind == TS_AMD_SMN) {
    uint32_t v = smn_read(0x00059800u), mc, i, sum = 0;
    int t, best = HW_NONE;
    mc = ((v >> 21) & 0x7FFu) * 125u;            /* 0,125 C por unidade */
    t = (int)(mc / 1000u);
    if (v & 0x80000u) t -= 49;                   /* faixa -49..206 */
    for (i = 0; i < 8; i++) {
      uint32_t r;
      int c;
      if (!((ts_ccd_mask >> i) & 1u)) continue;
      r = smn_read(ts_ccd_base + 4u * i);
      sum += r;
      if (!(r & 0x800u)) continue;
      c = (int)((r & 0x7FFu) * 125u / 1000u) - 49;
      if (c > 0 && c < 150 && c > best) best = c;
    }
    /* sensor parado: o Tctl cru e os CCD sem mudar nada por 60 s */
    {
      /* O "| 1" antigo (para 0 nao valer "nunca") punha change = agora + 1:
       * agora - change dava 0xFFFFFFFF e o sensor aparecia PARADO logo
       * depois de mudar (build 36: SENSOR PARADO piscando, CPUs caindo a 4). */
      uint32_t now = net_ms();
      if (v != ts_raw || sum != ts_raw_ccd || !ts_seen) {
        ts_change_ms = now;
        ts_seen = 1;
      }
      ts_stale = (int32_t)(now - ts_change_ms) >= 60000;
    }
    ts_raw = v;
    ts_raw_ccd = sum;
    ts_tctl = t;
    ts_ccd = best;
    return best > t ? best : t;                  /* o pior dos dois */
  }
  if (ts_kind == TS_INTEL_PKG || ts_kind == TS_INTEL_CORE) {
    uint64_t s = rdmsr(ts_kind == TS_INTEL_PKG ? 0x1B1 : 0x19C);
    if (ts_kind == TS_INTEL_CORE && !(s & (1ull << 31))) return HW_NONE;
    return ts_tjmax - (int)((s >> 16) & 0x7F);
  }
  return HW_NONE;
}

/* ---- texto ---- */
#define TLOG_MAX (64 * 1024)
static char tlog[TLOG_MAX + 512];
static int tlog_n;

static void tl_add(const char* s) {
  int ll = 0, i;
  while (s[ll]) ll++;
  if (tlog_n + ll > TLOG_MAX) {               /* corta linhas velhas do comeco */
    int cut = tlog_n + ll - TLOG_MAX;
    while (cut < tlog_n && tlog[cut - 1] != '\n') cut++;
    for (i = cut; i < tlog_n; i++) tlog[i - cut] = tlog[i];
    tlog_n -= cut;
  }
  for (i = 0; i < ll; i++) tlog[tlog_n++] = s[i];
}
static void s_cat(char* d, const char* s) { while (*d) d++; while ((*d++ = *s++)) {} }
static void s_num(char* d, int v, int w) {
  char b[12];
  int i = 0, neg = v < 0;
  unsigned u = neg ? (unsigned)-v : (unsigned)v;
  do { b[i++] = (char)('0' + u % 10); u /= 10; } while (u && i < 11);
  while (i < w) b[i++] = '0';
  while (*d) d++;
  if (neg) *d++ = '-';
  while (i > 0) *d++ = b[--i];
  *d = 0;
}
static void s_hms(char* d) {
  int t = rtc_time();
  if (t < 0) { s_cat(d, "--:--:--"); return; }
  s_num(d, t / 10000, 2); s_cat(d, ":");
  s_num(d, t / 100 % 100, 2); s_cat(d, ":");
  s_num(d, t % 100, 2);
}

static volatile int th_word;
static struct k_hwstat hw;               /* mexido so com o BKL */
static uint32_t prev_busy, prev_tot;

static void hist_push(signed char* h, int v) {
  int i;
  if (v == HW_NONE) v = -128;
  if (v > 127) v = 127;
  if (v < -127 && v != -128) v = -127;
  for (i = 0; i + 1 < HW_HIST; i++) h[i] = h[i + 1];
  h[HW_HIST - 1] = (signed char)v;
}

/* Carga das CPUs desde a ultima chamada (%), pelo tempo ocupado/ocioso. */
static int load_now(void) {
  uint32_t busy = 0, tot = 0, b, id, db, dt;
  int i, n = kt_online_count();
  for (i = 0; i < n && i < 64; i++) {
    b = id = 0;
    kt_cpu_times(i, &b, &id);
    busy += b;
    tot += b + id;
  }
  db = busy - prev_busy;
  dt = tot - prev_tot;
  prev_busy = busy;
  prev_tot = tot;
  hw.ncpu = n;
  return dt ? (int)((uint64_t)db * 100u / dt) : 0;
}

static void mem_now(void) {
  uint32_t ut = 0, uf = 0;
  uint64_t ht = 0, hf = 0;
  um_stats(&ut, 0, &uf, 0);
  hm_stats(&ht, 0, &hf, 0);
  hw.mem_total_mb = (uint32_t)((ut + ht) >> 20);
  hw.mem_free_mb = (uint32_t)((uf + hf) >> 20);
}

/* "1,9" GB com uma casa */
static void s_gb(char* d, uint32_t mb) {
  s_num(d, (int)(mb / 1024u), 1);
  s_cat(d, ",");
  s_num(d, (int)(mb % 1024u * 10u / 1024u), 1);
}

/* ---- placas de video ----
 * AMD Polaris (RX 470/480/570/580/590, device 67Cx-67Fx, 6FDF, 698x-699x):
 *   registrador do SMC ixCG_MULT_THERMAL_STATUS (0xC0300014), campo CTF_TEMP
 *   (bits 17:9, graus C; bit 9 do campo = acima da faixa), lido pelo par
 *   SMC_IND_INDEX_11/DATA_11 (dwords 0x1AC/0x1AD do BAR5), como o
 *   smu7_thermal_get_temperature do amdgpu. O amdgop usa o mesmo par: tudo
 *   com o BKL e o indice volta ao valor de antes.
 * NVIDIA Pascal em diante (device 15xx-1Fxx: Tesla P40 = 1B38): BAR0 +
 *   0x020460, bit 29 = leitura valida, graus = (v & 0x1FFF8) >> 8, como o
 *   gp100_temp_get do nouveau.
 * So le placa com o MMIO ligado no comando PCI (bit 1): o kernel nao liga. */
struct gpu_dev {
  int kind;                    /* 1 AMD Polaris, 2 NVIDIA */
  volatile uint8_t* mmio;
};
static struct gpu_dev gpus[HW_GPUS];

static uint32_t pci_rd(uint32_t bus, uint32_t dev, uint32_t fun, uint32_t off) {
  outl_t(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fun << 8) | (off & 0xFCu));
  return inl_t(0xCFC);
}

static int amd_polaris(uint32_t d) {
  return (d >= 0x67C0u && d <= 0x67FFu) || d == 0x6FDFu || (d >= 0x6980u && d <= 0x699Fu);
}
static int nv_pascal_up(uint32_t d) { return d >= 0x1500u && d <= 0x1FFFu; }

static void gpu_name(char* o, uint32_t ven, uint32_t d) {
  static const char hx[] = "0123456789ABCDEF";
  const char* n = 0;
  int i;
  if (ven == 0x10DEu && d == 0x1B38u) n = "Tesla P40";
  else if (ven == 0x1002u && d == 0x67DFu) n = "RX 570/580";
  else if (ven == 0x1002u && d == 0x6FDFu) n = "RX 580 2048";
  else if (ven == 0x1002u && d == 0x67EFu) n = "RX 460/560";
  if (n) {
    for (i = 0; n[i] && i < 15; i++) o[i] = n[i];
    o[i] = 0;
    return;
  }
  s_cat(o, ven == 0x10DEu ? "NVIDIA " : "AMD ");
  for (i = 0; o[i]; i++) {}
  o[i++] = hx[(d >> 12) & 15]; o[i++] = hx[(d >> 8) & 15];
  o[i++] = hx[(d >> 4) & 15]; o[i++] = hx[d & 15]; o[i] = 0;
}

static void gpu_add(uint32_t bus, uint32_t dev, uint32_t fun, uint32_t id) {
  uint32_t ven = id & 0xFFFFu, did = id >> 16, kind, cmd, lo, hi = 0, off;
  uint64_t base;
  int k = hw.gpu_n;
  if (k >= HW_GPUS) return;
  if (ven == 0x1002u && amd_polaris(did)) { kind = 1; off = 0x24; }        /* BAR5 */
  else if (ven == 0x10DEu && nv_pascal_up(did)) { kind = 2; off = 0x10; }  /* BAR0 */
  else return;
  cmd = pci_rd(bus, dev, fun, 0x04);
  lo = pci_rd(bus, dev, fun, off);
  if ((lo & 6u) == 4u) hi = pci_rd(bus, dev, fun, off + 4);
  base = ((uint64_t)hi << 32) | (lo & ~0xFull);
  serial_puts("[temp] gpu ");
  serial_hex32(id);
  serial_puts(" bar ");
  serial_hex32((uint32_t)(base >> 32));
  serial_hex32((uint32_t)base);
  if (!(cmd & 2u) || !base || (lo & 1u)) {
    serial_puts(" -> MMIO desligado, sem leitura\n");
    return;
  }
  if (pci_mmio_map(base + (kind == 2 ? 0x20000u : 0u), 0x1000u) != 0) {
    serial_puts(" -> nao mapeou\n");
    return;
  }
  serial_puts("\n");
  gpus[k].kind = (int)kind;
  gpus[k].mmio = (volatile uint8_t*)(uintptr_t)base;
  hw.gpu_id[k] = ven << 16 | did;
  hw.gpu_name[k][0] = 0;
  gpu_name(hw.gpu_name[k], ven, did);
  hw.gpu_c[k] = hw.gpu_max_c[k] = HW_NONE;
  hw.gpu_n = k + 1;
}

static void gpu_walk(uint32_t bus, int depth) {
  uint32_t dev, fun;
  for (dev = 0; dev < 32; dev++) {
    uint32_t nfun = 1;
    for (fun = 0; fun < nfun; fun++) {
      uint32_t id = pci_rd(bus, dev, fun, 0), hdr, cls;
      if ((id & 0xFFFFu) == 0xFFFFu) continue;
      hdr = (pci_rd(bus, dev, fun, 0x0C) >> 16) & 0xFFu;
      if (fun == 0 && (hdr & 0x80u)) nfun = 8;
      if ((hdr & 0x7Fu) == 1u) {
        uint32_t sec = (pci_rd(bus, dev, fun, 0x18) >> 8) & 0xFFu;
        if (sec > bus && depth < 8) gpu_walk(sec, depth + 1);
        continue;
      }
      cls = pci_rd(bus, dev, fun, 0x08) >> 24;
      if (cls == 0x03u) gpu_add(bus, dev, fun, id);
    }
  }
}

static uint32_t mr32(volatile uint8_t* b, uint32_t off) { return *(volatile uint32_t*)(b + off); }
static void mw32(volatile uint8_t* b, uint32_t off, uint32_t v) { *(volatile uint32_t*)(b + off) = v; }

/* Graus C da placa i; HW_NONE sem leitura. Com o BKL. */
static int gpu_read(int i) {
  volatile uint8_t* b = gpus[i].mmio;
  if (!b) return HW_NONE;
  if (gpus[i].kind == 1) {
    uint32_t acc = mr32(b, 0x92u * 4u), idx = mr32(b, 0x1ACu * 4u), v, t;
    if (acc == 0xFFFFFFFFu) return HW_NONE;          /* placa sumiu / dormindo */
    if (acc & 0x800u) mw32(b, 0x92u * 4u, acc & ~0x800u);   /* sem auto-incremento */
    mw32(b, 0x1ACu * 4u, 0xC0300014u);               /* ixCG_MULT_THERMAL_STATUS */
    v = mr32(b, 0x1ADu * 4u);
    mw32(b, 0x1ACu * 4u, idx);
    if (acc & 0x800u) mw32(b, 0x92u * 4u, acc);
    t = (v >> 9) & 0x3FFu;                           /* CTF_TEMP */
    if (t & 0x200u) return 255;
    t &= 0x1FFu;
    return (t > 0 && t < 200) ? (int)t : HW_NONE;
  }
  if (gpus[i].kind == 2) {
    uint32_t v = mr32(b, 0x020460u);
    if (v == 0xFFFFFFFFu || !(v & 0x20000000u)) return HW_NONE;
    return (int)((v & 0x0001FFF8u) >> 8);
  }
  return HW_NONE;
}

static void gpu_sample(int hist) {
  int i;
  for (i = 0; i < hw.gpu_n; i++) {
    int t = gpu_read(i);
    hw.gpu_c[i] = t;
    if (t != HW_NONE && (hw.gpu_max_c[i] == HW_NONE || t > hw.gpu_max_c[i])) hw.gpu_max_c[i] = t;
    if (hist) hist_push(hw.gpu_hist[i], t);
  }
}

/* Uma amostra (com o BKL): le tudo e guarda no historico. */
static void sample(void) {
  int t = ts_read(), ssd = nvme_temp_c();
  hw.cpu_c = t;
  if (t != HW_NONE) {
    if (hw.cpu_max_c == HW_NONE || t > hw.cpu_max_c) hw.cpu_max_c = t;
    if (hw.cpu_min_c == HW_NONE || t < hw.cpu_min_c) hw.cpu_min_c = t;
    if (t >= 90) hw.hot_count++;
  }
  hw.ssd_c = ssd;
  if (ssd != HW_NONE && (hw.ssd_max_c == HW_NONE || ssd > hw.ssd_max_c)) hw.ssd_max_c = ssd;
  hw.load_pct = load_now();
  mem_now();
  hw.uptime_s = net_ms() / 1000u;
  hist_push(hw.cpu_hist, t);
  hist_push(hw.ssd_hist, ssd);
  hist_push(hw.load_hist, hw.load_pct);
  gpu_sample(1);
  if (hw.hist_n < HW_HIST) hw.hist_n++;
}

/* ---- protecao termica ----
 * O SODOS nao tem P-states nem ACPI _TZ: com WINE+Claude e Firefox nas 8 CPUs
 * o notebook desligou sozinho. A causa era o SMU parado por IPI broadcast
 * (smp.c); com o SMU vivo o firmware baixa o clock sozinho e isto e so o
 * ultimo recurso: a cada 2 s, a partir de TEMPMAX (95 C) as CPUs de cima
 * ficam estacionadas em hlt (kt_set_cpu_cap), sem descer de THERMMIN (4);
 * abaixo de TEMPMAX-8 voltam uma por vez a cada 10 s. SMU morto (Tctl e
 * RAPL parados): limite THERMSAFE (4), piso 2. SET THERMCAP=n fixa um
 * maximo; SET THERMCAP=0 desliga a protecao. */
#define TH_MAX_DEF 95                     /* 5825U: Tjmax 95; o SMU ja segura antes */
static int th_max_c = TH_MAX_DEF, th_cap_env = -1, th_safe = 4, th_min = 4, th_cap;

static int env_int(const char* name, int def) {
  const char* e = env_get(name);
  int v = 0;
  if (!e || !*e) return def;
  while (*e >= '0' && *e <= '9') v = v * 10 + (*e++ - '0');
  return v;
}

static void s_hex8(char* d, uint32_t v) {
  static const char hx[] = "0123456789ABCDEF";
  int i;
  while (*d) d++;
  for (i = 7; i >= 0; i--) *d++ = hx[(v >> (4 * i)) & 15];
  *d = 0;
}

/* ---- potencia (RAPL) ----
 * AMD Zen: MSR 0xC0010299 (unidade: 1/2^ESU J, bits 12:8) e 0xC001029B
 * (energia do pacote, 32 bits). O contador e mantido pelo SMU, como o Tctl:
 * se os dois param, o SMU parou (sem ele o EC nao ve a temperatura e a
 * ventoinha nao sobe). Em 2026-11-21 o Tctl ficou parado no notebook
 * (828B0000) e o PC resetou com 4 CPUs. So liga com CPUID 0x80000007 EDX
 * bit 14: sem ele o rdmsr daria #GP (o KVM nao mostra). */
static int rapl_ok;
static uint32_t rapl_esu, rapl_last, rapl_last_ms, rapl_change_ms;
static int rapl_mw = HW_NONE;            /* potencia do pacote, mW */
static int rapl_stale;

static void rapl_detect(void) {
  uint32_t a, b, c, d;
  if (ts_kind != TS_AMD_SMN) return;
  cpuid(0x80000000u, &a, &b, &c, &d);
  if (a < 0x80000007u) return;
  cpuid(0x80000007u, &a, &b, &c, &d);
  if (!(d & (1u << 14))) return;
  rapl_esu = (uint32_t)(rdmsr(0xC0010299u) >> 8) & 0x1Fu;
  rapl_last = (uint32_t)rdmsr(0xC001029Bu);
  rapl_last_ms = rapl_change_ms = net_ms();
  rapl_ok = 1;
}

static void rapl_sample(void) {
  uint32_t e, now, dt, de;
  if (!rapl_ok) return;
  e = (uint32_t)rdmsr(0xC001029Bu);
  now = net_ms();
  dt = now - rapl_last_ms;
  if (dt < 500u) return;
  de = e - rapl_last;
  if (de) rapl_change_ms = now;
  rapl_stale = now - rapl_change_ms >= 10000u;
  rapl_mw = (int)((((uint64_t)de * 1000000u) >> rapl_esu) / dt);
  rapl_last = e;
  rapl_last_ms = now;
}

/* Novo limite de CPUs para a temperatura t; devolve 1 se mudou. */
static int th_guard(int t, int full_step) {
  int n = kt_online_count(), lim = n, cap = th_cap ? th_cap : n, floor, smu_dead;
  if (full_step) {                    /* SET em execucao vale em ate 10 s */
    th_max_c = env_int("TEMPMAX", TH_MAX_DEF);
    if (th_max_c < 60 || th_max_c > 100) th_max_c = TH_MAX_DEF;
    th_cap_env = env_int("THERMCAP", -1);
    th_safe = env_int("THERMSAFE", 4);
    th_min = env_int("THERMMIN", 4);
  }
  if (th_cap_env == 0) {              /* protecao desligada: tudo liberado */
    if (th_cap == n) return 0;
    th_cap = n;
    kt_set_cpu_cap(KT_CPU_MAX);
    return 1;
  }
  /* SMU morto (Tctl parado e RAPL parado ou ausente): ninguem mais segura
   * a temperatura, entao THERMSAFE e piso de 2. SMU vivo: o firmware ja
   * baixa o clock perto do limite; aqui so um ultimo recurso, sem descer de
   * THERMMIN (4) - com 2 CPUs o Claude (Electron/WINE) passava metade da
   * CPU1 trocando de thread (build 36, PROF 12:40). */
  smu_dead = ts_stale && (!rapl_ok || rapl_stale);
  floor = smu_dead ? 2 : th_min;
  if (floor < 2) floor = 2;
  if (th_cap_env > 0 && th_cap_env < lim) lim = th_cap_env;
  if (smu_dead && th_safe > 0 && th_safe < lim) lim = th_safe;
  if (lim < 2) lim = n < 2 ? n : 2;
  if (floor > lim) floor = lim;
  if (t != HW_NONE && !ts_stale) {
    if (t >= th_max_c + 5) cap = floor;
    else if (t >= th_max_c) cap -= 2;
    else if (t <= th_max_c - 8 && full_step) cap++;
  } else if (full_step) {
    cap++;
  }
  if (cap < floor) cap = floor;
  if (cap > lim) cap = lim;
  if (cap == th_cap) return 0;
  th_cap = cap;
  kt_set_cpu_cap(cap);
  return 1;
}

static void th_line(char* line, int t) {
  int g;
  line[0] = 0;
  s_cat(line, "[temp] ");
  s_hms(line);
  if (t != HW_NONE) {
    s_cat(line, " CPU ");
    s_num(line, t, 1);
    s_cat(line, " C (max ");
    s_num(line, hw.cpu_max_c, 1);
    s_cat(line, ")");
    if (ts_kind == TS_AMD_SMN) {
      s_cat(line, " Tctl ");
      s_num(line, ts_tctl, 1);
      if (ts_ccd != HW_NONE) {
        s_cat(line, " CCD ");
        s_num(line, ts_ccd, 1);
      }
      s_cat(line, " [");
      s_hex8(line, ts_raw);
      s_cat(line, "]");
      if (ts_stale) s_cat(line, " SENSOR PARADO");
    }
  } else {
    s_cat(line, " CPU sem sensor");
  }
  if (rapl_ok) {
    if (rapl_stale) {
      s_cat(line, " RAPL PARADO");
    } else if (rapl_mw != HW_NONE) {
      s_cat(line, " pacote ");
      s_num(line, rapl_mw / 1000, 1);
      s_cat(line, ",");
      s_num(line, rapl_mw % 1000 / 100, 1);
      s_cat(line, " W");
    }
    if (ts_stale && rapl_stale) s_cat(line, " SMU PARADO?");
  }
  if (hw.ssd_c != HW_NONE) {
    s_cat(line, " SSD ");
    s_num(line, hw.ssd_c, 1);
    s_cat(line, " C");
  }
  for (g = 0; g < hw.gpu_n; g++) {
    s_cat(line, " GPU");
    s_num(line, g, 1);
    s_cat(line, " ");
    s_cat(line, hw.gpu_name[g]);
    s_cat(line, " ");
    if (hw.gpu_c[g] == HW_NONE) {
      s_cat(line, "--");
    } else {
      s_num(line, hw.gpu_c[g], 1);
      s_cat(line, " C");
    }
  }
  s_cat(line, " carga ");
  s_num(line, hw.load_pct, 1);
  s_cat(line, "% mem livre ");
  s_gb(line, hw.mem_free_mb);
  s_cat(line, "/");
  s_gb(line, hw.mem_total_mb);
  s_cat(line, " GB cpus ");
  s_num(line, kt_cpu_cap(), 1);
  s_cat(line, "/");
  s_num(line, kt_online_count(), 1);
  s_cat(line, t != HW_NONE && t >= th_max_c ? " QUENTE\n" : "\n");
}

#define TH_FAST 2                         /* segundos entre checagens */
/* SET LOGEVERY=s: de quanto em quanto tempo o TEMP.LOG e o SERIAL.LOG vao
 * para o disco (padrao 300 s; 30..3600). Antes eram 60 s e 30 s: o SERIAL.LOG
 * (ate 1,2 MB) era regravado inteiro a cada 30 s segurando o BKL, e a luz do
 * disco nao apagava. Evento do guarda termico grava na hora; quente, a cada
 * 60 s no maximo. REBOOT e queda gravam como antes. */
#define TH_LOG_DEF 300

static int thermal_thread(void* arg) {
  int ticks = 0, sub = 0, last_logged = HW_NONE, was_stale = 0, was_rstale = 0;
  int every = env_int("LOGEVERY", TH_LOG_DEF), tl_dirty = 0;
  uint32_t tl_saved = 0, sl_saved = 0;
  (void)arg;
  if (every < 30 || every > 3600) every = TH_LOG_DEF;
  for (;;) {
    char line[384];
    int t, hot, g, full = sub == 0, changed;
    kt_bkl_enter_kernel();
    if (full) {
      sample();
      t = hw.cpu_c;
    } else {
      t = ts_kind ? ts_read() : HW_NONE;  /* so a CPU, sem historico */
      if (t != HW_NONE) {
        hw.cpu_c = t;
        if (t > hw.cpu_max_c) hw.cpu_max_c = t;
      }
    }
    rapl_sample();
    changed = th_guard(t, full);
    hot = t != HW_NONE && t >= th_max_c;
    if (full)
      for (g = 0; g < hw.gpu_n; g++)
        if (hw.gpu_c[g] != HW_NONE && hw.gpu_c[g] >= 90) hot = 1;
    if (ts_stale != was_stale || rapl_stale != was_rstale) changed = 1;
    was_stale = ts_stale;
    was_rstale = rapl_stale;
    if (full || changed) {
      th_line(line, t);
      /* serial: a cada 60 s, ou na hora se mudou 5 C, esta quente ou o
       * limite de CPUs mudou */
      if (changed || (full && (ticks % 6 == 0 || hot || t - last_logged >= 5 || last_logged - t >= 5))) {
        serial_puts(line);
        tl_add(line + 7);                       /* sem o "[temp] " no arquivo */
        last_logged = t;
      }
      if (changed || (full && (ticks % 6 == 0 || hot || t - last_logged >= 5 || last_logged - t >= 5)))
        tl_dirty = 1;
    }
    {
      uint32_t now = net_ms();
      uint32_t gap = now - tl_saved;
      if (tl_dirty && (changed || (hot && gap >= 60000u) || gap >= (uint32_t)every * 1000u) &&
          drives_is_mounted('D')) {
        (void)file_save("D:\\LOG\\TEMP.LOG", tlog, tlog_n);
        tl_dirty = 0;
        tl_saved = now;
      }
      /* SERIAL.LOG: so se mudou (serial_log_save olha) e no maximo a cada
       * LOGEVERY s; antes de programa rodar, o module.c grava tambem */
      if (now - sl_saved >= (uint32_t)every * 1000u) {
        (void)serial_log_save();
        sl_saved = now;
      }
    }
    kt_bkl_leave();
    if (full) ticks++;
    if (++sub >= HW_STEP / TH_FAST) sub = 0;
    kt_futex_wait(&th_word, 0, TH_FAST * 1000);
  }
  return 0;
}

int thermal_stat(struct k_hwstat* out) {
  uint8_t* d = (uint8_t*)out;
  const uint8_t* s = (const uint8_t*)&hw;
  unsigned i;
  if (!out) return -1;
  if (hw.sensor) {
    int t = ts_read();                          /* o export ja tem o BKL */
    if (t != HW_NONE) {
      hw.cpu_c = t;
      if (t > hw.cpu_max_c) hw.cpu_max_c = t;
    }
  }
  gpu_sample(0);                                /* na hora, sem historico */
  mem_now();
  hw.uptime_s = net_ms() / 1000u;
  for (i = 0; i < sizeof hw; i++) d[i] = s[i];
  return hw.hist_n ? 0 : -1;
}

/* Fim do boot (D: ja montado, AUTOEXEC ja rodou). */
void thermal_start(void) {
  const char* e = env_get("TEMPLOG");
  char line[192];
  int n, t;
  hw.cpu_c = hw.cpu_max_c = hw.cpu_min_c = HW_NONE;
  hw.ssd_c = hw.ssd_max_c = HW_NONE;
  hw.step_s = HW_STEP;
  th_max_c = env_int("TEMPMAX", TH_MAX_DEF);
  if (th_max_c < 60 || th_max_c > 100) th_max_c = TH_MAX_DEF;
  th_cap_env = env_int("THERMCAP", -1);
  th_safe = env_int("THERMSAFE", 4);
  if (e && e[0] == '0') return;
  kt_bkl_enter_kernel();
  ts_detect();
  hw.sensor = ts_kind == TS_AMD_SMN ? 1 : ts_kind ? 2 : 0;
  t = ts_kind ? ts_read() : HW_NONE;
  rapl_detect();
  (void)load_now();                             /* zera a base da carga */
  gpu_walk(0, 0);                               /* RX 4xx/5xx e NVIDIA Pascal+ */
  kt_bkl_leave();
  serial_puts("[temp] cpu ");
  serial_puts(ts_vendor);
  serial_puts(" familia ");
  serial_hex32(ts_fam);
  serial_puts(" ponte 0:0.0 ");
  serial_hex32(ts_bridge);
  serial_puts(ts_kind ? "\n" : " -> sem sensor de temperatura da CPU conhecido\n");
  if (drives_is_mounted('D')) {
    (void)drives_mkdir("D:\\LOG");
    n = file_load("D:\\LOG\\TEMP.LOG", tlog, TLOG_MAX);
    tlog_n = n > 0 ? n : 0;
  }
  line[0] = 0;
  s_cat(line, "===== boot ");
  s_hms(line);
  s_cat(line, ts_kind == TS_AMD_SMN ? " sensor AMD Tctl" : ts_kind ? " sensor Intel DTS" : " sem sensor da CPU");
  if (ts_kind) {
    s_cat(line, ", agora ");
    s_num(line, t, 1);
    s_cat(line, " C");
  }
  if (ts_kind == TS_AMD_SMN) {
    s_cat(line, " fam ");
    s_hex8(line, ts_fam << 8 | ts_model);
    s_cat(line, " CCD ");
    s_hex8(line, ts_ccd_base);
    s_cat(line, "/");
    s_num(line, (int)ts_ccd_mask, 1);
  }
  s_cat(line, rapl_ok ? " RAPL" : " sem RAPL");
  s_cat(line, " limite ");
  s_num(line, th_max_c, 1);
  s_cat(line, " C =====\n");
  serial_puts("[temp] ");
  serial_puts(line);
  tl_add(line);
  /* sem sensor da CPU a thread sobe assim mesmo: SSD, carga e memoria */
  if (kt_thread_create(thermal_thread, 0, 16384, ~1u) <= 0)
    serial_puts("[temp] nao criou a thread\n");
}

/* Etapa do boot: o SMU ainda conta energia? (smuprobe.h) Vai para a serial,
 * ou seja, para o comeco do D:\\LOG\\SERIAL.LOG. */
void smu_mark(const char* stage) {
  struct smu_probe p;
  smu_probe_run(&p);
  if (!p.ok) return;
  serial_puts("[smu] ");
  serial_puts(stage);
  serial_puts(": energia +");
  serial_hex32(p.de);
  serial_puts(p.de ? "" : " PARADO");
  serial_puts(" Tctl ");
  serial_hex32(p.tctl);
  serial_puts("\n");
}

/* Uma linha avulsa no TEMP.LOG, gravada na hora (REBOOT usa). */
void thermal_note(const char* line) {
  if (!tlog_n && drives_is_mounted('D')) {
    int n = file_load("D:\\LOG\\TEMP.LOG", tlog, TLOG_MAX);
    tlog_n = n > 0 ? n : 0;
  }
  tl_add(line);
  if (drives_is_mounted('D')) (void)file_save("D:\\LOG\\TEMP.LOG", tlog, tlog_n);
}
