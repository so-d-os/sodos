/* ntfs.c - Leitor NTFS so leitura. Ver ntfs.h.
 *
 * Registro da MFT: "FILE" + fixups (USA) a cada 512 bytes. O $DATA de um
 * arquivo grande pode estar espalhado em varios registros, listados no
 * $ATTRIBUTE_LIST do registro base: juntamos todas as extensoes numa runlist
 * so, ordenada por VCN. Pastas sao arvores B ($I30): percorremos a raiz e os
 * blocos INDX pelos ponteiros de subno (so blocos vivos) e comparamos os nomes
 * sem diferenca de caixa (ASCII). */
#include <stdlib.h>
#include <string.h>
#include "ntfs.h"

char wsl_err[160];
void wsl_seterr(const char* a, const char* b) {
  size_t n = 0;
  if (a)
    while (*a && n < sizeof(wsl_err) - 1) wsl_err[n++] = *a++;
  if (b)
    while (*b && n < sizeof(wsl_err) - 1) wsl_err[n++] = *b++;
  wsl_err[n] = 0;
}

#define AT_STD_INFO 0x10u
#define AT_ATTR_LIST 0x20u
#define AT_FILE_NAME 0x30u
#define AT_VOLUME_NAME 0x60u
#define AT_DATA 0x80u
#define AT_INDEX_ROOT 0x90u
#define AT_INDEX_ALLOC 0xA0u
#define AT_END 0xFFFFFFFFu

typedef struct {
  uint64_t vcn, lcn, len; /* lcn = ~0 esparso */
} run_t;

typedef struct {
  run_t* r;
  int n, cap;
} runlist;

struct ntfs_vol {
  wsl_io io;
  uint32_t cluster;   /* bytes */
  uint32_t rec_size;  /* registro da MFT */
  uint32_t idx_size;  /* bloco INDX */
  runlist mft;        /* runlist do $MFT */
  uint8_t* rec;       /* rascunho de um registro */
  char label[64];
};

struct ntfs_file {
  ntfs_vol* v;
  uint64_t recno;
  int dir;
  uint64_t size;
  runlist data;      /* $DATA nao residente */
  uint8_t* resident; /* ou $DATA residente */
};

/* ---------------- runlists ---------------- */

static int rl_add(runlist* rl, uint64_t vcn, uint64_t lcn, uint64_t len) {
  if (rl->n == rl->cap) {
    int nc = rl->cap ? rl->cap * 2 : 16;
    run_t* p = (run_t*)realloc(rl->r, (size_t)nc * sizeof(run_t));
    if (!p) return -1;
    rl->r = p;
    rl->cap = nc;
  }
  rl->r[rl->n].vcn = vcn;
  rl->r[rl->n].lcn = lcn;
  rl->r[rl->n].len = len;
  rl->n++;
  return 0;
}

/* Decodifica a runlist de um atributo nao residente. */
static int rl_decode(runlist* rl, const uint8_t* a, uint32_t alen) {
  uint64_t vcn = rd64(a + 0x10);
  uint32_t off = rd16(a + 0x20);
  int64_t lcn = 0;
  if (off >= alen) return -1;
  while (off < alen && a[off]) {
    uint8_t h = a[off++];
    uint32_t ln = h & 0xFu, lo = h >> 4, i;
    uint64_t len = 0;
    int64_t delta = 0;
    if (ln == 0 || ln > 8 || lo > 8 || off + ln + lo > alen) return -1;
    for (i = 0; i < ln; i++) len |= (uint64_t)a[off + i] << (8 * i);
    off += ln;
    if (lo) {
      for (i = 0; i < lo; i++) delta |= (int64_t)((uint64_t)a[off + i] << (8 * i));
      if (a[off + lo - 1] & 0x80) delta |= (int64_t)(~0ULL << (8 * lo)); /* sinal */
      off += lo;
      lcn += delta;
      if (rl_add(rl, vcn, (uint64_t)lcn, len)) return -1;
    } else {
      if (rl_add(rl, vcn, ~0ULL, len)) return -1;
    }
    vcn += len;
  }
  return 0;
}

static int run_cmp(const void* a, const void* b) {
  const run_t* x = (const run_t*)a;
  const run_t* y = (const run_t*)b;
  return x->vcn < y->vcn ? -1 : x->vcn > y->vcn;
}

