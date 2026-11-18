/* ext4.c - Leitor ext2/3/4 so leitura. Ver ext4.h. */
#include <stdlib.h>
#include <string.h>
#include "ext4.h"

#define INCOMPAT_64BIT 0x80u
#define EXT4_EXTENTS_FL 0x80000u
#define EXT4_INLINE_DATA_FL 0x10000000u

struct ext4_fs {
  wsl_io io;
  uint32_t bsize;
  uint32_t inodes_per_group;
  uint32_t inode_size;
  uint32_t desc_size;
  uint64_t gdt_block;
  uint32_t groups;
  uint64_t blocks;
  uint8_t* gdt;
  uint8_t* scratch; /* um bloco */
  char label[17];
};

static int rd_blk(ext4_fs* fs, uint64_t blk, void* buf) {
  return wio_read(&fs->io, blk * fs->bsize, buf, fs->bsize);
}

ext4_fs* ext4_mount(const wsl_io* dev) {
  uint8_t sb[1024];
  ext4_fs* fs;
  uint32_t log, bpg, gdt_bytes;
  if (wio_read(dev, 1024, sb, 1024) || rd16(sb + 0x38) != 0xEF53) {
    wsl_seterr("ext4: sem superbloco", 0);
    return 0;
  }
  fs = (ext4_fs*)calloc(1, sizeof *fs);
  if (!fs) return 0;
  fs->io = *dev;
  log = rd32(sb + 0x18);
  if (log > 6) {
    wsl_seterr("ext4: bloco grande demais", 0);
    free(fs);
    return 0;
  }
  fs->bsize = 1024u << log;
  bpg = rd32(sb + 0x20);
  fs->inodes_per_group = rd32(sb + 0x28);
  fs->inode_size = rd32(sb + 0x4C) ? rd16(sb + 0x58) : 128;
  fs->blocks = rd32(sb + 0x04);
  if (rd32(sb + 0x60) & INCOMPAT_64BIT) {
    fs->desc_size = rd16(sb + 0xFE);
    fs->blocks |= (uint64_t)rd32(sb + 0x150) << 32;
  } else {
    fs->desc_size = 32;
  }
  if (fs->desc_size < 32) fs->desc_size = 32;
  if (!bpg || !fs->inodes_per_group || fs->inode_size < 128) {
    wsl_seterr("ext4: superbloco invalido", 0);
    free(fs);
    return 0;
  }
  fs->groups = (uint32_t)((fs->blocks - rd32(sb + 0x14) + bpg - 1) / bpg);
  fs->gdt_block = rd32(sb + 0x14) + 1;
  memcpy(fs->label, sb + 0x78, 16);
  fs->label[16] = 0;
  gdt_bytes = fs->groups * fs->desc_size;
  fs->gdt = (uint8_t*)malloc(gdt_bytes + fs->bsize);
  fs->scratch = (uint8_t*)malloc(fs->bsize);
  if (!fs->gdt || !fs->scratch ||
      wio_read(dev, fs->gdt_block * fs->bsize, fs->gdt, gdt_bytes)) {
    wsl_seterr("ext4: descritores de grupo ilegiveis", 0);
    ext4_umount(fs);
    return 0;
  }
  return fs;
}

void ext4_umount(ext4_fs* fs) {
  if (!fs) return;
  free(fs->gdt);
  free(fs->scratch);
  free(fs);
}

const char* ext4_label(ext4_fs* fs) { return fs->label; }
uint64_t ext4_bytes(ext4_fs* fs) { return fs->blocks * fs->bsize; }

int ext4_iget(ext4_fs* fs, uint32_t ino, ext4_inode* out) {
  uint32_t g, idx, n;
  const uint8_t* gd;
  uint64_t table;
  if (ino == 0) return -1;
  g = (ino - 1) / fs->inodes_per_group;
  idx = (ino - 1) % fs->inodes_per_group;
  if (g >= fs->groups) {
    wsl_seterr("ext4: inode fora do disco", 0);
    return -1;
  }
  gd = fs->gdt + (uint64_t)g * fs->desc_size;
  table = rd32(gd + 8);
  if (fs->desc_size >= 64) table |= (uint64_t)rd32(gd + 0x28) << 32;
  n = fs->inode_size < sizeof out->raw ? fs->inode_size : (uint32_t)sizeof out->raw;
  memset(out, 0, sizeof *out);
  if (wio_read(&fs->io, table * fs->bsize + (uint64_t)idx * fs->inode_size, out->raw, n)) {
    wsl_seterr("ext4: erro lendo inode", 0);
    return -1;
  }
  out->raw_len = (uint16_t)n;
  out->ino = ino;
  out->mode = rd16(out->raw);
  out->uid = rd16(out->raw + 2) | ((uint32_t)rd16(out->raw + 0x78) << 16);
  out->gid = rd16(out->raw + 0x18) | ((uint32_t)rd16(out->raw + 0x7A) << 16);
  out->size = rd32(out->raw + 4) | ((uint64_t)rd32(out->raw + 0x6C) << 32);
  out->mtime = rd32(out->raw + 0x10);
  out->flags = rd32(out->raw + 0x20);
  return 0;
}

