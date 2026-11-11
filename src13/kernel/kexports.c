/* kexports.c - Funcoes do kernel que .EXE/.DLL importam como "KERNEL".
 * Cada linha EXP(nome) vira uma linha de c_compiler/lib/KERNEL.IMP (Makefile),
 * que o link usa para saber o que o kernel oferece. Convencao: cdecl. */
#include <stdint.h>
#include "shell.h"
#include "drives.h"
#include "mm.h"
#include "net.h"
#include "module.h"
#include "kexports.h"
#include "clip.h"
#include "visual.h"
#include "fbcon.h"
#include "sched.h"
#include "serial.h"
#include "bootinfo.h"
#include "httpd.h"
#include "vt.h"
#include "smp.h"

void mod_exit(int code);
void shell_read_line(char* buf, int max);

static void (*con_hook)(char c);

void kexp_set_console_hook(void (*hook)(char c)) { con_hook = hook; }
void (*kexp_get_console_hook(void))(char c) { return con_hook; }

/* ---------- console ---------- */
/* Cada chamada de console e um ponto de Ctrl+C (con_break_poll). */
static void con_out(int c) {
  if (con_hook) con_hook((char)c);
  else putc((char)c);
}

static void k_putc(int c) {
  con_break_poll();
  con_out(c);
}

static void k_write(const char* s, int n) {
  int i;
  con_break_poll();
  if (!s) return;
  for (i = 0; i < n; i++) con_out(s[i]);
}

/* Depuracao de programas: vai direto para a serial (COM1), sem passar pelo
 * console/VGA. Util para portar bibliotecas antes de existir stdout. */
static void k_dbg(const char* s, int n) {
  int i;
  if (!s) return;
  for (i = 0; i < n; i++) serial_putc(s[i]);
}

static void k_puts(const char* s) {
  con_break_poll();
  if (!s) return;
  while (*s) con_out(*s++);
}

/* Do lado direito de um pipe ("type x | prog") a entrada vem do pipe;
 * no fim dele k_getc devolve -1 e k_readline -1. */
static int k_getc(void) {
  int c;
  if (shell_pipe_active()) return shell_pipe_getc();
  do {
    c = shell_get_char();
  } while (c == 0);
  return c;
}

/* Como k_getc, mas nao espera: -1 se nao ha tecla (ou fim do pipe). */
static int k_trygetc(void) {
  int c;
  if (shell_pipe_active()) return shell_pipe_getc();
  c = shell_try_char();
  return c ? c : -1;
}

static int k_readline(char* buf, int max) {
  int n = 0;
  if (!buf || max < 2) return -1;
  if (shell_pipe_active()) {
    int c = -1;
    while (n < max - 1 && (c = shell_pipe_getc()) >= 0 && c != '\n')
      if (c != '\r') buf[n++] = (char)c;
    buf[n] = '\0';
    return (c < 0 && n == 0) ? -1 : n;
  }
  shell_read_line(buf, max);
  while (buf[n]) n++;
  return n;
}

static void k_clear(void) { clear_screen(); }

/* ---------- arquivos ---------- */
static int k_fsize(const char* path) { return file_size(path); }
static int k_fload(const char* path, char* buf, int max) { return file_load(path, buf, max); }
static int k_fread(const char* path, uint32_t offset, char* buf, int len) {
  return file_read_at(path, offset, buf, len);
}
static int k_fsave(const char* path, const char* data, int size) { return file_save(path, data, size); }
static int k_drive(void) { return drives_current(); }
static int k_set_drive(int letter) { return drives_set_current((char)letter); }
/* Unidade do .EXE em execucao (para achar D:\LOG, D:\omni.ja, ...). */
static int k_exe_drive(void) { return mod_exe_drive(); }

/* Nomes da unidade separados por '\n'. Devolve quantos. */
static int k_flist(int letter, char* out, int max) {
  char names[64][32];
  int n = drives_names((char)letter, names, 64);
  int pos = 0;
  int i;
  if (!out || max < 1) return n;
  for (i = 0; i < n; i++) {
    int k = 0;
    while (names[i][k] && pos < max - 2) out[pos++] = names[i][k++];
    if (pos < max - 1) out[pos++] = '\n';
  }
  out[pos] = '\0';
  return n;
}

static int k_dirlist(const char* path, char* out, int max) {
  return file_list(path, out, max);
}

