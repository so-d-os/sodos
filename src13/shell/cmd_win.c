#include "shell.h"
#include "visual.h"
#include "serial.h"
#include "vt.h"

int cmd_win(int argc, char** argv) {
  /* WIN <programa>: abre o programa no WIN (o CLAUDEW.BAT usa). Com o WIN ja
   * aberto (o CMD e uma janela dele) so faz o pedido; o laco do WIN abre no
   * proximo quadro. */
  if (argc >= 2 && argv[1] && argv[1][0]) {
    if (win_launch(argv[1]) != 0) {
      puts_k("WIN: nao foi possivel pedir a abertura.\n");
      return 1;
    }
    serial_puts("[shell] win ");
    serial_puts(argv[1]);
    serial_puts("\n");
    if (win_screen_size(0, 0)) {
      puts_k("Abrindo no WIN: ");
      puts_k(argv[1]);
      puts_k("\n");
      return 0;
    }
  }
  serial_puts("[shell] win\n");
  puts_k("Starting WIN...\n");
  /* O WIN roda na tarefa propria (vt.c): Ctrl+F1..F12 trocam de tela.
   * Ctrl+Backspace fecha o WIN e devolve a memoria do ambiente grafico. */
  if (vt_win()) puts_k("O WIN continua aberto: Ctrl+F7 volta a ele.\n");
  else puts_k("Back to DOS.\n");
  return 0;
}
