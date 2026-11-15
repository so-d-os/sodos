/* explorer.c - gerenciador de arquivos do SODOS (EXPLORER.EXE).
 *
 * Navegacao entre pastas e unidades, no estilo do Windows:
 *   - barra com Voltar / Acima / Atualizar e campo de caminho (Enter vai)
 *   - coluna esquerda com unidades montadas (k_drive_mask) e atalhos
 *   - lista de arquivos com colunas Nome / Tamanho / Data
 *   - duplo clique (ou Enter) abre pasta, executavel ou mostra informacao
 *   - Nova pasta, Excluir (com confirmacao), F5, roda do rato, teclado
 *   - botao direito num item: Abrir / Criar atalho (.EXE) / Excluir; num
 *     espaco vazio: Nova pasta / Atualizar
 *   - campo de endereco: clique marca a linha toda, digitar substitui
 */
#include <stdio.h>
#include <string.h>
#include <gui.h>
#include <widgets.h>

#define MAXENT  512
#define MAXHIST 32
#define PATHMAX 160

static struct k_dirent g_ent[MAXENT];
static int g_idx[MAXENT];
static int g_n;

static char g_path[PATHMAX];
static char g_hist[MAXHIST][PATHMAX];
static int g_hn;
static int g_hpos;

static int g_sel;
static int g_top;
static int g_sort = 0;      /* 0 nome, 1 tamanho, 2 data */
static int g_confirm_del;

static gui_edit_t g_edit;
static char g_status[200];


/* debounce de DOWNs repetidos (o clique vindo da API HTTP as vezes chega
 * duas vezes no mesmo instante; um duplo clique real tem >50ms) */
static char g_rowid[MAXENT];
/* geometria da area de linhas (preenchida no desenho, usada no clique) */
static int g_lx, g_ly, g_lw, g_lh, g_rows;
static int g_mx, g_my, g_down;

/* ids de foco/clique */
static const char id_back, id_up, id_ref, id_path, id_go;
static const char id_open, id_newdir, id_del, id_list, id_drives;
static const char id_sortname, id_sortsize, id_sortdate;

static const wg_theme_t* th;
/* 1 = "visao de pasta" (EXPLORER /P caminho, clique numa pasta do desktop):
 * so a lista e o endereco no topo; o titulo da janela e o caminho */
static int g_folder;

/* menu de contexto (botao direito) */
#define CTX_OPEN   1
#define CTX_LINK   2
#define CTX_DEL    3
#define CTX_NEWDIR 4
#define CTX_REF    5
#define CTX_REN    6
static int g_ctx_n;          /* 0 = fechado */
static int g_ctx_item[6];
/* Renomear: linha em edicao (-1 = nenhuma) e o campo sobre o nome */
static int g_ren = -1;
static gui_edit_t g_ren_edit;
static int g_ren_x, g_ren_y, g_ren_w, g_ren_h;
static int g_ctx_x, g_ctx_y, g_ctx_w, g_ctx_ih;
/* pergunta "Excluir X?" */
static int g_ask_del;
static int g_ask_yes[4], g_ask_no[4];   /* x, y, w, h dos botoes */

/* ------------------------------------------------------------- utilidades */
static void set_status(const char* s) {
  int i = 0;
  while (s[i] && i < (int)sizeof(g_status) - 1) {
    g_status[i] = s[i];
    i++;
  }
  g_status[i] = 0;
}

static int ci_cmp(const char* a, const char* b) {
  int i = 0;
  for (;;) {
    int ca = (unsigned char)a[i];
    int cb = (unsigned char)b[i];
    if (ca >= 'a' && ca <= 'z') ca -= 32;
    if (cb >= 'a' && cb <= 'z') cb -= 32;
    if (ca != cb) return ca - cb;
    if (!ca) return 0;
    i++;
  }
}