/* Bloco fisico do bloco logico lb; 0 = buraco. <0 erro (em *err). */
static uint64_t map_extent(ext4_fs* fs, const uint8_t* node, uint32_t lb, int depth_left, int* err) {
  uint16_t n, depth, i;
  if (rd16(node) != 0xF30A || depth_left < 0) {
    *err = 1;
    wsl_seterr("ext4: arvore de extents invalida", 0);
    return 0;
  }
  n = rd16(node + 2);
  depth = rd16(node + 6);
  if (depth == 0) {
    for (i = 0; i < n; i++) {
      const uint8_t* e = node + 12 + i * 12;
      uint32_t first = rd32(e);
      uint32_t len = rd16(e + 4);
      int uninit = len > 32768;
      if (uninit) len -= 32768;
      if (lb >= first && lb < first + len) {
        uint64_t start = rd32(e + 8) | ((uint64_t)rd16(e + 6) << 32);
        return uninit ? 0 : start + (lb - first);
      }
    }
    return 0;
  } else {
    const uint8_t* best = 0;
    uint8_t* buf;
    uint64_t r;
    for (i = 0; i < n; i++) {
      const uint8_t* e = node + 12 + i * 12;
      if (rd32(e) <= lb) best = e;
      else break;
    }
    if (!best) return 0;
    buf = (uint8_t*)malloc(fs->bsize);
    if (!buf) {
      *err = 1;
      return 0;
    }
    if (rd_blk(fs, rd32(best + 4) | ((uint64_t)rd16(best + 8) << 32), buf)) {
      free(buf);
      *err = 1;
      return 0;
    }
    r = map_extent(fs, buf, lb, depth_left - 1, err);
    free(buf);
    return r;
  }
}

static uint64_t map_indirect(ext4_fs* fs, const uint8_t* iblock, uint32_t lb, int* err) {
  uint32_t per = fs->bsize / 4;
  uint32_t blk, level, idx[3];
  int lv, i;
  if (lb < 12) return rd32(iblock + lb * 4);
  lb -= 12;
  if (lb < per) {
    level = 1;
    blk = rd32(iblock + 12 * 4);
    idx[0] = lb;
  } else if ((lb -= per) < per * per) {
    level = 2;
    blk = rd32(iblock + 13 * 4);
    idx[0] = lb / per;
    idx[1] = lb % per;
  } else {
    lb -= per * per;
    level = 3;
    blk = rd32(iblock + 14 * 4);
    idx[0] = lb / (per * per);
    idx[1] = (lb / per) % per;
    idx[2] = lb % per;
  }
  for (lv = 0; lv < (int)level; lv++) {
    if (!blk) return 0;
    if (rd_blk(fs, blk, fs->scratch)) {
      *err = 1;
      return 0;
    }
    i = (int)idx[lv];
    blk = rd32(fs->scratch + i * 4);
  }
  return blk;
}

/* Valor do xattr system.data (resto dos dados inline) dentro do inode. */
static const uint8_t* inline_xattr(const ext4_inode* in, uint32_t* vlen) {
  uint32_t extra, base, off;
  if (in->raw_len <= 128) return 0;
  extra = rd16(in->raw + 0x80);
  base = 128 + extra;
  if (base + 4 > in->raw_len || rd32(in->raw + base) != 0xEA020000u) return 0;
  off = base + 4;
  while (off + 16 <= in->raw_len && rd32(in->raw + off) != 0) {
    uint8_t nl = in->raw[off], ni = in->raw[off + 1];
    uint16_t vo = rd16(in->raw + off + 2);
    uint32_t vs = rd32(in->raw + off + 8);
    if (ni == 7 && nl == 4 && !memcmp(in->raw + off + 16, "data", 4)) {
      if (base + 4 + vo + vs > in->raw_len) return 0;
      *vlen = vs;
      return in->raw + base + 4 + vo;
    }
    off += (16u + nl + 3u) & ~3u;
  }
  return 0;
}

