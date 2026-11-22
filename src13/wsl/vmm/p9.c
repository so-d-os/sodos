/* p9.c - Servidor 9P2000.L da VM do WSL: as unidades do SODOS no Linux.
 *
 * O Linux monta o virtio-9p (tag "sodos") em /mnt/sodos; a raiz lista as
 * unidades montadas (d, z, u...) e o sodos-init faz /mnt/d -> /mnt/sodos/d,
 * como no WSL. Cada fid guarda o caminho do SODOS ("D:\WINE\USERS"); tudo
 * passa pela API de arquivos do kernel (k_fread, k_fsave, k_dir_long...).
 *
 * Escrita: o k_fsave grava o arquivo inteiro, entao o arquivo aberto para
 * escrita fica em RAM (carregado na primeira escrita) e vai ao disco no
 * clunk/fsync. Datas: as do FAT (nao muda mtime: a API nao tem). Donos: root.
 * Roda na thread da CPU da VM (a fila do virtio), como o disco. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vmm.h"
#include "../sodos_api.h"

#define P9_MSIZE (256u * 1024u)
#define P9_FIDS 1024
#define P9_PATH 300

enum {
  E_PERM = 1, E_NOENT = 2, E_IO = 5, E_BADF = 9, E_NOMEM = 12, E_EXIST = 17, E_NOTDIR = 20,
  E_ISDIR = 21, E_INVAL = 22, E_NOSPC = 28, E_ROFS = 30, E_NOSYS = 38, E_NOTEMPTY = 39,
  E_OPNOTSUPP = 95
};

struct p9fid {
  int used;
  char path[P9_PATH]; /* "" = raiz; senao "D:\\..." (sem \\ no fim, exceto "D:\\") */
  int open;
  /* escrita em RAM */
  uint8_t* data;
  uint32_t size, cap;
  int loaded, dirty;
  /* readdir: retrato da pasta */
  struct k_direntl* ents;
  int nents;
};

static struct p9fid* fids; /* k_hmalloc: P9_FIDS */
static uint32_t msize = 8192;
static uint8_t* rq;        /* pedido inteiro */
static uint8_t* rs;        /* resposta */

/* ---------- utilitarios ---------- */
static uint32_t g32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t g64(const uint8_t* p) { return g32(p) | (uint64_t)g32(p + 4) << 32; }
static uint16_t g16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static void p16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t* p, uint32_t v) { p16(p, (uint16_t)v); p16(p + 2, (uint16_t)(v >> 16)); }
static void p64(uint8_t* p, uint64_t v) { p32(p, (uint32_t)v); p32(p + 4, (uint32_t)(v >> 32)); }

/* string 9P: tam[2] bytes -> out (C) ; devolve bytes consumidos ou 0 */
static int gstr(const uint8_t* p, const uint8_t* end, char* out, int max) {
  uint16_t n;
  if (p + 2 > end) return 0;
  n = g16(p);
  if (p + 2 + n > end || n >= max) return 0;
  memcpy(out, p + 2, n);
  out[n] = 0;
  return 2 + n;
}
static uint8_t* pstr(uint8_t* p, const char* s) {
  uint16_t n = (uint16_t)strlen(s);
  p16(p, n);
  memcpy(p + 2, s, n);
  return p + 2 + n;
}

static uint64_t fnv(const char* s) {
  uint64_t h = 1469598103934665603ull;
  for (; *s; s++) {
    char c = *s;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32); /* FAT: sem caixa */
    h = (h ^ (uint8_t)c) * 1099511628211ull;
  }
  return h;
}

static uint8_t* pqid(uint8_t* p, int dir, const char* path) {
  p[0] = dir ? 0x80 : 0;
  p32(p + 1, 0);
  p64(p + 5, fnv(path));
  return p + 13;
}

