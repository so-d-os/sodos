/* panel.c - Os dois paineis: ler a pasta, ordenar, desenhar e mover o cursor.
 * Cada painel tem 40 colunas: Nome (17) | Tamanho (7) | Modificado (12). */
#include "mc.h"

struct panel pan[2];
int act;
int today;

static struct k_dirent kd[ENT_MAX];
static int sort_mode;

static const char* months[12] = {
  "jan", "fev", "mar", "abr", "mai", "jun", "jul", "ago", "set", "out", "nov", "dez"
};

int is_root(const char* path) {
  return path[0] && path[1] == ':' && (path[2] == 0 || (path[2] == '\\' && path[3] == 0));
}

int path_join(char* out, int max, const char* dir, const char* name) {
  int a = (int)strlen(dir), b = (int)strlen(name);
  int sep = (a > 0 && dir[a - 1] != '\\') ? 1 : 0;
  if (a + sep + b + 1 > max) return -1;
  memcpy(out, dir, (size_t)a);
  if (sep) out[a++] = '\\';
  memcpy(out + a, name, (size_t)b + 1);
  return 0;
}

static const char* ext_of(const char* name) {
  const char* d = 0;
  for (; *name; name++)
    if (*name == '.') d = name + 1;
  return d ? d : "";
}

static int ext_in(const char* ext, const char* list) {
  /* list: "EXE BAT COM" */
  while (*list) {
    int i = 0;
    while (list[i] && list[i] != ' ' && ext[i] && scr_cp850((unsigned char)ext[i]) == list[i]) i++;
    if ((list[i] == ' ' || list[i] == 0) && ext[i] == 0) return 1;
    while (*list && *list != ' ') list++;
    while (*list == ' ') list++;
  }
  return 0;
}

int is_exec(const char* name) {
  return ext_in(ext_of(name), "EXE BAT COM");
}

int name_color(const struct ent* e) {
  const char* x;
  if (e->dir) return C_DIR;
  x = ext_of(e->name);
  if (ext_in(x, "EXE BAT COM")) return C_EXE;
  if (ext_in(x, "DLL OBJ IMP LIB")) return C_LIB;
  if (ext_in(x, "ZIP GZ TGZ TAR BZ2 XZ 7Z RAR ARJ LZH")) return C_ARCH;
  if (ext_in(x, "TXT MD DOC PDF HTM HTML RTF")) return C_DOC;
  if (ext_in(x, "C H CPP HPP ASM S INC JS PY MAK")) return C_SRC;
  if (ext_in(x, "PNG JPG JPE JPEG GIF BMP ICO THM PCX TGA")) return C_IMG;
  if (ext_in(x, "AVI MP4 MKV WAV MP3 OGG MID MJP")) return C_MEDIA;
  if (ext_in(x, "TMP BAK LOG OLD $$$")) return C_TEMP;
  return C_PANEL;
}

/* ---------- ordem ---------- */

static int name_cmp(const char* a, const char* b) {
  for (;;) {
    int x = scr_cp850((unsigned char)*a), y = scr_cp850((unsigned char)*b);
    if (x != y || !x) return x - y;
    a++;
    b++;
  }
}

int name_eq_path(const char* a, const char* b) {
  return name_cmp(a, b) == 0;
}

static int ent_cmp(const void* va, const void* vb) {
  const struct ent* a = (const struct ent*)va;
  const struct ent* b = (const struct ent*)vb;
  int r = 0;
  if (a->dir != b->dir) return b->dir - a->dir;
  if (sort_mode == SORT_EXT) {
    r = name_cmp(ext_of(a->name), ext_of(b->name));
  } else if (sort_mode == SORT_SIZE && !a->dir) {
    if (a->size != b->size) return a->size > b->size ? -1 : 1;
  } else if (sort_mode == SORT_TIME) {
    unsigned ta = ((unsigned)a->date << 16) | a->time;
    unsigned tb = ((unsigned)b->date << 16) | b->time;
    if (ta != tb) return ta > tb ? -1 : 1;
  }
  if (r) return r;
  return name_cmp(a->name, b->name);
}

static void panel_sort(struct panel* p) {
  int s = (p->n > 0 && p->e[0].up) ? 1 : 0;
  sort_mode = p->sort;
  if (p->n - s > 1) qsort(p->e + s, (size_t)(p->n - s), sizeof(struct ent), ent_cmp);
}

