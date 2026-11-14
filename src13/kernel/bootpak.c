/*
 * bootpak.c - Pacote de boot (SODOS.PAK): arquivos basicos que vem junto com
 * o kernel, sem depender de disco nenhum. Hoje: Z:\SODOS\DRV\FAT32.DLL, para
 * o notebook ler um pendrive FAT32 mesmo sem D:.
 *
 * De onde vem:
 *  - GRUB: "module2 /boot/sodos/sodos.pak" (BOOTINFO_FLAG_PAK_MEM). O GRUB
 *    poe o modulo em qualquer RAM livre; bootpak_early copia para logo depois
 *    do kernel antes do mm_init, e o mm comeca depois dele.
 *  - os.img (BIOS): o Makefile grava o pacote depois do kernel e do app; o
 *    stage2 passa o setor (BOOTINFO_FLAG_PAK_LBA) e aqui ele e lido do ATA 0.
 *
 * bootpak_install grava tudo em Z:\ (o ramdisk, recriado a cada boot).
 * Sem pacote, nada acontece.
 */
#include <stdint.h>
#include "bootpak.h"
#include "bootinfo.h"
#include "disk.h"
#include "drives.h"
#include "mm.h"
#include "serial.h"
#include "shell.h"

extern char __bss_end[];
extern void* umalloc_owned(uint32_t nbytes, uint32_t owner);
extern void ufree(void* ptr);

static const uint8_t* pak_mem;   /* pacote do GRUB, ja no lugar dele */
static uint32_t pak_len;
static uint32_t pak_end;

static int magic_ok(const uint8_t* p) {
  const char* m = PAK_MAGIC;
  int i;
  for (i = 0; i < 8; i++)
    if (p[i] != (uint8_t)m[i]) return 0;
  return 1;
}

void bootpak_early(const struct BootInfo* bi) {
  uint64_t src, n, dst, kstart, kend;
  if (!bi || !(bi->flags & BOOTINFO_FLAG_PAK_MEM)) return;
  src = bi->pak_addr;
  n = bi->pak_size;
  if (n < sizeof(struct pak_hdr) || n > PAK_MAX_SIZE || src + n > 0xFFE00000ULL) return;
  kstart = bi->kernel_phys;
  kend = (uint64_t)(uintptr_t)__bss_end;
  if (src < kend && src + n > kstart) return;   /* em cima do kernel: perdido */
  dst = (kend + 0xFFFu) & ~0xFFFULL;
  if (dst != src) {
    volatile uint8_t* d = (volatile uint8_t*)(uintptr_t)dst;
    const volatile uint8_t* s = (const volatile uint8_t*)(uintptr_t)src;
    uint64_t i;
    if (dst < src)
      for (i = 0; i < n; i++) d[i] = s[i];
    else
      for (i = n; i > 0; i--) d[i - 1] = s[i - 1];
  }
  if (!magic_ok((const uint8_t*)(uintptr_t)dst)) return;
  pak_mem = (const uint8_t*)(uintptr_t)dst;
  pak_len = (uint32_t)n;
  pak_end = (uint32_t)(dst + n);
}

uint32_t bootpak_reserved_end(void) { return pak_end; }

static uint32_t fnv(const uint8_t* p, uint32_t n) {
  uint32_t h = 2166136261u;
  uint32_t i;
  for (i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

/* os.img: le o pacote do ATA 0 a partir de lba. Devolve o buffer (ufree). */
static uint8_t* read_lba(uint32_t lba, uint32_t* len) {
  uint8_t* first = (uint8_t*)umalloc_owned(512, 0);
  uint8_t* buf;
  const struct pak_hdr* h;
  uint32_t secs, i;
  if (!first) return 0;
  if (disk_read_sectors(0, lba, 1, first) != 0 || !magic_ok(first)) {
    ufree(first);
    return 0;
  }
  h = (const struct pak_hdr*)first;
  if (h->total < sizeof(struct pak_hdr) || h->total > PAK_MAX_SIZE) {
    ufree(first);
    return 0;
  }
  secs = (h->total + 511u) / 512u;
  buf = (uint8_t*)umalloc_owned(secs * 512u, 0);
  *len = h->total;
  ufree(first);
  if (!buf) return 0;
  for (i = 0; i < secs; i += 128u) {
    uint32_t k = secs - i > 128u ? 128u : secs - i;
    if (disk_read_sectors(0, lba + i, k, buf + i * 512u) != 0) {
      ufree(buf);
      return 0;
    }
  }
  return buf;
}

/* Cria as pastas de "Z:\A\B\ARQ" (A e A\B). */
static void make_parents(char* full) {
  int i;
  for (i = 3; full[i]; i++) {
    if (full[i] != '\\') continue;
    full[i] = 0;
    drives_mkdir(full);
    full[i] = '\\';
  }
}

void bootpak_install(void) {
  const struct BootInfo* bi = bootinfo_get();
  const uint8_t* p = pak_mem;
  uint8_t* owned = 0;
  uint32_t len = pak_len, i, ok = 0;
  const struct pak_hdr* h;
  if (!drives_is_mounted('Z')) return;
  if (!p && bi && (bi->flags & BOOTINFO_FLAG_PAK_LBA) && bi->firmware == BOOTINFO_FIRMWARE_BIOS)
    p = owned = read_lba((uint32_t)bi->pak_addr, &len);
  if (!p) return;
  h = (const struct pak_hdr*)p;
  if (!magic_ok(p) || h->version != PAK_VERSION || h->total > len ||
      h->count > (len - sizeof(*h)) / sizeof(struct pak_ent)) {
    puts_k("SODOS.PAK invalido.\n");
    goto out;
  }
  for (i = 0; i < h->count; i++) {
    const struct pak_ent* e =
        (const struct pak_ent*)(p + sizeof(*h) + i * sizeof(struct pak_ent));
    char full[3 + PAK_PATH];
    int k;
    if (e->off > h->total || e->size > h->total - e->off || e->size > 0x7FFFFFFFu ||
        e->path[0] == 0 || e->path[PAK_PATH - 1] != 0)
      continue;
    if (fnv(p + e->off, e->size) != e->fnv) {
      serial_puts("[bootpak] soma errada: ");
      serial_puts(e->path);
      serial_puts("\n");
      continue;
    }
    full[0] = 'Z';
    full[1] = ':';
    full[2] = '\\';
    for (k = 0; e->path[k]; k++) full[3 + k] = e->path[k] == '/' ? '\\' : e->path[k];
    full[3 + k] = 0;
    make_parents(full);
    if (file_save(full, (const char*)(p + e->off), (int)e->size) == 0) {
      ok++;
      serial_puts("[bootpak] ");
      serial_puts(full);
      serial_puts("\n");
    }
  }
  {
    char n[12];
    uint32_to_string(ok, n, sizeof(n));
    puts_k("Z: ");
    puts_k(n);
    puts_k(" arquivo(s) do pacote de boot (SODOS.PAK).\n");
  }
out:
  if (owned) ufree(owned);
}
