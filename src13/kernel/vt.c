/* vt.c - Telas virtuais (Ctrl+F1..F6/F8..F12 texto, Ctrl+F7 WIN). Ver vt.h. */
#include "kthread.h"
#include <stdint.h>
#include "vt.h"
#include "mm.h"
#include "module.h"
#include "sched.h"
#include "serial.h"
#include "kexports.h"
#include "fbcon.h"
#include "visual.h"
#include "drives.h"
#include "shell.h"
#include "env.h"
#include "irq.h"

#define VT_CELLS (80 * 25)
#define VT_STACK (64 * 1024)
#define VT_WIN_STACK (256 * 1024)
#define VT_OUT_SLICE 256

#define VS_FREE 0
#define VS_RUN  1   /* ao fundo, quer andar */
#define VS_KEY  2   /* ao fundo, esperando tecla: so anda na tela */
#define VS_GFX  3   /* o WIN fora da tela: parado */
#define VS_DEAD 4   /* o WIN fechou */

struct vt {
  int state;
  uint64_t rsp;
  uint8_t* stack;
  struct mod_cpu cpu;
  struct con_state con;
  void (*hook)(char c);
  void (*idle)(void);
  struct drives_state drv;
  uint16_t cells[VT_CELLS];
  uint8_t font[256][16];
  int blink;
  int cur_hidden;
  int back;          /* WIN: tela que volta quando ele fecha */
  uint32_t poll_t;   /* tick+1 em que cedeu sem tecla (k_trygetc): ver vt_idle */
};

void ctx_switch(uint64_t* save_rsp, uint64_t next_rsp);

static struct vt vts[VT_TASKS];
static int vt_on;
static int vt_cur;   /* tarefa rodando */
static int vt_fg;    /* tarefa na tela */
static int vt_rr;
static int vt_outn;
static volatile uint16_t* vt_hw;   /* a tela de verdade */
static uint8_t font0[256][16];     /* fonte do boot: a de cada tela nova */

/* ---- log do sistema (Ctrl+F12) ----
 * Anel em memoria: cada execucao (mod_exec) e, com SET PRODUCTION=1, a saida
 * dos programas do WIN sem janela propria. A tela Ctrl+F12 mostra o anel
 * desde o inicio (o que coube) e depois cada linha nova; quem estiver nela
 * (o visor do log, o DISKSYNC ou um prompt) recebe o texto no proprio cursor,
 * so quando ela e a tarefa rodando. */
#define VT_LOG 10                 /* Ctrl+F12 */
#define SLOG_SZ (64 * 1024)
static char slog[SLOG_SZ];
static volatile uint32_t slog_w;  /* bytes escritos desde o boot */
static uint32_t slog_r;           /* ate onde a tela Ctrl+F12 ja mostrou */
static volatile int slog_lk;
static int slog_bol = 1;          /* proximo byte comeca linha */
static int slog_in;               /* slog_flush rodando (putc pode ceder a vez) */
int rtc_time(void);

static void slog_add(char c) {
  slog[slog_w % SLOG_SZ] = c;
  slog_w++;
  slog_bol = (c == '\n');
}

void syslog_putc(char c) {
  if (c == '\r' || c == 0) return;
  while (__sync_lock_test_and_set(&slog_lk, 1)) { }
  slog_add(c);
  __sync_lock_release(&slog_lk);
}

void syslog_puts(const char* s) {
  if (!s) return;
  while (__sync_lock_test_and_set(&slog_lk, 1)) { }
  for (; *s; s++)
    if (*s != '\r') slog_add(*s);
  __sync_lock_release(&slog_lk);
}

