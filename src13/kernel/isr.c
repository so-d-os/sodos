/* isr.c - Exception dispatch com sistema de erro detalhado (VGA direto, freestanding) */
#include <stdint.h>
#include "isr.h"
#include "irq.h"
#include "serial.h"
#include "visual.h"
#include "trace.h"
#include "module.h"
#include "drives.h"
#include "sched.h"
#include "net.h"

#include "fbcon.h"
#include "smp.h"
#define VGA vga_text  /* 0xB8000, ou a copia do fbcon no UEFI */
#define ATTR 0x1F00  /* fundo azul, texto branco */

static void vga_putc(int row, int col, char c) {
  if (row >= 0 && row < 25 && col >= 0 && col < 80)
    VGA[row * 80 + col] = (uint16_t)(unsigned char)c | ATTR;
}

static void vga_puts_at(int row, int* col, const char* s) {
  while (*s) {
    vga_putc(row, *col, *s++);
    (*col)++;
  }
}

/* Imprime uint32_t em hex (8 dígitos) na posição (row, col); retorna col após */
static void print_hex(uint32_t v, int row, int* col) {
  static const char hex[] = "0123456789ABCDEF";
  int i;
  for (i = 7; i >= 0; i--) {
    vga_putc(row, (*col)++, hex[(v >> (i * 4)) & 0xF]);
  }
}

/* Endereco lido pelo handler sem gerar outra falta: canonico e mapeado nas
 * tabelas do CR3 (que ficam em memoria identidade abaixo de 4 GB). */
static int mapped(uint64_t va) {
  uint64_t cr3, e;
  volatile uint64_t* t;
  int sh;
  if ((uint64_t)((int64_t)(va << 16) >> 16) != va) return 0;
  __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
  t = (volatile uint64_t*)(uintptr_t)(cr3 & 0x000FFFFFFFFFF000ull);
  for (sh = 39; sh >= 12; sh -= 9) {
    if ((uintptr_t)t >= 0x100000000ull) return 0;
    e = t[(va >> sh) & 511u];
    if (!(e & 1u)) return 0;
    if (sh == 12 || (sh <= 30 && (e & 0x80u))) return 1;
    t = (volatile uint64_t*)(uintptr_t)(e & 0x000FFFFFFFFFF000ull);
  }
  return 0;
}

static int dump_stack_words(uint64_t rsp, int row) {
  if ((rsp & 7u) || !mapped(rsp) || !mapped(rsp + 47u)) {
    int c = 0;
    vga_puts_at(row, &c, "RSP nao mapeado: pilha nao lida");
    return row + 1;
  }
  volatile uint64_t* p = (volatile uint64_t*)(uintptr_t)rsp;
  for (int i = 0; i < 6 && row < 25; i++) {
    int c = 0;
    vga_puts_at(row, &c, "RSP+");
    vga_putc(row, c++, '0' + (char)i);
    vga_puts_at(row, &c, "*8: 0x");
    print_hex((uint32_t)(p[i] >> 32), row, &c);
    print_hex((uint32_t)p[i], row, &c);
    row++;
  }
  return row;
}

/* Opcional: imprime em decimal (sem libc) */
static void print_dec(uint32_t v, int row, int* col) {
  char buf[12];
  int n = 0;
  if (v == 0) {
    buf[n++] = '0';
  } else {
    while (v > 0 && n < 11) {
      buf[n++] = (char)('0' + (v % 10));
      v /= 10;
    }
  }
  while (n > 0)
    vga_putc(row, (*col)++, buf[--n]);
}

/* ---- CRASH.LOG -------------------------------------------------------
 * O notebook nao tem COM1: antes da tela azul o handler grava o que sabe em
 * X:\CRASH.LOG (X = unidade do .EXE, senao U:, senao D:). O disco funciona
 * com interrupcoes desligadas (IDE e xHCI por polling, relogio lido do PIT);
 * o sched_freeze impede o FAT de ceder a vez a outra tarefa. Tudo roda numa
 * pilha propria: a do programa pode ser justamente o que estourou. */

static char crash_txt[16384];
static int crash_n;
static const char* crash_path;  /* unidade onde gravou, ou 0 */
static isr_frame_t* crash_f;
static uint64_t crash_cr2;
static uint8_t crash_stack[65536] __attribute__((aligned(16)));
static volatile int crash_depth;
static char crash_rip[48];  /* "QEMU.EXE+0x..." para a tela */

