#include "shell.h"
#include "visual.h"
#include "serial.h"
#include "vt.h"

int cmd_win(int argc, char** argv) {
  (void)argc;
  (void)argv;
  serial_puts("[shell] win\n");
  puts_k("Starting WIN...\n");
  /* O WIN roda na tarefa propria (vt.c): Ctrl+F1..F5 voltam ao DOS. */
  if (vt_win()) puts_k("O WIN continua aberto: Ctrl+F7 volta a ele.\n");
  else puts_k("Back to DOS.\n");
  return 0;
}
