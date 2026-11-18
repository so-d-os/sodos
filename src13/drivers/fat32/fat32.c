/*
 * fat32.c - FAT32.DLL: leitura de FAT32 (contrato em fs/fsdrv.h).
 *
 * O kernel so carrega este DLL quando acha uma unidade FAT32 (pendrive em U:,
 * mount de uma particao 0x0B/0x0C). Le pastas, arquivos e nomes longos (VFAT,
 * CP850 como o fs/fat.c). Escrita: mkdir, arquivo inteiro (write) e apagar
 * arquivo (FAT nas copias espelhadas, FSInfo, nome longo + apelido 8.3 com ~N).
 * As mudancas na FAT ficam na janela fatc (marcada suja) e vao ao disco num
 * so lote no fim de cada operacao (ou antes da janela trocar).
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

/* Nome mostrado em mount/df; uma variante de teste usa -DFAT32_NAME=... */
#ifndef FAT32_NAME
#define FAT32_NAME "FAT32"
#endif

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
  uint32_t fat0_lba;   /* primeira FAT */
  uint32_t nfats;
  int mirror;          /* 1: grava em todas as FATs; 0: so na ativa */
  uint32_t fsinfo_lba; /* 0 = sem FSInfo */
  uint32_t next_free;  /* dica para alocar */
  uint8_t* cbuf;
  uint32_t cbuf_clus;  /* 0 = vazio */
  uint32_t* fatc;
  uint32_t fatc_first; /* primeiro setor (relativo a FAT) na janela */
  uint32_t fatc_n;     /* 0 = vazio */
  uint32_t fatc_d0, fatc_d1; /* setores sujos da janela [d0, d1); d1 = 0: limpa */
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
  /* posicao na pasta: o 8.3 e o primeiro slot (LFN, se houver); nslots no total */
  uint32_t pos_clus, pos_off, first_clus, first_off, nslots;
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

/* Setores sujos da janela vao para a FAT ativa (e as copias, se espelhadas).
 * 0 ok. */
static int fat_flush(struct vol* v) {
  uint32_t i, d0 = v->fatc_d0, d1 = v->fatc_d1;
  int rc = 0;
  if (!d1 || !v->fatc_n) {
    v->fatc_d1 = 0;
    return 0;
  }
  for (i = 0; i < v->nfats; i++) {
    uint32_t base = v->fat0_lba + i * v->fatsz;
    if (!v->mirror && base != v->fat_lba) continue;
    if (v->h->write(v->disk, base + d0, d1 - d0, v->fatc + (d0 - v->fatc_first) * 128u) != 0)
      rc = -1;
  }
  v->fatc_d1 = 0;
  return rc;
}

/* Janela da FAT com o setor sec (relativo a FAT). 0 ja estava, 1 leu, -1 erro. */
static int fat_win(struct vol* v, uint32_t sec) {
  uint32_t first, n;
  if (sec >= v->fatsz) return -1;
  if (v->fatc_n && sec >= v->fatc_first && sec < v->fatc_first + v->fatc_n) return 0;
  if (fat_flush(v) != 0) return -1;
  first = sec - sec % FATC_SECS;
  n = v->fatsz - first;
  if (n > FATC_SECS) n = FATC_SECS;
  v->fatc_n = 0;
  if (v->h->read(v->disk, v->fat_lba + first, n, v->fatc) != 0) return -1;
  v->fatc_first = first;
  v->fatc_n = n;
  return 1;
}

