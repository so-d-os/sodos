/* hello64c.c - LIBCX64 estatica: printf, malloc e snprintf em LP64. */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
  char* p = (char*)malloc(48);
  if (!p) return 1;
  snprintf(p, 48, "HELLO64C: argc=%d argv0=%s", argc, argc > 0 ? argv[0] : "?");
  printf("%s\n", p);
  free(p);
  return 0;
}
