/*
 * fsdrv.c - Carrega drivers de sistema de arquivos (fs/fsdrv.h) sob demanda.
 *
 * O DLL so e lido quando uma unidade precisa dele (hoje: U: ou mount de uma
 * particao FAT32). Procura X:\SODOS\DRV\<dll> em D: e depois nas outras
 * unidades FAT montadas, menos a que esta sendo montada. Achou uma vez, fica
 * carregado (pid 0) ate o desligamento.
 */
#include <stdint.h>
#include "fsdrv.h"
#include "drives.h"
#include "disk.h"
#include "mm.h"
#include "module.h"
#include "sched.h"
#include "serial.h"
#include "shell.h"
#include "env.h"

extern void* umalloc_owned(uint32_t nbytes, uint32_t owner);
extern void ufree(void* ptr);

/* Mesmo layout: o driver devolve fsdrv_item direto para os programas. */
typedef char fsdrv_item_size_ok[sizeof(struct fsdrv_item) == sizeof(struct dir_item) ? 1 : -1];

static int rd_n;   /* setores desde a ultima volta ao desktop */

static int h_read(uint32_t disk, uint32_t lba, uint32_t n, void* buf) {
  uint8_t* dst = (uint8_t*)buf;
  while (n > 0) {
    uint32_t k = n > 128u ? 128u : n;
    /* Abaixo de 1 MB o ATA so aceita a janela baixa; o driver usa o heap. */
    if ((uintptr_t)dst < 0x100000u && disk != DISK_USB && disk != DISK_RAM && disk != DISK_NVME) return -1;
    if (disk_read_sectors(disk, lba, k, dst) != 0) return -1;
    dst += k * 512u;
    lba += k;
    n -= k;
  }
  return 0;
}

static int h_write(uint32_t disk, uint32_t lba, uint32_t n, const void* buf) {
  const uint8_t* src = (const uint8_t*)buf;
  while (n > 0) {
    uint32_t k = n > 128u ? 128u : n;
    if ((uintptr_t)src < 0x100000u && disk != DISK_USB && disk != DISK_RAM && disk != DISK_NVME) return -1;
    if (disk_write_sectors(disk, lba, k, src) != 0) return -1;
    src += k * 512u;
    lba += k;
    n -= k;
  }
  return 0;
}

static void* h_alloc(uint32_t bytes) { return umalloc_owned(bytes ? bytes : 1u, 0); }
static void h_free(void* p) {
  if (p) ufree(p);
}

static void h_yield(void) {
  if (++rd_n < 8) return;
  rd_n = 0;
  sched_yield();
}

static void h_log(const char* s) { serial_puts(s); }

static const struct fsdrv_host host = {
  FSDRV_ABI, h_read, h_write, h_alloc, h_free, h_yield, h_log,
};

const struct fsdrv_host* fsdrv_host(void) { return &host; }

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int fsdrv_is_fat32(const uint8_t* b) {
  if (!b || (b[0] != 0xEB && b[0] != 0xE9)) return 0;
  if (rd16(b + 11) != 512 || b[13] == 0 || b[16] == 0) return 0;
  return rd16(b + 22) == 0 && rd32(b + 36) != 0;
}

/* Drivers ja carregados, por caminho. Um driver trocado (SET FAT32DLL=...)
 * continua carregado: um volume antigo pode estar no meio de uma leitura. */
#define FSDRV_MAX 8
static struct {
  char path[DRV_PATH];
  const struct fsdrv_ops* ops;
} drv[FSDRV_MAX];
static int drv_n;
/* ultimo pedido por variavel que falhou (nao tenta de novo ate o proximo SET) */
static char bad_path[DRV_PATH];
static uint32_t bad_gen;
static const char* last_path;

static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int str_eq(const char* a, const char* b) {
  while (*a && upc(*a) == upc(*b)) {
    a++;
    b++;
  }
  return upc(*a) == upc(*b);
}