/* data/hora do FAT -> segundos unix */
static uint64_t fat_unix(uint16_t d, uint16_t t) {
  int y = 1980 + (d >> 9), m = (d >> 5) & 15, dd = d & 31, era;
  unsigned yoe, doy, doe;
  int64_t days;
  if (!d || m < 1 || m > 12 || dd < 1) return 0;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1);
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  days = (int64_t)era * 146097 + (int64_t)doe - 719468;
  return (uint64_t)(days * 86400 + (t >> 11) * 3600 + ((t >> 5) & 63) * 60 + (t & 31) * 2);
}

/* filho: pai + nome -> caminho do SODOS */
static int join(char* out, const char* parent, const char* name) {
  size_t pl = strlen(parent), nl = strlen(name);
  if (!strcmp(name, ".") ) { strcpy(out, parent); return 0; }
  if (!strcmp(name, "..")) {
    char* s;
    strcpy(out, parent);
    if (!out[0]) return 0;
    if (strlen(out) <= 3) { out[0] = 0; return 0; } /* "D:\\" -> raiz */
    s = strrchr(out, '\\');
    if (s && s - out == 2) s[1] = 0;
    else if (s) *s = 0;
    return 0;
  }
  if (strchr(name, '\\') || strchr(name, ':') || strchr(name, '/')) return -E_INVAL;
  if (!parent[0]) {
    /* raiz: o nome e a unidade */
    char c = name[0];
    if (nl != 1) return -E_NOENT;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c < 'A' || c > 'Z' || !(k_drive_mask() & (1u << (c - 'A')))) return -E_NOENT;
    out[0] = c;
    out[1] = ':';
    out[2] = '\\';
    out[3] = 0;
    return 0;
  }
  if (pl + nl + 2 >= P9_PATH) return -E_INVAL;
  strcpy(out, parent);
  if (out[pl - 1] != '\\') strcat(out, "\\");
  strcat(out, name);
  return 0;
}

/* ---------- estado de um caminho (pasta do pai, com cache curto) ---------- */
static struct {
  char dir[P9_PATH];
  struct k_direntl* e;
  int n, cap;
  uint32_t t;
} dc;

static void dc_drop(void) { dc.dir[0] = 0; }

static int list_dir(const char* dir, struct k_direntl** out, int* cap) {
  int n;
  if (!*out) {
    *cap = 256;
    *out = (struct k_direntl*)malloc(sizeof(struct k_direntl) * (size_t)*cap);
    if (!*out) return -1;
  }
  for (;;) {
    n = k_dir_long(dir, *out, *cap);
    if (n < *cap) return n;
    {
      struct k_direntl* b = (struct k_direntl*)realloc(*out, sizeof(struct k_direntl) * (size_t)*cap * 2);
      if (!b) return n;
      *out = b;
      *cap *= 2;
    }
  }
}

struct st {
  int dir;
  uint32_t size;
  uint64_t mtime;
};

static int pstat(const char* path, struct st* st) {
  char dir[P9_PATH];
  const char* leaf;
  char* s;
  int i;
  memset(st, 0, sizeof *st);
  if (!path[0] || strlen(path) <= 3) { /* raiz ou "D:\\" */
    st->dir = 1;
    return 0;
  }
  strcpy(dir, path);
  s = strrchr(dir, '\\');
  if (!s) return -E_NOENT;
  leaf = path + (s - dir) + 1;
  if (s - dir == 2) s[1] = 0;
  else *s = 0;
  if (strcmp(dc.dir, dir) || k_ms() - dc.t > 1000u) {
    int n = list_dir(dir, &dc.e, &dc.cap);
    if (n < 0) {
      dc.dir[0] = 0;
      return -E_NOENT;
    }
    dc.n = n;
    strcpy(dc.dir, dir);
    dc.t = k_ms();
  }
  for (i = 0; i < dc.n; i++) {
    const char* a = dc.e[i].name;
    const char* b = leaf;
    while (*a && *b && ((*a | 0x20) == (*b | 0x20) || *a == *b)) a++, b++;
    if (!*a && !*b) {
      st->dir = dc.e[i].dir;
      st->size = dc.e[i].size;
      st->mtime = fat_unix(dc.e[i].date, dc.e[i].time);
      return 0;
    }
  }
  return -E_NOENT;
}

