/*
 * fat32.c - FAT32.DLL: leitura de FAT32 (contrato em fs/fsdrv.h).
 *
 * O kernel so carrega este DLL quando acha uma unidade FAT32 (pendrive em U:,
 * mount de uma particao 0x0B/0x0C). Somente leitura: pastas, arquivos e nomes
 * longos (VFAT, CP850 como o fs/fat.c).
 *
 * Memoria por volume: um cluster (pastas e pontas de arquivo), uma janela de
 * 16 KB da FAT e a posicao do ultimo arquivo lido (k_fread em sequencia nao
 * anda a cadeia toda de novo). Trechos inteiros de clusters seguidos vao
 * direto para o buffer de quem pediu.
 *
 * O driver so cede a CPU (host->yield) entre trechos, sem ponteiro guardado
 * para os caches: outra chamada pode usar o volume nesse meio tempo.
 *
 * Roda no contexto do kernel: gcc -m64 -mno-sse -mno-red-zone, sem libc.
 */
#include <stdint.h>
#include "fsdrv.h"

#define NAME_MAX 256
#define PATH_MAX 260
#define FATC_SECS 32u                  /* janela da FAT: 16 KB, 4096 entradas */
#define RUN_MAX (1024u * 1024u)        /* leitura direta: ate 1 MB por vez */
#define CL_END 0x0FFFFFF8u

struct vol {
  const struct fsdrv_host* h;
  uint32_t disk;
  uint32_t spc;        /* setores por cluster */
  uint32_t csize;      /* bytes por cluster */
  uint32_t fat_lba;    /* FAT ativa (LBA absoluto) */
  uint32_t fatsz;      /* setores de uma FAT */
  uint32_t data_lba;
  uint32_t nclus;      /* clusters de dados: 2 .. nclus+1 */
  uint32_t root;
  uint32_t free_clus;  /* 0xFFFFFFFF = ainda nao contado */
  uint8_t* cbuf;
  uint32_t cbuf_clus;  /* 0 = vazio */
  uint32_t* fatc;
  uint32_t fatc_first; /* primeiro setor (relativo a FAT) na janela */
  uint32_t fatc_n;     /* 0 = vazio */
  /* ultimo arquivo lido */
  char lpath[PATH_MAX];
  uint32_t lfirst, lsize, lidx, lclus;
  int lvalid;
};

/* Uma entrada achada numa pasta. */
struct ent {
  char lname[NAME_MAX];   /* nome longo, ou o 8.3 sem LFN */
  char sname[13];         /* 8.3 */
  uint8_t attr;
  uint32_t clus;
  uint32_t size;
  uint16_t date, time;
};

/* ---------- utilidades ---------- */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void mcopy(void* d, const void* s, uint32_t n) {
  uint8_t* a = (uint8_t*)d;
  const uint8_t* b = (const uint8_t*)s;
  while (n--) *a++ = *b++;
}

