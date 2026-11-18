/* git.c - comandos do git do SODOS.
 *   git clone https://github.com/dono/projeto [pasta]
 *   git pull [pasta]       busca o commit novo e atualiza os arquivos
 *   git status [pasta]     o que mudou (staged, nao staged, nao rastreado)
 *   git add -u | -A | arquivo...   prepara o proximo commit (_GIT\INDEX.TXT)
 *   git commit [-a] -m mensagem    grava o commit em _GIT\OBJ
 *   git push               manda os commits locais (precisa de token)
 *   git diff [--cached] [arquivo...]
 *   git log [pasta]        os commits locais e o do clone/pull
 *   git config [--global] chave [valor]
 * O clone e raso (so o ultimo commit). Arquivos com nome que nao cabe em
 * 8.3 ganham um nome curto (LEIAME~1.MD); a lista fica em _GIT\NAMES.TXT.
 *
 * _GIT\STATE.TXT  url, ref, commit (o HEAD local) e remote (o ultimo do servidor)
 * _GIT\NAMES.TXT  arquivos do HEAD: curto TAB longo TAB sha TAB modo
 * _GIT\INDEX.TXT  mesmo formato, o que vai no proximo commit (sem ele = NAMES)
 * _GIT\COMMIT.TXT o commit do clone/pull; _GIT\HEAD.PCK o pack dele (diff)
 * _GIT\OBJ\*.OBJ  objetos criados aqui ("blob 12\0dados", sem zlib)
 * _GIT\CONFIG.TXT e X:\GITCONF.TXT (X = disco do GIT.EXE): chave=valor */
#include "git.h"

#define GITDIR "_GIT"
#define PATHMAX 160

static char err[320];

/* ---------- utilidades ---------- */

static void path_join(char* out, const char* a, const char* b) {
  int n = (int)strlen(a);
  strcpy(out, a);
  if (n && out[n - 1] != '\\') out[n++] = '\\';
  strcpy(out + n, b);
}

static int is_dir(const char* path) {
  struct k_dirent one;
  return k_dir(path, &one, 1) >= 0;
}

static int dir_entries(const char* path) {
  struct k_dirent one;
  return k_dir(path, &one, 1);
}

/* Arquivo inteiro (malloc, com 0 no fim). 0 se nao existe. */
static char* load_file(const char* path, int* len) {
  int n = k_fsize(path);
  char* b;
  if (n < 0) return 0;
  b = (char*)malloc((size_t)n + 1);
  if (!b) return 0;
  n = k_fload(path, b, n + 1);
  if (n < 0) {
    free(b);
    return 0;
  }
  b[n] = 0;
  *len = n;
  return b;
}

static int file_sha(const char* path, unsigned char* sha) {
  int n;
  char* b = load_file(path, &n);
  if (!b) return -1;
  git_hash("blob", (unsigned char*)b, n, sha);
  free(b);
  return 0;
}

static char* xstrdup(const char* s) {
  char* d = (char*)malloc(strlen(s) + 1);
  strcpy(d, s);
  return d;
}

static int up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int lowc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ci_cmp(const char* a, const char* b) {
  for (;; a++, b++) {
    int x = up((unsigned char)*a), y = up((unsigned char)*b);
    if (x != y || !x) return x - y;
  }
}

static int ci_start(const char* s, const char* p) {
  for (; *p; s++, p++)
    if (up((unsigned char)*s) != up((unsigned char)*p)) return 0;
  return 1;
}

static int sha_zero(const unsigned char* s) {
  int i;
  for (i = 0; i < 20; i++)
    if (s[i]) return 0;
  return 1;
}

struct sbuf {
  char* p;
  int len;
  int cap;
};

static void sb_add(struct sbuf* s, const char* t, int n) {
  if (n < 0) n = (int)strlen(t);
  if (s->len + n + 1 > s->cap) {
    int nc = s->cap ? s->cap * 2 : 4096;
    while (nc < s->len + n + 1) nc *= 2;
    s->p = (char*)realloc(s->p, (size_t)nc);
    s->cap = nc;
  }
  memcpy(s->p + s->len, t, (size_t)n);
  s->len += n;
  s->p[s->len] = 0;
}

static void sb_byte(struct sbuf* s, int c) {
  char ch = (char)c;
  sb_add(s, &ch, 1);
}

static void pkt_n(struct sbuf* s, const char* line, int n) {
  char h[8];
  sprintf(h, "%04x", n + 4);
  sb_add(s, h, 4);
  sb_add(s, line, n);
}

static void pkt(struct sbuf* s, const char* line) { pkt_n(s, line, (int)strlen(line)); }

static int pkt_len(const char* p) {
  int i, v = 0;
  for (i = 0; i < 4; i++) {
    int c = (unsigned char)p[i], d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return -1;
    v = v * 16 + d;
  }
  return v;
}

/* ---------- rede: refs e pack ---------- */

struct remote {
  char url[300];
  char ref[160];
  unsigned char head[20];
};

static void url_clean(char* u) {
  int n = (int)strlen(u);
  while (n > 0 && u[n - 1] == '/') u[--n] = 0;
}

/* push = 0: info/refs do upload-pack, acha HEAD e o ramo dele.
 * push = 1: info/refs do receive-pack, r->head = sha de r->ref (zeros se nao existe). */
static int get_refs(struct remote* r, int push) {
  char url[400];
  char* body;
  int len, st, p = 0, have = 0;
  unsigned char main_sha[20];
  int have_main = 0;
  sprintf(url, "%s/info/refs?service=%s", r->url, push ? "git-receive-pack" : "git-upload-pack");
  printf("buscando refs de %s\n", r->url);
  st = http_req(url, 0, 0, 0, &body, &len, err);
  if (st < 0) return -1;
  if (st == 401 || st == 403) {
    if (push) sprintf(err, "HTTP %d: o servidor recusou o login (confira o token: git config token ...)", st);
    else sprintf(err, "repositorio nao encontrado (ou privado): HTTP %d", st);
    free(body);
    return -1;
  }
  if (st == 404) {
    sprintf(err, "repositorio nao encontrado: HTTP 404");
    free(body);
    return -1;
  }
  if (st != 200 || len < 4) {
    sprintf(err, "HTTP %d em info/refs (o servidor fala o protocolo do git?)", st);
    free(body);
    return -1;
  }
  if (push) memset(r->head, 0, 20);
  else r->ref[0] = 0;
  while (p + 4 <= len) {
    int n = pkt_len(body + p);
    char line[512];
    int l;
    if (n < 0) break;
    if (n == 0) {
      p += 4;
      continue;
    }
    if (p + n > len || n < 4) break;
    l = n - 4 < (int)sizeof(line) - 1 ? n - 4 : (int)sizeof(line) - 1;
    memcpy(line, body + p + 4, (size_t)l);
    line[l] = 0;
    p += n;
    if (line[0] == '#') continue;
    if (l > 41 && line[40] == ' ') {
      char* name = line + 41;
      char* caps = name + strlen(name) + 1;
      int nl = (int)strlen(name);
      if (nl && name[nl - 1] == '\n') name[--nl] = 0;
      if (push) {
        if (strcmp(name, r->ref) == 0) hex_sha(line, r->head);
        continue;
      }
      if (strcmp(name, "HEAD") == 0) {
        hex_sha(line, r->head);
        have = 1;
        if (caps < line + l) {
          char* s = strstr(caps, "symref=HEAD:");
          if (s) {
            int k = 0;
            s += 12;
            while (*s && *s != ' ' && *s != '\n' && k < (int)sizeof(r->ref) - 1) r->ref[k++] = *s++;
            r->ref[k] = 0;
          }
        }
      } else if (!have_main && (strcmp(name, "refs/heads/main") == 0 || strcmp(name, "refs/heads/master") == 0)) {
        hex_sha(line, main_sha);
        have_main = 1;
        if (!r->ref[0]) strcpy(r->ref, name);
      }
    }
  }
  free(body);
  if (push) return 0;
  if (!have && have_main) {
    memcpy(r->head, main_sha, 20);
    have = 1;
  }
  if (!have) {
    sprintf(err, "o repositorio esta vazio (sem HEAD)");
    return -1;
  }
  if (!r->ref[0]) strcpy(r->ref, "HEAD");
  return 0;
}

/* Pede so o commit r->head (deepen 1). *buf e a resposta; *pack aponta dentro. */
static int get_pack(struct remote* r, char** buf, unsigned char** pack, int* plen) {
  struct sbuf req;
  char line[200], hex[41], url[400];
  int len, st, p = 0;
  memset(&req, 0, sizeof(req));
  sha_hex(r->head, hex);
  sprintf(line, "want %s ofs-delta shallow no-progress agent=git/sodos\n", hex);
  pkt(&req, line);
  pkt(&req, "deepen 1\n");
  sb_add(&req, "0000", 4);
  pkt(&req, "done\n");
  sprintf(url, "%s/git-upload-pack", r->url);
  printf("baixando o pack\n");
  st = http_req(url, "application/x-git-upload-pack-request", req.p, req.len, buf, &len, err);
  free(req.p);
  if (st < 0) return -1;
  if (st != 200) {
    sprintf(err, "HTTP %d em git-upload-pack", st);
    free(*buf);
    return -1;
  }
  /* shallow ..., 0000, NAK, depois o PACK cru */
  while (p + 4 <= len) {
    int n;
    if (memcmp(*buf + p, "PACK", 4) == 0) {
      *pack = (unsigned char*)*buf + p;
      *plen = len - p;
      return 0;
    }
    n = pkt_len(*buf + p);
    if (n < 0) break;
    if (n == 0) {
      p += 4;
      continue;
    }
    if (n >= 7 && memcmp(*buf + p + 4, "ERR", 3) == 0) {
      int l = n - 4 < 200 ? n - 4 : 200;
      memcpy(err, *buf + p + 4, (size_t)l);
      err[l] = 0;
      free(*buf);
      return -1;
    }
    p += n;
  }
  sprintf(err, "a resposta nao trouxe o pack");
  free(*buf);
  return -1;
}

/* ---------- nomes 8.3 ---------- */

static int ok83(int c) {
  if (c >= 'A' && c <= 'Z') return 1;
  if (c >= '0' && c <= '9') return 1;
  return strchr("!#$%&'()-@^_`{}~", c) != 0 && c != 0;
}

/* 1 se o nome ja e 8.3 (sem contar maiusculas). */
static int is83(const char* s) {
  int b = 0, e = -1;
  if (s[0] == '.') return 0;
  for (; *s; s++) {
    int c = up((unsigned char)*s);
    if (c == '.') {
      if (e >= 0) return 0;
      e = 0;
      continue;
    }
    if (!ok83(c)) return 0;
    if (e >= 0) {
      if (++e > 3) return 0;
    } else if (++b > 8) {
      return 0;
    }
  }
  return b > 0 && e != 0;
}

static int used_has(char (*used)[13], int n, const char* s) {
  int i;
  for (i = 0; i < n; i++)
    if (strcmp(used[i], s) == 0) return 1;
  return 0;
}

