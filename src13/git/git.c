/* git.c - comandos do git do SODOS.
 *   git clone https://github.com/dono/projeto [pasta]
 *   git pull [pasta]       busca o commit novo e atualiza os arquivos
 *   git status [pasta]     arquivos mudados ou apagados desde o clone/pull
 *   git log [pasta]        o commit que esta na pasta
 * O clone e raso (so o ultimo commit). Arquivos com nome que nao cabe em
 * 8.3 ganham um nome curto (LEIAME~1.MD); a lista fica em _GIT\NAMES.TXT. */
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

static void pkt(struct sbuf* s, const char* line) {
  char h[8];
  sprintf(h, "%04x", (int)strlen(line) + 4);
  sb_add(s, h, 4);
  sb_add(s, line, -1);
}

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

static int get_refs(struct remote* r) {
  char url[400];
  char* body;
  int len, st, p = 0, have = 0;
  unsigned char main_sha[20];
  int have_main = 0;
  sprintf(url, "%s/info/refs?service=git-upload-pack", r->url);
  printf("buscando refs de %s\n", r->url);
  st = http_req(url, 0, 0, 0, &body, &len, err);
  if (st < 0) return -1;
  if (st == 401 || st == 404) {
    sprintf(err, "repositorio nao encontrado (ou privado): HTTP %d", st);
    free(body);
    return -1;
  }
  if (st != 200 || len < 4) {
    sprintf(err, "HTTP %d em info/refs (o servidor fala o protocolo do git?)", st);
    free(body);
    return -1;
  }
  r->ref[0] = 0;
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

static int up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

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

/* ---------- lista de nomes (NAMES.TXT): curto TAB longo TAB sha ---------- */

struct nent {
  char* dos;
  char* lng;
  unsigned char sha[20];
  int seen;
};

static struct nent* olds;
static int nold;
static char* olds_text;

static int nent_cmp(const void* a, const void* b) {
  return strcmp(((const struct nent*)a)->lng, ((const struct nent*)b)->lng);
}

static int names_load(const char* root, struct nent** out, char** text) {
  char path[PATHMAX];
  int len, n = 0, i;
  char* t;
  char* p;
  struct nent* e;
  sprintf(path, "%s\\%s\\NAMES.TXT", root, GITDIR);
  t = load_file(path, &len);
  *out = 0;
  *text = t;
  if (!t) return 0;
  for (i = 0; i < len; i++)
    if (t[i] == '\n') n++;
  e = (struct nent*)calloc((size_t)n + 1, sizeof(struct nent));
  n = 0;
  p = t;
  while (*p) {
    char* a = p;
    char* b;
    char* c;
    char* nl = strchr(p, '\n');
    if (!nl) break;
    *nl = 0;
    p = nl + 1;
    b = strchr(a, '\t');
    if (!b) continue;
    *b++ = 0;
    c = strchr(b, '\t');
    if (!c) continue;
    *c++ = 0;
    e[n].dos = a;
    e[n].lng = b;
    if (hex_sha(c, e[n].sha) != 0) continue;
    n++;
  }
  qsort(e, (size_t)n, sizeof(struct nent), nent_cmp);
  *out = e;
  return n;
}

static struct nent* old_find(const char* lng) {
  int lo = 0, hi = nold - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2, c = strcmp(olds[m].lng, lng);
    if (!c) return &olds[m];
    if (c < 0) lo = m + 1;
    else hi = m - 1;
  }
  return 0;
}

/* ---------- checkout ---------- */

struct co {
  const char* root;      /* "D:\\SODOS" */
  struct sbuf names;
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

static void put_file(struct co* c, const char* dosrel, const char* lng, const unsigned char* sha) {
  char full[PATHMAX + 40], hex[41];
  struct nent* o = nold ? old_find(lng) : 0;
  unsigned char* data;
  int type, size;
  c->files++;
  sha_hex(sha, hex);
  path_join(full, c->root, dosrel);
  /* o kernel aceita ate ~60 caracteres depois de "D:" e 8 pastas */
  if ((int)strlen(full) - 2 > 60 || depth_of(full) > 8) {
    if (c->skipped < 8) printf("  caminho longo demais, pulado: %s\n", lng);
    c->skipped++;
    return;
  }
  sb_add(&c->names, dosrel, -1);
  sb_add(&c->names, "\t", 1);
  sb_add(&c->names, lng, -1);
  sb_add(&c->names, "\t", 1);
  sb_add(&c->names, hex, 40);
  sb_add(&c->names, "\n", 1);
  if (o) {
    unsigned char now[20];
    o->seen = 1;
    if (memcmp(o->sha, sha, 20) == 0 && strcmp(o->dos, dosrel) == 0 && k_fsize(full) >= 0) return;
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
    if (strcmp(mode, "160000") == 0) {
      printf("  submodulo ignorado: %s/%s\n", lngrel, name);
      continue;
    }
    make83(name, used, nused, short83);
    strcpy(used[nused++], short83);
    if (dosrel[0]) sprintf(dos, "%s\\%s", dosrel, short83);
    else strcpy(dos, short83);
    if (lngrel[0]) sprintf(lng, "%s/%s", lngrel, name);
    else strcpy(lng, name);
    if (strcmp(mode, "40000") == 0) {
      if (checkout_tree(c, sha, dos, lng) != 0) {
        free(tree);
        free(used);
        return -1;
      }
    } else {
      put_file(c, dos, lng, sha);
    }
  }
  free(tree);
  free(used);
  return 0;
}

/* ---------- estado ---------- */

static int state_read(const char* root, struct remote* r, unsigned char* commit) {
  char path[PATHMAX];
  int len;
  char* t;
  char* p;
  sprintf(path, "%s\\%s\\STATE.TXT", root, GITDIR);
  t = load_file(path, &len);
  if (!t) return -1;
  memset(r, 0, sizeof(*r));
  for (p = t; *p;) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (strncmp(p, "url=", 4) == 0) strncpy(r->url, p + 4, sizeof(r->url) - 1);
    else if (strncmp(p, "ref=", 4) == 0) strncpy(r->ref, p + 4, sizeof(r->ref) - 1);
    else if (strncmp(p, "commit=", 7) == 0) hex_sha(p + 7, commit);
    if (!nl) break;
    p = nl + 1;
  }
  free(t);
  return r->url[0] ? 0 : -1;
}