static void k_pwd(char* out, int max) { drives_pwd(out, max); }

static int k_chdir(const char* path) {
  if (!path || !path[0]) return 0;
  return drives_chdir(path);
}

/* Itens de uma pasta com tamanho e data (struct dir_item = k_dirent). */
static int k_dir(const char* path, struct dir_item* out, int max) {
  if (!path || !out || max <= 0) return -1;
  return drives_items(path, out, max);
}

/* Apaga arquivo (aceita * e ?). 0 ou quantos; <0 erro (drives_del). */
static int k_fdelete(const char* path) { return path ? drives_del(path) : -1; }
static int k_mkdir(const char* path) { return path ? drives_mkdir(path) : -1; }
static int k_rmdir(const char* path) { return path ? drives_rmdir(path) : -1; }
static int k_dfree(int letter, uint32_t* total_kb, uint32_t* free_kb) {
  return drives_space((char)letter, total_kb, free_kb);
}

/* Mascara das unidades montadas: bit 0 = A:, bit 1 = B:, ... bit 25 = Z:.
 * Usada pelo Explorer para listar as unidades sem chutar letras. */
static uint32_t k_drive_mask(void) {
  uint32_t mask = 0;
  int i;
  for (i = 0; i < 26; i++)
    if (drives_is_mounted((char)('A' + i))) mask |= (uint32_t)1u << i;
  return mask;
}

/* ---------- tela de texto 80x25 ---------- */
/* Celulas caractere | cor << 8, como na VGA. No WIN, ou com a saida
 * desviada (pipe), nao ha tela de celulas: devolve -1. */
static int text_screen_ok(void) {
  return !con_hook && !shell_get_out();
}

static int k_vput(int row, int col, const uint16_t* cells, int n) {
  int pos, i;
  if (!text_screen_ok()) return -1;
  if (!cells || row < 0 || row >= 25 || col < 0 || col >= 80) return 0;
  pos = row * 80 + col;
  if (n > 80 * 25 - pos) n = 80 * 25 - pos;
  for (i = 0; i < n; i++) vga_text[pos + i] = cells[i];
  fbcon_sync();
  return 0;
}

/* Fonte da tela de texto. Quem mudou primeiro responde por ela: quando esse
 * programa termina, kexp_release_pid volta ao desenho padrao. */
static uint32_t font_pid;

static int k_font_get(int ch, uint8_t* glyph) { return vga_font_get(ch, glyph); }

static int k_font_set(int ch, const uint8_t* glyph) {
  int rc;
  if (!text_screen_ok() || vt_bg()) return -1;
  rc = vga_font_set(ch, glyph);
  if (rc == 0 && !font_pid) font_pid = mod_current_pid();
  return rc;
}

static void k_font_reset(void) {
  if (!text_screen_ok() || vt_bg()) return;
  vga_font_reset();
  font_pid = 0;
}

/* Fundo claro na tela de texto: dura ate o programa que pediu terminar. */
static uint32_t blink_pid;

static int k_blink(int on) {
  if (!text_screen_ok() || vt_bg()) return -1;
  vga_text_blink(on);
  blink_pid = on ? 0 : mod_current_pid();
  return 0;
}

/* Cursor em (row, col); row < 0 esconde. */
static int cur_hidden;
static uint8_t cur_start;

int kexp_cursor_hidden(void) { return cur_hidden; }

/* Tela entrando: o modo texto pode ter voltado com o cursor aceso. */
void kexp_cursor_apply(int hidden) {
  if (!fbcon_active() && vga_legacy()) {
    uint8_t v;
    shell_outb(0x3D4, 0x0A);
    v = shell_inb(0x3D5);
    if (!hidden) {
      shell_outb(0x3D5, (uint8_t)(v & (uint8_t)~0x20));
    } else {
      cur_start = (uint8_t)(v & (uint8_t)~0x20);
      shell_outb(0x3D5, (uint8_t)(v | 0x20));
    }
  }
  cur_hidden = hidden;
}

