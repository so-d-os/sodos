/* kexports.c - Funcoes do kernel que .EXE/.DLL importam como "KERNEL".
 * Cada linha EXP(nome) vira uma linha de c_compiler/lib/KERNEL.IMP (Makefile),
 * que o link usa para saber o que o kernel oferece. Convencao: cdecl. */
#include "thermal.h"
#include <stdint.h>
#include "shell.h"
#include "drives.h"
#include "mm.h"
#include "net.h"
#include "module.h"
#include "kexports.h"
#include "env.h"
#include "isr.h"
#include "clip.h"
#include "visual.h"
#include "fbcon.h"
#include "sched.h"
#include "serial.h"
#include "bootinfo.h"
#include "httpd.h"
#include "vt.h"
#include "smp.h"
#include "kthread.h"
#include "linkext.h"
#include "nvme.h"
#include "reboot.h"
#include "wine.h"
#include "vmem.h"
#include "irq.h"

void mod_exit(int code);
void shell_read_line(char* buf, int max);

static void (*con_hook)(char c);

void kexp_set_console_hook(void (*hook)(char c)) { con_hook = hook; }
void (*kexp_get_console_hook(void))(char c) { return con_hook; }

/* Tela de texto de uma janela do Command (visual.c). Sem pipe: com a saida ou
 * a entrada desviada o programa ve o pipe, como no DOS. */
static const struct kexp_textscr* tscr;
void kexp_set_textscr(const struct kexp_textscr* ops) { tscr = ops; }
static const struct kexp_textscr* tscr_on(void) {
  return (tscr && con_hook && !shell_get_out()) ? tscr : 0;
}
static const struct kexp_textscr* tkey_on(void) {
  return (tscr && con_hook && !shell_pipe_active()) ? tscr : 0;
}

/* ---------- console ---------- */
/* O con_hook e global: e a tela do programa que o WIN esta rodando no mundo
 * (console_attach em visual.c). Uma thread de outro programa (k_thread numa
 * AP, ex.: o console.log do Firefox) escreveria na janela desse programa,
 * por cima do TOP num Command. A saida dela vai so para a serial (e o
 * \LOG\SERIAL.LOG). */
static int con_alheio(void) {
  return kt_in_thread() && kt_cur_pid() != mod_current_pid();
}

/* Captura (k_exec_capture/k_shell_capture) presa a uma tarefa do WIN: o WIN
 * troca o con_hook a cada volta da tarefa (console_attach), entao um
 * programa preemptado no meio da captura voltava escrevendo na janela e a
 * saida sumia (git --version do Claude Desktop: 25 bytes numa vez, 0 na
 * outra). -2 = nenhuma captura. */
int win_task_current(void);
static void cap_out(char c);
static char* cap_buf;
static int cap_task = -2;
/* Cada chamada de console e um ponto de Ctrl+C (con_break_poll). */
static void con_out(int c) {
  if (cap_task != -2 && cap_buf && win_task_current() == cap_task) {
    cap_out((char)c);
    return;
  }
  if (con_hook) con_hook((char)c);
  else putc((char)c);
}

static void k_putc(int c) {
  if (con_alheio()) {
    serial_putc((char)c);
    return;
  }
  con_break_poll();
  con_out(c);
}

static void k_write(const char* s, int n) {
  int i;
  if (s && con_alheio()) {
    for (i = 0; i < n; i++) serial_putc(s[i]);
    return;
  }
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
  if (s && con_alheio()) {
    while (*s) serial_putc(*s++);
    return;
  }
  con_break_poll();
  if (!s) return;
  while (*s) con_out(*s++);
}

/* Do lado direito de um pipe ("type x | prog") a entrada vem do pipe;
 * no fim dele k_getc devolve -1 e k_readline -1. */
static int k_getc(void) {
  int c;
  if (shell_pipe_active()) return shell_pipe_getc();
  if (tkey_on()) {
    do {
      c = tscr->key(1);
    } while (c == 0);
    return c;
  }
  do {
    c = shell_get_char();
  } while (c == 0);
  return c;
}

/* Como k_getc, mas nao espera: -1 se nao ha tecla (ou fim do pipe). */
static int k_trygetc(void) {
  int c;
  if (shell_pipe_active()) return shell_pipe_getc();
  if (tkey_on()) {
    c = tscr->key(0);
    return c > 0 ? c : -1;
  }
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
  if (tkey_on()) {
    /* Janela do Command: linha com eco, Backspace apaga. */
    for (;;) {
      int c = tscr->key(1);
      if (c < 0) {
        buf[n] = '\0';
        return n ? n : -1;
      }
      if (c == '\n' || c == '\r') {
        con_out('\n');
        break;
      }
      if (c == '\b') {
        if (n > 0) {
          n--;
          con_out('\b');
        }
        continue;
      }
      if (c >= 32 && c < 256 && n < max - 1) {
        buf[n++] = (char)c;
        con_out(c);
      }
    }
    buf[n] = '\0';
    return n;
  }
  shell_read_line(buf, max);
  while (buf[n]) n++;
  return n;
}

static void k_clear(void) {
  if (tscr_on()) {
    tscr->clear();
    return;
  }
  clear_screen();
}

/* ---------- arquivos ----------
 * Dono do sistema de arquivos. O fat.c tem um estado so (unidade, FAT,
 * pasta carregada) e cede a CPU no meio de leituras e gravacoes longas (para
 * o desktop respirar): o yield de uma tarefa do WIN passa a vez a outro
 * programa e o de uma thread solta o BKL. Com varios processos gravando ao
 * mesmo tempo (Claude Desktop: navegador, rede, renderer) um entrava no meio
 * do outro e o FAT do disco corrompia. Quem chega com o dono ocupado cede a
 * vez ate ele sair; o dono pode reentrar. Tudo isto corre sob o BKL. */