static void lc(char c) {
  if (crash_n < (int)sizeof(crash_txt) - 1) crash_txt[crash_n++] = c;
}
static void ls(const char* s) {
  while (*s) lc(*s++);
}
static void lhex(uint64_t v, int digits) {
  static const char hex[] = "0123456789ABCDEF";
  while (digits-- > 0) lc(hex[(v >> (digits * 4)) & 0xF]);
}
static void lh64(uint64_t v) { lhex(v, 16); }
static void ldec(uint64_t v) {
  char b[21];
  int n = 0;
  do b[n++] = (char)('0' + v % 10); while ((v /= 10) && n < 20);
  while (n) lc(b[--n]);
}

static const char* vec_name(uint64_t v) {
  static const char* const n[] = {
    "#DE divisao por zero", "#DB debug", "NMI", "#BP breakpoint", "#OF overflow",
    "#BR bound", "#UD instrucao invalida", "#NM sem FPU", "#DF double fault",
    "coproc", "#TS TSS invalido", "#NP segmento ausente", "#SS falta na pilha",
    "#GP protecao geral", "#PF falta de pagina", "reservado", "#MF x87",
    "#AC alinhamento", "#MC machine check", "#XM SIMD (SSE)", "#VE virtualizacao",
    "#CP control protection" };
  return v < sizeof(n) / sizeof(n[0]) ? n[v] : "reservado";
}

extern char __bss_start[];
#define KERNEL_LOAD 0x01000000ull

/* Onde fica o endereco a: "QEMU.EXE+0x1234" ou "kernel+0x..". 0 = em nenhum. */
static int where(uint64_t a) {
  struct mod_info m;
  int i;
  if (a >= KERNEL_LOAD && a < (uint64_t)(uintptr_t)__bss_start) {
    ls("kernel+0x");
    lhex(a - KERNEL_LOAD, 6);
    return 1;
  }
  if (a >= 0x100000000ull) return 0;
  for (i = 0; i < MOD_MAX; i++) {
    if (!mod_get(i, &m) || !m.base) continue;
    if (a >= m.base && a < (uint64_t)m.base + m.size) {
      ls(m.name);
      ls("+0x");
      lhex(a - m.base, 8);
      return 1;
    }
  }
  return 0;
}