static void k_cursor(int row, int col) {
  if (!text_screen_ok()) return;
  /* Ao fundo, o cursor de verdade e de outra tela: so a posicao muda. */
  if (vt_bg()) {
    if (row >= 0 && row < 25 && col >= 0 && col < 80) goto_xy(row, col);
    return;
  }
  if (row < 0 || row >= 25 || col < 0 || col >= 80) {
    if (fbcon_active()) {
      fbcon_cursor(-1);
    } else if (vga_legacy() && !cur_hidden) {
      shell_outb(0x3D4, 0x0A);
      cur_start = shell_inb(0x3D5);
      shell_outb(0x3D5, (uint8_t)(cur_start | 0x20));
    }
    cur_hidden = 1;
    return;
  }
  if (cur_hidden && !fbcon_active() && vga_legacy()) {
    shell_outb(0x3D4, 0x0A);
    shell_outb(0x3D5, (uint8_t)(cur_start & (uint8_t)~0x20));
  }
  cur_hidden = 0;
  goto_xy(row, col);
}

/* ---------- memoria ---------- */
static void* k_malloc(uint32_t n) { return umalloc(n); }
static void k_free(void* p) { ufree(p); }
static void* k_realloc(void* p, uint32_t n) { return urealloc(p, n); }
static uint32_t k_msize(void* p) { return usize(p); }

static void k_meminfo(uint32_t* total, uint32_t* freeb) {
  um_stats(total, 0, freeb, 0);
}

/* ---------- processos e DLL ---------- */
static void k_exit(int code) { mod_exit(code); }
static int k_shell(char* line) {
  char buf[256];
  int i = 0;
  int rc;
  void (*prev)(char);
  if (!line) return 1;
  while (line[i] && i < (int)sizeof(buf) - 1) {
    buf[i] = line[i];
    i++;
  }
  buf[i] = 0;
  /* No WIN a saida do programa vai pela hook; o comando interno usa putc. */
  prev = shell_get_out();
  if (!prev && con_hook) shell_set_out(con_hook);
  rc = shell_exec_quiet(buf);
  shell_set_out(prev);
  if (rc < 0) return 1;
  return rc;
}

/* Como k_shell, mas a saida (comando interno ou .EXE) vai para out, com 0
 * no fim. *len recebe quantos bytes vieram; o que passar de max-1 e cortado
 * e *len fica negativo. Devolve o codigo do comando. */
static char* cap_buf;
static int cap_len, cap_max, cap_cut;

static void cap_out(char c) {
  if (c == '\f') return;
  if (cap_len < cap_max - 1) cap_buf[cap_len++] = c;
  else cap_cut = 1;
}

static int k_shell_capture(char* line, char* out, int max, int* len) {
  char* sbuf = cap_buf;
  int slen = cap_len, smax = cap_max, scut = cap_cut;
  void (*prev)(char) = shell_get_out();
  void (*hook)(char) = con_hook;
  int rc;
  if (len) *len = 0;
  if (!out || max < 1) return k_shell(line);
  cap_buf = out;
  cap_len = 0;
  cap_max = max;
  cap_cut = 0;
  con_hook = 0;
  shell_set_out(cap_out);
  rc = k_shell(line);
  shell_set_out(prev);
  con_hook = hook;
  out[cap_len] = 0;
  if (len) *len = cap_cut ? -cap_len : cap_len;
  cap_buf = sbuf;
  cap_len = slen;
  cap_max = smax;
  cap_cut = scut;
  return rc;
}

static int k_exec(const char* path, int argc, char** argv) {
  char full[DRV_PATH];
  char* wide[32];
  uint32_t* narrow = (uint32_t*)argv;
  int n, i;
  if (mod_find_exe(path, full, sizeof(full)) != 0) return MOD_ERR_NOTFOUND;
  /* O programa chamador guarda cada ponteiro em 4 bytes. */
  n = argc;
  if (n < 0) n = 0;
  if (n > 31) n = 31;
  for (i = 0; i < n; i++) {
    uint32_t p = narrow ? narrow[i] : 0;
    wide[i] = p ? (char*)(uintptr_t)p : "";
  }
  wide[n] = 0;
  return mod_exec(full, n, wide);
}
static const char* k_last_error(void) { return mod_last_error(); }
static int k_dll_load(const char* name) { return mod_dll_load(name); }
static void* k_dll_sym(int h, const char* name) { return mod_dll_sym(h, name); }
static void k_dll_free(int h) { mod_dll_free(h); }
/* Bibliotecas ELF (.so): dlopen/dlsym/dladdr da libc ficam em cima destas. */
static int k_dl_open(const char* name, int flags) { return mod_dl_open(name, flags); }
static void* k_dl_sym(int h, const char* name, const void* caller) { return mod_dl_sym(h, name, caller); }
static int k_dl_info(int h, struct k_dl_info* out) { return mod_dl_info(h, out); }
static int k_dl_iter(int index, struct k_dl_info* out) { return mod_dl_iter(index, out); }
static int k_dl_init_next(struct k_dl_info* out) { return mod_dl_init_next(out); }
static int k_dl_fini_next(struct k_dl_info* out) { return mod_dl_fini_next(out); }
static int k_dl_addr(const void* addr, struct k_dl_addr* out) { return mod_dl_addr(addr, out); }

