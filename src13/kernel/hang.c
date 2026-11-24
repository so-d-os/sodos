/* hang.c - Ctrl+Alt+Del: tela 1, HANG.LOG e reinicio (ver hang.h).
 *
 * Por que existe: 2026-11-24 o notebook travou com o Claude (WINE) rodando;
 * so o Ctrl+Alt+Del funcionou e ele reiniciava na hora, sem gravar nada. O
 * SERIAL.LOG so vai ao disco a cada programa (30 s) e pelo termico (5 min),
 * os dois com o BKL: com o sistema parado nao sobrou registro nenhum.
 *
 * Como funciona:
 * - kbd_watch (reboot.c) ve o Ctrl+Alt+Del em qualquer CPU e so marca o
 *   pedido; a thread daqui (fora da CPU0) faz o resto.
 * - "Parado" = o mundo (CPU0: shell, WIN, telas) sem ler o teclado ha
 *   HANG_STUCK_MS. Ai a thread manda NMI as outras CPUs algumas vezes: cada
 *   uma anota onde estava (RIP, registros, pilha) e volta. Com o resto do
 *   estado (BKL, threads, telas, trava do disco, final do log serial) isso
 *   vira o texto do HANG.LOG.
 * - Fora da tela 1: injeta Ctrl+F1 no teclado (o mundo vivo troca sozinho);
 *   se em 700 ms nao trocou, liga hang_force e o IRQ0 da CPU0 (com IF=1)
 *   interrompe o mundo onde ele estiver preso e troca de tela (vt_rescue). A
 *   tela 1 "foi" quando ela esta na frente e le o teclado por 500 ms seguidos.
 *   Foi: grava o HANG.LOG com as travas normais. Nao foi em 5 s: congela as
 *   outras CPUs no NMI, grava sem travas (fs_panic) e reinicia. As CPUs
 *   congeladas reiniciam sozinhas se a gravacao nao acabar em 15 s.
 * - Na tela 1: reinicia (com HANG.LOG antes se estava parada).
 * Nada aqui pega trava que outra CPU possa estar segurando antes de decidir:
 * quem travou pode ser justamente o dono dela. */
#include <stdint.h>
#include "hang.h"
#include "isr.h"
#include "serial.h"
#include "reboot.h"
#include "smp.h"
#include "kthread.h"
#include "module.h"
#include "trace.h"
#include "sched.h"
#include "vt.h"
#include "ps2in.h"
#include "drives.h"

#define HANG_STUCK_MS 1500u   /* o mundo sem ler o teclado: "parado" (como o guardiao) */
#define HANG_TRY_MS   5000u   /* prazo para a tela 1 aparecer e responder */
#define HANG_FORCE_MS 700u    /* sem a troca pelo teclado ate aqui: forca pelo IRQ0 */
#define HANG_ALIVE_MS 500u    /* tela 1 lendo o teclado por este tempo: foi */
#define HANG_HOLD_MS  15000u  /* CPUs congeladas reiniciam se a gravacao nao acabar */
#define HS_STK   48           /* palavras da pilha de cada CPU */
#define HS_CHAIN 12           /* retornos pela cadeia de RBP */
#define HS_SAMP  8            /* fotos por CPU */
#define HT_MAX   (448u * 1024u)
#define HT_SERIAL (256u * 1024u)  /* final do log serial no HANG.LOG */

/* ---- de outros arquivos ---- */
uint64_t kt_ms_robust(void);
void kt_bkl_steal(void);
uint32_t kt_online_mask(void);
int kt_hang_text(char* out, int max);
uint32_t ps2in_world_idle_ms(void);
int vt_fg_index(void);
int vt_cur_index(void);
int vt_state(int i);
int vt_rescue(void);
void vt_note_set(const char* s);
int isr_va_mapped(uint64_t va);
void fs_lock_info(int* owner, int* depth);
void version_reboot_note(const char* motivo);
const char* sodos_version(void);
int serial_log_save(void);
uint32_t serial_log_tail(char* out, uint32_t max, uint32_t* skipped);
const char* serial_log(uint32_t* n);
int rtc_date(void);
int rtc_time(void);
uint32_t net_ms(void);
void syslog_save_now(void);
void syslog_stamp(void);
void syslog_puts(const char* s);
int shell_try_char(void);
extern char __bss_start[];

