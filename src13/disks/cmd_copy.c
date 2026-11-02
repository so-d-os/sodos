#include "shell.h"
#include "drives.h"
#include "mm.h"

/* Arquivo inteiro na memoria alta: programas e DLLs passam de 200 KB. */
#define COPY_MAX (8u * 1024u * 1024u)

static void copy_name(char* dst, int max, const char* src) {
  int i = 0;
  if (max < 1) return;
  while (src[i] && i < max - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static int name_len(const char* s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

int cmd_copy(int argc, char** argv) {
  char src_letter;
  char dst_letter;
  char src_name[64];
  char dst_name[64];
  char dest_path[80];
  struct dir_item probe;
  char* buf;
  int n;

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
  /* Destino que e pasta (".", "temp", "d:", "u:\\src"): grava la dentro com o mesmo nome. */
  if (dst_name[0] == '\0' || drives_items(argv[2], &probe, 1) >= 0) {
    int len = name_len(dst_name);
    const char* base = src_name + name_len(src_name);
    while (base > src_name && base[-1] != '\\') base--;
    if (len > 0 && len < (int)sizeof(dst_name) - 1) dst_name[len++] = '\\';
    copy_name(dst_name + len, (int)sizeof(dst_name) - len, base);
  }
  if (!drives_is_mounted(src_letter)) {
    puts_k("origem nao montada\n");
    return 1;
  }
  if (!drives_is_mounted(dst_letter)) {
    puts_k("destino nao montado. Use: mount D:\n");
    return 1;
  }

  n = file_size(argv[1]);
  if (n < 0) {
    puts_k("arquivo nao encontrado\n");
    return 1;
  }
  if ((uint32_t)n >= COPY_MAX) {
    puts_k("arquivo grande demais\n");
    return 1;
  }
  buf = (char*)umalloc_owned((uint32_t)n + 2u, 0);
  if (!buf) {
    puts_k("sem memoria\n");
    return 1;
  }
  n = file_load(argv[1], buf, n + 2);
  if (n < 0) {
    puts_k("arquivo nao encontrado\n");
    ufree(buf);
    return 1;
  }

  /* dst_name ja vem da raiz: o \\ impede o file_save de somar a pasta atual de novo. */
  dest_path[0] = dst_letter;
  dest_path[1] = ':';
  dest_path[2] = '\\';
  copy_name(dest_path + 3, (int)sizeof(dest_path) - 3, dst_name);

  if (file_save(dest_path, buf, n) != 0) {
    puts_k("nao copiou\n");
    ufree(buf);
    return 1;
  }
  ufree(buf);
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
