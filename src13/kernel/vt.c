/* vt.c - Telas virtuais (Ctrl+F1..F6/F8..F12 texto, Ctrl+F7 WIN). Ver vt.h. */
#include "kthread.h"
#include <stdint.h>
#include "vt.h"
#include "reboot.h"  /* kbd_raw: teclado cru da tela visivel */
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
#define VS_HELD 5   /* parada pelo Ctrl+Alt+Del (hang.c): presa ao fundo sem largar a
                     * CPU0; o vt_idle nao volta a ela, so o Ctrl+F dela */

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
  int kraw;          /* k_kbd_raw desta tela (vale quando ela esta visivel) */
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
/* Troca de tela pela metade (estado de vt_cur/vt_fg mudando): o Ctrl+Alt+Del
 * pelo IRQ0 (vt_rescue) espera o proximo tique. */
static volatile int vt_busy;
/* Aviso para a tela 1, escrito no console dela quando ela voltar a rodar
 * (Ctrl+Alt+Del, hang.c). */
static volatile int vt_note_on;
static char vt_note[320];

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
  vt_busy++;
  vt_save(&vts[i]);
  vt_cur = j;
  vt_load(j);
  vt_busy--;
  ctx_switch(&vts[i].rsp, vts[j].rsp);
  if (vt_note_on && vt_cur == 0) {
    vt_note_on = 0;
    puts_k(vt_note);
  }
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

/* ---- servico WSL (SET WSL=1) ----
 * WSL.EXE run --service na tela Ctrl+F10: a VM do Linux fica ligada ao
 * fundo, recebe comandos (WSL exec, BASH.EXE do Claude Code) pela segunda
 * serial e o console dela aparece no Ctrl+F10. SET WSL=0: o proprio WSL.EXE
 * desliga a VM e sai; a tela fica livre. */
#define VT_WSL 8                   /* Ctrl+F10 */
static char wsl_exe[DRV_PATH];

static int wsl_wanted(void) {
  const char* v = env_get("WSL");
  return v && v[0] == '1';
}

static void vt_wsl_entry(void) {
  char line[DRV_PATH + 24];
  int i, k;
  puts_k("SO DOS - WSL (Linux) ao fundo. SET WSL=0 desliga.\n\n");
  for (i = 0; wsl_exe[i] && i < DRV_PATH; i++) line[i] = wsl_exe[i];
  for (k = 0; " run --service"[k]; k++) line[i++] = " run --service"[k];
  line[i] = 0;
  shell_exec_quiet(line);
  serial_puts("[vt] WSL terminou\n");
  vts[vt_cur].state = VS_DEAD;
  if (vt_fg == vt_cur) vt_select(0);
  else vt_switch_to(vt_fg);
  for (;;) __asm__ volatile ("hlt");
}

void vt_wsl_check(void) {
  static const char* const dirs[] = { "D:\\WSL.EXE", "D:\\SODOS\\WSL.EXE", "Z:\\SODOS\\WSL.EXE" };
  unsigned d;
  if (!vt_on || !wsl_wanted()) return;
  if (vts[VT_WSL].state != VS_FREE && vts[VT_WSL].state != VS_DEAD) return;  /* ja roda (ou prompt) */
  env_sodos_path("WSL.EXE", wsl_exe, (int)sizeof(wsl_exe));
  for (d = 0; file_size(wsl_exe) <= 0 && d < sizeof(dirs) / sizeof(dirs[0]); d++) {
    int k = 0;
    while (dirs[d][k] && k < (int)sizeof(wsl_exe) - 1) {
      wsl_exe[k] = dirs[d][k];
      k++;
    }
    wsl_exe[k] = 0;
  }
  if (file_size(wsl_exe) <= 0 && mod_find_exe("WSL", wsl_exe, (int)sizeof(wsl_exe)) != 0) {
    serial_puts("[vt] SET WSL=1, mas WSL.EXE nao foi achado\n");
    return;
  }
  if (vts[VT_WSL].state == VS_DEAD) {
    if (vts[VT_WSL].stack) kfree(vts[VT_WSL].stack);
    vts[VT_WSL].stack = 0;
    vts[VT_WSL].state = VS_FREE;
  }
  if (vt_new(VT_WSL, VT_STACK, vt_wsl_entry) != 0) return;
  vts[VT_WSL].cpu.bkl = 1;   /* pode vir de um k_setenv (BKL pego pelo thunk) */
  serial_puts("[vt] WSL na tela Ctrl+F10: ");
  serial_puts(wsl_exe);
  serial_puts("\n");
}