volatile int hang_force;

static volatile int cad_req;    /* Ctrl+Alt+Del esperando a thread */
static volatile int cad_busy;   /* a thread esta cuidando de um */
static int hang_up;             /* a thread existe */
static volatile int hang_word;
static uint64_t tsc_per_ms = 2000000u;   /* medido na subida da thread */

static inline uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t irq_off(void) {
  uint64_t f;
  __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) : : "memory");
  return f;
}
static inline void irq_on_if(uint64_t f) {
  if (f & 0x200u) __asm__ volatile ("sti" ::: "memory");
}
/* Espera ms pelo TSC (com IF=0 e as outras CPUs no NMI nao ha tique). */
static void tsc_wait(uint32_t ms) {
  uint64_t t0 = rdtsc(), n = (uint64_t)ms * tsc_per_ms;
  while (rdtsc() - t0 < n) __asm__ volatile ("pause");
}

/* ---------- fotos das CPUs (NMI) ---------- */
struct hcpu {
  volatile uint32_t seq;      /* NMIs atendidos */
  volatile int held;          /* parada no NMI (gravacao em panico) */
  uint64_t rip, rsp, rbp, rfl, cs, cr3;
  uint64_t rax, rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11;
  int full;                   /* pilha e cadeia copiadas */
  int nstk, nchain, nsamp;    /* nstk < 0: RSP fora da memoria */
  uint64_t stk[HS_STK];
  uint64_t chain[HS_CHAIN];
  uint64_t samp[HS_SAMP];
  uint8_t samp_if[HS_SAMP];
};
static struct hcpu hc[KT_CPU_MAX];
static volatile int nmi_mode;          /* 0 nada, 1 foto, 2 foto e fica parada */
static volatile int nmi_full;          /* esta rodada copia a pilha */
static volatile uint64_t hold_until;   /* TSC: congelada reinicia depois disto */
static int shots;                      /* rodadas tiradas */
static int shots_me = -1;              /* CPU da thread que tirou (nao responde) */
/* Ja houve fotos: um NMI atrasado (CPU que estava com NMI bloqueado) chega
 * depois do nmi_mode voltar a 0 e nao pode virar tela de EXCEPTION. */
static volatile int nmi_ever;

static void shot_frame(struct hcpu* h, const isr_frame_t* f) {
  uint64_t cr3;
  __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
  h->rip = f->eip;
  h->rsp = f->esp_dummy;
  h->rbp = f->ebp;
  h->rfl = f->eflags;
  h->cs = f->cs;
  h->cr3 = cr3;
  h->rax = f->rax; h->rbx = f->rbx; h->rcx = f->rcx; h->rdx = f->rdx;
  h->rsi = f->rsi; h->rdi = f->rdi;
  h->r8 = f->r8; h->r9 = f->r9; h->r10 = f->r10; h->r11 = f->r11;
  if (h->nsamp < HS_SAMP) {
    h->samp[h->nsamp] = f->eip;
    h->samp_if[h->nsamp] = (uint8_t)((f->eflags >> 9) & 1u);
    h->nsamp++;
  }
  if (nmi_full && !h->full) {
    uint64_t sp = f->esp_dummy, bp = f->ebp;
    int k;
    h->full = 1;
    h->nstk = 0;
    if ((sp & 7u) || !isr_va_mapped(sp)) {
      h->nstk = -1;
    } else {
      for (k = 0; k < HS_STK; k++) {
        uint64_t a = sp + (uint64_t)k * 8u;
        if ((a & 0xFFFu) == 0 && !isr_va_mapped(a)) break;
        h->stk[k] = *(volatile uint64_t*)(uintptr_t)a;
        h->nstk++;
      }
    }
    h->nchain = 0;
    for (k = 0; k < HS_CHAIN; k++) {
      volatile uint64_t* fr;
      if ((bp & 7u) || !isr_va_mapped(bp) || !isr_va_mapped(bp + 15u)) break;
      fr = (volatile uint64_t*)(uintptr_t)bp;
      h->chain[h->nchain++] = fr[1];
      if (fr[0] <= bp) break;
      bp = fr[0];
    }
  }
}

