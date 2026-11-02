#include "shell.h"

int cmd_cat(int argc, char** argv) {
  if (argc < 2) {
    puts_k("Usage: cat <filename>\n");
    puts_k("       cat <filename> | more\n");
    return 1;
  }
  return text_file(argv[1], 0);
}
