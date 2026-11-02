#include "shell.h"
#include "drives.h"

extern int fat_mkdir(const char* path);
extern int fat_rmdir(const char* path);

/* dir [unidade:][pasta\][padrao] [/s] [/w] [/d] */
int cmd_dir(int argc, char** argv) {
  const char* spec = "";
  int flags = 0;
  int i, j;
  for (i = 1; i < argc; i++) {
    if (argv[i][0] != '/') {
      spec = argv[i];
      continue;
    }
    for (j = 1; argv[i][j]; j++) {
      char c = argv[i][j];
      if (c == '/') continue;
      if (c == 's' || c == 'S') flags |= DIR_SUB;
      else if (c == 'w' || c == 'W') flags |= DIR_WIDE;
      else if (c == 'd' || c == 'D') flags |= DIR_COLS;
      else {
        puts_k("Uso: dir [pasta\\][*.ext] [/s] [/w] [/d]\n");
        puts_k("  /s  inclui as subpastas\n");
        puts_k("  /w  lista larga, em linhas\n");
        puts_k("  /d  lista larga, por coluna\n");
        puts_k("  * e ? valem no nome: dir *.exe   dir ???.txt\n");
        return 1;
      }
    }
  }
  drives_dir(spec, flags);
  return 0;
}

int cmd_cd(int argc, char** argv) {
  if (argc < 2) return drives_chdir(0);
  return drives_chdir(argv[1]);
}

int cmd_md(int argc, char** argv) {
  char L;
  char name[64];
  int idx_ok;
  int rc;
  if (argc < 2) {
    puts_k("Uso: md pasta\n");
    return 1;
  }
  path_parse(argv[1], &L, name, (int)sizeof(name));
  if (!name[0] || !drives_is_mounted(L)) {
    puts_k("pasta invalida\n");
    return 1;
  }
  idx_ok = drives_kind_name(L)[0] == 'F';
  if (!idx_ok) {
    puts_k("md so funciona em disco FAT\n");
    return 1;
  }
  if (drives_use(L) != 0) {
    puts_k("pasta invalida\n");
    return 1;
  }
  rc = fat_mkdir(name);
  if (rc == 1) puts_k("ja existe\n");
  else if (rc != 0) puts_k("nao criou a pasta\n");
  return rc == 0 ? 0 : 1;
}

int cmd_rd(int argc, char** argv) {
  char L;
  char name[64];
  int rc;
  if (argc < 2) {
    puts_k("Uso: rd pasta\n");
    return 1;
  }
  path_parse(argv[1], &L, name, (int)sizeof(name));
  if (!name[0] || !drives_is_mounted(L)) {
    puts_k("pasta invalida\n");
    return 1;
  }
  if (drives_kind_name(L)[0] != 'F') {
    puts_k("rd so funciona em disco FAT\n");
    return 1;
  }
  if (drives_use(L) != 0) {
    puts_k("pasta invalida\n");
    return 1;
  }
  rc = fat_rmdir(name);
  if (rc == -2) puts_k("pasta nao esta vazia\n");
  else if (rc == -3) puts_k("nao e pasta\n");
  else if (rc != 0) puts_k("nao removeu a pasta\n");
  return rc == 0 ? 0 : 1;
}
