#include <stdio.h>
int main(int argc, char** argv) {
  FILE* g; int i, c;
  if (argc < 3) { printf("uso: CONCAT SAIDA IN1 IN2 ...\n"); return 1; }
  g = fopen(argv[1], "w");
  if (!g) { printf("erro criar\n"); return 1; }
  for (i = 2; i < argc; i++) {
    FILE* f = fopen(argv[i], "r");
    if (!f) { printf("erro abrir %s\n", argv[i]); fclose(g); return 1; }
    while ((c = fgetc(f)) != EOF) fputc(c, g);
    fclose(f);
  }
  fclose(g);
  printf("ok: %s\n", argv[1]);
  return 0;
}
