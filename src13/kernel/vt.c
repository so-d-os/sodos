/* vt.c - Telas virtuais (Ctrl+F1..F6/F8..F12 texto, Ctrl+F7 WIN). Ver vt.h. */
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
  if (i == j) return;
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
    if (vt_new(i, VT_STACK, vt_text_entry) != 0) return;
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
  vt_outn = 0;
  vt_switch_to(vt_fg);
}

void vt_idle(void) {
  int k, j;
  if (!vt_on || vt_cur != vt_fg) return;
  for (k = 1; k <= VT_TASKS; k++) {
    j = (vt_rr + k) % VT_TASKS;
    if (j == vt_fg || vts[j].state != VS_RUN) continue;
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