int gui_task_cur(void); /* visual.c */
static int fs_owner, fs_depth;
static volatile int fs_gen; /* futex: muda a cada saida do dono */
static int fs_me(void) {
  if (kt_in_thread()) return 0x100000 + kt_thread_self();
  return 0x1000 + gui_task_cur() + 1;
}
/* A trava nunca prende o sistema: dona que morreu (thread encerrada a forca
 * no fim do programa, no meio de uma leitura) ou que nao solta em 20 s perde
 * a trava. Antes disso, um Firefox reaberto travava o mundo (e o teclado). */
#define FS_STEAL_MS 20000u
static void fs_steal(const char* why) {
  serial_puts("[fs] trava do sistema de arquivos retomada: ");
  serial_puts(why);
  serial_puts(" dono=");
  serial_hex32((uint32_t)fs_owner);
  serial_puts("\n");
  fs_owner = 0;
  fs_depth = 0;
  fs_gen++;
  kt_futex_wake(&fs_gen, -1);
}
/* 1 se quem chama segura a trava do FS (sched.c: a tarefa nao cede a vez). */
int fs_held_by_me(void) { return fs_depth > 0 && fs_owner == fs_me(); }
/* Dono (0x1000+tarefa do WIN+1 no mundo, 0x100000+tid numa thread) e
 * profundidade da trava, para o HANG.LOG (hang.c). */
void fs_lock_info(int* owner, int* depth) {
  if (owner) *owner = fs_owner;
  if (depth) *depth = fs_depth;
}
/* Ctrl+F1 com o mundo parado (ps2in.c, guardiao): solta a trava a forca. */
void fs_force_release(void) {
  if (fs_depth) fs_steal("tecla de emergencia");
}
/* Tela de EXCEPTION (isr.c, CRASH.LOG): o mundo vai parar; grava sem trava. */
volatile int fs_panic;
/* Publica (drives.h): o drives.c pega a trava em toda entrada; recursiva. */
void dstat_path(const char* p);   /* disks/cmd_dstat.c */
void fs_lock(void) {
  int me;
  uint64_t t0;
  if (fs_panic) return;
  me = fs_me();
  t0 = irq_ticks();
  while (fs_depth && fs_owner != me) {
    if (fs_owner >= 0x100000 && !kt_thread_alive(fs_owner - 0x100000)) {
      fs_steal("dona morta");
      break;
    }
    if (irq_ticks() - t0 > FS_STEAL_MS) {
      fs_steal("20 s sem soltar");
      break;
    }
    /* thread: dorme no futex (solta o BKL de verdade); girando com yield as
     * threads esperando tomavam o BKL o tempo todo e o dono (uma tarefa do
     * WIN, que precisa do mundo) nao voltava: tudo parava */
    if (kt_in_thread()) kt_futex_wait(&fs_gen, fs_gen, 5);
    else sched_yield();
  }
  fs_owner = me;
  fs_depth++;
}
#define fs_enter fs_lock
void fs_unlock(void) {
  if (fs_panic || fs_owner != fs_me()) return; /* a trava foi retomada de quem chama */
  if (fs_depth > 0 && --fs_depth == 0) {
    dstat_path(0);
    fs_owner = 0;
    fs_gen++;
    kt_futex_wake(&fs_gen, -1);
  }
}
#define fs_leave fs_unlock
static int k_fsize(const char* path) {
  int rc;
  fs_enter();
  rc = file_size(path);
  fs_leave();
  return rc;
}
static int k_fload(const char* path, char* buf, int max) {
  int rc;
  fs_enter();
  rc = file_load(path, buf, max);
  fs_leave();
  return rc;
}
static int k_fread(const char* path, uint32_t offset, char* buf, int len) {
  int rc;
  fs_enter();
  rc = file_read_at(path, offset, buf, len);
  fs_leave();
  return rc;
}
static int k_fsave(const char* path, const char* data, int size) {
  int rc;
  fs_enter();
  rc = file_save(path, data, size);
  fs_leave();
  win_desk_touch(path);
  return rc;
}
/* Trecho no lugar (k_dl_sym(0, "k_fwrite_at", 0) nos programas): 0 ok; -9 a
 * unidade nao sabe, grave o arquivo inteiro com k_fsave. */
static int k_fwrite_at(const char* path, uint32_t off, const char* data, int len, uint32_t nsize) {
  int rc;
  fs_enter();
  rc = file_write_at(path, off, data, len, nsize);
  fs_leave();
  if (rc == 0) win_desk_touch(path);
  return rc;
}
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
  int rc;
  fs_enter();
  rc = file_list(path, out, max);
  fs_leave();
  return rc;
}

static void k_pwd(char* out, int max) { drives_pwd(out, max); }

static int k_chdir(const char* path) {
  if (!path || !path[0]) return 0;
  return drives_chdir(path);
}

/* Itens de uma pasta com tamanho e data (struct dir_item = k_dirent). */
static int k_dir(const char* path, struct dir_item* out, int max) {
  int rc;
  if (!path || !out || max <= 0) return -1;
  fs_enter();
  rc = drives_items(path, out, max);
  fs_leave();
  return rc;
}

/* Como k_dir, mas com o nome longo inteiro (ate 255): a k_dirent tem
 * name[32] (ABI dos EXEs ja compilados) e nomes maiores voltam como o apelido
 * 8.3 (NOME~1). Aqui cada apelido e trocado pelo nome longo (drives_long_name).
 * Para o WINE (FindFirstFile): o Chromium cria pastas com GUID de 36
 * caracteres e depois lista e compara os nomes. */