/* isr.c, vetor 2. 1 = era nosso. */
int hang_nmi(isr_frame_t* f) {
  struct hcpu* h;
  int i, mode = nmi_mode;
  if (!mode) return nmi_ever;
  i = smp_cpu_self();
  if (i < 0 || i >= KT_CPU_MAX) return 1;
  h = &hc[i];
  shot_frame(h, f);
  __atomic_add_fetch(&h->seq, 1u, __ATOMIC_SEQ_CST);
  if (mode == 2) {
    h->held = 1;
    while (nmi_mode == 2) {
      uint64_t d = hold_until;
      if (d && rdtsc() > d) {
        serial_puts("[hang] gravacao do HANG.LOG nao terminou: reiniciando\n");
        sys_reboot();
      }
      __asm__ volatile ("pause");
    }
    h->held = 0;
  }
  return 1;
}

/* Uma rodada: NMI nas outras CPUs e espera cada uma responder (ate 50 ms).
 * mode 2 = elas ficam paradas no NMI. Devolve quantas responderam. */
static int nmi_round(int mode, int me) {
  uint32_t mask = kt_online_mask(), before[KT_CPU_MAX];
  uint64_t t0;
  int i, want = 0, got = 0;
  for (i = 0; i < KT_CPU_MAX; i++) before[i] = hc[i].seq;
  nmi_mode = mode;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  smp_nmi_others();
  t0 = rdtsc();
  for (;;) {
    want = got = 0;
    for (i = 0; i < KT_CPU_MAX; i++) {
      if (!(mask & (1u << i)) || i == me) continue;
      want++;
      if (hc[i].seq != before[i]) got++;
    }
    if (got >= want || rdtsc() - t0 > 50u * tsc_per_ms) break;
    __asm__ volatile ("pause");
  }
  return got;
}

/* Fotos de todas as CPUs: HS_SAMP rodadas, ~20 ms entre elas. A desta CPU
 * (a thread do hang) nao interessa: ela so esta aqui. */
static void take_shots(void) {
  uint64_t fl = irq_off();
  int me = smp_cpu_self(), r, i;
  nmi_ever = 1;
  for (i = 0; i < KT_CPU_MAX; i++) {
    uint32_t s = hc[i].seq;
    char* p = (char*)&hc[i];
    unsigned k;
    for (k = 0; k < sizeof(hc[i]); k++) p[k] = 0;
    hc[i].seq = s;
  }
  for (r = 0; r < HS_SAMP; r++) {
    nmi_full = (r == 0);
    (void)nmi_round(1, me);
    nmi_mode = 0;
    if (r + 1 < HS_SAMP) tsc_wait(20);
  }
  shots = HS_SAMP;
  shots_me = me;
  irq_on_if(fl);
}

/* ---------- texto ---------- */
static char ht[HT_MAX];
static int hn;
static void tc(char c) { if (hn < (int)HT_MAX - 1) ht[hn++] = c; }
static void ts(const char* s) { while (s && *s) tc(*s++); }
static void tu(uint64_t v) {
  char b[21];
  int n = 0;
  do b[n++] = (char)('0' + v % 10u); while ((v /= 10u) && n < 20);
  while (n) tc(b[--n]);
}
static void tx(uint64_t v, int digits) {
  static const char hx[] = "0123456789ABCDEF";
  while (digits-- > 0) tc(hx[(v >> (digits * 4)) & 0xFu]);
}
static void t2(int v) { tc((char)('0' + v / 10 % 10)); tc((char)('0' + v % 10)); }
static void tnl(void) { ts("\r\n"); }

/* "kernel+0x..." ou "MODULO.EXE+0x..."; 0 = nem kernel nem modulo. */
static int twhere(uint64_t a) {
  struct mod_info m;
  int i;
  if (a >= 0x01000000ull && a < (uint64_t)(uintptr_t)__bss_start) {
    ts("kernel+0x");
    tx(a - 0x01000000ull, 6);
    return 1;
  }
  if (a >= 0x100000000ull) return 0;
  for (i = 0; i < MOD_MAX; i++) {
    if (!mod_get(i, &m) || !m.base) continue;
    if (a >= m.base && a < (uint64_t)m.base + m.size) {
      ts(m.name);
      ts("+0x");
      tx(a - m.base, 8);
      return 1;
    }
  }
  return 0;
}