/* ---------- fids ---------- */
static struct p9fid* fid_get(uint32_t f) {
  if (f >= P9_FIDS || !fids[f].used) return 0;
  return &fids[f];
}

static int fid_flush(struct p9fid* f) {
  int rc = 0;
  if (f->dirty) {
    if (k_fsave(f->path, (const char*)(f->data ? f->data : (uint8_t*)""), (int)f->size) < 0) rc = -E_IO;
    f->dirty = 0;
    dc_drop();
  }
  return rc;
}

static void fid_free(struct p9fid* f) {
  fid_flush(f);
  free(f->data);
  free(f->ents);
  memset(f, 0, sizeof *f);
}

/* arquivo inteiro em RAM (para escrever) */
static int fid_load(struct p9fid* f) {
  int n;
  if (f->loaded) return 0;
  n = k_fsize(f->path);
  if (n < 0) n = 0;
  f->cap = (uint32_t)n + 4096;
  f->data = (uint8_t*)malloc(f->cap);
  if (!f->data) return -E_NOMEM;
  if (n > 0 && k_fread(f->path, 0, (char*)f->data, n) != n) {
    free(f->data);
    f->data = 0;
    return -E_IO;
  }
  f->size = (uint32_t)n;
  f->loaded = 1;
  return 0;
}

static int fid_resize(struct p9fid* f, uint32_t sz) {
  if (sz > f->cap) {
    uint32_t c = f->cap ? f->cap : 4096;
    uint8_t* b;
    while (c < sz) c *= 2;
    b = (uint8_t*)realloc(f->data, c);
    if (!b) return -E_NOMEM;
    f->data = b;
    f->cap = c;
  }
  if (sz > f->size) memset(f->data + f->size, 0, sz - f->size);
  f->size = sz;
  f->dirty = 1;
  return 0;
}

/* ---------- mensagens ---------- */
static uint32_t rerr(uint16_t tag, int e) {
  p32(rs, 11);
  rs[4] = 7; /* Rlerror */
  p16(rs + 5, tag);
  p32(rs + 7, (uint32_t)e);
  return 11;
}

static uint32_t rhdr(uint8_t type, uint16_t tag, uint8_t* end) {
  uint32_t n = (uint32_t)(end - rs);
  p32(rs, n);
  rs[4] = type;
  p16(rs + 5, tag);
  return n;
}

static uint8_t* attr(uint8_t* p, const char* path, const struct st* st) {
  uint32_t mode = st->dir ? 0040755u : 0100644u;
  p64(p, 0x7FFull); /* basico */
  p = pqid(p + 8, st->dir, path);
  p32(p, mode);
  p32(p + 4, 0);
  p32(p + 8, 0);
  p64(p + 12, st->dir ? 2 : 1);
  p64(p + 20, 0);
  p64(p + 28, st->size);
  p64(p + 36, 4096);
  p64(p + 44, ((uint64_t)st->size + 511) / 512);
  p64(p + 52, st->mtime);
  p64(p + 60, 0);
  p64(p + 68, st->mtime);
  p64(p + 76, 0);
  p64(p + 84, st->mtime);
  p64(p + 92, 0);
  memset(p + 100, 0, 32); /* btime, gen, data_version */
  return p + 132;
}

