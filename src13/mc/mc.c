/* mc.c - Midnight Commander do SODOS: tela, menu (F9), linha de comando e
 * teclas. Os paineis estao em panel.c; copiar/mover/apagar em ops.c; F3 em view.c.
 *
 *   mc [pasta-esquerda [pasta-direita]]
 *   mc auto         desenha uma vez e sai (teste) */
#include "mc.h"

static char cmd[200];
static int cmdlen;
static int confirm_exit = 1;

void fmt_size(char* out, unsigned v) {
  char t[16];
  int n = 0, d = 0, i;
  do {
    if (d && d % 3 == 0) t[n++] = '.';
    t[n++] = (char)('0' + v % 10u);
    v /= 10u;
    d++;
  } while (v);
  for (i = 0; i < n; i++) out[i] = t[n - 1 - i];
  out[n] = 0;
}

/* ---------- barra de menu, linha de comando e teclas ---------- */

static const char* menu_title[5] = { "Esquerdo", "Arquivo", "Comando", "Opcoes", "Direito" };
static const int menu_x[5] = { 2, 14, 25, 36, 46 };

static void draw_menubar(int open, int sel) {
  int i;
  scr_fill(0, 0, 1, SCR_W, ' ', C_MENU);
  for (i = 0; i < 5; i++) {
    int on = open && i == sel;
    int c = on ? C_MENUSEL : C_MENU;
    scr_put(0, menu_x[i] - 1, ' ', c);
    scr_text(0, menu_x[i], menu_title[i], c);
    scr_put(0, menu_x[i], (unsigned char)menu_title[i][0], on ? C_MENUSELHOT : C_MENUHOT);
    scr_put(0, menu_x[i] + (int)strlen(menu_title[i]), ' ', c);
  }
  scr_text(0, SCR_W - 13, "SODOS MC 1.0", C_MENU);
}

static void draw_cmdline(void) {
  char pr[PATH_MAX_MC + 2];
  int pl, start = 0, x, i;
  sprintf(pr, "%s>", pan[act].path);
  pl = (int)strlen(pr);
  if (pl + cmdlen > SCR_W - 1) start = pl + cmdlen - (SCR_W - 1);
  scr_fill(23, 0, 1, SCR_W, ' ', C_CMD);
  x = 0;
  for (i = start; i < pl + cmdlen; i++) {
    int c = i < pl ? (unsigned char)pr[i] : (unsigned char)cmd[i - pl];
    scr_put(23, x++, c, C_CMD);
  }
  k_cursor(23, x);
}

static void draw_keybar(void) {
  static const char* lbl[10] = {
    "Ajuda", "Unid.", "Ver", "Editar", "Copiar", "RenMov", "CrPast", "Apagar", "Menu", "Sair"
  };
  int i, x = 0;
  for (i = 0; i < 10; i++) {
    char num[4];
    int w = i == 9 ? SCR_W - x - 2 : 7;
    sprintf(num, "%d", i + 1);
    scr_text(24, x, num, C_KEYNUM);
    x += (int)strlen(num);
    scr_textw(24, x, lbl[i], w, C_KEYLBL);
    x += w;
  }
}

void draw_all(void) {
  draw_menubar(0, 0);
  panel_draw(&pan[0], act == 0);
  panel_draw(&pan[1], act == 1);
  draw_cmdline();
  draw_keybar();
}

/* ---------- rodar comandos ---------- */

static void wait_key(void) {
  k_puts("\nTecle algo para voltar ao MC...");
  k_getc();
}

/* Roda uma linha do prompt na tela normal, como o mc faz com o Enter. */
static void run_line(const char* line, int pause) {
  char buf[256];
  k_cursor(0, 0);
  k_clear();
  k_chdir(pan[act].path);
  k_puts(pan[act].path);
  k_puts(">");
  k_puts(line);
  k_putc('\n');
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  k_shell(buf);
  if (pause) wait_key();
  scr_invalidate();
  panel_reload(&pan[0]);
  panel_reload(&pan[1]);
}