/* Nome curto unico na pasta: README.MD fica; README.pt-BR.md vira README~1.MD. */
static void make83(const char* name, char (*used)[13], int nused, char* out) {
  char base[16], ext[4];
  const char* dot = 0;
  const char* p;
  int nb = 0, ne = 0, k;
  if (is83(name)) {
    for (k = 0; name[k]; k++) out[k] = (char)up((unsigned char)name[k]);
    out[k] = 0;
    if (!used_has(used, nused, out)) return;
  }
  for (p = name + 1; *p; p++)
    if (*p == '.') dot = p;
  for (p = name; *p && p != dot && nb < 8; p++) {
    int c = up((unsigned char)*p);
    if (c == '.' || c == ' ') continue;
    base[nb++] = (char)(ok83(c) ? c : '_');
  }
  if (!nb) base[nb++] = '_';
  base[nb] = 0;
  if (dot)
    for (p = dot + 1; *p && ne < 3; p++) {
      int c = up((unsigned char)*p);
      if (c == '.' || c == ' ') continue;
      ext[ne++] = (char)(ok83(c) ? c : '_');
    }
  ext[ne] = 0;
  for (k = 1; k < 100000; k++) {
    char num[8];
    int nl = sprintf(num, "~%d", k), keep = 8 - nl;
    if (keep > nb) keep = nb;
    memcpy(out, base, (size_t)keep);
    strcpy(out + keep, num);
    if (ne) {
      strcat(out, ".");
      strcat(out, ext);
    }
    if (!used_has(used, nused, out)) return;
  }
}

/* ---------- listas de arquivos (NAMES.TXT, INDEX.TXT) ----------
 * linha: curto TAB longo TAB sha [TAB modo]. Curto "*" = nao esta no disco
 * (caminho longo demais ou submodulo), mas faz parte da arvore. */

struct nent {
  char* dos;
  char* lng;
  unsigned char sha[20];
  char mode[8];
  int hasmode;
  int seen;
  int gone;
};

struct list {
  struct nent* e;
  int n;
  int cap;
  char* text;
};

static int on_disk(const struct nent* e) { return !e->gone && e->dos[0] != '*'; }

static int nent_cmp(const void* a, const void* b) {
  return strcmp(((const struct nent*)a)->lng, ((const struct nent*)b)->lng);
}

static void list_sort(struct list* l) {
  if (l->n > 1) qsort(l->e, (size_t)l->n, sizeof(struct nent), nent_cmp);
}

static struct nent* list_push(struct list* l) {
  struct nent* e;
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 256;
    l->e = (struct nent*)realloc(l->e, sizeof(struct nent) * (size_t)l->cap);
  }
  e = &l->e[l->n++];
  memset(e, 0, sizeof(*e));
  strcpy(e->mode, "100644");
  return e;
}

/* le _GIT\<file>; -1 se nao existe */
static int list_load(const char* root, const char* file, struct list* l) {
  char path[PATHMAX];
  int len;
  char* p;
  memset(l, 0, sizeof(*l));
  sprintf(path, "%s\\%s\\%s", root, GITDIR, file);
  l->text = load_file(path, &len);
  if (!l->text) return -1;
  p = l->text;
  while (*p) {
    char* f[4];
    int nf = 1;
    char* nl = strchr(p, '\n');
    struct nent* e;
    if (!nl) break;
    *nl = 0;
    if (nl > p && nl[-1] == '\r') nl[-1] = 0;
    f[0] = p;
    p = nl + 1;
    while (nf < 4 && (f[nf] = strchr(f[nf - 1], '\t')) != 0) {
      *f[nf]++ = 0;
      nf++;
    }
    if (nf < 3 || strlen(f[2]) < 40) continue;
    e = list_push(l);
    e->dos = f[0];
    e->lng = f[1];
    if (hex_sha(f[2], e->sha) != 0) {
      l->n--;
      continue;
    }
    if (nf > 3 && f[3][0]) {
      strncpy(e->mode, f[3], sizeof(e->mode) - 1);
      e->hasmode = 1;
    }
  }
  list_sort(l);
  return l->n;
}

static int list_save(const char* root, const char* file, struct list* l) {
  struct sbuf s;
  char path[PATHMAX], hex[41];
  int i, r;
  memset(&s, 0, sizeof(s));
  list_sort(l);
  for (i = 0; i < l->n; i++) {
    struct nent* e = &l->e[i];
    if (e->gone) continue;
    sha_hex(e->sha, hex);
    sb_add(&s, e->dos, -1);
    sb_add(&s, "\t", 1);
    sb_add(&s, e->lng, -1);
    sb_add(&s, "\t", 1);
    sb_add(&s, hex, 40);
    /* sem modo conhecido (NAMES.TXT antigo) fica sem: o commit pega o do HEAD */
    if (e->hasmode) {
      sb_add(&s, "\t", 1);
      sb_add(&s, e->mode, -1);
    }
    sb_add(&s, "\n", 1);
  }
  sprintf(path, "%s\\%s\\%s", root, GITDIR, file);
  r = k_fsave(path, s.p ? s.p : "", s.len);
  free(s.p);
  return r;
}

/* procura pelo nome longo (lista ordenada); ignora as apagadas */
static struct nent* list_find(struct list* l, const char* lng) {
  int lo = 0, hi = l->n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2, c = strcmp(l->e[m].lng, lng);
    if (!c) return l->e[m].gone ? 0 : &l->e[m];
    if (c < 0) lo = m + 1;
    else hi = m - 1;
  }
  return 0;
}

static struct nent* list_find_dos(struct list* l, const char* dos) {
  int i;
  for (i = 0; i < l->n; i++)
    if (!l->e[i].gone && ci_cmp(l->e[i].dos, dos) == 0) return &l->e[i];
  return 0;
}

/* INDEX.TXT, ou NAMES.TXT se nao tem nada staged. 1 = veio do INDEX */
static int index_load(const char* root, struct list* l) {
  if (list_load(root, "INDEX.TXT", l) >= 0) return 1;
  list_load(root, "NAMES.TXT", l);
  return 0;
}

static struct list olds;   /* NAMES.TXT antes do pull */

/* ---------- checkout ---------- */

struct co {
  const char* root;      /* "D:\\SODOS" */
  struct sbuf names;
  int keep;              /* so refaz a lista: nao grava nada que ja existia */
  int files;
  int written;
  int skipped;
  int kept;
};

static int depth_of(const char* path) {
  int n = 0;
  for (; *path; path++)
    if (*path == '\\') n++;
  return n;
}

static void names_line(struct co* c, const char* dos, const char* lng, const unsigned char* sha, const char* mode) {
  char hex[41];
  sha_hex(sha, hex);
  sb_add(&c->names, dos, -1);
  sb_add(&c->names, "\t", 1);
  sb_add(&c->names, lng, -1);
  sb_add(&c->names, "\t", 1);
  sb_add(&c->names, hex, 40);
  sb_add(&c->names, "\t", 1);
  sb_add(&c->names, mode, -1);
  sb_add(&c->names, "\n", 1);
}

static void put_file(struct co* c, const char* dosrel, const char* lng, const unsigned char* sha, const char* mode) {
  char full[PATHMAX + 40], hex[41];
  struct nent* o = olds.n ? list_find(&olds, lng) : 0;
  unsigned char* data;
  int type, size;
  c->files++;
  sha_hex(sha, hex);
  path_join(full, c->root, dosrel);
  /* o kernel aceita ate ~60 caracteres depois de "D:" e 8 pastas */
  if ((int)strlen(full) - 2 > 60 || depth_of(full) > 8) {
    if (c->skipped < 8) printf("  caminho longo demais, pulado: %s\n", lng);
    c->skipped++;
    names_line(c, "*", lng, sha, mode);
    return;
  }
  names_line(c, dosrel, lng, sha, mode);
  if (o) {
    unsigned char now[20];
    o->seen = 1;
    if (memcmp(o->sha, sha, 20) == 0 && strcmp(o->dos, dosrel) == 0 && (c->keep || k_fsize(full) >= 0)) return;
    if (file_sha(full, now) == 0 && memcmp(now, o->sha, 20) != 0 && memcmp(now, sha, 20) != 0) {
      printf("  mudado aqui, mantido: %s\n", lng);
      c->kept++;
      return;
    }
  }
  data = pack_get(sha, &type, &size);
  if (!data || type != OBJ_BLOB) {
    printf("  objeto %s nao veio no pack: %s\n", hex, lng);
    c->skipped++;
    return;
  }
  if (k_fsave(full, (const char*)data, size) != 0) {
    printf("  nao gravou %s (%s)\n", full, lng);
    c->skipped++;
    return;
  }
  c->written++;
  if ((c->written & 15) == 0) printf("\r  %d arquivos", c->written);
}

static int checkout_tree(struct co* c, const unsigned char* tsha, const char* dosrel, const char* lngrel) {
  unsigned char* t;
  unsigned char* tree;
  int type, size, p, n = 0, nused = 0;
  char (*used)[13];
  t = pack_get(tsha, &type, &size);
  if (!t || type != OBJ_TREE) {
    sprintf(err, "arvore que nao veio no pack");
    return -1;
  }
  tree = (unsigned char*)malloc((size_t)size + 1);
  if (!tree) {
    sprintf(err, "sem memoria");
    return -1;
  }
  memcpy(tree, t, (size_t)size);
  for (p = 0; p < size; p++)
    if (tree[p] == 0) n++;
  used = (char (*)[13])malloc(sizeof(*used) * (size_t)(n + 1));
  p = 0;
  while (p < size) {
    char mode[8], name[256], short83[13], dos[PATHMAX], lng[PATHMAX + 256];
    const unsigned char* sha;
    int k = 0;
    while (p < size && tree[p] != ' ' && k < 7) mode[k++] = (char)tree[p++];
    mode[k] = 0;
    p++;
    k = 0;
    while (p < size && tree[p] && k < 255) name[k++] = (char)tree[p++];
    name[k] = 0;
    p++;
    sha = tree + p;
    p += 20;
    if (lngrel[0]) sprintf(lng, "%s/%s", lngrel, name);
    else strcpy(lng, name);
    if (strcmp(mode, "160000") == 0) {
      printf("  submodulo ignorado: %s\n", lng);
      names_line(c, "*", lng, sha, mode);
      continue;
    }
    make83(name, used, nused, short83);
    strcpy(used[nused++], short83);
    if (dosrel[0]) sprintf(dos, "%s\\%s", dosrel, short83);
    else strcpy(dos, short83);
    if (strcmp(mode, "40000") == 0) {
      if (checkout_tree(c, sha, dos, lng) != 0) {
        free(tree);
        free(used);
        return -1;
      }
    } else {
      put_file(c, dos, lng, sha, mode);
    }
  }
  free(tree);
  free(used);
  return 0;
}

/* ---------- estado ---------- */