/* ---------- ler ---------- */

void panel_init(struct panel* p, int x, const char* path) {
  memset(p, 0, sizeof(*p));
  p->x = x;
  p->e = (struct ent*)malloc(sizeof(struct ent) * ENT_MAX);
  panel_chdir(p, path);
}

void panel_load(struct panel* p) {
  int n, i;
  p->n = 0;
  p->cur = 0;
  p->top = 0;
  p->bad = 0;
  if (!p->e) return;
  if (!is_root(p->path)) {
    memset(&p->e[0], 0, sizeof(struct ent));
    strcpy(p->e[0].name, "..");
    p->e[0].dir = 1;
    p->e[0].up = 1;
    p->n = 1;
  }
  n = k_dir(p->path, kd, ENT_MAX - 1);
  if (n < 0) {
    p->bad = 1;
    n = 0;
  }
  for (i = 0; i < n && p->n < ENT_MAX; i++) {
    struct ent* e = &p->e[p->n++];
    memset(e, 0, sizeof(*e));
    memcpy(e->name, kd[i].name, sizeof(e->name));
    e->name[sizeof(e->name) - 1] = 0;
    e->size = kd[i].size;
    e->date = kd[i].date;
    e->time = kd[i].time;
    e->dir = kd[i].dir ? 1 : 0;
  }
  panel_sort(p);
}

static void panel_goto_name(struct panel* p, const char* name) {
  int i;
  for (i = 0; i < p->n; i++)
    if (name_cmp(p->e[i].name, name) == 0) {
      p->cur = i;
      return;
    }
}

void panel_reload(struct panel* p) {
  char keep[32];
  int old = p->cur;
  keep[0] = 0;
  if (p->cur < p->n) strcpy(keep, p->e[p->cur].name);
  panel_load(p);
  p->cur = old < p->n ? old : (p->n ? p->n - 1 : 0);
  if (keep[0]) panel_goto_name(p, keep);
}

void panel_chdir(struct panel* p, const char* path) {
  char t[PATH_MAX_MC];
  int n;
  strncpy(t, path, PATH_MAX_MC - 1);
  t[PATH_MAX_MC - 1] = 0;
  for (n = 0; t[n]; n++)
    if (t[n] >= 'a' && t[n] <= 'z') t[n] = (char)(t[n] - 32);
  if (n == 2 && t[1] == ':') {
    t[2] = '\\';
    t[3] = 0;
  }
  while (n > 3 && t[n - 1] == '\\') t[--n] = 0;
  strcpy(p->path, t);
  panel_load(p);
}

void panel_up(struct panel* p) {
  char child[32];
  int n = (int)strlen(p->path);
  if (is_root(p->path)) return;
  while (n > 0 && p->path[n - 1] != '\\') n--;
  strncpy(child, p->path + n, sizeof(child) - 1);
  child[sizeof(child) - 1] = 0;
  if (n <= 3) p->path[3] = 0;   /* "U:\" */
  else p->path[n - 1] = 0;
  panel_load(p);
  panel_goto_name(p, child);
}

void panel_enter_dir(struct panel* p, const char* name) {
  char t[PATH_MAX_MC];
  if (strcmp(name, "..") == 0) {
    panel_up(p);
    return;
  }
  if (path_join(t, sizeof(t), p->path, name) != 0) return;
  strcpy(p->path, t);
  panel_load(p);
}

struct ent* panel_cur(struct panel* p) {
  if (p->cur < 0 || p->cur >= p->n) return 0;
  return &p->e[p->cur];
}

int panel_marked(struct panel* p, unsigned* bytes) {
  int i, n = 0;
  unsigned b = 0;
  for (i = 0; i < p->n; i++)
    if (p->e[i].mark) {
      n++;
      b += p->e[i].size;
    }
  if (bytes) *bytes = b;
  return n;
}

void panel_key_move(struct panel* p, int key) {
  if (key == K_UP) p->cur--;
  else if (key == K_DOWN) p->cur++;
  else if (key == K_PGUP) p->cur -= PANEL_ROWS - 1;
  else if (key == K_PGDN) p->cur += PANEL_ROWS - 1;
  else if (key == K_HOME) p->cur = 0;
  else if (key == K_END) p->cur = p->n - 1;
  if (p->cur >= p->n) p->cur = p->n - 1;
  if (p->cur < 0) p->cur = 0;
}