static int run_internal(char* line) {
  char* a = line;
  while (*a == ' ') a++;
  /* "U:" troca a unidade do painel; "cd pasta" troca a pasta */
  if (a[0] && a[1] == ':' && (a[2] == 0 || (a[2] == '\\' && a[3] == 0))) {
    panel_chdir(&pan[act], a);
    return 1;
  }
  if ((a[0] == 'c' || a[0] == 'C') && (a[1] == 'd' || a[1] == 'D') && (a[2] == ' ' || a[2] == 0)) {
    char t[PATH_MAX_MC];
    a += 2;
    while (*a == ' ') a++;
    if (!*a) return 1;
    if (strcmp(a, "..") == 0) {
      panel_up(&pan[act]);
      return 1;
    }
    if (a[0] && a[1] == ':') strncpy(t, a, sizeof(t) - 1);
    else if (a[0] == '\\') sprintf(t, "%c:%s", pan[act].path[0], a);
    else if (path_join(t, sizeof(t), pan[act].path, a) != 0) return 1;
    t[sizeof(t) - 1] = 0;
    {
      struct k_dirent one;
      if (k_dir(t, &one, 1) < 0) dlg_msg("cd", "Pasta nao encontrada.", 1);
      else panel_chdir(&pan[act], t);
    }
    return 1;
  }
  return 0;
}

static void exec_cmdline(void) {
  if (!cmdlen) return;
  if (!run_internal(cmd)) run_line(cmd, 1);
  cmdlen = 0;
  cmd[0] = 0;
}

/* ---------- acoes ---------- */

enum {
  A_NONE, A_HELP, A_DRIVE, A_VIEW, A_EDIT, A_COPY, A_MOVE, A_MKDIR, A_DELETE, A_MENU, A_QUIT,
  A_MARK, A_MARKGRP, A_UNMARKGRP, A_INVERT, A_REREAD, A_SORT_NAME, A_SORT_EXT, A_SORT_SIZE,
  A_SORT_TIME, A_SWAP, A_SAME, A_RUN, A_CONFIRM, A_ABOUT
};

static void do_action(int a, int pi);

static void choose_drive(int pi) {
  const char* items[26];
  char text[26][24];
  char letters[26];
  int n = 0, i, cur = 0, r;
  for (i = 0; i < 26; i++) {
    char root[4];
    struct k_dirent one;
    unsigned t, f;
    root[0] = (char)('A' + i);
    root[1] = ':';
    root[2] = '\\';
    root[3] = 0;
    if (k_dir(root, &one, 1) < 0) continue;
    if (k_dfree(root[0], &t, &f) == 0) {
      char s[16];
      fmt_size(s, f / 1024u);
      sprintf(text[n], "%c:  %s MB livres", root[0], s);
    } else {
      sprintf(text[n], "%c:  (RAM)", root[0]);
    }
    if (root[0] == pan[pi].path[0]) cur = n;
    letters[n] = root[0];
    items[n] = text[n];
    n++;
  }
  if (!n) return;
  r = dlg_list(pi == 0 ? "Unidade (esquerdo)" : "Unidade (direito)", items, n, cur);
  if (r >= 0) {
    char d[4];
    d[0] = letters[r];
    d[1] = ':';
    d[2] = '\\';
    d[3] = 0;
    panel_chdir(&pan[pi], d);
  }
}

static void show_help(void) {
  dlg_msg("Ajuda",
          "Setas, PgUp/PgDn, Home/End   movem o cursor\n"
          "Tab                          troca de painel\n"
          "Enter          entra na pasta, roda .EXE/.BAT ou\n"
          "               executa a linha de comando\n"
          "Esquerda / Direita           sobe / entra na pasta\n"
          "Insert         marca o arquivo\n"
          "+  -  *        marca grupo, desmarca, inverte\n"
          "F2 unidade   F3 ver      F4 editar   F5 copiar\n"
          "F6 mover     F7 cr.pasta F8 apagar   F9 menu\n"
          "F10 sair\n"
          "\n"
          "Na linha de comando: digite e Enter (U: ou cd\n"
          "pasta trocam a pasta do painel).",
          0);
}