static void crash_build(void) {
  isr_frame_t* f = crash_f;
  struct mod_info m;
  uint64_t cr0, cr3, cr4;
  int i;
  __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
  __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
  crash_n = 0;
  ls("SODOS CRASH.LOG\r\nuptime ");
  ldec(net_ms());
  ls(" ms  pid ");
  ldec(mod_current_pid());
  ls("  exe da unidade ");
  lc(mod_exe_drive() ? mod_exe_drive() : '-');
  ls(":\r\n\r\nvetor ");
  ldec(f->int_no);
  ls(" ");
  ls(vec_name(f->int_no));
  ls("  err=0x");
  lhex(f->err_code, 8);
  if (f->int_no == 14) {
    ls(f->err_code & 1u ? "  (protecao" : "  (pagina ausente");
    ls(f->err_code & 2u ? ", escrita" : ", leitura");
    if (f->err_code & 16u) ls(", busca de instrucao");
    if (f->err_code & 8u) ls(", bit reservado");
    ls(")");
  }
  ls("\r\nRIP=");
  lh64(f->eip);
  ls("  ");
  {
    int at = crash_n;
    if (!where(f->eip)) ls("(fora do kernel e dos modulos)");
    for (i = 0; at + i < crash_n && i < (int)sizeof(crash_rip) - 1; i++)
      crash_rip[i] = crash_txt[at + i];
    crash_rip[i] = 0;
  }
  ls("\r\nRSP=");
  lh64(f->esp_dummy);
  ls(" RBP=");
  lh64(f->ebp);
  ls(" CR2=");
  lh64(crash_cr2);
  ls("\r\nRAX=");
  lh64(f->rax);
  ls(" RBX=");
  lh64(f->rbx);
  ls(" RCX=");
  lh64(f->rcx);
  ls("\r\nRDX=");
  lh64(f->rdx);
  ls(" RSI=");
  lh64(f->rsi);
  ls(" RDI=");
  lh64(f->rdi);
  ls("\r\nCS=");
  lhex(f->cs, 4);
  ls(" SS=");
  lhex(f->ss, 4);
  ls(" RFLAGS=");
  lhex(f->eflags, 8);
  ls(" CR0=");
  lhex(cr0, 8);
  ls(" CR3=");
  lhex(cr3, 8);
  ls(" CR4=");
  lhex(cr4, 8);
  ls("\r\n");

  if (mapped(f->eip) && mapped(f->eip + 15u)) {
    volatile uint8_t* p = (volatile uint8_t*)(uintptr_t)f->eip;
    ls("bytes em RIP:");
    for (i = 0; i < 16; i++) {
      lc(' ');
      lhex(p[i], 2);
    }
    ls("\r\n");
  }

  ls("\r\nmodulos (base tamanho nome):\r\n");
  for (i = 0; i < MOD_MAX; i++) {
    if (!mod_get(i, &m)) continue;
    ls("  ");
    lhex(m.base, 8);
    lc(' ');
    lhex(m.size, 8);
    lc(' ');
    ls(m.is_dll ? "DLL " : "EXE ");
    ls(m.name);
    ls("  pid ");
    ldec(m.pid);
    ls("\r\n");
  }

  ls("\r\npilha a partir de RSP");
  if ((f->esp_dummy & 7u) || !mapped(f->esp_dummy)) {
    ls(": RSP nao mapeado (pilha estourada ou corrompida?)\r\n");
  } else {
    volatile uint64_t* sp = (volatile uint64_t*)(uintptr_t)f->esp_dummy;
    ls(" (* = aponta para codigo):\r\n");
    for (i = 0; i < 64; i++) {
      uint64_t a = (uint64_t)(uintptr_t)&sp[i];
      if ((a & 0xFFFu) == 0 && !mapped(a)) {
        ls("  (fim da memoria mapeada)\r\n");
        break;
      }
      ls("  +");
      lhex((uint64_t)i * 8u, 3);
      lc(' ');
      lh64(sp[i]);
      {
        int mark = crash_n;
        ls("  * ");
        if (!where(sp[i])) crash_n = mark;
      }
      ls("\r\n");
    }
  }

  ls("\r\ncadeia de RBP:\r\n");
  {
    uint64_t bp = f->ebp;
    for (i = 0; i < 16; i++) {
      volatile uint64_t* fr;
      if ((bp & 7u) || !mapped(bp) || !mapped(bp + 15u)) {
        ls("  (RBP=");
        lh64(bp);
        ls(" fora da memoria: fim)\r\n");
        break;
      }
      fr = (volatile uint64_t*)(uintptr_t)bp;
      ls("  ret ");
      lh64(fr[1]);
      ls("  ");
      where(fr[1]);
      ls("\r\n");
      if (fr[0] <= bp) break;
      bp = fr[0];
    }
  }

  ls("\r\ntrace do kernel (mais recente por ultimo):\r\n");
  crash_n += trace_dump_text(crash_txt + crash_n, (int)sizeof(crash_txt) - 1 - crash_n);
  crash_txt[crash_n] = 0;
}

static void crash_save(void) {
  static char path[] = "?:\\CRASH.LOG";
  char order[3] = { mod_exe_drive(), 'U', 'D' };
  int i, k;
  for (i = 0; i < 3; i++) {
    char L = order[i];
    if (!L) continue;
    if (L >= 'a' && L <= 'z') L = (char)(L - 32);
    for (k = 0; k < i; k++) if (order[k] == L) break;
    if (k < i) continue;
    /* So disco FAT ja montado: C: e RAM e some com o reset; U: sem pendrive
     * faria o xHCI procurar de novo. */
    if (drives_kind_name(L)[0] != 'F') continue;
    path[0] = L;
    if (file_save(path, crash_txt, crash_n) >= 0) {
      crash_path = path;
      return;
    }
  }
}

static void crash_on_stack(void) {
  crash_build();
  serial_puts("[exc] CRASH.LOG:\n");
  serial_puts(crash_txt);
  crash_save();
}

