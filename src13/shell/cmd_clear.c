#include "shell.h"

int cmd_clear(int argc, char** argv) {
  (void)argc;
  (void)argv;
  clear_screen();
  return 0;
}