/* commit = HEAD local; base = o ultimo commit conhecido do servidor */
static int state_read(const char* root, struct remote* r, unsigned char* commit, unsigned char* base) {
  char path[PATHMAX];
  int len, have_base = 0;
  char* t;
  char* p;
  sprintf(path, "%s\\%s\\STATE.TXT", root, GITDIR);
  memset(r, 0, sizeof(*r));
  memset(commit, 0, 20);
  t = load_file(path, &len);
  if (!t) return -1;
  for (p = t; *p;) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (nl && nl > p && nl[-1] == '\r') nl[-1] = 0;
    if (strncmp(p, "url=", 4) == 0) strncpy(r->url, p + 4, sizeof(r->url) - 1);
    else if (strncmp(p, "ref=", 4) == 0) strncpy(r->ref, p + 4, sizeof(r->ref) - 1);
    else if (strncmp(p, "commit=", 7) == 0) hex_sha(p + 7, commit);
    else if (strncmp(p, "remote=", 7) == 0 && base) have_base = hex_sha(p + 7, base) == 0;
    if (!nl) break;
    p = nl + 1;
  }
  free(t);
  if (base && !have_base) memcpy(base, commit, 20);
  return r->url[0] ? 0 : -1;
}

static int state_write(const char* root, const struct remote* r, const unsigned char* commit,
                       const unsigned char* base) {
  char path[PATHMAX], text[700], hex[41], bhex[41];
  sha_hex(commit, hex);
  sha_hex(base, bhex);
  sprintf(text, "url=%s\nref=%s\ncommit=%s\nremote=%s\n", r->url, r->ref, hex, bhex);
  sprintf(path, "%s\\%s\\STATE.TXT", root, GITDIR);
  return k_fsave(path, text, (int)strlen(text));
}

/* Acha a pasta do repositorio: arg, ou a atual e as de cima. */
static int find_root(const char* arg, char* root) {
  char path[PATHMAX];
  if (arg) {
    if (arg[0] && arg[1] == ':') strcpy(root, arg);
    else {
      k_pwd(path, sizeof(path));
      path_join(root, path, arg);
    }
  } else {
    k_pwd(root, PATHMAX);
  }
  for (;;) {
    int n = (int)strlen(root);
    while (n > 3 && root[n - 1] == '\\') root[--n] = 0;
    sprintf(path, "%s%s%s\\STATE.TXT", root, n > 3 ? "\\" : "", GITDIR);
    if (k_fsize(path) >= 0) {
      if (n == 3) root[2] = 0;   /* "D:" para juntar com "\\_GIT" */
      return 0;
    }
    if (arg || n <= 3) break;
    while (n > 3 && root[n - 1] != '\\') n--;
    root[n > 3 ? n - 1 : 3] = 0;
  }
  sprintf(err, "nao e uma pasta de git (sem %s\\STATE.TXT)", GITDIR);
  return -1;
}

static void git_file(const char* root, const char* name, char* out) {
  sprintf(out, "%s\\%s\\%s", root, GITDIR, name);
}

/* ---------- objetos locais (_GIT\OBJ) ---------- */

static const char* type_str(int t) {
  if (t == OBJ_COMMIT) return "commit";
  if (t == OBJ_TREE) return "tree";
  return "blob";
}

static void obj_path(const char* root, const unsigned char* sha, char* out) {
  char hex[41];
  sha_hex(sha, hex);
  sprintf(out, "%s\\%s\\OBJ\\%.8s.OBJ", root, GITDIR, hex);
}

static int obj_exists(const char* root, const unsigned char* sha) {
  char path[PATHMAX];
  obj_path(root, sha, path);
  return k_fsize(path) >= 0;
}

/* Conteudo (malloc) do objeto sha guardado aqui, ou 0. */
static unsigned char* obj_read(const char* root, const unsigned char* sha, int* type, int* len) {
  char path[PATHMAX];
  unsigned char now[20];
  sha1_ctx c;
  char* b;
  char* z;
  int n;
  unsigned char* out;
  obj_path(root, sha, path);
  b = load_file(path, &n);
  if (!b) return 0;
  sha1_init(&c);
  sha1_update(&c, b, n);
  sha1_final(&c, now);
  z = (char*)memchr(b, 0, (size_t)n);
  if (memcmp(now, sha, 20) != 0 || !z) {
    free(b);
    return 0;
  }
  if (strncmp(b, "commit ", 7) == 0) *type = OBJ_COMMIT;
  else if (strncmp(b, "tree ", 5) == 0) *type = OBJ_TREE;
  else *type = OBJ_BLOB;
  *len = n - (int)(z + 1 - b);
  out = (unsigned char*)malloc((size_t)*len + 1);
  memcpy(out, z + 1, (size_t)*len);
  out[*len] = 0;
  free(b);
  return out;
}

static int obj_write(const char* root, int type, const void* data, int len, unsigned char* sha) {
  char path[PATHMAX], hdr[32];
  char* b;
  int hn, r, t2, l2;
  unsigned char* old;
  git_hash(type_str(type), (const unsigned char*)data, len, sha);
  obj_path(root, sha, path);
  if (k_fsize(path) >= 0) {
    old = obj_read(root, sha, &t2, &l2);
    if (old) {
      free(old);
      return 0;
    }
  }
  sprintf(hdr, "%s\\%s\\OBJ", root, GITDIR);
  k_mkdir(hdr);
  hn = sprintf(hdr, "%s %d", type_str(type), len) + 1;
  b = (char*)malloc((size_t)(hn + len) + 1);
  if (!b) {
    sprintf(err, "sem memoria");
    return -1;
  }
  memcpy(b, hdr, (size_t)hn);
  memcpy(b + hn, data, (size_t)len);
  r = k_fsave(path, b, hn + len);
  free(b);
  if (r != 0) {
    sprintf(err, "nao gravou %s", path);
    return -1;
  }
  return 0;
}

/* pack do clone/pull, carregado so quando precisa (diff) */
static char* headpack;
static int headpack_state;

static unsigned char* headpack_get(const char* root, const unsigned char* sha, int* type, int* len) {
  unsigned char* d;
  unsigned char* out;
  if (headpack_state == 0) {
    char path[PATHMAX];
    int n;
    headpack_state = -1;
    git_file(root, "HEAD.PCK", path);
    headpack = load_file(path, &n);
    if (headpack) {
      printf("lendo %s\n", path);
      if (pack_load((unsigned char*)headpack, n, err) >= 0) headpack_state = 1;
      else printf("aviso: %s\n", err);
    }
  }
  if (headpack_state != 1) return 0;
  d = pack_get(sha, type, len);
  if (!d) return 0;
  out = (unsigned char*)malloc((size_t)*len + 1);
  memcpy(out, d, (size_t)*len);
  out[*len] = 0;
  return out;
}

/* conteudo de um blob: _GIT\OBJ, ou o pack do clone/pull */
static unsigned char* blob_get(const char* root, const unsigned char* sha, int* len) {
  int type;
  unsigned char* b = obj_read(root, sha, &type, len);
  if (b) return b;
  return headpack_get(root, sha, &type, len);
}

/* Commit sha: de _GIT\OBJ ou o COMMIT.TXT (o do clone/pull). malloc, ou 0. */
static char* commit_get(const char* root, const unsigned char* sha, int* len) {
  char path[PATHMAX];
  unsigned char now[20];
  int type;
  char* t = (char*)obj_read(root, sha, &type, len);
  if (t) {
    if (type == OBJ_COMMIT) return t;
    free(t);
    return 0;
  }
  git_file(root, "COMMIT.TXT", path);
  t = load_file(path, len);
  if (!t) return 0;
  git_hash("commit", (unsigned char*)t, *len, now);
  if (memcmp(now, sha, 20) == 0) return t;
  free(t);
  return 0;
}

/* "tree X" e "parent Y" de um commit */
static int commit_parse(const char* t, unsigned char* tree, unsigned char* parent) {
  const char* p = t;
  int have_tree = 0;
  if (parent) memset(parent, 0, 20);
  while (*p && *p != '\n') {
    if (strncmp(p, "tree ", 5) == 0) have_tree = hex_sha(p + 5, tree) == 0;
    else if (parent && strncmp(p, "parent ", 7) == 0) hex_sha(p + 7, parent);
    p = strchr(p, '\n');
    if (!p) break;
    p++;
  }
  return have_tree ? 0 : -1;
}

/* ---------- arvores a partir de uma lista ---------- */

struct tctx {
  const char* root;
  int write;                 /* grava as arvores que nao estao em known */
  unsigned char (*known)[20];
  int nknown;
  int capknown;
  int written;
};

static int known_has(struct tctx* t, const unsigned char* sha) {
  int i;
  for (i = 0; i < t->nknown; i++)
    if (memcmp(t->known[i], sha, 20) == 0) return 1;
  return 0;
}

static void known_add(struct tctx* t, const unsigned char* sha) {
  if (t->nknown == t->capknown) {
    t->capknown = t->capknown ? t->capknown * 2 : 64;
    t->known = (unsigned char (*)[20])realloc(t->known, 20 * (size_t)t->capknown);
  }
  memcpy(t->known[t->nknown++], sha, 20);
}

/* v[lo..hi) tem o prefixo de pl letras ("src/"). A ordem por strcmp do
 * caminho inteiro e a mesma ordem das arvores do git. */
static int build_tree(struct nent** v, int lo, int hi, int pl, struct tctx* t, unsigned char* out) {
  struct sbuf s;
  int i = lo;
  memset(&s, 0, sizeof(s));
  sb_add(&s, "", 0);
  while (i < hi) {
    const char* name = v[i]->lng + pl;
    const char* sl = strchr(name, '/');
    if (sl) {
      int dl = (int)(sl - name), j = i;
      unsigned char sub[20];
      while (j < hi && strncmp(v[j]->lng + pl, name, (size_t)dl) == 0 && v[j]->lng[pl + dl] == '/') j++;
      if (build_tree(v, i, j, pl + dl + 1, t, sub) != 0) {
        free(s.p);
        return -1;
      }
      sb_add(&s, "40000 ", 6);
      sb_add(&s, name, dl);
      sb_byte(&s, 0);
      sb_add(&s, (const char*)sub, 20);
      i = j;
    } else {
      sb_add(&s, v[i]->mode, -1);
      sb_byte(&s, ' ');
      sb_add(&s, name, -1);
      sb_byte(&s, 0);
      sb_add(&s, (const char*)v[i]->sha, 20);
      i++;
    }
  }
  git_hash("tree", (unsigned char*)s.p, s.len, out);
  if (t->write) {
    if (!known_has(t, out)) {
      unsigned char chk[20];
      if (obj_write(t->root, OBJ_TREE, s.p, s.len, chk) != 0) {
        free(s.p);
        return -1;
      }
      t->written++;
      known_add(t, out);
    }
  } else {
    known_add(t, out);
  }
  free(s.p);
  return 0;
}

static int list_tree(struct list* l, struct tctx* t, unsigned char* out) {
  struct nent** v;
  int i, n = 0, r;
  list_sort(l);
  v = (struct nent**)malloc(sizeof(*v) * (size_t)(l->n + 1));
  for (i = 0; i < l->n; i++)
    if (!l->e[i].gone) v[n++] = &l->e[i];
  r = build_tree(v, 0, n, 0, t, out);
  free(v);
  return r;
}

/* ---------- configuracao ---------- */

static void cfg_path(const char* root, int global, char* out) {
  if (global || !root) {
    int d = k_exe_drive();
    sprintf(out, "%c:\\GITCONF.TXT", d ? d : 'D');
  } else {
    git_file(root, "CONFIG.TXT", out);
  }
}

