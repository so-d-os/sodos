#include "shell.h"
#include "visual.h"
#include "serial.h"

int cmd_visual(int argc, char** argv) {
  (void)argc;
  (void)argv;
  serial_puts("[shell] visual\n");
  puts_k("Modo grafico: ESC sai | area azul | rato move o cursor.\n");
  visual_run();
  puts_k("Back to text mode.\n");
  return 0;
}
