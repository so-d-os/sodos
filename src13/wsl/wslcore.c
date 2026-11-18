/* wslcore.c - Ver wslcore.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wslcore.h"

/* ---------------- disco cru ---------------- */

typedef struct {
  sector_read_fn fn;
  void* ctx;
  uint8_t* bounce; /* 64 KB */
} disk_ctx;

static int disk_read(void* c, uint64_t off, void* buf, uint32_t len) {
  disk_ctx* d = (disk_ctx*)c;
  uint8_t* p = (uint8_t*)buf;
  while (len) {
    uint64_t lba = off / 512;
    uint32_t in = (uint32_t)(off % 512);
    if (in == 0 && len >= 512) {
      /* direto no buffer, ate 128 setores por vez */
      uint32_t n = len / 512;
      if (n > 128) n = 128;
      if (d->fn(d->ctx, lba, n, p)) return -1;
      p += n * 512;
      off += n * 512;
      len -= n * 512;
    } else {
      uint32_t take = 512 - in;
      if (take > len) take = len;
      if (d->fn(d->ctx, lba, 1, d->bounce)) return -1;
      memcpy(p, d->bounce + in, take);
      p += take;
      off += take;
      len -= take;
    }
  }
  return 0;
}

void wsl_disk_io(wsl_io* out, sector_read_fn fn, void* ctx, uint64_t sectors) {
  disk_ctx* d = (disk_ctx*)calloc(1, sizeof *d);
  d->fn = fn;
  d->ctx = ctx;
  d->bounce = (uint8_t*)malloc(512);
  out->read = disk_read;
  out->ctx = d;
  out->size = sectors * 512;
}

static int sub_read(void* c, uint64_t off, void* buf, uint32_t len) {
  wsl_sub* s = (wsl_sub*)c;
  return wio_read(&s->base, s->off + off, buf, len);
}

void wsl_sub_io(wsl_sub* s, const wsl_io* base, uint64_t off, uint64_t size, wsl_io* out) {
  s->base = *base;
  s->off = off;
  out->read = sub_read;
  out->ctx = s;
  out->size = size;
}

/* ---------------- particoes ---------------- */

static int is_ntfs_at(const wsl_io* disk, uint64_t off) {
  uint8_t b[512];
  return wio_read(disk, off, b, 512) == 0 && ntfs_probe(b);
}

int wsl_ntfs_parts(const wsl_io* disk, wsl_part* out, int max) {
  uint8_t b[512];
  int n = 0, i;
  if (wio_read(disk, 512, b, 512)) return 0;
  if (!memcmp(b, "EFI PART", 8)) {
    uint64_t ent = rd64(b + 72);
    uint32_t cnt = rd32(b + 80), esz = rd32(b + 84), k;
    uint8_t e[512];
    if (esz < 128 || esz > 512 || cnt > 1024) return 0;
    for (k = 0; k < cnt && n < max; k++) {
      uint64_t first, last;
      if (wio_read(disk, ent * 512 + (uint64_t)k * esz, e, 128)) break;
      first = rd64(e + 32);
      last = rd64(e + 40);
      if (!first || last < first) continue;
      if (is_ntfs_at(disk, first * 512)) {
        out[n].off = first * 512;
        out[n].size = (last - first + 1) * 512;
        out[n].index = (int)k + 1;
        n++;
      }
    }
    return n;
  }
  if (wio_read(disk, 0, b, 512) || b[510] != 0x55 || b[511] != 0xAA) return 0;
  /* MBR (so primarias) ou NTFS sem tabela */
  if (ntfs_probe(b)) {
    out[0].off = 0;
    out[0].size = disk->size;
    out[0].index = 0;
    return 1;
  }
  for (i = 0; i < 4 && n < max; i++) {
    const uint8_t* e = b + 446 + i * 16;
    uint64_t first = rd32(e + 8), cnt = rd32(e + 12);
    if (!cnt || e[4] == 0xEE) continue;
    if (is_ntfs_at(disk, first * 512)) {
      out[n].off = first * 512;
      out[n].size = cnt * 512;
      out[n].index = i + 1;
      n++;
    }
  }
  return n;
}

/* ---------------- distros ---------------- */

#define MAXN 96
typedef struct {
  char names[MAXN][64];
  int dirs[MAXN];
  int n;
} name_list;

static int collect(void* arg, const char* name, int isdir, uint64_t size) {
  name_list* l = (name_list*)arg;
  (void)size;
  if (l->n >= MAXN || name[0] == '$' || strlen(name) >= 64) return 0;
  if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
  strcpy(l->names[l->n], name);
  l->dirs[l->n] = isdir;
  l->n++;
  return 0;
}