static const char* vt_name(int i) {
  static const char* const n[] = { "Ctrl+F1", "Ctrl+F2", "Ctrl+F3", "Ctrl+F4", "Ctrl+F5",
                                   "Ctrl+F6", "Ctrl+F8", "Ctrl+F9", "Ctrl+F10", "Ctrl+F11",
                                   "Ctrl+F12 (log)", "Ctrl+F7 (WIN)" };
  return (i >= 0 && i < VT_TASKS) ? n[i] : "?";
}
static const char* vt_st(int s) {
  switch (s) {
    case 0: return "livre";
    case 1: return "rodando";
    case 2: return "esperando tecla";
    case 3: return "WIN fora da tela";
    case 4: return "fechada";
    case 5: return "PARADA pelo Ctrl+Alt+Del";
    default: return "?";
  }
}

static void t_cpu(int i) {
  struct hcpu* h = &hc[i];
  int k, j;
  ts("cpu");
  tu((uint32_t)i);
  ts(": respondeu a ");
  tu((uint32_t)h->nsamp);
  ts(" de ");
  tu((uint32_t)shots);
  ts(" NMIs");
  if (i == shots_me) ts(" (e a CPU da thread do Ctrl+Alt+Del)");
  tnl();
  if (!h->nsamp) return;
  /* RIPs distintos com quantas vezes: o mesmo em todas = preso ali */
  ts("  onde estava:");
  for (k = 0; k < h->nsamp; k++) {
    int seen = 0, cnt = 0;
    for (j = 0; j < k; j++) if (h->samp[j] == h->samp[k]) seen = 1;
    if (seen) continue;
    for (j = 0; j < h->nsamp; j++) if (h->samp[j] == h->samp[k]) cnt++;
    ts("\r\n    ");
    tx(h->samp[k], 16);
    ts("  ");
    if (!twhere(h->samp[k])) ts("(fora do kernel e dos modulos)");
    ts(h->samp_if[k] ? "  IF=1" : "  IF=0 (interrupcoes desligadas)");
    ts("  x");
    tu((uint32_t)cnt);
  }
  tnl();
  ts("  ultima foto: RIP=");
  tx(h->rip, 16);
  ts(" RSP=");
  tx(h->rsp, 16);
  ts(" RBP=");
  tx(h->rbp, 16);
  ts("\r\n  RFLAGS=");
  tx(h->rfl, 8);
  ts(" CS=");
  tx(h->cs, 4);
  ts(" CR3=");
  tx(h->cr3, 8);
  ts("\r\n  RAX=");
  tx(h->rax, 16);
  ts(" RBX=");
  tx(h->rbx, 16);
  ts(" RCX=");
  tx(h->rcx, 16);
  ts("\r\n  RDX=");
  tx(h->rdx, 16);
  ts(" RSI=");
  tx(h->rsi, 16);
  ts(" RDI=");
  tx(h->rdi, 16);
  ts("\r\n  R8=");
  tx(h->r8, 16);
  ts(" R9=");
  tx(h->r9, 16);
  ts(" R10=");
  tx(h->r10, 16);
  ts(" R11=");
  tx(h->r11, 16);
  tnl();
  if (!h->full) return;
  if (h->nstk < 0) {
    ts("  pilha: RSP fora da memoria mapeada\r\n");
  } else {
    ts("  pilha da primeira foto (* = aponta para codigo):\r\n");
    for (k = 0; k < h->nstk; k++) {
      int mark;
      ts("    +");
      tx((uint64_t)k * 8u, 3);
      tc(' ');
      tx(h->stk[k], 16);
      mark = hn;
      ts("  * ");
      if (!twhere(h->stk[k])) hn = mark;
      tnl();
    }
  }
  if (h->nchain) {
    ts("  cadeia de RBP:");
    for (k = 0; k < h->nchain; k++) {
      ts("\r\n    ret ");
      tx(h->chain[k], 16);
      ts("  ");
      twhere(h->chain[k]);
    }
    tnl();
  }
}