static uint32_t handle(uint32_t len) {
  uint8_t type = rq[4];
  uint16_t tag = g16(rq + 5);
  const uint8_t* a = rq + 7;
  const uint8_t* end = rq + len;
  uint8_t* o = rs + 7;
  switch (type) {
  case 100: { /* Tversion */
    char ver[32];
    uint32_t m = g32(a);
    if (!gstr(a + 4, end, ver, sizeof ver)) return rerr(tag, E_INVAL);
    msize = m < P9_MSIZE ? m : P9_MSIZE;
    p32(o, msize);
    o = pstr(o + 4, strcmp(ver, "9P2000.L") ? "unknown" : "9P2000.L");
    { int i; for (i = 0; i < P9_FIDS; i++) if (fids[i].used) fid_free(&fids[i]); }
    return rhdr(101, tag, o);
  }
  case 104: { /* Tattach fid afid uname aname n_uname */
    uint32_t f = g32(a);
    if (f >= P9_FIDS) return rerr(tag, E_INVAL);
    if (fids[f].used) fid_free(&fids[f]);
    fids[f].used = 1;
    fids[f].path[0] = 0;
    o = pqid(o, 1, "");
    return rhdr(105, tag, o);
  }
  case 108: /* Tflush */
    return rhdr(109, tag, o);
  case 120: { /* Tclunk */
    struct p9fid* f = fid_get(g32(a));
    int rc;
    if (!f) return rerr(tag, E_BADF);
    rc = fid_flush(f);
    fid_free(f);
    if (rc) return rerr(tag, -rc);
    return rhdr(121, tag, o);
  }
  case 110: { /* Twalk fid newfid nwname wname* */
    struct p9fid* f = fid_get(g32(a));
    uint32_t nf = g32(a + 4);
    uint16_t nw = g16(a + 8), i;
    char cur[P9_PATH], name[256];
    const uint8_t* p = a + 10;
    uint8_t* cnt;
    if (!f || nf >= P9_FIDS || nw > 16) return rerr(tag, E_BADF);
    strcpy(cur, f->path);
    cnt = o;
    o += 2;
    for (i = 0; i < nw; i++) {
      char nx[P9_PATH];
      struct st st;
      int k = gstr(p, end, name, sizeof name), e;
      if (!k) return rerr(tag, E_INVAL);
      p += k;
      e = join(nx, cur, name);
      if (!e) e = pstat(nx, &st);
      if (e) {
        if (i == 0) return rerr(tag, -e);
        break;
      }
      o = pqid(o, st.dir, nx);
      strcpy(cur, nx);
    }
    p16(cnt, i);
    if (i == nw) {
      if (nf != g32(a) && fids[nf].used) fid_free(&fids[nf]);
      if (nf != g32(a)) memset(&fids[nf], 0, sizeof fids[nf]);
      fids[nf].used = 1;
      strcpy(fids[nf].path, cur);
    }
    return rhdr(111, tag, o);
  }
  case 24: { /* Tgetattr fid mask */
    struct p9fid* f = fid_get(g32(a));
    struct st st;
    int e;
    if (!f) return rerr(tag, E_BADF);
    e = pstat(f->path, &st);
    if (e) return rerr(tag, -e);
    if (f->loaded && !st.dir) st.size = f->size;
    o = attr(o, f->path, &st);
    return rhdr(25, tag, o);
  }
  case 26: { /* Tsetattr: so o tamanho */
    struct p9fid* f = fid_get(g32(a));
    uint32_t valid = g32(a + 4);
    if (!f) return rerr(tag, E_BADF);
    if (valid & 8) {
      uint64_t sz = g64(a + 20);
      int e;
      if (sz > 0x7FFFFFF0ull) return rerr(tag, E_INVAL);
      if ((e = fid_load(f)) || (e = fid_resize(f, (uint32_t)sz))) return rerr(tag, -e);
      if (!f->open && (e = fid_flush(f))) return rerr(tag, -e);
    }
    return rhdr(27, tag, o);
  }
  case 12: { /* Tlopen fid flags */
    struct p9fid* f = fid_get(g32(a));
    uint32_t fl = g32(a + 4);
    struct st st;
    int e;
    if (!f) return rerr(tag, E_BADF);
    e = pstat(f->path, &st);
    if (e) return rerr(tag, -e);
    f->open = 1;
    if (!st.dir && (fl & 01000)) { /* O_TRUNC */
      free(f->data);
      f->data = 0;
      f->size = f->cap = 0;
      f->loaded = 1;
      f->dirty = 1;
    }
    o = pqid(o, st.dir, f->path);
    p32(o, 0);
    return rhdr(13, tag, o + 4);
  }
  case 14: { /* Tlcreate fid name flags mode gid: fid vira o arquivo novo */
    struct p9fid* f = fid_get(g32(a));
    char name[256], np[P9_PATH];
    int e;
    if (!f || !gstr(a + 4, end, name, sizeof name)) return rerr(tag, E_BADF);
    if ((e = join(np, f->path, name))) return rerr(tag, -e);
    if (!f->path[0]) return rerr(tag, E_PERM);
    if (k_fsave(np, "", 0) < 0) return rerr(tag, E_INVAL);
    dc_drop();
    free(f->data);
    free(f->ents);
    memset(f, 0, sizeof *f);
    f->used = 1;
    f->open = 1;
    f->loaded = 1;
    strcpy(f->path, np);
    o = pqid(o, 0, np);
    p32(o, 0);
    return rhdr(15, tag, o + 4);
  }
  case 116: { /* Tread fid offset count */
    struct p9fid* f = fid_get(g32(a));
    uint64_t off = g64(a + 4);
    uint32_t cnt = g32(a + 12), n = 0;
    if (!f) return rerr(tag, E_BADF);
    if (cnt > msize - 11) cnt = msize - 11;
    if (f->loaded) {
      if (off < f->size) {
        n = f->size - (uint32_t)off;
        if (n > cnt) n = cnt;
        memcpy(o + 4, f->data + off, n);
      }
    } else if (off < 0xFFFFFFFFull) {
      int r = cnt ? k_fread(f->path, (unsigned)off, (char*)(o + 4), (int)cnt) : 0;
      if (r < 0) {
        int sz = k_fsize(f->path);
        if (sz >= 0 && off >= (uint64_t)sz) r = 0; /* fim do arquivo */
        else return rerr(tag, E_IO);
      }
      n = (uint32_t)r;
    }
    p32(o, n);
    return rhdr(117, tag, o + 4 + n);
  }
  case 118: { /* Twrite fid offset count data */
    struct p9fid* f = fid_get(g32(a));
    uint64_t off = g64(a + 4);
    uint32_t cnt = g32(a + 12);
    int e;
    if (!f) return rerr(tag, E_BADF);
    if (a + 16 + cnt > end || off + cnt > 0x7FFFFFF0ull) return rerr(tag, E_INVAL);
    if ((e = fid_load(f))) return rerr(tag, -e);
    if (off + cnt > f->size && (e = fid_resize(f, (uint32_t)(off + cnt)))) return rerr(tag, -e);
    memcpy(f->data + off, a + 16, cnt);
    f->dirty = 1;
    p32(o, cnt);
    return rhdr(119, tag, o + 4);
  }
  case 50: { /* Tfsync */
    struct p9fid* f = fid_get(g32(a));
    int e;
    if (!f) return rerr(tag, E_BADF);
    if ((e = fid_flush(f))) return rerr(tag, -e);
    return rhdr(51, tag, o);
  }
  case 40: { /* Treaddir fid offset count */
    struct p9fid* f = fid_get(g32(a));
    uint64_t off = g64(a + 4);
    uint32_t cnt = g32(a + 12), used = 0;
    int i, cap = 0;
    uint8_t* data = o + 4;
    if (!f) return rerr(tag, E_BADF);
    if (cnt > msize - 11) cnt = msize - 11;
    if (off == 0 || !f->ents) {
      free(f->ents);
      f->ents = 0;
      f->nents = 0;
      if (!f->path[0]) {
        /* raiz: as unidades montadas */
        unsigned m = k_drive_mask();
        f->ents = (struct k_direntl*)calloc(26, sizeof(struct k_direntl));
        for (i = 0; i < 26 && f->ents; i++)
          if (m & (1u << i)) {
            f->ents[f->nents].name[0] = (char)('a' + i);
            f->ents[f->nents].dir = 1;
            f->nents++;
          }
      } else {
        int n = list_dir(f->path, &f->ents, &cap);
        if (n < 0) return rerr(tag, E_NOTDIR);
        f->nents = n;
      }
    }
    /* 0 = ".", 1 = "..", 2.. = entradas */
    for (i = (int)off; i < f->nents + 2; i++) {
      const char* nm = i == 0 ? "." : i == 1 ? ".." : f->ents[i - 2].name;
      int dir = i < 2 ? 1 : f->ents[i - 2].dir;
      char cp[P9_PATH];
      uint32_t need = 13 + 8 + 1 + 2 + (uint32_t)strlen(nm);
      if (used + need > cnt) break;
      if (join(cp, f->path, nm)) strcpy(cp, f->path);
      pqid(data + used, dir, cp);
      p64(data + used + 13, (uint64_t)(i + 1));
      data[used + 21] = dir ? 4 : 8; /* DT_DIR / DT_REG */
      pstr(data + used + 22, nm);
      used += need;
    }
    p32(o, used);
    return rhdr(41, tag, o + 4 + used);
  }
  case 72: { /* Tmkdir dfid name mode gid */
    struct p9fid* f = fid_get(g32(a));
    char name[256], np[P9_PATH];
    int e, r;
    if (!f || !gstr(a + 4, end, name, sizeof name)) return rerr(tag, E_BADF);
    if ((e = join(np, f->path, name))) return rerr(tag, -e);
    r = k_mkdir(np);
    dc_drop();
    if (r == 1) return rerr(tag, E_EXIST);
    if (r < 0) return rerr(tag, E_INVAL);
    o = pqid(o, 1, np);
    return rhdr(73, tag, o);
  }
  case 76: { /* Tunlinkat dfid name flags */
    struct p9fid* f = fid_get(g32(a));
    char name[256], np[P9_PATH];
    int e, r, k;
    if (!f || !(k = gstr(a + 4, end, name, sizeof name))) return rerr(tag, E_BADF);
    if ((e = join(np, f->path, name))) return rerr(tag, -e);
    if (g32(a + 4 + k) & 0x200) { /* AT_REMOVEDIR */
      r = k_rmdir(np);
      dc_drop();
      if (r == -2) return rerr(tag, E_NOTEMPTY);
      if (r == -3) return rerr(tag, E_NOTDIR);
      if (r < 0) return rerr(tag, E_NOENT);
    } else {
      r = k_fdelete(np);
      dc_drop();
      if (r == -3) return rerr(tag, E_ISDIR);
      if (r == -4) return rerr(tag, E_ROFS);
      if (r < 0) return rerr(tag, E_NOENT);
    }
    return rhdr(77, tag, o);
  }
  case 74:   /* Trenameat olddir oldname newdir newname */
  case 20: { /* Trename fid dfid name */
    char from[P9_PATH], to[P9_PATH], n1[256], n2[256];
    int e, r;
    if (type == 74) {
      struct p9fid* d1 = fid_get(g32(a));
      int k = gstr(a + 4, end, n1, sizeof n1);
      struct p9fid* d2 = k ? fid_get(g32(a + 4 + k)) : 0;
      if (!d1 || !d2 || !gstr(a + 8 + k, end, n2, sizeof n2)) return rerr(tag, E_BADF);
      if ((e = join(from, d1->path, n1)) || (e = join(to, d2->path, n2))) return rerr(tag, -e);
    } else {
      struct p9fid* f = fid_get(g32(a));
      struct p9fid* d = fid_get(g32(a + 4));
      if (!f || !d || !gstr(a + 8, end, n2, sizeof n2)) return rerr(tag, E_BADF);
      fid_flush(f);
      strcpy(from, f->path);
      if ((e = join(to, d->path, n2))) return rerr(tag, -e);
    }
    {
      struct st st;
      if (!pstat(to, &st) && !st.dir) k_fdelete(to); /* POSIX: o destino some */
    }
    r = k_frename(from, to);
    if (r == -5 || r == -6) { /* sem rename nesta unidade: copia e apaga (arquivo) */
      int n = k_fsize(from);
      char* b = n >= 0 ? (char*)malloc((size_t)n + 1) : 0;
      r = -1;
      if (b && (n == 0 || k_fread(from, 0, b, n) == n) && k_fsave(to, b, n) >= 0) r = k_fdelete(from) < 0 ? -1 : 0;
      free(b);
    }
    dc_drop();
    if (r) return rerr(tag, r == -1 ? E_NOENT : E_INVAL);
    if (type == 20) strcpy(fid_get(g32(a))->path, to);
    return rhdr(type == 74 ? 75 : 21, tag, o);
  }
  case 122: { /* Tremove */
    struct p9fid* f = fid_get(g32(a));
    struct st st;
    int r;
    if (!f) return rerr(tag, E_BADF);
    r = pstat(f->path, &st) ? -1 : st.dir ? k_rmdir(f->path) : k_fdelete(f->path);
    f->dirty = 0;
    fid_free(f);
    dc_drop();
    if (r < 0) return rerr(tag, E_NOENT);
    return rhdr(123, tag, o);
  }
  case 8: { /* Tstatfs */
    struct p9fid* f = fid_get(g32(a));
    unsigned tot = 0, fr = 0;
    if (!f) return rerr(tag, E_BADF);
    if (f->path[0]) k_dfree(f->path[0], &tot, &fr);
    p32(o, 0x01021997u); /* V9FS_MAGIC */
    p32(o + 4, 4096);
    p64(o + 8, tot / 4);
    p64(o + 16, fr / 4);
    p64(o + 24, fr / 4);
    p64(o + 32, 1000000);
    p64(o + 40, 1000000);
    p64(o + 48, 0x50444F53ull);
    p32(o + 56, 255);
    return rhdr(9, tag, o + 60);
  }
  case 52: /* Tlock: aceita */
    o[0] = 0;
    return rhdr(53, tag, o + 1);
  case 54: { /* Tgetlock: ninguem trava */
    o[0] = 2; /* F_UNLCK */
    memcpy(o + 1, a + 5, 20);
    o = pstr(o + 21, "");
    return rhdr(55, tag, o);
  }
  case 30: /* Txattrwalk */
    return rerr(tag, E_OPNOTSUPP);
  case 22: /* Treadlink */
    return rerr(tag, E_INVAL);
  default: /* symlink, link, mknod, auth... */
    return rerr(tag, type == 16 || type == 18 || type == 70 ? E_PERM : E_NOSYS);
  }
}

int p9_init(void) {
  if (!fids) fids = (struct p9fid*)k_hmalloc(sizeof(struct p9fid) * P9_FIDS, 4096);
  if (!rq) rq = (uint8_t*)k_hmalloc(P9_MSIZE, 4096);
  if (!rs) rs = (uint8_t*)k_hmalloc(P9_MSIZE + 4096, 4096);
  if (!fids || !rq || !rs) return -1;
  memset(fids, 0, sizeof(struct p9fid) * P9_FIDS);
  return 0;
}

/* Um pedido (juntado dos segmentos de leitura); a resposta vai para rs.
 * Devolve o tamanho da resposta. */
uint32_t p9_request(const struct seg* sg, int n, uint8_t** resp) {
  uint32_t got = 0;
  int k;
  for (k = 0; k < n && !sg[k].w; k++) {
    uint32_t t = sg[k].len;
    if (got + t > P9_MSIZE) t = P9_MSIZE - got;
    memcpy(rq + got, sg[k].p, t);
    got += t;
  }
  *resp = rs;
  if (got < 7 || g32(rq) > got) return rerr(got >= 7 ? g16(rq + 5) : 0, E_INVAL);
  return handle(g32(rq));
}