/* ---------- tempo e aleatoriedade ---------- */
static uint32_t rdtsc_lo(uint32_t* hi) {
  uint32_t lo, h;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(h));
  if (hi) *hi = h;
  return lo;
}

static uint32_t k_rdtsc(uint32_t* hi) { return rdtsc_lo(hi); }
/* Lacos de espera costumam ler o relogio: tambem e ponto de Ctrl+C. */
static uint32_t k_ms(void) {
  con_break_poll();
  return net_ms();
}

static int cmos_in(int reg) {
  shell_outb(0x70, (uint8_t)reg);
  return shell_inb(0x71);
}

static int bcd_byte(int v) { return (v & 15) + ((v >> 4) & 15) * 10; }

/* AAAAMMDD. O seculo vem do registrador 0x32; sem ele, assume 2000. */
static int k_date(void) {
  int century, year, month, day, binary, guard = 0;
  while ((cmos_in(0x0A) & 0x80) && guard++ < 1000) {}
  binary = cmos_in(0x0B) & 0x04;
  day = cmos_in(0x07);
  month = cmos_in(0x08);
  year = cmos_in(0x09);
  century = cmos_in(0x32);
  if (!binary) {
    day = bcd_byte(day);
    month = bcd_byte(month);
    year = bcd_byte(year);
    century = bcd_byte(century);
  }
  if (century < 19 || century > 22) century = 20;
  if (month < 1 || month > 12 || day < 1 || day > 31) return 0;
  return (century * 100 + year) * 10000 + month * 100 + day;
}

/* HHMMSS do relogio (UTC, como o k_date); -1 se o RTC der lixo. */
static int k_time(void) {
  int h, m, s, binary, h24, pm, guard = 0;
  while ((cmos_in(0x0A) & 0x80) && guard++ < 1000) {}
  binary = cmos_in(0x0B) & 0x04;
  h24 = cmos_in(0x0B) & 0x02;
  s = cmos_in(0x00);
  m = cmos_in(0x02);
  h = cmos_in(0x04);
  pm = h & 0x80;
  h &= 0x7F;
  if (!binary) {
    s = bcd_byte(s);
    m = bcd_byte(m);
    h = bcd_byte(h);
  }
  if (!h24) {
    if (h == 12) h = 0;
    if (pm) h += 12;
  }
  if (h > 23 || m > 59 || s > 59) return -1;
  return h * 10000 + m * 100 + s;
}

/* Junta o TSC com a fase do PIT; o tempo entre teclas e pacotes varia de verdade. */
static uint32_t mix_state = 0x9E3779B9u;
static void k_random(uint8_t* out, int n) {
  int i;
  for (i = 0; i < n; i++) {
    uint32_t hi;
    uint32_t lo = rdtsc_lo(&hi);
    uint32_t pit;
    int k;
    shell_outb(0x43, 0x00);
    pit = shell_inb(0x40);
    pit |= (uint32_t)shell_inb(0x40) << 8;
    mix_state ^= lo + (hi << 7) + (pit << 16);
    for (k = 0; k < 4; k++) {
      mix_state ^= mix_state << 13;
      mix_state ^= mix_state >> 17;
      mix_state ^= mix_state << 5;
      mix_state += 0x6D2B79F5u;
    }
    out[i] = (uint8_t)(mix_state >> 24);
  }
}

/* ---------- escalonamento cooperativo ---------- */
/* Programas e DLLs (libsodos) usam para ceder o turno sem esperar tecla. */
static void k_yield(void) { sched_yield(); }

/* ---------- rede ---------- */
static int k_net_ready(void) { return net_ready(); }
static int k_dns(const char* host, uint32_t* ip) { return net_resolve(host, ip); }
static uint32_t tcp_pid;   /* programa dono da conexao da API */

