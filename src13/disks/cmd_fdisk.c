#include "shell.h"

extern int list_partitions(uint32_t drive, char* output, int max_output);
extern int create_partition(uint32_t drive, uint8_t partition_num, uint32_t lba_start, uint32_t sector_count, uint8_t type);
extern int get_partition_info(uint32_t drive, uint8_t partition_num, uint32_t* lba_start, uint32_t* sector_count, uint8_t* type);

int cmd_fdisk(int argc, char** argv) {
  if (argc < 2) {
    puts_k("Usage: fdisk <command> [args]\n");
    puts_k("Commands:\n");
    puts_k("  list [drive] - List partitions (default: drive 1)\n");
    puts_k("  create <num> <lba_start> <sectors> <type> [drive] - Create partition\n");
    puts_k("  Types: 0x01=FAT12, 0x04/0x06=FAT16\n");
    return 1;
  }
  uint32_t drive = 1;
  if (strcmp(argv[1], "list") == 0) {
    fdisk_log("[fdisk] list: start\n");
    if (argc >= 3)
      drive = str_to_uint32(argv[2], 10);
    fdisk_log("[fdisk] list: drive set, calling list_partitions\n");
    char* output = (char*)FDISK_LIST_OUT_BUF;
    int result = list_partitions(drive, output, 512);
    fdisk_log("[fdisk] list: list_partitions returned\n");
    if (result >= 0) {
      puts_k("Partitions on drive ");
      char* drive_str = (char*)FDISK_DRIVE_STR_BUF;
      uint32_t d = drive;
      int j = 0;
      if (d == 0)
        drive_str[j++] = '0';
      else {
        char* temp = (char*)(FDISK_DRIVE_STR_BUF + 16);
        int k = 0;
        while (d > 0 && k < 15) {
          temp[k++] = '0' + (d % 10);
          d /= 10;
        }
        for (int l = k - 1; l >= 0 && j < 15; l--)
          drive_str[j++] = temp[l];
      }
      drive_str[j] = '\0';
      puts_k(drive_str);
      puts_k(":\n");
      puts_k(output);
    } else {
      puts_k("Error: Could not read partitions\n");
    }
    return 0;
  }
  if (strcmp(argv[1], "create") == 0) {
    if (argc < 6) {
      puts_k("Usage: fdisk create <num> <lba_start> <sectors> <type> [drive]\n");
      return 1;
    }
    uint8_t part_num = (uint8_t)str_to_uint32(argv[2], 10);
    uint32_t lba_start = str_to_uint32(argv[3], 10);
    uint32_t sectors = str_to_uint32(argv[4], 10);
    uint8_t type = (uint8_t)str_to_uint32(argv[5], 16);
    if (sectors == 0) {
      puts_k("Error: Sector count must be > 0\n");
      return 1;
    }
    if (type != 0x01 && type != 0x04 && type != 0x06) {
      puts_k("Error: Invalid partition type. Use 0x01, 0x04, or 0x06\n");
      return 1;
    }
    if (argc >= 7)
      drive = str_to_uint32(argv[6], 10);
    int result = create_partition(drive, part_num, lba_start, sectors, type);
    if (result == 0)
      puts_k("Partition created successfully!\n");
    else if (result == -1)
      puts_k("Error: Invalid partition number\n");
    else if (result == -2)
      puts_k("Error: Partition slot already occupied\n");
    else if (result == -3)
      puts_k("Error: Failed to write MBR to disk\n");
    else
      puts_k("Error: Failed to create partition\n");
    return result;
  }
  puts_k("Unknown command\n");
  return 1;
}
