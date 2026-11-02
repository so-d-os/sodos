/* pack.c - Le um packfile (versao 2) em memoria: acha cada objeto, aplica
 * os deltas (OFS e REF) e guarda o sha de todos para pack_get.
 * Objetos resolvidos ficam num cache por ordem de chegada (CACHE_MAX bytes). */
#include "git.h"

#define CACHE_MAX (24 * 1024 * 1024)
#define CACHE_N 512

struct pobj {
  int off;          /* cabecalho no pack */
  int data;         /* inicio do zlib */
  int type;         /* 1..4, 6, 7 */
  int size;         /* tamanho descomprimido (do delta, se for delta) */
  int base;         /* indice da base (OFS/REF), -1 */
  int rtype;        /* tipo final, depois de resolvido */
  int hashed;
  unsigned char sha[20];
  unsigned char ref[20];
};

static unsigned char* pk;
static int pklen;
static struct pobj* obj;
static int nobj;
static int* by_sha;       /* indices ordenados por sha */

struct centry {
  int idx;
  unsigned char* buf;
  int size;
};
static struct centry cache[CACHE_N];
static int c_head, c_count, c_bytes;

static void cache_drop_oldest(void) {
  struct centry* e = &cache[c_head];
  free(e->buf);
  c_bytes -= e->size;
  e->buf = 0;
  c_head = (c_head + 1) % CACHE_N;
  c_count--;
}

static void cache_put(int idx, unsigned char* buf, int size) {
  struct centry* e;
  while (c_count > 0 && (c_count == CACHE_N || c_bytes + size > CACHE_MAX)) cache_drop_oldest();
  e = &cache[(c_head + c_count) % CACHE_N];
  e->idx = idx;
  e->buf = buf;
  e->size = size;
  c_bytes += size;
  c_count++;
}

static unsigned char* cache_find(int idx, int* size) {
  int i;
  for (i = c_count - 1; i >= 0; i--) {
    struct centry* e = &cache[(c_head + i) % CACHE_N];
    if (e->idx == idx) {
      *size = e->size;
      return e->buf;
    }
  }
  return 0;
}

static void cache_clear(void) {
  while (c_count) cache_drop_oldest();
  c_head = 0;
}

static unsigned be32(const unsigned char* p) {
  return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

/* objeto que comeca em off, entre os n primeiros */
static int find_off(int off, int n) {
  int lo = 0, hi = n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (obj[m].off == off) return m;
    if (obj[m].off < off) lo = m + 1;
    else hi = m - 1;
  }
  return -1;
}

/* Aplica o delta d (dl bytes) sobre base. Devolve malloc com *outn bytes. */
static unsigned char* apply_delta(const unsigned char* base, int bl, const unsigned char* d, int dl, int* outn) {
  int p = 0, sh, n = 0;
  unsigned src = 0, dst = 0;
  unsigned char* out;
  for (sh = 0; p < dl; sh += 7) {
    src |= (unsigned)(d[p] & 0x7F) << sh;
    if (!(d[p++] & 0x80)) break;
  }
  for (sh = 0; p < dl; sh += 7) {
    dst |= (unsigned)(d[p] & 0x7F) << sh;
    if (!(d[p++] & 0x80)) break;
  }
  if ((int)src != bl) return 0;
  out = (unsigned char*)malloc(dst ? dst : 1);
  if (!out) return 0;
  while (p < dl) {
    int c = d[p++];
    if (c & 0x80) {
      unsigned co = 0, cs = 0;
      if (c & 0x01) co |= d[p++];
      if (c & 0x02) co |= (unsigned)d[p++] << 8;
      if (c & 0x04) co |= (unsigned)d[p++] << 16;
      if (c & 0x08) co |= (unsigned)d[p++] << 24;
      if (c & 0x10) cs |= d[p++];
      if (c & 0x20) cs |= (unsigned)d[p++] << 8;
      if (c & 0x40) cs |= (unsigned)d[p++] << 16;
      if (cs == 0) cs = 0x10000;
      if (co + cs > (unsigned)bl || n + (int)cs > (int)dst) {
        free(out);
        return 0;
      }
      memcpy(out + n, base + co, cs);
      n += (int)cs;
    } else if (c) {
      if (p + c > dl || n + c > (int)dst) {
        free(out);
        return 0;
      }
      memcpy(out + n, d + p, (size_t)c);
      n += c;
      p += c;
    } else {
      free(out);
      return 0;
    }
  }
  if (n != (int)dst) {
    free(out);
    return 0;
  }
  *outn = n;
  return out;
}