static int cfg_read(const char* path, const char* key, char* out, int max) {
  int len, kl = (int)strlen(key), found = 0;
  char* t = load_file(path, &len);
  char* p;
  if (!t) return 0;
  for (p = t; *p;) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (nl && nl > p && nl[-1] == '\r') nl[-1] = 0;
    if (strncmp(p, key, (size_t)kl) == 0 && p[kl] == '=') {
      strncpy(out, p + kl + 1, (size_t)max - 1);
      out[max - 1] = 0;
      found = 1;
    }
    if (!nl) break;
    p = nl + 1;
  }
  free(t);
  return found;
}

/* repositorio, depois o global; o token tambem vem de SET GIT_TOKEN=... */
static int cfg_get(const char* root, const char* key, char* out, int max) {
  char path[PATHMAX];
  out[0] = 0;
  if (strcmp(key, "token") == 0 && k_getenv("GIT_TOKEN", out, max) > 0) return 1;
  if (root) {
    cfg_path(root, 0, path);
    if (cfg_read(path, key, out, max)) return 1;
  }
  cfg_path(root, 1, path);
  return cfg_read(path, key, out, max);
}

static int cfg_set(const char* path, const char* key, const char* val) {
  struct sbuf s;
  int len, kl = (int)strlen(key), r, done = 0;
  char* t = load_file(path, &len);
  char* p;
  memset(&s, 0, sizeof(s));
  sb_add(&s, "", 0);
  for (p = t; p && *p;) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (nl && nl > p && nl[-1] == '\r') nl[-1] = 0;
    if (strncmp(p, key, (size_t)kl) == 0 && p[kl] == '=') {
      if (val[0] && !done) {
        sb_add(&s, key, -1);
        sb_add(&s, "=", 1);
        sb_add(&s, val, -1);
        sb_add(&s, "\n", 1);
      }
      done = 1;
    } else if (*p) {
      sb_add(&s, p, -1);
      sb_add(&s, "\n", 1);
    }
    if (!nl) break;
    p = nl + 1;
  }
  if (!done && val[0]) {
    sb_add(&s, key, -1);
    sb_add(&s, "=", 1);
    sb_add(&s, val, -1);
    sb_add(&s, "\n", 1);
  }
  r = k_fsave(path, s.p, s.len);
  free(s.p);
  free(t);
  return r;
}

/* ---------- comandos ---------- */

/* Busca o commit de r e grava os arquivos em root.
 * keep = 1: so refaz NAMES.TXT/HEAD.PCK do commit atual (nao mexe no INDEX nem em OBJ). */
static int fetch_checkout(const char* root, struct remote* r, int keep) {
  char* resp;
  unsigned char* pack;
  int plen, type, size, i, removed = 0;
  unsigned char* cm;
  unsigned char tsha[20];
  char path[PATHMAX], hex[41];
  struct co c;
  unsigned t0 = k_ms();
  if (get_pack(r, &resp, &pack, &plen) != 0) return -1;
  printf("  pack de %d KB\n", plen / 1024);
  if (pack_load(pack, plen, err) < 0) {
    free(resp);
    return -1;
  }
  cm = pack_get(r->head, &type, &size);
  if (!cm || type != OBJ_COMMIT || size < 46 || memcmp(cm, "tree ", 5) != 0 || hex_sha((char*)cm + 5, tsha) != 0) {
    sprintf(err, "o commit nao veio no pack");
    free(resp);
    return -1;
  }
  git_file(root, "COMMIT.TXT", path);
  k_fsave(path, (char*)cm, size);
  memset(&c, 0, sizeof(c));
  c.root = root;
  c.keep = keep;
  printf(keep ? "refazendo a lista de arquivos\n" : "gravando arquivos\n");
  if (checkout_tree(&c, tsha, "", "") != 0) {
    free(resp);
    pack_free();
    return -1;
  }
  pack_free();
  /* o pack fica guardado para o diff */
  git_file(root, "HEAD.PCK", path);
  if (k_fsave(path, (const char*)pack, plen) != 0) printf("aviso: nao gravou %s\n", path);
  free(resp);
  /* o que saiu do repositorio sai da pasta, se nao foi mudado aqui */
  for (i = 0; i < olds.n && !keep; i++) {
    unsigned char now[20];
    char full[PATHMAX + 40];
    if (olds.e[i].seen || olds.e[i].dos[0] == '*') continue;
    path_join(full, root, olds.e[i].dos);
    if (file_sha(full, now) != 0) continue;
    if (memcmp(now, olds.e[i].sha, 20) != 0) {
      printf("  saiu do repositorio mas foi mudado aqui, mantido: %s\n", olds.e[i].lng);
      continue;
    }
    if (k_fdelete(full) == 0) removed++;
  }
  git_file(root, "NAMES.TXT", path);
  if (k_fsave(path, c.names.p ? c.names.p : "", c.names.len) != 0) printf("aviso: nao gravou %s\n", path);
  free(c.names.p);
  if (!keep) {
    git_file(root, "INDEX.TXT", path);
    if (k_fsize(path) >= 0) {
      printf("  o que estava staged (git add) foi descartado; rode git add de novo\n");
      k_fdelete(path);
    }
    sprintf(path, "%s\\%s\\OBJ\\*.OBJ", root, GITDIR);
    k_fdelete(path);
  }
  state_write(root, r, r->head, r->head);
  if (keep) return 0;
  sha_hex(r->head, hex);
  printf("\r%d arquivos (%d gravados", c.files, c.written);
  if (removed) printf(", %d apagados", removed);
  if (c.kept) printf(", %d mantidos", c.kept);
  if (c.skipped) printf(", %d pulados", c.skipped);
  printf(") em %u s\ncommit %.12s (%s)\n", (k_ms() - t0) / 1000, hex, r->ref);
  return 0;
}

