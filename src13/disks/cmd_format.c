#include "shell.h"

extern int get_partition_info(uint32_t drive, uint8_t partition_num, uint32_t* lba_start, uint32_t* sector_count, uint8_t* type);
extern int format_fat16(uint32_t drive, uint32_t lba_start, uint32_t total_sectors, const char* volume_label);

int cmd_format(int argc, char** argv) {
  if (argc < 3) {
    puts_k("Usage: format <partition> <label>\n");
    puts_k("Example: format 0 MYDISK\n");
    return 1;
  }
  uint32_t drive = 1;
  uint8_t part_num = (uint8_t)str_to_uint32(argv[1], 10);
  uint32_t lba_start, sector_count;
  uint8_t type;
  int result = get_partition_info(drive, part_num, &lba_start, &sector_count, &type);
  if (result != 0) {
    if (result == -2) {
      puts_k("Error: Partition ");
      putc('0' + (char)part_num);
      puts_k(" does not exist.\n");
      puts_k("Create it first: fdisk create 0 1 204799 0x06 1\n");
    } else {
      puts_k("Error: Cannot read partition table (disk or drive?).\n");
    }
    return 1;
  }
  if (type != 0x04 && type != 0x06) {
    puts_k("Error: Partition is not FAT16 (use type 0x04 or 0x06 in fdisk create).\n");
    return 1;
  }
  const char* label = argv[2];
  puts_k("Formatting partition...\n");
  result = format_fat16(drive, lba_start, sector_count, label);
  if (result == 0) {
    puts_k("Partition formatted successfully!\n");
    return 0;
  }
  if (result == -5) {
    puts_k("Error: Partition too small to format.\n");
    return 1;
  }
  if (result == -1) {
    puts_k("Error: Format failed (cannot write boot sector). Use 'dstat' to see ATA status.\n");
    return 1;
  }
  if (result == -2) {
    puts_k("Error: Format failed (cannot write FAT).\n");
    return 1;
  }
  if (result == -3) {
    puts_k("Error: Format failed (cannot write root directory).\n");
    return 1;
  }
  puts_k("Error: Format failed.\n");
  return 1;
}
