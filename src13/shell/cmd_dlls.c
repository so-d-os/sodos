#include "shell.h"
#include "module.h"

int cmd_dlls(int argc, char** argv) {
  (void)argc;
  (void)argv;
  puts_k("Modulos carregados:\n");
  mod_list();
  return 0;
}
