/* stdio de arquivos: grava, le de volta, acrescenta. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
  FILE* f;
  char buf[128];
  char tmp[16];
  int n;
  int total = 0;
  f = fopen("TIO.TXT", "w");
  if (!f) {
    printf("FALHOU fopen w\n");
    return 1;
  }
  fprintf(f, "linha %d\n", 1);
  fputs("linha 2\n", f);
  fclose(f);
  f = fopen("TIO.TXT", "a");
  fputs("linha 3\n", f);
  fclose(f);
  f = fopen("TIO.TXT", "rb");
  if (!f) {
    printf("FALHOU fopen r\n");
    return 1;
  }
  while ((n = (int)fread(tmp, 1, sizeof(tmp), f)) > 0) {
    memcpy(buf + total, tmp, (size_t)n);
    total += n;
  }
  fclose(f);
  buf[total] = 0;
  if (strcmp(buf, "linha 1\nlinha 2\nlinha 3\n") != 0) {
    printf("FALHOU conteudo (%d bytes): [%s]\n", total, buf);
    return 1;
  }
  f = fopen("NAOEXISTE.TXT", "r");
  if (f) {
    printf("FALHOU: abriu arquivo que nao existe\n");
    return 1;
  }
  printf("ok: io (%d bytes)\n", total);
  return 0;
}