static void build_text(const char* why, int shots_ok) {
  struct mod_info m;
  uint32_t mask = kt_online_mask(), idle = ps2in_world_idle_ms();
  int d = rtc_date(), t = rtc_time(), i, owner, depth, fg = vt_fg_index(), cur = vt_cur_index();
  uint32_t pid = mod_current_pid();
  hn = 0;
  ts("SODOS HANG.LOG\r\n");
  ts(sodos_version());
  ts("\r\ngravado em ");
  if (d > 0) {
    tu((uint32_t)(d / 10000));
    tc('-');
    t2(d / 100 % 100);
    tc('-');
    t2(d % 100);
  } else {
    ts("(sem data)");
  }
  tc(' ');
  if (t >= 0) {
    t2(t / 10000);
    tc(':');
    t2(t / 100 % 100);
    tc(':');
    t2(t % 100);
  }
  ts(" (relogio do PC), no ar ha ");
  tu(net_ms() / 1000u);
  ts(" s\r\nmotivo: ");
  ts(why);
  ts("\r\no mundo (CPU0: shell, WIN, telas) sem ler o teclado ha ");
  tu(idle);
  ts(" ms\r\n\r\ntelas: na frente ");
  ts(vt_name(fg));
  ts(", rodando ");
  ts(vt_name(cur));
  tnl();
  for (i = 0; i < VT_TASKS; i++) {
    int s = vt_state(i);
    if (s <= 0) continue;
    ts("  ");
    ts(vt_name(i));
    ts(": ");
    ts(vt_st(s));
    tnl();
  }
  ts("programa do mundo agora: pid ");
  tu(pid);
  for (i = 0; pid && i < MOD_MAX; i++) {
    if (!mod_get(i, &m) || m.is_dll || m.pid != pid) continue;
    tc(' ');
    ts(m.name);
    break;
  }
  fs_lock_info(&owner, &depth);
  ts("\r\ntrava do disco: ");
  if (!depth) {
    ts("livre");
  } else {
    ts("dono ");
    if (owner >= 0x100000) {
      ts("thread ");
      tu((uint32_t)(owner - 0x100000));
    } else if (owner > 0x1000) {
      ts("tarefa do WIN ");
      tu((uint32_t)(owner - 0x1001));
    } else {
      ts("o mundo (fora das tarefas do WIN)");
    }
    ts(", profundidade ");
    tu((uint32_t)depth);
  }
  ts("\r\n\r\n");
  hn += kt_hang_text(ht + hn, (int)HT_MAX - hn - 1);
  tnl();
  if (shots_ok) {
    ts("CPUs (NMI, fotos com ~20 ms entre elas):\r\n");
    for (i = 0; i < KT_CPU_MAX; i++) {
      if (!(mask & (1u << i))) continue;
      t_cpu(i);
    }
  } else {
    ts("CPUs: sem fotos (o sistema nao estava parado quando o Ctrl+Alt+Del chegou)\r\n");
  }
  ts("\r\nmodulos (base tamanho nome):\r\n");
  for (i = 0; i < MOD_MAX; i++) {
    if (!mod_get(i, &m)) continue;
    ts("  ");
    tx(m.base, 8);
    tc(' ');
    tx(m.size, 8);
    tc(' ');
    ts(m.is_dll ? "DLL " : "EXE ");
    ts(m.name);
    ts("  pid ");
    tu(m.pid);
    tnl();
    if (hn > (int)(HT_MAX - HT_SERIAL - 32768u)) {
      ts("  (lista cortada)\r\n");
      break;
    }
  }
  ts("\r\ntrace do kernel (mais recente por ultimo):\r\n");
  if (hn < (int)HT_MAX - 4096) hn += trace_dump_text(ht + hn, 4000);
  ts("\r\n---- final do log serial ----\r\n");
  {
    /* o log serial e a cabeca (primeiros 192 KB do boot) + um anel com o
     * final; no comeco do boot tudo ainda esta na cabeca */
    uint32_t room = (uint32_t)((int)HT_MAX - hn - 64), skipped = 0, n, hl, k, j;
    const char* head = serial_log(&hl);
    if (room > HT_SERIAL) room = HT_SERIAL;
    n = serial_log_tail(ht + hn, room, &skipped);
    if (n < room && hl > 0 && !skipped) {
      k = room - n;
      if (k > hl) k = hl;
      for (j = n; j > 0; j--) ht[hn + k + j - 1] = ht[hn + j - 1];
      for (j = 0; j < k; j++) ht[hn + j] = head[hl - k + j];
      n += k;
    }
    hn += (int)n;
  }
  ts("\r\n---- fim ----\r\n");
  ht[hn] = 0;
}

