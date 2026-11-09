#include "shell.h"

/* cat e o mesmo que type (cmd_type.c). */
int cmd_cat(int argc, char** argv) {
  return cmd_type(argc, argv);
}
