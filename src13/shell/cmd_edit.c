#include "shell.h"
#include "drives.h"

#define EDIT_MAX 2048
#define EDIT_COLS 80
#define EDIT_ROWS 25

static char edit_buf[EDIT_MAX];
static int edit_len;
static int edit_cur;
static int edit_dirty;
static char edit_path[64];

static void edit_status(const char* s) {
  int i;
  int c = 0;
  for (i = 0; s[i] && c < EDIT_COLS; i++)
    vga_put_at(EDIT_ROWS - 1, c++, (unsigned char)s[i]);
  while (c < EDIT_COLS)
    vga_put_at(EDIT_ROWS - 1, c++, ' ');
}

static void edit_draw(void) {
  int i;
  int r = 1;
  int c = 0;
  int cr = 1;
  int cc = 0;
  char num[16];
  char line[80];
  int n = 0;
  clear_screen();
  puts_k("EDIT ");
  puts_k(edit_path);
  puts_k("   ESC=salvar e sair  setas=mover\n");
  if (edit_len == 0) {
    edit_status("(arquivo vazio)");
    goto_xy(1, 0);
    update_cursor();
    return;
  }
  for (i = 0; i < edit_len && r < EDIT_ROWS - 1; i++) {
    unsigned char ch = (unsigned char)edit_buf[i];
    if (i == edit_cur) {
      cr = r;
      cc = c;
    }
    if (ch == '\n' || ch == '\r') {
      r++;
      c = 0;
      if (ch == '\r' && i + 1 < edit_len && edit_buf[i + 1] == '\n')
        i++;
      continue;
    }
    vga_put_at(r, c, ch);
    c++;
    if (c >= EDIT_COLS) {
      r++;
      c = 0;
    }
  }
  if (edit_cur >= edit_len) {
    cr = r;
    cc = c;
  }
  uint32_to_string((uint32_t)edit_len, num, (int)sizeof(num));
  line[n++] = 'b';
  line[n++] = 'y';
  line[n++] = 't';
  line[n++] = 'e';
  line[n++] = 's';
  line[n++] = ' ';
  for (i = 0; num[i] && n < 40; i++)
    line[n++] = num[i];
  line[n++] = ' ';
  line[n++] = ' ';
  line[n++] = 'E';
  line[n++] = 'S';
  line[n++] = 'C';
  line[n++] = '=';
  line[n++] = 's';
  line[n++] = 'a';
  line[n++] = 'l';
  line[n++] = 'v';
  line[n++] = 'a';
  line[n++] = 'r';
  line[n] = '\0';
  edit_status(line);
  if (cr >= EDIT_ROWS - 1) cr = EDIT_ROWS - 2;
  if (cc >= EDIT_COLS) cc = EDIT_COLS - 1;
  goto_xy(cr, cc);
  update_cursor();
}

static void edit_insert(char ch) {
  int i;
  if (edit_len >= EDIT_MAX - 1) return;
  for (i = edit_len; i > edit_cur; i--)
    edit_buf[i] = edit_buf[i - 1];
  edit_buf[edit_cur] = ch;
  edit_len++;
  edit_cur++;
  edit_buf[edit_len] = '\0';
  edit_dirty = 1;
}

static void edit_backspace(void) {
  int i;
  if (edit_cur <= 0) return;
  for (i = edit_cur - 1; i < edit_len; i++)
    edit_buf[i] = edit_buf[i + 1];
  edit_cur--;
  edit_len--;
  edit_buf[edit_len] = '\0';
  edit_dirty = 1;
}

static void edit_move_line(int dir) {
  int i = edit_cur;
  int col = 0;
  while (i > 0 && edit_buf[i - 1] != '\n') {
    i--;
    col++;
  }
  if (dir < 0) {
    if (i == 0) {
      edit_cur = 0;
      return;
    }
    i--;
    while (i > 0 && edit_buf[i - 1] != '\n') i--;
  } else {
    while (i < edit_len && edit_buf[i] != '\n') i++;
    if (i < edit_len) i++;
    else {
      edit_cur = edit_len;
      return;
    }
  }
  while (col > 0 && i < edit_len && edit_buf[i] != '\n') {
    i++;
    col--;
  }
  edit_cur = i;
}

int cmd_edit(int argc, char** argv) {
  int n;
  int existed;
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
    while (path[i] && i < 63) {
      edit_path[i] = path[i];
      i++;
    }
    edit_path[i] = '\0';
  }

  if (!drives_is_mounted(path_letter(edit_path))) {
    puts_k("Unidade nao montada. Use: mount D:\n");
    return 1;
  }

  n = file_load(edit_path, edit_buf, EDIT_MAX);
  existed = (n >= 0);
  if (n < 0) {
    edit_buf[0] = '\0';
    edit_len = 0;
  } else {
    edit_len = n;
    edit_buf[edit_len] = '\0';
  }
  edit_cur = edit_len;
  edit_dirty = 0;

  edit_draw();
  for (;;) {
    int k = shell_get_char();
    if (k == 0) continue;
    if (k == 27) break;
    if (k == KEY_LEFT) {
      if (edit_cur > 0) edit_cur--;
    } else if (k == KEY_RIGHT) {
      if (edit_cur < edit_len) edit_cur++;
    } else if (k == KEY_UP) {
      edit_move_line(-1);
    } else if (k == KEY_DOWN) {
      edit_move_line(1);
    } else if (k == KEY_HOME) {
      while (edit_cur > 0 && edit_buf[edit_cur - 1] != '\n') edit_cur--;
    } else if (k == KEY_END) {
      while (edit_cur < edit_len && edit_buf[edit_cur] != '\n') edit_cur++;
    } else if (k == '\b') {
      edit_backspace();
    } else if (k == '\n' || k == '\r') {
      edit_insert('\n');
    } else if (k >= 32 && k < 127) {
      edit_insert((char)k);
    } else {
      continue;
    }
    edit_draw();
  }

  edit_buf[edit_len] = '\0';
  if (!edit_dirty && !existed) {
    clear_screen();
    puts_k("nada gravado\n");
    return 0;
  }
  if (file_save(edit_path, edit_buf, edit_len) != 0) {
    clear_screen();
    puts_k("Erro ao salvar ");
    puts_k(edit_path);
    puts_k("\n");
    return 1;
  }
  clear_screen();
  puts_k("Salvo: ");
  puts_k(edit_path);
  puts_k("\n");
  return 0;
}
