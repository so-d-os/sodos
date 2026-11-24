#include <stdio.h>
int main(int argc, char** argv) {
  FILE* f; int c, n = 0, last = 0;
  if (argc < 2) return 1;
  f = fopen(argv[1], "r");
  if (!f) { printf("erro\n"); return 1; }
  while ((c = fgetc(f)) != EOF) { n++; if (c == '\n') last++; }
  fclose(f);
  printf("%s: %d linhas, %d bytes\n", argv[1], last, n);
  return 0;
}