static int k_tcp_connect(const char* host, int port) {
  int rc = net_tcp_connect(host, port);
  if (rc == 0) tcp_pid = mod_current_pid();
  return rc;
}
static int k_tcp_send(const void* d, int n) { return net_tcp_send(d, n); }
static int k_tcp_recv(void* d, int cap) {
  con_break_poll();
  return net_tcp_recv(d, cap);
}
static int k_net_pending(void) { return net_tcp_pending(); }
static void k_tcp_close(void) {
  net_tcp_close();
  tcp_pid = 0;
}

void kexp_release_pid(uint32_t pid) {
  if (!pid) return;
  /* Ao fundo, a fonte e o piscar na tela sao de outra tela: nao mexe. */
  if (font_pid == pid) {
    if (!vt_bg()) vga_font_reset();
    font_pid = 0;
  }
  if (blink_pid == pid) {
    if (!vt_bg()) vga_text_blink(1);
    blink_pid = 0;
  }
  if (tcp_pid != pid) return;
  net_tcp_close();
  tcp_pid = 0;
}

/* ---------- janela canvas do WIN (visual.c) ---------- */
static int k_gui_open(const char* title) { return gui_open(title); }
static void k_gui_close(void) { gui_close(); }
static void k_gui_title(const char* title) { gui_title(title); }
static void k_gui_size(int* w, int* h) { gui_size(w, h); }
static void k_gui_font(int* cw, int* ch) { gui_font(cw, ch); }
static void k_gui_fill(int x, int y, int w, int h, uint32_t rgb) { gui_fill(x, y, w, h, rgb); }
static void k_gui_text(int x, int y, const char* s, int n, uint32_t fg, uint32_t bg, int flags) {
  gui_text(x, y, s, n, fg, bg, flags);
}
static void k_gui_present(void) { gui_present(); }
static int k_gui_event(int* ev, int timeout_ms) { return gui_event(ev, timeout_ms); }
static int k_gui_raw(int on) { return gui_raw_set(on); }
static void k_gui_pixels(int x, int y, int w, int h, const void* rgba, int stride) {
  gui_pixels(x, y, w, h, rgba, stride);
}

/* 0 = retro, 1 = avancado; devolve o modo anterior (mode < 0 so consulta). */
static int k_gui_quality(int mode) {
  if (mode < 0) return gui_quality_get();
  return gui_quality_set(mode);
}

static int k_win_bg(uint32_t rgb) { return win_bg_set(rgb); }

static int k_win_key(int ch) { return win_api_key(ch); }

static int k_win_launch(const char* path) { return win_launch(path); }

/* area de transferencia do kernel (a mesma do Ctrl+C/Ctrl+V do WIN e do DOS) */
static void k_clip_set(const char* s, int n) { clip_set(s, n); }

static int k_clip_get(char* out, int max) { return clip_get(out, max); }

static int k_win_wallpaper(const void* rgba, int w, int h) {
  return win_wall_set((const uint8_t*)rgba, w, h);
}

/* interface: posicao do menu (0 esq, 1 centro, 2 dir), menu translucido, painel IA */
static int k_ui_menu_pos(int pos) {
  if (pos < 0) return ui_menu_pos_get();
  return ui_menu_pos_set(pos);
}
static int k_ui_glass(int on) {
  if (on < 0) return ui_glass_get();
  return ui_glass_set(on);
}
static int k_ui_ia(int on) {
  if (on < 0) return ui_ia_get();
  return ui_ia_set(on);
}
static int k_gui_dock(const char* title, int w) { return gui_dock_open(title, w); }

/* ---------- TOP: programas, servicos e mapa da memoria ----------
 * Os blocos de imagem, exports e strings de um modulo tem dono 0; os dados
 * (malloc e pilha) de um .EXE tem o pid dele. Tamanhos para o programa em KB. */
#define KP_EXE 1
#define KP_DLL 2
#define KP_SYS 3
#define KP_SVC 4

struct k_proc {
  char name[32];
  uint32_t pid;
  uint32_t kind;
  uint32_t state;
  uint32_t base;
  uint32_t image_kb;
  uint32_t data_kb;
  uint32_t refs;
  uint32_t pad;
};

#define KM_LIVRE 0
#define KM_KERNEL 1
#define KM_EXE 2
#define KM_DLL 3
#define KM_DADOS 4
#define KM_BUFFERS 5
#define KM_RESERV 6
#define KM_DONO 7
#define KM_CATS 8
#define KM_CELLS_MAX 2048
#define KM_HDR 16u