/* Le bytes [off, off+len) de um fluxo descrito por rl. Buracos = zeros. */
static int rl_read(ntfs_vol* v, const runlist* rl, uint64_t off, void* buf, uint32_t len) {
  uint8_t* p = (uint8_t*)buf;
  int lo = 0;
  while (len) {
    uint64_t vcn = off / v->cluster, in = off % v->cluster;
    int hi = rl->n - 1, found = -1;
    uint64_t avail, take;
    /* busca binaria a partir do ultimo achado */
    if (lo > hi || rl->r[lo].vcn > vcn) lo = 0;
    while (lo <= hi) {
      int m = (lo + hi) / 2;
      if (vcn < rl->r[m].vcn) hi = m - 1;
      else if (vcn >= rl->r[m].vcn + rl->r[m].len) lo = m + 1;
      else {
        found = m;
        break;
      }
    }
    if (found < 0) {
      wsl_seterr("ntfs: leitura fora da runlist", 0);
      return -1;
    }
    lo = found;
    avail = (rl->r[found].vcn + rl->r[found].len - vcn) * v->cluster - in;
    take = len < avail ? len : avail;
    if (rl->r[found].lcn == ~0ULL) {
      memset(p, 0, (size_t)take);
    } else {
      uint64_t pos = (rl->r[found].lcn + (vcn - rl->r[found].vcn)) * v->cluster + in;
      if (wio_read(&v->io, pos, p, (uint32_t)take)) {
        wsl_seterr("ntfs: erro de leitura no disco", 0);
        return -1;
      }
    }
    p += take;
    off += take;
    len -= (uint32_t)take;
  }
  return 0;
}

/* ---------------- registros ---------------- */

static int fixup(uint8_t* b, uint32_t size) {
  uint32_t uo = rd16(b + 4), uc = rd16(b + 6), i;
  if (uc == 0 || uo + uc * 2u > size || (uc - 1u) * 512u > size) return -1;
  for (i = 1; i < uc; i++) {
    uint8_t* end = b + i * 512u - 2u;
    if (end[0] != b[uo] || end[1] != b[uo + 1]) return -1;
    end[0] = b[uo + i * 2];
    end[1] = b[uo + i * 2 + 1];
  }
  return 0;
}

static int read_record(ntfs_vol* v, uint64_t recno, uint8_t* out) {
  if (rl_read(v, &v->mft, recno * v->rec_size, out, v->rec_size)) return -1;
  if (memcmp(out, "FILE", 4) || fixup(out, v->rec_size)) {
    wsl_seterr("ntfs: registro da MFT invalido", 0);
    return -1;
  }
  return 0;
}

/* Proximo atributo do registro a partir de *pos (0 = primeiro). */
static uint8_t* next_attr(ntfs_vol* v, uint8_t* rec, uint32_t* pos) {
  uint32_t off = *pos ? *pos : rd16(rec + 0x14);
  uint32_t used = rd32(rec + 0x18);
  uint8_t* a;
  if (used > v->rec_size) used = v->rec_size;
  if (off + 8 > used) return 0;
  a = rec + off;
  if (rd32(a) == AT_END) return 0;
  if (rd32(a + 4) < 16 || off + rd32(a + 4) > used) return 0;
  *pos = off + rd32(a + 4);
  return a;
}

static int attr_unnamed(const uint8_t* a) { return a[9] == 0; }

/* Nome do atributo == "$I30"? (UTF-16) */
static int attr_is_i30(const uint8_t* a) {
  const uint8_t* n = a + rd16(a + 0xA);
  return a[9] == 4 && n[0] == '$' && n[2] == 'I' && n[4] == '3' && n[6] == '0';
}

/* Chama fn para cada atributo do tipo pedido do arquivo, inclusive os que
 * estao em registros de extensao ($ATTRIBUTE_LIST). fn recebe o atributo
 * dentro de um buffer temporario. */
typedef int (*attr_fn)(ntfs_vol* v, const uint8_t* a, void* arg);

static int read_stream_small(ntfs_vol* v, const uint8_t* a, uint8_t** out, uint32_t* olen) {
  if (!a[8]) {
    uint32_t vl = rd32(a + 0x10), vo = rd16(a + 0x14);
    if (vo + vl > rd32(a + 4)) return -1;
    *out = (uint8_t*)malloc(vl ? vl : 1);
    if (!*out) return -1;
    memcpy(*out, a + vo, vl);
    *olen = vl;
    return 0;
  } else {
    runlist rl = {0, 0, 0};
    uint64_t sz = rd64(a + 0x30);
    if (sz > (64u << 20) || rl_decode(&rl, a, rd32(a + 4))) {
      free(rl.r);
      return -1;
    }
    *out = (uint8_t*)malloc(sz ? (size_t)sz : 1);
    if (!*out || rl_read(v, &rl, 0, *out, (uint32_t)sz)) {
      free(rl.r);
      free(*out);
      *out = 0;
      return -1;
    }
    free(rl.r);
    *olen = (uint32_t)sz;
    return 0;
  }
}