int ext4_pread(ext4_fs* fs, const ext4_inode* in, uint64_t off, void* buf, uint32_t len) {
  uint8_t* p = (uint8_t*)buf;
  int err = 0;
  if (off > in->size || len > in->size - off) {
    wsl_seterr("ext4: leitura alem do fim", 0);
    return -1;
  }
  if (in->flags & EXT4_INLINE_DATA_FL) {
    uint32_t xl = 0, i;
    const uint8_t* x = inline_xattr(in, &xl);
    for (i = 0; i < len; i++) {
      uint64_t o = off + i;
      p[i] = o < 60 ? in->raw[0x28 + o] : (x && o - 60 < xl ? x[o - 60] : 0);
    }
    return 0;
  }
  while (len) {
    uint32_t lb = (uint32_t)(off / fs->bsize), in_off = (uint32_t)(off % fs->bsize);
    uint32_t take = fs->bsize - in_off;
    uint64_t pb;
    if (take > len) take = len;
    pb = (in->flags & EXT4_EXTENTS_FL) ? map_extent(fs, in->raw + 0x28, lb, 8, &err)
                                       : map_indirect(fs, in->raw + 0x28, lb, &err);
    if (err) return -1;
    if (!pb) {
      memset(p, 0, take);
    } else if (wio_read(&fs->io, pb * fs->bsize + in_off, p, take)) {
      wsl_seterr("ext4: erro de leitura", 0);
      return -1;
    }
    /* blocos contiguos seguintes: uma leitura so (bem mais rapido no NVMe) */
    p += take;
    off += take;
    len -= take;
  }
  return 0;
}

int ext4_readlink(ext4_fs* fs, const ext4_inode* in, char* out, int max) {
  uint64_t n = in->size;
  if ((in->mode & EXT4_S_IFMT) != EXT4_S_IFLNK || max < 1) return -1;
  if (n >= (uint64_t)max) n = (uint64_t)max - 1;
  if (in->size < 60 && !(in->flags & (EXT4_EXTENTS_FL | EXT4_INLINE_DATA_FL))) {
    memcpy(out, in->raw + 0x28, (size_t)n);
  } else if (ext4_pread(fs, in, 0, out, (uint32_t)n)) {
    return -1;
  }
  out[n] = 0;
  return (int)n;
}

