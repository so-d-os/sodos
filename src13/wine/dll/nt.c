/*
 * nt.c - O que o libuv (Node.js dentro do Electron) pede direto a ntdll para
 * arquivos: NtQueryInformationFile (fs.stat usa FileAllInformation),
 * NtQueryVolumeInformationFile (numero do volume, tipo de dispositivo) e
 * NtQueryDirectoryFile (fs.readdir). Montados sobre GetFileInformationByHandle
 * e FindFirstFile, com os layouts exatos das estruturas do Windows (x64).
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

#define STATUS_SUCCESS_ 0
#define STATUS_INVALID_HANDLE_ ((long)0xC0000008)
#define STATUS_INVALID_INFO_CLASS_ ((long)0xC0000003)
#define STATUS_INFO_LENGTH_MISMATCH_ ((long)0xC0000004)
#define STATUS_BUFFER_OVERFLOW_ ((long)0x80000005)
#define STATUS_NO_MORE_FILES_ ((long)0x80000006)
#define STATUS_NO_SUCH_FILE_ ((long)0xC000000F)
#define VOLUME_SERIAL 0x50D05000u

struct iosb {
  uintptr_t status;
  uintptr_t info;
};
struct by_handle_info {
  DWORD attrs;
  uint64_t ctime, atime, mtime;
  DWORD serial, size_high, size_low, nlinks, idx_high, idx_low;
} __attribute__((packed));

static int handle_info(HANDLE h, struct by_handle_info* bi) {
  typedef BOOL(WINAPI * gfi)(HANDLE, struct by_handle_info*);
  gfi f = (gfi)builtin_lookup("GetFileInformationByHandle");
  return f && f(h, bi);
}
static int64_t handle_pos(HANDLE h) {
  typedef BOOL(WINAPI * sfp)(HANDLE, int64_t, int64_t*, DWORD);
  sfp f = (sfp)builtin_lookup("SetFilePointerEx");
  int64_t cur = 0;
  if (f) f(h, 0, &cur, 1);
  return cur;
}
static void done(struct iosb* io, long st, uintptr_t n) {
  if (io) {
    io->status = (uintptr_t)(intptr_t)st;
    io->info = n;
  }
}

/* FILE_BASIC_INFORMATION (40) */
static void put_basic(uint8_t* o, const struct by_handle_info* bi) {
  memset(o, 0, 40);
  memcpy(o, &bi->ctime, 8);
  memcpy(o + 8, &bi->atime, 8);
  memcpy(o + 16, &bi->mtime, 8);
  memcpy(o + 24, &bi->mtime, 8);
  memcpy(o + 32, &bi->attrs, 4);
}
/* FILE_STANDARD_INFORMATION (24) */
static void put_standard(uint8_t* o, const struct by_handle_info* bi) {
  uint64_t size = ((uint64_t)bi->size_high << 32) | bi->size_low, alloc = (size + 4095) & ~4095ull;
  memset(o, 0, 24);
  memcpy(o, &alloc, 8);
  memcpy(o + 8, &size, 8);
  *(DWORD*)(o + 16) = bi->nlinks ? bi->nlinks : 1;
  o[20] = 0;                         /* DeletePending */
  o[21] = (bi->attrs & 0x10) != 0;   /* Directory */
}

