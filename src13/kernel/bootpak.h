/* bootpak.h - pacote de boot SODOS.PAK (arquivos basicos que vem com o
 * kernel e vao para o Z: a cada boot). Formato em tools/mkpak.c. */
#pragma once
#include <stdint.h>
#include "bootinfo.h"

#define PAK_MAGIC "SODOSPAK"
#define PAK_VERSION 1u
#define PAK_PATH 64
#define PAK_MAX_SIZE (64u << 20)

struct pak_hdr {
  char magic[8];
  uint32_t version;
  uint32_t count;     /* entradas */
  uint32_t total;     /* bytes do pacote inteiro */
  uint32_t pad;
};

struct pak_ent {
  char path[PAK_PATH];  /* "SODOS\\DRV\\FAT32.DLL", sem letra */
  uint32_t off;         /* desde o comeco do pacote */
  uint32_t size;
  uint32_t fnv;         /* FNV-1a dos dados */
  uint32_t pad;
};

/* Antes do mm_init: tira o pacote do GRUB de onde ele caiu para logo depois
 * do kernel, onde o mm nao entrega memoria. */
void bootpak_early(const struct BootInfo* bi);
/* Fim da memoria que o pacote ocupa (0 se nao ha); o mm comeca depois. */
uint32_t bootpak_reserved_end(void);
/* Depois do Z:: grava os arquivos do pacote em Z:\. */
void bootpak_install(void);
