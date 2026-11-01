/* curses.h - subset ncurses para o term shim (VGA 80x25 / soxrun). */
#ifndef _CURSES_H
#define _CURSES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _win_st WINDOW;

/* SOX nao importa dados de DLL — macros + accessor. */
#define LINES 25
#define COLS  80
WINDOW* __stdscr(void);
#define stdscr (__stdscr())

#define ERR (-1)
#define OK  0

#define A_NORMAL    0
#define A_REVERSE   0x8000
#define A_BOLD      0x4000
#define A_UNDERLINE 0x2000

#define COLOR_BLACK   0
#define COLOR_RED     1
#define COLOR_GREEN   2
#define COLOR_YELLOW  3
#define COLOR_BLUE    4
#define COLOR_MAGENTA 5
#define COLOR_CYAN    6
#define COLOR_WHITE   7

#define KEY_DOWN  0x100
#define KEY_UP    0x101
#define KEY_LEFT  0x102
#define KEY_RIGHT 0x103
#define KEY_PPAGE 0x104
#define KEY_NPAGE 0x105
#define KEY_HOME  0x106
#define KEY_END   0x107
#define KEY_DC    0x108
#define KEY_BACKSPACE 0x108 /* mesmo codigo do kernel KEY_DEL em alguns casos */
#define KEY_ENTER 0x10A
#define KEY_F0    0x140
#define KEY_F(n)  (KEY_F0 + (n))

/* ACS: cantos/linhas ASCII simples (sem font line-drawing ainda) */
#define ACS_ULCORNER '+'
#define ACS_LLCORNER '+'
#define ACS_URCORNER '+'
#define ACS_LRCORNER '+'
#define ACS_HLINE    '-'
#define ACS_VLINE    '|'
#define ACS_LTEE     '+'
#define ACS_RTEE     '+'
#define ACS_TTEE     '+'
#define ACS_BTEE     '+'
#define ACS_PLUS     '+'

WINDOW* initscr(void);
int endwin(void);
int refresh(void);
int wrefresh(WINDOW* win);
int clear(void);
int erase(void);
int move(int y, int x);
int wmove(WINDOW* win, int y, int x);
int addch(int ch);
int waddch(WINDOW* win, int ch);
int mvaddch(int y, int x, int ch);
int addstr(const char* s);
int mvaddstr(int y, int x, const char* s);
int printw(const char* fmt, ...);
int mvprintw(int y, int x, const char* fmt, ...);
int getch(void);
int wgetch(WINDOW* win);
int keypad(WINDOW* win, int bf);
int noecho(void);
int echo(void);
int cbreak(void);
int nocbreak(void);
int raw(void);
int noraw(void);
int has_colors(void);
int start_color(void);
int init_pair(short pair, short f, short b);
int attron(int attrs);
int attroff(int attrs);
int COLOR_PAIR(int n);
void getmaxyx_impl(WINDOW* win, int* y, int* x);
#define getmaxyx(win, y, x) getmaxyx_impl((win), &(y), &(x))

#ifdef __cplusplus
}
#endif

#endif
