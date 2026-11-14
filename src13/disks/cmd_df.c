#include "shell.h"
#include "drives.h"

/* Espaco da unidade atual em KB (FAT do kernel ou FAT32 pelo driver). */
int cmd_df(int argc, char** argv) {
  (void)argc;
  (void)argv;
  char L = drives_current();
  uint32_t total_kb, free_kb;
  char num_str[32];
  if (drives_space(L, &total_kb, &free_kb) != 0) {
    puts_k("Error: No filesystem mounted.\n");
    puts_k("Use 'mount [partition]' to mount a filesystem.\n");
    return 1;
  }
  puts_k("Filesystem      Total      Used       Free\n");
  puts_k(drives_kind_name(L));
  puts_k(name_eq(drives_kind_name(L), "FAT") ? "             " : "           ");
  uint32_to_string(total_kb, num_str, sizeof(num_str));
  puts_k(num_str);
  puts_k(" KB    ");
  uint32_to_string(total_kb - free_kb, num_str, sizeof(num_str));
  puts_k(num_str);
  puts_k(" KB    ");
  uint32_to_string(free_kb, num_str, sizeof(num_str));
  puts_k(num_str);
  puts_k(" KB\n");
  return 0;
}