static int cmd_clone(int argc, char** argv) {
  struct remote r;
  char root[PATHMAX], name[64], short83[13], cwd[PATHMAX];
  const char* p;
  const char* last;
  int n;
  char none[1][13];
  if (argc < 3) {
    printf("uso: git clone https://servidor/dono/projeto [pasta]\n");
    return 1;
  }
  memset(&r, 0, sizeof(r));
  strncpy(r.url, argv[2], sizeof(r.url) - 1);
  url_clean(r.url);
  if (argc > 3) {
    strncpy(name, argv[3], sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    if (!strchr(name, ':') && !strchr(name, '\\')) {
      make83(name, none, 0, short83);
      strcpy(name, short83);
    }
  } else {
    last = r.url;
    for (p = r.url; *p; p++)
      if (*p == '/') last = p + 1;
    strncpy(name, last, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    n = (int)strlen(name);
    if (n > 4 && strcmp(name + n - 4, ".git") == 0) name[n - 4] = 0;
    make83(name, none, 0, short83);
    strcpy(name, short83);
  }
  if (name[0] && name[1] == ':') strcpy(root, name);
  else {
    k_pwd(cwd, sizeof(cwd));
    path_join(root, cwd, name);
  }
  if (dir_entries(root) > 0) {
    printf("%s ja existe e nao esta vazia\n", root);
    return 1;
  }
  if (!is_dir(root) && k_mkdir(root) != 0) {
    printf("nao criei a pasta %s\n", root);
    return 1;
  }
  if (!k_net_ready()) {
    printf("rede nao pronta\n");
    return 1;
  }
  if (get_refs(&r, 0) != 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  printf("clonando %s em %s\n", r.ref, root);
  memset(&olds, 0, sizeof(olds));
  if (fetch_checkout(root, &r, 0) != 0) {
    printf("\nerro: %s\n", err);
    return 1;
  }
  return 0;
}

static int cmd_pull(int argc, char** argv) {
  struct remote r;
  char root[PATHMAX], hex[41];
  unsigned char cur[20], base[20];
  if (find_root(argc > 2 ? argv[2] : 0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  if (state_read(root, &r, cur, base) != 0) {
    printf("%s\\%s\\STATE.TXT invalido\n", root, GITDIR);
    return 1;
  }
  if (!k_net_ready()) {
    printf("rede nao pronta\n");
    return 1;
  }
  if (get_refs(&r, 0) != 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  if (memcmp(cur, r.head, 20) == 0) {
    sha_hex(cur, hex);
    printf("Ja esta atualizado (%.12s).\n", hex);
    return 0;
  }
  if (memcmp(cur, base, 20) != 0) {
    printf("Ha commits locais que nao foram para o servidor e o servidor mudou.\n");
    printf("Este git nao faz merge: guarde os arquivos mudados, apague a pasta e\n");
    printf("faca o clone de novo, ou mande os commits antes (git push).\n");
    return 1;
  }
  list_load(root, "NAMES.TXT", &olds);
  if (fetch_checkout(root, &r, 0) != 0) {
    printf("\nerro: %s\n", err);
    return 1;
  }
  return 0;
}

/* NAMES.TXT antigo (sem modos) nao refaz a arvore do HEAD: baixa o commit de
 * novo so para refazer a lista. Os arquivos da pasta nao mudam. */
static int reindex(const char* root, struct remote* r, const unsigned char* cur) {
  printf("NAMES.TXT sem os modos dos arquivos: baixando o commit para refazer a lista\n");
  if (!k_net_ready()) {
    sprintf(err, "rede nao pronta");
    return -1;
  }
  memcpy(r->head, cur, 20);
  list_load(root, "NAMES.TXT", &olds);
  return fetch_checkout(root, r, 1);
}

/* ---------- caminhos dos argumentos ---------- */

/* arquivo do argumento (relativo a pasta atual) -> relativo a root, com \ */
static int rel_path(const char* root, const char* arg, char* rel) {
  char cwd[PATHMAX], full[PATHMAX * 2];
  int rl = (int)strlen(root);
  if (arg[0] && arg[1] == ':') strcpy(full, arg);
  else {
    k_pwd(cwd, sizeof(cwd));
    path_join(full, cwd, arg);
  }
  {
    /* tira "." e ".." */
    char* parts[32];
    int np = 0, i;
    char tmp[PATHMAX * 2];
    char* p;
    strcpy(tmp, full + 2);
    for (p = strtok(tmp, "\\/"); p; p = strtok(0, "\\/")) {
      if (strcmp(p, ".") == 0) continue;
      if (strcmp(p, "..") == 0) {
        if (np) np--;
        continue;
      }
      if (np < 32) parts[np++] = p;
    }
    full[2] = 0;
    for (i = 0; i < np; i++) {
      strcat(full, "\\");
      strcat(full, parts[i]);
    }
    if (!np) strcat(full, "\\");
  }
  if (!ci_start(full, root) || (full[rl] != '\\' && full[rl] != 0)) {
    sprintf(err, "%s esta fora do repositorio %s", arg, root);
    return -1;
  }
  strcpy(rel, full[rl] ? full + rl + 1 : "");
  return 0;
}

/* Nome longo de um arquivo novo: as pastas que ja existem no repositorio
 * ganham o nome longo delas; o resto fica como foi digitado (lower = minusculas). */
static void long_for(struct list* idx, const char* rel, int lower, char* out) {
  char comp[16][PATHMAX];
  int nc = 0, d, i, k;
  const char* p = rel;
  out[0] = 0;
  while (*p && nc < 16) {
    k = 0;
    while (*p && *p != '\\' && k < PATHMAX - 1) comp[nc][k++] = *p++;
    comp[nc][k] = 0;
    nc++;
    if (*p == '\\') p++;
  }
  for (d = nc - 1; d > 0; d--) {
    char pre[PATHMAX * 2];
    pre[0] = 0;
    for (i = 0; i < d; i++) {
      strcat(pre, comp[i]);
      strcat(pre, "\\");
    }
    for (i = 0; i < idx->n; i++) {
      struct nent* e = &idx->e[i];
      if (e->dos[0] != '*' && ci_start(e->dos, pre)) {
        const char* q = e->lng;
        int c = 0;
        while (*q && c < d) {
          if (*q == '/') c++;
          if (c < d) {
            int L = (int)strlen(out);
            out[L] = *q;
            out[L + 1] = 0;
          }
          q++;
        }
        break;
      }
    }
    if (i < idx->n) break;
  }
  for (i = d > 0 ? d : 0; i < nc; i++) {
    char* q;
    if (out[0]) strcat(out, "/");
    q = out + strlen(out);
    strcat(out, comp[i]);
    /* NOVO.TXT (como o FAT guarda) vira novo.txt; Novo.txt fica como foi digitado */
    if (!lower) {
      const char* c;
      lower = 1;
      for (c = comp[i]; *c; c++)
        if (*c >= 'a' && *c <= 'z') lower = 0;
      if (lower)
        for (; *q; q++) *q = (char)lowc((unsigned char)*q);
      lower = 0;
    } else {
      for (; *q; q++) *q = (char)lowc((unsigned char)*q);
    }
  }
}

/* ---------- add ---------- */

struct addst {
  const char* root;
  struct list* idx;
  struct nent** bydos;    /* rastreados, por nome curto (para a busca do -A) */
  int nbydos;
  int changed;
  int added;
  int removed;
  int quiet;
};

static int dos_cmp(const void* a, const void* b) {
  return ci_cmp((*(struct nent* const*)a)->dos, (*(struct nent* const*)b)->dos);
}

static void bydos_build(struct addst* a) {
  int i;
  a->bydos = (struct nent**)malloc(sizeof(struct nent*) * (size_t)(a->idx->n + 1));
  a->nbydos = 0;
  for (i = 0; i < a->idx->n; i++)
    if (on_disk(&a->idx->e[i])) a->bydos[a->nbydos++] = &a->idx->e[i];
  qsort(a->bydos, (size_t)a->nbydos, sizeof(struct nent*), dos_cmp);
}

static int bydos_has(struct addst* a, const char* dos) {
  int lo = 0, hi = a->nbydos - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2, c = ci_cmp(a->bydos[m]->dos, dos);
    if (!c) return 1;
    if (c < 0) lo = m + 1;
    else hi = m - 1;
  }
  return 0;
}

/* Poe o arquivo e (rastreado) no index como esta no disco. */
static int add_tracked(struct addst* a, struct nent* e) {
  char full[PATHMAX * 2];
  unsigned char sha[20];
  int n;
  char* b;
  path_join(full, a->root, e->dos);
  b = load_file(full, &n);
  if (!b) {
    e->gone = 1;
    a->removed++;
    if (!a->quiet) printf("  apagado:    %s\n", e->lng);
    return 0;
  }
  git_hash("blob", (unsigned char*)b, n, sha);
  if (memcmp(sha, e->sha, 20) != 0) {
    if (obj_write(a->root, OBJ_BLOB, b, n, sha) != 0) {
      free(b);
      return -1;
    }
    memcpy(e->sha, sha, 20);
    a->changed++;
    if (!a->quiet) printf("  modificado: %s\n", e->lng);
  }
  free(b);
  return 0;
}

static int add_new(struct addst* a, const char* rel, int lower) {
  char full[PATHMAX * 2], lng[PATHMAX * 2];
  unsigned char sha[20];
  int n, k;
  char* b;
  struct nent* e;
  path_join(full, a->root, rel);
  b = load_file(full, &n);
  if (!b) {
    sprintf(err, "nao li %s", full);
    return -1;
  }
  if (obj_write(a->root, OBJ_BLOB, b, n, sha) != 0) {
    free(b);
    return -1;
  }
  free(b);
  long_for(a->idx, rel, lower, lng);
  e = list_find(a->idx, lng);
  if (!e) {
    e = list_push(a->idx);
    e->lng = xstrdup(lng);
    e->hasmode = 1;
  }
  e->dos = xstrdup(rel);
  for (k = 0; e->dos[k]; k++) e->dos[k] = (char)up((unsigned char)e->dos[k]);
  memcpy(e->sha, sha, 20);
  a->added++;
  if (!a->quiet) printf("  novo:       %s\n", lng);
  return 0;
}

/* Arquivos que nao estao no index, em rel (recursivo). cb = 0 so conta. */
typedef int (*untracked_cb)(struct addst* a, const char* rel);

static int scan_untracked(struct addst* a, const char* rel, untracked_cb cb) {
  char path[PATHMAX * 2];
  char* buf;
  char* p;
  int n, total = 0;
  if (rel[0]) path_join(path, a->root, rel);
  else if (!a->root[2]) sprintf(path, "%s\\", a->root);   /* "D:" -> "D:\" */
  else strcpy(path, a->root);
  buf = (char*)malloc(256 * 1024);
  n = k_dirlist(path, buf, 256 * 1024 - 1);
  if (n < 0) {
    free(buf);
    return 0;
  }
  buf[n] = 0;
  for (p = buf; *p;) {
    char* nl = strchr(p, '\n');
    char sub[PATHMAX * 2];
    int l, dir = 0;
    if (nl) *nl = 0;
    l = (int)strlen(p);
    if (l && p[l - 1] == '\r') p[--l] = 0;
    if (l && p[l - 1] == '\\') {
      p[--l] = 0;
      dir = 1;
    }
    if (l && strcmp(p, ".") != 0 && strcmp(p, "..") != 0 && !(!rel[0] && ci_cmp(p, GITDIR) == 0)) {
      if (rel[0]) sprintf(sub, "%s\\%s", rel, p);
      else strcpy(sub, p);
      if (dir) total += scan_untracked(a, sub, cb);
      else if (!bydos_has(a, sub)) {
        total++;
        if (cb && cb(a, sub) != 0) {
          free(buf);
          return -100000;
        }
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  free(buf);
  return total;
}

static int cb_add_new(struct addst* a, const char* rel) { return add_new(a, rel, 1); }

static int cb_print(struct addst* a, const char* rel) {
  (void)a;
  printf("  %s\n", rel);
  return 0;
}

/* -u: todos os rastreados; prefix limita a uma pasta */
static int add_update(struct addst* a, const char* prefix) {
  int i, n = a->idx->n;
  for (i = 0; i < n; i++) {
    struct nent* e = &a->idx->e[i];
    if (!on_disk(e)) continue;
    if (prefix[0] && !(ci_start(e->dos, prefix) && e->dos[strlen(prefix)] == '\\')) continue;
    if (add_tracked(a, e) != 0) return -1;
  }
  return 0;
}

static int cmd_add(int argc, char** argv) {
  char root[PATHMAX], rel[PATHMAX * 2], path[PATHMAX * 2];
  struct list idx;
  struct addst a;
  int i, all = 0, upd = 0, nfiles = 0;
  if (find_root(0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  index_load(root, &idx);
  memset(&a, 0, sizeof(a));
  a.root = root;
  a.idx = &idx;
  for (i = 2; i < argc; i++) {
    if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--update") == 0) upd = 1;
    else if (strcmp(argv[i], "-A") == 0 || strcmp(argv[i], "--all") == 0) all = 1;
    else nfiles++;
  }
  if (!upd && !all && !nfiles) {
    printf("uso: git add -u | -A | arquivo-ou-pasta...\n");
    return 1;
  }
  if (upd || all) {
    if (add_update(&a, "") != 0) goto fail;
    if (all) {
      bydos_build(&a);
      if (scan_untracked(&a, "", cb_add_new) < 0) goto fail;
    }
  }
  for (i = 2; i < argc; i++) {
    struct nent* e;
    if (argv[i][0] == '-') continue;
    if (rel_path(root, argv[i], rel) != 0) goto fail;
    path_join(path, root, rel);
    if (!rel[0] || is_dir(path)) {
      /* pasta: rastreados dela e os novos */
      if (add_update(&a, rel) != 0) goto fail;
      bydos_build(&a);
      if (scan_untracked(&a, rel, cb_add_new) < 0) goto fail;
      continue;
    }
    e = list_find_dos(&idx, rel);
    if (e) {
      if (add_tracked(&a, e) != 0) goto fail;
    } else if (k_fsize(path) >= 0) {
      if (add_new(&a, rel, 0) != 0) goto fail;
    } else {
      printf("nao existe: %s\n", argv[i]);
      return 1;
    }
  }
  if (list_save(root, "INDEX.TXT", &idx) != 0) {
    printf("nao gravou o INDEX.TXT\n");
    return 1;
  }
  if (!a.changed && !a.added && !a.removed) printf("nada mudou\n");
  return 0;
fail:
  printf("erro: %s\n", err);
  return 1;
}

/* ---------- status ---------- */

static int count_ahead(const char* root, const unsigned char* cur, const unsigned char* base) {
  unsigned char c[20], tree[20], parent[20];
  int n = 0;
  memcpy(c, cur, 20);
  while (memcmp(c, base, 20) != 0 && n < 10000) {
    int len, type;
    char* t = (char*)obj_read(root, c, &type, &len);
    if (!t) break;
    n++;
    if (commit_parse(t, tree, parent) != 0 || sha_zero(parent)) {
      free(t);
      break;
    }
    free(t);
    memcpy(c, parent, 20);
  }
  return n;
}

static int cmd_status(int argc, char** argv) {
  char root[PATHMAX], full[PATHMAX + 40], hex[41];
  struct remote r;
  struct list head, idx;
  struct addst a;
  unsigned char cur[20], base[20], now[20];
  int i, staged = 0, unstaged = 0, ahead, nun;
  if (find_root(argc > 2 ? argv[2] : 0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  state_read(root, &r, cur, base);
  sha_hex(cur, hex);
  printf("No ramo %s, commit %.12s\n", r.ref, hex);
  ahead = count_ahead(root, cur, base);
  if (ahead) printf("%d commit(s) local(is) para mandar (git push)\n", ahead);
  list_load(root, "NAMES.TXT", &head);
  index_load(root, &idx);
  /* index x HEAD */
  for (i = 0; i < idx.n; i++) {
    struct nent* e = &idx.e[i];
    struct nent* h;
    if (e->gone) continue;
    h = list_find(&head, e->lng);
    if (!h || memcmp(h->sha, e->sha, 20) != 0 || strcmp(h->mode, e->mode) != 0) {
      if (!staged++) printf("Para o commit (staged):\n");
      printf("  %s %s\n", h ? "modificado:" : "novo:      ", e->lng);
    }
  }
  for (i = 0; i < head.n; i++)
    if (!list_find(&idx, head.e[i].lng)) {
      if (!staged++) printf("Para o commit (staged):\n");
      printf("  apagado:    %s\n", head.e[i].lng);
    }
  /* disco x index */
  for (i = 0; i < idx.n; i++) {
    struct nent* e = &idx.e[i];
    const char* what = 0;
    if (!on_disk(e)) continue;
    path_join(full, root, e->dos);
    if (file_sha(full, now) != 0) what = "apagado:   ";
    else if (memcmp(now, e->sha, 20) != 0) what = "modificado:";
    if (what) {
      if (!unstaged++) printf("Fora do commit (git add -u para incluir):\n");
      printf("  %s %s\n", what, e->lng);
    }
  }
  memset(&a, 0, sizeof(a));
  a.root = root;
  a.idx = &idx;
  bydos_build(&a);
  nun = scan_untracked(&a, "", 0);
  if (nun > 0) {
    printf("Nao rastreados (git add arquivo, ou git add -A):\n");
    scan_untracked(&a, "", cb_print);
  }
  if (!staged && !unstaged && nun <= 0) printf("nada mudou (%d arquivos)\n", idx.n);
  return 0;
}

/* ---------- diff ---------- */

struct line {
  const char* p;
  int n;
};

struct op {
  int t;    /* 0 igual, -1 saiu, 1 entrou */
  int a;
  int b;
};

static int split_lines(const char* s, int len, struct line** out) {
  int n = 0, i, k = 0;
  struct line* v;
  for (i = 0; i < len; i++)
    if (s[i] == '\n') n++;
  if (len && s[len - 1] != '\n') n++;
  v = (struct line*)malloc(sizeof(struct line) * (size_t)(n + 1));
  for (i = 0; i < len;) {
    int j = i;
    while (j < len && s[j] != '\n') j++;
    v[k].p = s + i;
    v[k].n = j - i;
    k++;
    i = j + 1;
  }
  *out = v;
  return k;
}

static int line_eq(const struct line* a, const struct line* b) {
  int x = a->n, y = b->n;
  if (x && a->p[x - 1] == '\r') x--;
  if (y && b->p[y - 1] == '\r') y--;
  return x == y && memcmp(a->p, b->p, (size_t)x) == 0;
}

#define DIFF_DMAX 2000

/* Myers O(ND). Devolve o numero de ops; ops em *out (malloc). */
static int diff_ops(struct line* A, int N, struct line* B, int M, struct op** out) {
  int pre = 0, suf = 0, n, m, max, d, k, x, y, D = -1, no = 0, i;
  int* V;
  int** trace;
  struct op* ops = (struct op*)malloc(sizeof(struct op) * (size_t)(N + M + 1));
  struct line* a;
  struct line* b;
  while (pre < N && pre < M && line_eq(&A[pre], &B[pre])) pre++;
  while (suf < N - pre && suf < M - pre && line_eq(&A[N - 1 - suf], &B[M - 1 - suf])) suf++;
  a = A + pre;
  b = B + pre;
  n = N - pre - suf;
  m = M - pre - suf;
  max = n + m;
  if (max > DIFF_DMAX) max = DIFF_DMAX;
  V = (int*)calloc((size_t)(2 * max + 3), sizeof(int));
  trace = (int**)calloc((size_t)(max + 2), sizeof(int*));
  for (d = 0; d <= max && D < 0; d++) {
    trace[d] = (int*)malloc(sizeof(int) * (size_t)(2 * max + 3));
    memcpy(trace[d], V, sizeof(int) * (size_t)(2 * max + 3));
    for (k = -d; k <= d; k += 2) {
      if (k == -d || (k != d && V[max + 1 + k - 1] < V[max + 1 + k + 1])) x = V[max + 1 + k + 1];
      else x = V[max + 1 + k - 1] + 1;
      y = x - k;
      while (x < n && y < m && line_eq(&a[x], &b[y])) {
        x++;
        y++;
      }
      V[max + 1 + k] = x;
      if (x >= n && y >= m) {
        D = d;
        break;
      }
    }
  }
  /* sufixo, de tras para frente */
  for (i = 0; i < suf; i++) {
    ops[no].t = 0;
    ops[no].a = N - 1 - i;
    ops[no].b = M - 1 - i;
    no++;
  }
  if (D < 0) {
    /* diferentes demais: tudo sai e tudo entra */
    for (i = m - 1; i >= 0; i--) {
      ops[no].t = 1;
      ops[no].a = 0;
      ops[no].b = pre + i;
      no++;
    }
    for (i = n - 1; i >= 0; i--) {
      ops[no].t = -1;
      ops[no].a = pre + i;
      ops[no].b = 0;
      no++;
    }
  } else {
    x = n;
    y = m;
    for (d = D; d > 0; d--) {
      int* v = trace[d];
      int pk, px, py;
      k = x - y;
      if (k == -d || (k != d && v[max + 1 + k - 1] < v[max + 1 + k + 1])) pk = k + 1;
      else pk = k - 1;
      px = v[max + 1 + pk];
      py = px - pk;
      while (x > px && y > py) {
        x--;
        y--;
        ops[no].t = 0;
        ops[no].a = pre + x;
        ops[no].b = pre + y;
        no++;
      }
      if (x == px) {
        y--;
        ops[no].t = 1;
        ops[no].a = pre + x;
        ops[no].b = pre + y;
      } else {
        x--;
        ops[no].t = -1;
        ops[no].a = pre + x;
        ops[no].b = pre + y;
      }
      no++;
      x = px;
      y = py;
    }
    while (x > 0 && y > 0) {
      x--;
      y--;
      ops[no].t = 0;
      ops[no].a = pre + x;
      ops[no].b = pre + y;
      no++;
    }
  }
  for (i = pre - 1; i >= 0; i--) {
    ops[no].t = 0;
    ops[no].a = i;
    ops[no].b = i;
    no++;
  }
  /* inverte */
  for (i = 0; i < no / 2; i++) {
    struct op t = ops[i];
    ops[i] = ops[no - 1 - i];
    ops[no - 1 - i] = t;
  }
  for (d = 0; d <= max + 1; d++) free(trace[d]);
  free(trace);
  free(V);
  *out = ops;
  return no;
}

static void print_line(char c, const struct line* l) {
  int n = l->n;
  if (n && l->p[n - 1] == '\r') n--;
  printf("%c%.*s\n", c, n, l->p);
}

static int is_binary(const unsigned char* s, int n) {
  int i;
  for (i = 0; i < n && i < 8000; i++)
    if (!s[i]) return 1;
  return 0;
}

/* diff unificado de dois textos; 3 linhas de contexto */
static void diff_text(const char* name, const char* oldt, int on, const char* newt, int nn, int isnew, int isdel) {
  struct line* A;
  struct line* B;
  struct op* ops;
  int N, M, no, i, oa = 0, ob = 0;
  printf("diff --git a/%s b/%s\n", name, name);
  if (isnew) printf("arquivo novo\n");
  if (isdel) printf("arquivo apagado\n");
  if (is_binary((const unsigned char*)oldt, on) || is_binary((const unsigned char*)newt, nn)) {
    printf("Arquivos binarios diferem\n");
    return;
  }
  printf("--- %s%s\n", isnew ? "" : "a/", isnew ? "/dev/null" : name);
  printf("+++ %s%s\n", isdel ? "" : "b/", isdel ? "/dev/null" : name);
  N = split_lines(oldt, on, &A);
  M = split_lines(newt, nn, &B);
  no = diff_ops(A, N, B, M, &ops);
  i = 0;
  while (i < no) {
    int s, e, j, ca = 0, cb = 0, sa, sb, r;
    if (ops[i].t == 0) {
      i++;
      continue;
    }
    s = i - 3 < 0 ? 0 : i - 3;
    e = i;
    for (;;) {
      while (e < no && ops[e].t != 0) e++;
      r = e;
      while (r < no && ops[r].t == 0) r++;
      if (r < no && r - e <= 6) {
        e = r;
        continue;
      }
      e = e + 3 > no ? no : e + 3;
      break;
    }
    /* linhas antes do pedaco */
    oa = 0;
    ob = 0;
    for (j = 0; j < s; j++) {
      if (ops[j].t <= 0) oa++;
      if (ops[j].t >= 0) ob++;
    }
    for (j = s; j < e; j++) {
      if (ops[j].t <= 0) ca++;
      if (ops[j].t >= 0) cb++;
    }
    sa = ca ? oa + 1 : oa;
    sb = cb ? ob + 1 : ob;
    printf("@@ -%d,%d +%d,%d @@\n", sa, ca, sb, cb);
    for (j = s; j < e; j++) {
      if (ops[j].t == 0) print_line(' ', &A[ops[j].a]);
      else if (ops[j].t < 0) print_line('-', &A[ops[j].a]);
      else print_line('+', &B[ops[j].b]);
    }
    i = e;
  }
  free(ops);
  free(A);
  free(B);
}

static int diff_wanted(int nf, char** files, const struct nent* e) {
  int i;
  if (!nf) return 1;
  for (i = 0; i < nf; i++) {
    int l = (int)strlen(files[i]);
    if (!files[i][0] || ci_cmp(e->dos, files[i]) == 0 ||
        (ci_start(e->dos, files[i]) && e->dos[l] == '\\'))
      return 1;
  }
  return 0;
}

static int cmd_diff(int argc, char** argv) {
  char root[PATHMAX], full[PATHMAX + 40];
  struct list head, idx;
  char* files[64];
  int nf = 0, cached = 0, i, shown = 0;
  if (find_root(0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  for (i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--cached") == 0 || strcmp(argv[i], "--staged") == 0) cached = 1;
    else if (nf < 64) {
      char rel[PATHMAX * 2];
      if (rel_path(root, argv[i], rel) != 0) {
        printf("%s\n", err);
        return 1;
      }
      files[nf++] = xstrdup(rel);
    }
  }
  list_load(root, "NAMES.TXT", &head);
  index_load(root, &idx);
  if (cached) {
    /* index x HEAD */
    for (i = 0; i < idx.n; i++) {
      struct nent* e = &idx.e[i];
      struct nent* h;
      unsigned char* o = 0;
      unsigned char* nw;
      int ol = 0, nl;
      if (e->gone || !diff_wanted(nf, files, e)) continue;
      h = list_find(&head, e->lng);
      if (h && memcmp(h->sha, e->sha, 20) == 0) continue;
      if (h && !(o = blob_get(root, h->sha, &ol))) {
        printf("sem o conteudo antigo de %s\n", e->lng);
        continue;
      }
      nw = blob_get(root, e->sha, &nl);
      if (!nw) {
        printf("sem o conteudo de %s\n", e->lng);
        free(o);
        continue;
      }
      diff_text(e->lng, o ? (char*)o : "", ol, (char*)nw, nl, !h, 0);
      shown++;
      free(o);
      free(nw);
    }
    for (i = 0; i < head.n; i++) {
      struct nent* h = &head.e[i];
      unsigned char* o;
      int ol;
      if (list_find(&idx, h->lng) || !diff_wanted(nf, files, h)) continue;
      o = blob_get(root, h->sha, &ol);
      if (!o) continue;
      diff_text(h->lng, (char*)o, ol, "", 0, 0, 1);
      shown++;
      free(o);
    }
    return 0;
  }
  /* disco x index */
  for (i = 0; i < idx.n; i++) {
    struct nent* e = &idx.e[i];
    unsigned char now[20];
    unsigned char* o;
    char* nw;
    int ol, nl = 0;
    if (!on_disk(e) || !diff_wanted(nf, files, e)) continue;
    path_join(full, root, e->dos);
    nw = load_file(full, &nl);
    if (nw) {
      git_hash("blob", (unsigned char*)nw, nl, now);
      if (memcmp(now, e->sha, 20) == 0) {
        free(nw);
        continue;
      }
    }
    o = blob_get(root, e->sha, &ol);
    if (!o) {
      printf("%s mudou, mas o conteudo antigo nao esta aqui (sem _GIT\\HEAD.PCK; faca o clone de novo)\n", e->lng);
      free(nw);
      continue;
    }
    diff_text(e->lng, (char*)o, ol, nw ? nw : "", nl, 0, !nw);
    shown++;
    free(o);
    free(nw);
  }
  (void)shown;
  return 0;
}

/* ---------- commit ---------- */

static int days_from_civil(int y, int m, int d) {
  int era, yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static int now_epoch(void) {
  int dt = k_date(), tm = k_time();
  if (dt <= 0 || tm < 0) return -1;
  return days_from_civil(dt / 10000, dt / 100 % 100, dt % 100) * 86400 + tm / 10000 * 3600 + tm / 100 % 100 * 60 +
         tm % 100;
}

/* index -> arvores, commit, NAMES.TXT. 0 ok, 1 nada para commitar. */
static int do_commit(const char* root, const char* msg) {
  struct remote r;
  struct list head, idx;
  struct tctx th, ti;
  unsigned char cur[20], base[20], htree[20], tree[20], csha[20];
  char name[128], email[128], tz[16], hex[41], path[PATHMAX];
  char* hc;
  int hl, t, i, has_index;
  struct sbuf s;
  if (state_read(root, &r, cur, base) != 0) {
    sprintf(err, "STATE.TXT invalido");
    return -1;
  }
  cfg_get(root, "user.name", name, sizeof(name));
  cfg_get(root, "user.email", email, sizeof(email));
  if (!name[0] || !email[0]) {
    sprintf(err, "falta o autor. Rode:\n  git config --global user.name Seu Nome\n  git config --global user.email voce@exemplo.com");
    return -1;
  }
  if (!cfg_get(root, "tz", tz, sizeof(tz))) strcpy(tz, "+0000");
  t = now_epoch();
  if (t < 0) {
    sprintf(err, "o relogio do sistema nao tem data");
    return -1;
  }
  hc = commit_get(root, cur, &hl);
  if (!hc || commit_parse(hc, htree, 0) != 0) {
    sprintf(err, "nao achei o commit atual (COMMIT.TXT)");
    free(hc);
    return -1;
  }
  free(hc);
  /* a lista do HEAD tem que dar a arvore do HEAD */
  list_load(root, "NAMES.TXT", &head);
  memset(&th, 0, sizeof(th));
  th.root = root;
  list_tree(&head, &th, tree);
  if (memcmp(tree, htree, 20) != 0) {
    if (memcmp(cur, base, 20) != 0 || reindex(root, &r, cur) != 0) {
      if (!err[0]) sprintf(err, "a lista NAMES.TXT nao confere com o commit");
      return -1;
    }
    list_load(root, "NAMES.TXT", &head);
    th.nknown = 0;
    list_tree(&head, &th, tree);
    if (memcmp(tree, htree, 20) != 0) {
      sprintf(err, "a lista NAMES.TXT nao confere com o commit, nem refeita");
      return -1;
    }
  }
  has_index = index_load(root, &idx);
  /* entradas antigas do INDEX sem modo pegam o modo do HEAD */
  for (i = 0; i < idx.n; i++) {
    struct nent* h;
    if (idx.e[i].hasmode) continue;
    h = list_find(&head, idx.e[i].lng);
    if (h) strcpy(idx.e[i].mode, h->mode);
    idx.e[i].hasmode = 1;
  }
  memset(&ti, 0, sizeof(ti));
  ti.root = root;
  ti.write = 1;
  ti.known = th.known;
  ti.nknown = th.nknown;
  ti.capknown = th.capknown;
  if (list_tree(&idx, &ti, tree) != 0) return -1;
  if (memcmp(tree, htree, 20) == 0) {
    if (has_index) {
      git_file(root, "INDEX.TXT", path);
      k_fdelete(path);
    }
    return 1;
  }
  memset(&s, 0, sizeof(s));
  sha_hex(tree, hex);
  sb_add(&s, "tree ", 5);
  sb_add(&s, hex, 40);
  sb_add(&s, "\nparent ", 8);
  sha_hex(cur, hex);
  sb_add(&s, hex, 40);
  {
    char who[400];
    sprintf(who, "\nauthor %s <%s> %d %s\ncommitter %s <%s> %d %s\n\n", name, email, t, tz, name, email, t, tz);
    sb_add(&s, who, -1);
  }
  sb_add(&s, msg, -1);
  if (s.p[s.len - 1] != '\n') sb_add(&s, "\n", 1);
  if (obj_write(root, OBJ_COMMIT, s.p, s.len, csha) != 0) {
    free(s.p);
    return -1;
  }
  free(s.p);
  if (list_save(root, "NAMES.TXT", &idx) != 0) {
    sprintf(err, "nao gravou NAMES.TXT");
    return -1;
  }
  git_file(root, "INDEX.TXT", path);
  k_fdelete(path);
  state_write(root, &r, csha, base);
  sha_hex(csha, hex);
  {
    const char* br = strrchr(r.ref, '/');
    int nl = 0;
    while (msg[nl] && msg[nl] != '\n') nl++;
    printf("[%s %.7s] %.*s\n", br ? br + 1 : r.ref, hex, nl, msg);
  }
  return 0;
}

static int cmd_commit(int argc, char** argv) {
  char root[PATHMAX];
  struct sbuf msg;
  int i, all = 0, r;
  memset(&msg, 0, sizeof(msg));
  for (i = 2; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "-a") == 0 || strcmp(a, "--all") == 0) {
      all = 1;
      continue;
    }
    if (strcmp(a, "-m") == 0 || strcmp(a, "-am") == 0 || strcmp(a, "-ma") == 0) {
      if (strcmp(a, "-m") != 0) all = 1;
      for (i++; i < argc; i++) {
        if (msg.len) sb_add(&msg, " ", 1);
        sb_add(&msg, argv[i], -1);
      }
      break;
    }
  }
  if (!msg.len) {
    printf("uso: git commit [-a] -m mensagem\n");
    return 1;
  }
  if (find_root(0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  if (all) {
    struct list idx;
    struct addst a;
    index_load(root, &idx);
    memset(&a, 0, sizeof(a));
    a.root = root;
    a.idx = &idx;
    a.quiet = 1;
    if (add_update(&a, "") != 0 || list_save(root, "INDEX.TXT", &idx) != 0) {
      printf("erro: %s\n", err);
      return 1;
    }
  }
  err[0] = 0;
  r = do_commit(root, msg.p);
  free(msg.p);
  if (r == 1) {
    printf("nada para o commit (use git add -u, git add arquivo ou git commit -a)\n");
    return 1;
  }
  if (r != 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  return 0;
}

/* ---------- push ---------- */

struct oset {
  unsigned char (*sha)[20];
  int* type;
  int n;
  int cap;
};

static int oset_has(struct oset* o, const unsigned char* sha) {
  int i;
  for (i = 0; i < o->n; i++)
    if (memcmp(o->sha[i], sha, 20) == 0) return 1;
  return 0;
}

static void oset_add(struct oset* o, const unsigned char* sha, int type) {
  if (o->n == o->cap) {
    o->cap = o->cap ? o->cap * 2 : 64;
    o->sha = (unsigned char (*)[20])realloc(o->sha, 20 * (size_t)o->cap);
    o->type = (int*)realloc(o->type, sizeof(int) * (size_t)o->cap);
  }
  memcpy(o->sha[o->n], sha, 20);
  o->type[o->n++] = type;
}

/* arvores e blobs criados aqui; o que nao esta em OBJ o servidor ja tem */
static void walk_tree(const char* root, const unsigned char* sha, struct oset* o) {
  unsigned char* t;
  int type, len, p = 0;
  if (oset_has(o, sha)) return;
  t = obj_read(root, sha, &type, &len);
  if (!t || type != OBJ_TREE) {
    free(t);
    return;
  }
  oset_add(o, sha, OBJ_TREE);
  while (p < len) {
    char mode[8];
    int k = 0;
    const unsigned char* s;
    while (p < len && t[p] != ' ' && k < 7) mode[k++] = (char)t[p++];
    mode[k] = 0;
    while (p < len && t[p]) p++;
    p++;
    s = t + p;
    p += 20;
    if (strcmp(mode, "40000") == 0) walk_tree(root, s, o);
    else if (strcmp(mode, "160000") != 0 && !oset_has(o, s) && obj_exists(root, s)) oset_add(o, s, OBJ_BLOB);
  }
  free(t);
}

static unsigned adler32(const unsigned char* d, int n) {
  unsigned a = 1, b = 0;
  int i;
  for (i = 0; i < n; i++) {
    a = (a + d[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

/* zlib sem compressao (blocos "stored"): o servidor aceita igual */
static void z_store(struct sbuf* s, const unsigned char* d, int n) {
  unsigned ad = adler32(d, n);
  int p = 0;
  sb_byte(s, 0x78);
  sb_byte(s, 0x01);
  do {
    int k = n - p > 65535 ? 65535 : n - p;
    sb_byte(s, p + k >= n ? 1 : 0);
    sb_byte(s, k & 255);
    sb_byte(s, k >> 8);
    sb_byte(s, ~k & 255);
    sb_byte(s, (~k >> 8) & 255);
    sb_add(s, (const char*)d + p, k);
    p += k;
  } while (p < n);
  sb_byte(s, (int)(ad >> 24) & 255);
  sb_byte(s, (int)(ad >> 16) & 255);
  sb_byte(s, (int)(ad >> 8) & 255);
  sb_byte(s, (int)ad & 255);
}

static int build_pack(const char* root, struct oset* o, struct sbuf* s) {
  unsigned char sum[20];
  sha1_ctx c;
  int i, start = s->len;   /* o pedido ja tem as linhas de comando antes do PACK */
  sb_add(s, "PACK", 4);
  sb_byte(s, 0);
  sb_byte(s, 0);
  sb_byte(s, 0);
  sb_byte(s, 2);
  sb_byte(s, (o->n >> 24) & 255);
  sb_byte(s, (o->n >> 16) & 255);
  sb_byte(s, (o->n >> 8) & 255);
  sb_byte(s, o->n & 255);
  for (i = 0; i < o->n; i++) {
    int type, len;
    unsigned size;
    unsigned char* d = obj_read(root, o->sha[i], &type, &len);
    int c0;
    if (!d) {
      char hex[41];
      sha_hex(o->sha[i], hex);
      sprintf(err, "objeto %s sumiu de _GIT\\OBJ", hex);
      return -1;
    }
    size = (unsigned)len;
    c0 = (type << 4) | (int)(size & 15);
    size >>= 4;
    if (size) c0 |= 0x80;
    sb_byte(s, c0);
    while (size) {
      int b = (int)(size & 0x7F);
      size >>= 7;
      sb_byte(s, b | (size ? 0x80 : 0));
    }
    z_store(s, d, len);
    free(d);
  }
  sha1_init(&c);
  sha1_update(&c, s->p + start, s->len - start);
  sha1_final(&c, sum);
  sb_add(s, (const char*)sum, 20);
  return 0;
}

static void base64(const unsigned char* in, int n, char* out) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int i, o = 0;
  for (i = 0; i < n; i += 3) {
    unsigned v = (unsigned)in[i] << 16;
    if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    out[o++] = t[(v >> 18) & 63];
    out[o++] = t[(v >> 12) & 63];
    out[o++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? t[v & 63] : '=';
  }
  out[o] = 0;
}

static int cmd_push(int argc, char** argv) {
  struct remote r;
  char root[PATHMAX], token[256], login[128], cred[400], hex[41], bhex[41], url[400], line[300];
  unsigned char cur[20], base[20], c[20], tree[20], parent[20];
  struct oset o;
  struct sbuf req;
  char* body;
  int len, st, p, ncommits = 0, ok = 0, unpack_ok = 0;
  if (find_root(argc > 2 ? argv[2] : 0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  if (state_read(root, &r, cur, base) != 0) {
    printf("STATE.TXT invalido\n");
    return 1;
  }
  if (memcmp(cur, base, 20) == 0) {
    printf("Nada para mandar (nenhum commit local).\n");
    return 0;
  }
  if (strncmp(r.ref, "refs/heads/", 11) != 0) {
    printf("o ramo %s nao e um refs/heads/...: nao sei para onde mandar\n", r.ref);
    return 1;
  }
  if (!cfg_get(root, "token", token, sizeof(token)) || !token[0]) {
    printf("falta o token de acesso (no GitHub: Settings > Developer settings > Personal access tokens,\n");
    printf("com permissao de escrita no repositorio). Guarde com:\n");
    printf("  git config --global token SEU_TOKEN     (ou SET GIT_TOKEN=...)\n");
    return 1;
  }
  if (!cfg_get(root, "user", login, sizeof(login)) || !login[0]) strcpy(login, "x-access-token");
  sprintf(line, "%s:%s", login, token);
  base64((unsigned char*)line, (int)strlen(line), cred);
  sprintf(http_auth, "Authorization: Basic %s", cred);
  /* objetos dos commits locais */
  memset(&o, 0, sizeof(o));
  memcpy(c, cur, 20);
  while (memcmp(c, base, 20) != 0) {
    int type, l;
    char* t = (char*)obj_read(root, c, &type, &l);
    if (!t || type != OBJ_COMMIT || commit_parse(t, tree, parent) != 0) {
      sha_hex(c, hex);
      printf("commit %s nao esta em _GIT\\OBJ\n", hex);
      free(t);
      return 1;
    }
    free(t);
    oset_add(&o, c, OBJ_COMMIT);
    walk_tree(root, tree, &o);
    ncommits++;
    if (sha_zero(parent)) break;
    memcpy(c, parent, 20);
  }
  if (!k_net_ready()) {
    printf("rede nao pronta\n");
    return 1;
  }
  if (get_refs(&r, 1) != 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  if (memcmp(r.head, base, 20) != 0) {
    sha_hex(r.head, hex);
    printf("O servidor mudou: %s esta em %.12s, e os commits daqui saem de outro.\n", r.ref, hex);
    printf("Este git nao faz merge; o push foi cancelado.\n");
    return 1;
  }
  memset(&req, 0, sizeof(req));
  sha_hex(base, bhex);
  sha_hex(cur, hex);
  {
    int n = sprintf(line, "%s %s %s", bhex, hex, r.ref);
    line[n++] = 0;
    n += sprintf(line + n, "report-status agent=git/sodos\n");
    pkt_n(&req, line, n);
  }
  sb_add(&req, "0000", 4);
  if (build_pack(root, &o, &req) != 0) {
    printf("erro: %s\n", err);
    free(req.p);
    return 1;
  }
  printf("mandando %d commit(s), %d objetos (%d KB)\n", ncommits, o.n, req.len / 1024);
  sprintf(url, "%s/git-receive-pack", r.url);
  st = http_req(url, "application/x-git-receive-pack-request", req.p, req.len, &body, &len, err);
  free(req.p);
  if (st < 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  if (st != 200) {
    printf("erro: HTTP %d em git-receive-pack\n", st);
    if (body && len) printf("%.*s\n", len > 400 ? 400 : len, body);
    free(body);
    return 1;
  }
  for (p = 0; p + 4 <= len;) {
    int n = pkt_len(body + p), l;
    char* s;
    if (n < 0) break;
    if (n == 0) {
      p += 4;
      continue;
    }
    if (n < 4 || p + n > len) break;
    l = n - 4 < (int)sizeof(line) - 1 ? n - 4 : (int)sizeof(line) - 1;
    memcpy(line, body + p + 4, (size_t)l);
    line[l] = 0;
    p += n;
    if (l && line[l - 1] == '\n') line[--l] = 0;
    s = line;
    if (strcmp(s, "unpack ok") == 0) unpack_ok = 1;
    else if (strncmp(s, "ok ", 3) == 0 && strcmp(s + 3, r.ref) == 0) ok = 1;
    else printf("servidor: %s\n", s);
  }
  free(body);
  if (!unpack_ok || !ok) {
    printf("o servidor nao aceitou o push\n");
    return 1;
  }
  state_write(root, &r, cur, cur);
  printf("%.7s..%.7s  %s\n", bhex, hex, r.ref);
  return 0;
}

/* ---------- log ---------- */

/* dias desde 1970-01-01 -> data */
static void civil(int days, int* y, int* m, int* d) {
  int z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
  int doe = z - era * 146097;
  int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = yoe + era * 400 + (*m <= 2);
}

static void print_person(const char* label, char* line) {
  /* "Nome <email> 1730000000 -0300" */
  char* gt = strrchr(line, '>');
  int t, tz, y, mo, d, sign, mins;
  char* tzs;
  if (!gt) {
    printf("%s %s\n", label, line);
    return;
  }
  gt[1] = 0;
  t = atoi(gt + 2);
  tzs = strchr(gt + 2, ' ');
  tz = tzs ? atoi(tzs + 1) : 0;
  sign = tz < 0 ? -1 : 1;
  mins = sign * ((sign * tz) / 100 * 60 + (sign * tz) % 100);
  t += mins * 60;
  civil(t / 86400, &y, &mo, &d);
  printf("%s %s\nData:  %04d-%02d-%02d %02d:%02d %s\n", label, line, y, mo, d, (t % 86400) / 3600,
         (t % 3600) / 60, tzs ? tzs + 1 : "");
}

static int cmd_log(int argc, char** argv) {
  char root[PATHMAX], hex[41];
  struct remote r;
  unsigned char cur[20], base[20], tree[20], parent[20];
  int n = 0;
  if (find_root(argc > 2 ? argv[2] : 0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  state_read(root, &r, cur, base);
  for (;;) {
    char* t;
    char* p;
    int len, body = 0;
    sha_hex(cur, hex);
    t = commit_get(root, cur, &len);
    if (!t) {
      if (!n) printf("sem o commit %s\n", hex);
      break;
    }
    if (n) printf("\n");
    printf("commit %s", hex);
    if (!n) printf(" (%s)", r.ref);
    if (memcmp(cur, base, 20) == 0) printf(" (servidor)");
    printf("\n");
    commit_parse(t, tree, parent);
    for (p = t; *p;) {
      char* nl = strchr(p, '\n');
      if (nl) *nl = 0;
      if (body) printf("    %s\n", p);
      else if (!*p) {
        body = 1;
        printf("\n");
      } else if (strncmp(p, "author ", 7) == 0) {
        print_person("Autor:", p + 7);
      }
      if (!nl) break;
      p = nl + 1;
    }
    free(t);
    n++;
    if (sha_zero(parent)) break;
    memcpy(cur, parent, 20);
  }
  printf("(clone raso: o historico para no commit do clone/pull)\n");
  return 0;
}

/* ---------- config ---------- */

static int cmd_config(int argc, char** argv) {
  char root[PATHMAX], path[PATHMAX], val[300];
  struct sbuf v;
  const char* key;
  int i = 2, global = 0, have_root;
  if (i < argc && strcmp(argv[i], "--global") == 0) {
    global = 1;
    i++;
  }
  have_root = find_root(0, root) == 0;
  if (!global && !have_root) global = 1;
  if (i >= argc || strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
    int len;
    char* t;
    cfg_path(have_root ? root : 0, 1, path);
    t = load_file(path, &len);
    printf("# %s\n%s", path, t ? t : "");
    free(t);
    if (have_root) {
      cfg_path(root, 0, path);
      t = load_file(path, &len);
      printf("# %s\n%s", path, t ? t : "");
      free(t);
    }
    return 0;
  }
  if (i + 1 >= argc) {
    if (!cfg_get(have_root ? root : 0, argv[i], val, sizeof(val))) return 1;
    if (strcmp(argv[i], "token") == 0 && strlen(val) > 8) strcpy(val + 4, "...");
    printf("%s\n", val);
    return 0;
  }
  key = argv[i];
  memset(&v, 0, sizeof(v));
  for (i++; i < argc; i++) {
    if (v.len) sb_add(&v, " ", 1);
    sb_add(&v, argv[i], -1);
  }
  cfg_path(have_root ? root : 0, global, path);
  if (strcmp(v.p, "\"\"") == 0) v.p[0] = 0;
  if (cfg_set(path, key, v.p) != 0) {
    printf("nao gravou %s\n", path);
    return 1;
  }
  free(v.p);
  return 0;
}

static void usage(void) {
  printf("git do SODOS (clone raso)\n");
  printf("  git clone https://github.com/dono/projeto [pasta]\n");
  printf("  git pull [pasta]        atualiza para o ultimo commit\n");
  printf("  git status [pasta]      o que mudou\n");
  printf("  git add -u              poe no commit os rastreados mudados/apagados\n");
  printf("  git add -A              idem, mais os arquivos novos\n");
  printf("  git add arquivo|pasta   poe esse no commit\n");
  printf("  git diff [--cached] [arquivo]\n");
  printf("  git commit [-a] -m mensagem\n");
  printf("  git push                manda os commits (git config token ...)\n");
  printf("  git log [pasta]\n");
  printf("  git config [--global] chave [valor]   user.name, user.email, token, tz (-0300)\n");
  printf("Nomes longos viram 8.3 (LEIAME~1.MD); veja _GIT\\NAMES.TXT.\n");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 1;
  }
  if (strcmp(argv[1], "clone") == 0) return cmd_clone(argc, argv);
  if (strcmp(argv[1], "pull") == 0) return cmd_pull(argc, argv);
  if (strcmp(argv[1], "status") == 0) return cmd_status(argc, argv);
  if (strcmp(argv[1], "log") == 0) return cmd_log(argc, argv);
  if (strcmp(argv[1], "add") == 0) return cmd_add(argc, argv);
  if (strcmp(argv[1], "commit") == 0) return cmd_commit(argc, argv);
  if (strcmp(argv[1], "push") == 0) return cmd_push(argc, argv);
  if (strcmp(argv[1], "diff") == 0) return cmd_diff(argc, argv);
  if (strcmp(argv[1], "config") == 0) return cmd_config(argc, argv);
  usage();
  return 1;
}
