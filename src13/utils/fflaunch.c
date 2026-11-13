/* fflaunch.c - atalho do Firefox no desktop do WIN.
 *
 * O desktop so executa .EXE que estao em D:\USER\DEFAULT\DESKTOP (ou
 * U:\USER\DEFAULT\DESKTOP no pendrive), e o Firefox precisa rodar da propria
 * pasta (os recursos ficam ao lado do executavel). Este programa vai para o
 * desktop como FIREFOX.EXE e so pede ao WIN que abra o de verdade, como o
 * GET /win/launch do httpd: primeiro na unidade do atalho, depois D: e U:.
 */
#include <so.h>
#include <stdio.h>

int main(void) {
  char cand[4] = {0, 'D', 'U', 0};
  char path[] = "?:\\FIREFOX\\FIREFOX.EXE";
  int i;
  cand[0] = (char)k_exe_drive();
  for (i = 0; i < 3; i++) {
    if (!cand[i] || (i > 0 && cand[i] == cand[0])) continue;
    path[0] = cand[i];
    if (k_fsize(path) <= 0) continue;
    if (k_win_launch(path) == 0) return 0;
  }
  printf("Nao achei FIREFOX\\FIREFOX.EXE em D: nem em U:.\n");
  return 1;
}
