#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
  FILE* f; FILE* g; int n = 0, ini, c;
  if (argc < 4) { printf("uso: TAIL2 ARQ N SAIDA\n"); return 1; }
  ini = atoi(argv[2]);
  f = fopen(argv[1], "r");
  if (!f) { printf("erro abrir\n"); return 1; }
  g = fopen(argv[3], "w");
  if (!g) { printf("erro criar\n"); return 1; }
  while ((c = fgetc(f)) != EOF) {
    if (c == '\n') n++;
    if (n >= ini) fputc(c, g);
  }
  fclose(f); fclose(g);
  printf("ok: %s a partir da linha %d\n", argv[3], ini + 1);
  return 0;
}