static void scopy(char* d, const char* s, int max) {
  int i = 0;
  while (s[i] && i < max - 1) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

/* Carrega (ou acha ja carregado) o driver em path. */
static const struct fsdrv_ops* load_path(const char* path, int quiet) {
  int i, h;
  fsdrv_entry_fn fn;
  const struct fsdrv_ops* ops;
  for (i = 0; i < drv_n; i++)
    if (str_eq(drv[i].path, path)) {
      last_path = drv[i].path;
      return drv[i].ops;
    }
  if (drv_n >= FSDRV_MAX) return 0;
  if (file_size(path) <= 0) {
    if (!quiet) {
      puts_k(path);
      puts_k(": nao existe\n");
    }
    return 0;
  }
  h = mod_dll_load_sys(path);
  if (!h) {
    puts_k(path);
    puts_k(": ");
    puts_k(mod_last_error());
    puts_k("\n");
    return 0;
  }
  fn = (fsdrv_entry_fn)mod_dll_sym(h, FSDRV_SYM);
  ops = fn ? fn() : 0;
  if (!ops || (ops->abi < FSDRV_ABI || ops->abi > FSDRV_ABI_ENTRY) || !ops->probe || !ops->mount || !ops->umount ||
      !ops->stat || !ops->read || !ops->items) {
    puts_k(path);
    puts_k(": nao e driver de sistema de arquivos\n");
    mod_dll_free(h);
    return 0;
  }
  serial_puts("[fsdrv] ");
  serial_puts(path);
  serial_puts(" carregado\n");
  scopy(drv[drv_n].path, path, DRV_PATH);
  drv[drv_n].ops = ops;
  last_path = drv[drv_n].path;
  drv_n++;
  return ops;
}

/* X:\SODOS\DRV\<dll> */
static const struct fsdrv_ops* load_std(char letter, const char* dll) {
  static const char dir[] = "X:\\SODOS\\DRV\\";
  char path[48];
  int i = 0, j;
  for (j = 0; dir[j]; j++) path[i++] = dir[j];
  for (j = 0; dll[j] && i < (int)sizeof(path) - 1; j++) path[i++] = dll[j];
  path[i] = 0;
  path[0] = letter;
  return load_path(path, 1);
}

/* "FAT32.DLL" -> "FAT32DLL" (o nome da variavel de ambiente). */
static void env_name_of(const char* dll, char* out, int max) {
  int i, k = 0;
  for (i = 0; dll[i] && k < max - 1; i++)
    if (dll[i] != '.') out[k++] = upc(dll[i]);
  out[k] = 0;
}

const char* fsdrv_path(void) { return last_path ? last_path : ""; }

const struct fsdrv_ops* fsdrv_get(const char* dll, char skip) {
  /* Z: tem o que veio no SODOS.PAK; depois os discos. U: monta sozinho ao
   * ser tocado e pode ser justamente quem pediu o driver: fica por ultimo. */
  static const char order[] = "ZDCEFGHIJKLMNOPQRSTVWXYAB";
  const struct fsdrv_ops* ops = 0;
  char var[24];
  const char* want;
  int i;
  if (skip >= 'a' && skip <= 'z') skip = (char)(skip - 32);
  /* SET FAT32DLL=D:\CAMINHO\FAT32.DLL (AUTOEXEC.BAT ou prompt) vem primeiro. */
  env_name_of(dll, var, (int)sizeof(var));
  want = env_get(var);
  if (want && want[0] && !(bad_gen == env_gen() && str_eq(bad_path, want))) {
    char L = upc(want[0]);
    if (want[1] == ':' && L == skip) {
      puts_k(var);
      puts_k(": o driver nao pode estar na unidade que ele vai montar\n");
    } else {
      ops = load_path(want, 0);
    }
    if (ops) return ops;
    scopy(bad_path, want, DRV_PATH);
    bad_gen = env_gen();
    puts_k(var);
    puts_k(": usando o driver padrao (\\SODOS\\DRV)\n");
  }
  for (i = 0; order[i] && !ops; i++) {
    char L = order[i];
    if (L == skip || L == 'C') continue;
    if (!drives_is_mounted(L) || !str_eq(drives_kind_name(L), "FAT")) continue;
    ops = load_std(L, dll);
  }
  if (!ops && skip != 'U' && drives_is_mounted('U')) ops = load_std('U', dll);
  return ops;
}
