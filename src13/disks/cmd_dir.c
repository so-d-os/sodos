#include "shell.h"
#include "drives.h"

extern int fat_mkdir(const char* path);
extern int fat_rmdir(const char* path);

int cmd_dir(int argc, char** argv) {
  drives_print_dir(argc >= 2 ? argv[1] : "");
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
  rc = fat_rmdir(name);
  if (rc == -2) puts_k("pasta nao esta vazia\n");
  else if (rc == -3) puts_k("nao e pasta\n");
  else if (rc != 0) puts_k("nao removeu a pasta\n");
  return rc == 0 ? 0 : 1;
}
