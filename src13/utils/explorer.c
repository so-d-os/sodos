/* explorer.c - gerenciador de arquivos do SODOS (EXPLORER.EXE), com o visual
 * do NOTEPAD.EXE (utils/winui.c: fonte TrueType suavizada e as cores do tema
 * do sistema, claro ou escuro, trocando na hora).
 *
 * Janela no estilo do Explorador de Arquivos do Windows 10:
 *   - barra de cima: Voltar / Avancar / Acima / Atualizar, o endereco (clique
 *     marca tudo, Enter vai) e a caixa Pesquisar (filtro da lista)
 *   - barra de comandos: Nova pasta, Novo arquivo, Abrir, Renomear, Excluir
 *   - painel da esquerda: Acesso rapido (Area de Trabalho...) e as unidades
 *   - lista com Nome / Data de modificacao / Tipo / Tamanho; clique no
 *     cabecalho ordena (de novo inverte); duplo clique (ou Enter) abre pasta,
 *     executavel ou o programa associado (LINKEXT; texto no NOTEPAD.EXE)
 *   - botao direito num item: Abrir / Criar atalho (.EXE) / Renomear /
 *     Excluir / Copiar caminho; num espaco vazio: Nova pasta / Novo arquivo /
 *     Atualizar
 *   - F2 renomeia o marcado (so o nome fica marcado: a extensao continua),
 *     F5 atualiza, Del exclui (com confirmacao)
 *   - a caixa Pesquisar tem o foco: digitar filtra a lista (pedaco do nome,
 *     ou curinga * e ?) e marca o melhor nome; Enter abre o marcado, o nome
 *     exato ou um caminho (.., ..\X, SUB\Y, D:\X); Backspace com a caixa
 *     vazia sobe uma pasta; Esc limpa (vazia: fecha); Tab vai ao endereco
 *   - linha ".." no topo da lista (fora da raiz) sobe uma pasta
 *
 * Dialogos do sistema (um argumento so, campos separados por |; quem chama
 * espera o arquivo de resposta, como o WINE no Escolher pasta):
 *   EXPLORER "/PICK|saida|pasta inicial|titulo"            escolher pasta
 *   EXPLORER "/OPEN|saida|pasta|titulo|filtros|nome"        abrir arquivo
 *   EXPLORER "/SAVE|saida|pasta|titulo|filtros|nome|extra"  salvar como
 * filtros: "Documentos de texto (*.txt)=*.txt;Todos os arquivos (*.*)=*.*"
 * (rotulos em UTF-8; varios padroes por virgula). extra (so /SAVE, opcional):
 * "Codificacao:=UTF-8,ANSI,...=indice", uma caixa de escolha a mais como a
 * Codificacao do Notepad. Pastas e nome: bytes do FAT (CP850). Resposta:
 *   "OK <caminho>\nFILTRO <n>\nEXTRA <n>\n" ou "CANCEL\n"
 * (no /PICK so "OK <pasta>\n"). "/PICK saida [pasta]" tambem vale.
 * EXPLORER @arquivo: o argumento vem da primeira linha do arquivo (que e
 * apagado): o WIN guarda so 192 bytes de argumentos por programa e um
 * /SAVE com pasta, nome e codificacao passa disso.
 */
#include "winui.c"

#define MAXENT  512
#define MAXHIST 32
#define PATHMAX 260 /* caminho com nomes longos */

static struct k_direntl g_ent[MAXENT]; /* nome inteiro, ate 255 */
static int g_idx[MAXENT];   /* todas as entradas, ordenadas */
static int g_n;
/* lista mostrada (filtro aplicado): -1 = "..", senao indice em g_ent.
 * g_sel, g_top, g_ren sao posicoes nesta lista. */
static int g_vis[MAXENT + 1];
static int g_nv;
static struct k_direntl g_up_ent;   /* a linha ".." */

static char g_path[PATHMAX];
static char g_hist[MAXHIST][PATHMAX];
static int g_hn;
static int g_hpos;

static int g_sel;
static int g_top;
static int g_sort = 0;      /* 0 nome, 1 tamanho, 2 data, 3 tipo */
static int g_rev;           /* 1 = ordem invertida (clique de novo no cabecalho) */
static int g_confirm_del;
/* 1 = a selecao mudou (teclado, filtro, clique): rola ate ela. A roda do rato
 * e a barra de rolagem nao ligam isso, senao a lista volta para a selecao. */
static int g_follow;

static gui_edit_t g_edit;   /* endereco */
static char g_status[200];

/* caixa Pesquisar: o texto e o filtro que ele da (maiusculas, sem espacos
 * nas pontas). g_fkind: 0 sem filtro (vazio, caminho ou so pontos), 1 pedaco
 * do nome, 2 curinga (* ?) */
static gui_edit_t g_name;
static char g_filt[PATHMAX];
static int g_fkind;

/* 1 = "visao de pasta" (EXPLORER /P caminho, clique numa pasta do desktop):
 * o titulo da janela e o caminho */
static int g_folder;
/* dialogos: g_pick = PK_*, g_pick_out = arquivo de resposta */
#define PK_DIR  1
#define PK_OPEN 2
#define PK_SAVE 3
static int g_pick;
static int g_pick_done;
static char g_pick_out[PATHMAX];
static char g_pick_title[80];
static gui_edit_t g_fname;          /* /OPEN /SAVE: o Nome */
static char g_flt_lab[8][72];       /* filtros: rotulo (UTF-8) e padroes */
static char g_flt_pat[8][64];
static int g_nflt, g_flt;
static char g_xlab[48];             /* caixa extra do /SAVE (Codificacao) */
static char g_xopt[8][40];
static int g_nx, g_xsel;
static char g_target[PATHMAX];      /* /SAVE: caminho a confirmar */

/* Renomear: linha em edicao (-1 = nenhuma). g_ren_ext: onde comeca a
 * extensao de um arquivo (-1 = pasta ou sem ponto): com o nome todo marcado,
 * digitar troca so o que vem antes. */
static int g_ren = -1;
static gui_edit_t g_ren_edit;
static int g_ren_ext = -1;
static int g_ren_x, g_ren_y, g_ren_w, g_ren_h;

static int pick_pattern_ok(const char* name);
static const char* name_ext(const char* n);
static void focus_set(int f);
static void ask_delete(void);

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
         (n[l - 2] == 'X' || n[l - 2] == 'x') &&
         (n[l - 1] == 'E' || n[l - 1] == 'e');
}

static int is_bat(const char* n) {
  int l = (int)strlen(n);
  if (l < 5) return 0;
  return n[l - 4] == '.' && (n[l - 3] == 'B' || n[l - 3] == 'b') &&
         (n[l - 2] == 'A' || n[l - 2] == 'a') &&
         (n[l - 1] == 'T' || n[l - 1] == 't');
}

/* Texto que o bloco de notas abre sem LINKEXT (a lista do @NOTEPAD do
 * LINKEXT.CFG de exemplo). */
static int is_text(const char* n) {
  static const char* const ext[] = {"TXT", "MD", "LOG", "INI", "CFG", "C", "H", "CSV"};
  const char* dot = strrchr(n, '.');
  int k;
  if (!dot || dot == n) return 0;
  for (k = 0; k < (int)(sizeof(ext) / sizeof(ext[0])); k++)
    if (ci_cmp(dot + 1, ext[k]) == 0) return 1;
  return 0;
}

/* Programa com um argumento, aberto pelo WIN (kexports.c); "@NOTEPAD" = o
 * bloco de notas, como no LINKEXT.CFG. */
