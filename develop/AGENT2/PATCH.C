#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
  FILE* f; FILE* g; FILE* b;
  int ini, fim, n = 0, c;
  char line[2048];
  if (argc < 5) { printf("uso: PATCH ARQ INI FIM BLOCO\n"); return 1; }
  ini = atoi(argv[2]); fim = atoi(argv[3]);
  f = fopen(argv[1], "r");
  if (!f) { printf("erro abrir arq\n"); return 1; }
  g = fopen("PATCH.TMP", "w");
  if (!g) { printf("erro criar tmp\n"); return 1; }
  while (fgets(line, sizeof line, f)) {
    n++;
    if (n < ini) {
      fputs(line, g);
    } else if (n == ini) {
      b = fopen(argv[4], "r");
      if (!b) { printf("erro abrir bloco\n"); fclose(f); fclose(g); return 1; }
      while ((c = fgetc(b)) != EOF) fputc(c, g);
      fclose(b);
    } else if (n > fim) {
      fputs(line, g);
    }
  }
  fclose(f); fclose(g);
  remove(argv[1]);
  rename("PATCH.TMP", argv[1]);
  printf("ok: %s linhas %d..%d trocadas\n", argv[1], ini, fim);
  return 0;
}
