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
 *   - barra Nome embaixo (tem o foco): digitar filtra a lista (pedaco do
 *     nome, ou curinga * e ?) e marca o melhor nome; Enter abre o marcado,
 *     o nome exato ou um caminho (.., ..\X, SUB\Y, D:\X); Backspace com o
 *     campo vazio sobe uma pasta; Esc limpa o filtro (vazio: fecha)
 *   - linha ".." no topo da lista (fora da raiz) sobe uma pasta
 *   - clique no cabecalho Nome/Tamanho/Data ordena (de novo inverte)
 */
#include <stdio.h>
#include <string.h>
#include <gui.h>
#include <widgets.h>

#define MAXENT  512
#define MAXHIST 32
#define PATHMAX 160

static struct k_dirent g_ent[MAXENT];
static int g_idx[MAXENT];   /* todas as entradas, ordenadas */
static int g_n;
/* lista mostrada (filtro aplicado): -1 = "..", senao indice em g_ent.
 * g_sel, g_top, g_ren sao posicoes nesta lista. */
static int g_vis[MAXENT + 1];
static int g_nv;
static struct k_dirent g_up_ent;   /* a linha ".." */

static char g_path[PATHMAX];
static char g_hist[MAXHIST][PATHMAX];
static int g_hn;
static int g_hpos;

static int g_sel;
static int g_top;
static int g_sort = 0;      /* 0 nome, 1 tamanho, 2 data */
static int g_rev;           /* 1 = ordem invertida (clique de novo no cabecalho) */
static int g_confirm_del;
/* 1 = a selecao mudou (teclado, filtro, clique): rola ate ela. A roda do rato
 * e a barra de rolagem nao ligam isso, senao a lista volta para a selecao. */
static int g_follow;

static gui_edit_t g_edit;
static char g_status[200];

/* barra Nome: o texto e o filtro que ele da (maiusculas, sem espacos nas
 * pontas). g_fkind: 0 sem filtro (vazio, caminho ou so pontos), 1 pedaco do
 * nome, 2 curinga (* ?) */
static gui_edit_t g_name;
static char g_filt[PATHMAX];
static int g_fkind;


/* ids das linhas da lista (wg_click_area) */
static char g_rowid[MAXENT + 1];
/* geometria da area de linhas (preenchida no desenho, usada no clique) */
static int g_lx, g_ly, g_lw, g_lh, g_rows;
static int g_mx, g_my, g_down;

/* ids de foco/clique */
static const char id_back, id_up, id_ref, id_path, id_go;
static const char id_open, id_newdir, id_del, id_list, id_drives, id_name;
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

/* Caminho digitado -> caminho completo (sempre com \ no fim), a partir da
 * pasta base: "." , "..", "..\\X", "SUB\\X", "\\X", "D:", "D:X", "D:\\X". */
static void path_resolve_in(const char* base, const char* arg, char* out) {
  char cwd[PATHMAX];
  char tmp[PATHMAX];
  int i, j, n;
  copy_str(cwd, base, PATHMAX);
  if (!cwd[0] || cwd[1] != ':') copy_str(cwd, "D:\\", PATHMAX);
  path_normalize(cwd);
  if (arg[0] && arg[1] == ':') {
    if (arg[2] == '\\' || arg[2] == '/') copy_str(tmp, arg, PATHMAX);
    else if ((arg[0] | 32) == (cwd[0] | 32))
      snprintf(tmp, PATHMAX, "%s%s", cwd, arg + 2); /* D:X na unidade atual */
    else
      snprintf(tmp, PATHMAX, "%c:\\%s", arg[0], arg + 2);
  } else if (arg[0] == '\\' || arg[0] == '/') {
    snprintf(tmp, PATHMAX, "%c:%s", cwd[0], arg);
  } else {
    snprintf(tmp, PATHMAX, "%s%s", cwd, arg);
  }
  path_normalize(tmp);
  /* tira os "." e resolve os ".." (sem passar da raiz "X:\\") */
  copy_str(out, tmp, PATHMAX);
  out[3] = 0;
  n = 3;
  i = 3;
  while (tmp[i]) {
    char part[PATHMAX];
    j = 0;
    while (tmp[i] && tmp[i] != '\\' && j < PATHMAX - 1) part[j++] = tmp[i++];
    part[j] = 0;
    if (tmp[i] == '\\') i++;
    if (!j || (j == 1 && part[0] == '.')) continue;
    if (j == 2 && part[0] == '.' && part[1] == '.') {
      if (n > 3) {
        n--; /* barra final */
        while (n > 3 && out[n - 1] != '\\') n--;
        out[n] = 0;
      }
      continue;
    }
    if (n + j + 1 >= PATHMAX) break;
    memcpy(out + n, part, (size_t)j);
    n += j;
    out[n++] = '\\';
    out[n] = 0;
  }
}