long WINAPI wNtQueryInformationFile(HANDLE h, struct iosb* io, void* buf, DWORD len, int cls) {
  struct by_handle_info bi;
  uint8_t* o = (uint8_t*)buf;
  if (!handle_info(h, &bi)) {
    done(io, STATUS_INVALID_HANDLE_, 0);
    return STATUS_INVALID_HANDLE_;
  }
  switch (cls) {
    case 4: /* FileBasicInformation */
      if (len < 40) return STATUS_INFO_LENGTH_MISMATCH_;
      put_basic(o, &bi);
      done(io, 0, 40);
      return 0;
    case 5: /* FileStandardInformation */
      if (len < 24) return STATUS_INFO_LENGTH_MISMATCH_;
      put_standard(o, &bi);
      done(io, 0, 24);
      return 0;
    case 6: /* FileInternalInformation */
      if (len < 8) return STATUS_INFO_LENGTH_MISMATCH_;
      *(uint64_t*)o = ((uint64_t)bi.idx_high << 32) | bi.idx_low;
      done(io, 0, 8);
      return 0;
    case 14: /* FilePositionInformation */
      if (len < 8) return STATUS_INFO_LENGTH_MISMATCH_;
      *(int64_t*)o = handle_pos(h);
      done(io, 0, 8);
      return 0;
    case 16: /* FileModeInformation */
    case 17: /* FileAlignmentInformation */
      if (len < 4) return STATUS_INFO_LENGTH_MISMATCH_;
      *(DWORD*)o = 0;
      done(io, 0, 4);
      return 0;
    case 18: { /* FileAllInformation: basic, standard, internal, ea, access, position, mode, alignment, nome */
      if (len < 104) return STATUS_INFO_LENGTH_MISMATCH_;
      memset(o, 0, len);
      put_basic(o, &bi);
      put_standard(o + 40, &bi);
      *(uint64_t*)(o + 64) = ((uint64_t)bi.idx_high << 32) | bi.idx_low;
      *(DWORD*)(o + 72) = 0;          /* EaSize */
      *(DWORD*)(o + 76) = 0x1F01FF;   /* AccessFlags */
      *(int64_t*)(o + 80) = handle_pos(h);
      *(DWORD*)(o + 88) = 0;          /* Mode */
      *(DWORD*)(o + 92) = 0;          /* AlignmentRequirement */
      *(DWORD*)(o + 96) = 0;          /* FileNameLength: o nome nao e devolvido */
      done(io, STATUS_BUFFER_OVERFLOW_, 104);
      return STATUS_BUFFER_OVERFLOW_; /* aviso (como o Windows sem espaco para o nome) */
    }
    case 35: /* FileAttributeTagInformation */
      if (len < 8) return STATUS_INFO_LENGTH_MISMATCH_;
      *(DWORD*)o = bi.attrs;
      *(DWORD*)(o + 4) = 0;
      done(io, 0, 8);
      return 0;
    case 34: /* FileNetworkOpenInformation (56) */
      if (len < 56) return STATUS_INFO_LENGTH_MISMATCH_;
      memset(o, 0, 56);
      memcpy(o, &bi.ctime, 8);
      memcpy(o + 8, &bi.atime, 8);
      memcpy(o + 16, &bi.mtime, 8);
      memcpy(o + 24, &bi.mtime, 8);
      {
        uint64_t size = ((uint64_t)bi.size_high << 32) | bi.size_low, alloc = (size + 4095) & ~4095ull;
        memcpy(o + 32, &alloc, 8);
        memcpy(o + 40, &size, 8);
      }
      *(DWORD*)(o + 48) = bi.attrs;
      done(io, 0, 56);
      return 0;
  }
  done(io, STATUS_INVALID_INFO_CLASS_, 0);
  return STATUS_INVALID_INFO_CLASS_;
}