/* Grava ht em X:\LOG\HANG.LOG (D:, senao U:). panic: sem travas. */
static char saved_at[24];
static int write_log(int panic) {
  static const char order[] = { 'D', 'U' };
  static char path[] = "?:\\LOG\\HANG.LOG";
  static char dir[] = "?:\\LOG";
  unsigned i;
  saved_at[0] = 0;
  for (i = 0; i < sizeof order; i++) {
    /* so disco FAT ja montado (U: sem pendrive faria o xHCI procurar) */
    if (drives_kind_name(order[i])[0] != 'F') continue;
    path[0] = dir[0] = order[i];
    if (file_save(path, ht, hn) < 0) {
      (void)drives_mkdir(dir);
      if (file_save(path, ht, hn) < 0) continue;
    }
    for (i = 0; path[i] && i < sizeof(saved_at) - 1; i++) saved_at[i] = path[i];
    saved_at[i] = 0;
    return 1;
  }
  (void)panic;
  return 0;
}

/* Nao deu: congela as outras CPUs, grava sem travas e reinicia. */
static void panic_save_reboot(const char* why, const char* boot_why, int have_shots) {
  uint64_t fl;
  int me, i, want = 0, got = 0;
  uint32_t mask = kt_online_mask();
  serial_puts("[hang] ");
  serial_puts(why);
  serial_puts(": gravando o HANG.LOG e reiniciando\n");
  build_text(why, have_shots);
  fl = irq_off();
  (void)fl;
  me = smp_cpu_self();
  nmi_ever = 1;
  hold_until = rdtsc() + (uint64_t)HANG_HOLD_MS * tsc_per_ms;
  (void)nmi_round(2, me);
  /* espera elas pararem de verdade (held), ate 100 ms */
  {
    uint64_t t0 = rdtsc();
    for (;;) {
      want = got = 0;
      for (i = 0; i < KT_CPU_MAX; i++) {
        if (!(mask & (1u << i)) || i == me) continue;
        want++;
        if (hc[i].held) got++;
      }
      if (got >= want || rdtsc() - t0 > 100u * tsc_per_ms) break;
      __asm__ volatile ("pause");
    }
  }
  fs_panic = 1;       /* o dono da trava do disco pode ser quem travou */
  sched_freeze();     /* o FAT nao cede a vez no meio da gravacao */
  kt_bkl_steal();     /* o dono do BKL esta parado no NMI (ou e quem travou) */
  if (write_log(1)) {
    serial_puts("[hang] HANG.LOG gravado em ");
    serial_puts(saved_at);
    serial_puts("\n");
  } else {
    serial_puts("[hang] HANG.LOG nao gravado (sem disco FAT)\n");
  }
  version_reboot_note(boot_why);
  (void)serial_log_save();
  sys_reboot();
}

/* ---------- tela 1 ---------- */
static int goto_vt1(void) {
  static const uint8_t ctrl_f1[4] = { 0x1D, 0x3B, 0xBB, 0x9D };
  static const uint8_t f1[2] = { 0x3B, 0xBB };
  uint64_t t0 = kt_ms_robust(), alive = 0, inj_t = t0;
  int armed = 0, again = 0;
  /* Ctrl ainda apertado (o do Ctrl+Alt+Del): so o F1 */
  if (kbd_ctrl_down()) (void)ps2in_type(f1, 2);
  else (void)ps2in_type(ctrl_f1, 4);
  for (;;) {
    uint64_t now;
    (void)kt_futex_wait(&hang_word, hang_word, 20);
    now = kt_ms_robust();
    /* O mundo voltou a ler o teclado (uma tela ao fundo foi parada) e as
     * teclas ja foram lidas, mas a tela 1 nao veio: Ctrl+F1 de novo. */
    if (armed && again < 3 && vt_fg_index() != 0 && ps2in_world_idle_ms() < 300u &&
        ps2in_type_left() == 0 && now - inj_t >= 600u) {
      again++;
      inj_t = now;
      serial_puts("[hang] o mundo voltou e a tela 1 nao veio: Ctrl+F1 de novo\n");
      (void)ps2in_type(ctrl_f1, 4);
    }
    if (vt_fg_index() == 0 && ps2in_world_idle_ms() < 300u) {
      if (!alive) alive = now;
      else if (now - alive >= HANG_ALIVE_MS) {
        hang_force = 0;
        return 1;
      }
    } else {
      alive = 0;
    }
    if (!armed && now - t0 >= HANG_FORCE_MS) {
      armed = 1;
      hang_force = 1;
      serial_puts("[hang] a tela 1 nao veio pelo teclado: forcando pelo IRQ0 da CPU0\n");
    }
    if (now - t0 >= HANG_TRY_MS) {
      hang_force = 0;
      serial_puts("[hang] tela na frente ");
      serial_hex32((uint32_t)vt_fg_index());
      serial_puts(", mundo sem ler o teclado ha ");
      serial_hex32(ps2in_world_idle_ms());
      serial_puts(" ms, teclas injetadas nao lidas ");
      serial_hex32((uint32_t)ps2in_type_left());
      serial_puts("\n");
      return 0;
    }
  }
}

