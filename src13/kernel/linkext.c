/*
 * linkext.c - Lado do kernel da associacao de extensoes (ver linkext.h).
 *
 * Desligado (padrao): linkext_fn = 0, nenhum DLL carregado; o launcher do
 * WIN so testa o ponteiro. SET LINKEXT=1 chama linkext_env_changed (env.c),
 * que carrega PATHSODOS\DRV\LINKEXT.DLL. Se a unidade ainda nao esta montada
 * (pendrive) ou o DLL nao existe, linkext_fn aponta para lx_retry, que tenta
 * de novo no proximo arquivo aberto. SET LINKEXT=0 descarrega.
 */
#include <stdint.h>
#include "linkext.h"
#include "drives.h"
#include "env.h"
#include "module.h"
#include "serial.h"
#include "shell.h"
#include "visual.h"

int (*linkext_fn)(const char* file);

static int lx_handle;                   /* mod_dll_* (0 = nao carregado) */
static const struct linkext_ops* lx_ops;
static int lx_busy;                     /* chamadas dentro do DLL agora */
static int lx_free_later;               /* desligou durante uma chamada */
static char lx_path[LINKEXT_PATH_MAX];  /* de onde veio o DLL */

static int h_fload(const char* path, char* buf, int max) {
  if (file_size(path) < 0) return -1;
  return file_load(path, buf, max);
}
static int h_fsize(const char* path) { return file_size(path); }
static void h_log(const char* s) {
  serial_puts("[linkext] ");
  serial_puts(s);
  serial_puts("\n");
}

static const struct linkext_host host = {
  LINKEXT_ABI, h_fload, h_fsize, env_sodos_path, win_launch_arg, h_log,
};

static int wanted(void) {
  const char* v = env_get("LINKEXT");
  return v && v[0] && !(v[0] == '0' && v[1] == 0);
}

static void unload_now(void) {
  if (lx_handle) {
    mod_dll_free(lx_handle);
    serial_puts("[linkext] descarregado\n");
  }
  lx_handle = 0;
  lx_ops = 0;
  lx_free_later = 0;
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
  linkext_entry_fn fn;
  const struct linkext_ops* ops;
  if (file_size(path) <= 0) return 0;
  h = mod_dll_load_sys(path);
  if (!h) {
    serial_puts("[linkext] ");
    serial_puts(path);
    serial_puts(": ");
    serial_puts(mod_last_error());
    serial_puts("\n");
    return 0;
  }
  fn = (linkext_entry_fn)mod_dll_sym(h, LINKEXT_SYM);
  ops = fn ? fn() : 0;
  if (!ops || ops->abi != LINKEXT_ABI || !ops->open) {
    serial_puts("[linkext] ");
    serial_puts(path);
    serial_puts(": nao e o LINKEXT.DLL\n");
    mod_dll_free(h);
    return 0;
  }
  lx_handle = h;
  lx_ops = ops;
  scopy(lx_path, path, (int)sizeof(lx_path));
  serial_puts("[linkext] ");
  serial_puts(path);
  serial_puts(" carregado\n");
  return 1;
}

/* PATHSODOS\DRV, depois as copias do disco e do SODOS.PAK. */
static int load(void) {
  char p[LINKEXT_PATH_MAX];
  env_sodos_path("DRV\\LINKEXT.DLL", p, (int)sizeof(p));
  if (try_load(p)) return 1;
  if (drives_is_mounted('D') && try_load("D:\\SODOS\\DRV\\LINKEXT.DLL")) return 1;
  if (drives_is_mounted('Z') && try_load("Z:\\SODOS\\DRV\\LINKEXT.DLL")) return 1;
  return 0;
}

static int lx_call(const char* file) {
  int rc;
  if (!lx_ops || !file || !file[0]) return 1;
  lx_busy++;
  rc = lx_ops->open(&host, file);
  lx_busy--;
  if (lx_free_later && !lx_busy) unload_now();
  return rc == 0 ? 0 : 1;
}

/* Ligado mas sem DLL ainda (unidade do PATHSODOS nao montada no SET). */
static int lx_retry(const char* file) {
  if (!lx_ops && !load()) return 1;
  linkext_fn = lx_call;
  return lx_call(file);
}

/* TROCA LINKEXT.DLL (troca.c): descarrega e le o arquivo de novo. 0 ok, 1
 * desligado (SET LINKEXT=1 ja carrega o novo), -1 em uso agora. */
int linkext_reload(void) {
  if (!wanted()) return 1;
  if (lx_busy) return -1;
  unload_now();
  linkext_fn = load() ? lx_call : lx_retry;
  return 0;
}

int linkext_loaded(void) { return lx_ops != 0; }

void linkext_env_changed(void) {
  if (!wanted()) {
    linkext_fn = 0;
    if (lx_busy) lx_free_later = 1;
    else unload_now();
    return;
  }
  lx_free_later = 0;
  if (lx_ops) {
    /* Ja carregado (PATHSODOS mudou ou SET LINKEXT=1 de novo): o DLL le o
     * LINKEXT.CFG do PATHSODOS atual a cada arquivo, nao precisa trocar. */
    linkext_fn = lx_call;
    return;
  }
  if (load()) {
    linkext_fn = lx_call;
    return;
  }
  linkext_fn = lx_retry;
  /* Aviso so na hora do SET LINKEXT=1 e so em modo texto (prompt, AUTOEXEC):
   * dentro do WIN nada aparece para nao atrapalhar (fica so na serial). */
  serial_puts("[linkext] LINKEXT.DLL nao encontrado; tenta de novo ao abrir um arquivo\n");
  if (!win_screen_size(0, 0)) {
    char p[LINKEXT_PATH_MAX];
    env_sodos_path("DRV", p, (int)sizeof(p));
    puts_k("LINKEXT: LINKEXT.DLL nao encontrado (");
    puts_k(p);
    puts_k(", D:\\SODOS\\DRV, Z:\\SODOS\\DRV).\n");
    puts_k("LINKEXT: associacoes sem efeito ate o DLL aparecer.\n");
  }
}