static struct mod_info top_mod[MOD_MAX];
static int top_nmod;

static void top_load_mods(void) {
  int i;
  top_nmod = 0;
  for (i = 0; i < MOD_MAX; i++)
    if (mod_get(i, &top_mod[top_nmod])) top_nmod++;
}

/* Modulo dono do bloco (pelo endereco util), ou -1. */
static int top_block_mod(uint64_t addr) {
  uint32_t p = (uint32_t)(addr + KM_HDR);
  int i, k;
  if (addr + KM_HDR > 0xFFFFFFFFull) return -1;
  for (i = 0; i < top_nmod; i++)
    for (k = 0; k < 3; k++)
      if (top_mod[i].blk[k] == p) return i;
  return -1;
}

static int top_exe_of(uint32_t pid) {
  int i;
  for (i = 0; i < top_nmod; i++)
    if (!top_mod[i].is_dll && top_mod[i].pid == pid) return i;
  return -1;
}

static uint32_t to_kb(uint64_t b) {
  b = (b + 1023u) >> 10;
  return b > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)b;
}

struct ps_acc {
  uint64_t image[MOD_MAX];
  uint64_t data[MOD_MAX];
  uint64_t klow, buffers, high;
};

static void ps_walk(void* arg, int area, uint64_t addr, uint64_t size, int used, uint32_t owner) {
  struct ps_acc* a = (struct ps_acc*)arg;
  int m;
  if (!used) return;
  if (area == 0) { a->klow += size; return; }
  if (area == 2) { a->high += size; return; }
  m = top_block_mod(addr);
  if (m >= 0) { a->image[m] += size; return; }
  m = owner ? top_exe_of(owner) : -1;
  if (m >= 0) a->data[m] += size;
  else a->buffers += size;
}

static void ps_name(struct k_proc* p, const char* s) {
  int i;
  for (i = 0; i < 31 && s[i]; i++) p->name[i] = s[i];
  for (; i < 32; i++) p->name[i] = 0;
}

static struct ps_acc ps_tmp;

/* Programas, DLLs, partes do kernel e servicos. Devolve quantos gravou. */
static int k_ps(struct k_proc* out, int max) {
  int i, n = 0;
  uint32_t cur = mod_current_pid();
  struct k_proc* p;
  if (!out || max <= 0) return 0;
  top_load_mods();
  for (i = 0; i < (int)sizeof(ps_tmp); i++) ((uint8_t*)&ps_tmp)[i] = 0;
  mm_walk(ps_walk, &ps_tmp);
  for (i = 0; i < top_nmod && n < max; i++) {
    const struct mod_info* m = &top_mod[i];
    p = &out[n++];
    ps_name(p, m->name);
    p->kind = m->is_dll ? KP_DLL : KP_EXE;
    p->pid = m->is_dll ? 0 : m->pid;
    p->state = m->is_dll ? 'S' : (m->pid == cur ? 'R' : 'E');
    p->base = m->base;
    p->image_kb = to_kb(ps_tmp.image[i]);
    p->data_kb = to_kb(ps_tmp.data[i]);
    p->refs = (uint32_t)m->refs;
    p->pad = 0;
  }
  if (n < max) {
    p = &out[n++];
    ps_name(p, "KERNEL");
    p->kind = KP_SYS;
    p->pid = 0;
    p->state = 'S';
    p->base = 0x01000000u;
    p->image_kb = to_kb(mm_kernel_end() - 0x01000000u);
    p->data_kb = to_kb(ps_tmp.klow);
    p->refs = 0;
    p->pad = 0;
  }
  if (n < max) {
    p = &out[n++];
    ps_name(p, "BUFFERS");
    p->kind = KP_SYS;
    p->pid = 0;
    p->state = 'S';
    p->base = um_start();
    p->image_kb = 0;
    p->data_kb = to_kb(ps_tmp.buffers);
    p->refs = 0;
    p->pad = 0;
  }
  if (hm_ready() && n < max) {
    p = &out[n++];
    ps_name(p, "MEM>4GB");
    p->kind = KP_SYS;
    p->pid = 0;
    p->state = 'S';
    p->base = 0;
    p->image_kb = 0;
    p->data_kb = to_kb(ps_tmp.high);
    p->refs = 0;
    p->pad = 0;
  }
  if (n < max) {
    p = &out[n++];
    ps_name(p, "REDE");
    p->kind = KP_SVC;
    p->pid = 0;
    p->state = net_ready() ? 'A' : 'P';
    p->base = 0;
    p->image_kb = p->data_kb = 0;
    p->refs = 0;
    p->pad = 0;
  }
  if (n < max) {
    p = &out[n++];
    ps_name(p, "HTTPD");
    p->kind = KP_SVC;
    p->pid = 0;
    p->state = httpd_port() ? 'A' : 'P';
    p->base = 0;
    p->image_kb = p->data_kb = 0;
    p->refs = (uint32_t)httpd_port();
    p->pad = 0;
  }
  return n;
}

