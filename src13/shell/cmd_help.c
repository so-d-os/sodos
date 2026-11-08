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
  puts_k("Ctrl+F1..F5: telas do DOS (cada uma guarda o seu programa)\n");
  puts_k("Ctrl+F7: WIN\n");
  return 0;
}