long WINAPI wNtQueryVolumeInformationFile(HANDLE h, struct iosb* io, void* buf, DWORD len, int cls) {
  uint8_t* o = (uint8_t*)buf;
  unsigned total = 0, freeb = 0;
  (void)h;
  switch (cls) {
    case 1: /* FileFsVolumeInformation: tempo, serial, tam. do rotulo, objetos, rotulo */
      if (len < 24) return STATUS_INFO_LENGTH_MISMATCH_;
      memset(o, 0, 24);
      *(DWORD*)(o + 8) = VOLUME_SERIAL;
      done(io, 0, 18);
      return 0;
    case 3: /* FileFsSizeInformation (24) */
    case 7: /* FileFsFullSizeInformation (32) */
      if (len < (cls == 3 ? 24u : 32u)) return STATUS_INFO_LENGTH_MISMATCH_;
      k_meminfo(&total, &freeb);
      memset(o, 0, cls == 3 ? 24 : 32);
      *(uint64_t*)o = 4u << 20;           /* unidades de alocacao: 16 GB / 4 KB */
      *(uint64_t*)(o + 8) = 2u << 20;
      if (cls == 7) *(uint64_t*)(o + 16) = 2u << 20;
      *(DWORD*)(o + (cls == 3 ? 16 : 24)) = 8;   /* setores por unidade */
      *(DWORD*)(o + (cls == 3 ? 20 : 28)) = 512; /* bytes por setor */
      done(io, 0, cls == 3 ? 24 : 32);
      return 0;
    case 4: /* FileFsDeviceInformation: FILE_DEVICE_DISK */
      if (len < 8) return STATUS_INFO_LENGTH_MISMATCH_;
      *(DWORD*)o = 7;
      *(DWORD*)(o + 4) = 0;
      done(io, 0, 8);
      return 0;
    case 5: { /* FileFsAttributeInformation: atributos, nome max, tamanho e nome do FS */
      static const wchar16 nm[] = {'F', 'A', 'T', '3', '2'};
      if (len < 12 + sizeof(nm)) return STATUS_INFO_LENGTH_MISMATCH_;
      *(DWORD*)o = 0x3;   /* CASE_PRESERVED_NAMES | CASE_SENSITIVE_SEARCH(nao) | UNICODE_ON_DISK */
      *(DWORD*)(o + 4) = 255;
      *(DWORD*)(o + 8) = sizeof(nm);
      memcpy(o + 12, nm, sizeof(nm));
      done(io, 0, 12 + sizeof(nm));
      return 0;
    }
  }
  done(io, STATUS_INVALID_INFO_CLASS_, 0);
  return STATUS_INVALID_INFO_CLASS_;
}

/* ---------- NtQueryDirectoryFile ---------- */
struct find_w {
  DWORD attr;
  uint64_t ctime, atime, mtime;
  DWORD hi, lo, r0, r1;
  wchar16 name[260];
  wchar16 alt[14];
} __attribute__((packed));
struct dirscan {
  HANDLE find;
  int have, done;
  struct find_w fd;
};

