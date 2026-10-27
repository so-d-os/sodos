// fat_format.c - Formatação de sistemas de arquivos FAT
#include <stdint.h>
#include "disk.h"

/* Buffer em 0x80800 reservado no linker.ld (evita sobreposição com .rodata/.bss do kernel → GPF) */
extern uint8_t format_buf_region[512];
#define format_buf  format_buf_region

// Estrutura do Boot Sector FAT (mesma de fat.c)
struct fat_boot_sector {
  uint8_t  jmp[3];
  char     oem_name[8];
  uint16_t bytes_per_sector;
  uint8_t  sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t  fat_count;
  uint16_t root_entries;
  uint16_t total_sectors_16;
  uint8_t  media_type;
  uint16_t sectors_per_fat_16;
  uint16_t sectors_per_track;
  uint16_t heads;
  uint32_t hidden_sectors;
  uint32_t total_sectors_32;
  uint8_t  drive_number;
  uint8_t  reserved;
  uint8_t  boot_signature;
  uint32_t volume_id;
  char     volume_label[11];
  char     fs_type[8];
  uint8_t  boot_code[448];
  uint16_t boot_signature_word;
} __attribute__((packed));

#define MIN_PART_SECTORS  (1u + 32u)  /* reserved + root_dir mínimo */

// Formata partição em FAT16 (usa format_buf para I/O; não usa pilha)
int format_fat16(uint32_t drive, uint32_t lba_start, uint32_t total_sectors, const char* volume_label) {
  uint16_t bytes_per_sector = 512;
  uint8_t sectors_per_cluster = 1;
  uint16_t reserved_sectors = 1;
  uint8_t fat_count = 2;
  uint16_t root_entries = 512;

  if (total_sectors < MIN_PART_SECTORS)
    return -5; /* partição pequena demais */

  uint32_t root_dir_sectors = ((root_entries * 32) + (bytes_per_sector - 1)) / bytes_per_sector;
  uint32_t data_sectors = total_sectors - (reserved_sectors + root_dir_sectors);

  if (data_sectors < 32680u) {
    sectors_per_cluster = 1;
  } else if (data_sectors < 262144u) {
    sectors_per_cluster = 4;
  } else {
    sectors_per_cluster = 16;
  }
  if (sectors_per_cluster == 0)
    sectors_per_cluster = 1; /* evita divisão por zero */

  data_sectors = total_sectors - (reserved_sectors + root_dir_sectors);
  uint32_t clusters = data_sectors / sectors_per_cluster;
  /* FAT16 acaba em 65524 clusters. Cluster maior mantem o volume montavel. */
  while (clusters >= 65525u && sectors_per_cluster < 64) {
    sectors_per_cluster = (uint8_t)(sectors_per_cluster * 2);
    clusters = data_sectors / sectors_per_cluster;
  }
  uint32_t sectors_per_fat = ((clusters * 2) + (bytes_per_sector - 1)) / bytes_per_sector;

  struct fat_boot_sector* boot = (struct fat_boot_sector*)format_buf;
  for (int i = 0; i < 512; i++) format_buf[i] = 0;

  boot->jmp[0] = 0xEB;
  boot->jmp[1] = 0x3C;
  boot->jmp[2] = 0x90;
  {
    const char* oem = "MSDOS5.0";
    for (int i = 0; i < 8; i++) boot->oem_name[i] = oem[i];
  }
  boot->bytes_per_sector = bytes_per_sector;
  boot->sectors_per_cluster = sectors_per_cluster;
  boot->reserved_sectors = reserved_sectors;
  boot->fat_count = fat_count;
  boot->root_entries = root_entries;
  boot->total_sectors_16 = (total_sectors < 65536) ? (uint16_t)total_sectors : 0;
  boot->total_sectors_32 = (total_sectors >= 65536) ? total_sectors : 0;
  boot->media_type = 0xF8;
  boot->sectors_per_fat_16 = (uint16_t)sectors_per_fat;
  boot->sectors_per_track = 63;
  boot->heads = 16;
  boot->hidden_sectors = lba_start;
  boot->drive_number = 0x80;
  boot->reserved = 0;
  boot->boot_signature = 0x29;
  boot->volume_id = 0x12345678;
  for (int i = 0; i < 11; i++) {
    boot->volume_label[i] = (volume_label && volume_label[i] != '\0') ? volume_label[i] : ' ';
  }
  {
    const char* fs_type = "FAT16   ";
    for (int i = 0; i < 8; i++) boot->fs_type[i] = fs_type[i];
  }
  boot->boot_signature_word = 0xAA55;

  if (disk_write_sector(drive, lba_start, format_buf) != 0)
    return -1;

  for (int i = 0; i < 512; i++) format_buf[i] = 0;
  format_buf[0] = 0xF8;
  format_buf[1] = 0xFF;
  format_buf[2] = 0xFF;

  for (int f = 0; f < fat_count; f++) {
    uint32_t fat_start = lba_start + reserved_sectors + (f * sectors_per_fat);
    for (uint32_t i = 0; i < sectors_per_fat; i++) {
      if (i != 0) {
        for (int j = 0; j < 512; j++) format_buf[j] = 0;
      }
      if (disk_write_sector(drive, fat_start + i, format_buf) != 0)
        return -2;
    }
  }

  uint32_t root_start = lba_start + reserved_sectors + (fat_count * sectors_per_fat);
  for (int i = 0; i < 512; i++) format_buf[i] = 0;
  for (uint32_t i = 0; i < root_dir_sectors; i++) {
    if (disk_write_sector(drive, root_start + i, format_buf) != 0)
      return -3;
  }

  return 0;
}
