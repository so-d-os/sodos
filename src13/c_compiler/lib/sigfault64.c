/* sigfault64.c - excecoes da CPU entregues como sinais (x64/LP64).
 *
 * O kernel roda tudo em ring 0, na pilha de quem falhou. Com uma entrada
 * registrada por k_fault_entry, o isr_common (kernel/isr_stubs.asm) troca o
 * destino do iretq por __so_fault_entry:
 *   rdi = frame salvo pelo isr_common (r11 r10 r9 r8 rdi rsi rbp rbx rdx rcx
 *         rax int_no err rip cs rflags rsp ss), intacto acima da pilha nova;
 *   rsi = CR2;  r12-r15, xmm e MXCSR = os do programa.
 * Aqui vira siginfo_t + ucontext_t (layout do Linux), o handler do sigaction
 * roda e o contexto que ele deixou e retomado com iretq (ring 0 -> ring 0).
 *
 * Sem handler (SIG_DFL/SIG_IGN), ou falta dentro do proprio handler: desliga a
 * entrada e retoma a instrucao que falhou. Ela falha de novo e o kernel mostra
 * a tela de EXCEPTION com o CRASH.LOG de sempre (o mesmo que o Linux faz ao
 * voltar de um handler que repos SIG_DFL).
 *
 * Quem usa: o SpiderMonkey compila os traps de WebAssembly como ud2 e espera
 * SIGILL (js/src/wasm/WasmSignalHandlers.cpp). Sem isto um trap derrubava o SO.
 */
#if defined(__x86_64__) && !defined(__ILP32__)

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <so.h>

struct sigaction* __so_sig_act(int sig);

/* Area montada pela entrada em asm, na pilha. iret primeiro: um IRQ entre o
 * cli e o iretq nao chega a ela (e o cli evita ate isso). */
struct so_fault_save {
  uint64_t iret[5];          /* 0: rip cs rflags rsp ss */
  uint64_t r12_15[4];        /* 40 */
  uint64_t pad[1];           /* 72 */
  uint8_t fx[512];           /* 80: fxsave64 (16 alinhado) */
  ucontext_t uc;             /* 592 */
};
_Static_assert(offsetof(struct so_fault_save, r12_15) == 40, "r12_15");
_Static_assert(offsetof(struct so_fault_save, fx) == 80, "fx");
_Static_assert(offsetof(struct so_fault_save, uc) == 592, "uc");
#define UC_GREG(r) (592 + offsetof(ucontext_t, uc_mcontext.gregs) + 8 * (r))
_Static_assert(UC_GREG(REG_R8) == 632, "gregs");

/* Indices do frame do isr_common. */
enum { F_R11, F_R10, F_R9, F_R8, F_RDI, F_RSI, F_RBP, F_RBX, F_RDX, F_RCX,
       F_RAX, F_VEC, F_ERR, F_RIP, F_CS, F_RFLAGS, F_RSP, F_SS };


static int fault_depth;
static int fault_armed;

void __so_fault_entry(void);
int __so_fault_dispatch(const uint64_t* fr, uint64_t cr2, struct so_fault_save* s);

static void fault_disarm(void) {
  int (*set)(void*) = (int (*)(void*))k_dl_sym(0, "k_fault_entry", 0);
  if (set) set(0);
  fault_armed = 0;
}

void __so_fault_arm(void) {
  int (*set)(void*);
  if (fault_armed) return;
  /* Kernel antigo nao tem k_fault_entry: segue sem sinais, como antes. */
  set = (int (*)(void*))k_dl_sym(0, "k_fault_entry", 0);
  if (set && set((void*)__so_fault_entry) == 0) fault_armed = 1;
}

