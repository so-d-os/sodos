#include "shell.h"
#include "drives.h"
#include "mm.h"
#include "fbcon.h"
#include "reboot.h"

/* Tela: linha 0 = barra azul, 1..23 = texto, 24 = estado. */
#define EDIT_COLS 80
#define EDIT_ROWS 25
#define EDIT_TOP 1
#define EDIT_LINES (EDIT_ROWS - 2)
#define EDIT_TAB 8

#define ATTR_TEXT 0x07
#define ATTR_BAR 0x1F   /* branco no azul */
#define ATTR_HOT 0x1E   /* amarelo no azul */

/* O texto fica todo na memoria alta (umalloc), sem \r: arquivo com CRLF
 * e lido sem \r e gravado com \r\n de novo. */
static char* edit_buf;
static int edit_len;
static int edit_cap;
static int edit_cur;
static int edit_top;       /* inicio da primeira linha na tela */
static int edit_top_line;  /* numero (0..) dessa linha */
static int edit_left;      /* primeira coluna visivel */
static int edit_dirty;
static int edit_crlf;
static char edit_path[128];
static char edit_msg[48];

static void edit_set_msg(const char* s) {
  int i = 0;
  while (s[i] && i < (int)sizeof(edit_msg) - 1) {
    edit_msg[i] = s[i];
    i++;
  }
  edit_msg[i] = '\0';
}

static int edit_grow(int need) {
  char* nb;
  int ncap;
  int i;
  if (need < edit_cap) return 0;
  ncap = edit_cap * 2 + 4096;
  if (ncap <= need) ncap = need + 4096;
  nb = (char*)umalloc_owned((uint32_t)ncap, 0);
  if (!nb) return -1;
  for (i = 0; i < edit_len; i++) nb[i] = edit_buf[i];
  ufree(edit_buf);
  edit_buf = nb;
  edit_cap = ncap;
  return 0;
}

static int edit_line_start(int p) {
  while (p > 0 && edit_buf[p - 1] != '\n') p--;
  return p;
}

static int edit_line_end(int p) {
  while (p < edit_len && edit_buf[p] != '\n') p++;
  return p;
}

static int edit_next_col(int col, char ch) {
  if (ch == '\t') return (col / EDIT_TAB + 1) * EDIT_TAB;
  return col + 1;
}

/* Coluna na tela de p, contando tabs, a partir do inicio da linha ls. */
static int edit_vcol(int ls, int p) {
  int col = 0;
  while (ls < p) col = edit_next_col(col, edit_buf[ls++]);
  return col;
}

static int edit_at_vcol(int ls, int want) {
  int col = 0;
  while (ls < edit_len && edit_buf[ls] != '\n') {
    int w = edit_next_col(col, edit_buf[ls]);
    if (w > want) break;
    col = w;
    ls++;
  }
  return ls;
}

/* Linhas entre o topo da tela e a linha do cursor. */
static int edit_cursor_row(void) {
  int ls = edit_line_start(edit_cur);
  int p = edit_top;
  int r = 0;
  while (p < ls) {
    p = edit_line_end(p) + 1;
    r++;
  }
  return r;
}

static void edit_scroll(void) {
  int ls = edit_line_start(edit_cur);
  int r, c;
  while (ls < edit_top) {
    edit_top = edit_line_start(edit_top - 1);
    edit_top_line--;
  }
  r = edit_cursor_row();
  while (r >= EDIT_LINES) {
    edit_top = edit_line_end(edit_top) + 1;
    edit_top_line++;
    r--;
  }
  c = edit_vcol(ls, edit_cur);
  if (c < edit_left) edit_left = c > 20 ? c - 20 : 0;
  if (c >= edit_left + EDIT_COLS) edit_left = c - EDIT_COLS + 20;
}

static void edit_cell(int r, int c, unsigned char ch, unsigned char attr) {
  if (r < 0 || r >= EDIT_ROWS || c < 0 || c >= EDIT_COLS) return;
  vga_text[r * EDIT_COLS + c] = (uint16_t)ch | (uint16_t)(attr << 8);
}

/* Escreve s a partir de c; devolve a coluna seguinte. */
static int edit_text(int r, int c, const char* s, unsigned char attr) {
  while (*s && c < EDIT_COLS) edit_cell(r, c++, (unsigned char)*s++, attr);
  return c;
}

static int edit_num(int r, int c, int n, unsigned char attr) {
  char num[16];
  uint32_to_string((uint32_t)n, num, (int)sizeof(num));
  return edit_text(r, c, num, attr);
}