void syslog_num(int v) {
  char b[12];
  int n = 0;
  unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
  do { b[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 11);
  if (v < 0) syslog_putc('-');
  while (n) syslog_putc(b[--n]);
}

/* "hh:mm:ss " no comeco de uma linha (fecha a linha anterior se precisar). */
void syslog_stamp(void) {
  int t = rtc_time();
  char b[10];
  if (!slog_bol) syslog_putc('\n');
  if (t < 0) t = 0;
  b[0] = (char)('0' + t / 100000);
  b[1] = (char)('0' + t / 10000 % 10);
  b[2] = ':';
  b[3] = (char)('0' + t / 1000 % 10);
  b[4] = (char)('0' + t / 100 % 10);
  b[5] = ':';
  b[6] = (char)('0' + t / 10 % 10);
  b[7] = (char)('0' + t % 10);
  b[8] = ' ';
  b[9] = 0;
  syslog_puts(b);
}

int syslog_production(void) {
  const char* v = env_get("PRODUCTION");
  char c;
  if (!v || !v[0]) return 0;
  c = (char)(v[0] | 0x20);
  return c == '1' || c == 'o' || c == 's' || c == 'y' || c == 't' || c == 'p';
}

/* Na tarefa da tela Ctrl+F12, com o console dela carregado. */
static void slog_flush(void) {
  struct con_state cs;
  if (!vt_on || vt_cur != VT_LOG || slog_in || kt_in_thread()) return;
  con_state_save(&cs);
  if (cs.out) return;   /* comando do prompt com > ou |: depois */
  slog_in = 1;
  while (slog_r != slog_w) {
    if (slog_w - slog_r > SLOG_SZ) slog_r = slog_w - SLOG_SZ;
    putc(slog[slog_r % SLOG_SZ]);
    slog_r++;
  }
  slog_in = 0;
}

static void vt_save(struct vt* v) {
  mod_cpu_save(&v->cpu);
  con_state_save(&v->con);
  v->hook = kexp_get_console_hook();
  v->idle = sched_get_idle();
  drives_state_save(&v->drv);
}

static void vt_load(int i) {
  struct vt* v = &vts[i];
  mod_cpu_load(&v->cpu);
  con_state_load(&v->con);
  kexp_set_console_hook(v->hook);
  sched_set_idle(v->idle);
  drives_state_load(&v->drv);
  vga_text = (i == vt_fg) ? vt_hw : v->cells;
}

/* Quem chama ja carregou nada: aqui sai o estado de vt_cur e entra o de j. */
static void vt_switch_to(int j) {
  int i = vt_cur;
  if (i == j || kt_in_thread()) return;   /* telas sao do mundo */
  vt_save(&vts[i]);
  vt_cur = j;
  vt_load(j);
  ctx_switch(&vts[i].rsp, vts[j].rsp);
  /* A CPU ja esta na pilha da tarefa que acordou. Se o WIN fechou, a pilha
   * dele (256 KB) pode ser liberada agora; ate aqui ela era a pilha corrente
   * ou ainda podia ser retomada pelo vt_idle/vt_yield. */
  if (vts[VT_WIN].state == VS_DEAD && vts[VT_WIN].stack) {
    kfree(vts[VT_WIN].stack);
    vts[VT_WIN].stack = 0;
    serial_puts("[vt] pilha do WIN liberada\n");
  }
  /* Tela de texto cujo programa de servico acabou (vt_svc_entry): livre de novo. */
  for (j = 0; j < VT_TEXT; j++) {
    if (j == vt_cur || vts[j].state != VS_DEAD) continue;
    if (vts[j].stack) kfree(vts[j].stack);
    vts[j].stack = 0;
    vts[j].state = VS_FREE;
  }
  if (vt_cur == VT_LOG) slog_flush();
}

static void font_save(uint8_t f[256][16]) {
  int c;
  for (c = 0; c < 256; c++) vga_font_get(c, f[c]);
}

/* So os caracteres que mudaram: no VGA cada um reprograma a plane 2. */
static void font_load(uint8_t f[256][16]) {
  uint8_t g[16];
  int c, k;
  for (c = 0; c < 256; c++) {
    vga_font_get(c, g);
    for (k = 0; k < 16 && g[k] == f[c][k]; k++) { }
    if (k < 16) vga_font_set(c, f[c]);
  }
}

/* Tela de texto saindo: guarda o que esta no hardware. */
static void screen_out(int i) {
  struct vt* v = &vts[i];
  int k;
  if (i == VT_WIN) return;
  for (k = 0; k < VT_CELLS; k++) v->cells[k] = vt_hw[k];
  font_save(v->font);
  v->blink = vga_text_blink_get();
  v->cur_hidden = kexp_cursor_hidden();
}

/* Tela de texto entrando: o modo texto ja esta no hardware. */
static void screen_in(int i) {
  struct vt* v = &vts[i];
  int k;
  if (i == VT_WIN) return;
  for (k = 0; k < VT_CELLS; k++) vt_hw[k] = v->cells[k];
  font_load(v->font);
  vga_text_blink(v->blink);
  kexp_cursor_apply(v->cur_hidden);
  if (fbcon_active()) fbcon_cursor(v->cur_hidden ? -1 : v->con.row * 80 + v->con.col);
  else if (!v->cur_hidden) con_cursor_at(v->con.row, v->con.col);
}

static void vt_arm(struct vt* v, uint32_t size, void (*fn)(void)) {
  uintptr_t top = ((uintptr_t)v->stack + size) & ~(uintptr_t)15;
  uint64_t* sp = (uint64_t*)top;
  /* Como task_arm do WIN: 6 registros, fn e alinhamento. */
  sp -= 8;
  sp[0] = sp[1] = sp[2] = sp[3] = sp[4] = sp[5] = 0;
  sp[6] = (uint64_t)(uintptr_t)fn;
  sp[7] = 0;
  v->rsp = (uint64_t)(uintptr_t)sp;
}

/* Estado de uma tarefa nova: prompt limpo, na unidade de quem a criou. */
static int vt_new(int i, uint32_t size, void (*fn)(void)) {
  struct vt* v = &vts[i];
  int k;
  if (v->stack) kfree(v->stack);
  v->stack = (uint8_t*)kmalloc(size);
  if (!v->stack) {
    v->state = VS_FREE;
    serial_puts("[vt] sem memoria para a pilha\n");
    return -1;
  }
  vt_arm(v, size, fn);
  mod_cpu_save(&v->cpu);
  v->cpu.ctx = 0;
  v->cpu.pid = 0;
  v->cpu.owner = 0;
  v->cpu.depth = 0;
  v->con.row = 0;
  v->con.col = 0;
  v->con.out = 0;
  v->con.exec = 0;
  v->con.pipe_on = 0;
  v->hook = 0;
  v->idle = 0;
  drives_state_save(&v->drv);
  for (k = 0; k < VT_CELLS; k++) v->cells[k] = 0x0720;
  for (k = 0; k < 256 * 16; k++) v->font[k / 16][k % 16] = font0[k / 16][k % 16];
  v->blink = 1;
  v->cur_hidden = 0;
  v->state = VS_RUN;
  if (i == VT_LOG) slog_r = slog_w > SLOG_SZ ? slog_w - SLOG_SZ : 0;   /* tudo o que coube */
  return 0;
}

static void vt_text_entry(void) {
  int n = vt_cur < 6 ? vt_cur + 1 : vt_cur + 2;   /* F7 e o WIN */
  puts_k("SO DOS - tela ");
  if (n >= 10) putc((char)('0' + n / 10));
  putc((char)('0' + n % 10));
  puts_k(" (Ctrl+F1..F12 trocam de tela, F7 = WIN)\n\n");
  shell_loop();
}

static void vt_win_entry(void) {
  struct vt* v = &vts[VT_WIN];
  int to;
  win_run();
  serial_puts("[vt] WIN fechou\n");
  v->state = VS_DEAD;
  to = v->back;
  if (to < 0 || to >= VT_TEXT || vts[to].state == VS_FREE) to = 0;
  vt_select(to);
  for (;;) __asm__ volatile ("hlt");
}

/* ---- servico DISKSYNC (SET DISKSYNC=1) ----
 * DISKSYNC.EXE roda na tela do log (Ctrl+F12), como um programa qualquer de
 * tela ao fundo: anda quando a tela visivel (o WIN ou um prompt) espera
 * tecla. O programa le DISKSYNC sozinho e sai com SET DISKSYNC=0; a tela
 * volta a ser so o log. Se o usuario abriu um prompt no Ctrl+F12, o servico
 * vai para a tela livre mais alta, como antes. */
#define VT_SVC VT_LOG
static int svc_vt = -1;
static char svc_exe[DRV_PATH];
static int log_idle;   /* o visor do log esta no laco dele (pode rodar o servico) */
static int log_svc;    /* pedido de servico para o visor do log */

static int svc_wanted(void) {
  const char* v = env_get("DISKSYNC");
  return v && v[0] && !(v[0] == '0' && v[1] == 0);
}

static int svc_find(char* out, int max) {
  static const char* const dirs[] = { "D:\\SODOS\\DISKSYNC.EXE", "Z:\\SODOS\\DISKSYNC.EXE",
                                      "U:\\SODOS\\DISKSYNC.EXE" };
  unsigned i;
  env_sodos_path("DISKSYNC.EXE", out, max);
  if (file_size(out) > 0) return 0;
  for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    int k = 0;
    while (dirs[i][k] && k < max - 1) {
      out[k] = dirs[i][k];
      k++;
    }
    out[k] = 0;
    if (file_size(out) > 0) return 0;
  }
  return mod_find_exe("DISKSYNC", out, max);
}