/* Contexto do programa no momento da falha (gregs do Linux + fxsave). */
static void fill_ctx(const uint64_t* fr, uint64_t cr2, struct so_fault_save* s) {
  ucontext_t* uc = &s->uc;
  greg_t* g = uc->uc_mcontext.gregs;
  memset(uc, 0, sizeof(*uc));
  g[REG_R8] = fr[F_R8];
  g[REG_R9] = fr[F_R9];
  g[REG_R10] = fr[F_R10];
  g[REG_R11] = fr[F_R11];
  g[REG_R12] = s->r12_15[0];
  g[REG_R13] = s->r12_15[1];
  g[REG_R14] = s->r12_15[2];
  g[REG_R15] = s->r12_15[3];
  g[REG_RDI] = fr[F_RDI];
  g[REG_RSI] = fr[F_RSI];
  g[REG_RBP] = fr[F_RBP];
  g[REG_RBX] = fr[F_RBX];
  g[REG_RDX] = fr[F_RDX];
  g[REG_RAX] = fr[F_RAX];
  g[REG_RCX] = fr[F_RCX];
  g[REG_RSP] = fr[F_RSP];
  g[REG_RIP] = fr[F_RIP];
  g[REG_EFL] = fr[F_RFLAGS];
  g[REG_CSGSFS] = fr[F_CS];
  g[REG_ERR] = fr[F_ERR];
  g[REG_TRAPNO] = fr[F_VEC];
  g[REG_CR2] = cr2;
  /* fxsave64 tem o layout de struct _libc_fpstate do Linux. */
  uc->uc_mcontext.fpregs = (struct _libc_fpstate*)s->fx;

  s->iret[0] = fr[F_RIP];
  s->iret[1] = fr[F_CS];
  s->iret[2] = fr[F_RFLAGS];
  s->iret[3] = fr[F_RSP];
  s->iret[4] = fr[F_SS];
}

/* Entrega em andamento (para raise() durante o handler). */
static const uint64_t* cur_fr;
static uint64_t cur_cr2;
static struct so_fault_save* cur_save;
static int cur_sig;

void __so_fault_resume(struct so_fault_save* s) __attribute__((noreturn));

/* raise(sig) com SIG_DFL dentro do handler de uma falha do mesmo sinal: o
 * Linux mataria o processo com core. Aqui a instrucao original e retomada sem
 * entrada de falhas, e o kernel mostra a EXCEPTION/CRASH.LOG com o RIP de
 * verdade. Usado pelo nsProfileLock::FatalSignalHandler do Firefox. */
int __so_fault_raise(int sig) {
  if (fault_depth <= 0 || sig != cur_sig || !cur_save) return 0;
  fault_disarm();
  fill_ctx(cur_fr, cur_cr2, cur_save);
  __so_fault_resume(cur_save);
}

int __so_fault_dispatch(const uint64_t* fr, uint64_t cr2, struct so_fault_save* s) {
  ucontext_t* uc = &s->uc;
  greg_t* g = uc->uc_mcontext.gregs;
  struct sigaction* a;
  siginfo_t si;
  int sig, code = 0;
  void* addr = (void*)(uintptr_t)fr[F_RIP];

  switch (fr[F_VEC]) {
    case 0:  sig = SIGFPE; code = FPE_INTDIV; break;
    case 6:  sig = SIGILL; code = ILL_ILLOPN; break;
    case 14:
      sig = SIGSEGV;
      code = (fr[F_ERR] & 1) ? SEGV_ACCERR : SEGV_MAPERR;
      addr = (void*)(uintptr_t)cr2;
      break;
    case 13: sig = SIGSEGV; code = SI_KERNEL; addr = 0; break;
    case 17: sig = SIGBUS; code = BUS_ADRALN; break;
    default: sig = SIGFPE; code = FPE_FLTINV; break;  /* #MF, #XM */
  }

  fill_ctx(fr, cr2, s);

  memset(&si, 0, sizeof(si));
  si.si_signo = sig;
  si.si_code = code;
  si.si_addr = addr;

  a = __so_sig_act(sig);
  if (fault_depth > 0 || !a ||
      ((a->sa_flags & SA_SIGINFO) ? !a->sa_sigaction
                                  : (a->sa_handler == SIG_DFL || a->sa_handler == SIG_IGN))) {
    /* Sem quem trate: a mesma instrucao falha de novo, agora sem entrada. */
    fault_disarm();
  } else {
    struct sigaction h = *a;
    const uint64_t* pfr = cur_fr;
    uint64_t pcr2 = cur_cr2;
    struct so_fault_save* ps = cur_save;
    int psig = cur_sig;
    cur_fr = fr;
    cur_cr2 = cr2;
    cur_save = s;
    cur_sig = sig;
    fault_depth++;
    if (h.sa_flags & SA_RESETHAND) {
      a->sa_handler = SIG_DFL;
      a->sa_sigaction = 0;
      a->sa_flags &= ~SA_SIGINFO;
    }
    if (h.sa_flags & SA_SIGINFO)
      h.sa_sigaction(sig, &si, uc);
    else
      h.sa_handler(sig);
    fault_depth--;
    cur_fr = pfr;
    cur_cr2 = pcr2;
    cur_save = ps;
    cur_sig = psig;
  }

  s->iret[0] = (uint64_t)g[REG_RIP];
  s->iret[1] = fr[F_CS];
  s->iret[2] = (uint64_t)g[REG_EFL];
  s->iret[3] = (uint64_t)g[REG_RSP];
  s->iret[4] = fr[F_SS];
  return 0;
}

