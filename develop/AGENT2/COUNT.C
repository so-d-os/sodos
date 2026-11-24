#include <stdio.h>

int main(int argc, char** argv) {
  FILE* f;
  char line[4096];
  int n = 0;
  if (argc < 2) return 1;
  f = fopen(argv[1], "r");
  if (!f) { printf("nao abriu\n"); return 1; }
  while (fgets(line, sizeof line, f)) n++;
  fclose(f);
  printf("%d linhas\n", n);
  return 0;
}