static int for_attrs(ntfs_vol* v, uint64_t recno, uint32_t type, attr_fn fn, void* arg) {
  uint8_t* rec = (uint8_t*)malloc(v->rec_size);
  uint8_t* ext = 0;
  uint8_t* al = 0;
  uint32_t pos = 0, alen = 0, i;
  uint8_t* a;
  int rc = 0, have_list = 0;
  if (!rec) return -1;
  if (read_record(v, recno, rec)) {
    free(rec);
    return -1;
  }
  while ((a = next_attr(v, rec, &pos)) != 0) {
    if (rd32(a) == AT_ATTR_LIST) {
      if (read_stream_small(v, a, &al, &alen)) {
        rc = -1;
        goto out;
      }
      have_list = 1;
      break;
    }
  }
  if (!have_list) {
    pos = 0;
    while ((a = next_attr(v, rec, &pos)) != 0) {
      if (rd32(a) == type && (rc = fn(v, a, arg)) != 0) goto out;
    }
    goto out;
  }
  /* Com lista: cada entrada aponta o registro que guarda o atributo. Varias
   * entradas podem apontar o mesmo registro: lemos cada (registro, id) uma vez. */
  ext = (uint8_t*)malloc(v->rec_size);
  if (!ext) {
    rc = -1;
    goto out;
  }
  for (i = 0; i + 0x1A <= alen;) {
    const uint8_t* e = al + i;
    uint32_t el = rd16(e + 4);
    if (el < 0x1A || i + el > alen) break;
    if (rd32(e) == type) {
      uint64_t ref = rd64(e + 0x10) & 0x0000FFFFFFFFFFFFULL;
      uint16_t id = rd16(e + 0x18);
      uint8_t* r = ref == recno ? rec : ext;
      uint32_t p2 = 0;
      if (r == ext && read_record(v, ref, ext)) {
        rc = -1;
        goto out;
      }
      while ((a = next_attr(v, r, &p2)) != 0) {
        if (rd32(a) == type && rd16(a + 0xE) == id) {
          if ((rc = fn(v, a, arg)) != 0) goto out;
          break;
        }
      }
    }
    i += el;
  }
out:
  free(al);
  free(ext);
  free(rec);
  return rc;
}

/* ---------------- $DATA ---------------- */

struct data_acc {
  ntfs_file* f;
  int found;
  int err;
};

static int data_fn(ntfs_vol* v, const uint8_t* a, void* arg) {
  struct data_acc* d = (struct data_acc*)arg;
  (void)v;
  if (!attr_unnamed(a)) return 0;
  if (rd16(a + 0xC) & 0x00FFu) { /* comprimido */
    wsl_seterr("ntfs: arquivo comprimido (nao suportado)", 0);
    d->err = 1;
    return 1;
  }
  if (rd16(a + 0xC) & 0x4000u) {
    wsl_seterr("ntfs: arquivo criptografado (EFS)", 0);
    d->err = 1;
    return 1;
  }
  d->found = 1;
  if (!a[8]) {
    uint32_t vl = rd32(a + 0x10), vo = rd16(a + 0x14);
    free(d->f->resident);
    d->f->resident = (uint8_t*)malloc(vl ? vl : 1);
    if (!d->f->resident) return 1;
    memcpy(d->f->resident, a + vo, vl);
    d->f->size = vl;
    return 0;
  }
  if (rd64(a + 0x10) == 0) d->f->size = rd64(a + 0x30);
  if (rl_decode(&d->f->data, a, rd32(a + 4))) {
    wsl_seterr("ntfs: runlist invalida", 0);
    d->err = 1;
    return 1;
  }
  return 0;
}

static int load_data(ntfs_file* f) {
  struct data_acc d = {f, 0, 0};
  if (for_attrs(f->v, f->recno, AT_DATA, data_fn, &d) < 0 || d.err) return -1;
  if (f->data.n > 1) qsort(f->data.r, (size_t)f->data.n, sizeof(run_t), run_cmp);
  return 0;
}

/* ---------------- montagem ---------------- */

