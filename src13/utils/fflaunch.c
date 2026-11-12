/* fflaunch.c - atalho do Firefox no desktop do WIN.
 *
 * O desktop so executa .EXE que estao em D:\USER\DEFAULT\DESKTOP, e o Firefox
 * precisa rodar de D:\FIREFOX (os recursos ficam ao lado do executavel).
 * Este programa vai para o desktop como FIREFOX.EXE e so pede ao WIN que abra
 * o de verdade, como o GET /win/launch do httpd.
 */
#include <so.h>
#include <stdio.h>

#define FIREFOX_PATH "D:\\FIREFOX\\FIREFOX.EXE"

int main(void) {
  if (k_win_launch(FIREFOX_PATH) != 0) {
    printf("Nao consegui abrir %s.\n", FIREFOX_PATH);
    return 1;
  }
  return 0;
}