static void handle(void) {
  uint32_t idle = ps2in_world_idle_ms();
  int stuck = idle >= HANG_STUCK_MS;
  int on1 = vt_fg_index() == 0;
  serial_puts(stuck ? "[hang] Ctrl+Alt+Del com o mundo parado\n" : "[hang] Ctrl+Alt+Del\n");
  if (stuck) take_shots();
  if (on1) {
    if (stuck) panic_save_reboot("Ctrl+Alt+Del na tela 1 com o sistema parado",
                                 "pelo Ctrl+Alt+Del na tela 1 com o sistema parado (HANG.LOG)", 1);
    /* tela 1 respondendo: reinicio normal, registrado como o REBOOT */
    kt_bkl_enter_kernel();
    version_reboot_note("pelo Ctrl+Alt+Del na tela 1");
    syslog_save_now();
    (void)serial_log_save();
    sys_reboot();
  }
  if (stuck) {
    /* o texto agora, antes da troca mexer no estado */
    build_text("Ctrl+Alt+Del com o sistema parado", 1);
    vt_note_set("\r\n*** Ctrl+Alt+Del: o sistema estava parado. Diagnostico em \\LOG\\HANG.LOG.\r\n"
                "*** Tela presa fica parada (Ctrl+F dela retoma). Ctrl+Alt+Del aqui reinicia.\r\n");
  } else {
    vt_note_set("\r\n*** Ctrl+Alt+Del: tela 1. Ctrl+Alt+Del de novo aqui reinicia o PC.\r\n");
  }
  if (goto_vt1()) {
    serial_puts("[hang] Ctrl+Alt+Del: a tela 1 esta respondendo\n");
    if (stuck) {
      kt_bkl_enter_kernel();
      if (write_log(0)) {
        serial_puts("[hang] HANG.LOG gravado em ");
        serial_puts(saved_at);
        serial_puts("\n");
      }
      (void)serial_log_save();
      kt_bkl_leave();
    }
    return;
  }
  panic_save_reboot(stuck ? "Ctrl+Alt+Del com o sistema parado; a tela 1 nao respondeu"
                          : "Ctrl+Alt+Del; a tela 1 nao respondeu em 5 s",
                    "pelo Ctrl+Alt+Del: a tela 1 nao respondeu (HANG.LOG)", stuck);
}

static int hang_main(void* arg) {
  (void)arg;
  /* TSC por ms (para esperar com IF=0 e as outras CPUs paradas) */
  {
    uint64_t m0 = kt_ms_robust(), c0 = rdtsc(), m1, c1;
    (void)kt_futex_wait(&hang_word, hang_word, 200);
    m1 = kt_ms_robust();
    c1 = rdtsc();
    if (m1 > m0 && c1 > c0) tsc_per_ms = (c1 - c0) / (m1 - m0);
    if (tsc_per_ms < 100000u) tsc_per_ms = 2000000u;
  }
  for (;;) {
    (void)kt_futex_wait(&hang_word, hang_word, 50);
    if (!cad_req) continue;
    cad_busy = 1;
    cad_req = 0;
    handle();
    cad_busy = 0;
  }
  return 0;
}

void hang_start(void) {
  int tid;
  if (hang_up || kt_online_count() < 2) return;
  tid = kt_thread_create(hang_main, 0, 65536, ~1u);   /* longe da CPU0 (o mundo) */
  hang_up = tid > 0;
  serial_puts(hang_up ? "[hang] Ctrl+Alt+Del: tela 1 / HANG.LOG ligado\n"
                      : "[hang] sem thread: Ctrl+Alt+Del reinicia na hora\n");
}