long WINAPI wNtQueryDirectoryFile(HANDLE h, HANDLE ev, void* apc, void* ctx, struct iosb* io, void* buf, DWORD len,
                                  int cls, BOOL single, void* mask, BOOL restart) {
  typedef HANDLE(WINAPI * ff_fn)(const wchar16*, struct find_w*);
  typedef BOOL(WINAPI * fn_fn)(HANDLE, struct find_w*);
  typedef BOOL(WINAPI * fc_fn)(HANDLE);
  struct wobj* d = wh_get(h, OBJ_FILE);
  struct dirscan* ds;
  uint8_t* o = (uint8_t*)buf;
  DWORD used = 0, prev = 0;
  int count = 0;
  (void)ev; (void)apc; (void)ctx; (void)mask;
  if (!d || !d->is_dir || !d->path) {
    done(io, STATUS_INVALID_HANDLE_, 0);
    return STATUS_INVALID_HANDLE_;
  }
  if (cls != 1 && cls != 2 && cls != 3 && cls != 37 && cls != 38) {
    done(io, STATUS_INVALID_INFO_CLASS_, 0);
    return STATUS_INVALID_INFO_CLASS_;
  }
  ds = (struct dirscan*)d->p2;
  if (ds && restart) {
    fc_fn fc = (fc_fn)builtin_lookup("FindClose");
    if (ds->find && fc) fc(ds->find);
    k_free(ds);
    ds = 0;
    d->p2 = 0;
  }
  if (!ds) {
    ff_fn ff = (ff_fn)builtin_lookup("FindFirstFileW");
    char pat[300];
    wchar16* w;
    ds = (struct dirscan*)wzalloc(sizeof(*ds));
    if (!ds) return (long)0xC0000017;
    strncpy(pat, d->path, sizeof(pat) - 3);
    pat[sizeof(pat) - 3] = 0;
    if (pat[0] && pat[strlen(pat) - 1] != '\\') strcat(pat, "\\");
    strcat(pat, "*");
    w = wdup8to16(pat);
    ds->find = (w && ff) ? ff(w, &ds->fd) : 0;
    k_free(w);
    if (!ds->find || ds->find == (HANDLE)(intptr_t)-1) {
      ds->find = 0;
      ds->done = 1;
    } else {
      ds->have = 1;
    }
    d->p2 = ds;
  }
  for (;;) {
    DWORD nlen, hdr, rec;
    uint64_t size;
    if (!ds->have) {
      fn_fn fnx = (fn_fn)builtin_lookup("FindNextFileW");
      if (ds->done || !ds->find || !fnx || !fnx(ds->find, &ds->fd)) {
        ds->done = 1;
        break;
      }
      ds->have = 1;
    }
    nlen = (DWORD)wstrlen16(ds->fd.name) * 2;
    /* cabecalhos: Directory 64, FullDirectory 68, BothDirectory 94, IdBoth 104, IdFull 80 */
    hdr = cls == 1 ? 64 : cls == 2 ? 68 : cls == 3 ? 94 : cls == 37 ? 104 : 80;
    rec = (hdr + nlen + 7) & ~7u;
    if (used + hdr + nlen > len) {
      if (!count) {
        done(io, STATUS_BUFFER_OVERFLOW_, 0);
        return STATUS_BUFFER_OVERFLOW_;
      }
      break;
    }
    memset(o + used, 0, hdr);
    size = ((uint64_t)ds->fd.hi << 32) | ds->fd.lo;
    *(uint64_t*)(o + used + 8) = ds->fd.ctime;
    *(uint64_t*)(o + used + 16) = ds->fd.atime;
    *(uint64_t*)(o + used + 24) = ds->fd.mtime;
    *(uint64_t*)(o + used + 32) = ds->fd.mtime;
    *(uint64_t*)(o + used + 40) = size;
    *(uint64_t*)(o + used + 48) = (size + 4095) & ~4095ull;
    *(DWORD*)(o + used + 56) = ds->fd.attr;
    *(DWORD*)(o + used + 60) = nlen;
    memcpy(o + used + hdr, ds->fd.name, nlen);
    if (count) *(DWORD*)(o + prev) = used - prev; /* NextEntryOffset do anterior */
    prev = used;
    used += rec;
    count++;
    ds->have = 0;
    if (single) break;
  }
  if (!count) {
    done(io, STATUS_NO_MORE_FILES_, 0);
    return STATUS_NO_MORE_FILES_;
  }
  done(io, 0, used);
  return 0;
}

long wfile_setinfo(HANDLE h, int cls, const void* buf, DWORD len); /* file.c */
long WINAPI wNtSetInformationFile(HANDLE h, struct iosb* io, const void* buf, DWORD len, int cls) {
  long st = wfile_setinfo(h, cls, buf, len);
  done(io, st, st == 0 ? len : 0);
  return st;
}

#define S(n) {#n, (void*)n}
const struct wine_sym nt_syms[] = {{"NtSetInformationFile", (void*)wNtSetInformationFile},
                                   {"ZwSetInformationFile", (void*)wNtSetInformationFile},{"NtQueryInformationFile", (void*)wNtQueryInformationFile},
                                   {"ZwQueryInformationFile", (void*)wNtQueryInformationFile},
                                   {"NtQueryVolumeInformationFile", (void*)wNtQueryVolumeInformationFile},
                                   {"ZwQueryVolumeInformationFile", (void*)wNtQueryVolumeInformationFile},
                                   {"NtQueryDirectoryFile", (void*)wNtQueryDirectoryFile},
                                   {"ZwQueryDirectoryFile", (void*)wNtQueryDirectoryFile},
                                   {0, 0}};