/* Argumento da linha de comando: a partir da pasta atual do shell
 * ("EXPLORER ." no Command). */
static void path_resolve(const char* arg, char* out) {
  char cwd[PATHMAX];
  k_pwd(cwd, PATHMAX);
  path_resolve_in(cwd, arg, out);
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
static int select_name(const char* name);
static void filter_update(void);

static void go(const char* p, int add_hist) {
  char tmp[PATHMAX];
  char old[PATHMAX];
  int ol;
  copy_str(tmp, p, PATHMAX);
  path_normalize(tmp);
  if (!dir_exists(tmp)) {
    set_status("pasta nao encontrada");
    return;
  }
  copy_str(old, g_path, PATHMAX);
  copy_str(g_path, tmp, PATHMAX);
  if (add_hist) hist_push(g_path);
  gui_edit_set(&g_edit, g_path);
  /* pasta nova: o filtro da barra Nome era da pasta anterior */
  gui_edit_set(&g_name, "");
  filter_update();
  load_dir(g_path);
  if (g_folder) gui_title(g_path);
  /* subiu (.., Acima, Voltar): marca a pasta de onde veio */
  ol = (int)strlen(g_path);
  if ((int)strlen(old) > ol && memcmp(old, g_path, (size_t)ol) == 0) {
    char child[40];
    int j = 0;
    while (old[ol + j] && old[ol + j] != '\\' && j < (int)sizeof(child) - 1) {
      child[j] = old[ol + j];
      j++;
    }
    child[j] = 0;
    if (j) select_name(child);
  }
}

/* Sobe uma pasta (botao Acima, linha "..", Backspace, ".." na barra Nome). */
static void go_up(void) {
  char p[PATHMAX];
  if ((int)strlen(g_path) <= 3) {
    set_status("ja esta na raiz da unidade");
    return;
  }
  copy_str(p, g_path, PATHMAX);
  path_parent(p);
  go(p, 1);
}

/* --------------------------------------------------------------- listagem */
static int cmp_entries(int a, int b) {
  struct k_dirent* x = &g_ent[a];
  struct k_dirent* y = &g_ent[b];
  int c = 0;
  /* pastas sempre antes, em qualquer ordem */
  if (x->dir != y->dir) return x->dir ? -1 : 1;
  if (g_sort == 1) {
    if (x->size != y->size) c = x->size > y->size ? -1 : 1;
  } else if (g_sort == 2) {
    unsigned kx = ((unsigned)x->date << 16) | x->time;
    unsigned ky = ((unsigned)y->date << 16) | y->time;
    if (kx != ky) c = kx > ky ? -1 : 1;
  }
  if (c) return g_rev ? -c : c;
  /* empate (ou ordem por nome): nome A-Z, so invertido na coluna Nome */
  c = ci_cmp(x->name, y->name);
  return (g_sort == 0 && g_rev) ? -c : c;
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

/* ----------------------------------------------------------------- filtro */
static int up_c(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* curinga do DOS/Windows: * qualquer sequencia, ? um caractere (p ja em
 * maiusculas) */
static int wild_match(const char* p, const char* s) {
  const char* star = 0;
  const char* ss = 0;
  while (*s) {
    if (*p == '?' || (*p && *p != '*' && *p == up_c((unsigned char)*s))) {
      p++;
      s++;
    } else if (*p == '*') {
      star = p++;
      ss = s;
    } else if (star) {
      p = star + 1;
      s = ++ss;
    } else
      return 0;
  }
  while (*p == '*') p++;
  return !*p;
}

static int wild_ok(const char* name) {
  int l;
  if (wild_match(g_filt, name)) return 1;
  /* "*.*" e "X*.*" pegam tambem nomes sem extensao, como no DOS */
  l = (int)strlen(g_filt);
  if (l >= 2 && g_filt[l - 2] == '.' && g_filt[l - 1] == '*' && !strchr(name, '.')) {
    char p[PATHMAX];
    copy_str(p, g_filt, PATHMAX);
    p[l - 2] = 0;
    return wild_match(p, name);
  }
  return 0;
}

static int has_prefix(const char* s, const char* pre) {
  int i;
  for (i = 0; pre[i]; i++)
    if (up_c((unsigned char)s[i]) != pre[i]) return 0;
  return 1;
}

static int has_sub(const char* s, const char* sub) {
  int i;
  for (i = 0; s[i]; i++)
    if (has_prefix(s + i, sub)) return 1;
  return !sub[0];
}

/* texto da barra Nome -> g_filt / g_fkind */
static void filter_update(void) {
  const char* s = g_name.text;
  int i = 0, n = 0, path = 0, wild = 0, dots = 1;
  while (s[i] == ' ') i++;
  for (; s[i] && n < PATHMAX - 1; i++) {
    char c = (char)up_c((unsigned char)s[i]);
    if (c == '\\' || c == '/' || c == ':') path = 1;
    if (c == '*' || c == '?') wild = 1;
    if (c != '.' && c != ' ') dots = 0;
    g_filt[n++] = c;
  }
  while (n > 0 && g_filt[n - 1] == ' ') n--;
  g_filt[n] = 0;
  if (!n || path || dots) g_fkind = 0;
  else g_fkind = wild ? 2 : 1;
}

static int real_count(void) {
  return g_nv - (g_nv > 0 && g_vis[0] < 0);
}

/* Monta a lista mostrada (g_vis) com o filtro. pick = 1 (o filtro mudou):
 * marca o melhor nome (igual > comeca com > contem; curinga so marca se
 * sobrou um). pick = 0: mantem marcado o item que estava, se ele aparece. */
static void rebuild_view(int pick) {
  int old = (g_sel >= 0 && g_sel < g_nv) ? g_vis[g_sel] : -2;
  int i, best = -1, best_sc = 0;
  g_nv = 0;
  /* ".." fora da raiz, quando nao ha filtro */
  if ((int)strlen(g_path) > 3 && g_fkind == 0) g_vis[g_nv++] = -1;
  for (i = 0; i < g_n; i++) {
    const char* nm = g_ent[g_idx[i]].name;
    int sc = 1;
    if (g_fkind == 1) {
      if (ci_cmp(nm, g_filt) == 0) sc = 3;
      else if (has_prefix(nm, g_filt)) sc = 2;
      else if (!has_sub(nm, g_filt)) continue;
    } else if (g_fkind == 2) {
      if (!wild_ok(nm)) continue;
    }
    if (sc > best_sc) {
      best_sc = sc;
      best = g_nv;
    }
    g_vis[g_nv++] = g_idx[i];
  }
  g_sel = -1;
  if (pick) {
    g_top = 0;
    if (g_fkind == 1) g_sel = best;
    else if (g_fkind == 2 && real_count() == 1) g_sel = best;
    else if (g_fkind == 0 && strcmp(g_filt, "..") == 0 && g_nv && g_vis[0] < 0)
      g_sel = 0;   /* ".." digitado: marca a linha ".." */
    else if (g_fkind == 0 && old != -2) pick = 0;   /* limpou: volta a de antes */
  }
  if (!pick && old != -2)
    for (i = 0; i < g_nv; i++)
      if (g_vis[i] == old) g_sel = i;
  g_follow = 1;
  g_confirm_del = 0;
}

/* O texto da barra Nome mudou: refiltra e diz quantos sobraram. */
static void name_changed(void) {
  char old[PATHMAX];
  int okind = g_fkind;
  copy_str(old, g_filt, PATHMAX);
  filter_update();
  if (okind == g_fkind && strcmp(old, g_filt) == 0) return;
  rebuild_view(1);
  {
    char b[200];
    int n = real_count();
    if (g_fkind && n)
      snprintf(b, sizeof(b), "%d item(ns) com \"%s\"%s", n, g_filt,
               g_sel >= 0 ? " - Enter abre o marcado" : " - escolha com as setas");
    else if (g_fkind)
      snprintf(b, sizeof(b), "nenhum item com \"%s\" nesta pasta", g_filt);
    else if (g_filt[0])
      snprintf(b, sizeof(b), "Enter vai para %s", g_filt);
    else
      snprintf(b, sizeof(b), "%d item(ns) em %s", g_n, g_path);
    set_status(b);
  }
}

static void name_set(const char* s) {
  gui_edit_set(&g_name, s);
  name_changed();
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
  rebuild_view(0);
  {
    char b[200];
    if (g_fkind)
      snprintf(b, sizeof(b), "%d de %d item(ns) em %s (filtro \"%s\")", real_count(), g_n,
               p, g_filt);
    else
      snprintf(b, sizeof(b), "%d item(ns) em %s", g_n, p);
    set_status(b);
  }
}

static int is_up_row(int i) { return i >= 0 && i < g_nv && g_vis[i] < 0; }

static struct k_dirent* row_ent(int i) {
  if (i < 0 || i >= g_nv) return 0;
  return g_vis[i] < 0 ? &g_up_ent : &g_ent[g_vis[i]];
}

/* item marcado; a linha ".." nao conta (nao se exclui nem renomeia) */
static struct k_dirent* sel_ent(void) {
  return is_up_row(g_sel) ? 0 : row_ent(g_sel);
}

/* Abre a pasta ou executa o arquivo selecionado. */
static void open_sel(void) {
  struct k_dirent* e = sel_ent();
  char full[PATHMAX];
  if (is_up_row(g_sel)) {
    go_up();
    return;
  }
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
    /* Associacao de extensoes (LINKEXT.DLL): 1 = desligado ou sem programa. */
    char full3[PATHMAX];
    copy_str(full3, g_path, PATHMAX);
    cat_str(full3, e->name, PATHMAX);
    if (k_open_file(full3) == 0) {
      char b[200];
      snprintf(b, sizeof(b), "abrindo %s", e->name);
      set_status(b);
      return;
    }
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

/* Seleciona a entrada com esse nome (sem diferenciar maiusculas). Se o
 * filtro a esconde, limpa o filtro. */
static int select_name(const char* name) {
  int i;
  for (i = 0; i < g_nv; i++)
    if (g_vis[i] >= 0 && ci_cmp(g_ent[g_vis[i]].name, name) == 0) {
      g_sel = i;
      g_follow = 1;
      return 1;
    }
  if (g_fkind) {
    gui_edit_set(&g_name, "");
    filter_update();
    rebuild_view(0);
    return select_name(name);
  }
  return 0;
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
  if (g_ren < 0 || g_ren >= g_nv || is_up_row(g_ren)) {
    g_ren = -1;
    return;
  }
  e = row_ent(g_ren);
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
      gui_edit_set(&g_name, "");   /* o filtro esconderia a pasta nova */
      filter_update();
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

/* ------------------------------------------------------ barra Nome / teclas */
/* Enter com um caminho (barra Nome ou endereco): pasta vai; arquivo abre na
 * pasta dele, marcado. */
static int nav_path(const char* t) {
  char full[PATHMAX];
  int l;
  path_resolve_in(g_path, t, full);
  if (dir_exists(full)) {
    go(full, 1);
    return 1;
  }
  l = (int)strlen(full);
  if (l > 3 && full[l - 1] == '\\') full[--l] = 0;
  if (l > 3 && k_fsize(full) >= 0) {
    char dir[PATHMAX];
    char base[40];
    int s = l;
    while (s > 0 && full[s - 1] != '\\') s--;
    copy_str(base, full + s, (int)sizeof(base));
    copy_str(dir, full, PATHMAX);
    dir[s] = 0;
    if (strcmp(dir, g_path) != 0) {
      go(dir, 1);
      if (strcmp(dir, g_path) != 0) return 0;
    }
    if (select_name(base)) open_sel();
    return 1;
  }
  {
    char b[200];
    snprintf(b, sizeof(b), "nao encontrado: %s", full);
    set_status(b);
  }
  return 0;
}

/* Enter na barra Nome: vazio abre o marcado; caminho ou ".." navega; nome
 * exato abre; senao abre o marcado pelo filtro. */
static void name_enter(void) {
  int i;
  if (!g_filt[0]) {
    open_sel();
    return;
  }
  if (g_fkind == 0) {
    nav_path(g_filt);
    return;
  }
  for (i = 0; i < g_nv; i++)
    if (g_vis[i] >= 0 && ci_cmp(g_ent[g_vis[i]].name, g_filt) == 0) {
      g_sel = i;
      open_sel();
      return;
    }
  if (g_sel >= 0) {
    open_sel();
    return;
  }
  {
    char b[200];
    if (real_count())
      snprintf(b, sizeof(b), "%d item(ns) com \"%s\": escolha com as setas", real_count(),
               g_filt);
    else
      snprintf(b, sizeof(b), "nenhum item com \"%s\" nesta pasta", g_filt);
    set_status(b);
  }
}

/* setas, PgUp/PgDn, Home/End movem a selecao (e a lista vai junto) */
static void list_key(int k, int rows) {
  int page = rows > 1 ? rows - 1 : 1;
  int cur = g_sel >= 0 ? g_sel : -1;
  if (!g_nv) return;
  if (k == GUI_KEY_UP) cur = cur < 0 ? g_top : cur - 1;
  else if (k == GUI_KEY_DOWN) cur = cur < 0 ? g_top : cur + 1;
  else if (k == GUI_KEY_PGUP) cur = (cur < 0 ? g_top : cur) - page;
  else if (k == GUI_KEY_PGDN) cur = (cur < 0 ? g_top : cur) + page;
  else if (k == GUI_KEY_HOME) cur = 0;
  else if (k == GUI_KEY_END) cur = g_nv - 1;
  if (cur < 0) cur = 0;
  if (cur > g_nv - 1) cur = g_nv - 1;
  g_sel = cur;
  g_follow = 1;
  g_confirm_del = 0;
}

/* Tecla com o foco na barra Nome (o normal). Devolve 1 para fechar. */
static int name_key(int k, int rows) {
  int empty = g_name.len == 0;
  if (k == '\r' || k == '\n') {
    name_enter();
    return 0;
  }
  if (k == 27) {
    if (empty) return 1;
    name_set("");
    return 0;
  }
  if (k == '\t') {
    wg_focus_set(&id_path);
    gui_edit_select_all(&g_edit);
    return 0;
  }
  if (k == GUI_KEY_UP || k == GUI_KEY_DOWN || k == GUI_KEY_PGUP || k == GUI_KEY_PGDN ||
      (empty && (k == GUI_KEY_HOME || k == GUI_KEY_END))) {
    list_key(k, rows);
    return 0;
  }
  if (empty && k == 8) { /* Backspace com o campo vazio: sobe um nivel */
    go_up();
    return 0;
  }
  if (empty && k == GUI_KEY_DEL) {
    if (sel_ent()) g_ask_del = 1;
    return 0;
  }
  if (gui_edit_key(&g_name, k)) name_changed();
  return 0;
}

/* --------------------------------------------------------------- desenho */
static void draw_folder(int x, int y, int on_sel) {
  /* tema com icones (Configuracoes > Temas): o desenho da pasta do tema */
  if (wg_icon("PASTA", x - 1, y, 17)) return;
  unsigned body = on_sel ? 0xFFE9A0 : 0xF0C860;
  unsigned dark = on_sel ? 0xC8A040 : 0xC09030;
  gui_fill(x, y + 2, 16, 3, dark);
  gui_fill(x, y + 4, 14, 9, body);
  gui_fill(x, y + 4, 14, 1, on_sel ? 0xFFFFC8 : 0xFFF0B0);
  gui_fill(x, y + 12, 14, 1, dark);
}

static void draw_file(int x, int y, int on_sel) {
  /* tema com icones (Configuracoes > Temas): o desenho da pasta do tema */
  if (wg_icon("ARQUIVO", x - 1, y, 17)) return;
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

/* setinha do cabecalho da coluna que ordena: up = crescente */
static void draw_sort_mark(int x, int y, int up, unsigned c) {
  int i;
  for (i = 0; i < 4; i++) {
    int r = up ? i : 3 - i;
    gui_fill(x + 3 - r, y + i, 2 * r + 1, 1, c);
  }
}

/* Cabecalho (clique ordena) e linhas da lista de arquivos. */
static int draw_file_list(int x, int y, int w, int h, int* out_rows) {
  int rh = wg_theme()->row_h;
  int rows;
  int i, k;
  int col_name = w - 250;
  int col_size = 90;
  int iw;
  char sz[32];
  char dt[32];
  wg_scroll_t sc;

  if (col_name < 120) col_name = 120;

  /* cabecalho das colunas: clique ordena, clique de novo inverte */
  gui_fill(x, y, w, 20, th->face);
  gui_fill(x, y, w, 1, th->face_hi);
  gui_fill(x, y + 19, w, 1, th->face_lo);
  {
    static const char* hn[3] = {"Nome", "Tamanho", "Data"};
    static const char* hid[3] = {&id_sortname, &id_sortsize, &id_sortdate};
    int hx[4];
    int menu = g_ctx_n || g_ask_del || g_ren >= 0;
    hx[0] = 0;
    hx[1] = col_name;
    hx[2] = col_name + col_size;
    hx[3] = w;
    for (k = 0; k < 3; k++) {
      int cx = x + hx[k], cw = hx[k + 1] - hx[k], tw;
      int hot = !menu && g_mx >= cx && g_mx < cx + cw && g_my >= y && g_my < y + 20;
      if (hot) gui_fill(cx + 1, y + 1, cw - 2, 18, wg_theme_is_dark() ? 0x3A3D48 : 0xDCE6F4);
      if (k) gui_fill(cx, y + 3, 1, 14, th->face_lo);
      tw = gui_text(cx + 6, y + 2, hn[k], -1, th->text, th->face, GUI_TEXT_NOBG);
      if (g_sort == k) {
        int asc = (k == 0) ? !g_rev : g_rev;   /* tamanho e data: maior/novo primeiro */
        draw_sort_mark(cx + 6 + tw + 6, y + 8, asc, th->text_dim);
      }
      if (wg_click_area(hid[k], cx, y, cw, 20, 1)) {
        if (g_sort == k) g_rev = !g_rev;
        else {
          g_sort = k;
          g_rev = 0;
        }
        sort_entries();
        rebuild_view(0);
      }
    }
  }
  y += 20;
  h -= 20;
  rows = (h - 2) / rh;
  if (rows < 1) rows = 1;
  *out_rows = rows;
  g_lx = x;
  g_ly = y;
  g_lw = w;
  g_lh = h;
  g_rows = rows;
  if (g_top > g_nv - rows) g_top = g_nv - rows;
  if (g_top < 0) g_top = 0;

  gui_fill(x, y, w, h, th->field_bg);
  gui_fill(x, y, w, 1, th->face_lo);
  gui_fill(x, y, 1, h, th->face_lo);
  gui_fill(x, y + h - 1, w, 1, th->face_hi);
  gui_fill(x + w - 1, y, 1, h, th->face_hi);

  /* scrollbar se precisa (as linhas param antes dela) */
  iw = w - 2;
  if (g_nv > rows) {
    iw = w - 16;
    sc.pos = g_top;
    sc.vis = rows;
    sc.total = g_nv;
    if (wg_scrollbar(&id_list, &sc, x + w - 14, y + 1, 13, h - 2)) g_top = sc.pos;
  }

  for (i = 0; i < rows; i++) {
    int idx = g_top + i;
    int iy = y + 1 + i * rh;
    int sel, up;
    unsigned fg, bg;
    struct k_dirent* e;
    if (idx >= g_nv) break;
    e = row_ent(idx);
    up = g_vis[idx] < 0;
    sel = (idx == g_sel);
    if (sel)
      gui_fill(x + 1, iy, iw, rh, th->sel_bg);
    else if (g_my >= iy && g_my < iy + rh && g_mx > x && g_mx < x + 1 + iw)
      gui_fill(x + 1, iy, iw, rh, wg_theme_is_dark() ? 0x2E3038 : 0xE8EEF8);
    if (wg_click_area(&g_rowid[idx], x + 1, iy, iw, rh, 1)) {
      if (idx == g_sel) {
        open_sel();
        return 0;   /* a lista mudou: o laco nao vale mais */
      }
      g_sel = idx;
      g_confirm_del = 0;
    }
    bg = sel ? th->sel_bg : th->field_bg;

    if (e->dir) draw_folder(x + 6, iy + 2, sel);
    else draw_file(x + 6, iy + 2, sel);

    if (up) {
      int tw = gui_text(x + 28, iy + 1, "..", -1, sel ? th->sel_text : th->text, bg,
                        GUI_TEXT_NOBG);
      gui_text_fit(x + 28 + tw + 12, iy + 1, "pasta acima", col_name - tw - 46,
                   sel ? th->sel_text : th->text_dim, bg, GUI_TEXT_NOBG);
      continue;
    }
    if (idx == g_ren) {
      g_ren_x = x + 26;
      g_ren_y = iy;
      g_ren_w = col_name - 30;
      g_ren_h = rh;
      if (g_ren_w < 120) g_ren_w = 120;
      gui_edit_draw(&g_ren_edit, g_ren_x, g_ren_y, g_ren_w, g_ren_h, 1);
    } else
      gui_text_fit(x + 28, iy + 1, e->name, col_name - 32,
                   sel ? th->sel_text : (e->dir ? th->text : th->field_text), bg,
                   GUI_TEXT_NOBG);
    fg = sel ? th->sel_text : th->text_dim;
    if (!e->dir) {
      fmt_size(e->size, sz, sizeof(sz));
      gui_text(x + col_name + 6, iy + 1, sz, -1, fg, bg, GUI_TEXT_NOBG);
    } else {
      gui_text(x + col_name + 6, iy + 1, "<DIR>", -1, fg, bg, GUI_TEXT_NOBG);
    }
    fmt_date(e->date, e->time, dt, sizeof(dt));
    gui_text_fit(x + col_name + col_size + 6, iy + 1, dt, iw - (col_name + col_size + 8), fg,
                 bg, GUI_TEXT_NOBG);
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
  /* dois tercos da largura e metade da altura da tela */
  if (gui_open_pct("Explorer", 67, 50) != 0) {
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
    path_resolve(argv[2], g_path);
    gui_title(g_path);
  } else if (argc > 1 && argv[1] && argv[1][0]) {
    /* EXPLORER . / EXPLORER SUB / EXPLORER ..: caminho completo, senao os
     * arquivos da lista nao abrem (ficariam relativos a ".") */
    path_resolve(argv[1], g_path);
    gui_title(g_path);
  }
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
  gui_edit_set(&g_name, "");
  copy_str(g_up_ent.name, "..", (int)sizeof(g_up_ent.name));
  g_up_ent.dir = 1;
  /* o foco fica na barra Nome: digitar ja filtra, setas andam na lista */
  wg_focus_set(&id_name);
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
    if (wg_button(&id_up, bx + 102, by, 96, bh, "Acima", (int)strlen(g_path) > 3)) go_up();
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
      if (nav_path(g_edit.text)) wg_focus_set(&id_name);
    }

    /* -------------------------------------------------------- coluna esquerda */
    lx = 10;
    ly = by + bh + 10;
    lw = sw < 760 ? 160 : 210; /* janela estreita (tela pequena) */
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

    /* ------------------------------------------- barra Nome + botoes de baixo */
    by = fy + fh + 8;
    bx = fx;
    {
      /* "Nome: [campo ..........] [Abrir] [Nova pasta] [Excluir]"; janela
       * estreita encolhe os botoes e deixa o campo com 140 px no minimo */
      static const int bw0[3] = {80, 104, 84};
      int bws[3], k, gap = 6, tot = 0, ex, ew, nlw, avail;
      nlw = wg_text_w("Nome:") + 8;
      for (k = 0; k < 3; k++) tot += bw0[k] + gap;
      avail = fw - nlw - 140;
      for (k = 0; k < 3; k++) {
        bws[k] = avail >= tot ? bw0[k] : bw0[k] * (avail > 150 ? avail : 150) / tot;
      }
      ew = fw - nlw - (bws[0] + bws[1] + bws[2] + 3 * gap);
      if (ew < 100) ew = 100;
      wg_label(bx, by + (28 - ch) / 2, "Nome:");
      ex = bx + nlw;
      wg_edit_box(&id_name, &g_name, ex, by + 1, ew, 26);
      if (!g_name.len) {
        /* dica no campo vazio */
        gui_text_fit(ex + 12, by + 1 + (26 - ch) / 2,
                     "digite para filtrar (*.EXE)  .. sobe  Enter abre", ew - 20,
                     wg_modern() ? th->text_dis : 0x909090, th->field_bg, GUI_TEXT_NOBG);
      }
      bx = ex + ew + gap;
      if (wg_button(&id_open, bx, by, bws[0], 28, "Abrir", g_nv > 0 || g_name.len > 0)) {
        name_enter();
        wg_focus_set(&id_name);
      }
      bx += bws[0] + gap;
      if (wg_button(&id_newdir, bx, by, bws[1], 28, "Nova pasta", 1)) make_dir();
      bx += bws[1] + gap;
      if (wg_button(&id_del, bx, by, bws[2], 28, "Excluir", sel_ent() != 0)) g_ask_del = 1;
    }

    /* -------------------------------------------------------- status bar */
    {
      int nd = 0, nf = 0;
      unsigned tot = 0;
      for (i = 0; i < g_nv; i++) {
        if (g_vis[i] < 0) continue;
        if (g_ent[g_vis[i]].dir) nd++;
        else {
          nf++;
          tot += g_ent[g_vis[i]].size;
        }
      }
      if (g_fkind)
        snprintf(st, sizeof(st), "%d de %d item(ns), %u KB", nd + nf, g_n,
                 (tot + 1023u) / 1024u);
      else
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
            int on = idx >= 0 && idx < g_nv && idx < g_top + g_rows;
            if (on) {
              g_sel = idx;
              g_confirm_del = 0;
            }
            if (wg_focus_get() == &id_path) gui_edit_set(&g_edit, g_path);
            wg_focus_set(&id_name);
            ctx_open_at(ev.x, ev.y, on && !is_up_row(idx));
          }
        } else {
          g_down = 1;
          /* clique abaixo da barra de cima tira o foco do endereco (sem Enter
           * ele volta a mostrar a pasta atual); a barra Nome pega o foco */
          if (wg_focus_get() == &id_path && ev.y > 8 + 28 + 2) {
            wg_focus_set(&id_name);
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
        /* roda: 3 linhas por passo (> 0 desce); so rola, nao mexe na
         * selecao (o g_follow fica 0, senao a lista volta para ela) */
        if (g_ren < 0 && !g_ask_del) {
          int step = ev.key * 3;
          if (step > rows) step = rows;
          if (step < -rows) step = -rows;
          g_top += step;
          g_ctx_n = 0;
        }
        g_mx = ev.x;
        g_my = ev.y;
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
            /* nao achou: fica no campo para corrigir */
            if (nav_path(g_edit.text)) {
              wg_focus_set(&id_name);
              gui_edit_set(&g_edit, g_path);
            }
          } else if (k == 27 || k == '\t') {
            gui_edit_set(&g_edit, g_path);
            wg_focus_set(&id_name);
          } else
            gui_edit_key(&g_edit, k);
        } else {
          /* o resto e da barra Nome (filtro) e da lista */
          wg_focus_set(&id_name);
          if (name_key(k, rows)) run = 0;
        }
      }
    }
    /* a selecao mudou (teclado, filtro, Enter): rola ate ela. A roda e a
     * barra de rolagem nao passam por aqui. */
    if (g_follow && g_sel >= 0) {
      if (g_sel < g_top) g_top = g_sel;
      if (g_sel >= g_top + rows) g_top = g_sel - rows + 1;
    }
    g_follow = 0;
    if (g_top > g_nv - rows) g_top = g_nv - rows;
    if (g_top < 0) g_top = 0;
  }
  gui_close();
  return 0;
}