int ext4_readdir(ext4_fs* fs, const ext4_inode* dir, ext4_dir_cb cb, void* arg) {
  uint64_t off;
  uint8_t* blk;
  char name[256];
  if ((dir->mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
    wsl_seterr("ext4: nao e pasta", 0);
    return -1;
  }
  if (dir->flags & EXT4_INLINE_DATA_FL) {
    /* pasta inline: 4 bytes do inode pai e entradas depois */
    uint32_t total = (uint32_t)dir->size, o = 4;
    uint8_t* d = (uint8_t*)malloc(total + 1);
    int rc = 0;
    if (!d || ext4_pread(fs, dir, 0, d, total)) {
      free(d);
      return -1;
    }
    if ((rc = cb(arg, "..", rd32(d), 2)) != 0) {
      free(d);
      return rc;
    }
    while (o + 8 <= total) {
      uint32_t ino = rd32(d + o);
      uint16_t rl = rd16(d + o + 4);
      uint8_t nl = d[o + 6];
      if (rl < 8 || o + rl > total) break;
      if (ino && nl && o + 8 + nl <= total) {
        memcpy(name, d + o + 8, nl);
        name[nl] = 0;
        if ((rc = cb(arg, name, ino, d[o + 7])) != 0) break;
      }
      o += rl;
    }
    free(d);
    return rc;
  }
  blk = (uint8_t*)malloc(fs->bsize);
  if (!blk) return -1;
  for (off = 0; off < dir->size; off += fs->bsize) {
    uint32_t o = 0;
    if (ext4_pread(fs, dir, off, blk, fs->bsize)) {
      free(blk);
      return -1;
    }
    while (o + 8 <= fs->bsize) {
      uint32_t ino = rd32(blk + o);
      uint16_t rl = rd16(blk + o + 4);
      uint8_t nl = blk[o + 6];
      if (rl < 8 || o + rl > fs->bsize) break;
      if (ino && nl && o + 8 + nl <= fs->bsize && blk[o + 7] != 0xDE) {
        int rc;
        memcpy(name, blk + o + 8, nl);
        name[nl] = 0;
        if ((rc = cb(arg, name, ino, blk[o + 7])) != 0) {
          free(blk);
          return rc;
        }
      }
      o += rl;
    }
  }
  free(blk);
  return 0;
}

struct find_acc {
  const char* name;
  uint32_t ino;
};

static int find_cb(void* arg, const char* name, uint32_t ino, int type) {
  struct find_acc* f = (struct find_acc*)arg;
  (void)type;
  if (strcmp(name, f->name) == 0) {
    f->ino = ino;
    return 1;
  }
  return 0;
}

static int lookup_rec(ext4_fs* fs, const char* path, int follow, ext4_inode* out, int depth) {
  ext4_inode cur, parent;
  char comp[256];
  if (depth > 40) {
    wsl_seterr("ext4: links demais", 0);
    return -1;
  }
  if (ext4_iget(fs, 2, &cur)) return -1;
  parent = cur;
  while (*path) {
    int n = 0;
    struct find_acc f;
    while (*path == '/') path++;
    if (!*path) break;
    while (*path && *path != '/' && n < 255) comp[n++] = *path++;
    comp[n] = 0;
    f.name = comp;
    f.ino = 0;
    if (ext4_readdir(fs, &cur, find_cb, &f) < 0) return -1;
    if (!f.ino) {
      wsl_seterr("ext4: nao achei ", comp);
      return -1;
    }
    parent = cur;
    if (ext4_iget(fs, f.ino, &cur)) return -1;
    if ((cur.mode & EXT4_S_IFMT) == EXT4_S_IFLNK && (follow || *path)) {
      /* resolve o alvo e continua com o resto do caminho */
      char tgt[1024], full[2048];
      int tl = ext4_readlink(fs, &cur, tgt, sizeof tgt);
      size_t rl = strlen(path);
      if (tl < 0 || (size_t)tl + rl + 2 > sizeof full) return -1;
      if (tgt[0] == '/') {
        memcpy(full, tgt, (size_t)tl);
        memcpy(full + tl, path, rl + 1);
        return lookup_rec(fs, full, follow, out, depth + 1);
      } else {
        /* relativo: subimos ate a pasta pai pelo "..": refazemos pelo
         * caminho percorrido, mais simples guardar o prefixo */
        (void)parent;
        return -2; /* tratado por ext4_lookup, que conhece o prefixo */
      }
    }
  }
  *out = cur;
  return 0;
}

/* Normaliza "a/b/../c/./d" (sem links) para "/a/c/d". */
static void normalize(char* p) {
  char out[2048];
  size_t o = 0;
  const char* s = p;
  while (*s) {
    const char* e;
    size_t n;
    while (*s == '/') s++;
    if (!*s) break;
    e = s;
    while (*e && *e != '/') e++;
    n = (size_t)(e - s);
    if (n == 1 && s[0] == '.') {
    } else if (n == 2 && s[0] == '.' && s[1] == '.') {
      while (o > 0 && out[o - 1] != '/') o--;
      if (o > 0) o--;
    } else if (o + n + 2 < sizeof out) {
      out[o++] = '/';
      memcpy(out + o, s, n);
      o += n;
    }
    s = e;
  }
  if (o == 0) out[o++] = '/';
  out[o] = 0;
  memcpy(p, out, o + 1);
}

/* Anda componente a componente, guardando o prefixo para links relativos. */
int ext4_lookup(ext4_fs* fs, const char* path, int follow, ext4_inode* out) {
  char cur[2048], rest[2048];
  int hops = 0;
  size_t pl = strlen(path);
  if (pl + 2 > sizeof rest) return -1;
  cur[0] = 0;
  rest[0] = '/';
  memcpy(rest + 1, path, pl + 1);
  normalize(rest);
  for (;;) {
    char* r = rest;
    char comp[256];
    int n = 0;
    ext4_inode in;
    size_t cl;
    while (*r == '/') r++;
    if (!*r) break;
    while (*r && *r != '/' && n < 255) comp[n++] = *r++;
    comp[n] = 0;
    cl = strlen(cur);
    if (cl + n + 2 >= sizeof cur) return -1;
    cur[cl] = '/';
    memcpy(cur + cl + 1, comp, (size_t)n + 1);
    if (lookup_rec(fs, cur, 0, &in, 0)) return -1;
    if ((in.mode & EXT4_S_IFMT) == EXT4_S_IFLNK && (follow || *r)) {
      char tgt[1024], nr[2048];
      int tl = ext4_readlink(fs, &in, tgt, sizeof tgt);
      if (tl < 0 || ++hops > 40) {
        wsl_seterr("ext4: link invalido ou em ciclo", 0);
        return -1;
      }
      cur[cl] = 0; /* pasta do link */
      if (tgt[0] == '/') {
        if ((size_t)tl + strlen(r) + 2 > sizeof nr) return -1;
        memcpy(nr, tgt, (size_t)tl);
        strcpy(nr + tl, r);
      } else {
        if (cl + (size_t)tl + strlen(r) + 4 > sizeof nr) return -1;
        memcpy(nr, cur, cl);
        nr[cl] = '/';
        memcpy(nr + cl + 1, tgt, (size_t)tl);
        strcpy(nr + cl + 1 + tl, r);
      }
      normalize(nr);
      strcpy(rest, nr);
      cur[0] = 0;
      continue;
    }
    memmove(rest, r, strlen(r) + 1);
  }
  if (!cur[0]) return ext4_iget(fs, 2, out);
  return lookup_rec(fs, cur, 0, out, 0);
}