static int k_dir_long(const char* path, struct dir_item_long* out, int max) {
  struct dir_item* tmp;
  int rc, i;
  if (!path || !out || max <= 0) return -1;
  tmp = (struct dir_item*)umalloc_owned((uint32_t)sizeof(*tmp) * (uint32_t)max, 0);
  if (!tmp) return -1;
  fs_enter();
  rc = drives_items(path, tmp, max);
  for (i = 0; i < rc; i++) {
    struct dir_item_long* o = &out[i];
    int k, pl = 0;
    for (k = 0; k < 31 && tmp[i].name[k]; k++) o->name[k] = tmp[i].name[k];
    o->name[k] = 0;
    o->size = tmp[i].size;
    o->date = tmp[i].date;
    o->time = tmp[i].time;
    o->dir = tmp[i].dir;
    o->pad[0] = o->pad[1] = o->pad[2] = 0;
    for (k = 0; tmp[i].name[k]; k++)
      if (tmp[i].name[k] == '~') break;
    if (!tmp[i].name[k]) continue; /* sem ~: ja e o nome inteiro */
    {
      char full[DRV_PATH + 40];
      char longn[256];
      while (path[pl] && pl < DRV_PATH - 1) {
        full[pl] = path[pl];
        pl++;
      }
      if (pl && full[pl - 1] != '\\') full[pl++] = '\\';
      for (k = 0; tmp[i].name[k] && pl < (int)sizeof(full) - 1; k++) full[pl++] = tmp[i].name[k];
      full[pl] = 0;
      if (drives_long_name(full, longn, (int)sizeof(longn)) == 0 && longn[0]) {
        for (k = 0; k < 255 && longn[k]; k++) o->name[k] = longn[k];
        o->name[k] = 0;
      }
    }
  }
  fs_leave();
  ufree(tmp);
  return rc;
}

/* A entrada de um arquivo ou pasta so (para o stat): como um item do
 * k_dir_long, mas com uma busca, sem listar a pasta. 0 achou; -1 nao. */
static int k_dir_entry(const char* path, struct dir_item_long* out) {
  int rc;
  if (!path || !out) return -1;
  fs_enter();
  rc = drives_entry(path, out);
  fs_leave();
  return rc;
}

/* Apaga arquivo (aceita * e ?). 0 ou quantos; <0 erro (drives_del). */
static int k_fdelete(const char* path) {
  int rc;
  fs_enter();
  rc = path ? drives_del(path) : -1;
  fs_leave();
  win_desk_touch(path);
  return rc;
}
/* Renomeia sem copiar (drives_rename). <0: ver drives.h; -5/-6 = copiar. */
static int k_frename(const char* from, const char* to) {
  int rc;
  fs_enter();
  rc = drives_rename(from, to);
  fs_leave();
  win_desk_touch(from);
  win_desk_touch(to);
  return rc;
}
/* Mudou algo em ...\DESKTOP: win_desk_touch faz o desktop reler os icones. */
static int k_mkdir(const char* path) {
  int rc;
  fs_enter();
  rc = path ? drives_mkdir(path) : -1;
  fs_leave();
  win_desk_touch(path);
  return rc;
}
static int k_rmdir(const char* path) {
  int rc;
  fs_enter();
  rc = path ? drives_rmdir(path) : -1;
  fs_leave();
  win_desk_touch(path);
  return rc;
}
/* Ambiente do sistema (SET): copia o valor para out. Tamanho; -1 se nao existe. */
static int k_getenv(const char* name, char* out, int max) {
  const char* v = env_get(name);
  int n = 0;
  if (!v) return -1;
  while (v[n] && out && n < max - 1) {
    out[n] = v[n];
    n++;
  }
  if (out && max > 0) out[n] = 0;
  while (v[n]) n++;
  return n;
}
/* val vazio apaga. 0 ok. */
static int k_setenv(const char* name, const char* val) { return env_set(name, val); }
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
  if (tscr_on()) return tscr->vput(row, col, cells, n);
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

static int k_font_get(int ch, uint8_t* glyph) {
  if (tscr_on()) return tscr->font_get(ch, glyph);
  return vga_font_get(ch, glyph);
}

static int k_font_set(int ch, const uint8_t* glyph) {
  int rc;
  if (tscr_on()) return tscr->font_set(ch, glyph);
  if (!text_screen_ok() || vt_bg()) return -1;
  rc = vga_font_set(ch, glyph);
  if (rc == 0 && !font_pid) font_pid = mod_current_pid();
  return rc;
}

static void k_font_reset(void) {
  if (tscr_on()) {
    tscr->font_reset();
    return;
  }
  if (!text_screen_ok() || vt_bg()) return;
  vga_font_reset();
  font_pid = 0;
}

/* Fundo claro na tela de texto: dura ate o programa que pediu terminar. */
static uint32_t blink_pid;

/* Teclado cru (terminal do WSL): ate o programa que pediu terminar. */
static uint32_t kbdraw_pid;
static void k_kbd_raw(int on) {
  vt_kbd_raw(on);   /* da tela do programa; vale quando ela esta visivel */
  kbdraw_pid = on ? mod_current_pid() : 0;
}

/* Registro de nomes -> ponteiro (memoria k_malloc_shared), para um programa
 * achar o servico de outro: o WSL.EXE publica "WSL" (caixa de comandos da
 * VM) e o WSL exec / BASH.EXE do Claude Code acham por aqui. p = 0 tira. */
