/* unixdrv.h - unidade de um caminho Unix "/D/..." de programa portado.
 *
 * Os ports (Firefox, NetSurf) usam /D/USER/DEFAULT, /D/TEMP e /D/SODOS/FONTS:
 * no QEMU o D: e o disco com essas pastas. No notebook (boot pelo GRUB) nao ha
 * D: de disco e as mesmas pastas estao no pendrive (U:). Entao /D/ vai para a
 * unidade de onde o .EXE foi aberto quando D:\USER nao existe e la existe.
 *
 * Incluido por stdio.c (fopen), misc.c (realpath) e libs/posix-lite/posix.c
 * (open/stat/opendir/mkdir...): cada um guarda a propria resposta. */
#ifndef SODOS_UNIXDRV_H
#define SODOS_UNIXDRV_H

#include <so.h>

static int unix_drive(int L) {
  static int mapped;
  struct k_dirent e;
  int x;
  char p[8];
  if (L >= 'a' && L <= 'z') L -= 32;
  if (L != 'D') return L;
  if (mapped) return mapped;
  mapped = 'D';
  if (k_dir("D:\\USER", &e, 1) >= 0) return mapped;
  x = k_exe_drive();
  if (x >= 'a' && x <= 'z') x -= 32;
  if (x < 'A' || x > 'Z' || x == 'D') return mapped;
  p[0] = (char)x;
  p[1] = ':';
  p[2] = '\\';
  p[3] = 'U';
  p[4] = 'S';
  p[5] = 'E';
  p[6] = 'R';
  p[7] = 0;
  if (k_dir(p, &e, 1) >= 0) mapped = x;
  return mapped;
}

#endif
