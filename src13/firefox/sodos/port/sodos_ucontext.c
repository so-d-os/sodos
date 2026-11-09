/*
 * sodos_ucontext.c - getcontext/setcontext/makecontext/swapcontext (LP64).
 *
 * O suficiente para corrotinas (backend ucontext do QEMU): troca os
 * registradores callee-saved, rsp/rip, os 6 registradores de argumento,
 * MXCSR e a palavra de controle do x87. Nao ha mascara de sinais (o SODOS
 * nao tem sinais) nem salva registradores SSE (sao caller-saved na ABI).
 *
 * Layout de ucontext_t (c_compiler/include/ucontext.h):
 *   uc_link 8, uc_stack.ss_sp 16, uc_stack.ss_size 32,
 *   uc_mcontext.gregs[REG_x] em 40 + 8 * REG_x, __fpregs_mem em 424.
 */
#if defined(__x86_64__) && !defined(__ILP32__)

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <ucontext.h>

#define GREG_OFF(r) (offsetof(ucontext_t, uc_mcontext.gregs) + 8 * (r))
_Static_assert(offsetof(ucontext_t, uc_link) == 8, "uc_link");
_Static_assert(offsetof(ucontext_t, uc_stack.ss_sp) == 16, "ss_sp");
_Static_assert(offsetof(ucontext_t, uc_stack.ss_size) == 32, "ss_size");
_Static_assert(GREG_OFF(REG_R8) == 40, "gregs");
_Static_assert(GREG_OFF(REG_RIP) == 168, "rip");
_Static_assert(offsetof(ucontext_t, __fpregs_mem) == 424, "fpregs_mem");

int getcontext(ucontext_t *ucp);
int setcontext(const ucontext_t *ucp);
int swapcontext(ucontext_t *oucp, const ucontext_t *ucp);
void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...);
void __sodos_ctx_trampoline(void);

/* Grava o contexto em %rdi como se a chamada retornasse 0 ao chamador. */
#define SAVE_CTX                                   \
  "  movq %rbx, 128(%rdi)\n"                       \
  "  movq %rbp, 120(%rdi)\n"                       \
  "  movq %r12, 72(%rdi)\n"                        \
  "  movq %r13, 80(%rdi)\n"                        \
  "  movq %r14, 88(%rdi)\n"                        \
  "  movq %r15, 96(%rdi)\n"                        \
  "  movq %rdi, 104(%rdi)\n"                       \
  "  movq %rsi, 112(%rdi)\n"                       \
  "  movq %rdx, 136(%rdi)\n"                       \
  "  movq %rcx, 152(%rdi)\n"                       \
  "  movq %r8, 40(%rdi)\n"                         \
  "  movq %r9, 48(%rdi)\n"                         \
  "  movq (%rsp), %rcx\n"                          \
  "  movq %rcx, 168(%rdi)\n"                       \
  "  leaq 8(%rsp), %rcx\n"                         \
  "  movq %rcx, 160(%rdi)\n"                       \
  "  stmxcsr 424(%rdi)\n"                          \
  "  fnstcw 428(%rdi)\n"                           \
  "  movq 152(%rdi), %rcx\n"

__asm__(
  ".text\n"
  ".globl getcontext\n"
  ".type getcontext,@function\n"
  "getcontext:\n"
  SAVE_CTX
  "  xorl %eax, %eax\n"
  "  ret\n"
  ".size getcontext, .-getcontext\n"

  ".globl setcontext\n"
  ".type setcontext,@function\n"
  "setcontext:\n"
  "  ldmxcsr 424(%rdi)\n"
  "  fldcw 428(%rdi)\n"
  "  movq 160(%rdi), %rsp\n"
  "  movq 128(%rdi), %rbx\n"
  "  movq 120(%rdi), %rbp\n"
  "  movq 72(%rdi), %r12\n"
  "  movq 80(%rdi), %r13\n"
  "  movq 88(%rdi), %r14\n"
  "  movq 96(%rdi), %r15\n"
  "  pushq 168(%rdi)\n"
  "  movq 112(%rdi), %rsi\n"
  "  movq 136(%rdi), %rdx\n"
  "  movq 152(%rdi), %rcx\n"
  "  movq 40(%rdi), %r8\n"
  "  movq 48(%rdi), %r9\n"
  "  movq 104(%rdi), %rdi\n"
  "  xorl %eax, %eax\n"
  "  ret\n"
  ".size setcontext, .-setcontext\n"

  ".globl swapcontext\n"
  ".type swapcontext,@function\n"
  "swapcontext:\n"
  SAVE_CTX
  "  movq %rsi, %rdi\n"
  "  jmp setcontext\n"
  ".size swapcontext, .-swapcontext\n"

  /* Quando a funcao do makecontext retorna, cai aqui com %rbx = uc_link. */
  ".globl __sodos_ctx_trampoline\n"
  ".type __sodos_ctx_trampoline,@function\n"
  "__sodos_ctx_trampoline:\n"
  "  movq %rbx, %rdi\n"
  "  testq %rdi, %rdi\n"
  "  jz 1f\n"
  "  call setcontext\n"
  "1:\n"
  "  xorl %edi, %edi\n"
  "  call exit\n"
  "  hlt\n"
  ".size __sodos_ctx_trampoline, .-__sodos_ctx_trampoline\n");

void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...)
{
  static const int regs[6] = { REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9 };
  unsigned long top = (unsigned long)ucp->uc_stack.ss_sp + ucp->uc_stack.ss_size;
  int nstack = argc > 6 ? argc - 6 : 0;
  unsigned long *sp;
  va_list ap;
  int i;

  /* Na entrada da funcao, %rsp + 8 fica alinhado a 16, como depois de um call. */
  top = (top - 8UL * nstack) & ~15UL;
  sp = (unsigned long *)(top - 8);
  sp[0] = (unsigned long)__sodos_ctx_trampoline;

  va_start(ap, argc);
  for (i = 0; i < argc; i++) {
    /* Como na glibc: argumentos sao "int", lidos como palavra de 64 bits. */
    unsigned long v = (unsigned long)va_arg(ap, long);
    if (i < 6)
      ucp->uc_mcontext.gregs[regs[i]] = (long)v;
    else
      sp[1 + (i - 6)] = v;
  }
  va_end(ap);

  ucp->uc_mcontext.gregs[REG_RIP] = (long)func;
  ucp->uc_mcontext.gregs[REG_RSP] = (long)sp;
  ucp->uc_mcontext.gregs[REG_RBX] = (long)ucp->uc_link;
}

#endif
