/*
 * wine.c - Lado do kernel dos EXEs do Windows (ver wine.h).
 *
 * Desligado (padrao): nenhum DLL carregado; wine_entry recusa com a mensagem
 * "SET WINE=1". Ligado: o WINE.DLL vem no primeiro EXE do Windows e fica.
 * SET WINE=0 descarrega na hora se nenhum EXE do Windows esta rodando, senao
 * quando o ultimo terminar (wine_exec_done).
 */
#include <stdint.h>
#include "wine.h"
#include "drives.h"
#include "env.h"
#include "module.h"
#include "serial.h"
#include "mm.h"
#include "irq.h"

void mod_set_err(const char* a, const char* b);
int kexp_date(void);
int kexp_time(void);

volatile uint8_t* wine_kuser;
static uint64_t kuser_epoch;   /* FILETIME quando irq_ticks era 0 */

/* KSYSTEM_TIME {low, high1, high2}: quem le repete ate high1 == high2. */
static void ksystime(volatile uint8_t* at, uint64_t v) {
  volatile uint32_t* k = (volatile uint32_t*)at;
  k[2] = (uint32_t)(v >> 32);
  k[0] = (uint32_t)v;
  k[1] = (uint32_t)(v >> 32);
}

void wine_tick(uint64_t ms) {
  volatile uint8_t* k = wine_kuser;
  uint64_t it = ms * 10000ull;   /* unidades de 100 ns */
  if (!k) return;
  ksystime(k + 0x08, it);                /* InterruptTime */
  ksystime(k + 0x14, kuser_epoch + it);  /* SystemTime */
  ksystime(k + 0x320, ms);               /* TickCount (multiplicador 1) */
}

static uint64_t days_from_civil(int y, unsigned m, unsigned d) {
  int era;
  unsigned yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint64_t)(era * 146097 + (int)doe - 719468);
}

static void* kuser_raw;        /* bloco do heap quando a pagina e um alias */

static void kuser_on(void) {
  uint8_t* k;
  int d, t, i;
  uint64_t secs;
  const char* force;
  if (wine_kuser) return;
  force = env_get("WINE_KUSERMAP"); /* teste: forca o alias mesmo com a RAM livre */
  k = (force && force[0] == '1') ? 0 : (uint8_t*)umalloc_at(WINE_KUSER_ADDR, 0x1000, 0);
  if (!k) {
    /* Num PC com UEFI o endereco pode ser do firmware ou estar fora do heap:
     * uma pagina nossa passa a aparecer em 0x7FFE0000 (tabela de paginas). */
    uint8_t* raw = (uint8_t*)umalloc_owned(0x2000, 0); /* do kernel: dura ate o kuser_off */
    uint8_t* pg = raw ? (uint8_t*)(((uintptr_t)raw + 0xFFFu) & ~(uintptr_t)0xFFFu) : 0;
    if (!pg || !mm_alias_page(WINE_KUSER_ADDR, (uint64_t)(uintptr_t)pg)) {
      if (raw) ufree(raw);
      serial_puts("[wine] sem a pagina 0x7FFE0000 (KUSER_SHARED_DATA)\n");
      return;
    }
    kuser_raw = raw;
    k = pg;
    serial_puts("[wine] KUSER_SHARED_DATA mapeada em 0x7FFE0000 (pagina do heap)\n");
  }
  for (i = 0; i < 0x1000; i++) k[i] = 0;
  d = kexp_date();
  t = kexp_time();
  if (d <= 0) d = 20260101;
  if (t < 0) t = 0;
  secs = days_from_civil(d / 10000, (unsigned)(d / 100 % 100), (unsigned)(d % 100)) * 86400u +
         (uint64_t)(t / 10000) * 3600u + (uint64_t)(t / 100 % 100) * 60u + (uint64_t)(t % 100);
  kuser_epoch = (secs + 11644473600ull) * 10000000ull - irq_ticks() * 10000ull;
  *(uint32_t*)(k + 0x04) = 1u << 24;     /* TickCountMultiplier */
  *(uint32_t*)(k + 0x260) = 19045;       /* NtBuildNumber */
  *(uint32_t*)(k + 0x264) = 1;           /* NtProductType: WinNt */
  *(uint32_t*)(k + 0x26C) = 10;          /* NtMajorVersion */
  *(uint32_t*)(k + 0x270) = 0;           /* NtMinorVersion */
  *(uint32_t*)(k + 0x3C0) = 1;           /* ActiveProcessorCount */
  wine_kuser = k;
  mm_null_guard(1); /* NULL falha, como no Windows */
  wine_tick(irq_ticks());
}

static void kuser_off(void) {
  uint8_t* k = (uint8_t*)wine_kuser;
  if (!k) return;
  wine_kuser = 0;
  mm_null_guard(0);
  if (kuser_raw) {
    mm_alias_undo();
    ufree(kuser_raw);
    kuser_raw = 0;
  } else {
    ufree(k);
  }
}

/* ---------- faixa baixa para imagens de base fixa ---------- */
#define LOW_MAX 8
static struct {
  uint32_t addr, size, pid;
} low[LOW_MAX];
static int low_n;

