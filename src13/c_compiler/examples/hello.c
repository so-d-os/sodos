#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
  int i;
  char* s;
  long long big = 1234567890123LL;
  printf("Ola do C! argc=%d\n", argc);
  for (i = 0; i < argc; i++) printf("  argv[%d] = \"%s\"\n", i, argv[i]);
  s = strdup("memoria do malloc");
  printf("%s (%u bytes)\n", s, (unsigned)strlen(s));
  free(s);
  printf("64 bits: %lld / 1000 = %lld, resto %lld\n", big, big / 1000, big % 1000);
  printf("hex %08x, %-6s|, %+d\n", 0xBEEF, "esq", 42);
  return 0;
}