void hang_ctrl_alt_del(void) {
  serial_puts("[reboot] Ctrl+Alt+Del\n");
  if (!hang_up) sys_reboot();
  if (cad_req || cad_busy) {
    serial_puts("[hang] Ctrl+Alt+Del de novo: reinicia ja\n");
    sys_reboot();
  }
  cad_req = 1;
}

/* ---------- IRQ0 da CPU0 ---------- */
static uint8_t xs_area[VT_TASKS][4096] __attribute__((aligned(64)));
static int xs_mode = -1;
static uint32_t xs_lo, xs_hi;
static void xs_probe(void) {
  uint64_t cr4;
  uint32_t a, b, c, d;
  xs_mode = 0;
  __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
  if (!(cr4 & (1u << 18))) return;
  __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0xD), "c"(0));
  if (b < 576 || b > 4096) return;
  __asm__ volatile ("xgetbv" : "=a"(xs_lo), "=d"(xs_hi) : "c"(0));
  xs_mode = 1;
}

void hang_tick(uint64_t rip) {
  static uint64_t last;
  uint64_t now;
  uint8_t* xs;
  int k, i;
  (void)rip;
  if (!hang_force) return;
  now = kt_ms_robust();
  if (now - last < 300u) return;
  /* o mundo esta lendo o teclado: deixa ele trocar sozinho */
  if (ps2in_world_idle_ms() < 800u) return;
  last = now;
  k = vt_cur_index();
  if (k < 0 || k >= VT_TASKS) k = 0;
  xs = xs_area[k];
  if (xs_mode < 0) xs_probe();
  /* SSE do programa interrompido: a troca de tela nao salva (sched.h) */
  if (xs_mode) {
    for (i = 512; i < 576; i++) xs[i] = 0;
    __asm__ volatile ("xsave %0" : "=m"(*(uint8_t(*)[4096])xs) : "a"(xs_lo), "d"(xs_hi) : "memory");
  } else {
    __asm__ volatile ("fxsave %0" : "=m"(*(uint8_t(*)[512])xs) : : "memory");
  }
  __asm__ volatile ("sti" ::: "memory");
  (void)vt_rescue();
  __asm__ volatile ("cli" ::: "memory");
  if (xs_mode) {
    __asm__ volatile ("xrstor %0" : : "m"(*(uint8_t(*)[4096])xs), "a"(xs_lo), "d"(xs_hi) : "memory");
  } else {
    __asm__ volatile ("fxrstor %0" : : "m"(*(uint8_t(*)[512])xs) : "memory");
  }
}

/* ---------- SMP TRAVAR (teste) ---------- */
static int bkl_hog(void* arg) {
  (void)arg;
  kt_bkl_enter_kernel();
  serial_puts("[hang] teste: thread com o BKL num laco\n");
  for (;;) __asm__ volatile ("pause");
  return 0;
}

int hang_test(const char* mode, int delay_s) {
  uint64_t t0 = kt_ms_robust();
  char c = mode ? mode[0] : 0;
  if (c != 'l' && c != 'c' && c != 'b') return -1;
  /* espera cedendo a vez e lendo o teclado (o get_key atende o Ctrl+Fn): da
   * tempo de ir para outra tela, e a trava vem com esta tela ao fundo */
  while (delay_s > 0 && kt_ms_robust() - t0 < (uint64_t)delay_s * 1000u) {
    if (vt_bg()) {
      vt_yield_poll();
    } else {
      (void)shell_try_char();
      kt_bkl_breathe();
      kt_world_idle(10);
      vt_idle();
    }
  }
  if (c == 'b') {
    serial_puts("[hang] teste: uma thread vai pegar o BKL e nao soltar\n");
    return kt_thread_create(bkl_hog, 0, 16384, ~1u) > 0 ? 0 : -2;
  }
  if (c == 'c') {
    serial_puts("[hang] teste: o mundo num laco com as interrupcoes desligadas\n");
    __asm__ volatile ("cli");
  } else {
    serial_puts("[hang] teste: o mundo num laco (IF=1, com o BKL)\n");
  }
  for (;;) __asm__ volatile ("pause");
  return 0;
}