int ntfs_probe(const uint8_t* b) {
  return memcmp(b + 3, "NTFS    ", 8) == 0 && b[510] == 0x55 && b[511] == 0xAA;
}

static void utf16_to_utf8(const uint8_t* s, int n, char* out, int max) {
  int i, o = 0;
  for (i = 0; i < n; i++) {
    uint32_t c = rd16(s + i * 2);
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
      uint32_t c2 = rd16(s + (i + 1) * 2);
      if (c2 >= 0xDC00 && c2 < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
        i++;
      }
    }
    if (c < 0x80) {
      if (o + 1 >= max) break;
      out[o++] = (char)c;
    } else if (c < 0x800) {
      if (o + 2 >= max) break;
      out[o++] = (char)(0xC0 | (c >> 6));
      out[o++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      if (o + 3 >= max) break;
      out[o++] = (char)(0xE0 | (c >> 12));
      out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (c & 0x3F));
    } else {
      if (o + 4 >= max) break;
      out[o++] = (char)(0xF0 | (c >> 18));
      out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
      out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (c & 0x3F));
    }
  }
  out[o] = 0;
}

static int mft_fn(ntfs_vol* v, const uint8_t* a, void* arg) {
  runlist* rl = (runlist*)arg;
  (void)v;
  if (!attr_unnamed(a) || !a[8]) return 0;
  return rl_decode(rl, a, rd32(a + 4)) ? 1 : 0;
}

static int label_fn(ntfs_vol* v, const uint8_t* a, void* arg) {
  (void)arg;
  if (!a[8]) utf16_to_utf8(a + rd16(a + 0x14), (int)(rd32(a + 0x10) / 2), v->label, sizeof v->label);
  return 1;
}

ntfs_vol* ntfs_mount(const wsl_io* part) {
  uint8_t b[512];
  ntfs_vol* v;
  uint32_t bps, spc;
  int8_t cpr, cpi;
  uint64_t mft_lcn;
  if (wio_read(part, 0, b, 512) || !ntfs_probe(b)) {
    wsl_seterr("ntfs: nao e NTFS", 0);
    return 0;
  }
  v = (ntfs_vol*)calloc(1, sizeof *v);
  if (!v) return 0;
  v->io = *part;
  bps = rd16(b + 0x0B);
  spc = b[0x0D];
  if (spc > 0x80) spc = 1u << (256u - spc);
  v->cluster = bps * spc;
  cpr = (int8_t)b[0x40];
  cpi = (int8_t)b[0x44];
  v->rec_size = cpr > 0 ? (uint32_t)cpr * v->cluster : 1u << (-cpr);
  v->idx_size = cpi > 0 ? (uint32_t)cpi * v->cluster : 1u << (-cpi);
  mft_lcn = rd64(b + 0x30);
  if (bps < 512 || v->cluster == 0 || v->cluster > (2u << 20) || v->rec_size < 1024 ||
      v->rec_size > 65536) {
    wsl_seterr("ntfs: geometria estranha", 0);
    free(v);
    return 0;
  }
  v->rec = (uint8_t*)malloc(v->rec_size);
  /* Ponto de partida: os primeiros registros da MFT ficam em mft_lcn. Com
   * isso lemos o registro 0 e tiramos a runlist inteira do $MFT. */
  rl_add(&v->mft, 0, mft_lcn, (16ull * v->rec_size + v->cluster - 1) / v->cluster);
  {
    runlist full = {0, 0, 0}, first = {0, 0, 0};
    uint32_t pos = 0;
    uint8_t* a;
    /* 1o: a extensao do $DATA que esta no proprio registro 0 (cobre bem
     * mais que 16 registros, inclusive os de extensao do $MFT). */
    if (read_record(v, 0, v->rec)) {
      ntfs_umount(v);
      return 0;
    }
    while ((a = next_attr(v, v->rec, &pos)) != 0)
      if (rd32(a) == AT_DATA && a[8] && attr_unnamed(a)) rl_decode(&first, a, rd32(a + 4));
    if (first.n) {
      free(v->mft.r);
      v->mft = first;
    }
    if (for_attrs(v, 0, AT_DATA, mft_fn, &full) != 0 || full.n == 0) {
      free(full.r);
      wsl_seterr("ntfs: $MFT ilegivel", 0);
      ntfs_umount(v);
      return 0;
    }
    if (full.n > 1) qsort(full.r, (size_t)full.n, sizeof(run_t), run_cmp);
    free(v->mft.r);
    v->mft = full;
  }
  for_attrs(v, 3, AT_VOLUME_NAME, label_fn, 0); /* $Volume */
  return v;
}