/* ---------- * e ? ---------- */

static int glob_at(const char* p, const char* s) {
  for (;;) {
    if (*p == '*') {
      while (*p == '*') p++;
      if (!*p) return 1;
      for (; *s; s++)
        if (glob_at(p, s)) return 1;
      return 0;
    }
    if (!*s) return !*p;
    if (*p != '?' && scr_cp850((unsigned char)*p) != scr_cp850((unsigned char)*s)) return 0;
    p++;
    s++;
  }
}

/* "*.*" casa tambem com nome sem ponto, como no DOS */
int glob_match(const char* pat, const char* name) {
  if (strcmp(pat, "*.*") == 0) return 1;
  return glob_at(pat, name);
}

void panel_mark_pattern(struct panel* p, const char* pat, int on) {
  int i;
  for (i = 0; i < p->n; i++) {
    if (p->e[i].up || p->e[i].dir) continue;
    if (glob_match(pat, p->e[i].name)) p->e[i].mark = (char)on;
  }
}

/* ---------- desenho ---------- */

static void fmt_col_size(char* out, const struct ent* e) {
  unsigned v = e->size;
  if (e->up) strcpy(out, "UP--DIR");
  else if (e->dir) strcpy(out, "<DIR>");
  else if (v < 10000000u) sprintf(out, "%u", v);
  else if (v / 1024u < 1000000u) sprintf(out, "%uK", v / 1024u);
  else sprintf(out, "%uM", v / (1024u * 1024u));
}

static void fmt_col_time(char* out, unsigned date, unsigned time) {
  int d = (int)(date & 31u), m = (int)((date >> 5) & 15u), y = 1980 + (int)(date >> 9);
  if (!date || d == 0 || m < 1 || m > 12) {
    out[0] = 0;
    return;
  }
  if (today && y == today / 10000)
    sprintf(out, "%s %02d %02u:%02u", months[m - 1], d, time >> 11, (time >> 5) & 63u);
  else
    sprintf(out, "%s %02d  %4d", months[m - 1], d, y);
}

static void fmt_short(char* out, unsigned kb) {
  if (kb >= 1024u * 1024u) sprintf(out, "%u,%uG", kb / (1024u * 1024u), (kb % (1024u * 1024u)) * 10u / (1024u * 1024u));
  else if (kb >= 1024u) sprintf(out, "%u,%uM", kb / 1024u, (kb % 1024u) * 10u / 1024u);
  else sprintf(out, "%uK", kb);
}

static void put_right(int y, int x, int w, const char* s, int color) {
  int l = (int)strlen(s), i;
  for (i = 0; i < w; i++) scr_put(y, x + i, ' ', color);
  if (l > w) {
    s += l - w;
    l = w;
  }
  scr_text(y, x + w - l, s, color);
}

static void put_center(int y, int x, int w, const char* s, int color) {
  int l = (int)strlen(s), i;
  for (i = 0; i < w; i++) scr_put(y, x + i, ' ', color);
  if (l > w) l = w;
  for (i = 0; i < l; i++) scr_put(y, x + (w - l) / 2 + i, (unsigned char)s[i], color);
}

static void draw_row(struct panel* p, int row, int active) {
  int x = p->x, y = 3 + row, i = p->top + row;
  struct ent* e;
  int color, sep;
  char buf[40];
  scr_put(y, x, 0xB3, C_PANEL);
  scr_put(y, x + 39, 0xB3, C_PANEL);
  if (i >= p->n) {
    scr_fill(y, x + 1, 1, 38, ' ', C_PANEL);
    scr_put(y, x + 18, 0xB3, C_PANEL);
    scr_put(y, x + 26, 0xB3, C_PANEL);
    return;
  }
  e = &p->e[i];
  color = e->mark ? C_MARK : name_color(e);
  sep = C_PANEL;
  if (active && i == p->cur) {
    color = e->mark ? C_MARKSEL : C_SEL;
    sep = color;
  }
  buf[0] = e->dir ? '/' : (is_exec(e->name) ? '*' : ' ');
  if (e->up) buf[0] = '/';
  strncpy(buf + 1, e->name, 30);
  buf[31] = 0;
  if ((int)strlen(buf) > 17) {
    buf[16] = '~';
    buf[17] = 0;
  }
  scr_textw(y, x + 1, buf, 17, color);
  scr_put(y, x + 18, 0xB3, sep);
  fmt_col_size(buf, e);
  put_right(y, x + 19, 7, buf, color);
  scr_put(y, x + 26, 0xB3, sep);
  fmt_col_time(buf, e->date, e->time);
  scr_textw(y, x + 27, buf, 12, color);
}

