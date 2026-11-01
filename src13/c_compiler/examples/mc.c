/* mc.c - file manager SODOS (port hibrido MC). gcc -mx32 + TERM/POSIX/GLIB/LIBCX.
 * Dois paineis 80x25; listagem via opendir. F10/q sai. "auto" = desenha e sai. */
#include <curses.h>
#include <dirent.h>
#include <glib.h>
#include <so.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_ENT 128
#define NAME_LEN 28
#define PANEL_W 38
#define PANEL_H 20
#define PANEL_ROWS (PANEL_H - 2)

typedef struct {
  char name[NAME_LEN];
  int is_dir;
} Ent;

typedef struct {
  char path[96];
  Ent ents[MAX_ENT];
  int n;
  int top;
  int cur;
} Panel;

static Panel panels[2];
static int active;

static void path_copy(char* dst, const char* src, int max) {
  int i = 0;
  if (!src) src = "";
  while (src[i] && i < max - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}

static void path_join(char* dst, int max, const char* dir, const char* name) {
  int i = 0, j = 0;
  while (dir[i] && i < max - 1) {
    dst[i] = dir[i];
    i++;
  }
  if (i > 0 && dst[i - 1] != '\\' && dst[i - 1] != '/' && i < max - 1) dst[i++] = '\\';
  while (name[j] && i < max - 1) dst[i++] = name[j++];
  dst[i] = 0;
}

static int path_parent(char* path) {
  int n = (int)strlen(path);
  if (n <= 3 && path[1] == ':') return 0; /* D:\ */
  while (n > 0 && (path[n - 1] == '\\' || path[n - 1] == '/')) {
    path[--n] = 0;
  }
  while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') n--;
  if (n == 0) return 0;
  if (n == 2 && path[1] == ':') {
    path[2] = '\\';
    path[3] = 0;
    return 1;
  }
  path[n] = 0;
  return 1;
}

static int ent_cmp(const void* a, const void* b) {
  const Ent* ea = (const Ent*)a;
  const Ent* eb = (const Ent*)b;
  if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
  return strcmp(ea->name, eb->name);
}

/* qsort minimo */
static void sort_ents(Ent* e, int n) {
  int i, j;
  for (i = 0; i < n; i++)
    for (j = i + 1; j < n; j++)
      if (ent_cmp(&e[j], &e[i]) < 0) {
        Ent t = e[i];
        e[i] = e[j];
        e[j] = t;
      }
}

static void panel_load(Panel* p) {
  DIR* d;
  struct dirent* de;
  p->n = 0;
  p->top = 0;
  p->cur = 0;
  d = opendir(p->path);
  if (!d) return;
  while ((de = readdir(d)) != NULL && p->n < MAX_ENT) {
    if (de->d_name[0] == '.' && de->d_name[1] == 0) continue;
    path_copy(p->ents[p->n].name, de->d_name, NAME_LEN);
    p->ents[p->n].is_dir = (de->d_type == DT_DIR);
    p->n++;
  }
  closedir(d);
  sort_ents(p->ents, p->n);
}

static void draw_hline(int y, int x0, int x1, int ch) {
  int x;
  for (x = x0; x <= x1; x++) mvaddch(y, x, ch);
}

static void draw_box(int y, int x, int h, int w) {
  int i;
  mvaddch(y, x, ACS_ULCORNER);
  draw_hline(y, x + 1, x + w - 2, ACS_HLINE);
  mvaddch(y, x + w - 1, ACS_URCORNER);
  for (i = 1; i < h - 1; i++) {
    mvaddch(y + i, x, ACS_VLINE);
    mvaddch(y + i, x + w - 1, ACS_VLINE);
  }
  mvaddch(y + h - 1, x, ACS_LLCORNER);
  draw_hline(y + h - 1, x + 1, x + w - 2, ACS_HLINE);
  mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
}

static void draw_panel(Panel* p, int y, int x, int focused) {
  int i, row;
  char title[40];
  draw_box(y, x, PANEL_H, PANEL_W);
  g_snprintf(title, sizeof title, " %s ", p->path);
  if ((int)strlen(title) > PANEL_W - 2) {
    title[0] = ' ';
    title[1] = '.';
    title[2] = '.';
    title[3] = '.';
    path_copy(title + 4, p->path + (int)strlen(p->path) - (PANEL_W - 8), PANEL_W - 6);
  }
  if (focused) attron(A_REVERSE);
  mvaddstr(y, x + 1, title);
  if (focused) attroff(A_REVERSE);

  if (p->cur < p->top) p->top = p->cur;
  if (p->cur >= p->top + PANEL_ROWS) p->top = p->cur - PANEL_ROWS + 1;
  if (p->top < 0) p->top = 0;

  for (row = 0; row < PANEL_ROWS; row++) {
    i = p->top + row;
    move(y + 1 + row, x + 1);
    addstr("                                  ");
    if (i >= p->n) continue;
    move(y + 1 + row, x + 1);
    if (focused && i == p->cur) attron(A_REVERSE);
    if (p->ents[i].is_dir) {
      addch('/');
      addstr(p->ents[i].name);
    } else {
      addch(' ');
      addstr(p->ents[i].name);
    }
    if (focused && i == p->cur) attroff(A_REVERSE);
  }
}

static void draw_all(void) {
  mvaddstr(0, 0, " Midnight Commander SODOS ");
  draw_hline(0, 26, 79, ACS_HLINE);
  draw_panel(&panels[0], 1, 1, active == 0);
  draw_panel(&panels[1], 1, 41, active == 1);
  move(22, 0);
  addstr("Tab:painel  Enter:abrir  Bksp:..  j/k:mover  q/F10:sair              ");
  move(23, 0);
  addstr("SODOS MC hibrido | TERM+POSIXX+GLIB+LIBCX (gcc -mx32)                ");
  refresh();
}

static void panel_enter(Panel* p) {
  Ent* e;
  char next[96];
  if (p->n == 0 || p->cur < 0 || p->cur >= p->n) return;
  e = &p->ents[p->cur];
  if (!e->is_dir) return;
  if (strcmp(e->name, "..") == 0) {
    path_parent(p->path);
  } else {
    path_join(next, sizeof next, p->path, e->name);
    path_copy(p->path, next, (int)sizeof p->path);
  }
  panel_load(p);
}

static int handle_key(int c) {
  Panel* p = &panels[active];
  if (c == 'q' || c == 'Q' || c == KEY_F(10) || c == 27) return 0;
  if (c == '\t') {
    active = 1 - active;
    return 1;
  }
  if (c == KEY_DOWN || c == 'j') {
    if (p->cur + 1 < p->n) p->cur++;
    return 1;
  }
  if (c == KEY_UP || c == 'k') {
    if (p->cur > 0) p->cur--;
    return 1;
  }
  if (c == '\n' || c == '\r' || c == KEY_ENTER || c == ' ') {
    panel_enter(p);
    return 1;
  }
  if (c == KEY_BACKSPACE || c == 127 || c == 8 || c == '-') {
    if (path_parent(p->path)) panel_load(p);
    return 1;
  }
  return 1;
}

int main(int argc, char** argv) {
  int auto_quit = 0;
  int i;
  for (i = 1; i < argc; i++)
    if (strcmp(argv[i], "auto") == 0) auto_quit = 1;

  path_copy(panels[0].path, "D:\\", (int)sizeof panels[0].path);
  path_copy(panels[1].path, "D:\\", (int)sizeof panels[1].path);
  getcwd(panels[0].path, sizeof panels[0].path);
  getcwd(panels[1].path, sizeof panels[1].path);
  active = 0;
  panel_load(&panels[0]);
  panel_load(&panels[1]);

  initscr();
  noecho();
  cbreak();
  keypad(stdscr, 1);
  if (has_colors()) {
    start_color();
    init_pair(1, COLOR_WHITE, COLOR_BLUE);
  }

  draw_all();
  if (auto_quit) {
    endwin();
    printf("mc auto: %d+%d ents\n", panels[0].n, panels[1].n);
    return 0;
  }
  while (handle_key(getch())) draw_all();
  endwin();
  return 0;
}