static void edit_file(void) {
  struct ent* e = panel_cur(&pan[act]);
  char path[PATH_MAX_MC], line[PATH_MAX_MC + 8];
  int sz;
  if (!e || e->dir) return;
  if (path_join(path, sizeof(path), pan[act].path, e->name) != 0) return;
  sz = k_fsize(path);
  if (sz > 2048) {
    dlg_msg("Editar", "O editor do SODOS abre ate 2 KB.\nUse F3 para ver o arquivo.", 1);
    return;
  }
  sprintf(line, "edit %s", path);
  run_line(line, 0);
}

static void enter_key(void) {
  struct panel* p = &pan[act];
  struct ent* e = panel_cur(p);
  char path[PATH_MAX_MC];
  if (cmdlen) {
    exec_cmdline();
    return;
  }
  if (!e) return;
  if (e->dir) {
    panel_enter_dir(p, e->name);
    return;
  }
  if (path_join(path, sizeof(path), p->path, e->name) != 0) return;
  if (is_exec(e->name)) run_line(path, 1);
  else view_file(path);
}

static void mark_group(int on) {
  char pat[40];
  strcpy(pat, "*.*");
  if (!dlg_input(on ? "Marcar grupo" : "Desmarcar grupo", "Arquivos que casam com:", pat, sizeof(pat))) return;
  if (pat[0]) panel_mark_pattern(&pan[act], pat, on);
}

/* ---------- menu F9 ---------- */

struct mitem {
  const char* text;
  const char* key;
  int act;
};

static const struct mitem m_panel[] = {
  { "Unidade...", "F2", A_DRIVE },
  { "Reler", "", A_REREAD },
  { "", "", A_NONE },
  { "Ordenar por nome", "", A_SORT_NAME },
  { "Ordenar por extensao", "", A_SORT_EXT },
  { "Ordenar por tamanho", "", A_SORT_SIZE },
  { "Ordenar por data", "", A_SORT_TIME },
};
static const struct mitem m_file[] = {
  { "Ver", "F3", A_VIEW },
  { "Editar", "F4", A_EDIT },
  { "Copiar", "F5", A_COPY },
  { "Mover/renomear", "F6", A_MOVE },
  { "Criar pasta", "F7", A_MKDIR },
  { "Apagar", "F8", A_DELETE },
  { "", "", A_NONE },
  { "Marcar", "Ins", A_MARK },
  { "Marcar grupo", "+", A_MARKGRP },
  { "Desmarcar grupo", "-", A_UNMARKGRP },
  { "Inverter marcas", "*", A_INVERT },
  { "", "", A_NONE },
  { "Sair", "F10", A_QUIT },
};
static const struct mitem m_cmd[] = {
  { "Executar comando...", "", A_RUN },
  { "Trocar os paineis", "", A_SWAP },
  { "Outro painel = este", "", A_SAME },
};
static const struct mitem m_opt[] = {
  { "Confirmar ao sair", "", A_CONFIRM },
  { "Sobre o MC...", "", A_ABOUT },
  { "Ajuda", "F1", A_HELP },
};

static const struct mitem* menu_items(int m, int* n) {
  if (m == 0 || m == 4) {
    *n = (int)(sizeof(m_panel) / sizeof(m_panel[0]));
    return m_panel;
  }
  if (m == 1) {
    *n = (int)(sizeof(m_file) / sizeof(m_file[0]));
    return m_file;
  }
  if (m == 2) {
    *n = (int)(sizeof(m_cmd) / sizeof(m_cmd[0]));
    return m_cmd;
  }
  *n = (int)(sizeof(m_opt) / sizeof(m_opt[0]));
  return m_opt;
}

