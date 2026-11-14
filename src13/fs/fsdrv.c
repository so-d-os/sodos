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
    if ((uintptr_t)dst < 0x100000u && disk != DISK_USB && disk != DISK_RAM) return -1;
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
    if ((uintptr_t)src < 0x100000u && disk != DISK_USB && disk != DISK_RAM) return -1;
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

/* Um DLL por formato; hoje so FAT32. */
#define FSDRV_MAX 2
static struct {
  char dll[16];
  int handle;
  const struct fsdrv_ops* ops;
} drv[FSDRV_MAX];

static int str_eq(const char* a, const char* b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static const struct fsdrv_ops* try_load(char letter, const char* dll) {
  static const char dir[] = "X:\\SODOS\\DRV\\";
  char path[48];
  int i = 0, j, h;
  fsdrv_entry_fn fn;
  const struct fsdrv_ops* ops;
  for (j = 0; dir[j]; j++) path[i++] = dir[j];
  for (j = 0; dll[j] && i < (int)sizeof(path) - 1; j++) path[i++] = dll[j];
  path[i] = 0;
  path[0] = letter;
  if (file_size(path) <= 0) return 0;
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
  if (!ops || ops->abi != FSDRV_ABI || !ops->probe || !ops->mount || !ops->umount ||
      !ops->stat || !ops->read || !ops->items) {
    puts_k(path);
    puts_k(": nao e driver de sistema de arquivos\n");
    mod_dll_free(h);
    return 0;
  }
  serial_puts("[fsdrv] ");
  serial_puts(path);
  serial_puts(" carregado\n");
  return ops;
}

const struct fsdrv_ops* fsdrv_get(const char* dll, char skip) {
  static const char order[] = "DCEFGHIJKLMNOPQRSTUVWXYZAB";
  const struct fsdrv_ops* ops = 0;
  int i, free_slot = -1;
  if (skip >= 'a' && skip <= 'z') skip = (char)(skip - 32);
  for (i = 0; i < FSDRV_MAX; i++) {
    if (drv[i].ops && str_eq(drv[i].dll, dll)) return drv[i].ops;
    if (!drv[i].ops && free_slot < 0) free_slot = i;
  }
  if (free_slot < 0) return 0;
  for (i = 0; order[i] && !ops; i++) {
    char L = order[i];
    /* C: so tem os arquivos embutidos; Z: e o ramdisk vazio. U: monta sozinho
     * ao ser tocado, e aqui ele pode ser justamente quem pediu o driver. */
    if (L == skip || L == 'C' || L == 'Z' || L == 'U') continue;
    if (!drives_is_mounted(L) || !str_eq(drives_kind_name(L), "FAT")) continue;
    ops = try_load(L, dll);
  }
  if (!ops && skip != 'U' && drives_is_mounted('U')) ops = try_load('U', dll);
  if (!ops) return 0;
  for (i = 0; dll[i] && i < (int)sizeof(drv[0].dll) - 1; i++) drv[free_slot].dll[i] = dll[i];
  drv[free_slot].dll[i] = 0;
  drv[free_slot].ops = ops;
  return ops;
}
