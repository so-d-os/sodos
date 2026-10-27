#include "shell.h"

int cmd_pause(int argc, char** argv) {
  (void)argc;
  (void)argv;
  pause();
  return 0;
}