/* Mapa: cada celula soma os bytes de cada categoria; a coluna KM_CATS guarda
 * quanto dela o E820 diz que e RAM. */
static uint64_t map_acc[KM_CELLS_MAX][KM_CATS + 1];
static uint64_t map_cell;
static int map_n;
static uint32_t map_pid;

static void map_add(uint64_t a, uint64_t len, int cat) {
  uint64_t e = a + len;
  uint64_t c;
  if (len == 0 || e < a) return;
  for (c = a / map_cell; c < (uint64_t)map_n && a < e; c++) {
    uint64_t ce = (c + 1) * map_cell;
    uint64_t k = (e < ce ? e : ce) - a;
    map_acc[c][cat] += k;
    a += k;
  }
}

static void map_walk(void* arg, int area, uint64_t addr, uint64_t size, int used, uint32_t owner) {
  int cat, m = -1;
  (void)arg;
  if (area == 0) cat = KM_KERNEL;
  else if (!used) cat = KM_LIVRE;
  else if (area == 2) cat = KM_BUFFERS;
  else if ((m = top_block_mod(addr)) >= 0) cat = top_mod[m].is_dll ? KM_DLL : KM_EXE;
  else cat = owner && top_exe_of(owner) >= 0 ? KM_DADOS : KM_BUFFERS;
  map_add(addr, size, cat);
  if (map_pid && used && area == 1 &&
      ((m >= 0 && !top_mod[m].is_dll && top_mod[m].pid == map_pid) || (m < 0 && owner == map_pid)))
    map_add(addr, size, KM_DONO);
}

/* kb[celula * KM_CATS + categoria]. *cell_kb = 0 escolhe a menor potencia de
 * 2 MB que cobre toda a RAM em 'cells'. pid != 0 tambem conta, em KM_DONO, o
 * que esse programa ocupa. Devolve quantas celulas usou. */
static int k_vram(struct k_vram* v) { return win_vram_info(v); }

/* CPUs (SMP): quantas o kernel acordou e o estado de cada uma (TOP, aba CPU). */
static int k_cpu_count(void) { return smp_cpu_count(); }
static int k_cpu_stat(int i, struct k_cpu_stat* out) { return smp_cpu_stat(i, out); }

static int k_memmap(uint32_t* kb, int cells, uint32_t* cell_kb, uint32_t pid) {
  const struct BootInfo* bi = bootinfo_get();
  const struct BootMemEntry* map = bootinfo_mem_map(bi);
  uint64_t span = 0;
  uint32_t i;
  int c, k;
  if (!kb || !cell_kb || cells <= 0) return 0;
  if (cells > KM_CELLS_MAX) cells = KM_CELLS_MAX;
  if (map && bi->mem_count) {
    for (i = 0; i < bi->mem_count; i++)
      if (map[i].type == BOOTINFO_MEM_USABLE && map[i].base + map[i].length > span)
        span = map[i].base + map[i].length;
  }
  if (span == 0) span = um_end();
  if (span == 0) return 0;
  if (*cell_kb) {
    map_cell = (uint64_t)*cell_kb << 10;
  } else {
    map_cell = 0x100000u;
    while ((span + map_cell - 1) / map_cell > (uint64_t)cells) map_cell <<= 1;
  }
  map_n = (int)((span + map_cell - 1) / map_cell);
  if (map_n > cells) map_n = cells;
  for (c = 0; c < map_n; c++)
    for (k = 0; k <= KM_CATS; k++) map_acc[c][k] = 0;
  if (map && bi->mem_count) {
    for (i = 0; i < bi->mem_count; i++)
      if (map[i].type == BOOTINFO_MEM_USABLE) map_add(map[i].base, map[i].length, KM_CATS);
  } else {
    map_add(0, span, KM_CATS);
  }
  top_load_mods();
  map_pid = pid;
  mm_walk(map_walk, 0);
  for (c = 0; c < map_n; c++) {
    uint64_t* a = map_acc[c];
    uint64_t size = map_cell, known = 0, ram = a[KM_CATS];
    if ((uint64_t)(c + 1) * map_cell > span) size = span - (uint64_t)c * map_cell;
    if (ram > size) ram = size;
    for (k = 0; k < KM_RESERV; k++) known += a[k];
    /* RAM fora das arenas: kernel, area baixa e o que o boot usa. */
    if (ram > known) a[KM_KERNEL] += ram - known;
    a[KM_RESERV] = size - ram;
    for (k = 0; k < KM_CATS; k++) kb[c * KM_CATS + k] = to_kb(a[k]);
  }
  *cell_kb = (uint32_t)(map_cell >> 10);
  return map_n;
}