static void copy_str(char* d, const char* s, int max) {
  int i = 0;
  while (s[i] && i < max - 1) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

static void cat_str(char* d, const char* s, int max) {
  int n = (int)strlen(d);
  copy_str(d + n, s, max - n);
}

static int is_exe(const char* n) {
  int l = (int)strlen(n);
  if (l < 5) return 0;
  return n[l - 4] == '.' && (n[l - 3] == 'E' || n[l - 3] == 'e') &&
         (n[l - 2] == 'X' || n[l - 2] == 'e') &&
         (n[l - 1] == 'E' || n[l - 1] == 'e');
}

static int is_bat(const char* n) {
  int l = (int)strlen(n);
  if (l < 5) return 0;
  return n[l - 4] == '.' && (n[l - 3] == 'B' || n[l - 3] == 'b') &&
         (n[l - 2] == 'A' || n[l - 2] == 'a') &&
         (n[l - 1] == 'T' || n[l - 1] == 't');
}

/* "1,2 MB" / "12 KB" / "345 B" */
static void fmt_size(unsigned sz, char* out, int max) {
  if (sz >= 1024u * 1024u)
    snprintf(out, max, "%u.%u MB", sz / (1024u * 1024u),
             (sz / (1024u * 102u)) % 10u);
  else if (sz >= 1024u)
    snprintf(out, max, "%u KB", (sz + 1023u) / 1024u);
  else
    snprintf(out, max, "%u B", sz);
}

static void fmt_date(unsigned short date, unsigned short time, char* out,
                     int max) {
  int d, m, y, hh, mm;
  if (!date) {
    copy_str(out, "-", max);
    return;
  }
  d = date & 31;
  m = (date >> 5) & 15;
  y = 1980 + (date >> 9);
  hh = time >> 11;
  mm = (time >> 5) & 63;
  snprintf(out, max, "%02d/%02d/%04d %02d:%02d", d, m, y, hh, mm);
}

/* ------------------------------------------------------------- navegacao */
static int ends_with_dir(char* p) {
  int l = (int)strlen(p);
  return l > 0 && p[l - 1] == '\\';
}

static void path_normalize(char* p) {
  int i, j = 0;
  char tmp[PATHMAX];
  /* maiusculas e barras invertidas */
  for (i = 0; p[i] && j < PATHMAX - 1; i++) {
    char c = p[i];
    if (c == '/') c = '\\';
    if (c >= 'a' && c <= 'z') c -= 32;
    tmp[j++] = c;
  }
  tmp[j] = 0;
  /* colapsa barras duplicadas */
  j = 0;
  for (i = 0; tmp[i] && j < PATHMAX - 1; i++) {
    if (tmp[i] == '\\' && j > 0 && tmp[j - 1] == '\\') continue;
    p[j++] = tmp[i];
  }
  p[j] = 0;
  if (j > 0 && p[j - 1] != '\\') {
    p[j] = '\\';
    p[j + 1] = 0;
  }
}

static void path_parent(char* p) {
  int l = (int)strlen(p);
  if (l > 3) {
    l--;
    if (l > 0 && p[l] == '\\') l--;
    while (l > 2 && p[l] != '\\') l--;
    if (l >= 2) {
      p[l + 1] = 0;
      if (!ends_with_dir(p)) {
        p[l + 1] = '\\';
        p[l + 2] = 0;
      }
    }
  }
}

static void hist_push(const char* p) {
  if (g_hpos < g_hn - 1) g_hn = g_hpos + 1;
  if (g_hn < MAXHIST) {
    copy_str(g_hist[g_hn], p, PATHMAX);
    g_hn++;
    g_hpos = g_hn - 1;
  }
}

static int dir_exists(const char* p) {
  char tmp[PATHMAX];
  char buf[8];
  int rc;
  copy_str(tmp, p, PATHMAX);
  path_normalize(tmp);
  rc = k_dirlist(tmp, buf, (int)sizeof(buf));
  return rc >= 0;
}

static void load_dir(const char* p);

static void go(const char* p, int add_hist) {
  char tmp[PATHMAX];
  copy_str(tmp, p, PATHMAX);
  path_normalize(tmp);
  if (!dir_exists(tmp)) {
    set_status("pasta nao encontrada");
    return;
  }
  copy_str(g_path, tmp, PATHMAX);
  if (add_hist) hist_push(g_path);
  gui_edit_set(&g_edit, g_path);
  load_dir(g_path);
  set_status(g_path);
  if (g_folder) gui_title(g_path);
}

/* --------------------------------------------------------------- listagem */
static int cmp_entries(int a, int b) {
  struct k_dirent* x = &g_ent[a];
  struct k_dirent* y = &g_ent[b];
  int c;
  if (x->dir != y->dir) return x->dir ? -1 : 1;
  if (g_sort == 1) {
    if (x->size != y->size) return x->size > y->size ? -1 : 1;
  } else if (g_sort == 2) {
    if (x->date != y->date) return x->date > y->date ? -1 : 1;
  }
  c = ci_cmp(x->name, y->name);
  return c;
}

static void sort_entries(void) {
  int i, j;
  for (i = 1; i < g_n; i++) {
    int v = g_idx[i];
    j = i - 1;
    while (j >= 0 && cmp_entries(g_idx[j], v) > 0) {
      g_idx[j + 1] = g_idx[j];
      j--;
    }
    g_idx[j + 1] = v;
  }
}

static void load_dir(const char* p) {
  int i;
  g_n = k_dir(p, g_ent, MAXENT);
  if (g_n < 0) {
    g_n = 0;
    set_status("erro ao listar a pasta");
  }
  for (i = 0; i < g_n; i++) g_idx[i] = i;
  sort_entries();
  g_sel = -1;
  g_top = 0;
  g_confirm_del = 0;
  {
    char b[120];
    snprintf(b, sizeof(b), "%d item(ns) em %s", g_n, p);
    set_status(b);
  }
}

static struct k_dirent* sel_ent(void) {
  if (g_sel < 0 || g_sel >= g_n) return 0;
  return &g_ent[g_idx[g_sel]];
}

/* Abre a pasta ou executa o arquivo selecionado. */
static void open_sel(void) {
  struct k_dirent* e = sel_ent();
  char full[PATHMAX];
  if (!e) {
    set_status("nada selecionado");
    return;
  }
  if (e->dir) {
    copy_str(full, g_path, PATHMAX);
    cat_str(full, e->name, PATHMAX);
    go(full, 1);
    return;
  }
  if (is_exe(e->name) || is_bat(e->name)) {
    char full2[PATHMAX];
    int rc;
    copy_str(full2, g_path, PATHMAX);
    cat_str(full2, e->name, PATHMAX);
    /* .EXE abre na propria janela do WIN, como pelo desktop: k_exec aqui
     * rodava o programa dentro da tarefa do Explorer (o Firefox nao abria e
     * o Explorer ficava preso ate ele sair). .BAT e caminho que nao cabe no
     * pedido do WIN (63 caracteres) continuam pelo k_exec. */
    if (is_exe(e->name) && strlen(full2) < 63 && k_win_launch(full2) == 0) {
      char b[160];
      snprintf(b, sizeof(b), "abrindo %s", full2);
      set_status(b);
      return;
    }
    set_status("executando...");
    gui_present();
    rc = k_exec(full2, 0, 0);
    {
      char b[160];
      snprintf(b, sizeof(b), "%s terminou (codigo %d)", e->name, rc);
      set_status(b);
    }
    load_dir(g_path);
    return;
  }
  {
    char b[200];
    char sz[32];
    char dt[32];
    fmt_size(e->size, sz, sizeof(sz));
    fmt_date(e->date, e->time, dt, sizeof(dt));
    snprintf(b, sizeof(b), "%s  %s  %s", e->name, sz, dt);
    set_status(b);
  }
}

/* Seleciona a entrada com esse nome (sem diferenciar maiusculas). */
static void select_name(const char* name) {
  int i;
  for (i = 0; i < g_n; i++)
    if (ci_cmp(g_ent[g_idx[i]].name, name) == 0) {
      g_sel = i;
      return;
    }
}

/* Poe o nome do item selecionado em edicao (todo marcado). */
static void start_rename(void) {
  struct k_dirent* e = sel_ent();
  if (!e) return;
  g_ren = g_sel;
  gui_edit_set(&g_ren_edit, e->name);
  gui_edit_select_all(&g_ren_edit);
  g_ren_w = 0;
}

static void cancel_rename(void) { g_ren = -1; }

/* Enter: grava o novo nome. Nao substitui arquivo que ja exista. */
static void commit_rename(void) {
  struct k_dirent* e;
  char nm[64];
  char from[PATHMAX];
  char to[PATHMAX];
  int i, n = 0, rc;
  if (g_ren < 0 || g_ren >= g_n) {
    g_ren = -1;
    return;
  }
  e = &g_ent[g_idx[g_ren]];
  g_ren = -1;
  /* sem espacos nas pontas */
  for (i = 0; g_ren_edit.text[i] == ' '; i++) { }
  for (; g_ren_edit.text[i] && n < (int)sizeof(nm) - 1; i++) nm[n++] = g_ren_edit.text[i];
  while (n > 0 && (nm[n - 1] == ' ' || nm[n - 1] == '.')) n--;
  nm[n] = 0;
  if (!n || strcmp(nm, e->name) == 0) {
    set_status("nome mantido");
    return;
  }
  if (n > 31) {
    set_status("nome longo demais (max 31)");
    return;
  }
  for (i = 0; i < n; i++) {
    char c = nm[i];
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|') {
      set_status("nome nao pode ter \\ / : * ? \" < > |");
      return;
    }
  }
  copy_str(from, g_path, PATHMAX);
  cat_str(from, e->name, PATHMAX);
  copy_str(to, g_path, PATHMAX);
  cat_str(to, nm, PATHMAX);
  /* outro item com esse nome? (so mudar maiusculas/minusculas pode) */
  if (ci_cmp(nm, e->name) != 0 && (k_fsize(to) >= 0 || dir_exists(to))) {
    set_status("ja existe um item com esse nome");
    return;
  }
  rc = k_frename(from, to);
  if (rc != 0) {
    char b[120];
    snprintf(b, sizeof(b), "nao foi possivel renomear (erro %d)", rc);
    set_status(b);
    return;
  }
  load_dir(g_path);
  select_name(nm);
  {
    char b[160];
    snprintf(b, sizeof(b), "renomeado para %s", nm);
    set_status(b);
  }
}