static unsigned char* unzip(struct pobj* o) {
  unsigned char* b = (unsigned char*)malloc(o->size ? o->size : 1);
  int used;
  if (!b) return 0;
  if (zinflate(pk + o->data, pklen - o->data, b, o->size, &used) != o->size) {
    free(b);
    return 0;
  }
  return b;
}

/* Conteudo do objeto i (no cache). *size = tamanho final. 0 se falhou. */
static unsigned char* resolve(int i, int* size, int depth) {
  struct pobj* o = &obj[i];
  unsigned char* b = cache_find(i, size);
  unsigned char* base;
  unsigned char* d;
  unsigned char* out;
  int bsize, n;
  if (b) return b;
  if (o->type >= OBJ_COMMIT && o->type <= OBJ_TAG) {
    b = unzip(o);
    if (!b) return 0;
    o->rtype = o->type;
    cache_put(i, b, o->size);
    *size = o->size;
    return b;
  }
  if (o->base < 0 || depth > 200) return 0;
  base = resolve(o->base, &bsize, depth + 1);
  if (!base) return 0;
  d = unzip(o);
  if (!d) return 0;
  out = apply_delta(base, bsize, d, o->size, &n);
  free(d);
  if (!out) return 0;
  o->rtype = obj[o->base].rtype;
  cache_put(i, out, n);
  *size = n;
  return out;
}

static const char* type_name(int t) {
  if (t == OBJ_COMMIT) return "commit";
  if (t == OBJ_TREE) return "tree";
  if (t == OBJ_BLOB) return "blob";
  return "tag";
}

static int sha_cmp(const void* a, const void* b) {
  return memcmp(obj[*(const int*)a].sha, obj[*(const int*)b].sha, 20);
}

static int lookup(const unsigned char* sha) {
  int lo = 0, hi = nobj - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2, c = memcmp(obj[by_sha[m]].sha, sha, 20);
    if (c == 0) return by_sha[m];
    if (c < 0) lo = m + 1;
    else hi = m - 1;
  }
  return -1;
}

static int lookup_slow(const unsigned char* sha) {
  int i;
  for (i = 0; i < nobj; i++)
    if (obj[i].hashed && memcmp(obj[i].sha, sha, 20) == 0) return i;
  return -1;
}