static void scopy(char* d, const char* s, int max) {
  int i = 0;
  if (max < 1) return;
  while (s[i] && i < max - 1) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

static int slen(const char* s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static int seq(const char* a, const char* b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* CP850 <-> Unicode, igual ao fs/fat.c. */
static const uint16_t cp850_hi[128] = {
  0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
  0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
  0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
  0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192,
  0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
  0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
  0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1, 0x00C2, 0x00C0,
  0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5, 0x2510,
  0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3,
  0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4,
  0x00F0, 0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE,
  0x00CF, 0x2518, 0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580,
  0x00D3, 0x00DF, 0x00D4, 0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE,
  0x00DE, 0x00DA, 0x00DB, 0x00D9, 0x00FD, 0x00DD, 0x00AF, 0x00B4,
  0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x00B8,
  0x00B0, 0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0,
};

static uint16_t oem_to_uni(unsigned char c) { return c < 0x80 ? c : cp850_hi[c - 0x80]; }

static char uni_to_oem(uint16_t u) {
  int i;
  if (u < 0x80) return (char)u;
  for (i = 0; i < 128; i++)
    if (cp850_hi[i] == u) return (char)(0x80 + i);
  return '_';
}

static uint16_t uni_fold(uint16_t u) {
  if (u >= 'a' && u <= 'z') return (uint16_t)(u - 32);
  if (u >= 0xE0 && u <= 0xFE && u != 0xF7) return (uint16_t)(u - 32);
  if (u == 0xFF) return 0x178;
  return u;
}

static int name_eq_fold(const char* a, const char* b) {
  while (*a && *b) {
    if (uni_fold(oem_to_uni((unsigned char)*a)) != uni_fold(oem_to_uni((unsigned char)*b)))
      return 0;
    a++;
    b++;
  }
  return *a == *b;
}

/* 8.3 do disco para texto, com as minusculas do NT (byte 12). */
static void short_name(const uint8_t* e, char* out) {
  int i = 0, j;
  uint8_t nt = e[12];
  for (j = 0; j < 8; j++) {
    unsigned char c = e[j];
    if (c == ' ') continue;
    if (j == 0 && c == 0x05) c = 0xE5;
    if ((nt & 0x08) && c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
    out[i++] = (char)c;
  }
  if (e[8] != ' ') {
    out[i++] = '.';
    for (j = 8; j < 11; j++) {
      unsigned char c = e[j];
      if (c == ' ') continue;
      if ((nt & 0x10) && c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
      out[i++] = (char)c;
    }
  }
  out[i] = 0;
}

static uint8_t sum11(const uint8_t* p) {
  uint8_t s = 0;
  int i;
  for (i = 0; i < 11; i++) s = (uint8_t)(((s & 1u) << 7) + (s >> 1) + p[i]);
  return s;
}

/* Posicao do k-esimo caractere UCS-2 numa entrada LFN. */
static const uint8_t lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/* ---------- disco ---------- */
static int valid_clus(const struct vol* v, uint32_t c) { return c >= 2 && c < v->nclus + 2; }

static uint32_t clus_lba(const struct vol* v, uint32_t c) {
  return v->data_lba + (c - 2) * v->spc;
}

/* Proximo cluster da cadeia; 0 se acabou ou e invalido. */
static uint32_t fat_next(struct vol* v, uint32_t c) {
  uint32_t sec = c / 128u;   /* 128 entradas de 4 bytes por setor */
  uint32_t n;
  if (!valid_clus(v, c) || sec >= v->fatsz) return 0;
  if (!v->fatc_n || sec < v->fatc_first || sec >= v->fatc_first + v->fatc_n) {
    uint32_t first = sec - sec % FATC_SECS;
    n = v->fatsz - first;
    if (n > FATC_SECS) n = FATC_SECS;
    v->fatc_n = 0;
    if (v->h->read(v->disk, v->fat_lba + first, n, v->fatc) != 0) return 0;
    v->fatc_first = first;
    v->fatc_n = n;
  }
  n = v->fatc[c - v->fatc_first * 128u] & 0x0FFFFFFFu;
  if (n >= CL_END || !valid_clus(v, n)) return 0;
  return n;
}

/* Cluster inteiro em cbuf. 0 ok. */
static int load_clus(struct vol* v, uint32_t c) {
  if (v->cbuf_clus == c) return 0;
  v->cbuf_clus = 0;
  if (!valid_clus(v, c)) return -1;
  if (v->h->read(v->disk, clus_lba(v, c), v->spc, v->cbuf) != 0) return -1;
  v->cbuf_clus = c;
  return 0;
}

/* ---------- pastas ---------- */
/* Chama fn para cada arquivo/pasta (sem . e .., sem rotulo). fn devolvendo
 * nao zero para; scan devolve esse valor (0 no fim, -1 erro de disco). */
typedef int (*scan_fn)(void* ctx, const struct ent* e);

static int dir_scan(struct vol* v, uint32_t clus, scan_fn fn, void* ctx, struct ent* e) {
  int need = 0, done = 0, len = 0;
  uint8_t sum = 0;
  uint32_t guard = 0;
  while (valid_clus(v, clus)) {
    uint32_t off;
    if (++guard > v->nclus) return -1;   /* cadeia em laco */
    if (load_clus(v, clus) != 0) return -1;
    for (off = 0; off < v->csize; off += 32) {
      const uint8_t* d = v->cbuf + off;
      uint8_t attr = d[11];
      int r;
      if (d[0] == 0x00) return 0;
      if (d[0] == 0xE5) {
        need = done = 0;
        continue;
      }
      if ((attr & 0x3F) == 0x0F) {
        int ord = d[0] & 0x1F, k, base;
        if (d[0] & 0x40) {
          if (ord < 1 || ord > 20) {
            need = done = 0;
            continue;
          }
          need = ord;
          sum = d[13];
          len = ord * 13;
          if (len > NAME_MAX - 1) len = NAME_MAX - 1;
        } else if (!need || ord != need || d[13] != sum) {
          need = done = 0;
          continue;
        }
        base = (ord - 1) * 13;
        for (k = 0; k < 13; k++) {
          uint16_t u = rd16(d + lfn_off[k]);
          if (u == 0) {
            if (base + k < len) len = base + k;
            break;
          }
          if (base + k < NAME_MAX - 1) e->lname[base + k] = uni_to_oem(u);
        }
        need--;
        done = need == 0;
        continue;
      }
      if (attr & 0x08) {   /* rotulo do volume */
        need = done = 0;
        continue;
      }
      short_name(d, e->sname);
      if (done && len > 0 && sum11(d) == sum) e->lname[len] = 0;
      else scopy(e->lname, e->sname, NAME_MAX);
      need = done = 0;
      if (e->sname[0] == '.' && (!e->sname[1] || (e->sname[1] == '.' && !e->sname[2])))
        continue;
      e->attr = attr;
      e->clus = (uint32_t)rd16(d + 20) << 16 | rd16(d + 26);
      e->size = rd32(d + 28);
      e->time = rd16(d + 22);
      e->date = rd16(d + 24);
      r = fn(ctx, e);
      if (r) return r;
    }
    clus = fat_next(v, clus);
  }
  return 0;
}

struct find_ctx {
  const char* want;
};

static int find_cb(void* ctx, const struct ent* e) {
  const char* w = ((struct find_ctx*)ctx)->want;
  return name_eq_fold(e->lname, w) || name_eq_fold(e->sname, w);
}

/* Acha path. 0 ok (e preenchido; raiz = pasta com clus = root), -1 nao. */
static int lookup(struct vol* v, const char* path, struct ent* e) {
  char comp[NAME_MAX];
  struct find_ctx fc;
  uint32_t dir = v->root;
  int pos = 0;
  e->attr = 0x10;
  e->clus = v->root;
  e->size = 0;
  e->date = e->time = 0;
  e->lname[0] = e->sname[0] = 0;
  for (;;) {
    int n = 0;
    while (path[pos] == '\\' || path[pos] == '/') pos++;
    if (!path[pos]) return 0;
    if (!(e->attr & 0x10)) return -1;   /* arquivo no meio do caminho */
    while (path[pos] && path[pos] != '\\' && path[pos] != '/') {
      if (n < NAME_MAX - 1) comp[n++] = path[pos];
      pos++;
    }
    comp[n] = 0;
    fc.want = comp;
    if (dir_scan(v, dir, find_cb, &fc, e) != 1) return -1;
    dir = e->clus;
    if ((e->attr & 0x10) && !dir) dir = v->root;   /* ".." da raiz */
    if (e->attr & 0x10) e->clus = dir;
  }
}

/* ---------- operacoes ---------- */
static int f_probe(const uint8_t* b) {
  if (b[0] != 0xEB && b[0] != 0xE9) return 0;
  if (rd16(b + 11) != 512 || b[13] == 0 || (b[13] & (b[13] - 1)) || b[16] == 0) return 0;
  if (rd16(b + 14) == 0 || rd16(b + 22) != 0 || rd32(b + 36) == 0) return 0;
  return rd32(b + 44) >= 2 && (rd16(b + 19) || rd32(b + 32));
}

static void f_umount(void* p) {
  struct vol* v = (struct vol*)p;
  const struct fsdrv_host* h;
  if (!v) return;
  h = v->h;
  h->free(v->cbuf);
  h->free(v->fatc);
  h->free(v);
}

static void* f_mount(const struct fsdrv_host* h, uint32_t disk, uint32_t lba) {
  uint8_t* b;
  struct vol* v;
  uint32_t res, nfats, fatsz, total, active, fsinfo, i;
  if (!h || h->abi != FSDRV_ABI) return 0;
  b = (uint8_t*)h->alloc(512);
  v = (struct vol*)h->alloc(sizeof(struct vol));
  if (!b || !v) goto fail;
  for (i = 0; i < sizeof(struct vol); i++) ((uint8_t*)v)[i] = 0;
  v->h = h;
  if (h->read(disk, lba, 1, b) != 0 || !f_probe(b)) goto fail;
  res = rd16(b + 14);
  nfats = b[16];
  fatsz = rd32(b + 36);
  total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
  if (total <= res + nfats * fatsz) goto fail;
  v->disk = disk;
  v->spc = b[13];
  v->csize = v->spc * 512u;
  v->fatsz = fatsz;
  /* ExtFlags bit 7: so uma FAT ativa (bits 0-3); senao todas iguais, usa a 0 */
  active = (rd16(b + 40) & 0x80) ? (rd16(b + 40) & 0x0Fu) : 0;
  if (active >= nfats) active = 0;
  v->fat_lba = lba + res + active * fatsz;
  v->data_lba = lba + res + nfats * fatsz;
  v->nclus = (total - res - nfats * fatsz) / v->spc;
  /* a FAT tem que caber as entradas */
  if ((uint64_t)v->nclus + 2 > (uint64_t)fatsz * 128u) v->nclus = fatsz * 128u - 2;
  v->root = rd32(b + 44);
  if (!valid_clus(v, v->root)) goto fail;
  v->free_clus = 0xFFFFFFFFu;
  fsinfo = rd16(b + 48);
  if (fsinfo && fsinfo < res && h->read(disk, lba + fsinfo, 1, b) == 0 &&
      rd32(b) == 0x41615252u && rd32(b + 484) == 0x61417272u && rd32(b + 488) <= v->nclus)
    v->free_clus = rd32(b + 488);
  v->cbuf = (uint8_t*)h->alloc(v->csize);
  v->fatc = (uint32_t*)h->alloc(FATC_SECS * 512u);
  if (!v->cbuf || !v->fatc) goto fail;
  h->free(b);
  h->log("[fat32] volume montado\n");
  return v;
fail:
  if (b) h->free(b);
  if (v) {
    if (v->cbuf) h->free(v->cbuf);
    if (v->fatc) h->free(v->fatc);
    h->free(v);
  }
  return 0;
}

static int f_stat(void* p, const char* path, uint32_t* size) {
  struct vol* v = (struct vol*)p;
  struct ent e;
  if (!v || lookup(v, path ? path : "", &e) != 0) return -1;
  if (e.attr & 0x10) return 1;
  if (size) *size = e.size;
  return 0;
}

static int f_read(void* p, const char* path, uint32_t off, void* buf, uint32_t len) {
  struct vol* v = (struct vol*)p;
  uint8_t* dst = (uint8_t*)buf;
  uint32_t first, size, idx, clus, done = 0, steps = 0;
  if (!v || !path || !buf) return -1;
  if (v->lvalid && seq(v->lpath, path)) {
    first = v->lfirst;
    size = v->lsize;
  } else {
    struct ent e;
    if (lookup(v, path, &e) != 0 || (e.attr & 0x10)) return -1;
    first = e.clus;
    size = e.size;
    v->lvalid = 0;
  }
  if (off >= size || len == 0) return 0;
  if (len > size - off) len = size - off;
  /* cluster que contem off: da posicao guardada se der, senao do comeco */
  idx = off / v->csize;
  if (v->lvalid && seq(v->lpath, path) && v->lidx <= idx && valid_clus(v, v->lclus)) {
    clus = v->lclus;
    steps = v->lidx;
  } else {
    clus = first;
  }
  while (steps < idx && clus) {
    clus = fat_next(v, clus);
    steps++;
  }
  if (!valid_clus(v, clus)) return -1;
  while (done < len) {
    uint32_t co = off % v->csize;
    uint32_t left = len - done;
    if (co == 0 && left >= v->csize && (uintptr_t)dst >= 0x100000u) {
      /* clusters inteiros e seguidos: direto para o buffer */
      uint32_t n = 1, c = clus, nx;
      while ((n + 1) * v->csize <= left && (n + 1) * v->csize <= RUN_MAX &&
             (nx = fat_next(v, c)) == c + 1) {
        c = nx;
        n++;
      }
      if (v->h->read(v->disk, clus_lba(v, clus), n * v->spc, dst + done) != 0) return done ? (int)done : -1;
      done += n * v->csize;
      off += n * v->csize;
      idx += n;
      clus = fat_next(v, c);
    } else {
      uint32_t k = v->csize - co;
      if (k > left) k = left;
      if (load_clus(v, clus) != 0) return done ? (int)done : -1;
      mcopy(dst + done, v->cbuf + co, k);
      done += k;
      off += k;
      if (co + k == v->csize) {
        clus = fat_next(v, clus);
        idx++;
      }
    }
    if (done < len && !clus) break;   /* cadeia menor que o tamanho */
    v->h->yield();
  }
  /* posicao para a proxima leitura (k_fread em sequencia) */
  scopy(v->lpath, path, PATH_MAX);
  v->lfirst = first;
  v->lsize = size;
  v->lidx = idx;
  v->lclus = clus;
  v->lvalid = clus != 0;
  if (!v->lvalid) {
    v->lidx = 0;
    v->lclus = first;
    v->lvalid = valid_clus(v, first);
  }
  return (int)done;
}

struct items_ctx {
  struct fsdrv_item* out;
  int n, max;
};

static int items_cb(void* ctx, const struct ent* e) {
  struct items_ctx* c = (struct items_ctx*)ctx;
  struct fsdrv_item* it;
  if (c->n >= c->max) return 1;
  it = &c->out[c->n];
  /* nome longo se couber (k_dirent tem 32 bytes); senao o 8.3, que abre igual */
  scopy(it->name, slen(e->lname) > 31 ? e->sname : e->lname, (int)sizeof(it->name));
  if (!it->name[0]) return 0;
  it->dir = (e->attr & 0x10) ? 1 : 0;
  it->size = it->dir ? 0 : e->size;
  it->date = e->date;
  it->time = e->time;
  c->n++;
  return 0;
}

static int f_items(void* p, const char* path, struct fsdrv_item* out, int max) {
  struct vol* v = (struct vol*)p;
  struct ent e;
  struct items_ctx c;
  if (!v || !out || max <= 0) return -1;
  if (lookup(v, path ? path : "", &e) != 0 || !(e.attr & 0x10)) return -1;
  c.out = out;
  c.n = 0;
  c.max = max;
  if (dir_scan(v, e.clus, items_cb, &c, &e) < 0 && c.n == 0) return -1;
  return c.n;
}

static int f_long_name(void* p, const char* path, char* out, int max) {
  struct vol* v = (struct vol*)p;
  struct ent e;
  if (!v || !out || max < 2 || !path || !path[0]) return -1;
  if (lookup(v, path, &e) != 0) return -1;
  scopy(out, e.lname, max);
  return 0;
}

static int f_space(void* p, uint32_t* total_kb, uint32_t* free_kb) {
  struct vol* v = (struct vol*)p;
  if (!v) return -1;
  if (v->free_clus == 0xFFFFFFFFu) {
    /* sem FSInfo: conta a FAT uma vez (somente leitura, nao muda) */
    uint32_t c, free_n = 0;
    for (c = 2; c < v->nclus + 2; c++) {
      uint32_t sec = c / 128u;
      if (!v->fatc_n || sec < v->fatc_first || sec >= v->fatc_first + v->fatc_n) {
        uint32_t first = sec - sec % FATC_SECS, n = v->fatsz - first;
        if (n > FATC_SECS) n = FATC_SECS;
        v->fatc_n = 0;
        if (v->h->read(v->disk, v->fat_lba + first, n, v->fatc) != 0) return -1;
        v->fatc_first = first;
        v->fatc_n = n;
        v->h->yield();
        /* outra chamada pode ter trocado a janela no yield: le de novo */
        if (sec < v->fatc_first || sec >= v->fatc_first + v->fatc_n) {
          c--;
          continue;
        }
      }
      if ((v->fatc[c - v->fatc_first * 128u] & 0x0FFFFFFFu) == 0) free_n++;
    }
    v->free_clus = free_n;
  }
  if (total_kb) *total_kb = (uint32_t)((uint64_t)v->nclus * v->csize / 1024u);
  if (free_kb) *free_kb = (uint32_t)((uint64_t)v->free_clus * v->csize / 1024u);
  return 0;
}

static const struct fsdrv_ops ops = {
  FSDRV_ABI, "FAT32", f_probe, f_mount, f_umount, f_stat, f_read, f_items, f_long_name, f_space,
};

const struct fsdrv_ops* fs_driver(void) { return &ops; }