/* Chama fn com RSP no topo da pilha top; volta para a pilha antiga. */
static void call_on_stack(void (*fn)(void), void* top) {
  __asm__ volatile (
    "mov %%rsp, %%rbx\n\t"
    "mov %0, %%rsp\n\t"
    "call *%1\n\t"
    "mov %%rbx, %%rsp\n\t"
    : : "r"(top), "r"(fn)
    : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
}

/* ---- Excecoes entregues ao programa (sinais sincronos) ----
 * Tudo roda em ring 0 na pilha de quem falhou, entao "entregar" e so trocar o
 * destino do iretq: o isr_common pula para a entrada registrada com rdi = o
 * frame salvo (que fica intacto acima da nova pilha) e rsi = CR2. A libc
 * (c_compiler/lib/sigfault64.c) monta siginfo/ucontext, chama o handler do
 * sigaction e retoma no contexto que ele deixou. O SpiderMonkey precisa disso:
 * os traps de WebAssembly sao ud2 e esperam SIGILL. */
#define FAULT_MAX 32
static const isr_ret_t isr_ret0 = { 0, 0 };
static struct {
  uint32_t pid;
  uint64_t entry;
} fault_tab[FAULT_MAX];

int isr_fault_set(uint64_t entry) {
  uint32_t pid = mod_current_pid();
  int i, freei = -1;
  if (!pid) return -1;
  for (i = 0; i < FAULT_MAX; i++) {
    if (fault_tab[i].pid == pid) {
      if (!entry) fault_tab[i].pid = 0;
      fault_tab[i].entry = entry;
      return 0;
    }
    if (!fault_tab[i].pid && freei < 0) freei = i;
  }
  if (!entry) return 0;
  if (freei < 0) return -1;
  fault_tab[freei].entry = entry;
  fault_tab[freei].pid = pid;
  return 0;
}

void isr_fault_release(uint32_t pid) {
  int i;
  for (i = 0; i < FAULT_MAX; i++)
    if (fault_tab[i].pid == pid) {
      fault_tab[i].pid = 0;
      fault_tab[i].entry = 0;
    }
}

static uint64_t fault_target(const isr_frame_t* f) {
  extern char __bss_end[];
  uint32_t pid = mod_current_pid();
  uint64_t v = f->int_no;
  int i;
  if (!pid || crash_depth) return 0;
  if (v != 0 && v != 6 && v != 13 && v != 14 && v != 16 && v != 17 && v != 19) return 0;
  /* Falta dentro do kernel (num k_* chamado pelo programa): tela de sempre. */
  if (f->eip >= KERNEL_LOAD && f->eip < (uint64_t)(uintptr_t)__bss_end) return 0;
  if (f->eip < 0x100000) return 0;
  /* A entrada roda logo abaixo do frame: a pilha tem que existir ali. */
  if (!mapped((uint64_t)(uintptr_t)f - 0x4000) || !mapped(f->esp_dummy)) return 0;
  for (i = 0; i < FAULT_MAX; i++)
    if (fault_tab[i].pid == pid && fault_tab[i].entry) return fault_tab[i].entry;
  return 0;
}

isr_ret_t isr_dispatch(isr_frame_t* f) {
  __asm__ volatile ("cli");

  /* IPI do SMP (vetor 0xF1): so confirma no LAPIC e volta. */
  if (f->int_no == 0xF1) {
    smp_ipi_ack();
    return isr_ret0;
  }

  /* IRQ 32..47: enviar EOI e retornar (nao travar) */
  if (f->int_no >= 32 && f->int_no <= 47) {
    irq_handle(f);
    return isr_ret0;
  }

  /* Programa com entrada de falhas: ele decide (sinal). CR2 volta em rdx. */
  {
    isr_ret_t r;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(r.cr2));
    r.entry = fault_target(f);
    if (r.entry) return r;
  }

  /* Excecao 0..31: log, panic e halt */
  uint64_t cr2;
  __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
  serial_puts("[exc] vector=0x");
  serial_hex32((uint32_t)f->int_no);
  serial_puts(" err=0x");
  serial_hex32((uint32_t)f->err_code);
  serial_puts(" rip=0x");
  serial_hex32((uint32_t)(f->eip >> 32));
  serial_hex32((uint32_t)f->eip);
  serial_puts(" rsp=0x");
  serial_hex32((uint32_t)(f->esp_dummy >> 32));
  serial_hex32((uint32_t)f->esp_dummy);
  serial_puts(" cr2=0x");
  serial_hex32((uint32_t)(cr2 >> 32));
  serial_hex32((uint32_t)cr2);
  serial_puts("\n");

  /* Falta dentro do proprio log (disco, FAT, pilha lida): a tela mostra a
   * excecao original, sem log. */
  if (crash_depth++ == 0) {
    crash_f = f;
    crash_cr2 = cr2;
    trace_freeze();
    sched_freeze();
    call_on_stack(crash_on_stack, crash_stack + sizeof(crash_stack));
  } else {
    serial_puts("[exc] falta durante o CRASH.LOG: log abandonado\n");
    f = crash_f;
  }

  /* Se estiver em modo grafico, volta a modo texto para panic ser visivel */
  vga_force_text_mode();

  /* Limpa tela VGA */
  {
    int i;
    for (i = 0; i < 80 * 25; i++)
      VGA[i] = (uint16_t)0x1F20;
  }

  int row = 0;
  int c;

  c = 0;
  vga_puts_at(row, &c, "!!! EXCEPTION !!!  vetor ");
  print_dec((uint32_t)f->int_no, row, &c);
  vga_puts_at(row, &c, " ");
  vga_puts_at(row, &c, vec_name(f->int_no));
  row++;

  c = 0;
  vga_puts_at(row, &c, "err_code: 0x");
  print_hex((uint32_t)f->err_code, row, &c);
  vga_puts_at(row, &c, "  CR2: 0x");
  print_hex((uint32_t)(crash_cr2 >> 32), row, &c);
  print_hex((uint32_t)crash_cr2, row, &c);
  row++;

  c = 0;
  vga_puts_at(row, &c, "RIP: 0x");
  print_hex((uint32_t)(f->eip >> 32), row, &c);
  print_hex((uint32_t)f->eip, row, &c);
  vga_puts_at(row, &c, "  ");
  vga_puts_at(row, &c, crash_rip);
  row++;

  c = 0;
  vga_puts_at(row, &c, "RSP: 0x");
  print_hex((uint32_t)(f->esp_dummy >> 32), row, &c);
  print_hex((uint32_t)f->esp_dummy, row, &c);
  vga_puts_at(row, &c, "  RBP: 0x");
  print_hex((uint32_t)(f->ebp >> 32), row, &c);
  print_hex((uint32_t)f->ebp, row, &c);
  row++;

  c = 0;
  vga_puts_at(row, &c, "CS: 0x");
  print_hex((uint32_t)f->cs, row, &c);
  vga_puts_at(row, &c, "  RFLAGS: 0x");
  print_hex((uint32_t)f->eflags, row, &c);
  vga_puts_at(row, &c, "  pid ");
  print_dec(mod_current_pid(), row, &c);
  row++;

  c = 0;
  if (crash_path) {
    vga_puts_at(row, &c, "Log gravado em ");
    vga_puts_at(row, &c, crash_path);
  } else {
    vga_puts_at(row, &c, crash_depth > 1 ? "Log NAO gravado (falta ao gravar)"
                                         : "Log NAO gravado (sem disco FAT montado)");
  }
  row++;

  row++;
  c = 0;
  vga_puts_at(row, &c, "System halted.");

  row++;
  row = dump_stack_words(f->esp_dummy, row);

  trace_dump_vga(row);
  fbcon_sync();

  for (;;) {
    __asm__ volatile ("hlt");
  }
}

void isr_df_panic(isr_frame_t* f) {
  (void)f;
  __asm__ volatile ("cli");
  serial_puts("[exc] DOUBLE FAULT (#DF)\n");
  vga_force_text_mode();
  {
    int i;
    for (i = 0; i < 80 * 25; i++)
      VGA[i] = (uint16_t)0x1F20;
  }
  int c = 0;
  int row = 0;
  vga_puts_at(row++, &c, "!!! DOUBLE FAULT (#DF) !!!");
  c = 0;
  vga_puts_at(row++, &c, "Panic stack in use. System halted.");
  fbcon_sync();
  for (;;) {
    __asm__ volatile ("hlt");
  }
}
