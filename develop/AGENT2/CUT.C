#include <stdio.h>
#include <stdlib.h>
/* CUT ARQ INI FIM SAIDA : copia linhas INI..FIM (1-based) para SAIDA */
int main(int argc, char** argv) {
  FILE* f; FILE* g; int a, b, n = 0; char line[2048];
  if (argc < 5) { printf("uso: CUT ARQ INI FIM SAIDA\n"); return 1; }
  a = atoi(argv[2]); b = atoi(argv[3]);
  f = fopen(argv[1], "r");
  if (!f) { printf("erro abrir %s\n", argv[1]); return 1; }
  g = fopen(argv[4], "w");
  if (!g) { printf("erro criar %s\n", argv[4]); return 1; }
  while (fgets(line, sizeof line, f)) {
    n++;
    if (n >= a && n <= b) fputs(line, g);
  }
  fclose(f); fclose(g);
  printf("ok: %s linhas %d..%d -> %s\n", argv[1], a, b, argv[4]);
  return 0;
}