/* Nova pasta NOVAPASTA, NOVAPA~1, ... */
static void make_dir(void) {
  char full[PATHMAX];
  int i;
  for (i = 0; i < 100; i++) {
    char name[16];
    if (i == 0) copy_str(name, "NOVAPASTA", sizeof(name));
    else snprintf(name, sizeof(name), "NOVAP%03d", i);
    copy_str(full, g_path, PATHMAX);
    cat_str(full, name, PATHMAX);
    if (k_mkdir(full) == 0) {
      char b[120];
      snprintf(b, sizeof(b), "pasta %s criada: digite o nome e Enter", name);
      load_dir(g_path);
      set_status(b);
      select_name(name);
      start_rename();
      return;
    }
  }
  set_status("nao foi possivel criar a pasta");
}

static void delete_sel(void) {
  struct k_dirent* e = sel_ent();
  char full[PATHMAX];
  int rc;
  if (!e) {
    set_status("nada selecionado");
    return;
  }
  if (e->dir) {
    copy_str(full, g_path, PATHMAX);
    cat_str(full, e->name, PATHMAX);
    rc = k_rmdir(full);
  } else {
    if (!g_confirm_del) {
      g_confirm_del = 1;
      set_status("clique em Excluir de novo para confirmar");
      return;
    }
    copy_str(full, g_path, PATHMAX);
    cat_str(full, e->name, PATHMAX);
    rc = k_fdelete(full);
  }
  g_confirm_del = 0;
  if (rc == 0) set_status("excluido");
  else if (rc == -2) set_status("pasta nao esta vazia");
  else if (rc == -3) set_status("e uma pasta");
  else set_status("nao foi possivel excluir");
  load_dir(g_path);
}

/* ------------------------------------------------------------- atalhos */
static int dir_has_items(const char* p) {
  char buf[64];
  return k_dirlist(p, buf, (int)sizeof(buf)) > 0;
}

/* Pasta do desktop que o WIN mostra: D:\USER\DEFAULT\DESKTOP com itens, senao
 * a do pendrive (U:); se nenhuma existe, cria em D: (ou U: sem D:). */
static int desktop_dir(char* out, int max) {
  static const char* parts[3] = {"USER", "DEFAULT", "DESKTOP"};
  unsigned mask = k_drive_mask();
  char p[PATHMAX];
  char L;
  int i;
  if ((mask & (1u << 3)) && dir_has_items("D:\\USER\\DEFAULT\\DESKTOP\\")) {
    copy_str(out, "D:\\USER\\DEFAULT\\DESKTOP\\", max);
    return 0;
  }
  if ((mask & (1u << 20)) && dir_has_items("U:\\USER\\DEFAULT\\DESKTOP\\")) {
    copy_str(out, "U:\\USER\\DEFAULT\\DESKTOP\\", max);
    return 0;
  }
  if (mask & (1u << 3)) L = 'D';
  else if (mask & (1u << 20)) L = 'U';
  else return -1;
  p[0] = L;
  p[1] = ':';
  p[2] = '\\';
  p[3] = 0;
  for (i = 0; i < 3; i++) {
    cat_str(p, parts[i], PATHMAX);
    if (!dir_exists(p)) k_mkdir(p);
    cat_str(p, "\\", PATHMAX);
  }
  if (!dir_exists(p)) return -1;
  copy_str(out, p, max);
  return 0;
}

