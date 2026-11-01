/* tterm.c - prova do TERM.DLL (gcc -mx32) */
#include <curses.h>
#include <so.h>

int main(void) {
  initscr();
  noecho();
  cbreak();
  keypad(stdscr, 1);
  if (has_colors()) {
    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLUE);
  }
  attron(COLOR_PAIR(1));
  mvprintw(0, 0, "SODOS term shim %dx%d", COLS, LINES);
  attroff(COLOR_PAIR(1));
  mvaddstr(2, 2, "ACS: ");
  addch(ACS_ULCORNER);
  addch(ACS_HLINE);
  addch(ACS_URCORNER);
  mvprintw(4, 2, "ok");
  refresh();
  endwin();
  k_puts("tterm ok\n");
  return 0;
}