static void svc_run(void) {
  char line[DRV_PATH + 4];
  int i;
  for (i = 0; svc_exe[i] && i < (int)sizeof(line) - 1; i++) line[i] = svc_exe[i];
  line[i] = 0;
  shell_exec_quiet(line);
  serial_puts("[vt] DISKSYNC terminou\n");
  svc_vt = -1;
}

static void vt_svc_entry(void) {
  puts_k("SO DOS - servico DISKSYNC (SET DISKSYNC=0 para parar)\n\n");
  svc_run();
  vts[vt_cur].state = VS_DEAD;
  /* A pilha desta tarefa e liberada pela proxima troca (vt_switch_to). */
  if (vt_fg == vt_cur) vt_select(0);
  else vt_switch_to(vt_fg);
  for (;;) __asm__ volatile ("hlt");
}

/* Ctrl+F12: o log do sistema. Enter abre um prompt nesta tela. */
static void vt_log_entry(void) {
  int c;
  puts_k("SO DOS - log do sistema (Ctrl+F12). Enter abre um prompt aqui.\n");
  puts_k("SET PRODUCTION=1: programas do WIN sem janela, saida aqui.\n\n");
  slog_flush();
  for (;;) {
    if (log_svc) {
      log_svc = 0;
      log_idle = 0;
      puts_k("\n--- servico DISKSYNC (SET DISKSYNC=0 para parar) ---\n");
      svc_run();
      puts_k("\n--- DISKSYNC terminou ---\n");
      slog_flush();
    }
    log_idle = 1;
    if (vt_bg()) {
      vt_yield(1);
      continue;
    }
    c = shell_try_char();
    if (!c) {
      kt_bkl_breathe();
      cpu_idle(1);
      vt_idle();
      continue;
    }
    cpu_idle(0);
    if (c == '\n' || c == '\r') {
      log_idle = 0;
      puts_k("\n");
      shell_loop();
    }
  }
}