/* Botao direito > Criar atalho: grava NOME.LNK (texto com o caminho do .EXE)
 * na pasta do desktop. O WIN abre o .EXE ao clicar no icone. */
static void make_shortcut(void) {
  struct k_dirent* e = sel_ent();
  char target[PATHMAX];
  char dir[PATHMAX];
  char base[9];
  char lnk[PATHMAX];
  char body[PATHMAX + 4];
  int i, n, try_;
  if (!e || e->dir || !is_exe(e->name)) {
    set_status("atalho so para .EXE");
    return;
  }
  copy_str(target, g_path, PATHMAX);
  cat_str(target, e->name, PATHMAX);
  if (strlen(target) > 62) {
    set_status("caminho longo demais para atalho (max 62)");
    return;
  }
  if (desktop_dir(dir, PATHMAX) != 0) {
    set_status("nao achei nem criei USER\\DEFAULT\\DESKTOP");
    return;
  }
  /* nome 8.3: ate 8 letras do nome do programa */
  n = 0;
  for (i = 0; e->name[i] && e->name[i] != '.' && n < 8; i++) {
    char c = e->name[i];
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c == ' ') continue;
    base[n++] = c;
  }
  base[n] = 0;
  if (!n) copy_str(base, "ATALHO", sizeof(base));
  copy_str(body, target, PATHMAX);
  cat_str(body, "\r\n", sizeof(body));
  for (try_ = 0; try_ < 10; try_++) {
    char nm[16];
    char old[PATHMAX];
    int ol;
    copy_str(nm, base, sizeof(nm));
    if (try_ > 0) {
      int l = (int)strlen(nm);
      if (l > 7) l = 7;
      nm[l] = (char)('0' + try_);
      nm[l + 1] = 0;
    }
    copy_str(lnk, dir, PATHMAX);
    cat_str(lnk, nm, PATHMAX);
    cat_str(lnk, ".LNK", PATHMAX);
    if (k_fsize(lnk) < 0) break;
    /* ja existe: se aponta para o mesmo programa, nada a fazer */
    ol = k_fload(lnk, old, PATHMAX);
    if (ol > 0) {
      int k = 0;
      old[ol < PATHMAX ? ol : PATHMAX - 1] = 0;
      while (old[k] && old[k] != '\r' && old[k] != '\n') k++;
      old[k] = 0;
      if (ci_cmp(old, target) == 0) {
        char b[200];
        snprintf(b, sizeof(b), "atalho ja existe: %s", lnk);
        set_status(b);
        return;
      }
    }
  }
  if (k_fsave(lnk, body, (int)strlen(body)) < 0) {
    set_status("nao foi possivel gravar o atalho");
    return;
  }
  {
    char b[200];
    snprintf(b, sizeof(b), "atalho criado: %s", lnk);
    set_status(b);
  }
  if (ci_cmp(g_path, dir) == 0) load_dir(g_path);
}

static void ctx_open_at(int x, int y, int on_item) {
  struct k_dirent* e = sel_ent();
  g_ctx_n = 0;
  if (on_item && e) {
    g_ctx_item[g_ctx_n++] = CTX_OPEN;
    if (!e->dir && is_exe(e->name)) g_ctx_item[g_ctx_n++] = CTX_LINK;
    g_ctx_item[g_ctx_n++] = CTX_REN;
    g_ctx_item[g_ctx_n++] = CTX_DEL;
  } else {
    g_ctx_item[g_ctx_n++] = CTX_NEWDIR;
    g_ctx_item[g_ctx_n++] = CTX_REF;
  }
  g_ctx_x = x;
  g_ctx_y = y;
}

static const char* ctx_label(int it) {
  switch (it) {
    case CTX_OPEN: return "Abrir";
    case CTX_LINK: return "Criar atalho";
    case CTX_DEL: return "Excluir";
    case CTX_NEWDIR: return "Nova pasta";
    case CTX_REF: return "Atualizar";
    case CTX_REN: return "Renomear";
  }
  return "";
}

static void ctx_run(int it) {
  g_ctx_n = 0;
  if (it == CTX_OPEN) open_sel();
  else if (it == CTX_LINK) make_shortcut();
  else if (it == CTX_REN) start_rename();
  else if (it == CTX_DEL) {
    if (sel_ent()) g_ask_del = 1;
  } else if (it == CTX_NEWDIR) make_dir();
  else if (it == CTX_REF) {
    load_dir(g_path);
    set_status("lista atualizada");
  }
}

static void draw_ctx(int sw, int sh) {
  int i, ch = gui_char_h();
  int hot = -1;
  if (!g_ctx_n) return;
  g_ctx_ih = ch + 10;
  g_ctx_w = 16 * gui_char_w() + 20;
  if (g_ctx_x + g_ctx_w > sw - 2) g_ctx_x = sw - 2 - g_ctx_w;
  if (g_ctx_y + g_ctx_n * g_ctx_ih + 4 > sh - 2)
    g_ctx_y = sh - 2 - g_ctx_n * g_ctx_ih - 4;
  if (g_ctx_x < 0) g_ctx_x = 0;
  if (g_ctx_y < 0) g_ctx_y = 0;
  if (g_mx >= g_ctx_x && g_mx < g_ctx_x + g_ctx_w && g_my >= g_ctx_y + 2)
    hot = (g_my - g_ctx_y - 2) / g_ctx_ih;
  gui_fill(g_ctx_x + 3, g_ctx_y + 3, g_ctx_w, g_ctx_n * g_ctx_ih + 4,
           wg_theme_is_dark() ? 0x101014 : 0xA0A0A0);
  gui_fill(g_ctx_x, g_ctx_y, g_ctx_w, g_ctx_n * g_ctx_ih + 4, th->panel);
  gui_frame(g_ctx_x, g_ctx_y, g_ctx_w, g_ctx_n * g_ctx_ih + 4, th->face_lo);
  for (i = 0; i < g_ctx_n; i++) {
    int iy = g_ctx_y + 2 + i * g_ctx_ih;
    int on = i == hot;
    if (on) gui_fill(g_ctx_x + 2, iy, g_ctx_w - 4, g_ctx_ih, th->sel_bg);
    gui_text(g_ctx_x + 12, iy + 5, ctx_label(g_ctx_item[i]), -1,
             on ? th->sel_text : th->text, on ? th->sel_bg : th->panel,
             GUI_TEXT_NOBG);
  }
}