#define SVC_MAX 8
static struct { char name[16]; void* p; } svc_tab[SVC_MAX];
static int svc_eq(const char* a, const char* b) {
  int i;
  for (i = 0; i < 15 && a[i] && a[i] == b[i]; i++) { }
  return i == 15 || a[i] == b[i];
}
static int k_svc_set(const char* name, void* p) {
  int i, free_i = -1;
  if (!name || !name[0]) return -1;
  for (i = 0; i < SVC_MAX; i++) {
    if (svc_tab[i].name[0] && svc_eq(svc_tab[i].name, name)) {
      svc_tab[i].p = p;
      if (!p) svc_tab[i].name[0] = 0;
      return 0;
    }
    if (!svc_tab[i].name[0] && free_i < 0) free_i = i;
  }
  if (!p) return 0;
  if (free_i < 0) return -1;
  for (i = 0; i < 15 && name[i]; i++) svc_tab[free_i].name[i] = name[i];
  svc_tab[free_i].name[i] = 0;
  svc_tab[free_i].p = p;
  return 0;
}
static void* k_svc_get(const char* name) {
  int i;
  if (!name) return 0;
  for (i = 0; i < SVC_MAX; i++)
    if (svc_tab[i].name[0] && svc_eq(svc_tab[i].name, name)) return svc_tab[i].p;
  return 0;
}
/* 1 se o programa roda numa tela de texto ao fundo (nao a visivel). */
static int k_vt_bg(void) { return vt_bg(); }

