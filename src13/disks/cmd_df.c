#include "shell.h"

extern int fat_get_space_info(uint32_t* total_bytes, uint32_t* free_bytes);

int cmd_df(int argc, char** argv) {
  (void)argc;
  (void)argv;
  uint32_t total_bytes, free_bytes;
  int result = fat_get_space_info(&total_bytes, &free_bytes);
  if (result < 0) {
    if (result == -1) {
      puts_k("Error: No filesystem mounted.\n");
      puts_k("Use 'mount [partition]' to mount a filesystem.\n");
    } else {
      puts_k("Error: Could not calculate filesystem space.\n");
    }
    return 1;
  }
  uint32_t used_bytes = total_bytes - free_bytes;
  uint32_t total_kb = total_bytes / 1024;
  uint32_t used_kb = used_bytes / 1024;
  uint32_t free_kb = free_bytes / 1024;
  puts_k("Filesystem      Total      Used       Free\n");
  puts_k("FAT16           ");
  char num_str[32];
  uint32_to_string(total_kb, num_str, sizeof(num_str));
  puts_k(num_str);
  puts_k(" KB    ");
  uint32_to_string(used_kb, num_str, sizeof(num_str));
  puts_k(num_str);
  puts_k(" KB    ");
  uint32_to_string(free_kb, num_str, sizeof(num_str));
  puts_k(num_str);
  puts_k(" KB\n");
  return 0;
}
