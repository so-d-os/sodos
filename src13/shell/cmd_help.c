#include "shell.h"

int cmd_help(int argc, char** argv) {
  (void)argc;
  (void)argv;
  puts_k("COMMANDS:\n");
  for (int i = 0; i < cmd_count; ++i) {
    puts_k("  ");
    puts_k(commands[i].name);
    puts_k("\n");
  }
  return 0;
}
