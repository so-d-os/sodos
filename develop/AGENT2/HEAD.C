#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
  FILE* f; FILE* g; int n = 0, lim, c;
  if (argc < 4) { printf("uso: HEAD ARQ N SAIDA\n"); return 1; }
  lim = atoi(argv[2]);
  f = fopen(argv[1], "r");
  if (!f) { printf("erro abrir\n"); return 1; }
  g = fopen(argv[3], "w");
  if (!g) { printf("erro criar\n"); return 1; }
  while ((c = fgetc(f)) != EOF) {
    if (n >= lim) break;
    fputc(c, g);
    if (c == '\n') n++;
  }
  fclose(f); fclose(g);
  printf("ok: %d linhas de %s -> %s\n", n, argv[1], argv[3]);
  return 0;
}