static int state_write(const char* root, const struct remote* r) {
  char path[PATHMAX], text[600], hex[41];
  sha_hex(r->head, hex);
  sprintf(text, "url=%s\nref=%s\ncommit=%s\n", r->url, r->ref, hex);
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

/* ---------- comandos ---------- */

/* Busca o commit de r e grava os arquivos em root. */
static int fetch_checkout(const char* root, struct remote* r) {
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
  sprintf(path, "%s\\%s\\COMMIT.TXT", root, GITDIR);
  k_fsave(path, (char*)cm, size);
  memset(&c, 0, sizeof(c));
  c.root = root;
  printf("gravando arquivos\n");
  if (checkout_tree(&c, tsha, "", "") != 0) {
    free(resp);
    pack_free();
    return -1;
  }
  pack_free();
  free(resp);
  /* o que saiu do repositorio sai da pasta, se nao foi mudado aqui */
  for (i = 0; i < nold; i++) {
    unsigned char now[20];
    char full[PATHMAX + 40];
    if (olds[i].seen) continue;
    path_join(full, root, olds[i].dos);
    if (file_sha(full, now) != 0) continue;
    if (memcmp(now, olds[i].sha, 20) != 0) {
      printf("  saiu do repositorio mas foi mudado aqui, mantido: %s\n", olds[i].lng);
      continue;
    }
    if (k_fdelete(full) == 0) removed++;
  }
  sprintf(path, "%s\\%s\\NAMES.TXT", root, GITDIR);
  if (k_fsave(path, c.names.p ? c.names.p : "", c.names.len) != 0) printf("aviso: nao gravou %s\n", path);
  free(c.names.p);
  state_write(root, r);
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
  if (get_refs(&r) != 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  printf("clonando %s em %s\n", r.ref, root);
  nold = 0;
  if (fetch_checkout(root, &r) != 0) {
    printf("\nerro: %s\n", err);
    return 1;
  }
  return 0;
}

static int cmd_pull(int argc, char** argv) {
  struct remote r;
  char root[PATHMAX], hex[41];
  unsigned char cur[20];
  if (find_root(argc > 2 ? argv[2] : 0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  memset(cur, 0, sizeof(cur));
  if (state_read(root, &r, cur) != 0) {
    printf("%s\\%s\\STATE.TXT invalido\n", root, GITDIR);
    return 1;
  }
  if (!k_net_ready()) {
    printf("rede nao pronta\n");
    return 1;
  }
  if (get_refs(&r) != 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  if (memcmp(cur, r.head, 20) == 0) {
    sha_hex(cur, hex);
    printf("Ja esta atualizado (%.12s).\n", hex);
    return 0;
  }
  nold = names_load(root, &olds, &olds_text);
  if (fetch_checkout(root, &r) != 0) {
    printf("\nerro: %s\n", err);
    return 1;
  }
  return 0;
}

static int cmd_status(int argc, char** argv) {
  char root[PATHMAX], full[PATHMAX + 40], hex[41];
  struct remote r;
  unsigned char cur[20], now[20];
  int i, changed = 0, gone = 0;
  if (find_root(argc > 2 ? argv[2] : 0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  state_read(root, &r, cur);
  sha_hex(cur, hex);
  printf("%s, commit %.12s\n", r.ref, hex);
  nold = names_load(root, &olds, &olds_text);
  for (i = 0; i < nold; i++) {
    path_join(full, root, olds[i].dos);
    if (file_sha(full, now) != 0) {
      printf("  apagado:    %s\n", olds[i].lng);
      gone++;
    } else if (memcmp(now, olds[i].sha, 20) != 0) {
      printf("  modificado: %s\n", olds[i].lng);
      changed++;
    }
  }
  if (!changed && !gone) printf("nada mudou (%d arquivos)\n", nold);
  return 0;
}

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
  char root[PATHMAX], path[PATHMAX], hex[41];
  struct remote r;
  unsigned char cur[20];
  char* t;
  char* p;
  int len, body = 0;
  if (find_root(argc > 2 ? argv[2] : 0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  state_read(root, &r, cur);
  sprintf(path, "%s\\%s\\COMMIT.TXT", root, GITDIR);
  t = load_file(path, &len);
  if (!t) {
    printf("sem %s\n", path);
    return 1;
  }
  sha_hex(cur, hex);
  printf("commit %s (%s)\n", hex, r.ref);
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
  printf("(clone raso: so o ultimo commit)\n");
  return 0;
}

static void usage(void) {
  printf("git do SODOS (clone raso, so leitura)\n");
  printf("  git clone https://github.com/dono/projeto [pasta]\n");
  printf("  git pull [pasta]     atualiza para o ultimo commit\n");
  printf("  git status [pasta]   arquivos mudados ou apagados\n");
  printf("  git log [pasta]      o commit que esta na pasta\n");
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
  usage();
  return 1;
}