/* Entrada: [rsp] = 0 (retorno falso), rsp = 8 mod 16. */
__asm__(
  ".text\n"
  ".globl __so_fault_entry\n"
  ".type __so_fault_entry,@function\n"
  "__so_fault_entry:\n"
  
  "  subq $1600, %rsp\n"
  "  andq $-16, %rsp\n"
  "  fxsave64 80(%rsp)\n"
  "  movq %r12, 40(%rsp)\n"
  "  movq %r13, 48(%rsp)\n"
  "  movq %r14, 56(%rsp)\n"
  "  movq %r15, 64(%rsp)\n"
  "  movq %rsp, %rdx\n"
  "  movq %rsp, %rbx\n"
#ifdef __PIC__
  "  callq __so_fault_dispatch@PLT\n"
#else
  "  movabsq $__so_fault_dispatch, %rax\n"
  "  callq *%rax\n"
#endif
  "  movq %rbx, %rdi\n"
  ".globl __so_fault_resume\n"
  ".type __so_fault_resume,@function\n"
  "__so_fault_resume:\n"
  "  movq %rdi, %rbx\n"
  "  cli\n"
  "  fxrstor64 80(%rbx)\n"
  "  movq %rbx, %rax\n"
  "  movq %rbx, %rsp\n"
  "  movq 632(%rax), %r8\n"
  "  movq 640(%rax), %r9\n"
  "  movq 648(%rax), %r10\n"
  "  movq 656(%rax), %r11\n"
  "  movq 664(%rax), %r12\n"
  "  movq 672(%rax), %r13\n"
  "  movq 680(%rax), %r14\n"
  "  movq 688(%rax), %r15\n"
  "  movq 696(%rax), %rdi\n"
  "  movq 704(%rax), %rsi\n"
  "  movq 712(%rax), %rbp\n"
  "  movq 720(%rax), %rbx\n"
  "  movq 728(%rax), %rdx\n"
  "  movq 744(%rax), %rcx\n"
  "  movq 736(%rax), %rax\n"
  "  iretq\n"
  ".size __so_fault_entry, .-__so_fault_entry\n"
  ".size __so_fault_resume, .-__so_fault_resume\n");

_Static_assert(UC_GREG(REG_R9) == 640 && UC_GREG(REG_R15) == 688, "r9..r15");
_Static_assert(UC_GREG(REG_RDI) == 696 && UC_GREG(REG_RBX) == 720, "rdi..rbx");
_Static_assert(UC_GREG(REG_RDX) == 728 && UC_GREG(REG_RAX) == 736, "rdx rax");
_Static_assert(UC_GREG(REG_RCX) == 744, "rcx");
_Static_assert(sizeof(struct so_fault_save) + 16 <= 1600, "save cabe");

#endif
