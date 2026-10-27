#include "shell.h"
#include "visual.h"
#include "serial.h"

int cmd_win(int argc, char** argv) {
  (void)argc;
  (void)argv;
  serial_puts("[shell] win\n");
  puts_k("Starting WIN...\n");
  win_run();
  puts_k("Back to DOS.\n");
  return 0;
}
