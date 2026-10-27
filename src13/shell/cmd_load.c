#include "shell.h"

int cmd_load(int argc, char** argv) {
  if (argc < 2) {
    puts_k("Usage: load <executable>\n");
    puts_k("Available executables:\n");
    for (int i = 0; i < executable_count; i++) {
      puts_k("  ");
      puts_k(executables[i].name);
      puts_k("\n");
    }
    return 1;
  }
  const struct executable_entry* exe = find_executable(argv[1]);
  if (!exe) {
    puts_k("Executable not found: ");
    puts_k(argv[1]);
    puts_k("\n");
    return 1;
  }
  void* load_addr = (void*)0x30000;
  const uint8_t* src = exe->data;
  uint8_t* dst = (uint8_t*)load_addr;
  for (uint32_t i = 0; i < exe->size; i++)
    dst[i] = src[i];
  puts_k("Loading ");
  puts_k(exe->name);
  puts_k("...\n");
  typedef int (*exec_main_fn)(void (*puts)(const char*));
  exec_main_fn entry = (exec_main_fn)exe->entry_point;
  int result = entry(puts_k);
  puts_k("\nExecutable returned: ");
  char num_str[16];
  int n = result;
  int j = 0;
  if (n == 0) {
    num_str[j++] = '0';
  } else {
    if (n < 0) {
      num_str[j++] = '-';
      n = -n;
    }
    char temp[16];
    int k = 0;
    while (n > 0) {
      temp[k++] = '0' + (n % 10);
      n /= 10;
    }
    for (int l = k - 1; l >= 0; l--)
      num_str[j++] = temp[l];
  }
  num_str[j] = '\0';
  puts_k(num_str);
  puts_k("\n");
  return 0;
}
