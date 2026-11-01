/* term.c - shim ncurses minimo (80x25) sobre k_putc/k_clear/k_getc.
 * Compilar com gcc -mx32 (SysV). Saida: TERM.DLL */
#include <curses.h>
#include <so.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define T_ROWS 25
#define T_COLS 80
#define T_CELLS (T_ROWS * T_COLS)

struct _win_st {
  int dummy;
};

static WINDOW stdscr_store;
static WINDOW* stdscr_ptr;
static unsigned short cells[T_CELLS]; /* ch | (pair<<8) | attrs hi */
static unsigned short prev[T_CELLS];
static int cur_y, cur_x;
static int attr_cur;
static int pair_cur;
static int echo_on;
static int inited;
static short pairs[64][2];

WINDOW* __stdscr(void) { return stdscr_ptr; }

static unsigned short pack(int ch) {
  int c = ch & 0xFF;
  unsigned short a = 0;
  if (c == 0) c = ' ';
  if (attr_cur & A_REVERSE) a |= A_REVERSE;
  if (attr_cur & A_BOLD) a |= A_BOLD;
  return (unsigned short)(c | ((pair_cur & 0x3F) << 8) | a);
}

static void cell_set(int y, int x, int ch) {
  if (y < 0 || y >= T_ROWS || x < 0 || x >= T_COLS) return;
  cells[y * T_COLS + x] = pack(ch);
}

WINDOW* initscr(void) {
  int i;
  if (inited) return stdscr;
  for (i = 0; i < T_CELLS; i++) {
    cells[i] = (unsigned short)' ';
    prev[i] = 0xFFFF;
  }
  cur_y = cur_x = 0;
  attr_cur = A_NORMAL;
  pair_cur = 0;
  echo_on = 1;
  stdscr_ptr = &stdscr_store;
  inited = 1;
  k_clear();
  return stdscr_ptr;
}

int endwin(void) {
  if (!inited) return ERR;
  k_clear();
  inited = 0;
  stdscr_ptr = 0;
  return OK;
}

int clear(void) {
  int i;
  for (i = 0; i < T_CELLS; i++) cells[i] = (unsigned short)' ';
  cur_y = cur_x = 0;
  return OK;
}

int erase(void) { return clear(); }

int move(int y, int x) {
  if (y < 0) y = 0;
  if (x < 0) x = 0;
  if (y >= T_ROWS) y = T_ROWS - 1;
  if (x >= T_COLS) x = T_COLS - 1;
  cur_y = y;
  cur_x = x;
  return OK;
}

int wmove(WINDOW* win, int y, int x) {
  (void)win;
  return move(y, x);
}

int addch(int ch) {
  cell_set(cur_y, cur_x, ch);
  cur_x++;
  if (cur_x >= T_COLS) {
    cur_x = 0;
    if (cur_y < T_ROWS - 1) cur_y++;
  }
  return OK;
}

int waddch(WINDOW* win, int ch) {
  (void)win;
  return addch(ch);
}

int mvaddch(int y, int x, int ch) {
  move(y, x);
  return addch(ch);
}

int addstr(const char* s) {
  if (!s) return ERR;
  while (*s) addch((unsigned char)*s++);
  return OK;
}

int mvaddstr(int y, int x, const char* s) {
  move(y, x);
  return addstr(s);
}

int printw(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return addstr(buf);
}

int mvprintw(int y, int x, const char* fmt, ...) {
  char buf[256];
  va_list ap;
  move(y, x);
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return addstr(buf);
}

/* Redesenha a tela inteira (simples; ok para 80x25). A_REVERSE -> ANSI. */
int refresh(void) {
  int y, x;
  int rev = 0;
  if (!inited) return ERR;
  k_clear();
  for (y = 0; y < T_ROWS; y++) {
    for (x = 0; x < T_COLS; x++) {
      unsigned short c = cells[y * T_COLS + x];
      char ch = (char)(c & 0xFF);
      int want = (c & A_REVERSE) != 0;
      if (!ch) ch = ' ';
      if (want && !rev) {
        k_puts("\033[7m");
        rev = 1;
      } else if (!want && rev) {
        k_puts("\033[0m");
        rev = 0;
      }
      k_putc(ch);
      prev[y * T_COLS + x] = c;
    }
    if (rev) {
      k_puts("\033[0m");
      rev = 0;
    }
    if (y + 1 < T_ROWS) k_putc('\n');
  }
  return OK;
}

int wrefresh(WINDOW* win) {
  (void)win;
  return refresh();
}

int getch(void) {
  int c = k_getc();
  if (echo_on && c >= 32 && c < 127) {
    addch(c);
    refresh();
  }
  return c;
}

int wgetch(WINDOW* win) {
  (void)win;
  return getch();
}

int keypad(WINDOW* win, int bf) {
  (void)win;
  (void)bf;
  return OK;
}
int noecho(void) {
  echo_on = 0;
  return OK;
}
int echo(void) {
  echo_on = 1;
  return OK;
}
int cbreak(void) { return OK; }
int nocbreak(void) { return OK; }
int raw(void) { return OK; }
int noraw(void) { return OK; }

int has_colors(void) { return 1; }
int start_color(void) { return OK; }

int init_pair(short pair, short f, short b) {
  if (pair < 0 || pair >= 64) return ERR;
  pairs[pair][0] = f;
  pairs[pair][1] = b;
  return OK;
}

int COLOR_PAIR(int n) { return (n & 0x3F) << 8; }

int attron(int attrs) {
  if (attrs & 0x3F00) pair_cur = (attrs >> 8) & 0x3F;
  attr_cur |= attrs & ~0x3F00;
  return OK;
}

int attroff(int attrs) {
  attr_cur &= ~attrs;
  return OK;
}

void getmaxyx_impl(WINDOW* win, int* y, int* x) {
  (void)win;
  if (y) *y = T_ROWS;
  if (x) *x = T_COLS;
}
