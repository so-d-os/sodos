/* fat32t.c - FAT32T.EXE: le arquivos pelas chamadas dos programas (k_fsize,
 * k_fload, k_fread em pedacos, k_dir, k_flist) e imprime tamanho e FNV-1a de
 * cada um, para comparar com o host. Feito para o FAT32.DLL (pendrive em U:),
 * mas vale para qualquer unidade.
 *
 *   FAT32T U:\PASTA\ARQ.BIN ...   arquivos: tamanho, hash k_fload, hash k_fread
 *   FAT32T -d U:\PASTA            pasta: k_dir (nome, tamanho, dir)
 *   FAT32T -l U                   k_flist da unidade
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>

static unsigned fnv(const unsigned char* p, int n, unsigned h) {
  int i;
  for (i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

static int one_file(const char* path) {
  int sz = k_fsize(path);
  char* buf;
  char* part;
  int n, off, got;
  unsigned h1, h2 = 2166136261u;
  if (sz < 0) {
    printf("%s: nao existe (%d)\n", path, sz);
    return 1;
  }
  buf = (char*)malloc(sz + 1);
  part = (char*)malloc(4093);
  if (!buf || !part) {
    printf("%s: sem memoria\n", path);
    return 1;
  }
  n = k_fload(path, buf, sz + 1);
  h1 = fnv((unsigned char*)buf, n < 0 ? 0 : n, 2166136261u);
  /* pedacos de tamanho impar: atravessam clusters e setores */
  off = 0;
  for (;;) {
    got = k_fread(path, off, part, 4093);
    if (got <= 0) break;
    h2 = fnv((unsigned char*)part, got, h2);
    off += got;
  }
  printf("%s size=%d fload=%d fnv=%08x fread=%d fnv=%08x %s\n", path, sz, n, h1, off, h2,
         (n == sz && off == sz && h1 == h2) ? "OK" : "DIFERENTE");
  free(part);
  free(buf);
  return (n == sz && off == sz && h1 == h2) ? 0 : 1;
}

static int one_dir(const char* path) {
  struct k_dirent* d = (struct k_dirent*)malloc(256 * sizeof(struct k_dirent));
  int n, i;
  if (!d) return 1;
  n = k_dir(path, d, 256);
  printf("k_dir %s = %d\n", path, n);
  for (i = 0; i < n; i++) printf("  %s %s %u\n", d[i].dir ? "D" : "F", d[i].name, d[i].size);
  free(d);
  return n < 0;
}

int main(int argc, char** argv) {
  int i, bad = 0;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-d") && i + 1 < argc) {
      bad |= one_dir(argv[++i]);
    } else if (!strcmp(argv[i], "-l") && i + 1 < argc) {
      char* out = (char*)malloc(4096);
      int n = k_flist(argv[++i][0], out, 4096);
      printf("k_flist %c = %d\n%s", argv[i][0], n, out);
      free(out);
    } else {
      bad |= one_file(argv[i]);
    }
  }
  printf("FAT32T: %s\n", bad ? "FALHA" : "OK");
  return bad;
}