void vt_service_check(void) {
  int i;
  if (!vt_on || !svc_wanted()) return;
  if (svc_vt >= 0 && vts[svc_vt].state != VS_FREE && vts[svc_vt].state != VS_DEAD) return;
  if (svc_find(svc_exe, (int)sizeof(svc_exe)) != 0) {
    serial_puts("[vt] SET DISKSYNC=1, mas DISKSYNC.EXE nao foi achado\n");
    if (!win_screen_size(0, 0)) puts_k("DISKSYNC.EXE nao encontrado (PATHSODOS, D:\\SODOS, Z:\\SODOS)\n");
    return;
  }
  /* Na tela do log (Ctrl+F12): criada agora, ou o visor parado pega o pedido. */
  if (vts[VT_LOG].state == VS_FREE || vts[VT_LOG].state == VS_DEAD) {
    if (vts[VT_LOG].state == VS_DEAD) vts[VT_LOG].state = VS_FREE;
    if (vt_new(VT_LOG, VT_STACK, vt_log_entry) != 0) return;
    vts[VT_LOG].cpu.bkl = 1;   /* pode vir de um k_setenv (BKL pego pelo thunk) */
    log_idle = 1;
  }
  if (log_idle) {
    log_svc = 1;
    if (vts[VT_LOG].state == VS_KEY) vts[VT_LOG].state = VS_RUN;
    svc_vt = VT_LOG;
    serial_puts("[vt] DISKSYNC na tela do log: ");
    serial_puts(svc_exe);
    serial_puts("\n");
    return;
  }
  for (i = VT_SVC - 1; i > 0 && vts[i].state != VS_FREE; i--) { }
  if (i <= 0) {
    serial_puts("[vt] DISKSYNC: nenhuma tela livre\n");
    return;
  }
  if (vt_new(i, VT_STACK, vt_svc_entry) != 0) return;
  /* Pode vir de um k_setenv (BKL pego pelo thunk): a tarefa nova comeca como
   * codigo do kernel do mundo, com o BKL em profundidade 1. */
  vts[i].cpu.bkl = 1;
  svc_vt = i;
  serial_puts("[vt] DISKSYNC na tela ");
  serial_hex32((uint32_t)i);
  serial_puts(": ");
  serial_puts(svc_exe);
  serial_puts("\n");
}