static void edit_draw(void) {
  int r, c, p;
  int row = edit_cursor_row();
  int ls = edit_line_start(edit_cur);
  int ccol = edit_vcol(ls, edit_cur) - edit_left;

  /* Barra azul no topo */
  for (c = 0; c < EDIT_COLS; c++) edit_cell(0, c, ' ', ATTR_BAR);
  c = edit_text(0, 1, "EDIT ", ATTR_HOT);
  c = edit_text(0, c, edit_path, ATTR_BAR);
  if (edit_dirty) edit_text(0, c, " *", ATTR_HOT);
  c = EDIT_COLS - 26;
  c = edit_text(0, c, "CTRL+W", ATTR_HOT);
  c = edit_text(0, c, "=Salvar  ", ATTR_BAR);
  c = edit_text(0, c, "ESC", ATTR_HOT);
  edit_text(0, c, "=Sair", ATTR_BAR);

  /* Texto */
  p = edit_top;
  for (r = 0; r < EDIT_LINES; r++) {
    int col = 0;
    int y = EDIT_TOP + r;
    for (c = 0; c < EDIT_COLS; c++) edit_cell(y, c, ' ', ATTR_TEXT);
    if (p > edit_len) continue;
    while (p < edit_len && edit_buf[p] != '\n') {
      unsigned char ch = (unsigned char)edit_buf[p];
      int w = edit_next_col(col, (char)ch);
      if (ch != '\t') {
        if (ch < 32) ch = '.';
        if (col >= edit_left) edit_cell(y, col - edit_left, ch, ATTR_TEXT);
      }
      col = w;
      p++;
    }
    p++;
  }

  /* Estado */
  for (c = 0; c < EDIT_COLS; c++) edit_cell(EDIT_ROWS - 1, c, ' ', ATTR_TEXT);
  c = edit_text(EDIT_ROWS - 1, 0, "Lin ", ATTR_TEXT);
  c = edit_num(EDIT_ROWS - 1, c, edit_top_line + row + 1, ATTR_TEXT);
  c = edit_text(EDIT_ROWS - 1, c, "  Col ", ATTR_TEXT);
  c = edit_num(EDIT_ROWS - 1, c, edit_vcol(ls, edit_cur) + 1, ATTR_TEXT);
  c = edit_text(EDIT_ROWS - 1, c, "  bytes ", ATTR_TEXT);
  c = edit_num(EDIT_ROWS - 1, c, edit_len, ATTR_TEXT);
  if (edit_crlf) c = edit_text(EDIT_ROWS - 1, c, "  CRLF", ATTR_TEXT);
  if (edit_msg[0]) {
    c = edit_text(EDIT_ROWS - 1, c, "   ", ATTR_TEXT);
    edit_text(EDIT_ROWS - 1, c, edit_msg, ATTR_TEXT);
  }

  fbcon_sync();
  if (ccol < 0) ccol = 0;
  if (ccol >= EDIT_COLS) ccol = EDIT_COLS - 1;
  goto_xy(EDIT_TOP + row, ccol);
}

static void edit_insert(char ch) {
  int i;
  if (edit_grow(edit_len + 1) != 0) {
    edit_set_msg("sem memoria");
    return;
  }
  for (i = edit_len; i > edit_cur; i--)
    edit_buf[i] = edit_buf[i - 1];
  edit_buf[edit_cur] = ch;
  edit_len++;
  edit_cur++;
  edit_dirty = 1;
}

static void edit_delete(int p) {
  int i;
  if (p < 0 || p >= edit_len) return;
  for (i = p; i < edit_len - 1; i++)
    edit_buf[i] = edit_buf[i + 1];
  edit_len--;
  if (edit_cur > p) edit_cur--;
  /* Apagou o \n logo acima da tela: o topo vira o inicio da linha juntada. */
  if (p < edit_top) {
    edit_top = edit_line_start(p);
    edit_top_line--;
  }
  edit_dirty = 1;
}

static void edit_move_line(int dir) {
  int ls = edit_line_start(edit_cur);
  int col = edit_vcol(ls, edit_cur);
  if (dir < 0) {
    if (ls == 0) {
      edit_cur = 0;
      return;
    }
    ls = edit_line_start(ls - 1);
  } else {
    int le = edit_line_end(edit_cur);
    if (le >= edit_len) {
      edit_cur = edit_len;
      return;
    }
    ls = le + 1;
  }
  edit_cur = edit_at_vcol(ls, col);
}