/* Proximo cluster da cadeia; 0 se acabou ou e invalido. */
static uint32_t fat_next(struct vol* v, uint32_t c) {
  uint32_t n;
  if (!valid_clus(v, c) || fat_win(v, c / 128u) < 0) return 0;   /* 128 entradas por setor */
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
  int need = 0, done = 0, len = 0, total = 0;
  uint8_t sum = 0;
  uint32_t guard = 0, lclus = 0, loff = 0;
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
          total = ord;
          lclus = clus;
          loff = off;
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
      e->pos_clus = e->first_clus = clus;
      e->pos_off = e->first_off = off;
      e->nslots = 1;
      if (done && len > 0 && sum11(d) == sum) {
        e->lname[len] = 0;
        e->first_clus = lclus;
        e->first_off = loff;
        e->nslots = (uint32_t)total + 1;
      } else {
        scopy(e->lname, e->sname, NAME_MAX);
      }
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
  v->fat0_lba = lba + res;
  v->nfats = nfats;
  v->mirror = (rd16(b + 40) & 0x80) ? 0 : 1;
  v->data_lba = lba + res + nfats * fatsz;
  v->nclus = (total - res - nfats * fatsz) / v->spc;
  /* a FAT tem que caber as entradas */
  if ((uint64_t)v->nclus + 2 > (uint64_t)fatsz * 128u) v->nclus = fatsz * 128u - 2;
  v->root = rd32(b + 44);
  if (!valid_clus(v, v->root)) goto fail;
  v->free_clus = 0xFFFFFFFFu;
  fsinfo = rd16(b + 48);
  if (fsinfo && fsinfo < res && h->read(disk, lba + fsinfo, 1, b) == 0 &&
      rd32(b) == 0x41615252u && rd32(b + 484) == 0x61417272u) {
    v->fsinfo_lba = lba + fsinfo;
    if (rd32(b + 488) <= v->nclus) v->free_clus = rd32(b + 488);
    v->next_free = rd32(b + 492);
  }
  if (!valid_clus(v, v->next_free)) v->next_free = 2;
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
      int w = fat_win(v, sec);
      if (w < 0) return -1;
      if (w > 0) {
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


/* ---------- escrita (mkdir) ---------- */
static void wr16(uint8_t* p, uint16_t x) {
  p[0] = (uint8_t)x;
  p[1] = (uint8_t)(x >> 8);
}
static void wr32(uint8_t* p, uint32_t x) {
  wr16(p, (uint16_t)x);
  wr16(p + 2, (uint16_t)(x >> 16));
}
static void mzero(void* d, uint32_t n) {
  uint8_t* a = (uint8_t*)d;
  while (n--) *a++ = 0;
}

/* Entrada crua da FAT ativa (pela janela fatc). 0 ok. */
static int fat_get(struct vol* v, uint32_t c, uint32_t* out) {
  if (!valid_clus(v, c) || fat_win(v, c / 128u) < 0) return -1;
  *out = v->fatc[c - v->fatc_first * 128u] & 0x0FFFFFFFu;
  return 0;
}

/* val (28 bits, preserva os 4 de cima) na janela; vai ao disco no fat_flush.
 * sbuf nao e mais usado (fica pela assinatura antiga). 0 ok. */
static int fat_set(struct vol* v, uint32_t c, uint32_t val, uint8_t* sbuf) {
  uint32_t sec = c / 128u;
  uint32_t* e;
  (void)sbuf;
  if (!valid_clus(v, c) || fat_win(v, sec) < 0) return -1;
  e = &v->fatc[c - v->fatc_first * 128u];
  *e = (*e & 0xF0000000u) | (val & 0x0FFFFFFFu);
  if (!v->fatc_d1) {
    v->fatc_d0 = sec;
    v->fatc_d1 = sec + 1;
  } else {
    if (sec < v->fatc_d0) v->fatc_d0 = sec;
    if (sec + 1 > v->fatc_d1) v->fatc_d1 = sec + 1;
  }
  return 0;
}

/* FSInfo com o contador e a dica atuais (falha aqui nao desfaz nada). */
static void fsinfo_save(struct vol* v, uint8_t* sbuf) {
  fat_flush(v);
  if (!v->fsinfo_lba || v->h->read(v->disk, v->fsinfo_lba, 1, sbuf) != 0) return;
  if (rd32(sbuf) != 0x41615252u || rd32(sbuf + 484) != 0x61417272u) return;
  wr32(sbuf + 488, v->free_clus);
  wr32(sbuf + 492, v->next_free);
  v->h->write(v->disk, v->fsinfo_lba, 1, sbuf);
}

/* Cluster novo, zerado no disco e marcado como fim de cadeia; prev != 0 e
 * ligado a ele. 0 se nao ha espaco ou erro (*err = -2 cheio, -3 disco). */
static uint32_t clus_alloc(struct vol* v, uint32_t prev, uint8_t* sbuf, int* err) {
  uint32_t c = v->next_free, n, val;
  if (!valid_clus(v, c)) c = 2;
  for (n = 0; n < v->nclus; n++) {
    if (fat_get(v, c, &val) != 0) {
      *err = -3;
      return 0;
    }
    if (val == 0) break;
    if (++c >= v->nclus + 2) c = 2;
  }
  if (n == v->nclus) {
    *err = -2;
    return 0;
  }
  /* zera antes de ligar: uma pasta nunca aponta para lixo */
  mzero(v->cbuf, v->csize);
  v->cbuf_clus = 0;
  if (v->h->write(v->disk, clus_lba(v, c), v->spc, v->cbuf) != 0) {
    *err = -3;
    return 0;
  }
  v->cbuf_clus = c;
  if (fat_set(v, c, 0x0FFFFFFFu, sbuf) != 0 || (prev && fat_set(v, prev, c, sbuf) != 0)) {
    *err = -3;
    return 0;
  }
  if (v->free_clus != 0xFFFFFFFFu && v->free_clus) v->free_clus--;
  v->next_free = c + 1 < v->nclus + 2 ? c + 1 : 2;
  return c;
}

/* Grava 32 bytes na posicao (clus, off) de uma pasta e anda para a proxima. */
static int slot_put(struct vol* v, uint32_t* clus, uint32_t* off, const uint8_t* e) {
  if (load_clus(v, *clus) != 0) return -1;
  mcopy(v->cbuf + *off, e, 32);
  if (v->h->write(v->disk, clus_lba(v, *clus) + *off / 512u, 1, v->cbuf + (*off & ~511u)) != 0) {
    v->cbuf_clus = 0;
    return -1;
  }
  *off += 32;
  if (*off >= v->csize) {
    *off = 0;
    *clus = fat_next(v, *clus);
  }
  return 0;
}

/* Caractere valido num nome 8.3 (ja em maiuscula). */
static int sfn_ok(unsigned char c) {
  if (c >= 0x80) return 1;
  if (c >= 'A' && c <= 'Z') return 1;
  if (c >= '0' && c <= '9') return 1;
  return c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' || c == '@' || c == '~' ||
         c == '`' || c == '!' || c == '(' || c == ')' || c == '{' || c == '}' || c == '^' ||
         c == '#' || c == '&';
}

static unsigned char oem_upper(unsigned char c) {
  return (unsigned char)uni_to_oem(uni_fold(oem_to_uni(c)));
}

/* Nome aceito para criar: sem \ / : * ? " < > | nem controle, nao so pontos. */
static int lfn_ok(const char* n) {
  int i, dots = 1;
  for (i = 0; n[i]; i++) {
    unsigned char c = (unsigned char)n[i];
    if (c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|')
      return 0;
    if (c != '.') dots = 0;
  }
  return i > 0 && i < NAME_MAX && !dots && n[i - 1] != ' ' && n[i - 1] != '.';
}

/* Monta o 8.3 de name em n11. Devolve 0 se o nome e um 8.3 exato (com nt =
 * bits de minuscula do NT, sem LFN), 1 se precisa de LFN e ~N (base/ext
 * prontas em n11, sem o ~N), 2 se precisa de LFN mas n11 serve sem ~N
 * (caixa misturada). */
static int sfn_make(const char* name, uint8_t* n11, uint8_t* nt) {
  int i, dot = -1, bl = 0, el = 0, lossy = 0, lo[2] = { 0, 0 }, up[2] = { 0, 0 };
  int len = slen(name);
  for (i = 0; i < 11; i++) n11[i] = ' ';
  *nt = 0;
  for (i = len - 1; i > 0; i--)
    if (name[i] == '.') {
      dot = i;
      break;
    }
  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)name[i], u;
    int part = dot >= 0 && i > dot;
    if (i == dot) continue;
    if (c == ' ' || (c == '.' && !part && i != dot)) {
      lossy = 1;   /* espacos e pontos extras somem */
      continue;
    }
    u = oem_upper(c);
    if (u != c) lo[part] = 1;
    else if (c >= 'A' && c <= 'Z') up[part] = 1;
    if (!sfn_ok(u)) {
      u = '_';
      lossy = 1;
    }
    if (!part) {
      if (bl < 8) n11[bl] = u;
      else lossy = 1;
      bl++;
    } else {
      if (el < 3) n11[8 + el] = u;
      else lossy = 1;
      el++;
    }
  }
  if (bl == 0) {
    n11[0] = '_';
    lossy = 1;
  }
  if (n11[0] == 0xE5) n11[0] = 0x05;
  if (lossy) return 1;
  if ((lo[0] && up[0]) || (lo[1] && up[1])) return 2;
  if (lo[0]) *nt |= 0x08;
  if (lo[1]) *nt |= 0x10;
  return 0;
}

struct sfn_ctx {
  char want[13];
};

static int sfn_cb(void* ctx, const struct ent* e) {
  return name_eq_fold(e->sname, ((struct sfn_ctx*)ctx)->want);
}

/* 1 se ja existe na pasta um 8.3 igual a n11; -1 erro. */
static int sfn_used(struct vol* v, uint32_t dir, const uint8_t* n11, struct ent* tmp) {
  struct sfn_ctx c;
  uint8_t e[32];
  mzero(e, 32);
  mcopy(e, n11, 11);
  short_name(e, c.want);
  return dir_scan(v, dir, sfn_cb, &c, tmp);
}

/* Poe ~N na base do 8.3 (base ate 8-len("~N")). */
static void sfn_tail(uint8_t* n11, const uint8_t* base, uint32_t num) {
  char t[8];
  int tl = 0, bl = 0, i;
  uint32_t x = num;
  char d[8];
  int dn = 0;
  while (x) {
    d[dn++] = (char)('0' + x % 10u);
    x /= 10u;
  }
  t[tl++] = '~';
  while (dn) t[tl++] = d[--dn];
  while (bl < 8 && base[bl] != ' ') bl++;
  if (bl > 8 - tl) bl = 8 - tl;
  for (i = 0; i < 8; i++) n11[i] = ' ';
  for (i = 0; i < bl; i++) n11[i] = base[i];
  for (i = 0; i < tl; i++) n11[bl + i] = (uint8_t)t[i];
}

/* Separa path em pai (*parent: "" na raiz, ou par cortado na barra) e ultimo
 * nome (*leaf, dentro de par). 0 ok; -1 caminho invalido. */
static int split_path(const char* path, char* par, const char** parent, const char** leaf) {
  int len, i, s = -1;
  len = slen(path);
  while (len > 0 && (path[len - 1] == '\\' || path[len - 1] == '/')) len--;
  if (len <= 0 || len >= PATH_MAX) return -1;
  for (i = 0; i < len; i++) {
    par[i] = path[i];
    if (path[i] == '\\' || path[i] == '/') s = i;
  }
  par[len] = 0;
  *leaf = par + s + 1;
  if (!lfn_ok(*leaf)) return -1;
  if (s >= 0) par[s] = 0;
  *parent = s >= 0 ? par : "";
  return 0;
}

/* Cria em dir as entradas de leaf (LFN se precisar + 8.3). tpl traz os 32
 * bytes do 8.3 sem o nome (atributo, datas, cluster, tamanho). 0 ok; -1
 * nome; -2 pasta/disco cheio; -3 disco. */
static int dir_add(struct vol* v, uint32_t dir, const char* leaf, const uint8_t* tpl,
                   uint8_t* sbuf) {
  struct ent e;
  uint8_t n11[11], base[11], nt, sum, ent[32];
  uint32_t nl, need, clus, off, run_clus = 0, run_off = 0, run = 0, last = 0, newc, guard = 0;
  int kind, err = -3, r = 0;

  /* nome: 8.3 exato ou LFN + apelido */
  kind = sfn_make(leaf, n11, &nt);
  if (kind == 2 && (r = sfn_used(v, dir, n11, &e)) != 0) kind = 1;
  if (r < 0) return -3;
  if (kind == 1) {
    uint32_t num;
    mcopy(base, n11, 11);
    for (num = 1; num < 1000000u; num++) {
      sfn_tail(n11, base, num);
      r = sfn_used(v, dir, n11, &e);
      if (r < 0) return -3;
      if (r == 0) break;
    }
    if (num >= 1000000u) return -1;
  }
  nl = kind ? (uint32_t)(slen(leaf) + 12) / 13u : 0;
  need = nl + 1;

  /* need entradas livres seguidas na pasta (estende se faltar) */
  clus = dir;
  while (valid_clus(v, clus)) {
    if (++guard > v->nclus) return -3;
    if (load_clus(v, clus) != 0) return -3;
    for (off = 0; off < v->csize; off += 32) {
      uint8_t b0 = v->cbuf[off];
      if (b0 == 0x00 || b0 == 0xE5) {
        if (!run) {
          run_clus = clus;
          run_off = off;
        }
        if (++run >= need) goto found;
      } else {
        run = 0;
      }
    }
    last = clus;
    clus = fat_next(v, clus);
  }
  while (run < need) {
    if ((guard + 1) * (v->csize / 32u) > 65536u) return -2;   /* pasta no limite */
    newc = clus_alloc(v, last, sbuf, &err);
    if (!newc) return err;
    guard++;
    if (!run) {
      run_clus = newc;
      run_off = 0;
    }
    run += v->csize / 32u;
    last = newc;
  }
found:
  /* a FAT (cluster do conteudo, pasta estendida) antes da entrada que aponta */
  if (fat_flush(v) != 0) return -3;
  /* LFN (do ultimo pedaco para o primeiro) e o 8.3 */
  sum = sum11(n11);
  clus = run_clus;
  off = run_off;
  for (r = (int)nl; r >= 1; r--) {
    int k, base_i = (r - 1) * 13, ended = 0;
    mzero(ent, 32);
    ent[0] = (uint8_t)(r | (r == (int)nl ? 0x40 : 0));
    ent[11] = 0x0F;
    ent[13] = sum;
    for (k = 0; k < 13; k++) {
      uint16_t u;
      int ci = base_i + k;
      if (ended) u = 0xFFFF;
      else if (ci < slen(leaf)) u = oem_to_uni((unsigned char)leaf[ci]);
      else {
        u = 0;
        ended = 1;
      }
      wr16(ent + lfn_off[k], u);
    }
    if (!valid_clus(v, clus) || slot_put(v, &clus, &off, ent) != 0) return -3;
  }
  mcopy(ent, tpl, 32);
  mcopy(ent, n11, 11);
  ent[12] = kind == 0 ? nt : 0;
  if (!valid_clus(v, clus) || slot_put(v, &clus, &off, ent) != 0) return -3;
  return 0;
}

/* 8.3 sem nome: atributo, datas, primeiro cluster e tamanho. */
static void ent_tpl(uint8_t* ent, uint8_t attr, uint16_t date, uint16_t time, uint32_t clus,
                    uint32_t size) {
  int i;
  mzero(ent, 32);
  for (i = 0; i < 11; i++) ent[i] = ' ';
  ent[11] = attr;
  wr16(ent + 14, time);
  wr16(ent + 16, date);
  wr16(ent + 18, date);
  wr16(ent + 22, time);
  wr16(ent + 24, date);
  wr16(ent + 20, (uint16_t)(clus >> 16));
  wr16(ent + 26, (uint16_t)clus);
  wr32(ent + 28, size);
}

static int f_mkdir(void* p, const char* path, uint16_t date, uint16_t time) {
  struct vol* v = (struct vol*)p;
  struct ent e;
  struct find_ctx fc;
  char par[PATH_MAX];
  const char* leaf;
  const char* parent;
  uint8_t ent[32];
  uint8_t* sbuf;
  uint32_t dir, newc, i;
  int err = -3, r;
  if (!v || !path || split_path(path, par, &parent, &leaf) != 0) return -1;
  if (lookup(v, parent, &e) != 0 || !(e.attr & 0x10)) return -1;
  dir = e.clus;
  fc.want = leaf;
  r = dir_scan(v, dir, find_cb, &fc, &e);
  if (r < 0) return -3;
  if (r == 1) return (e.attr & 0x10) ? 1 : -1;

  sbuf = (uint8_t*)v->h->alloc(512);
  if (!sbuf) return -3;
  /* a pasta nova: cluster zerado com . e .. */
  newc = clus_alloc(v, 0, sbuf, &err);
  if (!newc) goto out;
  if (load_clus(v, newc) != 0) goto out;
  ent_tpl(ent, 0x10, date, time, newc, 0);
  ent[0] = '.';
  mcopy(v->cbuf, ent, 32);
  ent[1] = '.';
  i = dir == v->root ? 0 : dir;
  wr16(ent + 20, (uint16_t)(i >> 16));
  wr16(ent + 26, (uint16_t)i);
  mcopy(v->cbuf + 32, ent, 32);
  if (v->h->write(v->disk, clus_lba(v, newc), 1, v->cbuf) != 0) {
    v->cbuf_clus = 0;
    goto out;
  }
  ent_tpl(ent, 0x10, date, time, newc, 0);
  err = dir_add(v, dir, leaf, ent, sbuf);
  if (err != 0) {
    fat_set(v, newc, 0, sbuf);   /* devolve o cluster da pasta */
    if (v->free_clus != 0xFFFFFFFFu) v->free_clus++;
  }
out:
  fsinfo_save(v, sbuf);
  v->h->free(sbuf);
  v->lvalid = 0;
  return err;
}

/* Libera a cadeia que comeca em c (na janela; quem chama faz o flush). */
static void chain_free(struct vol* v, uint32_t c, uint8_t* sbuf) {
  uint32_t guard = 0;
  while (valid_clus(v, c) && guard++ <= v->nclus) {
    uint32_t nx = 0;
    if (fat_get(v, c, &nx) != 0 || nx == 0) break;   /* ja livre: cadeia estranha, para */
    if (fat_set(v, c, 0, sbuf) != 0) break;
    if (v->free_clus != 0xFFFFFFFFu) v->free_clus++;
    if (c < v->next_free) v->next_free = c;
    c = nx >= CL_END ? 0 : nx;
  }
}

/* Primeiro cluster livre a partir da dica (sem marcar). 0 = cheio/erro. */
static uint32_t clus_free_find(struct vol* v, int* err) {
  uint32_t c = v->next_free, n, val;
  if (!valid_clus(v, c)) c = 2;
  for (n = 0; n < v->nclus; n++) {
    if (fat_get(v, c, &val) != 0) {
      *err = -3;
      return 0;
    }
    if (val == 0) return c;
    if (++c >= v->nclus + 2) c = 2;
  }
  *err = -2;
  return 0;
}

static int f_write(void* p, const char* path, const void* data, uint32_t len, uint16_t date,
                   uint16_t time) {
  struct vol* v = (struct vol*)p;
  const uint8_t* src = (const uint8_t*)data;
  struct ent e;
  struct find_ctx fc;
  char par[PATH_MAX];
  const char* leaf;
  const char* parent;
  uint8_t ent[32];
  uint8_t* sbuf;
  uint32_t dir, nclus, first = 0, prev = 0, i, c, done;
  int err = -3, r, exists;
  if (!v || !path || (len && !data) || split_path(path, par, &parent, &leaf) != 0) return -1;
  if (lookup(v, parent, &e) != 0 || !(e.attr & 0x10)) return -1;
  dir = e.clus;
  fc.want = leaf;
  r = dir_scan(v, dir, find_cb, &fc, &e);
  if (r < 0) return -3;
  exists = r == 1;
  if (exists && (e.attr & 0x10)) return -1;
  if (exists && (e.attr & 0x01)) return -1;   /* somente leitura */

  sbuf = (uint8_t*)v->h->alloc(512);
  if (!sbuf) return -3;
  v->cbuf_clus = 0;
  v->lvalid = 0;

  /* 1) cadeia nova inteira na FAT (marcada na janela) */
  nclus = (len + v->csize - 1) / v->csize;
  if (v->free_clus != 0xFFFFFFFFu && nclus > v->free_clus) {
    err = -2;
    goto out;
  }
  for (i = 0; i < nclus; i++) {
    c = clus_free_find(v, &err);
    if (!c) goto undo;
    if (fat_set(v, c, 0x0FFFFFFFu, sbuf) != 0) goto undo;
    if (prev && fat_set(v, prev, c, sbuf) != 0) goto undo;
    if (!first) first = c;
    prev = c;
    if (v->free_clus != 0xFFFFFFFFu && v->free_clus) v->free_clus--;
    v->next_free = c + 1 < v->nclus + 2 ? c + 1 : 2;
  }
  if (fat_flush(v) != 0) goto undo;

  /* 2) dados: trechos de clusters seguidos direto do buffer; a ponta pelo cbuf */
  c = first;
  done = 0;
  while (done < len && valid_clus(v, c)) {
    uint32_t left = len - done;
    if (left >= v->csize && (uintptr_t)(src + done) >= 0x100000u) {
      uint32_t n = 1, cc = c, nx;
      while ((n + 1) * v->csize <= left && (n + 1) * v->csize <= RUN_MAX &&
             (nx = fat_next(v, cc)) == cc + 1) {
        cc = nx;
        n++;
      }
      if (v->h->write(v->disk, clus_lba(v, c), n * v->spc, src + done) != 0) goto undo;
      done += n * v->csize;
      c = fat_next(v, cc);
    } else {
      uint32_t k = left < v->csize ? left : v->csize;
      mcopy(v->cbuf, src + done, k);
      if (k < v->csize) mzero(v->cbuf + k, v->csize - k);
      if (v->h->write(v->disk, clus_lba(v, c), v->spc, v->cbuf) != 0) goto undo;
      v->cbuf_clus = 0;
      done += k;
      c = fat_next(v, c);
    }
    v->h->yield();
    v->cbuf_clus = 0;
  }
  if (done < len) goto undo;

  /* 3) entrada: troca no lugar ou cria; so depois solta a cadeia antiga */
  if (exists) {
    uint32_t old = e.clus;
    if (load_clus(v, e.pos_clus) != 0) goto undo;
    {
      uint8_t* d = v->cbuf + e.pos_off;
      d[11] = (uint8_t)((d[11] & 0x06) | 0x20);
      wr16(d + 18, date);
      wr16(d + 22, time);
      wr16(d + 24, date);
      wr16(d + 20, (uint16_t)(first >> 16));
      wr16(d + 26, (uint16_t)first);
      wr32(d + 28, len);
    }
    if (v->h->write(v->disk, clus_lba(v, e.pos_clus) + e.pos_off / 512u, 1,
                    v->cbuf + (e.pos_off & ~511u)) != 0) {
      v->cbuf_clus = 0;
      goto undo;
    }
    if (valid_clus(v, old) && old != first) chain_free(v, old, sbuf);
    err = 0;
  } else {
    ent_tpl(ent, 0x20, date, time, first, len);
    err = dir_add(v, dir, leaf, ent, sbuf);
    if (err != 0) goto undo_keep;
  }
  goto out;
undo:
  err = err == -2 ? -2 : -3;
undo_keep:
  if (first) chain_free(v, first, sbuf);
out:
  fsinfo_save(v, sbuf);
  v->h->free(sbuf);
  v->lvalid = 0;
  return err;
}

/* Marca como livres as entradas de e (LFN + 8.3). 0 ok; -5 erro de disco. */
static int ent_unlink(struct vol* v, const struct ent* e) {
  uint32_t clus = e->first_clus, off = e->first_off, i;
  for (i = 0; i < e->nslots && valid_clus(v, clus); i++) {
    if (load_clus(v, clus) != 0) return -5;
    v->cbuf[off] = 0xE5;
    if (v->h->write(v->disk, clus_lba(v, clus) + off / 512u, 1, v->cbuf + (off & ~511u)) != 0) {
      v->cbuf_clus = 0;
      return -5;
    }
    off += 32;
    if (off >= v->csize) {
      off = 0;
      clus = fat_next(v, clus);
    }
  }
  return 0;
}

static int f_remove(void* p, const char* path) {
  struct vol* v = (struct vol*)p;
  struct ent e;
  uint8_t* sbuf;
  int err;
  if (!v || !path || !path[0]) return -1;
  if (lookup(v, path, &e) != 0) return -1;
  if (e.attr & 0x10) return -3;
  if (e.attr & 0x01) return -4;
  sbuf = (uint8_t*)v->h->alloc(512);
  if (!sbuf) return -5;
  /* LFN + 8.3 marcados como livres; depois a cadeia */
  err = ent_unlink(v, &e);
  if (!err && valid_clus(v, e.clus)) chain_free(v, e.clus, sbuf);
  fsinfo_save(v, sbuf);
  v->h->free(sbuf);
  v->lvalid = 0;
  return err;
}

/* Acha leaf na pasta de path (*dir = cluster dela). 1 achou (e preenchido),
 * 0 nao existe, -1 a pasta nao existe, -2 nome invalido, -4 erro de disco.
 * par e de quem chama: leaf aponta para dentro dele. */
static int find_in_parent(struct vol* v, const char* path, char* par, const char** leaf,
                          uint32_t* dir, struct ent* e) {
  struct find_ctx fc;
  const char* parent;
  int r;
  if (split_path(path, par, &parent, leaf) != 0) return -2;
  if (lookup(v, parent, e) != 0 || !(e->attr & 0x10)) return -1;
  *dir = e->clus;
  fc.want = *leaf;
  r = dir_scan(v, *dir, find_cb, &fc, e);
  return r < 0 ? -4 : r;
}

/* Troca o nome e/ou a pasta sem copiar: uma entrada nova aponta para a mesma
 * cadeia e a antiga some. Codigos em fsdrv.h (os de drives_rename). */
static int f_rename(void* p, const char* from, const char* to) {
  struct vol* v = (struct vol*)p;
  struct ent src, dst;
  char par[PATH_MAX];
  const char* leaf;
  uint8_t tpl[32];
  uint8_t* sbuf;
  uint32_t sdir = 0, dir = 0;
  int r, exists, same, err = 0;
  if (!v || !from || !to) return -2;
  r = find_in_parent(v, from, par, &leaf, &sdir, &src);
  if (r <= 0) return r == 0 ? -1 : r;
  r = find_in_parent(v, to, par, &leaf, &dir, &dst);
  if (r < 0) return r;
  if ((src.attr & 0x10) && dir != sdir) return -7;
  exists = r == 1;
  same = exists && dst.pos_clus == src.pos_clus && dst.pos_off == src.pos_off;  /* so muda a caixa */
  if (exists && !same) {
    if ((dst.attr & 0x10) || (src.attr & 0x10)) return -3;
    if (dst.attr & 0x01) return -4;
  }
  /* o 8.3 da origem vira o molde: atributo, datas, cluster e tamanho */
  if (load_clus(v, src.pos_clus) != 0) return -4;
  mcopy(tpl, v->cbuf + src.pos_off, 32);
  sbuf = (uint8_t*)v->h->alloc(512);
  if (!sbuf) return -4;
  v->lvalid = 0;
  /* O nome que sai da pasta de destino sai antes, para o novo poder usar o
   * mesmo 8.3: o arquivo substituido, ou a propria origem se so muda a caixa. */
  if (exists && ent_unlink(v, same ? &src : &dst) != 0) {
    err = -4;
    goto out;
  }
  r = dir_add(v, dir, leaf, tpl, sbuf);
  if (r == 0 && !same && ent_unlink(v, &src) != 0) r = -3;
  if (r != 0) err = r == -3 ? -4 : -2;
  /* o substituido ja saiu da pasta: a cadeia dele volta para o disco */
  if (exists && !same && valid_clus(v, dst.clus) && dst.clus != src.clus)
    chain_free(v, dst.clus, sbuf);
out:
  fsinfo_save(v, sbuf);
  v->h->free(sbuf);
  v->lvalid = 0;
  return err;
}

static const struct fsdrv_ops ops = {
  FSDRV_ABI_RENAME, FAT32_NAME, f_probe, f_mount, f_umount, f_stat, f_read, f_items, f_long_name,
  f_space, f_mkdir, f_write, f_remove, f_rename,
};

const struct fsdrv_ops* fs_driver(void) { return &ops; }
