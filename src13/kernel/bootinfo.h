/* bootinfo.h - contrato de entrada do kernel SODOS (BootInfo v1).
 *
 * O mesmo contrato e usado pelo carregador BIOS (stage 2) e devera ser usado
 * pelo futuro carregador UEFI.  Todos os enderecos sao fisicos e a estrutura
 * precisa estar acessivel na identidade mapeada pelo kernel_entry.
 *
 * Layout fixo (128 bytes).  Os offsets sao repetidos em kernel/stage2.asm;
 * os _Static_assert abaixo fazem o compilador recusar qualquer divergencia.
 */
#ifndef SODOS_KERNEL_BOOTINFO_H
#define SODOS_KERNEL_BOOTINFO_H

#include <stddef.h>
#include <stdint.h>

#define BOOTINFO_MAGIC   0x53444F53u  /* "SODS" em little-endian */
#define BOOTINFO_VERSION 1u
#define BOOTINFO_SIZE    128u
#define BOOTINFO_MAX_MEM_ENTRIES 64u

/* Tipo de firmware que preencheu a estrutura. */
#define BOOTINFO_FIRMWARE_UNKNOWN 0u
#define BOOTINFO_FIRMWARE_BIOS    1u
#define BOOTINFO_FIRMWARE_UEFI    2u

/* Formato de pixel do framebuffer; NONE significa "kernel inicializa o video". */
#define BOOTINFO_FB_FORMAT_NONE 0u
#define BOOTINFO_FB_FORMAT_RGB  1u
#define BOOTINFO_FB_FORMAT_BGR  2u

/* Tipos de entrada do mapa, iguais aos valores E820. */
#define BOOTINFO_MEM_USABLE       1u
#define BOOTINFO_MEM_RESERVED     2u
#define BOOTINFO_MEM_ACPI_RECLAIM 3u
#define BOOTINFO_MEM_ACPI_NVS     4u
#define BOOTINFO_MEM_BAD          5u

/* Entrada do mapa fisico.  Formato identico a uma entrada E820 de 24 bytes. */
struct BootMemEntry {
  uint64_t base;
  uint64_t length;
  uint32_t type;
  uint32_t attr;
} __attribute__((packed));

struct BootInfo {
  uint32_t magic;          /* 0   BOOTINFO_MAGIC */
  uint32_t version;        /* 4   BOOTINFO_VERSION */
  uint32_t size;           /* 8   tamanho desta estrutura */
  uint32_t firmware;       /* 12  BOOTINFO_FIRMWARE_* */
  uint64_t kernel_phys;    /* 16  inicio fisico do kernel */
  uint64_t kernel_size;    /* 24  bytes uteis do kernel.bin */
  uint64_t kernel_entry;   /* 32  endereco fisico de kernel_entry */
  uint64_t app_phys;       /* 40  aplicacao/initrd, 0 se ausente */
  uint64_t app_size;       /* 48  tamanho da aplicacao/initrd */
  uint64_t fb_addr;        /* 56  framebuffer linear, 0 se nao fornecido */
  uint32_t fb_width;       /* 64 */
  uint32_t fb_height;      /* 68 */
  uint32_t fb_pitch;       /* 72  bytes por linha */
  uint32_t fb_bpp;         /* 76  bits por pixel */
  uint32_t fb_format;      /* 80  BOOTINFO_FB_FORMAT_* */
  uint32_t mem_count;      /* 84  entradas validas */
  uint32_t mem_capacity;   /* 88  capacidade da area apontada */
  uint32_t flags;          /* 92  reservado para o futuro; 0 agora */
  uint64_t mem_map_addr;   /* 96  endereco fisico de BootMemEntry[] */
  uint64_t boot_drive;     /* 104 unidade BIOS usada no boot */
  uint64_t reserved[2];    /* 112 reservado; deve ser 0 */
} __attribute__((packed));

_Static_assert(sizeof(struct BootMemEntry) == 24u,
               "BootMemEntry deve ter 24 bytes (formato E820)");
_Static_assert(sizeof(struct BootInfo) == BOOTINFO_SIZE,
               "BootInfo deve ter 128 bytes");
_Static_assert(offsetof(struct BootInfo, kernel_phys) == 16u,
               "offset de kernel_phys mudou");
_Static_assert(offsetof(struct BootInfo, kernel_size) == 24u,
               "offset de kernel_size mudou");
_Static_assert(offsetof(struct BootInfo, kernel_entry) == 32u,
               "offset de kernel_entry mudou");
_Static_assert(offsetof(struct BootInfo, app_phys) == 40u,
               "offset de app_phys mudou");
_Static_assert(offsetof(struct BootInfo, app_size) == 48u,
               "offset de app_size mudou");
_Static_assert(offsetof(struct BootInfo, fb_addr) == 56u,
               "offset de fb_addr mudou");
_Static_assert(offsetof(struct BootInfo, fb_width) == 64u,
               "offset de fb_width mudou");
_Static_assert(offsetof(struct BootInfo, mem_count) == 84u,
               "offset de mem_count mudou");
_Static_assert(offsetof(struct BootInfo, mem_map_addr) == 96u,
               "offset de mem_map_addr mudou");
_Static_assert(offsetof(struct BootInfo, boot_drive) == 104u,
               "offset de boot_drive mudou");

void bootinfo_set(const struct BootInfo *bi);
const struct BootInfo *bootinfo_get(void);

/* Retorna 1 para uma estrutura v1 minima e internamente consistente. */
int bootinfo_valid(const struct BootInfo *bi);

/* Mapa fisico, ou NULL quando nao ha entradas. */
const struct BootMemEntry *bootinfo_mem_map(const struct BootInfo *bi);

#endif
