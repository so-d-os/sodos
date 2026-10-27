#include "shell.h"

int cmd_ls(int argc, char** argv) {
  (void)argc;
  (void)argv;
  if (mem_file_count > 0) {
    puts_k("Memory files:\n");
    for (int i = 0; i < mem_file_count; i++) {
      puts_k("  ");
      puts_k(mem_files[i].name);
      puts_k(" (");
      char size_str[16];
      int n = mem_files[i].size;
      int j = 0;
      if (n == 0) {
        size_str[j++] = '0';
      } else {
        char temp[16];
        int k = 0;
        while (n > 0) {
          temp[k++] = '0' + (n % 10);
          n /= 10;
        }
        for (int l = k - 1; l >= 0; l--)
          size_str[j++] = temp[l];
      }
      size_str[j] = '\0';
      puts_k(size_str);
      puts_k(" bytes)\n");
    }
  }
  if (file_count > 0) {
    puts_k("Embedded files:\n");
    for (int i = 0; i < file_count; i++) {
      puts_k("  ");
      puts_k(files[i].name);
      puts_k("\n");
    }
  }
  if (executable_count > 0) {
    puts_k("Executables:\n");
    for (int i = 0; i < executable_count; i++) {
      puts_k("  ");
      puts_k(executables[i].name);
      puts_k("\n");
    }
  }
  return 0;
}
