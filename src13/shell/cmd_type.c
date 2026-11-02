#include "shell.h"

/* Mostra o arquivo inteiro (em pedacos). Para paginar: type arquivo | more */
int cmd_type(int argc, char** argv) {
  if (argc < 2) {
    puts_k("Usage: type <arquivo>\n");
    puts_k("  type readme.txt\n");
    puts_k("  type D:\\nota.txt\n");
    puts_k("  type D:\\nota.txt | more\n");
    return 1;
  }
  return text_file(argv[1], 0);
}
