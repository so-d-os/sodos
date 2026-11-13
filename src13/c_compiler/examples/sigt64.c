/* sigt64.c - testa a entrega de excecoes como sinais (x64, LIBCX64).
 *   SIGT64        ud2, divisao por zero, #PF e #GP com handlers: o programa
 *                 continua e imprime "SIGT64 OK" (4 sinais).
 *   SIGT64 semh   ud2 sem handler: tela de EXCEPTION, como antes.
 *   SIGT64 dfl    handler que volta a SIG_DFL e retorna sem mexer no contexto
 *                 (o que o SpiderMonkey faz com uma falha que nao e dele): a
 *                 instrucao falha de novo e o SODOS para, como antes.
 *   SIGT64 raise  handler que repoe SIG_DFL e chama raise() (como o
 *                 nsProfileLock do Firefox): o SODOS para no ud2 original.
 * Os handlers pulam para um rotulo de retomada pelo ucontext, como o
 * SpiderMonkey faz com os traps de WebAssembly. */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

static volatile int got_sig, got_code;
static volatile unsigned long got_addr;
static volatile unsigned long resume;
static volatile long got_trap, got_cr2;

static volatile int dfl_calls;
static void handler_dfl(int sig, siginfo_t* si, void* ctx) {
  struct sigaction d;
  (void)si;
  (void)ctx;
  dfl_calls++;
  memset(&d, 0, sizeof(d));
  d.sa_handler = SIG_DFL;
  sigaction(sig, &d, 0);
}

static void handler_raise(int sig, siginfo_t* si, void* ctx) {
  struct sigaction d;
  (void)si;
  (void)ctx;
  memset(&d, 0, sizeof(d));
  d.sa_handler = SIG_DFL;
  sigaction(sig, &d, 0);
  raise(sig);
  printf("raise voltou\n");
}

static void handler(int sig, siginfo_t* si, void* ctx) {
  ucontext_t* uc = (ucontext_t*)ctx;
  got_sig = sig;
  got_code = si->si_code;
  got_addr = (unsigned long)si->si_addr;
  got_trap = uc->uc_mcontext.gregs[REG_TRAPNO];
  got_cr2 = uc->uc_mcontext.gregs[REG_CR2];
  uc->uc_mcontext.gregs[REG_RIP] = (long)resume;
  uc->uc_mcontext.gregs[REG_RAX] = 0x5151;
}

/* Executa a instrucao que falha; o handler retoma em 1: com rax = 0x5151.
 * r12 e xmm0 tem que sobreviver. */
__attribute__((noinline)) static long provoca(int tipo) {
  long rax;
  long r12 = 0x1212;
  double x = 2.5;
  resume = 0;
  __asm__ volatile(
      "movabsq $1f, %%rax\n\t"
      "movq %%rax, resume(%%rip)\n\t"
      "movq %[r12v], %%r12\n\t"
      "movq $0, %%rax\n\t"
      "cmpl $0, %[t]\n\t"
      "jne 2f\n\t"
      "ud2\n\t"
      "2: cmpl $1, %[t]\n\t"
      "jne 3f\n\t"
      "xorl %%ecx, %%ecx\n\t"
      "movl $7, %%eax\n\t"
      "cltd\n\t"
      "idivl %%ecx\n\t"
      "3: cmpl $2, %[t]\n\t"
      "jne 4f\n\t"
      "movabsq $0x0000700000000000, %%rcx\n\t"
      "movb (%%rcx), %%al\n\t"
      "4: movabsq $0x8000000000000000, %%rcx\n\t"
      "movb (%%rcx), %%al\n\t"
      "1: movq %%r12, %[r12v]\n\t"
      : "=&a"(rax), [r12v] "+m"(r12), "+x"(x)
      : [t] "r"(tipo)
      : "rcx", "rdx", "r12", "memory", "cc");
  if (r12 != 0x1212) return -1;
  if (x != 2.5) return -2;
  return rax;
}

int main(int argc, char** argv) {
  struct sigaction sa;
  static const int esperado[4] = { SIGILL, SIGFPE, SIGSEGV, SIGSEGV };
  static const char* nome[4] = { "ud2", "div0", "#PF", "#GP" };
  int i, ok = 0;

  if (argc > 1 && !strcmp(argv[1], "semh")) {
    printf("SIGT64 semh: ud2 sem handler, o SODOS deve parar\n");
    provoca(0);
    printf("nao parou?\n");
    return 2;
  }
  memset(&sa, 0, sizeof(sa));
  if (argc > 1 && (!strcmp(argv[1], "dfl") || !strcmp(argv[1], "raise"))) {
    sa.sa_sigaction = argv[1][0] == 'd' ? handler_dfl : handler_raise;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, 0);
    printf("SIGT64 %s: handler repoe SIG_DFL, o SODOS deve parar\n", argv[1]);
    provoca(0);
    printf("nao parou? (handler chamado %d vezes)\n", dfl_calls);
    return 2;
  }
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGILL, &sa, 0);
  sigaction(SIGFPE, &sa, 0);
  sigaction(SIGSEGV, &sa, 0);

  for (i = 0; i < 4; i++) {
    long r;
    got_sig = 0;
    r = provoca(i);
    printf("%-5s sinal=%d codigo=%d addr=%lx trap=%ld cr2=%lx rax=%lx %s\n", nome[i], got_sig,
           got_code, got_addr, got_trap, got_cr2, (unsigned long)r,
           (got_sig == esperado[i] && r == 0x5151) ? "ok" : "FALHOU");
    if (got_sig == esperado[i] && r == 0x5151) ok++;
  }
  printf(ok == 4 ? "SIGT64 OK (4 sinais)\n" : "SIGT64 FALHOU (%d de 4)\n", ok);
  return ok == 4 ? 0 : 1;
}