void panel_draw(struct panel* p, int active) {
  int x = p->x, i, nm;
  unsigned kb_total, kb_free, bytes;
  char t[PATH_MAX_MC + 8];
  const char* path = p->path;
  int pl;

  if (p->cur < p->top) p->top = p->cur;
  if (p->cur >= p->top + PANEL_ROWS) p->top = p->cur - PANEL_ROWS + 1;
  if (p->top < 0) p->top = 0;

  /* moldura */
  scr_box(1, x, 22, 40, C_PANEL, 0);
  scr_put(1, x + 18, 0xC2, C_PANEL);
  scr_put(1, x + 26, 0xC2, C_PANEL);
  scr_put(20, x, 0xC3, C_PANEL);
  scr_put(20, x + 39, 0xB4, C_PANEL);
  for (i = 1; i < 39; i++) scr_put(20, x + i, 0xC4, C_PANEL);
  scr_put(20, x + 18, 0xC1, C_PANEL);
  scr_put(20, x + 26, 0xC1, C_PANEL);

  /* caminho no alto */
  pl = (int)strlen(path);
  if (pl > 32) {
    sprintf(t, " ...%s ", path + pl - 29);
  } else {
    sprintf(t, " %s ", path);
  }
  scr_text(1, x + 2, t, active ? C_TITLE : C_PANEL);

  /* cabecalho */
  scr_put(2, x, 0xB3, C_PANEL);
  scr_put(2, x + 39, 0xB3, C_PANEL);
  put_center(2, x + 1, 17, p->sort == SORT_NAME ? "Nome \x19" : (p->sort == SORT_EXT ? "Ext \x19" : "Nome"), C_HEAD);
  scr_put(2, x + 18, 0xB3, C_PANEL);
  put_center(2, x + 19, 7, p->sort == SORT_SIZE ? "Tam.\x19" : "Tamanho", C_HEAD);
  scr_put(2, x + 26, 0xB3, C_PANEL);
  put_center(2, x + 27, 12, p->sort == SORT_TIME ? "Modific. \x19" : "Modificado", C_HEAD);

  for (i = 0; i < PANEL_ROWS; i++) draw_row(p, i, active);

  /* linha de baixo: arquivo atual ou marcados */
  scr_put(21, x, 0xB3, C_PANEL);
  scr_put(21, x + 39, 0xB3, C_PANEL);
  nm = panel_marked(p, &bytes);
  if (nm) {
    char b[16];
    fmt_size(b, bytes);
    sprintf(t, "%s bytes em %d arquivo%s", b, nm, nm > 1 ? "s" : "");
    put_center(21, x + 1, 38, t, C_MARK);
  } else if (p->bad) {
    put_center(21, x + 1, 38, "pasta nao abriu", C_PANEL);
  } else if (p->cur < p->n) {
    struct ent* e = &p->e[p->cur];
    char sz[16];
    if (e->dir) strcpy(sz, e->up ? "UP--DIR" : "<DIR>");
    else fmt_size(sz, e->size);
    scr_textw(21, x + 1, e->name, 38, C_PANEL);
    put_right(21, x + 38 - (int)strlen(sz), (int)strlen(sz), sz, C_PANEL);
  } else {
    scr_fill(21, x + 1, 1, 38, ' ', C_PANEL);
  }

  /* espaco livre no canto de baixo */
  if (k_dfree(p->path[0], &kb_total, &kb_free) == 0 && kb_total) {
    char a[16], b[16];
    fmt_short(a, kb_free);
    fmt_short(b, kb_total);
    sprintf(t, " %s/%s (%u%%) ", a, b, (unsigned)((kb_free / 1024u) * 100u / (kb_total / 1024u ? kb_total / 1024u : 1u)));
    scr_text(22, x + 38 - (int)strlen(t), t, C_PANEL);
  }
}
