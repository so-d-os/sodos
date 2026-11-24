#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
  FILE* f; int a, b, n = 0; char line[1024];
  if (argc < 4) { printf("uso: SLICE ARQ INI FIM\n"); return 1; }
  a = atoi(argv[2]); b = atoi(argv[3]);
  f = fopen(argv[1], "r");
  if (!f) { printf("erro abrir\n"); return 1; }
  while (fgets(line, sizeof line, f)) {
    n++;
    if (n >= a && n <= b) printf("%4d| %s", n, line);
  }
  fclose(f);
  return 0;
}