static void draw_menu(int m, int sel) {
  int n, i, w = 0, x, y = 1;
  const struct mitem* it = menu_items(m, &n);
  for (i = 0; i < n; i++) {
    int l = (int)strlen(it[i].text) + (int)strlen(it[i].key) + 4;
    if (l > w) w = l;
  }
  w += 4;
  x = menu_x[m] - 1;
  if (x + w > SCR_W - 2) x = SCR_W - 2 - w;
  scr_fill(y, x, n + 2, w, ' ', C_MENU);
  scr_box(y, x, n + 2, w, C_MENU, 0);
  scr_shadow(y, x, n + 2, w);
  for (i = 0; i < n; i++) {
    int c = i == sel ? C_MENUSEL : C_MENU;
    if (!it[i].text[0]) {
      int k;
      scr_put(y + 1 + i, x, 0xC3, C_MENU);
      for (k = 1; k < w - 1; k++) scr_put(y + 1 + i, x + k, 0xC4, C_MENU);
      scr_put(y + 1 + i, x + w - 1, 0xB4, C_MENU);
      continue;
    }
    scr_fill(y + 1 + i, x + 1, 1, w - 2, ' ', c);
    scr_text(y + 1 + i, x + 2, it[i].text, c);
    scr_put(y + 1 + i, x + 2, (unsigned char)it[i].text[0], i == sel ? C_MENUSELHOT : C_MENUHOT);
    scr_text(y + 1 + i, x + w - 2 - (int)strlen(it[i].key), it[i].key, c);
    if (it[i].act == A_CONFIRM) scr_text(y + 1 + i, x + w - 5, confirm_exit ? "[x]" : "[ ]", c);
    if (m == 0 || m == 4) {
      int s = it[i].act - A_SORT_NAME;
      if (s >= 0 && s <= 3 && pan[m == 0 ? 0 : 1].sort == s) scr_put(y + 1 + i, x + 1, 0x07, c);
    }
  }
}

static void run_menu(void) {
  int m = act == 0 ? 0 : 4, sel = 0, n, k;
  const struct mitem* it;
  k_cursor(-1, 0);
  for (;;) {
    it = menu_items(m, &n);
    while (!it[sel].text[0]) sel++;
    draw_all();
    draw_menubar(1, m);
    draw_menu(m, sel);
    k_cursor(-1, 0);
    k = scr_key();
    if (k == K_ESC || k == K_F(9) || k == K_F(10) || k == K_EOF) break;
    if (k == K_LEFT) {
      m = (m + 4) % 5;
      sel = 0;
    } else if (k == K_RIGHT) {
      m = (m + 1) % 5;
      sel = 0;
    } else if (k == K_UP) {
      do sel = (sel + n - 1) % n; while (!it[sel].text[0]);
    } else if (k == K_DOWN) {
      do sel = (sel + 1) % n; while (!it[sel].text[0]);
    } else if (k == K_HOME) {
      sel = 0;
    } else if (k == K_END) {
      sel = n - 1;
    } else if (k == K_ENTER) {
      int a = it[sel].act;
      draw_all();
      do_action(a, m == 0 ? 0 : (m == 4 ? 1 : act));
      return;
    } else if (k < 0x100) {
      int i;
      for (i = 0; i < n; i++)
        if (it[i].text[0] && scr_cp850(k) == scr_cp850((unsigned char)it[i].text[0])) {
          draw_all();
          do_action(it[i].act, m == 0 ? 0 : (m == 4 ? 1 : act));
          return;
        }
    }
  }
  draw_all();
}

static int quit_asked;

static void do_action(int a, int pi) {
  struct panel* p = &pan[act];
  struct ent* e = panel_cur(p);
  char path[PATH_MAX_MC];
  int i;
  switch (a) {
  case A_HELP: show_help(); break;
  case A_DRIVE: choose_drive(pi); break;
  case A_VIEW:
    if (e && !e->dir && path_join(path, sizeof(path), p->path, e->name) == 0) view_file(path);
    break;
  case A_EDIT: edit_file(); break;
  case A_COPY: op_copy(0); break;
  case A_MOVE: op_copy(1); break;
  case A_MKDIR: op_mkdir(); break;
  case A_DELETE: op_delete(); break;
  case A_MENU: run_menu(); break;
  case A_QUIT:
    if (!confirm_exit || dlg_yesno("Sair", "Sair do Midnight Commander?", 0)) quit_asked = 1;
    break;
  case A_MARK:
    if (e && !e->up) e->mark = (char)!e->mark;
    panel_key_move(p, K_DOWN);
    break;
  case A_MARKGRP: mark_group(1); break;
  case A_UNMARKGRP: mark_group(0); break;
  case A_INVERT:
    for (i = 0; i < p->n; i++)
      if (!p->e[i].dir) p->e[i].mark = (char)!p->e[i].mark;
    break;
  case A_REREAD: panel_reload(&pan[pi]); break;
  case A_SORT_NAME:
  case A_SORT_EXT:
  case A_SORT_SIZE:
  case A_SORT_TIME:
    pan[pi].sort = a - A_SORT_NAME;
    panel_reload(&pan[pi]);
    break;
  case A_SWAP: {
    char t[PATH_MAX_MC];
    strcpy(t, pan[0].path);
    panel_chdir(&pan[0], pan[1].path);
    panel_chdir(&pan[1], t);
    break;
  }
  case A_SAME: panel_chdir(&pan[1 - act], p->path); break;
  case A_RUN: {
    char line[200];
    line[0] = 0;
    if (dlg_input("Executar", "Comando:", line, sizeof(line)) && line[0] && !run_internal(line))
      run_line(line, 1);
    break;
  }
  case A_CONFIRM: confirm_exit = !confirm_exit; break;
  case A_ABOUT:
    dlg_msg("Sobre",
            "Midnight Commander do SODOS 1.0\n"
            "\n"
            "Gerenciador de arquivos de dois paineis no estilo\n"
            "do GNU Midnight Commander, feito para o SODOS.",
            0);
    break;
  }
  draw_all();
}