static int k_blink(int on) {
  /* A janela sempre mostra o fundo claro: nada a trocar. */
  if (tscr_on()) return 0;
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
  if (tscr_on()) {
    tscr->cursor(row, col);
    return;
  }
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

/* Tamanho da tela de celulas: 80x25 no DOS; numa janela do Command do WIN, o
 * tamanho da janela (e cada mudanca dele chega a k_getc como KEY_RESIZE). */
static int k_textsize(int* cols, int* rows) {
  int c = 80, r = 25;
  if (tscr_on() && tscr->size) {
    tscr->size(&c, &r);
  } else if (!text_screen_ok()) {
    if (cols) *cols = c;
    if (rows) *rows = r;
    return -1;
  }
  if (cols) *cols = c;
  if (rows) *rows = r;
  return 0;
}

int kx_textsize(int* cols, int* rows) { return k_textsize(cols, rows); }
int kx_vput(int row, int col, const uint16_t* cells, int n) { return k_vput(row, col, cells, n); }
void kx_cursor(int row, int col) { k_cursor(row, col); }
int kx_getc(void) { return k_getc(); }
void kx_clear(void) { k_clear(); }
void kx_puts(const char* s) { k_puts(s); }

/* ---------- memoria ---------- */
static void* k_malloc(uint32_t n) { return umalloc(n); }
static void* k_malloc_aligned(uint32_t n, uint32_t align) { return umalloc_aligned(n, align); }
static void k_free(void* p) { ufree(p); }
/* Memoria sem dono (do kernel): sobrevive ao processo que pediu. Para o que
 * dois processos dividem (pipes do WINE.DLL); quem pediu tem de soltar. */
static void* k_malloc_shared(uint32_t n) { return umalloc_owned(n, WINE_SHARED_OWNER); }
static void k_free_shared(void* p) { ufree(p); }
/* RAM acima de 4 GB (so programas de 64 bits enxergam). Teto: SET HIMEM. */
static void* k_hmalloc(uint64_t n, uint64_t align) { return hm_user_alloc(n, align, um_owner()); }
static void k_hfree(void* p) { hm_user_free(p); }
/* Encerra a forca um programa (pid) ou, com all=1, todos menos os de inicio
 * e quem chamou; a memoria dele sai junto (TOP: tecla DEL). Quantos
 * encerrou; -1 sem WIN, -2 o WIN nao respondeu. */
static int k_kill(uint32_t pid, int all) { return win_kill(pid, all); }
static void k_hminfo(uint64_t* quota, uint64_t* used, uint64_t* total_ram) {
  hm_user_info(quota, used, total_ram);
}
/* SSD NVMe cru (so leitura): o WSL.EXE le o NTFS do Windows por aqui. */
static int k_nvme_raw_read(uint64_t lba, uint32_t count, void* buf) {
  return nvme_raw_read(lba, count, buf);
}
static uint64_t k_nvme_raw_sectors(void) { return nvme_raw_sectors(); }

/* NAT da VM do WSL.EXE (net_tap_*): fecha sozinho quando o programa sai. */
static uint32_t tap_pid;
static int k_net_tap_open(int lo, int hi) {
  int r = net_tap_open((uint16_t)lo, (uint16_t)hi);
  if (r == 0) tap_pid = mod_current_pid();
  return r;
}
static void k_net_tap_close(void) {
  net_tap_close();
  tap_pid = 0;
}
static int k_net_tap_recv(void* buf, int cap) { return net_tap_recv((uint8_t*)buf, cap); }
static int k_net_tap_send(const void* ip, int len) { return net_tap_send((const uint8_t*)ip, len); }
static void k_net_addr(uint32_t* ip, uint32_t* mask, uint32_t* gw, uint32_t* dns) {
  net_get_addr(ip, mask, gw, dns);
}
/* TEB do Windows da thread corrente (WINE.DLL): vira a base do GS dela
 * (TEB+0 e TEB+0x18 ficam com o kernel, ver kthread.c). 0 desliga. */
static void k_win_teb(void* teb) { kt_win_teb_set((uint64_t)(uintptr_t)teb); }
/* pid do SODOS do processo corrente (WINE.DLL: casar com o wine_pid_gone). */
static uint32_t k_getpid(void) { return um_owner(); } /* certo tambem numa k_thread */
/* Memoria num endereco baixo fixo para a imagem de um EXE sem relocacoes. */
static void* k_win_lowmem(uint32_t addr, uint32_t size) { return wine_low_alloc(addr, size); }
/* Memoria virtual dos programas do Windows (kernel/vmem.c). */
static uint64_t k_vm_reserve(uint64_t size, uint64_t align, uint64_t at) {
  return at ? vmem_reserve_at(at, size, um_owner()) : vmem_reserve(size, align, um_owner());
}
static int k_vm_commit(uint64_t va, uint64_t size, int prot) { return vmem_commit(va, size, prot); }
static int k_vm_decommit(uint64_t va, uint64_t size) { return vmem_decommit(va, size); }
static int k_vm_release(uint64_t va) {
  uint32_t o = vmem_owner(va), me = um_owner();
  if (o && o != me) { /* reserva de outro processo: quem e? (vista repetida do Claude sumindo) */
    serial_puts("[vmem] release de reserva alheia va=");
    serial_hex32((uint32_t)(va >> 32));
    serial_hex32((uint32_t)va);
    serial_puts(" dono=");
    serial_hex32(o);
    serial_puts(" quem=");
    serial_hex32(me);
    serial_puts("\n");
  }
  return vmem_release(va);
}
static int k_vm_protect(uint64_t va, uint64_t size, int prot, int* old) { return vmem_protect(va, size, prot, old); }
static int k_vm_query(uint64_t va, struct vmem_info* out) { return vmem_query(va, out); }
static int k_vm_owns(uint64_t va) { return vmem_owns(va); }
static int k_vm_alias(uint64_t va, uint64_t phys, uint64_t size, int prot) { return vmem_alias(va, phys, size, prot); }
static void* k_realloc(void* p, uint32_t n) { return urealloc(p, n); }
static uint32_t k_msize(void* p) { return usize(p); }
static int k_date(void);
static int k_time(void);
/* Relogio do CMOS para o resto do kernel (wine.c). */
int kexp_date(void) { return k_date(); }
int kexp_time(void) { return k_time(); }

static void k_meminfo(uint32_t* total, uint32_t* freeb) {
  um_stats(total, 0, freeb, 0);
}

/* ---------- processos e DLL ---------- */
/* Numa thread (kthread.c), exit encerra so a thread: o quadro do mod_exec
 * e da tarefa principal, em outra pilha. */
static void k_exit(int code) {
  if (kt_in_thread()) kt_thread_exit(code);
  mod_exit(code);
}
static int k_shell(char* line) {
  char buf[256];
  int i = 0;
  int rc;
  void (*prev)(char);
  if (!line || kt_in_thread()) return 1;
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
static int cap_len, cap_max, cap_cut;

static void cap_out(char c) {
  if (c == '\f') return;
  if (cap_len < cap_max - 1) cap_buf[cap_len++] = c;
  else cap_cut = 1;
}

static int k_shell_capture(char* line, char* out, int max, int* len) {
  char* sbuf = cap_buf;
  int slen = cap_len, smax = cap_max, scut = cap_cut, stask = cap_task;
  void (*prev)(char) = shell_get_out();
  void (*hook)(char) = con_hook;
  int rc;
  if (len) *len = 0;
  if (kt_in_thread()) return -1;
  if (!out || max < 1) return k_shell(line);
  cap_task = win_task_current();
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
  cap_task = stask;
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
  if (kt_in_thread()) return MOD_ERR_DEPTH;   /* programa novo so do mundo */
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
/* Como k_exec (argv com ponteiros de 8 bytes), mas a saida do programa vai
 * para out, como no k_shell_capture: o WINE roda o GIT.EXE do SODOS para o
 * Claude Desktop e entrega a saida no pipe do processo do Windows. Sem o
 * limite de 255 caracteres da linha do k_shell e sem as regras de aspas do
 * shell (cada argumento vai como veio). */
static int k_exec_capture(const char* path, int argc, char** argv, char* out, int max, int* len) {
  char* sbuf = cap_buf;
  int slen = cap_len, smax = cap_max, scut = cap_cut, stask = cap_task;
  void (*prev)(char) = shell_get_out();
  void (*hook)(char) = con_hook;
  char full[DRV_PATH];
  char* wide[32];
  int rc, n, i;
  if (len) *len = 0;
  if (kt_in_thread() || !out || max < 1) return MOD_ERR_DEPTH;
  if (mod_find_exe(path, full, sizeof(full)) != 0) return MOD_ERR_NOTFOUND;
  n = argc < 0 ? 0 : (argc > 31 ? 31 : argc);
  for (i = 0; i < n; i++) wide[i] = argv && argv[i] ? argv[i] : "";
  wide[n] = 0;
  cap_task = win_task_current();
  cap_buf = out;
  cap_len = 0;
  cap_max = max;
  cap_cut = 0;
  con_hook = 0;
  shell_set_out(cap_out);
  rc = mod_exec(full, n, wide);
  shell_set_out(prev);
  con_hook = hook;
  out[cap_len] = 0;
  if (len) *len = cap_cut ? -cap_len : cap_len;
  cap_task = stask;
  cap_buf = sbuf;
  cap_len = slen;
  cap_max = smax;
  cap_cut = scut;
  return rc;
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
/* Lacos de espera costumam ler o relogio: tambem e ponto de Ctrl+C.
 * Sem BKL no thunk (EXP_NOLOCK): o relogio e o k_* mais chamado pelas threads
 * do Firefox (clock_gettime) e esperar o BKL do mundo por ele custava caro.
 * Thread: so le os ticks. Mundo: pega o BKL como o thunk faria. */
static uint32_t k_ms(void) {
  uint32_t r;
  if (kt_in_thread()) return net_ms_fast();
  kt_bkl_enter();
  con_break_poll();
  r = net_ms();
  kt_bkl_leave();
  return r;
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

/* Soquetes nao bloqueantes (varias conexoes): ver net.h. */
static int k_sock_connect(uint32_t ip, int port) {
  return net_sock_connect(ip, port, mod_current_pid());
}
static int k_sock_poll(int h) { return net_sock_poll(h); }
static int k_sock_err(int h) { return net_sock_err(h); }
static int k_sock_send(int h, const void* d, int n) { return net_sock_send(h, d, n); }
static int k_sock_recv(int h, void* d, int cap, int peek) { return net_sock_recv(h, d, cap, peek); }
static void k_sock_close(int h) { net_sock_close(h); }
static int k_sock_info(int h, uint32_t* lip, int* lport, uint32_t* rip, int* rport) {
  return net_sock_info(h, lip, lport, rip, rport);
}
static void k_net_pump(void) { net_sock_pump(); }

void kexp_release_pid(uint32_t pid) {
  if (!pid) return;
  net_sock_release_owner(pid);
  /* Ao fundo, a fonte e o piscar na tela sao de outra tela: nao mexe. */
  if (font_pid == pid) {
    if (!vt_bg()) vga_font_reset();
    font_pid = 0;
  }
  if (tap_pid == pid) {
    net_tap_close();
    tap_pid = 0;
  }
  if (kbdraw_pid == pid) {
    vt_kbd_raw(0);
    kbdraw_pid = 0;
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
/* gui_thread_enter/leave: chamado por uma thread do programa (o compositor
 * do Firefox pinta de outra CPU), o desenho vai para a janela do programa
 * dela, e nao para a do programa que o mundo esta rodando. */
static int k_gui_open(const char* title) { return gui_open(title); }
static void k_gui_close(void) { gui_close(); }
static void k_gui_title(const char* title) {
  int g = gui_thread_enter();
  gui_title(title);
  gui_thread_leave(g);
}
static void k_gui_size(int* w, int* h) {
  int g = gui_thread_enter();
  gui_size(w, h);
  gui_thread_leave(g);
}
static void k_gui_font(int* cw, int* ch) { gui_font(cw, ch); }
static void k_gui_fill(int x, int y, int w, int h, uint32_t rgb) {
  int g = gui_thread_enter();
  gui_fill(x, y, w, h, rgb);
  gui_thread_leave(g);
}
static void k_gui_text(int x, int y, const char* s, int n, uint32_t fg, uint32_t bg, int flags) {
  int g = gui_thread_enter();
  gui_text(x, y, s, n, fg, bg, flags);
  gui_thread_leave(g);
}
static void k_gui_present(void) {
  int g = gui_thread_enter();
  gui_present();
  gui_thread_leave(g);
}
/* Numa thread a espera fica aqui, cedendo a CPU dela entre uma olhada e
 * outra: o gui_event so tira da fila, e a janela trocada pelo
 * gui_thread_enter nao pode ficar assim enquanto a thread solta o BKL. */
static int k_gui_event(int* ev, int timeout_ms) {
  uint32_t t0;
  if (!kt_in_thread()) return gui_event(ev, timeout_ms);
  if (!ev) return -1;
  t0 = net_ms_fast();
  for (;;) {
    int g = gui_thread_enter();
    int r = gui_event(ev, 0);
    gui_thread_leave(g);
    if (r != 0 || timeout_ms == 0) return r;
    if (timeout_ms > 0 && (int)(net_ms_fast() - t0) >= timeout_ms) return 0;
    kt_yield_in_thread();
  }
}
static int k_gui_raw(int on) {
  int g = gui_thread_enter();
  int r = gui_raw_set(on);
  gui_thread_leave(g);
  return r;
}
static int k_gui_clip(int on) {
  int g = gui_thread_enter();
  int r = gui_clip_set(on);
  gui_thread_leave(g);
  return r;
}
static void k_gui_pixels(int x, int y, int w, int h, const void* rgba, int stride) {
  int g = gui_thread_enter();
  gui_pixels(x, y, w, h, rgba, stride);
  gui_thread_leave(g);
}

/* 0 = retro, 1 = avancado; devolve o modo anterior (mode < 0 so consulta). */
static int k_gui_quality(int mode) {
  if (mode < 0) return gui_quality_get();
  return gui_quality_set(mode);
}

static int k_win_bg(uint32_t rgb) { return win_bg_set(rgb); }

static int k_win_key(int ch) { return win_api_key(ch); }

static int k_win_launch(const char* path) { return kt_in_thread() ? -1 : win_launch(path); }
/* Programa com um argumento, como tarefa do WIN (roda em paralelo com quem pediu).
 * -1: o WIN nao esta no ar ou ja ha um pedido na fila (tente de novo). */
static int k_win_launch_arg(const char* exe, const char* arg) {
  return kt_in_thread() ? -1 : win_launch_arg(exe, arg);
}

/* Abrir um arquivo pelo programa associado (LINKEXT.DLL). 0 = abriu; 1 = sem
 * associacao ou LINKEXT desligado (o programa segue com o padrao dele). */
static int k_open_file(const char* path) {
  if (kt_in_thread() || !path) return 1;
  return linkext_fn ? linkext_fn(path) : 1;
}

/* Pendrive (PENDRIVE.EXE, DISKSYNC). op 0: letra atual (0 = SET PENDRIVE=0),
 * negativa se ainda nao montado; 1: SCAN, texto em out, devolve a letra
 * montada; 2: montar na letra arg; 3: soltar (EJECT). */
static int k_pendrive(int op, int arg, char* out, int max) {
  int L;
  if (kt_in_thread()) return -1;
  switch (op) {
    case 0:
      L = drives_usb_letter();
      if (!L) return 0;
      (void)drives_is_mounted((char)L);   /* monta sozinho se puder */
      return drives_usb_mounted() ? L : -L;
    case 1: return drives_usb_scan(out, max);
    case 2: return drives_usb_mount_at((char)arg);
    case 3: return drives_usb_eject();
    default: return -2;
  }
}

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
/* tema: set = pasta em \SODOS\ICONS (NULL so consulta), out = estado */
static int k_ui_theme(const char* set, struct ui_theme_info* out) {
  return ui_theme_api(set, out);
}
static int k_ui_ia(int on) {
  if (on < 0) return ui_ia_get();
  return ui_ia_set(on);
}
static int k_gui_dock(const char* title, int w) { return gui_dock_open(title, w); }
/* Alt+Tab: o WINTAB.EXE le a pilha de janelas e as miniaturas. */
static int k_win_list(struct k_win_info* out, int max) { return kt_in_thread() ? -1 : win_list_api(out, max); }
static int k_win_thumb(int i, void* rgba, int w, int h) {
  return kt_in_thread() ? -1 : win_thumb_api(i, rgba, w, h);
}
static int k_win_shot(int x, int y, int w, int h, void* rgba) {
  return kt_in_thread() ? -1 : win_shot_api(x, y, w, h, rgba);
}
static int k_win_activate(int id) { return kt_in_thread() ? -1 : win_activate_api(id); }
static int k_alttab(int* tabs) { return alttab_api(tabs); }
static int k_alttab_wait(int seq, int ms) { return kt_in_thread() ? -1 : alttab_wait_api(seq, ms); }
static int k_win_size(int* w, int* h) { return win_screen_size(w, h) ? 0 : -1; }
static int k_win_primary(int* w, int* h) { return win_primary_size(w, h) ? 0 : -1; }
static int k_gui_panel(const char* title, int x, int y, int w, int h) {
  return kt_in_thread() ? -1 : gui_panel_open(title, x, y, w, h);
}
static int k_gui_open_pct(const char* title, int wpct, int hpct) {
  return gui_open_pct(title, wpct, hpct);
}

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

/* Programas do Windows (tarefas do WIN sem modulo, pelo WINE.DLL) e a
 * memoria compartilhada do WINE: somados pelo dono do bloco. */
#define PS_EXTRA 20
static struct {
  int n;
  uint32_t pid[PS_EXTRA];
  uint64_t bytes[PS_EXTRA];
} ps_x;
static void ps_walk_owner(void* arg, int area, uint64_t addr, uint64_t size, int used, uint32_t owner) {
  int i;
  (void)arg;
  (void)area;
  (void)addr;
  if (!used || !owner) return;
  for (i = 0; i < ps_x.n; i++)
    if (ps_x.pid[i] == owner) {
      ps_x.bytes[i] += size;
      return;
    }
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
  { /* tarefas do WIN sem modulo proprio: EXEs do Windows (Claude, ...) */
    static char nm[PS_EXTRA][32];
    static uint32_t pids[PS_EXTRA];
    int t = win_task_list(nm, pids, PS_EXTRA - 1), j, k;
    ps_x.n = 0;
    for (j = 0; j < t; j++) {
      int dup = 0;
      for (k = 0; k < top_nmod; k++)
        if (!top_mod[k].is_dll && top_mod[k].pid == pids[j]) dup = 1;
      if (!dup) ps_x.pid[ps_x.n++] = pids[j];
    }
    ps_x.pid[ps_x.n++] = WINE_SHARED_OWNER;
    for (j = 0; j < ps_x.n; j++) ps_x.bytes[j] = 0;
    mm_walk(ps_walk_owner, 0);
    for (j = 0; j < ps_x.n && n < max; j++) {
      uint32_t pid = ps_x.pid[j];
      if (pid == WINE_SHARED_OWNER && !ps_x.bytes[j]) continue;
      p = &out[n++];
      if (pid == WINE_SHARED_OWNER) {
        ps_name(p, "WINE (secoes)");
      } else {
        const char* nmj = "?";
        for (k = 0; k < t; k++)
          if (pids[k] == pid) nmj = nm[k];
        ps_name(p, nmj);
      }
      p->kind = pid == WINE_SHARED_OWNER ? KP_SYS : KP_EXE;
      p->pid = pid == WINE_SHARED_OWNER ? 0 : pid;
      p->state = pid == WINE_SHARED_OWNER ? 'S' : 'E';
      p->base = 0;
      p->image_kb = 0;
      p->data_kb = to_kb(ps_x.bytes[j]);
      p->refs = 0;
      p->pad = 0;
    }
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
/* Temperatura (CPU, SSD), carga e memoria com historico: TOP, aba Temp. */
static int k_hwstat(struct k_hwstat* out) { return thermal_stat(out); }

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

/* ---------- threads (kthread.c) ----------
 * k_thread_create: fn(arg) numa thread nova, em qualquer CPU de cpu_mask
 * (0 = todas). stack_size 0 = 256 KB. A thread herda a base do FS (TLS) de
 * quem cria; k_thread_tls troca. Devolve tid > 0 ou erro < 0. */
static int k_thread_create(int (*fn)(void*), void* arg, unsigned stack_size, unsigned cpu_mask) {
  return kt_thread_create(fn, arg, stack_size, cpu_mask);
}
static int k_thread_join(int tid, int* code) { return kt_thread_join(tid, code); }
static int k_thread_detach(int tid) { return kt_thread_detach(tid); }
static void k_thread_exit(int code) { kt_thread_exit(code); }
static int k_thread_self(void) { return kt_thread_self(); }
static int k_thread_cpu(void) { return kt_cpu_index(); }
static int k_thread_affinity(int tid, unsigned mask) { return kt_thread_affinity(tid, mask); }
/* yield e futex_wait: a thread solta o BKL logo em seguida, entao o thunk
 * nao o pega (EXP_NOLOCK); so o mundo precisa dele. */
static void k_thread_yield(void) {
  if (kt_in_thread()) {
    kt_yield_in_thread();
    return;
  }
  kt_bkl_enter();
  kt_thread_yield();
  kt_bkl_leave();
}
static int k_thread_tls(unsigned long long fs) { return kt_thread_tls(fs); }
void kt_if_check(const char* onde, uint64_t marca);
static int k_futex_wait(volatile int* addr, int val, int timeout_ms) {
  int r;
  if (kt_in_thread()) {
    kt_if_check("entrada de k_futex_wait, anterior", (uint64_t)(uintptr_t)__builtin_return_address(0));
    r = kt_futex_wait(addr, val, timeout_ms);
    kt_if_check("saida de k_futex_wait", (uint64_t)(uintptr_t)__builtin_return_address(0));
    return r;
  }
  kt_bkl_enter();
  r = kt_futex_wait(addr, val, timeout_ms);
  kt_bkl_leave();
  return r;
}
static int k_futex_wake(volatile int* addr, int n) {
  int r;
  kt_if_check("entrada de k_futex_wake, anterior", (uint64_t)(uintptr_t)__builtin_return_address(0));
  r = kt_futex_wake(addr, n);
  kt_if_check("saida de k_futex_wake", (uint64_t)(uintptr_t)__builtin_return_address(0));
  return r;
}

/* Excecoes do programa vao para entry (sinais da libc; ver isr.c). 0 desliga. */
static int k_fault_entry(void* entry) { return isr_fault_set((uint64_t)(uintptr_t)entry); }

struct kexp {
  const char* name;
  void* fn;
  int nolock;   /* 1 = nao passa pelo BKL (so mexe no escalonador) */
};

#define EXP(n) { #n, (void*)n, 0 }
#define EXP_NOLOCK(n) { #n, (void*)n, 1 }

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
  EXP(k_fwrite_at),
  EXP(k_flist),
  EXP(k_dirlist),
  EXP(k_pwd),
  EXP(k_chdir),
  EXP(k_dir),
  EXP(k_dir_long),
  EXP(k_dir_entry),
  EXP(k_getenv),
  EXP(k_setenv),
  EXP(k_fdelete),
  EXP(k_frename),
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
  EXP(k_malloc_aligned),
  EXP(k_hmalloc),
  EXP(k_hfree),
  EXP(k_hminfo),
  EXP(k_nvme_raw_read),
  EXP(k_nvme_raw_sectors),
  EXP(k_kbd_raw),
  EXP(k_svc_set),
  EXP(k_svc_get),
  EXP(k_vt_bg),
  EXP(k_net_tap_open),
  EXP(k_net_tap_close),
  EXP(k_net_tap_recv),
  EXP(k_net_tap_send),
  EXP(k_net_addr),
  EXP_NOLOCK(k_win_teb),
  EXP_NOLOCK(k_getpid),
  EXP(k_win_lowmem),
  EXP(k_vm_reserve),
  EXP(k_vm_commit),
  EXP(k_vm_decommit),
  EXP(k_vm_release),
  EXP(k_vm_protect),
  EXP(k_vm_query),
  EXP(k_vm_owns),
  EXP(k_vm_alias),
  EXP(k_fault_entry),
  EXP(k_free),
  EXP(k_realloc),
  EXP(k_msize),
  EXP(k_meminfo),
  EXP(k_ps),
  EXP(k_kill),
  EXP(k_memmap),
  EXP(k_vram),
  EXP(k_cpu_count),
  EXP(k_cpu_stat),
  EXP(k_hwstat),
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
  EXP_NOLOCK(k_ms),
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
  EXP(k_sock_connect),
  EXP(k_sock_poll),
  EXP(k_sock_err),
  EXP(k_sock_send),
  EXP(k_sock_recv),
  EXP(k_sock_close),
  EXP(k_sock_info),
  EXP(k_net_pump),
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
  EXP(k_gui_clip),
  EXP(k_gui_pixels),
  EXP(k_gui_quality),
  EXP(k_win_bg),
  EXP(k_win_wallpaper),
  EXP(k_win_key),
  EXP(k_win_launch),
  EXP(k_win_launch_arg),
  EXP(k_malloc_shared),
  EXP(k_free_shared),
  EXP(k_clip_set),
  EXP(k_clip_get),
  EXP(k_ui_menu_pos),
  EXP(k_ui_glass),
  EXP(k_ui_ia),
  EXP(k_ui_theme),
  EXP(k_gui_dock),
  EXP(k_gui_open_pct),
  EXP(k_win_primary),
  EXP(k_thread_create),
  EXP(k_thread_join),
  EXP(k_thread_detach),
  EXP_NOLOCK(k_thread_exit),
  EXP_NOLOCK(k_thread_self),
  EXP_NOLOCK(k_thread_cpu),
  EXP(k_thread_affinity),
  EXP_NOLOCK(k_thread_yield),
  EXP_NOLOCK(k_thread_tls),
  EXP_NOLOCK(k_futex_wait),
  EXP_NOLOCK(k_futex_wake),
  EXP(k_open_file),
  EXP(k_pendrive),
  EXP(k_textsize),
  EXP(k_win_list),
  EXP(k_win_thumb),
  EXP(k_win_shot),
  EXP(k_win_activate),
  EXP(k_alttab),
  EXP(k_win_size),
  EXP(k_gui_panel),
  EXP(k_exec_capture),
  EXP(k_alttab_wait),
};

#define NEXP (sizeof(table) / sizeof(table[0]))

/* Um thunk por export: mov r11, alvo (49 BB imm64) + jmp bkl_thunk_common
 * (E9 rel32). Programas e .so recebem o thunk no lugar da funcao. */
extern void bkl_thunk_common(void);
static uint8_t thunks[NEXP][16] __attribute__((aligned(16)));
static int thunks_ok;

static void thunks_build(void) {
  unsigned i;
  for (i = 0; i < NEXP; i++) {
    uint8_t* p = thunks[i];
    uint64_t fn = (uint64_t)(uintptr_t)table[i].fn;
    int64_t rel = (int64_t)(uintptr_t)bkl_thunk_common - (int64_t)(uintptr_t)(p + 15);
    int k;
    p[0] = 0x49;
    p[1] = 0xBB;
    for (k = 0; k < 8; k++) p[2 + k] = (uint8_t)(fn >> (k * 8));
    p[10] = 0xE9;
    for (k = 0; k < 4; k++) p[11 + k] = (uint8_t)((uint64_t)rel >> (k * 8));
    p[15] = 0xCC;
  }
  thunks_ok = 1;
}

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
    if (*a == *b) {
      if (table[i].nolock) return table[i].fn;
      if (!thunks_ok) thunks_build();
      return thunks[i];
    }
  }
  return 0;
}
