/*
 * exc.c - Excecoes do Windows: faltas da CPU, handlers vetorizados, contexto.
 *
 * O kernel entrega as excecoes da CPU do programa (#DE #UD #GP #PF #MF #XM)
 * a entrada registrada com k_fault_entry, na pilha de quem falhou:
 *   rdi = frame do isr_common (r11 r10 r9 r8 rdi rsi rbp rbx rdx rcx rax
 *         int_no err rip cs rflags rsp ss), rsi = CR2; r12-r15 e xmm intactos.
 * Aqui isso vira EXCEPTION_RECORD + CONTEXT (layout x64 do Windows), passa
 * pelos handlers vetorizados (AddVectoredExceptionHandler; e como o runtime do
 * Go trata nil pointer, divisao por zero...) e o contexto que eles deixarem e
 * retomado com iretq.
 *
 * Ainda sem SEH por tabela (.pdata/__C_specific_handler, throw do C++):
 * excecao que nenhum handler vetorizado trata encerra o programa.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

/* CONTEXT x64 */
#define CTX_SIZE 1232
#define CTX_FLAGS 0x30
#define CTX_MXCSR 0x34
#define CTX_SEGCS 0x38
#define CTX_SEGSS 0x42
#define CTX_EFLAGS 0x44
#define CTX_RAX 0x78
#define CTX_RSP 0x98
#define CTX_RIP 0xF8
#define CTX_FLTSAVE 0x100
#define CTX_ALL 0x10000Bu /* AMD64 | CONTROL | INTEGER | FLOATING_POINT */

struct exc_record {
  DWORD code;
  DWORD flags;
  struct exc_record* next;
  void* address;
  DWORD nparams;
  DWORD pad;
  uintptr_t info[15];
};
struct exc_pointers {
  struct exc_record* rec;
  uint8_t* ctx;
};
/* LONG do Windows tem 32 bits (o long daqui tem 64): o retorno e int32_t */
typedef int32_t (WINAPI * veh_fn)(struct exc_pointers*);

enum { F_R11, F_R10, F_R9, F_R8, F_RDI, F_RSI, F_RBP, F_RBX, F_RDX, F_RCX,
       F_RAX, F_VEC, F_ERR, F_RIP, F_CS, F_RFLAGS, F_RSP, F_SS };

void wine_fault_entry(void);
__attribute__((noreturn)) void wine_ctx_resume(const uint8_t* ctx);
void wine_fault_dispatch(const uint64_t* fr, uint64_t cr2, uint8_t* ctx);