/* ---------- principal ---------- */

static int handle_key(int k) {
  struct panel* p = &pan[act];
  if (k == K_EOF) return 0;
  if (k == K_F(1)) do_action(A_HELP, act);
  else if (k == K_F(2)) do_action(A_DRIVE, act);
  else if (k == K_F(3)) do_action(A_VIEW, act);
  else if (k == K_F(4)) do_action(A_EDIT, act);
  else if (k == K_F(5)) do_action(A_COPY, act);
  else if (k == K_F(6)) do_action(A_MOVE, act);
  else if (k == K_F(7)) do_action(A_MKDIR, act);
  else if (k == K_F(8) || (k == K_DEL && !cmdlen)) do_action(A_DELETE, act);
  else if (k == K_F(9)) do_action(A_MENU, act);
  else if (k == K_F(10)) do_action(A_QUIT, act);
  else if (k == K_TAB) act = 1 - act;
  else if (k == K_INS) do_action(A_MARK, act);
  else if (k == K_UP || k == K_DOWN || k == K_PGUP || k == K_PGDN || k == K_HOME || k == K_END)
    panel_key_move(p, k);
  else if (k == K_ENTER) enter_key();
  else if (k == K_LEFT && !cmdlen) panel_up(p);
  else if (k == K_RIGHT && !cmdlen) {
    struct ent* e = panel_cur(p);
    if (e && e->dir) panel_enter_dir(p, e->name);
  } else if (k == K_ESC) {
    cmdlen = 0;
    cmd[0] = 0;
  } else if (k == K_BS) {
    if (cmdlen) cmd[--cmdlen] = 0;
  } else if (!cmdlen && k == '+') do_action(A_MARKGRP, act);
  else if (!cmdlen && k == '-') do_action(A_UNMARKGRP, act);
  else if (!cmdlen && k == '*') do_action(A_INVERT, act);
  else if (k >= 32 && k < 256 && k != 127 && cmdlen < (int)sizeof(cmd) - 1) {
    cmd[cmdlen++] = (char)k;
    cmd[cmdlen] = 0;
  }
  return !quit_asked;
}

int main(int argc, char** argv) {
  char cwd[PATH_MAX_MC];
  int auto_quit = 0, i, np = 0;
  const char* paths[2];
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "auto") == 0) auto_quit = 1;
    else if (np < 2) paths[np++] = argv[i];
  }
  if (scr_init() != 0) {
    printf("MC precisa da tela de texto: rode fora do WIN.\n");
    return 1;
  }
  today = k_date();
  k_pwd(cwd, sizeof(cwd));
  panel_init(&pan[0], 0, np > 0 ? paths[0] : cwd);
  panel_init(&pan[1], 40, np > 1 ? paths[1] : (np > 0 ? paths[0] : cwd));
  if (!pan[0].e || !pan[1].e) {
    printf("MC: sem memoria\n");
    return 1;
  }
  act = 0;
  draw_all();
  if (auto_quit) {
    scr_flush();
    return 0;
  }
  while (handle_key(scr_key())) draw_all();
  k_chdir(pan[act].path);
  scr_done();
  return 0;
}