struct kexp {
  const char* name;
  void* fn;
};

#define EXP(n) { #n, (void*)n }

static const struct kexp table[] = {
  EXP(k_putc),
  EXP(k_write),
  EXP(k_dbg),
  EXP(k_puts),
  EXP(k_getc),
  EXP(k_trygetc),
  EXP(k_readline),
  EXP(k_clear),
  EXP(k_yield),
  EXP(k_fsize),
  EXP(k_fload),
  EXP(k_fread),
  EXP(k_fsave),
  EXP(k_flist),
  EXP(k_dirlist),
  EXP(k_pwd),
  EXP(k_chdir),
  EXP(k_dir),
  EXP(k_fdelete),
  EXP(k_mkdir),
  EXP(k_rmdir),
  EXP(k_dfree),
  EXP(k_drive_mask),
  EXP(k_vput),
  EXP(k_cursor),
  EXP(k_font_get),
  EXP(k_font_set),
  EXP(k_font_reset),
  EXP(k_blink),
  EXP(k_drive),
  EXP(k_set_drive),
  EXP(k_exe_drive),
  EXP(k_malloc),
  EXP(k_free),
  EXP(k_realloc),
  EXP(k_msize),
  EXP(k_meminfo),
  EXP(k_ps),
  EXP(k_memmap),
  EXP(k_vram),
  EXP(k_cpu_count),
  EXP(k_cpu_stat),
  EXP(k_exit),
  EXP(k_exec),
  EXP(k_shell),
  EXP(k_shell_capture),
  EXP(k_last_error),
  EXP(k_dll_load),
  EXP(k_dll_sym),
  EXP(k_dll_free),
  EXP(k_dl_open),
  EXP(k_dl_sym),
  EXP(k_dl_info),
  EXP(k_dl_iter),
  EXP(k_dl_init_next),
  EXP(k_dl_fini_next),
  EXP(k_dl_addr),
  EXP(k_rdtsc),
  EXP(k_ms),
  EXP(k_date),
  EXP(k_time),
  EXP(k_random),
  EXP(k_net_ready),
  EXP(k_dns),
  EXP(k_tcp_connect),
  EXP(k_tcp_send),
  EXP(k_tcp_recv),
  EXP(k_net_pending),
  EXP(k_tcp_close),
  EXP(k_gui_open),
  EXP(k_gui_close),
  EXP(k_gui_title),
  EXP(k_gui_size),
  EXP(k_gui_font),
  EXP(k_gui_fill),
  EXP(k_gui_text),
  EXP(k_gui_present),
  EXP(k_gui_event),
  EXP(k_gui_raw),
  EXP(k_gui_pixels),
  EXP(k_gui_quality),
  EXP(k_win_bg),
  EXP(k_win_wallpaper),
  EXP(k_win_key),
  EXP(k_win_launch),
  EXP(k_clip_set),
  EXP(k_clip_get),
  EXP(k_ui_menu_pos),
  EXP(k_ui_glass),
  EXP(k_ui_ia),
  EXP(k_gui_dock),
};

int rtc_date(void) { return k_date(); }
int rtc_time(void) { return k_time(); }

void* kexport_find(const char* name) {
  unsigned i;
  for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    const char* a = table[i].name;
    const char* b = name;
    while (*a && *a == *b) {
      a++;
      b++;
    }
    if (*a == *b) return table[i].fn;
  }
  return 0;
}
