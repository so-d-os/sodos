#include <stdio.h>

int main(int argc, char** argv) {
  FILE* g;
  int i;
  char buf[4096];
  int n;
  if (argc < 3) { printf("uso: CAT OUT IN1 IN2 ...\n"); return 1; }
  g = fopen(argv[1], "w");
  if (!g) { printf("nao criou %s\n", argv[1]); return 1; }
  for (i = 2; i < argc; i++) {
    FILE* f = fopen(argv[i], "r");
    if (!f) { printf("nao abriu %s\n", argv[i]); fclose(g); return 1; }
    while ((n = (int)fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, (size_t)n, g);
    fclose(f);
  }
  fclose(g);
  printf("ok\n");
  return 0;
}
