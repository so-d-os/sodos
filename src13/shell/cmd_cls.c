#include "shell.h"

int cmd_cls(int argc, char** argv) {
  (void)argc;
  (void)argv;
  clear_screen();
  return 0;
}