void ntfs_umount(ntfs_vol* v) {
  if (!v) return;
  free(v->mft.r);
  free(v->rec);
  free(v);
}

const char* ntfs_label(ntfs_vol* v) { return v->label; }

/* ---------------- pastas ---------------- */

typedef int (*ent_fn)(void* arg, const uint8_t* fname /* FILE_NAME */, uint64_t ref);

struct idx_walk {
  ntfs_vol* v;
  runlist alloc;  /* INDEX_ALLOCATION */
  int have_alloc;
  uint8_t* blk;
  ent_fn fn;
  void* arg;
  int depth;
};

static int walk_node(struct idx_walk* w, const uint8_t* hdr, uint32_t limit);

static int walk_vcn(struct idx_walk* w, uint64_t vcn) {
  uint8_t* blk;
  uint64_t off;
  int rc;
  if (!w->have_alloc || w->depth > 32) return -1;
  /* O VCN do subno conta em clusters se o bloco >= cluster, senao em 512. */
  off = w->v->idx_size >= w->v->cluster ? vcn * w->v->cluster : vcn * 512u;
  blk = (uint8_t*)malloc(w->v->idx_size);
  if (!blk) return -1;
  if (rl_read(w->v, &w->alloc, off, blk, w->v->idx_size) || memcmp(blk, "INDX", 4) ||
      fixup(blk, w->v->idx_size)) {
    free(blk);
    wsl_seterr("ntfs: bloco INDX invalido", 0);
    return -1;
  }
  w->depth++;
  rc = walk_node(w, blk + 0x18, w->v->idx_size - 0x18);
  w->depth--;
  free(blk);
  return rc;
}

/* hdr = cabecalho do indice (entries_offset, index_length, ...). */
static int walk_node(struct idx_walk* w, const uint8_t* hdr, uint32_t limit) {
  uint32_t off = rd32(hdr), end = rd32(hdr + 4);
  int rc;
  if (end > limit) end = limit;
  while (off + 0x10 <= end) {
    const uint8_t* e = hdr + off;
    uint32_t el = rd16(e + 8), kl = rd16(e + 0xA), fl = rd16(e + 0xC);
    if (el < 0x10 || off + el > end) return -1;
    if (fl & 1) { /* subno antes desta entrada */
      if ((rc = walk_vcn(w, rd64(e + el - 8))) != 0) return rc;
    }
    if (fl & 2) break; /* ultima */
    if (kl >= 0x42 && (rc = w->fn(w->arg, e + 0x10, rd64(e))) != 0) return rc;
    off += el;
  }
  return 0;
}

static int alloc_fn(ntfs_vol* v, const uint8_t* a, void* arg) {
  struct idx_walk* w = (struct idx_walk*)arg;
  (void)v;
  if (!a[8] || !attr_is_i30(a)) return 0;
  if (rl_decode(&w->alloc, a, rd32(a + 4))) return 1;
  w->have_alloc = 1;
  return 0;
}

struct root_acc {
  uint8_t* val;
  uint32_t len;
};

static int root_fn(ntfs_vol* v, const uint8_t* a, void* arg) {
  struct root_acc* r = (struct root_acc*)arg;
  (void)v;
  if (a[8] || !attr_is_i30(a)) return 0;
  r->len = rd32(a + 0x10);
  r->val = (uint8_t*)malloc(r->len ? r->len : 1);
  if (!r->val) return 1;
  memcpy(r->val, a + rd16(a + 0x14), r->len);
  return 1;
}

static int walk_dir(ntfs_vol* v, uint64_t recno, ent_fn fn, void* arg) {
  struct root_acc r = {0, 0};
  struct idx_walk w;
  int rc;
  memset(&w, 0, sizeof w);
  w.v = v;
  w.fn = fn;
  w.arg = arg;
  if (for_attrs(v, recno, AT_INDEX_ROOT, root_fn, &r) < 0 || !r.val || r.len < 0x20) {
    free(r.val);
    wsl_seterr("ntfs: nao e pasta", 0);
    return -1;
  }
  if (for_attrs(v, recno, AT_INDEX_ALLOC, alloc_fn, &w) < 0) {
    free(r.val);
    free(w.alloc.r);
    return -1;
  }
  if (w.alloc.n > 1) qsort(w.alloc.r, (size_t)w.alloc.n, sizeof(run_t), run_cmp);
  rc = walk_node(&w, r.val + 0x10, r.len - 0x10);
  free(r.val);
  free(w.alloc.r);
  return rc;
}