static int add_if_vhdx(ntfs_vol* v, const char* dirpath, const char* name, const char* user,
                       wsl_distro* out, int* n, int max) {
  char p[400];
  ntfs_file* f;
  if (*n >= max || strlen(dirpath) + 12 >= sizeof p) return 0;
  strcpy(p, dirpath);
  strcat(p, "\\ext4.vhdx");
  f = ntfs_open(v, p);
  if (!f) return 0;
  strncpy(out[*n].name, name, 63);
  out[*n].name[63] = 0;
  strncpy(out[*n].user, user, 63);
  out[*n].user[63] = 0;
  strcpy(out[*n].path, p);
  out[*n].size = ntfs_fsize(f);
  ntfs_close(f);
  (*n)++;
  return 1;
}

int wsl_find_distros(ntfs_vol* v, wsl_distro* out, int max) {
  static name_list users, sub;
  char p[400];
  int n = 0, u, k;
  users.n = 0;
  if (ntfs_list(v, "Users", collect, &users) < 0) return 0;
  for (u = 0; u < users.n && n < max; u++) {
    if (!users.dirs[u]) continue;
    /* WSL novo: AppData\Local\wsl\{guid}\ext4.vhdx */
    sub.n = 0;
    snprintf(p, sizeof p, "Users\\%s\\AppData\\Local\\wsl", users.names[u]);
    if (ntfs_list(v, p, collect, &sub) == 0) {
      for (k = 0; k < sub.n && n < max; k++) {
        char d[520];
        snprintf(d, sizeof d, "%s\\%s", p, sub.names[k]);
        add_if_vhdx(v, d, sub.names[k], users.names[u], out, &n, max);
      }
    }
    /* Loja: AppData\Local\Packages\CanonicalGroupLimited...\LocalState */
    sub.n = 0;
    snprintf(p, sizeof p, "Users\\%s\\AppData\\Local\\Packages", users.names[u]);
    if (ntfs_list(v, p, collect, &sub) == 0) {
      for (k = 0; k < sub.n && n < max; k++) {
        char d[520];
        snprintf(d, sizeof d, "%s\\%s\\LocalState", p, sub.names[k]);
        add_if_vhdx(v, d, sub.names[k], users.names[u], out, &n, max);
      }
    }
  }
  return n;
}

const char* wsl_find_kernel(ntfs_vol* v) {
  static const char* cands[] = {"Program Files\\WSL\\tools\\kernel",
                                "Windows\\System32\\lxss\\tools\\kernel", 0};
  int i;
  for (i = 0; cands[i]; i++) {
    ntfs_file* f = ntfs_open(v, cands[i]);
    if (f) {
      ntfs_close(f);
      return cands[i];
    }
  }
  return 0;
}

/* ---------------- pilha ---------------- */

static int name_match(const wsl_distro* d, const char* which) {
  const char* a = d->name;
  /* aceita o nome inteiro ou um prefixo (ex.: "{b4b8" ou "Canonical") */
  while (*which) {
    char x = *a++, y = *which++;
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
  }
  return 1;
}

int wsl_open_stack(const wsl_io* disk, const char* which, wsl_stack* st, wsl_distro* info) {
  wsl_part parts[WSL_MAX_PARTS];
  int np = wsl_ntfs_parts(disk, parts, WSL_MAX_PARTS), i, k;
  memset(st, 0, sizeof *st);
  if (np == 0) {
    wsl_seterr("nenhuma particao NTFS no disco", 0);
    return -1;
  }
  for (i = 0; i < np; i++) {
    wsl_distro ds[WSL_MAX_DISTROS];
    int nd;
    wsl_sub_io(&st->part_sub, disk, parts[i].off, parts[i].size, &st->part);
    st->ntfs = ntfs_mount(&st->part);
    if (!st->ntfs) continue;
    nd = wsl_find_distros(st->ntfs, ds, WSL_MAX_DISTROS);
    for (k = 0; k < nd; k++) {
      if (which && *which && !name_match(&ds[k], which)) continue;
      st->file = ntfs_open(st->ntfs, ds[k].path);
      if (!st->file) continue;
      ntfs_file_io(st->file, &st->file_io);
      st->vd = vhdx_open(&st->file_io);
      if (!st->vd) {
        ntfs_close(st->file);
        st->file = 0;
        continue;
      }
      vhdx_io(st->vd, &st->disk);
      st->fs = ext4_mount(&st->disk); /* pode falhar: a VM ainda sobe */
      if (info) *info = ds[k];
      return 0;
    }
    ntfs_umount(st->ntfs);
    st->ntfs = 0;
  }
  if (!wsl_err[0]) wsl_seterr("nenhuma distro WSL (ext4.vhdx) achada", 0);
  return -1;
}

void wsl_close_stack(wsl_stack* st) {
  if (st->fs) ext4_umount(st->fs);
  if (st->vd) vhdx_close(st->vd);
  if (st->file) ntfs_close(st->file);
  if (st->ntfs) ntfs_umount(st->ntfs);
  memset(st, 0, sizeof *st);
}
