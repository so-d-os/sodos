/* calc.c - calculadora em dois arquivos: cc calc.c calcops.c
 * Uso: calc "2*(3+4)"   ou sem argumento para digitar linhas (vazio sai). */
#include <stdio.h>
#include <string.h>
#include "calcops.h"

static void run(const char* line) {
  const char* err;
  num v = calc_eval(line, &err);
  if (err) printf("erro: %s\n", err);
  else printf("%s = %lld\n", line, v);
}

int main(int argc, char** argv) {
  char line[128];
  int i;
  if (argc > 1) {
    for (i = 1; i < argc; i++) run(argv[i]);
    return 0;
  }
  printf("calc: digite uma expressao (linha vazia sai)\n");
  for (;;) {
    printf("> ");
    if (!fgets(line, sizeof(line), stdin)) break;
    line[strcspn(line, "\n")] = 0;
    if (!line[0]) break;
    run(line);
  }
  printf("%d operacoes\n", calc_ops_used());
  return 0;
}