static int ieq(const char* a, const char* b) {
  for (;; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
    if (!x) return 1;
  }
}

struct find_acc {
  const char* name;
  uint64_t ref;
  int found;
};

static int find_fn(void* arg, const uint8_t* fn, uint64_t ref) {
  struct find_acc* f = (struct find_acc*)arg;
  char nm[256];
  if (fn[0x41] == 2) return 0; /* nome DOS */
  utf16_to_utf8(fn + 0x42, fn[0x40], nm, sizeof nm);
  if (ieq(nm, f->name)) {
    f->ref = ref;
    f->found = 1;
    return 1;
  }
  return 0;
}

/* Registro do caminho; 5 = raiz. */
static int resolve(ntfs_vol* v, const char* path, uint64_t* out) {
  uint64_t cur = 5;
  char comp[256];
  while (*path) {
    int n = 0;
    struct find_acc f;
    while (*path == '\\' || *path == '/') path++;
    if (!*path) break;
    while (*path && *path != '\\' && *path != '/' && n < 255) comp[n++] = *path++;
    comp[n] = 0;
    f.name = comp;
    f.found = 0;
    f.ref = 0;
    if (walk_dir(v, cur, find_fn, &f) < 0 && !f.found) return -1;
    if (!f.found) {
      wsl_seterr("ntfs: nao achei ", comp);
      return -1;
    }
    /* confere a sequencia: entrada velha num bloco reaproveitado */
    if (read_record(v, f.ref & 0x0000FFFFFFFFFFFFULL, v->rec)) return -1;
    if (!(rd16(v->rec + 0x16) & 1) || rd16(v->rec + 0x10) != (uint16_t)(f.ref >> 48)) {
      wsl_seterr("ntfs: entrada velha para ", comp);
      return -1;
    }
    cur = f.ref & 0x0000FFFFFFFFFFFFULL;
  }
  *out = cur;
  return 0;
}

ntfs_file* ntfs_open(ntfs_vol* v, const char* path) {
  uint64_t rec;
  ntfs_file* f;
  if (resolve(v, path, &rec)) return 0;
  f = (ntfs_file*)calloc(1, sizeof *f);
  if (!f) return 0;
  f->v = v;
  f->recno = rec;
  if (read_record(v, rec, v->rec)) {
    free(f);
    return 0;
  }
  f->dir = (rd16(v->rec + 0x16) & 2) != 0;
  if (!f->dir && load_data(f)) {
    ntfs_close(f);
    return 0;
  }
  return f;
}

void ntfs_close(ntfs_file* f) {
  if (!f) return;
  free(f->data.r);
  free(f->resident);
  free(f);
}

int ntfs_isdir(const ntfs_file* f) { return f->dir; }
uint64_t ntfs_fsize(const ntfs_file* f) { return f->size; }

int ntfs_pread(ntfs_file* f, uint64_t off, void* buf, uint32_t len) {
  if (off > f->size || len > f->size - off) {
    wsl_seterr("ntfs: leitura alem do fim", 0);
    return -1;
  }
  if (f->resident) {
    memcpy(buf, f->resident + off, len);
    return 0;
  }
  return rl_read(f->v, &f->data, off, buf, len);
}

static int fio_read(void* ctx, uint64_t off, void* buf, uint32_t len) {
  return ntfs_pread((ntfs_file*)ctx, off, buf, len);
}

void ntfs_file_io(ntfs_file* f, wsl_io* out) {
  out->read = fio_read;
  out->ctx = f;
  out->size = f->size;
}

struct list_acc {
  ntfs_dir_cb cb;
  void* arg;
};

static int list_fn(void* arg, const uint8_t* fn, uint64_t ref) {
  struct list_acc* l = (struct list_acc*)arg;
  char nm[256];
  (void)ref;
  if (fn[0x41] == 2) return 0;
  utf16_to_utf8(fn + 0x42, fn[0x40], nm, sizeof nm);
  return l->cb(l->arg, nm, (rd32(fn + 0x38) & 0x10000000u) != 0, rd64(fn + 0x30));
}

int ntfs_list(ntfs_vol* v, const char* path, ntfs_dir_cb cb, void* arg) {
  uint64_t rec;
  struct list_acc l = {cb, arg};
  if (resolve(v, path, &rec)) return -1;
  return walk_dir(v, rec, list_fn, &l) < 0 ? -1 : 0;
}