void vt_init(void) {
  if (vt_on) return;
  vt_hw = vga_text;
  font_save(font0);
  vt_cur = 0;
  vt_fg = 0;
  vts[0].state = VS_RUN;
  vts[0].back = -1;
  vts[VT_WIN].back = -1;
  vt_on = 1;
  vt_service_check();   /* SET DISKSYNC=1 do AUTOEXEC (antes das telas existirem) */
}

int vt_bg(void) {
  return vt_on && vt_cur != vt_fg;
}

/* Ctrl+Fn: as doze combinacoes sao do SO e nenhum programa as recebe.
 * F7 continua sendo o WIN; as outras dez sao telas de texto numeradas 1..12
 * (a tela de texto 7 nao existe: esse numero e o WIN). */
int vt_hotkey(uint8_t sc) {
  if (!vt_on) return -1;
  if (sc >= 0x3B && sc <= 0x40) return sc - 0x3B;           /* F1..F6  -> 0..5 */
  if (sc == 0x41) return VT_WIN;                            /* F7      -> WIN */
  if (sc >= 0x42 && sc <= 0x44) return 6 + (sc - 0x42);     /* F8..F10 -> 6..8 */
  if (sc == 0x57) return 9;                                 /* F11     -> 9 */
  if (sc == 0x58) return 10;                                /* F12     -> 10 */
  return -1;
}

void vt_select(int i) {
  int old = vt_fg;
  if (!vt_on || i < 0 || i >= VT_TASKS) return;
  if (i == VT_WIN) {
    if (vts[i].state == VS_FREE || vts[i].state == VS_DEAD) {
      vts[i].back = vt_cur < VT_TEXT ? vt_cur : 0;
      if (vt_new(i, VT_WIN_STACK, vt_win_entry) != 0) return;
    }
  } else if (vts[i].state == VS_FREE) {
    if (vt_new(i, VT_STACK, i == VT_LOG ? vt_log_entry : vt_text_entry) != 0) return;
  }
  if (i == old) return;
  serial_puts("[vt] tela ");
  serial_hex32((uint32_t)i);
  serial_puts("\n");
  if (vts[old].state != VS_DEAD) {
    screen_out(old);
    vts[old].state = (old == VT_WIN) ? VS_GFX : VS_RUN;
  }
  vt_fg = i;
  screen_in(i);
  /* Quem chamou ja nao esta na tela: a copia passa a ser a dele. */
  vga_text = (vt_cur == i) ? vt_hw : vts[vt_cur].cells;
  vts[i].state = VS_RUN;
  vt_outn = 0;
  vt_switch_to(i);
}

void vt_yield(int wait) {
  if (!vt_bg()) return;
  vts[vt_cur].state = wait ? VS_KEY : VS_RUN;
  vts[vt_cur].poll_t = 0;
  vt_outn = 0;
  vt_switch_to(vt_fg);
}

/* Programa ao fundo perguntou por tecla e nao havia (laco de k_trygetc, como
 * o DISKSYNC): cede, e o vt_idle so volta a ele no proximo tick. Antes a tela
 * visivel trocava para ele a cada volta do seu laco ocioso (milhares de vezes
 * por segundo), e a CPU0 nunca parava. */
void vt_yield_poll(void) {
  if (!vt_bg()) return;
  vts[vt_cur].state = VS_RUN;
  vts[vt_cur].poll_t = (uint32_t)irq_ticks() + 1u;
  vt_outn = 0;
  vt_switch_to(vt_fg);
}

void vt_idle(void) {
  int k, j;
  if (!vt_on || vt_cur != vt_fg) return;
  if (vt_cur == VT_LOG) slog_flush();
  for (k = 1; k <= VT_TASKS; k++) {
    j = (vt_rr + k) % VT_TASKS;
    if (j == vt_fg || vts[j].state != VS_RUN) continue;
    if (vts[j].poll_t && vts[j].poll_t == (uint32_t)irq_ticks() + 1u) continue;
    vt_rr = j;
    vt_switch_to(j);
    return;
  }
}

void vt_out_tick(void) {
  if (!vt_bg() || sched_in_yield()) return;
  if (++vt_outn < VT_OUT_SLICE) return;
  vt_yield(0);
}

int vt_win(void) {
  vt_init();
  vt_select(VT_WIN);
  return vts[VT_WIN].state != VS_DEAD;
}