/* clique com o menu aberto: executa o item ou so fecha. */
static void ctx_click(int x, int y) {
  int it = -1;
  if (x >= g_ctx_x && x < g_ctx_x + g_ctx_w && y >= g_ctx_y + 2 &&
      y < g_ctx_y + 2 + g_ctx_n * g_ctx_ih)
    it = (y - g_ctx_y - 2) / g_ctx_ih;
  if (it >= 0 && it < g_ctx_n) ctx_run(g_ctx_item[it]);
  else g_ctx_n = 0;
}

static void draw_btn_rect(int* r, const char* label, int hot) {
  gui_fill(r[0], r[1], r[2], r[3], hot ? th->sel_bg : th->face);
  gui_frame(r[0], r[1], r[2], r[3], th->face_lo);
  gui_text(r[0] + (r[2] - (int)strlen(label) * gui_char_w()) / 2,
           r[1] + (r[3] - gui_char_h()) / 2, label, -1,
           hot ? th->sel_text : th->text, hot ? th->sel_bg : th->face,
           GUI_TEXT_NOBG);
}

static int in_rect(int* r, int x, int y) {
  return x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
}

static void draw_ask(int sw, int sh) {
  struct k_dirent* e = sel_ent();
  char q[200];
  int w, h, x, y, ch = gui_char_h();
  if (!g_ask_del) return;
  if (!e) {
    g_ask_del = 0;
    return;
  }
  snprintf(q, sizeof(q), "Excluir %s \"%s\"?", e->dir ? "a pasta" : "o arquivo",
           e->name);
  w = (int)strlen(q) * gui_char_w() + 40;
  if (w < 320) w = 320;
  if (w > sw - 20) w = sw - 20;
  h = ch * 2 + 70;
  x = (sw - w) / 2;
  y = (sh - h) / 2;
  gui_fill(x + 4, y + 4, w, h, wg_theme_is_dark() ? 0x101014 : 0x909090);
  gui_fill(x, y, w, h, th->panel);
  gui_frame(x, y, w, h, th->face_lo);
  gui_text_fit(x + 20, y + 16, q, w - 40, th->text, th->panel, GUI_TEXT_NOBG);
  if (e->dir)
    gui_text_fit(x + 20, y + 16 + ch + 4, "(so pastas vazias)", w - 40,
                 th->text_dim, th->panel, GUI_TEXT_NOBG);
  g_ask_yes[2] = g_ask_no[2] = 90;
  g_ask_yes[3] = g_ask_no[3] = ch + 12;
  g_ask_no[0] = x + w - 20 - 90;
  g_ask_yes[0] = g_ask_no[0] - 10 - 90;
  g_ask_yes[1] = g_ask_no[1] = y + h - ch - 12 - 14;
  draw_btn_rect(g_ask_yes, "Sim", in_rect(g_ask_yes, g_mx, g_my));
  draw_btn_rect(g_ask_no, "Nao", in_rect(g_ask_no, g_mx, g_my));
}

static void ask_answer(int yes) {
  g_ask_del = 0;
  if (!yes) {
    set_status("exclusao cancelada");
    return;
  }
  g_confirm_del = 1;   /* ja confirmado */
  delete_sel();
}

/* --------------------------------------------------------------- desenho */
static void draw_folder(int x, int y, int on_sel) {
  unsigned body = on_sel ? 0xFFE9A0 : 0xF0C860;
  unsigned dark = on_sel ? 0xC8A040 : 0xC09030;
  gui_fill(x, y + 2, 16, 3, dark);
  gui_fill(x, y + 4, 14, 9, body);
  gui_fill(x, y + 4, 14, 1, on_sel ? 0xFFFFC8 : 0xFFF0B0);
  gui_fill(x, y + 12, 14, 1, dark);
}

static void draw_file(int x, int y, int on_sel) {
  unsigned p = on_sel ? 0xFFFFFF : 0xF8F8F8;
  gui_fill(x + 2, y + 2, 11, 12, p);
  gui_fill(x + 2, y + 2, 11, 1, 0x909090);
  gui_fill(x + 2, y + 2, 1, 12, 0x909090);
  gui_fill(x + 12, y + 2, 1, 13, 0x909090);
  gui_fill(x + 2, y + 14, 11, 1, 0x909090);
  /* linhas de "texto" */
  gui_fill(x + 4, y + 5, 7, 1, 0xB0B0B0);
  gui_fill(x + 4, y + 8, 7, 1, 0xB0B0B0);
  gui_fill(x + 4, y + 11, 5, 1, 0xB0B0B0);
}