int pack_load(unsigned char* data, int len, char* err) {
  int i, p, pending, done;
  unsigned char sum[20];
  sha1_ctx c;
  pk = data;
  pklen = len;
  if (len < 32 || memcmp(data, "PACK", 4) != 0 || be32(data + 4) != 2) {
    sprintf(err, "pack invalido");
    return -1;
  }
  sha1_init(&c);
  sha1_update(&c, data, len - 20);
  sha1_final(&c, sum);
  if (memcmp(sum, data + len - 20, 20) != 0) {
    sprintf(err, "pack corrompido (sha do fim nao confere)");
    return -1;
  }
  nobj = (int)be32(data + 8);
  obj = (struct pobj*)calloc((size_t)(nobj ? nobj : 1), sizeof(struct pobj));
  by_sha = (int*)malloc(sizeof(int) * (size_t)(nobj ? nobj : 1));
  if (!obj || !by_sha) {
    sprintf(err, "sem memoria para %d objetos", nobj);
    return -1;
  }
  /* 1) onde comeca cada objeto */
  p = 12;
  for (i = 0; i < nobj; i++) {
    struct pobj* o = &obj[i];
    int ch, sh = 4, used;
    unsigned size;
    unsigned char* tmp;
    if (p >= len - 20) {
      sprintf(err, "pack curto (%d de %d objetos)", i, nobj);
      return -1;
    }
    o->off = p;
    ch = data[p++];
    o->type = (ch >> 4) & 7;
    size = (unsigned)ch & 15u;
    while (ch & 0x80) {
      ch = data[p++];
      size |= (unsigned)(ch & 0x7F) << sh;
      sh += 7;
    }
    o->size = (int)size;
    o->base = -1;
    if (o->type == OBJ_OFS) {
      int back;
      ch = data[p++];
      back = ch & 0x7F;
      while (ch & 0x80) {
        ch = data[p++];
        back = ((back + 1) << 7) | (ch & 0x7F);
      }
      o->base = find_off(o->off - back, i);
      if (o->base < 0) {
        sprintf(err, "delta com base fora do pack");
        return -1;
      }
    } else if (o->type == OBJ_REF) {
      memcpy(o->ref, data + p, 20);
      p += 20;
    } else if (o->type < OBJ_COMMIT || o->type > OBJ_TAG) {
      sprintf(err, "objeto de tipo %d", o->type);
      return -1;
    }
    o->data = p;
    /* so para achar o fim do zlib */
    tmp = (unsigned char*)malloc(size ? size : 1);
    if (!tmp || zinflate(data + p, len - 20 - p, tmp, (int)size, &used) != (int)size) {
      free(tmp);
      sprintf(err, "zlib invalido no objeto %d", i);
      return -1;
    }
    if (o->type >= OBJ_COMMIT && o->type <= OBJ_TAG) {
      git_hash(type_name(o->type), tmp, (int)size, o->sha);
      o->hashed = 1;
      o->rtype = o->type;
      if (size <= 1024 * 1024) cache_put(i, tmp, (int)size);
      else free(tmp);
    } else {
      free(tmp);
    }
    p += used;
    if ((i & 255) == 0) printf("\r  indexando %d/%d", i, nobj);
  }
  /* 2) deltas; REF espera a base ter sha */
  do {
    pending = 0;
    done = 0;
    for (i = 0; i < nobj; i++) {
      struct pobj* o = &obj[i];
      unsigned char* b;
      int n;
      if (o->hashed) continue;
      if (o->type == OBJ_REF && o->base < 0) {
        o->base = lookup_slow(o->ref);
        if (o->base < 0) {
          pending++;
          continue;
        }
      }
      if (o->base >= 0 && !obj[o->base].hashed) {
        int k = o->base;
        while (k >= 0 && obj[k].type == OBJ_OFS) k = obj[k].base;
        if (k < 0 || (obj[k].type == OBJ_REF && obj[k].base < 0)) {
          pending++;
          continue;
        }
      }
      b = resolve(i, &n, 0);
      if (!b) {
        sprintf(err, "delta invalido no objeto %d", i);
        return -1;
      }
      git_hash(type_name(o->rtype), b, n, o->sha);
      o->hashed = 1;
      done++;
      if ((i & 255) == 0) printf("\r  deltas %d/%d   ", i, nobj);
    }
  } while (pending && done);
  printf("\r  %d objetos ok          \n", nobj);
  if (pending) {
    sprintf(err, "%d deltas sem base no pack", pending);
    return -1;
  }
  for (i = 0; i < nobj; i++) by_sha[i] = i;
  qsort(by_sha, (size_t)nobj, sizeof(int), sha_cmp);
  return nobj;
}

unsigned char* pack_get(const unsigned char* sha, int* type, int* size) {
  int i = lookup(sha);
  unsigned char* b;
  if (i < 0) return 0;
  b = resolve(i, size, 0);
  if (!b) return 0;
  *type = obj[i].rtype;
  return b;
}

void pack_free(void) {
  cache_clear();
  free(obj);
  free(by_sha);
  obj = 0;
  by_sha = 0;
  nobj = 0;
}
