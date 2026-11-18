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

void mod_set_err(const char* a, const char* b);

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
  w_busy++;
  return (void*)w_ops->start;
}

void wine_exec_done(void) {
  if (w_busy > 0) w_busy--;
  if (!w_busy && w_ops && !wanted()) unload_now();
}

void wine_env_changed(void) {
  /* So descarrega: carregar fica para o primeiro EXE do Windows. */
  if (!wanted() && !w_busy) unload_now();
}