/* Uma linha da lista de arquivos. Devolve a altura usada. */
static int draw_file_list(int x, int y, int w, int h, int* out_rows) {
  int rh = wg_theme()->row_h;
  int rows = (h - 2) / rh;
  int i;
  int col_name = w - 250;
  int col_size = 90;
  int col_date = 150;
  char sz[32];
  char dt[32];
  wg_scroll_t sc;
  int changed = 0;

  if (rows < 1) rows = 1;
  *out_rows = rows;

  /* cabecalho das colunas */
  gui_fill(x, y, w, 20, th->face);
  gui_fill(x, y, w, 1, th->face_hi);
  gui_fill(x, y + 19, w, 1, th->face_lo);
  gui_text(x + 6, y + 2, "Nome", -1, th->text, th->face, GUI_TEXT_NOBG);
  gui_text(x + col_name + 6, y + 2, "Tamanho", -1, th->text, th->face,
           GUI_TEXT_NOBG);
  gui_text(x + col_name + col_size + 6, y + 2, "Data", -1, th->text,
           th->face, GUI_TEXT_NOBG);
  y += 20;
  h -= 20;
  g_lx = x;
  g_ly = y;
  g_lw = w;
  g_lh = h;
  g_rows = rows;

  gui_fill(x, y, w, h, th->field_bg);
  gui_fill(x, y, w, 1, th->face_lo);
  gui_fill(x, y, 1, h, th->face_lo);
  gui_fill(x, y + h - 1, w, 1, th->face_hi);
  gui_fill(x + w - 1, y, 1, h, th->face_hi);

  /* scrollbar se precisa */
  if (g_n > rows) {
    sc.pos = g_top;
    sc.vis = rows;
    sc.total = g_n;
    if (wg_scrollbar(&id_list, &sc, x + w - 14, y + 1, 13, h - 2)) {
      g_top = sc.pos;
      changed = 1;
    }
  }
  (void)changed;

  for (i = 0; i < rows; i++) {
    int idx = g_top + i;
    int iy = y + 1 + i * rh;
    int sel;
    struct k_dirent* e;
    if (idx >= g_n) break;
    e = &g_ent[g_idx[idx]];
    sel = (idx == g_sel);
    if (sel)
      gui_fill(x + 1, iy, w - 2, rh, th->sel_bg);
    else if (g_my >= iy && g_my < iy + rh && g_mx > x && g_mx < x + w)
      gui_fill(x + 1, iy, w - 2, rh, wg_theme_is_dark() ? 0x2E3038 : 0xE8EEF8);
    if (wg_click_area(&g_rowid[idx], x + 1, iy, w - 2, rh, 1)) {
      if (idx == g_sel) {
        open_sel();
        return 0;   /* a lista mudou: o laco nao vale mais */
      }
      g_sel = idx;
      g_confirm_del = 0;
    }

    if (e->dir) draw_folder(x + 6, iy + 2, sel);
    else draw_file(x + 6, iy + 2, sel);

    if (idx == g_ren) {
      g_ren_x = x + 26;
      g_ren_y = iy;
      g_ren_w = col_name - 30;
      g_ren_h = rh;
      if (g_ren_w < 120) g_ren_w = 120;
      gui_edit_draw(&g_ren_edit, g_ren_x, g_ren_y, g_ren_w, g_ren_h, 1);
    } else
      gui_text(x + 28, iy + 1, e->name, -1,
               sel ? th->sel_text : (e->dir ? th->text : th->field_text),
               sel ? th->sel_bg : th->field_bg, GUI_TEXT_NOBG);
    if (!e->dir) {
      fmt_size(e->size, sz, sizeof(sz));
      gui_text(x + col_name + 6, iy + 1, sz, -1,
               sel ? th->sel_text : th->text_dim,
               sel ? th->sel_bg : th->field_bg, GUI_TEXT_NOBG);
    } else {
      gui_text(x + col_name + 6, iy + 1, "<DIR>", -1,
               sel ? th->sel_text : th->text_dim,
               sel ? th->sel_bg : th->field_bg, GUI_TEXT_NOBG);
    }
    fmt_date(e->date, e->time, dt, sizeof(dt));
    gui_text(x + col_name + col_size + 6, iy + 1, dt, -1,
             sel ? th->sel_text : th->text_dim,
             sel ? th->sel_bg : th->field_bg, GUI_TEXT_NOBG);

  }
  return 0;
}

/* ------------------------------------------------------------ unidades */
static int drive_lines(char* out, int max, int* offsets, int maxoff) {
  unsigned mask = k_drive_mask();
  int i, n = 0, pos = 0;
  for (i = 0; i < 26; i++) {
    if (!(mask & (1u << i))) continue;
    if (n >= maxoff) break;
    offsets[n] = pos;
    out[pos++] = (char)('A' + i);
    out[pos++] = ':';
    out[pos++] = '\\';
    out[pos++] = 0;
    n++;
  }
  (void)max;
  return n;
}