static void edit_save(void) {
  const char* data = edit_buf;
  char* tmp = 0;
  int n = edit_len;
  int i, j;
  if (edit_crlf) {
    int nl = 0;
    for (i = 0; i < edit_len; i++)
      if (edit_buf[i] == '\n') nl++;
    tmp = (char*)umalloc_owned((uint32_t)(edit_len + nl + 1), 0);
    if (!tmp) {
      edit_set_msg("sem memoria para salvar");
      return;
    }
    for (i = 0, j = 0; i < edit_len; i++) {
      if (edit_buf[i] == '\n') tmp[j++] = '\r';
      tmp[j++] = edit_buf[i];
    }
    data = tmp;
    n = j;
  }
  if (file_save(edit_path, data, n) != 0) {
    edit_set_msg("ERRO ao salvar");
  } else {
    edit_set_msg("Salvo.");
    edit_dirty = 0;
  }
  if (tmp) ufree(tmp);
}

/* Carrega o arquivo inteiro; tira o \r de cada \r\n. -1 = sem memoria. */
static int edit_load(void) {
  int sz = file_size(edit_path);
  int n, i, j;
  if (sz < 0) sz = 0;
  edit_cap = sz + sz / 2 + 4096;
  edit_buf = (char*)umalloc_owned((uint32_t)edit_cap, 0);
  if (!edit_buf) return -1;
  edit_len = 0;
  edit_crlf = 0;
  if (sz > 0) {
    n = file_load(edit_path, edit_buf, edit_cap);
    if (n > 0) edit_len = n;
  }
  for (i = 0, j = 0; i < edit_len; i++) {
    if (edit_buf[i] == '\r' && i + 1 < edit_len && edit_buf[i + 1] == '\n') {
      edit_crlf = 1;
      continue;
    }
    edit_buf[j++] = edit_buf[i];
  }
  edit_len = j;
  return 0;
}

int cmd_edit(int argc, char** argv) {
  char defpath[16];
  const char* path;
  if (argc >= 2) {
    path = argv[1];
  } else {
    defpath[0] = drives_current();
    defpath[1] = ':';
    defpath[2] = '\\';
    defpath[3] = 'n';
    defpath[4] = 'o';
    defpath[5] = 't';
    defpath[6] = 'a';
    defpath[7] = '.';
    defpath[8] = 't';
    defpath[9] = 'x';
    defpath[10] = 't';
    defpath[11] = '\0';
    path = defpath;
  }

  {
    int i = 0;
    while (path[i] && i < (int)sizeof(edit_path) - 1) {
      edit_path[i] = path[i];
      i++;
    }
    edit_path[i] = '\0';
  }

  if (!drives_is_mounted(path_letter(edit_path))) {
    puts_k("Unidade nao montada. Use: mount D:\n");
    return 1;
  }

  if (edit_load() != 0) {
    puts_k("sem memoria para abrir o arquivo\n");
    return 1;
  }
  edit_cur = 0;
  edit_top = 0;
  edit_top_line = 0;
  edit_left = 0;
  edit_dirty = 0;
  edit_set_msg(edit_len ? "" : "(arquivo vazio ou novo)");

  clear_screen();
  edit_draw();
  for (;;) {
    int k = shell_get_char();
    if (k == 0) continue;
    if (k == 27) break;
    edit_msg[0] = '\0';
    if (kbd_ctrl_down() && k < 0x100) {
      if (k == 'w' || k == 'W' || k == 23) edit_save();
      else continue;
    } else if (k == KEY_LEFT) {
      if (edit_cur > 0) edit_cur--;
    } else if (k == KEY_RIGHT) {
      if (edit_cur < edit_len) edit_cur++;
    } else if (k == KEY_UP) {
      edit_move_line(-1);
    } else if (k == KEY_DOWN) {
      edit_move_line(1);
    } else if (k == KEY_PGUP) {
      int i;
      for (i = 0; i < EDIT_LINES - 1; i++) edit_move_line(-1);
    } else if (k == KEY_PGDN) {
      int i;
      for (i = 0; i < EDIT_LINES - 1; i++) edit_move_line(1);
    } else if (k == KEY_HOME) {
      edit_cur = edit_line_start(edit_cur);
    } else if (k == KEY_END) {
      edit_cur = edit_line_end(edit_cur);
    } else if (k == KEY_DEL) {
      edit_delete(edit_cur);
    } else if (k == '\b') {
      if (edit_cur > 0) edit_delete(edit_cur - 1);
    } else if (k == '\n' || k == '\r') {
      edit_insert('\n');
    } else if (k == '\t' || (k >= 32 && k < 127) || (k >= 128 && k < 256)) {
      edit_insert((char)k);
    } else {
      continue;
    }
    edit_scroll();
    edit_draw();
  }

  ufree(edit_buf);
  edit_buf = 0;
  clear_screen();
  if (edit_dirty) {
    puts_k("Saiu sem salvar: ");
    puts_k(edit_path);
    puts_k("\n");
  }
  return 0;
}
