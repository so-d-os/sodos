#include "shell.h"

int cmd_disks(int argc, char** argv) {
  (void)argc;
  (void)argv;
  puts_k("Available drives:\n");
  puts_k("  Drive 0: Primary Master (os.img - boot disk)\n");
  puts_k("  Drive 1: Primary Slave (disk.img - 2GB data disk)\n");
  puts_k("  Drive 2: Secondary Master (not configured)\n");
  puts_k("  Drive 3: Secondary Slave (not configured)\n");
  puts_k("\n");
  puts_k("Use 'fdisk list [drive]' to list partitions on a drive.\n");
  puts_k("Example: fdisk list 1\n");
  return 0;
}