/* ---- SET SYSTEMLOG=1: o log do Ctrl+F12 em \LOG\CONSOLE.LOG ----
 * Uma thread do kernel (fora da CPU0) grava o que entrou no anel desde a
 * ultima vez: ao ligar e a cada 5 minutos (e no REBOOT). O arquivo e
 * acrescentado (um cabecalho por boot) e guarda os ultimos 2 MB. D:, ou U:
 * sem D:. SET SYSTEMLOG=0 para. */
#define CLOG_MAX (2u * 1024u * 1024u)
#define CLOG_EVERY_MS (5u * 60u * 1000u)
uint32_t net_ms(void);
int rtc_date(void);
const char* sodos_version(void);
static uint32_t clog_saved;   /* slog_w ja gravado */
static int clog_boot_done;
static volatile int clog_word; /* 1 = gravar ja (SET SYSTEMLOG=1, REBOOT) */
static int clog_thread_on;

static int clog_wanted(void) {
  const char* v = env_get("SYSTEMLOG");
  return v && v[0] == '1';
}

static void cat_num(char* d, int* n, unsigned v, int w) {
  char b[12];
  int k = 0;
  do { b[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
  while (k < w) b[k++] = '0';
  while (k) d[(*n)++] = b[--k];
}

/* Com o BKL. 0 ok; -1 sem D:/U:, -2 sem memoria, -3 erro gravando. */
int syslog_save_file(void) {
  char path[24] = "D:\\LOG\\CONSOLE.LOG";
  char dir[8] = "D:\\LOG";
  uint32_t w = slog_w, from = clog_saved, lost = 0, i;
  char* buf;
  int n;
  if (!drives_is_mounted('D')) {
    if (!drives_is_mounted('U')) return -1;
    path[0] = dir[0] = 'U';
  }
  if (w == from && clog_boot_done) return 0;
  if (w - from > SLOG_SZ) {
    lost = w - from - SLOG_SZ;
    from = w - SLOG_SZ;
  }
  /* 2 MB: o kmalloc nao tem; arena de baixo (DMA do disco), dono 0 = kernel */
  buf = (char*)umalloc_owned(CLOG_MAX + SLOG_SZ + 512, 0);
  if (!buf) return -2;
  (void)drives_mkdir(dir);
  n = file_load(path, buf, (int)CLOG_MAX);
  if (n < 0) n = 0;
  if (!clog_boot_done) {
    const char* v = sodos_version();
    int t = rtc_time(), d = rtc_date();
    static const char h[] = "\n===== boot ";
    static const char h2[] = " - CONSOLE.LOG ligado em ";
    for (i = 0; h[i]; i++) buf[n++] = h[i];
    for (i = 0; v && v[i] && i < 80; i++) buf[n++] = v[i];
    for (i = 0; h2[i]; i++) buf[n++] = h2[i];
    if (d > 0) {
      cat_num(buf, &n, (unsigned)(d / 10000), 4); buf[n++] = '-';
      cat_num(buf, &n, (unsigned)(d / 100 % 100), 2); buf[n++] = '-';
      cat_num(buf, &n, (unsigned)(d % 100), 2); buf[n++] = ' ';
    }
    if (t >= 0) {
      cat_num(buf, &n, (unsigned)(t / 10000), 2); buf[n++] = ':';
      cat_num(buf, &n, (unsigned)(t / 100 % 100), 2); buf[n++] = ':';
      cat_num(buf, &n, (unsigned)(t % 100), 2); buf[n++] = ' ';
    }
    if (n && buf[n - 1] == ' ') n--;
    for (i = 0; " =====\n"[i]; i++) buf[n++] = " =====\n"[i];
    clog_boot_done = 1;
  }
  if (lost) {
    static const char m[] = "[log: bytes perdidos (o anel de 64 KB encheu): ";
    for (i = 0; m[i]; i++) buf[n++] = m[i];
    cat_num(buf, &n, lost, 1);
    buf[n++] = ']';
    buf[n++] = '\n';
  }
  for (i = from; i != w; i++) buf[n++] = slog[i % SLOG_SZ];
  if ((uint32_t)n > CLOG_MAX) { /* fica o fim, a partir de uma linha inteira */
    uint32_t cut = (uint32_t)n - CLOG_MAX, k;
    while (cut < (uint32_t)n && buf[cut - 1] != '\n') cut++;
    for (k = cut; k < (uint32_t)n; k++) buf[k - cut] = buf[k];
    n -= (int)cut;
  }
  i = file_save(path, buf, n) < 0;
  ufree(buf);
  if (i) return -3;
  clog_saved = w;
  return 0;
}

static int clog_thread(void* arg) {
  uint32_t last = 0;
  (void)arg;
  for (;;) {
    kt_futex_wait(&clog_word, 0, 10000);
    if (!clog_wanted()) {
      clog_word = 0;
      last = 0;
      continue;
    }
    if (clog_word || !last || net_ms() - last >= CLOG_EVERY_MS) {
      clog_word = 0;
      kt_bkl_enter_kernel();
      {
        int rc = syslog_save_file();
        if (rc == -1) serial_puts("[syslog] CONSOLE.LOG nao gravado: sem D: nem U:\n");
        else if (rc == -2) serial_puts("[syslog] CONSOLE.LOG nao gravado: sem memoria\n");
        else if (rc) serial_puts("[syslog] CONSOLE.LOG nao gravado: erro de disco\n");
      }
      kt_bkl_leave();
      last = net_ms() | 1u;
    }
  }
  return 0;
}

/* SET SYSTEMLOG mudou (ou boot): liga a thread e pede uma gravacao. */
void syslog_service_check(void) {
  if (!clog_wanted()) return;
  if (!clog_thread_on) {
    if (kt_thread_create(clog_thread, 0, 16384, ~1u) <= 0) {
      serial_puts("[syslog] thread do CONSOLE.LOG nao criada\n");
      return;
    }
    clog_thread_on = 1;
    serial_puts("[syslog] SET SYSTEMLOG=1: log do Ctrl+F12 em \\LOG\\CONSOLE.LOG a cada 5 min\n");
  }
  clog_word = 1;
  kt_futex_wake(&clog_word, 1);
}

/* REBOOT: grava ja, na tarefa de quem chamou (com o BKL). */
void syslog_save_now(void) {
  if (clog_wanted()) (void)syslog_save_file();
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
  vt_wsl_check();       /* SET WSL=1 do AUTOEXEC */
  syslog_service_check(); /* SET SYSTEMLOG=1 do AUTOEXEC */
}

int vt_bg(void) {
  return vt_on && vt_cur != vt_fg;
}

/* O WIN e a tela visivel (ou nao ha telas): a RIT pode desenhar a seta. */
int vt_win_fg(void) {
  return !vt_on || vt_fg == VT_WIN;
}

void vt_kbd_raw(int on) {
  if (!vt_on) {
    kbd_raw_set(on);
    return;
  }
  vts[vt_cur].kraw = on ? 1 : 0;
  if (vt_cur == vt_fg) kbd_raw_set(on);
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

/* Tudo de vt_select antes da troca. 1 = trocar para a tela i agora; 0 = ja
 * esta nela (ou faltou memoria para cria-la). */
static int vt_select_prep(int i) {
  int old = vt_fg;
  if (i == VT_WIN) {
    if (vts[i].state == VS_FREE || vts[i].state == VS_DEAD) {
      vts[i].back = vt_cur < VT_TEXT ? vt_cur : 0;
      if (vt_new(i, VT_WIN_STACK, vt_win_entry) != 0) return 0;
    }
  } else if (vts[i].state == VS_FREE) {
    if (vt_new(i, VT_STACK, i == VT_LOG ? vt_log_entry : vt_text_entry) != 0) return 0;
  }
  if (i == old) return 0;
  serial_puts("[vt] tela ");
  serial_hex32((uint32_t)i);
  serial_puts("\n");
  if (old == VT_WIN) {
    void rit_vt_leave(void);           /* video/visual.c: a seta da RIT sai */
    rit_vt_leave();
  }
  if (vts[old].state != VS_DEAD) {
    screen_out(old);
    vts[old].state = (old == VT_WIN) ? VS_GFX : VS_RUN;
  }
  vt_fg = i;
  /* teclado cru e da tela: o WSL no Ctrl+F10 nao leva o Ctrl+C das outras */
  if (kbd_raw() != vts[i].kraw) kbd_raw_set(vts[i].kraw);
  screen_in(i);
  /* Quem chamou ja nao esta na tela: a copia passa a ser a dele. */
  vga_text = (vt_cur == i) ? vt_hw : vts[vt_cur].cells;
  vts[i].state = VS_RUN;
  vt_outn = 0;
  return 1;
}

void vt_select(int i) {
  int go;
  if (!vt_on || i < 0 || i >= VT_TASKS) return;
  vt_busy++;
  go = vt_select_prep(i);
  vt_busy--;
  if (go) vt_switch_to(i);
}

/* ---- Ctrl+Alt+Del com o sistema parado (hang.c) ----
 * Chamado no IRQ0 da CPU0 (hang_tick), com IF=1 e o mundo interrompido onde
 * estava preso. 1 = fez alguma coisa (so volta quando a tarefa interrompida
 * for retomada), 0 = nada agora. */
int vt_fg_index(void) { return vt_on ? vt_fg : 0; }
int vt_cur_index(void) { return vt_on ? vt_cur : 0; }
int vt_state(int i) { return (vt_on && i >= 0 && i < VT_TASKS) ? vts[i].state : -1; }
int vt_held(int i) { return vt_state(i) == VS_HELD; }

void vt_note_set(const char* s) {
  int n = 0;
  while (s && s[n] && n < (int)sizeof(vt_note) - 1) {
    vt_note[n] = s[n];
    n++;
  }
  vt_note[n] = 0;
  vt_note_on = n > 0;
}

void win_rescue_vt(int to);   /* video/visual.c */

int vt_rescue(void) {
  int fg, cur;
  if (!vt_on || vt_busy || kt_in_thread()) return 0;
  fg = vt_fg;
  cur = vt_cur;
  if (fg == 0 && cur == 0) return 0;   /* a propria tela 1 esta presa */
  if (cur != fg) {
    /* Uma tela ao fundo nao larga a CPU0 (o programa dela num laco): fica
     * parada e a tela visivel volta; o Ctrl+F1 que o hang.c ja mandou leva
     * dela para a tela 1. Ctrl+F da tela parada a retoma. */
    serial_puts("[vt] Ctrl+Alt+Del: tela ");
    serial_hex32((uint32_t)cur);
    serial_puts(" presa ao fundo fica parada\n");
    vts[cur].state = VS_HELD;
    vt_outn = 0;
    vt_switch_to(fg);
    return 1;
  }
  serial_puts("[vt] Ctrl+Alt+Del: a tela visivel esta presa, indo para a tela 1\n");
  if (fg == VT_WIN) win_rescue_vt(0);
  else vt_select(0);
  return 1;
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
