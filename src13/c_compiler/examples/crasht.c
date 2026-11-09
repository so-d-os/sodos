/* crasht.c - provoca uma excecao de proposito, para testar o CRASH.LOG.
 *   CRASHT div   divisao por zero (#DE)
 *   CRASHT ud    instrucao invalida (#UD)
 *   CRASHT gp    endereco nao canonico (#GP)
 *   CRASHT pf    endereco acima do que o kernel mapeia (#PF)
 * O SODOS para (tela azul) e grava X:\CRASH.LOG na unidade deste EXE. */
#include <stdio.h>
#include <string.h>

/* ud2 ; ret */
static unsigned char code_ud[] = { 0x0F, 0x0B, 0xC3 };
/* mov rax, 0x8000000000000000 ; mov al, [rax] ; ret */
static unsigned char code_gp[] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x8A, 0x00, 0xC3 };
/* mov rax, 0x0000700000000000 ; mov al, [rax] ; ret */
static unsigned char code_pf[] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0x70, 0, 0, 0x8A, 0x00, 0xC3 };

static int fundo(int n, int z) {
  if (n > 0) return fundo(n - 1, z) + 1;
  return 100 / z;
}

int main(int argc, char** argv) {
  void (*fn)(void) = 0;
  if (argc < 2) {
    printf("uso: CRASHT div | ud | gp | pf\n");
    return 1;
  }
  printf("CRASHT %s: o SODOS vai parar e gravar CRASH.LOG\n", argv[1]);
  if (!strcmp(argv[1], "div")) return fundo(3, argc - argc);
  if (!strcmp(argv[1], "ud")) fn = (void (*)(void))code_ud;
  if (!strcmp(argv[1], "gp")) fn = (void (*)(void))code_gp;
  if (!strcmp(argv[1], "pf")) fn = (void (*)(void))code_pf;
  if (!fn) {
    printf("tipo desconhecido: %s\n", argv[1]);
    return 1;
  }
  fn();
  printf("nao houve excecao?\n");
  return 2;
}