void* wine_low_alloc(uint32_t addr, uint32_t size) {
  uint32_t pid = mod_current_pid();
  uint32_t i;
  int slot = -1, k;
  if (!pid || !size || (addr & 0xFFFu)) return 0;
  size = (size + 0xFFFu) & ~0xFFFu;
  if (addr < WINE_LOW_START || addr >= WINE_LOW_END || size > WINE_LOW_END - addr) return 0;
  for (k = 0; k < LOW_MAX; k++) {
    if (!low[k].size) {
      if (slot < 0) slot = k;
      continue;
    }
    if (addr < low[k].addr + low[k].size && low[k].addr < addr + size) return 0;   /* ocupado */
  }
  if (slot < 0) return 0;
  if (!low_n && gfx_back_move(1) != 0) return 0;
  low[slot].addr = addr;
  low[slot].size = size;
  low[slot].pid = pid;
  low_n++;
  for (i = 0; i < size; i++) ((uint8_t*)(uintptr_t)addr)[i] = 0;
  return (void*)(uintptr_t)addr;
}

void wine_low_release_pid(uint32_t pid) {
  int k;
  if (!low_n) return;
  for (k = 0; k < LOW_MAX; k++)
    if (low[k].size && low[k].pid == pid) {
      low[k].size = 0;
      low_n--;
    }
  if (!low_n) gfx_back_move(0);
}

static int w_handle;                   /* mod_dll_* (0 = nao carregado) */
static const struct wine_ops* w_ops;
static int w_busy;                     /* EXEs do Windows rodando agora */
static char w_path[WINE_PATH_MAX];

static int wanted(void) {
  const char* v = env_get("WINE");
  return v && v[0] && !(v[0] == '0' && v[1] == 0);
}

int wine_loaded(void) { return w_ops != 0; }

static void unload_now(void) {
  if (w_handle) {
    mod_dll_free(w_handle);
    serial_puts("[wine] WINE.DLL descarregado\n");
  }
  w_handle = 0;
  w_ops = 0;
  w_path[0] = 0;
}

static void scopy(char* d, const char* s, int max) {
  int i = 0;
  while (s[i] && i < max - 1) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

static int try_load(const char* path) {
  int h;
  wine_entry_fn fn;
  const struct wine_ops* ops;
  if (file_size(path) <= 0) return 0;
  h = mod_dll_load_sys(path);
  if (!h) {
    serial_puts("[wine] ");
    serial_puts(path);
    serial_puts(": ");
    serial_puts(mod_last_error());
    serial_puts("\n");
    return 0;
  }
  fn = (wine_entry_fn)mod_dll_sym(h, WINE_SYM);
  ops = fn ? fn() : 0;
  if (!ops || ops->abi != WINE_ABI || !ops->start) {
    serial_puts("[wine] ");
    serial_puts(path);
    serial_puts(": nao e o WINE.DLL desta versao\n");
    mod_dll_free(h);
    return 0;
  }
  w_handle = h;
  w_ops = ops;
  scopy(w_path, path, (int)sizeof(w_path));
  serial_puts("[wine] ");
  serial_puts(path);
  serial_puts(" carregado\n");
  return 1;
}

static int load(void) {
  char p[WINE_PATH_MAX];
  env_sodos_path("DRV\\WINE.DLL", p, (int)sizeof(p));
  if (try_load(p)) return 1;
  if (drives_is_mounted('D') && try_load("D:\\SODOS\\DRV\\WINE.DLL")) return 1;
  if (drives_is_mounted('Z') && try_load("Z:\\SODOS\\DRV\\WINE.DLL")) return 1;
  return 0;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int wine_is_pe(const char* path) {
  uint8_t h[64];
  uint8_t sig[4];
  uint32_t lfanew;
  if (!path || file_read_at(path, 0, (char*)h, (int)sizeof(h)) != (int)sizeof(h)) return 0;
  if (h[0] != 'M' || h[1] != 'Z') return 0;
  lfanew = rd32(h + 0x3C);
  if (lfanew < 0x40 || lfanew > 0x10000) return 0;
  if (file_read_at(path, lfanew, (char*)sig, 4) != 4) return 0;
  (void)rd16;
  return sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0;
}

void* wine_entry(void) {
  if (!wanted()) {
    mod_set_err("programa do Windows: ligue com SET WINE=1", 0);
    return 0;
  }
  if (!w_ops && !load()) {
    char p[WINE_PATH_MAX];
    env_sodos_path("DRV", p, (int)sizeof(p));
    mod_set_err("WINE.DLL nao encontrado em ", p);
    return 0;
  }
  if (!w_busy) kuser_on();
  w_busy++;
  return (void*)w_ops->start;
}

void wine_exec_done(void) {
  if (w_busy > 0) w_busy--;
  if (!w_busy) kuser_off();
  if (!w_busy && w_ops && !wanted()) unload_now();
}

void wine_env_changed(void) {
  /* So descarrega: carregar fica para o primeiro EXE do Windows. */
  if (!wanted() && !w_busy) unload_now();
}