static void exc_report(const char* what, const struct exc_record* rec, const uint8_t* ctx) {
  uint64_t rip = *(const uint64_t*)(ctx + CTX_RIP);
  struct wmod* m;
  wlogx(what, rec->code);
  wlogx("  rip ", rip);
  wlogx("  rsp ", *(const uint64_t*)(ctx + CTX_RSP));
  if (rec->nparams >= 2) wlogx("  endereco ", rec->info[1]);
  m = pe_from_addr(rip);
  if (m) {
    wlog("  modulo ", m->name);
    wlogx("  rva ", rip - (uint64_t)(uintptr_t)m->base);
  } else {
    /* dentro do WINE.DLL: deslocamento a partir de wine_driver (ver LINK64_MAP) */
    extern const void* wine_driver(void);
    wlogx("  WINE.DLL, wine_driver + ", rip - (uint64_t)(uintptr_t)wine_driver);
  }
  if (wine_debug >= 2) { /* registradores; texto quando apontam para a imagem */
    static const char* const nm[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                       "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    const uint64_t* g = (const uint64_t*)(ctx + CTX_RAX);
    uint64_t rsp = g[4];
    int i;
    for (i = 0; i < 16; i++) {
      char t[64];
      int k = 0;
      const char* n = nm[i];
      t[k++] = ' ';
      t[k++] = ' ';
      while (*n) t[k++] = *n++;
      t[k++] = ' ';
      t[k] = 0;
      wlogx(t, g[i]);
      /* ponteiro na pilha (uma struct como base::Location): campos que
       * apontam para texto na imagem */
      if (g[i] && g[i] - rsp < 0x10000 && g[i] + 0x60 <= *(uint64_t*)(wteb() + TEB_STACKBASE)) {
        const uint64_t* f = (const uint64_t*)(uintptr_t)g[i];
        const char* in = (const char*)(uintptr_t)g[i] + 1; /* std::string curta (libc++) */
        int j;
        if (in[0] >= 32 && in[0] <= 126 && in[1] >= 32 && in[1] <= 126 && in[2] >= 32 && in[2] <= 126)
          wlog("    texto curto: ", in);
        for (j = 0; j < 12; j++) {
          struct wmod* mm = pe_from_addr(f[j]);
          const char* str = (const char*)(uintptr_t)f[j];
          int c, ok = 1;
          /* imagem, ou heap abaixo de 4 GB (mapeado 1:1, sempre legivel) */
          if ((!mm || !mm->base) && (f[j] < 0x100000 || f[j] >= 0x100000000ull)) continue;
          for (c = 0; c < 4; c++)
            if (str[c] < 32 || str[c] > 126) ok = 0;
          if (ok) wlog("    texto: ", str);
        }
      }
    }
  }
  { /* enderecos de retorno na pilha que caem em algum modulo (rva) */
    const uint64_t* sp = (const uint64_t*)(uintptr_t)*(const uint64_t*)(ctx + CTX_RSP);
    int i, shown = 0;
    uint64_t top = *(uint64_t*)(wteb() + TEB_STACKBASE);
    int lim = 256;
    if (top > (uint64_t)(uintptr_t)sp && (top - (uint64_t)(uintptr_t)sp) / 8 < (uint64_t)lim)
      lim = (int)((top - (uint64_t)(uintptr_t)sp) / 8);
    for (i = 0; i < lim && shown < 12; i++) {
      struct wmod* mm = pe_from_addr(sp[i]);
      if (mm && mm->base) {
        wlog("  pilha: ", mm->name);
        wlogx("    rva ", sp[i] - (uint64_t)(uintptr_t)mm->base);
        shown++;
      }
    }
  }
}

/* 1 = algum handler mandou continuar (o contexto pode ter mudado). */
static int exc_dispatch(struct exc_record* rec, uint8_t* ctx) {
  struct wproc* p = wp();
  struct exc_pointers ep;
  int i, cont = 0;
  ep.rec = rec;
  ep.ctx = ctx;
  for (i = 0; i < WVEH_MAX && !cont; i++)
    if (p->veh[i] && ((veh_fn)p->veh[i])(&ep) == -1) cont = 1;
  if (!cont && p->unhandled_filter) {
    /* o filtro do programa (crashpad) costuma encerrar o processo sem dizer
     * o que houve: com WINEDEBUG=2 o registro sai antes */
    if (wine_debug >= 2) exc_report("excecao para o filtro do programa, codigo ", rec, ctx);
    if (((veh_fn)p->unhandled_filter)(&ep) == -1) cont = 1;
  }
  /* O Windows chama os "continue handlers" nos dois casos. */
  for (i = 0; i < WVEH_MAX; i++)
    if (p->vch[i] && ((veh_fn)p->vch[i])(&ep) == -1) {
      cont = 1;
      break;
    }
  return cont;
}

static void exc_fatal(const struct exc_record* rec, const uint8_t* ctx) {
  exc_report("excecao nao tratada, codigo ", rec, ctx);
  k_puts("wine: excecao do Windows nao tratada (detalhes na serial)\n");
  wexit((int)rec->code);
}

void wine_fault_dispatch(const uint64_t* fr, uint64_t cr2, uint8_t* ctx) {
  struct exc_record rec;
  uint64_t* g = (uint64_t*)(ctx + CTX_RAX);
  uint64_t r12_15[4];
  memcpy(r12_15, ctx + 0xD8, sizeof(r12_15));
  memset(ctx, 0, CTX_FLTSAVE);
  memcpy(ctx + 0xD8, r12_15, sizeof(r12_15));
  memset(&rec, 0, sizeof(rec));
  *(DWORD*)(ctx + CTX_FLAGS) = CTX_ALL;
  *(DWORD*)(ctx + CTX_MXCSR) = *(DWORD*)(ctx + CTX_FLTSAVE + 0x18);
  *(uint16_t*)(ctx + CTX_SEGCS) = (uint16_t)fr[F_CS];
  *(uint16_t*)(ctx + CTX_SEGSS) = (uint16_t)fr[F_SS];
  *(DWORD*)(ctx + CTX_EFLAGS) = (DWORD)fr[F_RFLAGS];
  g[0] = fr[F_RAX];
  g[1] = fr[F_RCX];
  g[2] = fr[F_RDX];
  g[3] = fr[F_RBX];
  g[4] = fr[F_RSP];
  g[5] = fr[F_RBP];
  g[6] = fr[F_RSI];
  g[7] = fr[F_RDI];
  g[8] = fr[F_R8];
  g[9] = fr[F_R9];
  g[10] = fr[F_R10];
  g[11] = fr[F_R11];
  *(uint64_t*)(ctx + CTX_RIP) = fr[F_RIP];

  rec.address = (void*)(uintptr_t)fr[F_RIP];
  switch (fr[F_VEC]) {
    case 0: rec.code = 0xC0000094; break;          /* INT_DIVIDE_BY_ZERO */
    case 3:                                        /* int3: BREAKPOINT (o CHECK do Chromium) */
      rec.code = 0x80000003;
      rec.address = (void*)(uintptr_t)(fr[F_RIP] - 1);
      *(uint64_t*)(ctx + CTX_RIP) = fr[F_RIP] - 1;
      rec.nparams = 1;
      break;
    case 6: rec.code = 0xC000001D; break;          /* ILLEGAL_INSTRUCTION */
    case 14:
      rec.code = 0xC0000005;                       /* ACCESS_VIOLATION */
      rec.nparams = 2;
      rec.info[0] = (fr[F_ERR] & 0x10) ? 8 : ((fr[F_ERR] & 2) ? 1 : 0);
      rec.info[1] = cr2;
      break;
    case 13:
      rec.code = 0xC0000005;
      rec.nparams = 2;
      rec.info[0] = 0;
      rec.info[1] = ~(uintptr_t)0;
      break;
    case 17: rec.code = 0x80000002; break;         /* DATATYPE_MISALIGNMENT */
    default: rec.code = 0xC0000090; break;         /* FLT_INVALID_OPERATION */
  }
  if (!exc_dispatch(&rec, ctx)) exc_fatal(&rec, ctx);
  /* volta para a entrada em asm, que retoma ctx */
}

/* Entrada: [rsp] = 0 (retorno falso), rsp = 8 mod 16. */
__asm__(
    ".text\n"
    ".globl wine_fault_entry\n"
    ".type wine_fault_entry,@function\n"
    "wine_fault_entry:\n"
    "  subq $1248, %rsp\n"
    "  andq $-16, %rsp\n"
    "  fxsave64 0x100(%rsp)\n"
    "  movq %r12, 0xD8(%rsp)\n"
    "  movq %r13, 0xE0(%rsp)\n"
    "  movq %r14, 0xE8(%rsp)\n"
    "  movq %r15, 0xF0(%rsp)\n"
    "  movq %rsp, %rdx\n"
    "  movq %rsp, %rbx\n"
    "  movabsq $wine_fault_dispatch, %rax\n"
    "  callq *%rax\n"
    "  movq %rbx, %rdi\n"
    /* void wine_ctx_resume(const CONTEXT* rdi): retoma o contexto com iretq */
    ".globl wine_ctx_resume\n"
    ".type wine_ctx_resume,@function\n"
    "wine_ctx_resume:\n"
    "  movq %rdi, %rax\n"
    "  cli\n"
    "  fxrstor64 0x100(%rax)\n"
    "  movl %ss, %ecx\n"
    "  pushq %rcx\n"
    "  pushq 0x98(%rax)\n"
    "  movl 0x44(%rax), %ecx\n"
    "  orl $0x200, %ecx\n"
    "  pushq %rcx\n"
    "  movl %cs, %ecx\n"
    "  pushq %rcx\n"
    "  pushq 0xF8(%rax)\n"
    "  movq 0xB8(%rax), %r8\n"
    "  movq 0xC0(%rax), %r9\n"
    "  movq 0xC8(%rax), %r10\n"
    "  movq 0xD0(%rax), %r11\n"
    "  movq 0xD8(%rax), %r12\n"
    "  movq 0xE0(%rax), %r13\n"
    "  movq 0xE8(%rax), %r14\n"
    "  movq 0xF0(%rax), %r15\n"
    "  movq 0xB0(%rax), %rdi\n"
    "  movq 0xA8(%rax), %rsi\n"
    "  movq 0xA0(%rax), %rbp\n"
    "  movq 0x90(%rax), %rbx\n"
    "  movq 0x88(%rax), %rdx\n"
    "  movq 0x80(%rax), %rcx\n"
    "  movq 0x78(%rax), %rax\n"
    "  iretq\n"
    ".size wine_fault_entry, .-wine_fault_entry\n"
    /* void WINAPI RtlCaptureContext(CONTEXT* rcx): contexto de quem chamou */
    ".globl wine_RtlCaptureContext\n"
    ".type wine_RtlCaptureContext,@function\n"
    "wine_RtlCaptureContext:\n"
    "  pushfq\n"
    "  movq %rax, 0x78(%rcx)\n"
    "  movq %rcx, 0x80(%rcx)\n"
    "  movq %rdx, 0x88(%rcx)\n"
    "  movq %rbx, 0x90(%rcx)\n"
    "  leaq 16(%rsp), %rax\n"
    "  movq %rax, 0x98(%rcx)\n"
    "  movq %rbp, 0xA0(%rcx)\n"
    "  movq %rsi, 0xA8(%rcx)\n"
    "  movq %rdi, 0xB0(%rcx)\n"
    "  movq %r8, 0xB8(%rcx)\n"
    "  movq %r9, 0xC0(%rcx)\n"
    "  movq %r10, 0xC8(%rcx)\n"
    "  movq %r11, 0xD0(%rcx)\n"
    "  movq %r12, 0xD8(%rcx)\n"
    "  movq %r13, 0xE0(%rcx)\n"
    "  movq %r14, 0xE8(%rcx)\n"
    "  movq %r15, 0xF0(%rcx)\n"
    "  movq 8(%rsp), %rax\n"
    "  movq %rax, 0xF8(%rcx)\n"
    "  movq (%rsp), %rax\n"
    "  movl %eax, 0x44(%rcx)\n"
    "  movl $0x10000B, 0x30(%rcx)\n"
    "  movw %cs, 0x38(%rcx)\n"
    "  movw %ss, 0x42(%rcx)\n"
    "  fxsave64 0x100(%rcx)\n"
    "  stmxcsr 0x34(%rcx)\n"
    "  movq 0x78(%rcx), %rax\n"
    "  popfq\n"
    "  ret\n"
    ".size wine_RtlCaptureContext, .-wine_RtlCaptureContext\n");

void WINAPI wine_RtlCaptureContext(uint8_t* ctx);

void exc_arm(void) {
  struct wproc* p = wp();
  if (p->fault_armed) return;
  if (k_fault_entry((void*)wine_fault_entry) == 0) p->fault_armed = 1;
  else wlog("k_fault_entry falhou: faltas da CPU nao viram excecoes", 0);
}

static void* add_handler(void** tab, DWORD first, void* fn) {
  int i;
  wlock();
  if (first) {
    for (i = WVEH_MAX - 1; i > 0; i--) tab[i] = tab[i - 1];
    tab[0] = fn;
  } else {
    for (i = 0; i < WVEH_MAX && tab[i]; i++) {
    }
    if (i < WVEH_MAX) tab[i] = fn;
    else fn = 0;
  }
  wunlock();
  return fn;
}
static DWORD del_handler(void** tab, void* fn) {
  int i, j;
  for (i = 0; i < WVEH_MAX; i++)
    if (tab[i] == fn && fn) {
      for (j = i; j < WVEH_MAX - 1; j++) tab[j] = tab[j + 1];
      tab[WVEH_MAX - 1] = 0;
      return 1;
    }
  return 0;
}
static WINAPI void* AddVectoredExceptionHandler(DWORD first, void* fn) { return add_handler(wp()->veh, first, fn); }
static WINAPI void* AddVectoredContinueHandler(DWORD first, void* fn) { return add_handler(wp()->vch, first, fn); }
static WINAPI DWORD RemoveVectoredExceptionHandler(void* h) { return del_handler(wp()->veh, h); }
static WINAPI DWORD RemoveVectoredContinueHandler(void* h) { return del_handler(wp()->vch, h); }
static WINAPI void* SetUnhandledExceptionFilter(void* f) {
  struct wproc* p = wp();
  void* old = p->unhandled_filter;
  p->unhandled_filter = f;
  return old;
}
static WINAPI long UnhandledExceptionFilter(void* e) {
  (void)e;
  return 1; /* EXCEPTION_EXECUTE_HANDLER */
}

static WINAPI void RaiseException(DWORD code, DWORD fl, DWORD n, const uintptr_t* args) {
  uint8_t ctx[CTX_SIZE] __attribute__((aligned(16)));
  struct exc_record rec;
  DWORD i;
  wine_RtlCaptureContext(ctx);
  memset(&rec, 0, sizeof(rec));
  rec.code = code;
  rec.flags = fl;
  rec.address = __builtin_return_address(0);
  if (n > 15) n = 15;
  rec.nparams = args ? n : 0;
  for (i = 0; i < rec.nparams; i++) rec.info[i] = args[i];
  if (exc_dispatch(&rec, ctx)) return;
  exc_fatal(&rec, ctx);
}
static WINAPI void RaiseFailFastException(struct exc_record* rec, uint8_t* ctx, DWORD fl) {
  (void)ctx;
  (void)fl;
  wlogx("RaiseFailFastException, codigo ", rec ? rec->code : 0);
  k_puts("wine: o programa abortou (fail fast)\n");
  wexit(rec ? (int)rec->code : (int)0xC0000409);
}
static WINAPI __attribute__((noreturn)) void RtlRestoreContext(uint8_t* ctx, void* rec) {
  (void)rec;
  wine_ctx_resume(ctx);
}
static WINAPI long NtContinue(uint8_t* ctx, BOOL alert) {
  (void)alert;
  wine_ctx_resume(ctx);
}

/* .pdata: RUNTIME_FUNCTION {begin, end, unwind} em ordem de endereco. */
static WINAPI void* RtlLookupFunctionEntry(uint64_t pc, uint64_t* base, void* hist) {
  struct wmod* m = pe_from_addr(pc);
  const uint32_t* t;
  uint32_t rva, lo = 0, hi;
  (void)hist;
  if (!m || !m->pdata_rva) return 0;
  if (base) *base = (uint64_t)(uintptr_t)m->base;
  rva = (uint32_t)(pc - (uint64_t)(uintptr_t)m->base);
  t = (const uint32_t*)(m->base + m->pdata_rva);
  hi = m->pdata_size / 12;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (rva < t[mid * 3]) hi = mid;
    else if (rva >= t[mid * 3 + 1]) lo = mid + 1;
    else return (void*)&t[mid * 3];
  }
  return 0;
}
static WINAPI void* RtlVirtualUnwind(DWORD t, uint64_t b, uint64_t pc, void* f, void* c, void** h,
                                     uint64_t* e, void* p) {
  (void)t; (void)b; (void)pc; (void)f; (void)c; (void)e; (void)p;
  if (h) *h = 0;
  return 0;
}
static WINAPI void RtlUnwindEx(void* a, void* b, void* c, void* d, void* e, void* f) {
  (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
  wsay("RtlUnwindEx: SEH por tabela/excecoes C++ ainda nao suportados", 0);
  wexit((int)0xC0000027);
}
static WINAPI DWORD WerSetFlags(DWORD f) {
  (void)f;
  return 0;
}
static WINAPI DWORD WerGetFlags(HANDLE h, DWORD* f) {
  (void)h;
  if (f) *f = 0;
  return 0;
}
static WINAPI DWORD SetErrorMode(DWORD m) {
  (void)m;
  return 0;
}
static WINAPI DWORD GetErrorMode(void) { return 0; }

#define S(n) {#n, (void*)n}
const struct wine_sym exc_syms[] = {
    S(AddVectoredExceptionHandler), S(AddVectoredContinueHandler), S(RemoveVectoredExceptionHandler),
    S(RemoveVectoredContinueHandler), S(SetUnhandledExceptionFilter), S(UnhandledExceptionFilter),
    S(RaiseException), S(RaiseFailFastException), S(RtlRestoreContext), S(NtContinue),
    {"RtlCaptureContext", (void*)wine_RtlCaptureContext}, S(RtlLookupFunctionEntry),
    S(RtlVirtualUnwind), S(RtlUnwindEx), S(WerSetFlags), S(WerGetFlags), S(SetErrorMode),
    S(GetErrorMode),
    {0, 0}};
