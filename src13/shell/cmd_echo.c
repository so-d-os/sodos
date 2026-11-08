#include "shell.h"

/* echo texto. "echo texto > arquivo" e o shell que grava (exec_line). */
int cmd_echo(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    puts_k(argv[i]);
    if (i + 1 < argc) putc(' ');
  }
  putc('\n');
  return 0;
}