int k_win_launch_arg(const char* exe, const char* arg);

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
  if (!date || !(date & 31) || !((date >> 5) & 15)) {
    /* sem data (ou data zerada no FAT: "00/00/2017") */
    out[0] = 0;
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
  struct k_direntl* x = &g_ent[a];
  struct k_direntl* y = &g_ent[b];
  int c = 0;
  /* pastas sempre antes, em qualquer ordem */
  if (x->dir != y->dir) return x->dir ? -1 : 1;
  if (g_sort == 1) {
    if (x->size != y->size) c = x->size > y->size ? -1 : 1;
  } else if (g_sort == 3) {
    c = ci_cmp(name_ext(x->name), name_ext(y->name));
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
  g_n = k_dir_long(p, g_ent, MAXENT);
  if (g_n < 0) {
    g_n = 0;
    set_status("erro ao listar a pasta");
  }
  if (g_pick) {
    /* selecionar pasta: os arquivos nao entram na lista; abrir/salvar:
     * so os do filtro escolhido (Documentos de texto, Todos...) */
    int k = 0;
    for (i = 0; i < g_n; i++)
      if (g_ent[i].dir || (g_pick != PK_DIR && pick_pattern_ok(g_ent[i].name)))
        g_ent[k++] = g_ent[i];
    g_n = k;
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

static struct k_direntl* row_ent(int i) {
  if (i < 0 || i >= g_nv) return 0;
  return g_vis[i] < 0 ? &g_up_ent : &g_ent[g_vis[i]];
}

/* item marcado; a linha ".." nao conta (nao se exclui nem renomeia) */
static struct k_direntl* sel_ent(void) {
  return is_up_row(g_sel) ? 0 : row_ent(g_sel);
}

/* Abre a pasta ou executa o arquivo selecionado. */
static void open_sel(void) {
  struct k_direntl* e = sel_ent();
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
    if (k_open_file(full3) == 0 ||
        (is_text(e->name) && k_win_launch_arg("@NOTEPAD", full3) == 0)) {
      /* sem LINKEXT (ou sem associacao): texto vai para o bloco de notas
       * do WIN (o NOTEPAD.EXE, como o @NOTEPAD do LINKEXT.CFG) */
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
  struct k_direntl* e = sel_ent();
  if (!e) return;
  g_ren = g_sel;
  gui_edit_set(&g_ren_edit, e->name);
  gui_edit_select_all(&g_ren_edit);
  g_ren_w = 0;
  g_ren_ext = -1;
  if (!e->dir) {
    int i;
    for (i = 1; e->name[i]; i++)
      if (e->name[i] == '.') g_ren_ext = i;
  }
}

/* Tecla no nome em edicao. Como no Windows, com tudo marcado a primeira
 * letra (ou Backspace/Del) troca so o nome e deixa a extensao. */
static void ren_key(int k) {
  if (g_ren_edit.all && g_ren_ext > 0 && g_ren_ext < g_ren_edit.len &&
      (k == '\b' || k == GUI_KEY_DEL || (k >= 32 && k < 127) || (k >= 0x80 && k <= 0xFF))) {
    char ext[64];
    copy_str(ext, g_ren_edit.text + g_ren_ext, (int)sizeof(ext));
    gui_edit_set(&g_ren_edit, ext);
    g_ren_edit.caret = 0;
    g_ren_ext = -1;
    if (k == '\b' || k == GUI_KEY_DEL) return;
  }
  if (k == GUI_KEY_LEFT || k == GUI_KEY_RIGHT || k == GUI_KEY_HOME || k == GUI_KEY_END)
    g_ren_ext = -1;
  gui_edit_key(&g_ren_edit, k);
}

static void cancel_rename(void) { g_ren = -1; }

/* Enter: grava o novo nome. Nao substitui arquivo que ja exista. */
static void commit_rename(void) {
  struct k_direntl* e;
  char nm[256];
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
  if (n > 255) {
    set_status("nome longo demais (max 255)");
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

/* Novo arquivo: arquivo.txt vazio, depois arquivo2.txt, arquivo3.txt...
 * (como o "Novo Documento de Texto" do Windows), ja com o nome em edicao. */
static void make_file(void) {
  char full[PATHMAX];
  int i;
  for (i = 1; i < 1000; i++) {
    char name[24];
    if (i == 1) copy_str(name, "arquivo.txt", sizeof(name));
    else snprintf(name, sizeof(name), "arquivo%d.txt", i);
    copy_str(full, g_path, PATHMAX);
    cat_str(full, name, PATHMAX);
    if (k_fsize(full) >= 0 || dir_exists(full)) continue;
    if (k_fsave(full, "", 0) < 0 || k_fsize(full) < 0) break;
    {
      char b[120];
      snprintf(b, sizeof(b), "%s criado: digite o nome e Enter", name);
      gui_edit_set(&g_name, "");   /* o filtro esconderia o arquivo novo */
      filter_update();
      load_dir(g_path);
      set_status(b);
      select_name(name);
      start_rename();
    }
    return;
  }
  set_status("nao foi possivel criar o arquivo");
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
  struct k_direntl* e = sel_ent();
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
  struct k_direntl* e = sel_ent();
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
    focus_set(1);
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
    if (sel_ent()) ask_delete();
    return 0;
  }
  if (k == GUI_KEY_F1 + 1) { /* F2: renomear */
    if (sel_ent() && !is_up_row(g_sel)) start_rename();
    return 0;
  }
  if (k == GUI_KEY_F1 + 4) { /* F5: atualizar */
    load_dir(g_path);
    set_status("lista atualizada");
    return 0;
  }
  if (gui_edit_key(&g_name, k)) name_changed();
  return 0;
}


/* ------------------------------------------------------------- dialogos (pick) */
/* O nome passa no filtro escolhido (/OPEN e /SAVE)? */
static int pick_pattern_ok(const char* name) {
  const char* p;
  if (g_pick < PK_OPEN || g_nflt <= 0) return 1;
  p = g_flt_pat[g_flt >= 0 && g_flt < g_nflt ? g_flt : 0];
  while (*p) {
    char pat[64];
    int k = 0;
    while (*p == ' ' || *p == ',' || *p == ';') p++;
    while (*p && *p != ',' && *p != ';' && k < 63) pat[k++] = (char)up_c((unsigned char)*p++);
    while (k > 0 && pat[k - 1] == ' ') k--;
    pat[k] = 0;
    if (!k) continue;
    if (!strcmp(pat, "*") || !strcmp(pat, "*.*")) return 1;
    if (wild_match(pat, name)) return 1;
  }
  return 0;
}

/* ".txt" do primeiro padrao "*.txt" do filtro (0 = nenhuma) */
static int pick_default_ext(char* out, int max) {
  const char* p;
  int k = 0;
  out[0] = 0;
  if (g_nflt <= 0) return 0;
  p = g_flt_pat[g_flt >= 0 && g_flt < g_nflt ? g_flt : 0];
  while (*p == ' ') p++;
  if (p[0] != '*' || p[1] != '.' || p[2] == '*' || !p[2]) return 0;
  p++;
  while (*p && *p != ',' && *p != ';' && *p != ' ' && k < max - 1) out[k++] = *p++;
  out[k] = 0;
  return k > 1;
}

/* ------------------------------------------------------------- tipo e tamanho */
static const char* name_ext(const char* n) {
  const char* d = strrchr(n, '.');
  return (d && d != n) ? d + 1 : "";
}

static void type_of(const struct k_direntl* e, char* out, int max) {
  static const char* const map[][2] = {
    {"TXT", "Documento de texto"}, {"MD", "Documento Markdown"}, {"LOG", "Registro (log)"},
    {"EXE", "Aplicativo"}, {"BAT", "Arquivo em lotes"}, {"DLL", "Extens\xc3\xa3o de aplicativo"},
    {"LNK", "Atalho"}, {"PNG", "Imagem PNG"}, {"JPG", "Imagem JPEG"}, {"JPE", "Imagem JPEG"},
    {"GIF", "Imagem GIF"}, {"BMP", "Imagem BMP"}, {"ICO", "\xc3\x8d" "cone"},
    {"MP4", "V\xc3\xad" "deo MP4"}, {"AVI", "V\xc3\xad" "deo AVI"}, {"MKV", "V\xc3\xad" "deo MKV"},
    {"MP3", "\xc3\x81udio MP3"}, {"WAV", "\xc3\x81udio WAV"}, {"C", "Arquivo C"}, {"H", "Arquivo de cabe\xc3\xa7" "alho C"},
    {"CPP", "Arquivo C++"}, {"INI", "Configura\xc3\xa7\xc3\xb5" "es"}, {"CFG", "Configura\xc3\xa7\xc3\xb5" "es"},
    {"HTM", "Documento HTML"}, {"HTML", "Documento HTML"}, {"ZIP", "Pasta compactada"},
    {"TTF", "Fonte TrueType"}, {"PDF", "Documento PDF"}, {"CSV", "Valores separados por v\xc3\xadrgula"},
    {"JS", "Arquivo JavaScript"}, {"JSON", "Arquivo JSON"}, {"THM", "Miniatura"},
  };
  const char* x;
  int k;
  if (e->dir) {
    scopy(out, "Pasta de arquivos", max);
    return;
  }
  x = name_ext(e->name);
  for (k = 0; k < (int)(sizeof(map) / sizeof(map[0])); k++)
    if (ci_cmp(x, map[k][0]) == 0) {
      scopy(out, map[k][1], max);
      return;
    }
  if (x[0]) {
    char u[16];
    int i;
    for (i = 0; x[i] && i < 15; i++) u[i] = (char)up_c((unsigned char)x[i]);
    u[i] = 0;
    snprintf(out, max, "Arquivo %s", u);
  } else
    scopy(out, "Arquivo", max);
}

/* "12 KB", "1.234 KB" (como o Explorador do Windows) */
static void size_kb(unsigned sz, char* out, int max) {
  unsigned kb = (sz + 1023u) / 1024u;
  if (kb >= 1000u)
    snprintf(out, max, "%u.%03u KB", kb / 1000u, kb % 1000u);
  else
    snprintf(out, max, "%u KB", kb);
}

/* ------------------------------------------------------------- caixas de texto */
static int edit_w(const char* s, int n) {
  int i, pen = 0;
  for (i = 0; i < n && s[i]; i++) pen += f_ui.g[gmap(cp850_to_uni((unsigned char)s[i]))].adv;
  return (pen + 128) >> 8;
}

/* gui_edit_t (bytes CP850, como o teclado manda) com a fonte da interface.
 * sel_end: marcado [0, sel_end) quando e->all (renomear: so o nome). */
static void edit_draw(gui_edit_t* e, int x, int y, int w, int h, int focused,
                      const char* hint, int sel_end) {
  int avail = w - 12, i, pen, ty = y + (h - f_ui.h) / 2;
  fill(x, y, w, h, P.field_bg);
  frame(x, y, w, h, focused ? P.focus : P.field_b);
  if (focused) fill(x, y + h - 2, w, 2, P.focus);
  if (e->caret > e->len) e->caret = e->len;
  if (e->scroll > e->caret) e->scroll = e->caret;
  while (e->scroll < e->caret && edit_w(e->text + e->scroll, e->caret - e->scroll) > avail)
    e->scroll++;
  clip_to(x + 2, y + 1, w - 4, h - 2);
  if (!e->len && hint) text_draw(&f_ui, x + 6, ty, hint, P.dim);
  pen = (x + 6) * 256;
  if (sel_end < 0 || sel_end > e->len) sel_end = e->len;
  for (i = e->scroll; i < e->len; i++) {
    int idx = gmap(cp850_to_uni((unsigned char)e->text[i]));
    int px = (pen + 128) >> 8, adv = (f_ui.g[idx].adv + 128) >> 8;
    int sel = focused && e->all && i < sel_end;
    if (px > x + w) break;
    if (sel) fill(px, y + 4, adv + 1, h - 8, P.sel_bg);
    glyph_draw(&f_ui, idx, px, ty, sel ? P.sel_fg : P.text);
    pen += f_ui.g[idx].adv;
  }
  if (focused && !(e->all && sel_end >= e->len)) {
    int cx = x + 6 + edit_w(e->text + e->scroll, e->caret - e->scroll);
    fill(cx, y + 5, 1, h - 10, P.text);
  }
  clip_all();
}

static void edit_click(gui_edit_t* e, int x, int px) {
  int i, cx = x + 6;
  for (i = e->scroll; i < e->len; i++) {
    int adv = (f_ui.g[gmap(cp850_to_uni((unsigned char)e->text[i]))].adv + 128) >> 8;
    if (px < cx + adv / 2) break;
    cx += adv;
  }
  e->caret = i;
  e->all = 0;
}

/* texto em CP850 (nomes do FAT) com corte "..." */
static void cp_text(int x, int y, const char* s, int maxw, unsigned rgb) {
  char b[600];
  cp_to_u8(s, b, (int)sizeof(b));
  text_fit(&f_ui, x, y, b, maxw, rgb);
}

/* ------------------------------------------------------------- icones */
static void ico_folder(int x, int y, int s) {
  int th = s / 5 > 2 ? s / 5 : 2;
  fill(x, y + 1, s * 5 / 12, th + 1, 0xE0A92C);
  fill(x, y + th, s, s * 3 / 4 - th + 1, 0xE8B739);
  fill(x, y + th + 2, s, s * 3 / 4 - th - 1, 0xF8D775);
  fill(x, y + th + 2, s, 1, 0xFCE6A4);
}

static void ico_file(int x, int y, int s, const char* name) {
  const char* e = name_ext(name);
  unsigned band = 0;
  int w = s * 3 / 4, k;
  if (!ci_cmp(e, "EXE") || !ci_cmp(e, "BAT") || !ci_cmp(e, "LNK")) band = 0x2F78D0;
  else if (!ci_cmp(e, "PNG") || !ci_cmp(e, "JPG") || !ci_cmp(e, "GIF") || !ci_cmp(e, "BMP") ||
           !ci_cmp(e, "JPE") || !ci_cmp(e, "ICO"))
    band = 0x3AA55A;
  else if (!ci_cmp(e, "MP4") || !ci_cmp(e, "AVI") || !ci_cmp(e, "MKV") || !ci_cmp(e, "MOV"))
    band = 0xD04A4A;
  fill(x + 2, y, w, s, 0xFFFFFF);
  frame(x + 2, y, w, s, 0x8A8A8A);
  fill(x + 2 + w - 4, y, 4, 4, P.dark ? 0x30363F : 0xE6E6E6);
  if (band) {
    fill(x + 4, y + s / 2 - 2, w - 4, s / 3, band);
  } else {
    for (k = 0; k < 3; k++) fill(x + 5, y + 4 + k * (s / 5), w - 6 - (k == 2 ? 3 : 0), 1, 0x9A9A9A);
  }
}

static void ico_drive(int x, int y, int s, int letter) {
  fill(x, y + s / 4, s, s / 2, 0xB9BDC3);
  frame(x, y + s / 4, s, s / 2, 0x7D838C);
  fill(x + 3, y + s / 2, s / 3, 2, letter == 'U' ? 0xE0A92C : 0x3AA55A);
}

static void ico_desktop(int x, int y, int s) {
  fill(x, y + 1, s, s * 2 / 3, 0x2F78D0);
  frame(x, y + 1, s, s * 2 / 3, 0x1F5596);
  fill(x + s / 2 - 1, y + s * 2 / 3 + 1, 3, s / 6, 0x7D838C);
  fill(x + s / 4, y + s * 5 / 6, s / 2 + 1, 2, 0x7D838C);
}

/* setas da barra de cima: 0 esquerda, 1 direita, 2 cima */
static void ico_arrow(int cx, int cy, int dir, unsigned c) {
  int i;
  if (dir == 2) {
    for (i = 0; i < 6; i++) fill(cx - i, cy - 5 + i, 2 * i + 1, 1, c);
    fill(cx - 1, cy, 3, 7, c);
    return;
  }
  for (i = 0; i < 6; i++) {
    int x = dir == 0 ? cx - 5 + i : cx + 5 - i;
    fill(x, cy - i, 1, 2 * i + 1, c);
  }
  fill(dir == 0 ? cx : cx - 7, cy - 1, 8, 3, c);
}

static void ico_refresh(int cx, int cy, unsigned c) {
  int x, y;
  for (y = -7; y <= 7; y++)
    for (x = -7; x <= 7; x++) {
      int d = x * x + y * y;
      if (d >= 25 && d <= 49 && !(x > 1 && y < -1)) fill(cx + x, cy + y, 1, 1, c);
    }
  for (y = 0; y < 4; y++) fill(cx + 2, cy - 7 + y, 4 - y, 1, c);
  fill(cx + 2, cy - 4, 4, 1, c);
}

/* ------------------------------------------------------------- botoes */
static void btn_draw(int x, int y, int w, int h, const char* label, int hot, int def, int en) {
  unsigned bg = hot && en ? P.btn_hot : P.btn, bd = (hot && en) || def ? P.focus : P.btn_b;
  fill(x, y, w, h, bg);
  frame(x, y, w, h, bd);
  if (def) frame(x + 1, y + 1, w - 2, h - 2, bd);
  text_draw(&f_ui, x + (w - text_w(&f_ui, label)) / 2, y + (h - f_ui.h) / 2, label,
            en ? P.text : P.dis);
}

/* combo: caixa com o texto escolhido e a setinha (abre um menu) */
static void combo_draw(int x, int y, int w, int h, const char* t, int hot) {
  fill(x, y, w, h, hot ? P.btn_hot : P.field_bg);
  frame(x, y, w, h, hot ? P.focus : P.field_b);
  text_fit(&f_ui, x + 8, y + (h - f_ui.h) / 2, t, w - 30, P.text);
  draw_tri_down(x + w - 16, y + h / 2 - 2, 4, P.text);
}

/* ------------------------------------------------------------- regioes de clique */
#define H_BACK 1
#define H_FWD 2
#define H_UP 3
#define H_REF 4
#define H_ADDR 5
#define H_SEARCH 6
#define H_CMD 7
#define H_PLACE 8
#define H_HEAD 9
#define H_ROW 10
#define H_LIST 11
#define H_SB 12
#define H_FNAME 13
#define H_FILTER 14
#define H_EXTRA 15
#define H_OK 16
#define H_CANCEL 17
#define H_DYES 18
#define H_DNO 19
#define H_DCLOSE 20
#define HR_MAX 160
struct hr {
  int x, y, w, h, id, arg;
};
static struct hr hrs[HR_MAX];
static int nhr;
static int g_mx = -1, g_my = -1;

static void hr_add(int x, int y, int w, int h, int id, int arg) {
  if (nhr >= HR_MAX) return;
  hrs[nhr].x = x;
  hrs[nhr].y = y;
  hrs[nhr].w = w;
  hrs[nhr].h = h;
  hrs[nhr].id = id;
  hrs[nhr].arg = arg;
  nhr++;
}

/* a ultima regiao que contem (x, y): a de cima */
static int hr_find(int x, int y, int* arg) {
  int i;
  for (i = nhr - 1; i >= 0; i--)
    if (x >= hrs[i].x && y >= hrs[i].y && x < hrs[i].x + hrs[i].w && y < hrs[i].y + hrs[i].h) {
      if (arg) *arg = hrs[i].arg;
      return hrs[i].id;
    }
  return 0;
}

static int hover(int x, int y, int w, int h) {
  return g_mx >= x && g_my >= y && g_mx < x + w && g_my < y + h;
}

/* ------------------------------------------------------------- foco */
#define FOC_NAME 0   /* caixa Pesquisar (o normal: digitar filtra) */
#define FOC_PATH 1   /* endereco */
#define FOC_FNAME 2  /* Nome do /OPEN /SAVE */
static int g_focus;

static void focus_set(int f) {
  if (g_focus == FOC_PATH && f != FOC_PATH) gui_edit_set(&g_edit, g_path);
  g_focus = f;
}

/* ------------------------------------------------------------- menus */
#define MN_CTX 1
#define MN_FILTER 2
#define MN_EXTRA 3
#define CM_OPEN 1
#define CM_LINK 2
#define CM_REN 3
#define CM_DEL 4
#define CM_COPY 5
#define CM_NEWDIR 6
#define CM_NEWFILE 7
#define CM_REF 8
#define ID_FLT0 100
#define ID_X0 200
static int mn_kind;
static struct mi mn_items[14];
static int mn_x, mn_y, mn_hot = -1;

static int mi_enabled(int id) {
  (void)id;
  return 1;
}
static int mi_checked(int id) {
  if (mn_kind == MN_FILTER) return id - ID_FLT0 == g_flt;
  if (mn_kind == MN_EXTRA) return id - ID_X0 == g_xsel;
  return 0;
}

static void mn_add(int* n, const char* t, const char* k, int id) {
  if (*n >= 13) return;
  mn_items[*n].t = t;
  mn_items[*n].k = k;
  mn_items[*n].id = id;
  mn_items[*n].sub = 0;
  (*n)++;
  mn_items[*n].t = 0;
}

static void ctx_open_at(int x, int y, int on_item) {
  struct k_direntl* e = on_item ? sel_ent() : 0;
  int n = 0;
  mn_items[0].t = 0;
  if (e) {
    mn_add(&n, "Abrir", "Enter", CM_OPEN);
    if (!e->dir && is_exe(e->name)) mn_add(&n, "Criar atalho na \xc3\x81rea de Trabalho", "", CM_LINK);
    mn_add(&n, "-", 0, 0);
    mn_add(&n, "Renomear", "F2", CM_REN);
    mn_add(&n, "Excluir", "Del", CM_DEL);
    mn_add(&n, "-", 0, 0);
    mn_add(&n, "Copiar caminho", "", CM_COPY);
  } else {
    mn_add(&n, "Nova pasta", "", CM_NEWDIR);
    if (g_pick != PK_DIR) mn_add(&n, "Novo arquivo", "", CM_NEWFILE);
    mn_add(&n, "-", 0, 0);
    mn_add(&n, "Atualizar", "F5", CM_REF);
    mn_add(&n, "Copiar caminho da pasta", "", CM_COPY);
  }
  mn_kind = MN_CTX;
  mn_x = x;
  mn_y = y;
  mn_hot = -1;
}

static void combo_menu(int kind, int x, int y) {
  int n = 0, i;
  mn_items[0].t = 0;
  if (kind == MN_FILTER)
    for (i = 0; i < g_nflt; i++) mn_add(&n, g_flt_lab[i], "", ID_FLT0 + i);
  else
    for (i = 0; i < g_nx; i++) mn_add(&n, g_xopt[i], "", ID_X0 + i);
  if (!n) return;
  mn_kind = kind;
  mn_x = x;
  mn_y = y;
  mn_hot = -1;
}

/* ------------------------------------------------------------- confirmacao */
#define DK_DEL 1
#define DK_OVER 2
#define DK_MSG 3
static int dk;
static char dk_title[64];
static char dk_msg[400];

static void dk_open(int kind, const char* title, const char* msg) {
  dk = kind;
  scopy(dk_title, title, (int)sizeof(dk_title));
  scopy(dk_msg, msg, (int)sizeof(dk_msg));
  mn_kind = 0;
}

static void ask_delete(void) {
  struct k_direntl* e = sel_ent();
  char m[400], nm[300];
  if (!e) return;
  cp_to_u8(e->name, nm, (int)sizeof(nm));
  if (e->dir)
    snprintf(m, sizeof(m), "Tem certeza de que deseja excluir a pasta \"%s\"?\n(s\xc3\xb3 pastas vazias)", nm);
  else
    snprintf(m, sizeof(m), "Tem certeza de que deseja excluir o arquivo \"%s\"?", nm);
  dk_open(DK_DEL, e->dir ? "Excluir pasta" : "Excluir arquivo", m);
}

/* ------------------------------------------------------------- respostas */
static void pick_target(char* out, int max) {
  struct k_direntl* e = sel_ent();
  int l;
  copy_str(out, g_path, max);
  if (e && e->dir) cat_str(out, e->name, max);
  l = (int)strlen(out);
  if (l > 3 && out[l - 1] == '\\') out[l - 1] = 0;
}

static void pick_write(const char* b) {
  if (g_pick_out[0]) k_fsave(g_pick_out, b, (int)strlen(b));
}

/* /PICK: OK com a pasta marcada (ou a aberta); 0 = cancelado */
static void pick_answer(int ok) {
  char b[PATHMAX + 8];
  if (!g_pick || g_pick_done) return;
  g_pick_done = 1;
  if (ok && g_pick == PK_DIR) {
    char t[PATHMAX];
    pick_target(t, PATHMAX);
    snprintf(b, sizeof(b), "OK %s\n", t);
  } else
    copy_str(b, "CANCEL\n", (int)sizeof(b));
  pick_write(b);
}

/* /OPEN /SAVE: OK com o arquivo */
static void pick_answer_file(const char* full) {
  char b[PATHMAX + 64];
  if (g_pick_done) return;
  g_pick_done = 1;
  snprintf(b, sizeof(b), "OK %s\nFILTRO %d\nEXTRA %d\n", full, g_flt, g_xsel);
  pick_write(b);
}

static int run = 1;

/* Abrir / Salvar do /OPEN /SAVE: o Nome (ou a pasta marcada) */
static void file_accept(void) {
  char nm[PATHMAX], full[PATHMAX];
  int n, i;
  copy_str(nm, g_fname.text, PATHMAX);
  i = 0;
  while (nm[i] == ' ') i++;
  if (i) memmove(nm, nm + i, strlen(nm + i) + 1);
  n = (int)strlen(nm);
  while (n > 0 && (nm[n - 1] == ' ')) nm[--n] = 0;
  if (!n) {
    struct k_direntl* e = sel_ent();
    if (is_up_row(g_sel)) go_up();
    else if (e && e->dir) open_sel();
    return;
  }
  if (strchr(nm, '*') || strchr(nm, '?')) {
    /* curinga: vira o filtro da lista */
    gui_edit_set(&g_name, nm);
    gui_edit_set(&g_fname, "");
    name_changed();
    return;
  }
  if (nm[1] == ':') copy_str(full, nm, PATHMAX);
  else if (nm[0] == '\\') snprintf(full, PATHMAX, "%c:%s", g_path[0], nm);
  else {
    copy_str(full, g_path, PATHMAX);
    cat_str(full, nm, PATHMAX);
  }
  if (dir_exists(full)) {
    go(full, 1);
    gui_edit_set(&g_fname, "");
    return;
  }
  if (g_pick == PK_OPEN) {
    if (k_fsize(full) < 0) {
      char ext[16], t2[PATHMAX], m[400], u[300];
      if (!strchr(nm, '.') && pick_default_ext(ext, (int)sizeof(ext))) {
        copy_str(t2, full, PATHMAX);
        cat_str(t2, ext, PATHMAX);
        if (k_fsize(t2) >= 0) {
          pick_answer_file(t2);
          run = 0;
          return;
        }
      }
      cp_to_u8(nm, u, (int)sizeof(u));
      snprintf(m, sizeof(m), "%s\nArquivo n\xc3\xa3o encontrado.\nVerifique o nome e tente de novo.", u);
      dk_open(DK_MSG, g_pick_title[0] ? g_pick_title : "Abrir", m);
      return;
    }
    pick_answer_file(full);
    run = 0;
    return;
  }
  /* Salvar como: sem extensao ganha a do filtro (*.txt -> .txt) */
  {
    char ext[16];
    const char* base = strrchr(full, '\\');
    base = base ? base + 1 : full;
    if (!strchr(base, '.') && pick_default_ext(ext, (int)sizeof(ext))) cat_str(full, ext, PATHMAX);
  }
  copy_str(g_target, full, PATHMAX);
  if (k_fsize(full) >= 0) {
    char m[400], u[300];
    const char* base = strrchr(full, '\\');
    cp_to_u8(base ? base + 1 : full, u, (int)sizeof(u));
    snprintf(m, sizeof(m), "%s j\xc3\xa1 existe.\nDeseja substitu\xc3\xad-lo?", u);
    dk_open(DK_OVER, "Confirmar Salvar como", m);
    return;
  }
  pick_answer_file(full);
  run = 0;
}

/* ------------------------------------------------------------- comandos */
static void cmd_run(int id) {
  mn_kind = 0;
  switch (id) {
    case CM_OPEN:
      if (g_pick >= PK_OPEN) {
        struct k_direntl* e = sel_ent();
        if (e && !e->dir) {
          gui_edit_set(&g_fname, e->name);
          file_accept();
        } else
          open_sel();
      } else
        open_sel();
      break;
    case CM_LINK: make_shortcut(); break;
    case CM_REN:
      if (sel_ent()) start_rename();
      break;
    case CM_DEL:
      if (sel_ent()) ask_delete();
      break;
    case CM_COPY: {
      char p[PATHMAX], u[PATHMAX * 2];
      struct k_direntl* e = sel_ent();
      copy_str(p, g_path, PATHMAX);
      if (e) cat_str(p, e->name, PATHMAX);
      cp_to_u8(p, u, (int)sizeof(u));
      k_clip_set(u, -1);
      set_status("caminho copiado");
      break;
    }
    case CM_NEWDIR: make_dir(); break;
    case CM_NEWFILE: make_file(); break;
    case CM_REF:
      load_dir(g_path);
      set_status("lista atualizada");
      break;
    default:
      if (id >= ID_FLT0 && id < ID_FLT0 + 8) {
        g_flt = id - ID_FLT0;
        load_dir(g_path);
      } else if (id >= ID_X0 && id < ID_X0 + 8)
        g_xsel = id - ID_X0;
      break;
  }
}

/* ------------------------------------------------------------- lugares */
#define NPL 16
static char pl_lab[NPL][48];
static char pl_path[NPL][PATHMAX];
static int pl_kind[NPL];   /* 0 cabecalho, 1 area de trabalho, 2 pasta, 3 unidade */
static int npl;

static void pl_add(const char* lab, const char* path, int kind) {
  if (npl >= NPL) return;
  scopy(pl_lab[npl], lab, 48);
  copy_str(pl_path[npl], path, PATHMAX);
  pl_kind[npl] = kind;
  npl++;
}

static void places_init(void) {
  unsigned mask = k_drive_mask();
  char d[PATHMAX];
  int L;
  npl = 0;
  pl_add("Acesso r\xc3\xa1pido", "", 0);
  if (desktop_dir(d, PATHMAX) == 0) pl_add("\xc3\x81rea de Trabalho", d, 1);
  if (g_pick == PK_DIR) {
    static const char* const pk[3] = {"D:\\REPO\\", "D:\\SRC\\", "D:\\WINE\\USERS\\SODOS\\"};
    static const char* const pkl[3] = {"Reposit\xc3\xb3rios", "Fontes (SRC)", "Usu\xc3\xa1rio do WINE"};
    int i;
    for (i = 0; i < 3; i++)
      if (dir_exists(pk[i])) pl_add(pkl[i], pk[i], 2);
  } else {
    if (dir_exists("D:\\USER\\DEFAULT\\")) pl_add("Usu\xc3\xa1rio", "D:\\USER\\DEFAULT\\", 2);
    if (dir_exists("D:\\SODOS\\")) pl_add("Sistema (SODOS)", "D:\\SODOS\\", 2);
  }
  pl_add("Este computador", "", 0);
  for (L = 0; L < 26 && npl < NPL; L++) {
    char lab[48], p[4];
    if (!(mask & (1u << L)) || L < 2) continue;
    if (L == 'U' - 'A') snprintf(lab, sizeof(lab), "Pendrive (%c:)", 'A' + L);
    else if (L == 'Z' - 'A') snprintf(lab, sizeof(lab), "Mem\xc3\xb3ria (%c:)", 'A' + L);
    else snprintf(lab, sizeof(lab), "Disco local (%c:)", 'A' + L);
    p[0] = (char)('A' + L);
    p[1] = ':';
    p[2] = '\\';
    p[3] = 0;
    pl_add(lab, p, 3);
  }
}

/* ------------------------------------------------------------- desenho */
static int top_h, cmd_h, nav_w, bot_h, stat_h, row_h, head_h;
static int lx, ly, lw, lh;          /* lista (com o cabecalho) */
static int g_rows = 10;
static int sb_drag, sb_off;
static int col_date_x, col_type_x, col_size_x;   /* 0 = coluna escondida */

static void ui_geometry(void) {
  top_h = f_ui.h + 26;
  cmd_h = f_ui.h + 18;
  row_h = f_ui.h + 10;
  head_h = f_ui.h + 12;
  stat_h = f_ui.h + 10;
  nav_w = fbw >= 900 ? 210 : fbw >= 640 ? 180 : 0;
  /* dialogos: /PICK uma linha (Pasta + botoes); /OPEN /SAVE duas */
  bot_h = g_pick == PK_DIR ? (f_ui.h + 14) + 24 : g_pick ? 2 * (f_ui.h + 14) + 36 : 0;
  lx = nav_w;
  ly = top_h + cmd_h;
  lw = fbw - nav_w;
  lh = fbh - ly - bot_h - stat_h;
  if (lh < head_h + row_h) lh = head_h + row_h;
  g_rows = (lh - head_h - 2) / row_h;
  if (g_rows < 1) g_rows = 1;
}

static void draw_top(void) {
  int x = 8, y = 6, b = top_h - 12, sw, ax, aw;
  struct {
    int id, en, dir;
  } nb[4];
  int i;
  nb[0].id = H_BACK;
  nb[0].en = g_hpos > 0;
  nb[0].dir = 0;
  nb[1].id = H_FWD;
  nb[1].en = g_hpos < g_hn - 1;
  nb[1].dir = 1;
  nb[2].id = H_UP;
  nb[2].en = (int)strlen(g_path) > 3;
  nb[2].dir = 2;
  nb[3].id = H_REF;
  nb[3].en = 1;
  nb[3].dir = 3;
  fill(0, 0, fbw, top_h, P.bar_bg);
  for (i = 0; i < 4; i++) {
    unsigned c = nb[i].en ? P.text : P.dis;
    if (nb[i].en && hover(x, y, b, b)) fill(x, y, b, b, P.mb_hot);
    if (nb[i].dir == 3) ico_refresh(x + b / 2, y + b / 2, c);
    else ico_arrow(x + b / 2, y + b / 2, nb[i].dir, c);
    if (nb[i].en) hr_add(x, y, b, b, nb[i].id, 0);
    x += b + 4;
  }
  sw = fbw >= 760 ? 230 : 150;
  ax = x + 6;
  aw = fbw - ax - sw - 18;
  if (aw < 120) aw = 120;
  /* endereco: pasta e setinha como o "Este computador > Disco > pasta" */
  {
    int foc = g_focus == FOC_PATH;
    edit_draw(&g_edit, ax, y, aw, b, foc, 0, -1);
    hr_add(ax, y, aw, b, H_ADDR, 0);
  }
  {
    int sx = ax + aw + 10, foc = g_focus == FOC_NAME;
    char hint[300], u[260];
    const char* base = g_path;
    int k, l = (int)strlen(g_path);
    for (k = 0; k < l - 1; k++)
      if (g_path[k] == '\\') base = g_path + k + 1;
    cp_to_u8(base, u, (int)sizeof(u));
    l = (int)strlen(u);
    if (l && u[l - 1] == '\\') u[l - 1] = 0;
    if (!u[0] || u[1] == ':') snprintf(hint, sizeof(hint), "Pesquisar");
    else snprintf(hint, sizeof(hint), "Pesquisar em %s", u);
    edit_draw(&g_name, sx, y, sw, b, foc, hint, -1);
    /* lupa */
    if (!g_name.len) {
      int cx = sx + sw - 16, cy = y + b / 2 - 2, xx, yy;
      for (yy = -5; yy <= 5; yy++)
        for (xx = -5; xx <= 5; xx++) {
          int d = xx * xx + yy * yy;
          if (d >= 12 && d <= 25) fill(cx + xx, cy + yy, 1, 1, P.dim);
        }
      for (k = 0; k < 4; k++) fill(cx + 4 + k, cy + 4 + k, 2, 2, P.dim);
    }
    hr_add(sx, y, sw, b, H_SEARCH, 0);
  }
  fill(0, top_h - 1, fbw, 1, P.bar_line);
}

static void draw_cmdbar(void) {
  static const char* const lab[5] = {"Nova pasta", "Novo arquivo", "Abrir", "Renomear", "Excluir"};
  static const int ids[5] = {CM_NEWDIR, CM_NEWFILE, CM_OPEN, CM_REN, CM_DEL};
  int x = 8, y = top_h + 4, h = cmd_h - 8, i;
  int has = sel_ent() != 0;
  fill(0, top_h, fbw, cmd_h, P.bar_bg);
  for (i = 0; i < 5; i++) {
    int en = i < 2 || (i == 2 && (has || is_up_row(g_sel))) || (i >= 3 && has);
    int w = text_w(&f_ui, lab[i]) + 34;
    unsigned c = en ? P.text : P.dis;
    if (g_pick == PK_DIR && i == 1) continue;
    if (g_pick && i >= 3) continue;
    if (en && hover(x, y, w, h)) fill(x, y, w, h, P.mb_hot);
    /* icone pequeno */
    if (i == 0) ico_folder(x + 6, y + h / 2 - 7, 16);
    else if (i == 1) ico_file(x + 6, y + h / 2 - 8, 16, "a.txt");
    else if (i == 2) ico_arrow(x + 14, y + h / 2, 1, c);
    else if (i == 3) {
      fill(x + 7, y + h / 2 - 5, 14, 10, P.field_bg);
      frame(x + 7, y + h / 2 - 5, 14, 10, c);
      fill(x + 10, y + h / 2 - 2, 1, 5, c);
    } else {
      int k;
      for (k = 0; k < 9; k++) {
        fill(x + 9 + k, y + h / 2 - 4 + k, 2, 1, en ? 0xD04A4A : P.dis);
        fill(x + 17 - k, y + h / 2 - 4 + k, 2, 1, en ? 0xD04A4A : P.dis);
      }
    }
    text_draw(&f_ui, x + 28, y + (h - f_ui.h) / 2, lab[i], c);
    if (en) hr_add(x, y, w, h, H_CMD, ids[i]);
    x += w + 4;
  }
  fill(0, top_h + cmd_h - 1, fbw, 1, P.bar_line);
}

static void draw_nav(void) {
  int i, y = ly + 6, ih = f_ui.h + 10;
  if (!nav_w) return;
  fill(0, ly, nav_w, fbh - ly - bot_h - stat_h, P.ed_bg);
  fill(nav_w - 1, ly, 1, fbh - ly - bot_h - stat_h, P.bar_line);
  clip_to(0, ly, nav_w - 1, fbh - ly - bot_h - stat_h);
  for (i = 0; i < npl; i++) {
    int on = 0;
    if (pl_kind[i] == 0) {
      if (i) y += 6;
      text_draw(&f_ui, 12, y + (ih - f_ui.h) / 2, pl_lab[i], P.dim);
      y += ih;
      continue;
    }
    {
      /* marcado: o lugar de prefixo mais longo da pasta aberta */
      int n = (int)strlen(pl_path[i]), k, best = 1, j;
      for (k = 0; k < n && up_c((unsigned char)g_path[k]) == up_c((unsigned char)pl_path[i][k]); k++) { }
      if (k == n) {
        for (j = 0; j < npl; j++) {
          int m = (int)strlen(pl_path[j]), q;
          if (j == i || !pl_kind[j] || m <= n) continue;
          for (q = 0; q < m && up_c((unsigned char)g_path[q]) == up_c((unsigned char)pl_path[j][q]); q++) { }
          if (q == m) best = 0;
        }
        on = best;
      }
    }
    if (on) fill(4, y, nav_w - 9, ih, P.list_sel);
    else if (hover(4, y, nav_w - 9, ih)) fill(4, y, nav_w - 9, ih, P.list_hot);
    if (pl_kind[i] == 1) ico_desktop(20, y + ih / 2 - 7, 16);
    else if (pl_kind[i] == 3) ico_drive(20, y + ih / 2 - 8, 16, pl_path[i][0]);
    else ico_folder(20, y + ih / 2 - 7, 16);
    text_fit(&f_ui, 44, y + (ih - f_ui.h) / 2, pl_lab[i], nav_w - 52, P.text);
    hr_add(4, y, nav_w - 9, ih, H_PLACE, i);
    y += ih;
  }
  clip_all();
}

static void draw_list(void) {
  int x = lx, y = ly, w = lw, h = lh, i, sbw = 14, iw;
  int col_name_w;
  static const char* const hn[4] = {"Nome", "Data de modifica\xc3\xa7\xc3\xa3o", "Tipo", "Tamanho"};
  static const int sortk[4] = {0, 2, 3, 1};
  fill(x, y, w, h, P.ed_bg);
  iw = w - (g_nv > g_rows ? sbw : 0);
  /* colunas: tamanho na direita; tipo e data somem em janela estreita */
  col_size_x = x + iw - 96;
  col_type_x = iw >= 620 ? col_size_x - 170 : 0;
  col_date_x = iw >= 470 ? (col_type_x ? col_type_x : col_size_x) - 178 : 0;
  col_name_w = (col_date_x ? col_date_x : col_type_x ? col_type_x : col_size_x) - (x + 34) - 8;
  /* cabecalho */
  {
    int cx[5], k;
    cx[0] = x + 8;
    cx[1] = col_date_x;
    cx[2] = col_type_x;
    cx[3] = col_size_x;
    for (k = 0; k < 4; k++) {
      int x0 = cx[k], x1 = x + iw, j;
      if (!x0) continue;
      for (j = k + 1; j < 4; j++)
        if (cx[j]) {
          x1 = cx[j];
          break;
        }
      if (hover(x0 - 6, y, x1 - x0, head_h)) fill(x0 - 6, y, x1 - x0, head_h, P.list_hot);
      if (k) fill(x0 - 7, y + 4, 1, head_h - 8, P.list_b);
      text_draw(&f_ui, x0 + (k == 0 ? 26 : 0), y + (head_h - f_ui.h) / 2, hn[k], P.head);
      if (g_sort == sortk[k]) {
        int asc = sortk[k] == 0 || sortk[k] == 3 ? !g_rev : g_rev;
        int mx = x0 + (k == 0 ? 26 : 0) + text_w(&f_ui, hn[k]) / 2;
        if (asc) draw_tri_up(mx - 4, y + 1, 4, P.dim);
        else draw_tri_down(mx - 4, y + 1, 4, P.dim);
      }
      hr_add(x0 - 6, y, x1 - x0, head_h, H_HEAD, sortk[k]);
    }
    fill(x, y + head_h - 1, w, 1, P.list_b);
  }
  y += head_h;
  h -= head_h;
  hr_add(x, y, iw, h, H_LIST, 0);
  if (g_top > g_nv - g_rows) g_top = g_nv - g_rows;
  if (g_top < 0) g_top = 0;
  clip_to(x, y, iw, h);
  for (i = 0; i < g_rows + 1 && g_top + i < g_nv; i++) {
    int idx = g_top + i, iy = y + 1 + i * row_h, sel = idx == g_sel, ty = iy + (row_h - f_ui.h) / 2;
    struct k_direntl* e = row_ent(idx);
    char b[120];
    if (sel) {
      fill(x + 2, iy, iw - 4, row_h, P.list_sel);
      if (P.dark == 0) frame(x + 2, iy, iw - 4, row_h, mixc(P.list_sel, P.focus, 70));
    } else if (hover(x + 2, iy, iw - 4, row_h))
      fill(x + 2, iy, iw - 4, row_h, P.list_hot);
    if (e->dir) ico_folder(x + 10, iy + row_h / 2 - 7, 16);
    else ico_file(x + 10, iy + row_h / 2 - 8, 16, e->name);
    if (g_vis[idx] < 0) {
      int tw = text_draw(&f_ui, x + 34, ty, "..", P.text);
      text_draw(&f_ui, x + 34 + tw + 12, ty, "(pasta acima)", P.dim);
    } else if (idx == g_ren) {
      g_ren_x = x + 30;
      g_ren_y = iy + 1;
      g_ren_w = col_name_w + 6;
      g_ren_h = row_h - 2;
      if (g_ren_w < 140) g_ren_w = 140;
    } else
      cp_text(x + 34, ty, e->name, col_name_w, P.text);
    if (g_vis[idx] >= 0) {
      if (col_date_x && e->date) {
        fmt_date(e->date, e->time, b, (int)sizeof(b));
        text_draw(&f_ui, col_date_x, ty, b, P.dim);
      }
      if (col_type_x) {
        type_of(e, b, (int)sizeof(b));
        text_fit(&f_ui, col_type_x, ty, b, 164, P.dim);
      }
      if (!e->dir) {
        size_kb(e->size, b, (int)sizeof(b));
        text_draw(&f_ui, x + iw - 14 - text_w(&f_ui, b), ty, b, P.dim);
      }
    }
    hr_add(x + 2, iy, iw - 4, row_h, H_ROW, idx);
  }
  clip_all();
  if (g_nv == 0 || (g_nv == 1 && g_vis[0] < 0))
    text_draw(&f_ui, x + 34, y + row_h + 8,
              g_fkind ? "Nenhum item corresponde \xc3\xa0 pesquisa." : "Esta pasta est\xc3\xa1 vazia.", P.dim);
  /* campo de renomear por cima da linha */
  if (g_ren >= 0 && g_ren >= g_top && g_ren < g_top + g_rows) {
    edit_draw(&g_ren_edit, g_ren_x, g_ren_y, g_ren_w, g_ren_h, 1, 0,
              g_ren_ext > 0 ? g_ren_ext : -1);
    hr_add(g_ren_x, g_ren_y, g_ren_w, g_ren_h, H_ROW, g_ren);
  }
  /* barra de rolagem */
  if (g_nv > g_rows) {
    int tx = x + w - sbw, tl = h * g_rows / g_nv, tp;
    if (tl < 24) tl = 24;
    tp = (h - tl) * g_top / (g_nv - g_rows);
    fill(tx, y, sbw, h, P.sb_track);
    fill(tx + 3, y + tp + 2, sbw - 6, tl - 4,
         sb_drag ? P.sb_drag : hover(tx, y + tp, sbw, tl) ? P.sb_hot : P.sb_thumb);
    hr_add(tx, y, sbw, h, H_SB, 0);
  }
}

/* parte de baixo dos dialogos: Nome, filtro, extra, botoes */
static void draw_bottom(void) {
  int y = fbh - stat_h - bot_h, x = 12, bh = f_ui.h + 14, bw = 104, labw;
  const char* ok = g_pick == PK_DIR ? "Selecionar pasta" : g_pick == PK_OPEN ? "Abrir" : "Salvar";
  int okw = text_w(&f_ui, ok) + 40;
  if (!g_pick) return;
  if (okw < bw) okw = bw;
  fill(0, y, fbw, bot_h, P.dlg_bg);
  fill(0, y, fbw, 1, P.bar_line);
  labw = text_w(&f_ui, g_nx ? g_xlab : "Nome:") + 14;
  if (labw < text_w(&f_ui, "Pasta:") + 14) labw = text_w(&f_ui, "Pasta:") + 14;
  y += 12;
  if (g_pick == PK_DIR) {
    char t[PATHMAX];
    int fw = fbw - x - labw - 12 - okw - 8 - bw - 12;
    pick_target(t, PATHMAX);
    text_draw(&f_ui, x, y + (bh - f_ui.h) / 2, "Pasta:", P.text);
    fill(x + labw, y, fw, bh, P.field_bg);
    frame(x + labw, y, fw, bh, P.field_b);
    cp_text(x + labw + 6, y + (bh - f_ui.h) / 2, t, fw - 12, P.text);
    btn_draw(fbw - 12 - bw - 8 - okw, y, okw, bh, ok, hover(fbw - 12 - bw - 8 - okw, y, okw, bh), 1, 1);
    hr_add(fbw - 12 - bw - 8 - okw, y, okw, bh, H_OK, 0);
    btn_draw(fbw - 12 - bw, y, bw, bh, "Cancelar", hover(fbw - 12 - bw, y, bw, bh), 0, 1);
    hr_add(fbw - 12 - bw, y, bw, bh, H_CANCEL, 0);
    return;
  }
  {
    int cw = 270, fw = fbw - x - labw - 12 - cw - 12;
    text_draw(&f_ui, x, y + (bh - f_ui.h) / 2, "Nome:", P.text);
    edit_draw(&g_fname, x + labw, y, fw, bh, g_focus == FOC_FNAME, 0, -1);
    hr_add(x + labw, y, fw, bh, H_FNAME, 0);
    if (g_nflt) {
      int cx = fbw - 12 - cw;
      combo_draw(cx, y, cw, bh, g_flt_lab[g_flt], hover(cx, y, cw, bh) || mn_kind == MN_FILTER);
      hr_add(cx, y, cw, bh, H_FILTER, 0);
    }
  }
  y += bh + 12;
  if (g_nx) {
    text_draw(&f_ui, x, y + (bh - f_ui.h) / 2, g_xlab, P.text);
    combo_draw(x + labw, y, 220, bh, g_xopt[g_xsel], hover(x + labw, y, 220, bh) || mn_kind == MN_EXTRA);
    hr_add(x + labw, y, 220, bh, H_EXTRA, 0);
  }
  btn_draw(fbw - 12 - bw - 8 - okw, y, okw, bh, ok, hover(fbw - 12 - bw - 8 - okw, y, okw, bh), 1, 1);
  hr_add(fbw - 12 - bw - 8 - okw, y, okw, bh, H_OK, 0);
  btn_draw(fbw - 12 - bw, y, bw, bh, "Cancelar", hover(fbw - 12 - bw, y, bw, bh), 0, 1);
  hr_add(fbw - 12 - bw, y, bw, bh, H_CANCEL, 0);
}

static void draw_status(void) {
  int y = fbh - stat_h, i, nd = 0, nf = 0, w;
  char b[160];
  fill(0, y, fbw, stat_h, P.stat_bg);
  fill(0, y, fbw, 1, P.stat_line);
  for (i = 0; i < g_nv; i++) {
    if (g_vis[i] < 0) continue;
    if (g_ent[g_vis[i]].dir) nd++;
    else nf++;
  }
  if (nd + nf == 1) snprintf(b, sizeof(b), "1 item");
  else snprintf(b, sizeof(b), "%d itens", nd + nf);
  w = text_draw(&f_ui, 12, y + (stat_h - f_ui.h) / 2, b, P.text);
  {
    struct k_direntl* e = sel_ent();
    if (e) {
      char s[64];
      if (!e->dir) {
        size_kb(e->size, s, (int)sizeof(s));
        snprintf(b, sizeof(b), "1 item selecionado  %s", s);
      } else
        snprintf(b, sizeof(b), "1 item selecionado");
      fill(12 + w + 12, y + 5, 1, stat_h - 10, P.stat_line);
      w += 24 + text_draw(&f_ui, 12 + w + 24, y + (stat_h - f_ui.h) / 2, b, P.text);
    }
  }
  if (g_status[0]) {
    char u[400];
    int sw;
    cp_to_u8(g_status, u, (int)sizeof(u));
    sw = text_w(&f_ui, u);
    if (sw > fbw - (12 + w + 40)) sw = fbw - (12 + w + 40);
    if (sw > 20) text_fit(&f_ui, fbw - 12 - sw, y + (stat_h - f_ui.h) / 2, u, sw, P.dim);
  }
}

static void draw_dialog(void) {
  int nl = 1, mw = 0, k, w, h, x, y, th = f_ui.h + 14, by, bw = 96, bh = f_ui.h + 14;
  char line[200];
  int j, n = 0;
  if (!dk) return;
  for (j = 0;; j++) {
    if (dk_msg[j] == '\n' || !dk_msg[j]) {
      line[n] = 0;
      if (text_w(&f_ui, line) > mw) mw = text_w(&f_ui, line);
      if (!dk_msg[j]) break;
      nl++;
      n = 0;
    } else if (n < 199)
      line[n++] = dk_msg[j];
  }
  w = mw + 60;
  if (w < 360) w = 360;
  if (w > fbw - 20) w = fbw - 20;
  h = th + 24 + nl * (f_ui.h + 4) + 24 + bh + 18;
  x = (fbw - w) / 2;
  y = (fbh - h) / 3;
  /* fundo escurecido de leve: a janela de baixo fica "presa" */
  shadow(x, y, w, h, 6);
  fill(x, y, w, h, P.dlg_bg);
  fill(x, y, w, th, P.dlg_title);
  frame(x, y, w, h, P.dlg_frame);
  text_draw(&f_ui, x + 12, y + (th - f_ui.h) / 2, dk_title, P.text);
  {
    int bx = x + w - 46, on = hover(bx, y, 45, th - 1), cx = bx + 22, cy = y + th / 2;
    if (on) fill(bx, y + 1, 45, th - 2, 0xE81123);
    for (k = -5; k <= 5; k++) {
      fill(cx + k, cy + k, 1, 1, on ? 0xFFFFFF : P.text);
      fill(cx + k, cy - k, 1, 1, on ? 0xFFFFFF : P.text);
    }
    hr_add(bx, y, 45, th, H_DCLOSE, 0);
  }
  fill(x + 1, y + th, w - 2, h - th - bh - 26, P.dlg_body);
  n = 0;
  {
    int ly2 = y + th + 22;
    for (j = 0;; j++) {
      if (dk_msg[j] == '\n' || !dk_msg[j]) {
        line[n] = 0;
        text_draw(&f_ui, x + 24, ly2, line, dk == DK_MSG ? P.text : P.ask);
        ly2 += f_ui.h + 4;
        n = 0;
        if (!dk_msg[j]) break;
      } else if (n < 199)
        line[n++] = dk_msg[j];
    }
  }
  by = y + h - bh - 13;
  if (dk == DK_MSG) {
    btn_draw(x + w - 12 - bw, by, bw, bh, "OK", hover(x + w - 12 - bw, by, bw, bh), 1, 1);
    hr_add(x + w - 12 - bw, by, bw, bh, H_DYES, 0);
  } else {
    btn_draw(x + w - 12 - bw - 8 - bw, by, bw, bh, "Sim", hover(x + w - 12 - bw - 8 - bw, by, bw, bh), dk == DK_DEL, 1);
    hr_add(x + w - 12 - bw - 8 - bw, by, bw, bh, H_DYES, 0);
    btn_draw(x + w - 12 - bw, by, bw, bh, "N\xc3\xa3o", hover(x + w - 12 - bw, by, bw, bh), dk == DK_OVER, 1);
    hr_add(x + w - 12 - bw, by, bw, bh, H_DNO, 0);
  }
  /* o dialogo e o unico que recebe clique */
  {
    int i, keep = 0;
    for (i = 0; i < nhr; i++)
      if (hrs[i].id >= H_DYES) hrs[keep++] = hrs[i];
    nhr = keep;
  }
}

static void render(void) {
  nhr = 0;
  clip_all();
  ui_geometry();
  fill(0, 0, fbw, fbh, P.ed_bg);
  draw_top();
  draw_cmdbar();
  draw_nav();
  draw_list();
  draw_bottom();
  draw_status();
  if (mn_kind) {
    struct pg g;
    pop_geom(mn_items, mn_x, mn_y, &g);
    draw_pop(mn_items, &g, mn_hot, -1);
  }
  draw_dialog();
  clip_all();
}

static int fb_alloc(void) {
  int w = 0, h = 0;
  unsigned* n;
  k_gui_size(&w, &h);
  if (w < 64) w = 64;
  if (h < 64) h = 64;
  if (fb && w == fbw && h == fbh) return 1;
  n = (unsigned*)malloc((size_t)w * (size_t)h * 4);
  if (!n) return 0;
  if (fb) free(fb);
  fb = n;
  fbw = w;
  fbh = h;
  clip_all();
  return 1;
}

static int wait_ev(gui_event_t* ev, int ms) {
  int raw[4];
  int t = k_gui_event(raw, ms);
  if (t < 0) {
    ev->type = -1;
    return -1;
  }
  ev->type = raw[0];
  ev->x = raw[1];
  ev->y = raw[2];
  ev->key = raw[3];
  return ev->type;
}

/* ------------------------------------------------------------- argumentos */
/* campo k (0..) de "a|b|c" para out */
static void arg_field(const char* a, int k, char* out, int max) {
  int i = 0;
  while (k > 0 && *a) {
    if (*a == '|') k--;
    a++;
  }
  while (*a && *a != '|' && i < max - 1) out[i++] = *a++;
  out[i] = 0;
}

/* "/PICK|...", "/OPEN|...", "/SAVE|..." ou /PICK saida [pasta] */
static int pick_args(int argc, char** argv, char* init, int imax) {
  const char* a = argc > 1 ? argv[1] : 0;
  int mode = 0;
  char word[8];
  int i;
  init[0] = 0;
  if (!a || (a[0] != '/' && a[0] != '-')) return 0;
  for (i = 0; i < 4 && a[1 + i]; i++) word[i] = (char)up_c((unsigned char)a[1 + i]);
  word[i] = 0;
  if (!strcmp(word, "PICK")) mode = PK_DIR;
  else if (!strcmp(word, "OPEN")) mode = PK_OPEN;
  else if (!strcmp(word, "SAVE")) mode = PK_SAVE;
  else return 0;
  if (a[5] == '|') {
    char f[700];
    arg_field(a, 1, g_pick_out, PATHMAX);
    arg_field(a, 2, init, imax);
    arg_field(a, 3, g_pick_title, (int)sizeof(g_pick_title));
    if (mode >= PK_OPEN) {
      const char* p;
      arg_field(a, 4, f, (int)sizeof(f));
      p = f;
      g_nflt = 0;
      while (*p && g_nflt < 8) {
        int k = 0;
        char item[140];
        char* eq;
        while (*p && *p != ';' && k < 139) item[k++] = *p++;
        item[k] = 0;
        if (*p == ';') p++;
        eq = strrchr(item, '=');
        if (!eq) continue;
        *eq = 0;
        scopy(g_flt_lab[g_nflt], item, 72);
        scopy(g_flt_pat[g_nflt], eq + 1, 64);
        g_nflt++;
      }
      arg_field(a, 5, f, (int)sizeof(f));
      gui_edit_set(&g_fname, f);
      if (mode == PK_SAVE) {
        /* "Rotulo=op1,op2,...=indice" */
        char* e1;
        char* e2;
        arg_field(a, 6, f, (int)sizeof(f));
        e1 = strchr(f, '=');
        e2 = e1 ? strrchr(e1 + 1, '=') : 0;
        if (e1 && e2) {
          char* q = e1 + 1;
          *e1 = 0;
          *e2 = 0;
          scopy(g_xlab, f, (int)sizeof(g_xlab));
          g_nx = 0;
          while (*q && g_nx < 8) {
            int k = 0;
            while (*q && *q != ',' && k < 39) g_xopt[g_nx][k++] = *q++;
            g_xopt[g_nx][k] = 0;
            if (*q == ',') q++;
            g_nx++;
          }
          g_xsel = clampi(atoi(e2 + 1), 0, g_nx > 0 ? g_nx - 1 : 0);
        }
      }
    }
  } else if (mode == PK_DIR) {
    if (argc > 2 && argv[2]) copy_str(g_pick_out, argv[2], PATHMAX);
    if (argc > 3 && argv[3]) copy_str(init, argv[3], imax);
  }
  return mode;
}

/* ------------------------------------------------------------- eventos */
static unsigned last_ms;
static int last_row = -1;

static void scroll_to_sel(void) {
  if (g_follow && g_sel >= 0) {
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + g_rows) g_top = g_sel - g_rows + 1;
  }
  g_follow = 0;
  if (g_top > g_nv - g_rows) g_top = g_nv - g_rows;
  if (g_top < 0) g_top = 0;
}

static void dk_answer(int yes) {
  int kind = dk;
  dk = 0;
  if (kind == DK_DEL) {
    if (yes) {
      g_confirm_del = 1;
      delete_sel();
    } else
      set_status("exclusao cancelada");
  } else if (kind == DK_OVER) {
    if (yes) {
      pick_answer_file(g_target);
      run = 0;
    }
  }
}

static void on_down(int x, int y, int button) {
  int arg = 0, id;
  /* menu aberto: escolhe ou fecha */
  if (mn_kind) {
    struct pg g;
    int h;
    pop_geom(mn_items, mn_x, mn_y, &g);
    h = pop_hit(mn_items, &g, x, y);
    if (h >= 0) cmd_run(mn_items[h].id);
    else mn_kind = 0;
    return;
  }
  id = hr_find(x, y, &arg);
  if (dk) {
    if (id == H_DYES) dk_answer(1);
    else if (id == H_DNO || id == H_DCLOSE) dk_answer(0);
    return;
  }
  /* renomeando: clique no campo move o cursor; fora dele grava */
  if (g_ren >= 0) {
    if (x >= g_ren_x && x < g_ren_x + g_ren_w && y >= g_ren_y && y < g_ren_y + g_ren_h) {
      edit_click(&g_ren_edit, g_ren_x, x);
      g_ren_ext = -1;
      return;
    }
    commit_rename();
    id = hr_find(x, y, &arg);
  }
  if (button == 2) {
    if (id == H_ROW) {
      g_sel = arg;
      g_confirm_del = 0;
      ctx_open_at(x, y, !is_up_row(arg));
    } else if (id == H_LIST) {
      g_sel = -1;
      ctx_open_at(x, y, 0);
    }
    return;
  }
  if (id != H_ADDR && g_focus == FOC_PATH) focus_set(g_pick >= PK_OPEN ? FOC_FNAME : FOC_NAME);
  switch (id) {
    case H_BACK:
      if (g_hpos > 0) {
        g_hpos--;
        go(g_hist[g_hpos], 0);
      }
      break;
    case H_FWD:
      if (g_hpos < g_hn - 1) {
        g_hpos++;
        go(g_hist[g_hpos], 0);
      }
      break;
    case H_UP: go_up(); break;
    case H_REF:
      load_dir(g_path);
      set_status("lista atualizada");
      break;
    case H_ADDR:
      if (g_focus != FOC_PATH) {
        focus_set(FOC_PATH);
        gui_edit_set(&g_edit, g_path);
        gui_edit_select_all(&g_edit);
      } else {
        int k;
        for (k = 0; k < nhr; k++)
          if (hrs[k].id == H_ADDR) edit_click(&g_edit, hrs[k].x, x);
      }
      break;
    case H_SEARCH:
    case H_FNAME: {
      int k, want = id == H_SEARCH ? H_SEARCH : H_FNAME;
      gui_edit_t* e = id == H_SEARCH ? &g_name : &g_fname;
      focus_set(id == H_SEARCH ? FOC_NAME : FOC_FNAME);
      for (k = 0; k < nhr; k++)
        if (hrs[k].id == want) edit_click(e, hrs[k].x, x);
      break;
    }
    case H_CMD: cmd_run(arg); break;
    case H_PLACE:
      if (pl_path[arg][0]) go(pl_path[arg], 1);
      break;
    case H_HEAD:
      if (g_sort == arg) g_rev = !g_rev;
      else {
        g_sort = arg;
        g_rev = 0;
      }
      sort_entries();
      rebuild_view(0);
      break;
    case H_ROW: {
      unsigned now = k_ms();
      int dbl = last_row == arg && now - last_ms < 480;
      last_row = arg;
      last_ms = now;
      g_sel = arg;
      g_confirm_del = 0;
      if (g_pick >= PK_OPEN) {
        struct k_direntl* e = sel_ent();
        if (e && !e->dir) gui_edit_set(&g_fname, e->name);
      }
      if (dbl) {
        last_row = -1;
        cmd_run(CM_OPEN);
      }
      break;
    }
    case H_LIST:
      g_sel = -1;
      break;
    case H_SB: {
      int y0 = ly + head_h, h = lh - head_h, tl = h * g_rows / (g_nv > 0 ? g_nv : 1), tp;
      if (tl < 24) tl = 24;
      tp = g_nv > g_rows ? (h - tl) * g_top / (g_nv - g_rows) : 0;
      if (y >= y0 + tp && y < y0 + tp + tl) {
        sb_drag = 1;
        sb_off = y - (y0 + tp);
      } else if (y < y0 + tp)
        g_top -= g_rows - 1;
      else
        g_top += g_rows - 1;
      break;
    }
    case H_FILTER: {
      int k;
      for (k = 0; k < nhr; k++)
        if (hrs[k].id == H_FILTER) combo_menu(MN_FILTER, hrs[k].x, hrs[k].y + hrs[k].h);
      break;
    }
    case H_EXTRA: {
      int k;
      for (k = 0; k < nhr; k++)
        if (hrs[k].id == H_EXTRA) combo_menu(MN_EXTRA, hrs[k].x, hrs[k].y + hrs[k].h);
      break;
    }
    case H_OK:
      if (g_pick == PK_DIR) {
        pick_answer(1);
        run = 0;
      } else
        file_accept();
      break;
    case H_CANCEL:
      pick_answer(0);
      run = 0;
      break;
  }
}

static void on_drag(int y) {
  int y0 = ly + head_h, h = lh - head_h, tl, span;
  if (!sb_drag || g_nv <= g_rows) return;
  tl = h * g_rows / g_nv;
  if (tl < 24) tl = 24;
  span = h - tl;
  if (span < 1) span = 1;
  g_top = clampi((y - sb_off - y0) * (g_nv - g_rows) / span, 0, g_nv - g_rows);
}

static void menu_key(int k) {
  int n, i, s = mn_hot;
  for (n = 0; mn_items[n].t; n++) { }
  if (k == 27) {
    mn_kind = 0;
    return;
  }
  if (k == GUI_KEY_DOWN || k == GUI_KEY_UP) {
    for (i = 0; i < n; i++) {
      s = (s + (k == GUI_KEY_DOWN ? 1 : -1) + n) % n;
      if (!(mn_items[s].t[0] == '-' && !mn_items[s].t[1])) break;
    }
    mn_hot = s;
    return;
  }
  if ((k == '\r' || k == '\n' || k == ' ') && mn_hot >= 0) cmd_run(mn_items[mn_hot].id);
}

/* tecla; 1 = fechar a janela */
static int on_key(int k, int mods) {
  if (dk) {
    if (k == 27 || k == 'n' || k == 'N') dk_answer(dk == DK_MSG);
    else if (k == '\r' || k == '\n') dk_answer(dk != DK_OVER);
    else if (k == 's' || k == 'S') dk_answer(1);
    return 0;
  }
  if (mn_kind) {
    menu_key(k);
    return 0;
  }
  if (g_ren >= 0) {
    if (k == '\r' || k == '\n') commit_rename();
    else if (k == 27) {
      cancel_rename();
      set_status("renomear cancelado");
    } else
      ren_key(k);
    return 0;
  }
  if (k == GUI_KEY_F1 + 1) {   /* F2 */
    if (sel_ent()) start_rename();
    return 0;
  }
  if (k == GUI_KEY_F1 + 4) {   /* F5 */
    load_dir(g_path);
    set_status("lista atualizada");
    return 0;
  }
  if (g_focus == FOC_PATH) {
    if (k == '\r' || k == '\n') {
      if (nav_path(g_edit.text)) focus_set(g_pick >= PK_OPEN ? FOC_FNAME : FOC_NAME);
    } else if (k == 27 || k == '\t')
      focus_set(g_pick >= PK_OPEN ? FOC_FNAME : FOC_NAME);
    else
      gui_edit_key(&g_edit, k);
    return 0;
  }
  if (g_focus == FOC_FNAME) {
    if (k == '\r' || k == '\n') file_accept();
    else if (k == 27) {
      pick_answer(0);
      return 1;
    } else if (k == '\t') {
      focus_set(FOC_NAME);
    } else if (k == GUI_KEY_UP || k == GUI_KEY_DOWN || k == GUI_KEY_PGUP || k == GUI_KEY_PGDN) {
      struct k_direntl* e;
      list_key(k, g_rows);
      e = sel_ent();
      if (e && !e->dir) gui_edit_set(&g_fname, e->name);
    } else
      gui_edit_key(&g_fname, k);
    return 0;
  }
  /* caixa Pesquisar (o normal) */
  if (k == '\t') {
    if (g_pick >= PK_OPEN) focus_set(FOC_FNAME);
    else {
      focus_set(FOC_PATH);
      gui_edit_select_all(&g_edit);
    }
    return 0;
  }
  if (g_pick && k == 27 && g_name.len == 0) {
    pick_answer(0);
    return 1;
  }
  if (g_pick == PK_DIR && (k == '\r' || k == '\n') && !g_name.len && !sel_ent()) {
    pick_answer(1);
    return 1;
  }
  (void)mods;
  return name_key(k, g_rows);
}

int main(int argc, char** argv) {
  gui_event_t ev;
  char lastpath[PATHMAX];
  char pick_init[PATHMAX];
  int scr_w = 0, scr_h = 0, need = 1;
  int (*hov)(int) = 0;
  const char* title;

  if (argc > 1 && argv[1] && argv[1][0] == '@') {
    /* argumento longo num arquivo (o WIN corta argumentos em 192 bytes) */
    static char argf[1800];
    int n = k_fload(argv[1] + 1, argf, (int)sizeof(argf));
    if (n > 0) {
      if (n >= (int)sizeof(argf)) n = (int)sizeof(argf) - 1;
      argf[n] = 0;
      while (n > 0 && (argf[n - 1] == '\r' || argf[n - 1] == '\n')) argf[--n] = 0;
      k_fdelete(argv[1] + 1);
      argv[1] = argf;
    }
  }
  g_pick = pick_args(argc, argv, pick_init, PATHMAX);
  if (!g_pick && argc > 2 && argv[1] && (argv[1][0] == '/' || argv[1][0] == '-') &&
      (argv[1][1] == 'P' || argv[1][1] == 'p') && argv[2] && argv[2][0])
    g_folder = 1;
  title = g_pick_title[0] ? g_pick_title
          : g_pick == PK_DIR ? "Selecionar pasta"
          : g_pick == PK_OPEN ? "Abrir"
          : g_pick == PK_SAVE ? "Salvar como" : "Explorer";
  {
    /* o titulo do WIN so tem ASCII */
    char t[80];
    int i;
    scopy(t, title, (int)sizeof(t));
    for (i = 0; t[i]; i++)
      if ((unsigned char)t[i] >= 127) t[i] = '?';
    if (gui_open_pct(t, g_pick ? 62 : 70, g_pick ? 66 : 64) != 0) {
      pick_answer(0);
      printf("EXPLORER: nao abriu a janela\n");
      return 1;
    }
  }
  hov = (int (*)(int))k_dl_sym(0, "k_gui_hover", 0);
  if (hov) hov(1);
  k_win_size(&scr_w, &scr_h);
  pal_sync();
  ui_font_init(scr_h >= 1000 ? 16 : scr_h >= 700 ? 15 : 13);
  if (!fb_alloc()) {
    pick_answer(0);
    gui_close();
    return 1;
  }
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
  if (g_pick) {
    /* pasta inicial que nao existe: a mais proxima acima que existe */
    if (pick_init[0]) {
      char t[PATHMAX];
      path_resolve(pick_init, t);
      while ((int)strlen(t) > 3 && !dir_exists(t)) path_parent(t);
      if (dir_exists(t)) copy_str(g_path, t, PATHMAX);
    } else {
      char d[PATHMAX];
      if (desktop_dir(d, PATHMAX) == 0) path_resolve(d, g_path);
    }
  } else if (g_folder) {
    path_resolve(argv[2], g_path);
    gui_title(g_path);
  } else if (argc > 1 && argv[1] && argv[1][0]) {
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
  g_focus = g_pick >= PK_OPEN ? FOC_FNAME : FOC_NAME;
  if (g_pick >= PK_OPEN) {
    /* o nome do argumento vem em UTF-8 (Sem titulo.txt): o FAT usa CP850 */
    char c[PATHMAX];
    u8_to_cp(g_fname.text, c, PATHMAX);
    gui_edit_set(&g_fname, c);
    gui_edit_select_all(&g_fname);
  }
  places_init();
  hist_push(g_path);
  load_dir(g_path);

  while (run) {
    int t;
    if (need) {
      ui_geometry();
      scroll_to_sel();
      render();
      gui_pixels(0, 0, fbw, fbh, fb, fbw * 4);
      gui_present();
      need = 0;
    }
    t = wait_ev(&ev, 600);
    if (t < 0) break;
    if (pal_sync()) need = 1;
    if (t == GUI_EV_NONE) continue;
    do {
      if (ev.type == GUI_EV_CLOSE) {
        pick_answer(0);
        run = 0;
      } else if (ev.type == GUI_EV_RESIZE) {
        fb_alloc();
      } else if (ev.type == GUI_EV_MOVE || ev.type == GUI_EV_DRAG) {
        g_mx = ev.x;
        g_my = ev.y;
        if (sb_drag) on_drag(ev.y);
        if (mn_kind) {
          struct pg g;
          int h;
          pop_geom(mn_items, mn_x, mn_y, &g);
          h = pop_hit(mn_items, &g, ev.x, ev.y);
          if (h != -2) mn_hot = h;
        }
      } else if (ev.type == GUI_EV_UP) {
        sb_drag = 0;
      } else if (ev.type == GUI_EV_DOWN) {
        g_mx = ev.x;
        g_my = ev.y;
        on_down(ev.x, ev.y, ev.key);
      } else if (ev.type == GUI_EV_WHEEL) {
        if (!mn_kind && !dk) g_top = clampi(g_top + ev.key * 3, 0, g_nv - g_rows > 0 ? g_nv - g_rows : 0);
      } else if (ev.type == GUI_EV_KEY) {
        if (on_key(ev.key, ev.x)) run = 0;
      }
      if (!run) break;
      t = wait_ev(&ev, 0);
      if (t < 0) run = 0;
    } while (t > 0);
    need = 1;
  }
  pick_answer(0); /* fechou a janela ou Esc: cancelado */
  gui_close();
  return 0;
}
