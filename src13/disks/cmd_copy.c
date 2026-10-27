#include "shell.h"
#include "drives.h"
#include "mm.h"

#define COPY_MAX 32768

static void copy_name(char* dst, int max, const char* src) {
  int i = 0;
  if (max < 1) return;
  while (src[i] && i < max - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

int cmd_copy(int argc, char** argv) {
  char src_letter;
  char dst_letter;
  char src_name[64];
  char dst_name[64];
  char dest_path[80];
  char* buf;
  int n;
  int i;

  if (argc < 3) {
    puts_k("Uso: copy origem destino\n");
    puts_k("Ex.: copy c:hello.asm d:\n");
    return 1;
  }

  path_parse(argv[1], &src_letter, src_name, (int)sizeof(src_name));
  path_parse(argv[2], &dst_letter, dst_name, (int)sizeof(dst_name));
  if (src_name[0] == '\0') {
    puts_k("origem invalida\n");
    return 1;
  }
  if (dst_name[0] == '\0')
    copy_name(dst_name, (int)sizeof(dst_name), src_name);
  if (!drives_is_mounted(src_letter)) {
    puts_k("origem nao montada\n");
    return 1;
  }
  if (!drives_is_mounted(dst_letter)) {
    puts_k("destino nao montado. Use: mount D:\n");
    return 1;
  }

  buf = (char*)kmalloc(COPY_MAX);
  if (!buf) {
    puts_k("sem memoria\n");
    return 1;
  }
  n = file_load(argv[1], buf, COPY_MAX);
  if (n < 0) {
    puts_k("arquivo nao encontrado\n");
    kfree(buf);
    return 1;
  }

  dest_path[0] = dst_letter;
  dest_path[1] = ':';
  i = 0;
  while (dst_name[i] && i < (int)sizeof(dest_path) - 3) {
    dest_path[2 + i] = dst_name[i];
    i++;
  }
  dest_path[2 + i] = '\0';

  if (file_save(dest_path, buf, n) != 0) {
    puts_k("nao copiou\n");
    kfree(buf);
    return 1;
  }
  kfree(buf);
  puts_k("copiado ");
  puts_k(dest_path);
  puts_k(" (");
  {
    char num[16];
    uint32_to_string((uint32_t)n, num, (int)sizeof(num));
    puts_k(num);
  }
  puts_k(" bytes)\n");
  return 0;
}
