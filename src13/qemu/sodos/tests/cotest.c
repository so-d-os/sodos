/* cotest.c - o padrao exato do util/coroutine-ucontext.c do QEMU 9.2. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <ucontext.h>

typedef struct {
  void* entry_arg;
  void* stack;
  size_t stack_size;
  sigjmp_buf env;
  int ran;
} Co;

union cc_arg {
  void* p;
  int i[2];
};

static Co* leader_co;

static void trampoline(int i0, int i1)
{
  union cc_arg arg;
  Co* self;
  arg.i[0] = i0;
  arg.i[1] = i1;
  self = arg.p;
  printf("trampolim: i0=%08x i1=%08x self=%p pilha~%p\n", (unsigned)i0, (unsigned)i1, arg.p, (void*)&arg);
  if (!sigsetjmp(self->env, 0)) siglongjmp(*(sigjmp_buf*)self->entry_arg, 1);
  for (;;) {
    self->ran++;
    siglongjmp(leader_co->env, 1);
  }
}

static Co* co_new(void)
{
  Co* co;
  ucontext_t old_uc, uc;
  sigjmp_buf old_env;
  union cc_arg arg = {0};
  if (getcontext(&uc) == -1) abort();
  co = calloc(1, sizeof *co);
  co->stack_size = 1 << 20;
  co->stack = mmap(NULL, co->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  co->entry_arg = &old_env;
  uc.uc_link = &old_uc;
  uc.uc_stack.ss_sp = co->stack;
  uc.uc_stack.ss_size = co->stack_size;
  uc.uc_stack.ss_flags = 0;
  arg.p = co;
  printf("co=%p pilha=%p..%p\n", (void*)co, co->stack, (char*)co->stack + co->stack_size);
  makecontext(&uc, (void (*)(void))trampoline, 2, arg.i[0], arg.i[1]);
  printf("depois do makecontext: rip=%lx rsp=%lx rdi=%lx rsi=%lx\n",
         (unsigned long)uc.uc_mcontext.gregs[REG_RIP], (unsigned long)uc.uc_mcontext.gregs[REG_RSP],
         (unsigned long)uc.uc_mcontext.gregs[REG_RDI], (unsigned long)uc.uc_mcontext.gregs[REG_RSI]);
  if (!sigsetjmp(old_env, 0)) swapcontext(&old_uc, &uc);
  return co;
}

int main(void)
{
  static Co leader;
  Co* co;
  leader_co = &leader;
  co = co_new();
  printf("criada\n");
  if (!sigsetjmp(leader.env, 0)) siglongjmp(co->env, 1);
  {
    /* longjmp devolve o valor pedido (o QEMU usa 1, 2 e 3) e 0 vira 1 */
    static sigjmp_buf b;
    static volatile int n, got[3];
    int r = sigsetjmp(b, 0);
    if (n > 0) got[n - 1] = r;
    if (n < 3) {
      n++;
      siglongjmp(b, n == 1 ? 3 : n == 2 ? 2 : 0);
    }
    printf("longjmp valores: %d %d %d\n", got[0], got[1], got[2]);
    if (got[0] != 3 || got[1] != 2 || got[2] != 1) co->ran = -1;
  }
  printf("COTEST: %s (ran=%d)\n", co->ran == 1 ? "ok" : "FALHOU", co->ran);
  return co->ran != 1;
}
