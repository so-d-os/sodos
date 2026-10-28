/* bootinfo.c - guarda e valida a estrutura entregue pelo carregador. */
#include "bootinfo.h"

static const struct BootInfo *g_bootinfo;

void bootinfo_set(const struct BootInfo *bi) {
  g_bootinfo = bi;
}

const struct BootInfo *bootinfo_get(void) {
  return g_bootinfo;
}

int bootinfo_valid(const struct BootInfo *bi) {
  if (!bi) return 0;
  if (bi->magic != BOOTINFO_MAGIC) return 0;
  if (bi->version != BOOTINFO_VERSION) return 0;
  if (bi->size < BOOTINFO_SIZE) return 0;
  if (bi->firmware > BOOTINFO_FIRMWARE_UEFI) return 0;
  if (bi->mem_capacity > BOOTINFO_MAX_MEM_ENTRIES) return 0;
  if (bi->mem_count > bi->mem_capacity) return 0;
  if (bi->mem_count != 0 && bi->mem_map_addr == 0) return 0;
  if ((bi->mem_map_addr & 3u) != 0) return 0;
  if (bi->kernel_phys == 0) return 0;
  return 1;
}

const struct BootMemEntry *bootinfo_mem_map(const struct BootInfo *bi) {
  if (!bi || bi->mem_map_addr == 0 || bi->mem_count == 0) return 0;
  return (const struct BootMemEntry *)(uintptr_t)bi->mem_map_addr;
}
