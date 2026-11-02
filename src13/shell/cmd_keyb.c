/* keyb - escolhe o layout do teclado (br = ABNT2, us = EUA).
 * A escolha fica em KEYB.CFG na raiz de D: (ou de U:, sem D:) e vale no boot. */
#include "shell.h"
#include "drives.h"
#include "keymap.h"

static int parse(const char* s) {
  if (!s) return -1;
  if ((s[0] == 'b' || s[0] == 'B') && (s[1] == 'r' || s[1] == 'R')) return KEYB_BR;
  if ((s[0] == 'u' || s[0] == 'U') && (s[1] == 's' || s[1] == 'S')) return KEYB_US;
  return -1;
}

static const char* cfg_path(void) {
  if (drives_is_mounted('D')) return "D:\\KEYB.CFG";
  if (drives_is_mounted('U')) return "U:\\KEYB.CFG";
  return 0;
}

void keyb_load_cfg(void) {
  static const char* paths[] = { "D:\\KEYB.CFG", "U:\\KEYB.CFG" };
  char buf[8];
  int i;
  for (i = 0; i < 2; i++) {
    int l;
    if (!drives_is_mounted(paths[i][0])) continue;
    if (file_load(paths[i], buf, (int)sizeof(buf) - 1) < 2) continue;
    l = parse(buf);
    if (l >= 0) {
      keymap_set(l);
      return;
    }
  }
}

int cmd_keyb(int argc, char** argv) {
  int l;
  const char* path;
  if (argc < 2) {
    puts_k("Teclado: ");
    puts_k(keymap_name(keymap_get()));
    puts_k("\nUso: keyb br   ABNT2 (c cedilha, acentos ~ ' ` ^ \")\n");
    puts_k("     keyb us   EUA\n");
    return 0;
  }
  l = parse(argv[1]);
  if (l < 0) {
    puts_k("keyb: layout desconhecido (use br ou us)\n");
    return 1;
  }
  keymap_set(l);
  puts_k("Teclado: ");
  puts_k(keymap_name(l));
  puts_k("\n");
  path = cfg_path();
  if (path && file_save(path, l == KEYB_US ? "us\n" : "br\n", 3) < 0) {
    puts_k("keyb: nao gravou ");
    puts_k(path);
    puts_k("\n");
  }
  return 0;
}
