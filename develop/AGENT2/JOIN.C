#include <stdio.h>
/* JOIN SAIDA A B C ... : concatena os arquivos na ordem em SAIDA */
int main(int argc, char** argv) {
  FILE* g; int i, c; FILE* f;
  if (argc < 3) { printf("uso: JOIN SAIDA A B ...\n"); return 1; }
  g = fopen(argv[1], "w");
  if (!g) { printf("erro criar %s\n", argv[1]); return 1; }
  for (i = 2; i < argc; i++) {
    f = fopen(argv[i], "r");
    if (!f) { printf("erro abrir %s\n", argv[i]); fclose(g); return 1; }
    while ((c = fgetc(f)) != EOF) fputc(c, g);
    fclose(f);
  }
  fclose(g);
  printf("ok: %s\n", argv[1]);
  return 0;
}