int main(int argc, char** argv) {
  gui_event_t ev;
  int sw, sh, ch, run = 1;
  int bx, by, bw, bh;
  int lx, ly, lw, lh;
  int fx, fy, fw, fh;
  int rows = 10;
  char drives[26 * 4 + 1];
  int doff[26];
  int ndrv;
  char lastpath[PATHMAX];

  if (argc > 2 && argv[1] && (argv[1][0] == '/' || argv[1][0] == '-') &&
      (argv[1][1] == 'P' || argv[1][1] == 'p') && argv[2] && argv[2][0])
    g_folder = 1;
  if (gui_open("Explorer") != 0) {
    printf("EXPLORER: nao abriu a janela\n");
    return 1;
  }
  th = wg_theme();
  /* comeca na unidade do proprio .EXE (D:, o disco) e, se nao houver,
   * na pasta atual do shell */
  {
    int d = k_exe_drive();
    char root[4];
    if (!d) d = k_drive();
    if (!d) d = 'D';
    root[0] = (char)d;
    root[1] = ':';
    root[2] = '\\';
    root[3] = 0;
    copy_str(g_path, root, PATHMAX);
  }
  if (g_folder) {
    copy_str(g_path, argv[2], PATHMAX);
    path_normalize(g_path);
    gui_title(g_path);
  } else if (argc > 1 && argv[1] && argv[1][0])
    copy_str(g_path, argv[1], PATHMAX);
  k_pwd(lastpath, PATHMAX);
  if (!g_path[0] && lastpath[0] && lastpath[1] == ':') {
    char root[4];
    root[0] = lastpath[0];
    root[1] = ':';
    root[2] = '\\';
    root[3] = 0;
    copy_str(g_path, root, PATHMAX);
  }
  gui_edit_set(&g_edit, g_path);
  wg_focus_set(&id_list);
  hist_push(g_path);
  load_dir(g_path);

  while (run) {
    int i;
    char st[120];

    sw = gui_width();
    sh = gui_height();
    ch = gui_char_h();
    /* menu ou pergunta aberta: os controles de baixo nao veem o rato */
    if (g_ctx_n || g_ask_del || g_ren >= 0) wg_input(-1, -1, 0);
    else wg_input(g_mx, g_my, g_down);
    gui_fill(0, 0, sw, sh, th->bg);

    /* ------------------------------------------------------- barra superior */
    bx = 10;
    by = 8;
    bh = 28;
    if (wg_button(&id_back, bx, by, 96, bh, "Voltar", g_hpos > 0)) {
      if (g_hpos > 0) {
        g_hpos--;
        go(g_hist[g_hpos], 0);
      }
    }
    if (wg_button(&id_up, bx + 102, by, 96, bh, "Acima", 1)) {
      char p[PATHMAX];
      copy_str(p, g_path, PATHMAX);
      path_parent(p);
      go(p, 1);
    }
    if (wg_button(&id_ref, bx + 204, by, 106, bh, "Atualizar", 1)) {
      load_dir(g_path);
      set_status("lista atualizada");
    }
    bw = sw - (bx + 204 + 106 + 16) - 10 - 90;
    if (bw < 120) bw = 120;
    {
      /* endereco: o clique que da o foco marca a linha toda (digitar
       * substitui); ao sair sem Enter volta a mostrar a pasta atual */
      int had = wg_focus_get() == &id_path;
      wg_edit_box(&id_path, &g_edit, bx + 204 + 106 + 10, by + 1, bw, bh - 2);
      if (!had && wg_focus_get() == &id_path) gui_edit_select_all(&g_edit);
    }
    if (wg_button(&id_go, bx + 204 + 106 + 10 + bw + 8, by, 80, bh, "Ir", 1)) {
      go(g_edit.text, 1);
    }

    /* -------------------------------------------------------- coluna esquerda */
    lx = 10;
    ly = by + bh + 10;
    lw = 210;
    lh = sh - ly - (ch + 26) - 10;
    if (lh < 200) lh = 200;
    /* visao de pasta (aberta do desktop): sem unidades nem atalhos */
    if (!g_folder) {
    wg_group(lx, ly, lw, 120, "Unidades");
    ndrv = drive_lines(drives, (int)sizeof(drives), doff, 26);
    for (i = 0; i < ndrv; i++) {
      const char* nm = drives + doff[i];
      int iy = ly + 26 + i * 20;
      char root[4];
      root[0] = nm[0];
      root[1] = ':';
      root[2] = '\\';
      root[3] = 0;
      if (g_mx > lx + 10 && g_mx < lx + lw - 10 && g_my >= iy &&
          g_my < iy + 18 && g_down) {
        go(root, 1);
        g_down = 0;
      }
      if (g_path[0] == nm[0] && g_path[1] == ':')
        gui_fill(lx + 10, iy, lw - 20, 18, th->sel_bg);
      draw_folder(lx + 14, iy + 2, g_path[0] == nm[0]);
      gui_text(lx + 36, iy + 1, nm, -1,
               (g_path[0] == nm[0]) ? th->sel_text : th->text,
               (g_path[0] == nm[0]) ? th->sel_bg : th->panel,
               GUI_TEXT_NOBG);
    }

    ly += 120 + 8;
    wg_group(lx, ly, lw, 108, "Atalhos");
    {
      static const char* al[] = {"D:\\", "D:\\SODOS", "D:\\USER", "C:\\"};
      for (i = 0; i < 4; i++) {
        int iy = ly + 26 + i * 20;
        if (g_mx > lx + 10 && g_mx < lx + lw - 10 && g_my >= iy &&
            g_my < iy + 18 && g_down) {
          go(al[i], 1);
          g_down = 0;
        }
        gui_text(lx + 14, iy + 1, al[i], -1, th->text, th->panel,
                 GUI_TEXT_NOBG);
      }
    }
    }

    /* ------------------------------------------------------------- lista */
    fx = g_folder ? 10 : lx + lw + 10;
    fy = by + bh + 10;
    fw = sw - fx - 10;
    if (fw < 320) fw = 320;
    fh = sh - fy - (ch + 26) - 10 - 40;
    if (fh < 160) fh = 160;
    draw_file_list(fx, fy, fw, fh, &rows);

    /* --------------------------------------------------- botoes da direita */
    by = fy + fh + 8;
    bx = fx;
    if (wg_button(&id_open, bx, by, 110, 28, "Abrir", g_n > 0)) open_sel();
    if (wg_button(&id_newdir, bx + 116, by, 130, 28, "Nova pasta", 1))
      make_dir();
    if (wg_button(&id_del, bx + 252, by, 130, 28,
                  "Excluir", g_n > 0 && g_sel >= 0))
      g_ask_del = 1;
    if (wg_button(&id_sortname, bx + 390, by, 90, 28, "Nome", 1)) {
      g_sort = 0;
      sort_entries();
    }
    if (wg_button(&id_sortsize, bx + 486, by, 90, 28, "Tamanho", 1)) {
      g_sort = 1;
      sort_entries();
    }
    if (wg_button(&id_sortdate, bx + 582, by, 80, 28, "Data", 1)) {
      g_sort = 2;
      sort_entries();
    }

    /* -------------------------------------------------------- status bar */
    {
      int nd = 0, nf = 0;
      unsigned tot = 0;
      for (i = 0; i < g_n; i++) {
        if (g_ent[g_idx[i]].dir) nd++;
        else {
          nf++;
          tot += g_ent[g_idx[i]].size;
        }
      }
      snprintf(st, sizeof(st), "%d pasta(s), %d arquivo(s), %u KB", nd, nf,
               (tot + 1023u) / 1024u);
    }
    wg_status(0, sh - (ch + 26), sw, g_status, st);

    draw_ctx(sw, sh);
    draw_ask(sw, sh);
    gui_present();

    /* ------------------------------------------------------------ eventos */
    if (gui_wait(&ev, 120) != GUI_EV_NONE) {
      if (ev.type == GUI_EV_CLOSE) run = 0;
      else if (ev.type == GUI_EV_DOWN) {
        int rh = wg_theme()->row_h;
        g_mx = ev.x;
        g_my = ev.y;
        if (g_ren >= 0) {
          /* renomeando: clique no campo move o cursor; fora dele grava */
          if (g_ren_w > 0 && ev.x >= g_ren_x && ev.x < g_ren_x + g_ren_w &&
              ev.y >= g_ren_y && ev.y < g_ren_y + g_ren_h)
            gui_edit_click(&g_ren_edit, g_ren_x, ev.x);
          else
            commit_rename();
        } else if (g_ask_del) {
          if (in_rect(g_ask_yes, ev.x, ev.y)) ask_answer(1);
          else if (in_rect(g_ask_no, ev.x, ev.y)) ask_answer(0);
        } else if (g_ctx_n) {
          ctx_click(ev.x, ev.y);
        } else if (ev.key == 2) {
          /* botao direito: menu do item (ou do espaco vazio da lista) */
          if (ev.x > g_lx && ev.x < g_lx + g_lw && ev.y >= g_ly &&
              ev.y < g_ly + g_lh) {
            int idx = g_top + (ev.y - g_ly - 1) / rh;
            int on = idx >= 0 && idx < g_n && idx < g_top + g_rows;
            if (on) {
              g_sel = idx;
              g_confirm_del = 0;
            }
            wg_focus_set(&id_list);
            ctx_open_at(ev.x, ev.y, on);
          }
        } else {
          g_down = 1;
          /* clique fora do endereco devolve o foco a lista */
          if (wg_focus_get() == &id_path && ev.x > g_lx && ev.x < g_lx + g_lw &&
              ev.y >= g_ly && ev.y < g_ly + g_lh) {
            wg_focus_set(&id_list);
            gui_edit_set(&g_edit, g_path);
          }
        }
      } else if (ev.type == GUI_EV_UP) {
        g_mx = ev.x;
        g_my = ev.y;
        g_down = 0;
      } else if (ev.type == GUI_EV_MOVE) {
        g_mx = ev.x;
        g_my = ev.y;
      } else if (ev.type == GUI_EV_DRAG) {
        g_mx = ev.x;
        g_my = ev.y;
        g_down = 1;
      } else if (ev.type == GUI_EV_WHEEL) {
        g_top += (ev.key > 0) ? 1 : -1;
        if (g_top > g_n - rows) g_top = g_n - rows;
        if (g_top < 0) g_top = 0;
      } else if (ev.type == GUI_EV_KEY) {
        int k = ev.key;
        if (g_ren >= 0) {
          if (k == '\r' || k == '\n') commit_rename();
          else if (k == 27) {
            cancel_rename();
            set_status("renomear cancelado");
          } else
            gui_edit_key(&g_ren_edit, k);
        } else if (g_ask_del) {
          if (k == 27 || k == 'n' || k == 'N') ask_answer(0);
          else if (k == '\r' || k == '\n' || k == 's' || k == 'S') ask_answer(1);
        } else if (g_ctx_n) {
          if (k == 27) g_ctx_n = 0;
        } else if (wg_focus_get() == &id_path) {
          /* endereco com foco: as teclas sao do campo */
          if (k == '\r' || k == '\n') {
            go(g_edit.text, 1);
            wg_focus_set(&id_list);
          } else if (k == 27 || k == '\t') {
            gui_edit_set(&g_edit, g_path);
            wg_focus_set(&id_list);
          } else
            gui_edit_key(&g_edit, k);
        } else if (k == 27) run = 0;
        else if (k == GUI_KEY_DEL) {
          if (sel_ent()) g_ask_del = 1;
        } else if (k == GUI_KEY_UP) {
          if (g_sel > 0) g_sel--;
          else if (g_sel < 0 && g_n > 0) g_sel = 0;
          if (g_sel < g_top) g_top = g_sel;
          g_confirm_del = 0;
        } else if (k == GUI_KEY_DOWN) {
          if (g_sel < g_n - 1) g_sel++;
          else if (g_sel < 0 && g_n > 0) g_sel = 0;
          if (g_sel >= g_top + rows) g_top = g_sel - rows + 1;
          g_confirm_del = 0;
        } else if (k == GUI_KEY_PGUP) {
          g_top -= rows;
          if (g_top < 0) g_top = 0;
        } else if (k == GUI_KEY_PGDN) {
          g_top += rows;
          if (g_top > g_n - rows) g_top = g_n - rows;
          if (g_top < 0) g_top = 0;
        } else if (k == GUI_KEY_HOME) {
          g_sel = 0;
          g_top = 0;
        } else if (k == GUI_KEY_END) {
          g_sel = g_n - 1;
          if (g_sel >= 0 && g_sel >= g_top + rows) g_top = g_sel - rows + 1;
        } else if (k == '\r' || k == '\n') {
          open_sel();
        } else if (k == 8) { /* Backspace: sobe um nivel */
          char p[PATHMAX];
          copy_str(p, g_path, PATHMAX);
          path_parent(p);
          go(p, 1);
        } else if (k == '\t') {
          wg_focus_set(&id_path);
          gui_edit_select_all(&g_edit);
        }
      }
    }
    /* mantem a selecao visivel quando muda por clique */
    if (g_sel >= 0) {
      if (g_sel < g_top) g_top = g_sel;
      if (g_sel >= g_top + rows) g_top = g_sel - rows + 1;
    }
    if (g_top < 0) g_top = 0;
  }
  gui_close();
  return 0;
}
