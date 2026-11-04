/* ucontext.h minimo para o SpiderMonkey no SODOS (x86_64). */
#ifndef _UCONTEXT_H
#define _UCONTEXT_H

#include <stddef.h>

#ifndef _STACK_T_DEFINED
#define _STACK_T_DEFINED
typedef struct {
  void* ss_sp;
  int ss_flags;
  size_t ss_size;
} stack_t;
#endif

typedef long greg_t;
typedef greg_t gregset_t[23];

typedef struct {
  gregset_t gregs;
  void* fpregs;
  unsigned long __reserved1[8];
} mcontext_t;

typedef struct ucontext_t {
  unsigned long uc_flags;
  struct ucontext_t* uc_link;
  stack_t uc_stack;
  mcontext_t uc_mcontext;
  unsigned long uc_sigmask[16];
  unsigned long __fpregs_mem[64];
} ucontext_t;

#define REG_R8 0
#define REG_R9 1
#define REG_R10 2
#define REG_R11 3
#define REG_R12 4
#define REG_R13 5
#define REG_R14 6
#define REG_R15 7
#define REG_RDI 8
#define REG_RSI 9
#define REG_RBP 10
#define REG_RBX 11
#define REG_RDX 12
#define REG_RAX 13
#define REG_RCX 14
#define REG_RSP 15
#define REG_RIP 16
#define REG_EFL 17
#define REG_CSGSFS 18
#define REG_ERR 19
#define REG_TRAPNO 20
#define REG_OLDMASK 21
#define REG_CR2 22

#endif
