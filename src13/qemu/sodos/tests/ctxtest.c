/* ctxtest.c - ucontext + sigsetjmp no SODOS, do jeito que o QEMU usa. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

static ucontext_t main_uc, co_uc, done_uc;
static int fails, steps;
static sigjmp_buf main_env, co_env;

static void co_fn(int a, int b, int c, int d, int e, int f, int g, int h)
{
  if (a + b + c + d + e + f + g + h != 36) { printf("FAIL args %d %d %d %d %d %d %d %d\n", a, b, c, d, e, f, g, h); fails++; }
  else printf("PASS makecontext: 8 argumentos (6 em registrador, 2 na pilha)\n");
  {
    double x = 1.5; /* pilha alinhada: movaps em double/printf nao pode falhar */
    printf("PASS pilha da corrotina: %.2f\n", x * 2);
  }
  steps++;
  swapcontext(&co_uc, &main_uc);  /* volta ao main */
  steps++;
  /* estilo QEMU: daqui em diante so sigsetjmp/siglongjmp */
  if (!sigsetjmp(co_env, 0)) siglongjmp(main_env, 1);
  steps++;
  /* retorna: o trampolim segue uc_link (main_uc) */
}

int main(void)
{
  static char stack[64 * 1024] __attribute__((aligned(16)));
  volatile int phase = 0;
  getcontext(&done_uc);
  if (phase == 2) goto done;
  getcontext(&co_uc);
  co_uc.uc_stack.ss_sp = stack;
  co_uc.uc_stack.ss_size = sizeof stack;
  co_uc.uc_link = &done_uc;
  makecontext(&co_uc, (void (*)(void))co_fn, 8, 1, 2, 3, 4, 5, 6, 7, 8);
  swapcontext(&main_uc, &co_uc);
  if (phase == 0) {
    phase = 1;
    printf("%s swapcontext ida e volta (steps=%d)\n", steps == 1 ? "PASS" : "FAIL", steps);
    if (steps != 1) fails++;
    if (!sigsetjmp(main_env, 0)) swapcontext(&main_uc, &co_uc);
    printf("%s siglongjmp de volta da corrotina (steps=%d)\n", steps == 2 ? "PASS" : "FAIL", steps);
    if (steps != 2) fails++;
    phase = 2;
    if (!sigsetjmp(main_env, 0)) siglongjmp(co_env, 1);
  }
done:
  /* uc_link (done_uc) traz de volta ao getcontext do inicio */
  printf("%s fim da corrotina via uc_link (steps=%d)\n", steps == 3 ? "PASS" : "FAIL", steps);
  if (steps != 3) fails++;
  printf("CTXTEST: %s\n", fails ? "FALHOU" : "ok");
  return fails;
}
