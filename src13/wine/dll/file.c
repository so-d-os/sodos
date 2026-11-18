/*
 * file.c - Arquivos e pastas do Windows sobre a API do SODOS.
 *
 * O SODOS le por trecho (k_fread) mas grava o arquivo inteiro (k_fsave).
 * Entao: handle so de leitura le direto do disco; handle com escrita mantem
 * o conteudo em memoria e grava no FlushFileBuffers/CloseHandle.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

#define H_STDIN ((HANDLE)(uintptr_t)0x10)
#define H_STDOUT ((HANDLE)(uintptr_t)0x14)
#define H_STDERR ((HANDLE)(uintptr_t)0x18)

#define ATTR_DIR 0x10u
#define ATTR_ARCHIVE 0x20u
#define ATTR_NORMAL 0x80u
#define INVALID_ATTRS 0xFFFFFFFFu

uint64_t wfiletime_now(void);

/* ---------- caminhos ---------- */
char* wpath(const char* in, char* out, int max) {
  int i = 0, o = 0;
  if (!in) in = "";
  /* \\?\C:\x  e  \??\C:\x */
  if ((in[0] == '\\' && in[1] == '\\' && in[2] == '?' && in[3] == '\\') ||
      (in[0] == '\\' && in[1] == '?' && in[2] == '?' && in[3] == '\\'))
    i = 4;
  for (; in[i] && o < max - 1; i++) {
    char c = in[i] == '/' ? '\\' : in[i];
    /* barras repetidas (menos no comeco) */
    if (c == '\\' && o > 0 && out[o - 1] == '\\') continue;
    out[o++] = c;
  }
  /* "D:\PASTA\" -> "D:\PASTA" (a raiz fica) */
  while (o > 3 && out[o - 1] == '\\') o--;
  out[o] = 0;
  return out;
}

char* wpath16(const wchar16* in, char* out, int max) {
  char tmp[400];
  int n = in ? wutf16_to_8(in, -1, tmp, (int)sizeof(tmp) - 1) : 0;
  tmp[n] = 0;
  return wpath(tmp, out, max);
}

/* ---------- temporarios grandes em RAM ----------
 * O SODOS grava o arquivo inteiro de uma vez (k_fsave) e o disco e lento:
 * um download de 300 MB levaria minutos para ir a pasta TEMP so para ser
 * lido de volta pelo mesmo programa. Arquivos de RAM_MIN bytes ou mais em
 * D:\WINE\TEMP ficam em memoria ao fechar; abrir, mover, apagar e consultar
 * funcionam igual, e eles somem quando o processo termina (sao temporarios).
 * Movidos para fora de TEMP, vao para o disco. */
#define RAM_MIN (4u << 20)
const char* wine_dir(const char* rel);

static int path_eq(const char* a, const char* b) { return wstreq(a, b); }
static int is_temp_path(const char* path) {
  static const char pre[] = "D:\\WINE\\TEMP\\";
  int i;
  for (i = 0; pre[i]; i++) {
    char a = path[i], b = pre[i];
    if (a >= 'a' && a <= 'z') a = (char)(a - 32);
    if (a != b) return 0;
  }
  return 1;
}
/* A lista e do DLL, nao do processo: o instalador baixa o pacote e passa o
 * caminho para um processo filho (ele mesmo, "elevado"), que precisa abrir o
 * mesmo arquivo. A memoria e do processo que gravou e vale enquanto ele vive
 * (o pai espera o filho); wfile_proc_exit tira as entradas dele da lista. */
static struct ramfile* ram_list;
static volatile int ram_lock_word;
static void ram_lock(void) {
  while (__sync_lock_test_and_set(&ram_lock_word, 1)) k_thread_yield();
}
static void ram_unlock(void) { __sync_lock_release(&ram_lock_word); }

static struct ramfile* ram_find(const char* path) {
  struct ramfile* r;
  for (r = ram_list; r; r = r->next)
    if (!r->dead && path_eq(r->path, path)) return r;
  return 0;
}
static void ram_drop(struct ramfile* r) {
  struct ramfile** pp;
  if (r->users > 0) {
    r->dead = 1;   /* some quando o ultimo handle fechar */
    return;
  }
  for (pp = &ram_list; *pp; pp = &(*pp)->next)
    if (*pp == r) {
      *pp = r->next;
      break;
    }
  /* so o dono pode devolver a memoria (e de outro pid) */
  if (r->owner == wp()) {
    if (r->data) k_free(r->data);
    k_free(r->path);
    k_free(r);
  } else {
    r->dead = 1;
    r->next = ram_list;   /* fica na lista, morta, ate o dono sair */
    ram_list = r;
  }
}

void wfile_proc_exit(void) {
  struct wproc* p = wp();
  struct ramfile** pp;
  ram_lock();
  for (pp = &ram_list; *pp;) {
    if ((*pp)->owner == p) *pp = (*pp)->next;   /* a memoria vai com o pid */
    else pp = &(*pp)->next;
  }
  ram_unlock();
}

/* Conteudo inteiro de um arquivo para leitura. Em RAM: o proprio bloco
 * (*tofree = 0); em disco: uma copia (k_free(*tofree) depois). */
const uint8_t* wfile_map(const char* path, uint64_t* size, void** tofree) {
  struct ramfile* r;
  int n;
  uint8_t* buf;
  *tofree = 0;
  ram_lock();
  r = ram_find(path);
  if (r) {
    *size = r->size;
    ram_unlock();
    return r->data;
  }
  ram_unlock();
  n = k_fsize(path);
  if (n < 0) return 0;
  buf = (uint8_t*)k_malloc((unsigned)n + 1u);
  if (!buf) return 0;
  if (n && k_fread(path, 0, (char*)buf, n) != n) {
    k_free(buf);
    return 0;
  }
  *size = (uint64_t)n;
  *tofree = buf;
  return buf;
}

/* 0 = nao existe, 1 = arquivo, 2 = pasta. */
static int wstat(const char* path, uint32_t* size) {
  struct k_dirent d;
  struct ramfile* r;
  int n;
  ram_lock();
  r = ram_find(path);
  if (r) {
    if (size) *size = r->size;
    ram_unlock();
    return 1;
  }
  ram_unlock();
  n = k_fsize(path);
  if (size) *size = 0;
  if (n >= 0) {
    if (size) *size = (uint32_t)n;
    return 1;
  }
  if (!path[0]) return 0;
  if (k_dir(path, &d, 1) >= 0) return 2;
  return 0;
}

/* ---------- objetos de arquivo ---------- */
void wfile_flush(struct wobj* f) {
  if (f->type != OBJ_FILE || !f->writable || !f->dirty || !f->path) return;
  if (k_fsave(f->path, (const char*)(f->data ? f->data : (const uint8_t*)""), (int)f->size) < 0)
    wlog("erro gravando ", f->path);
  f->dirty = 0;
}

static char* sdup(const char* s);

void wfile_close(struct wobj* f) {
  if (f->type == OBJ_FILE && f->ram) {
    /* leitura de arquivo em RAM: os dados sao da ramfile */
    ram_lock();
    f->ram->users--;
    if (f->ram->dead && f->ram->users <= 0 && f->ram->owner == wp()) ram_drop(f->ram);
    ram_unlock();
    f->ram = 0;
    f->data = 0;
  } else if (f->type == OBJ_FILE && f->writable && f->path && f->size >= RAM_MIN && is_temp_path(f->path)) {
    /* temporario grande: fica em RAM em vez de ir para o disco */
    struct ramfile* r = (struct ramfile*)wzalloc(sizeof(*r));
    if (r) {
      struct ramfile* old;
      r->path = sdup(f->path);
      r->data = f->data;
      r->size = f->size;
      r->cap = f->cap;
      r->owner = wp();
      ram_lock();
      old = ram_find(f->path);
      if (old) ram_drop(old);
      r->next = ram_list;
      ram_list = r;
      ram_unlock();
      k_fdelete(f->path);   /* o marcador de 0 byte no disco */
      wlogx("arquivo temporario em RAM, bytes ", f->size);
      f->data = 0;
      f->dirty = 0;
    }
  }
  wfile_flush(f);
  if (f->data) k_free(f->data);
  if (f->path) k_free(f->path);
  if (f->p2) k_free(f->p2);
  f->data = 0;
  f->path = 0;
  f->p2 = 0;
}

static char* sdup(const char* s) {
  size_t n = strlen(s) + 1;
  char* d = (char*)k_malloc((unsigned)n);
  if (d) memcpy(d, s, n);
  return d;
}

static int grow(struct wobj* f, uint64_t need) {
  uint8_t* nd;
  uint32_t ncap;
  if (need <= f->cap) return 1;
  if (need > 0x7FF00000u) return 0;
  ncap = f->cap ? f->cap : 4096;
  while (ncap < need) ncap = ncap < (64u << 20) ? ncap * 2 : ncap + (64u << 20);
  nd = (uint8_t*)k_realloc(f->data, ncap);
  if (!nd) return 0;
  f->data = nd;
  f->cap = ncap;
  return 1;
}

static HANDLE open_file(const char* name, DWORD access, DWORD disp, DWORD flags) {
  char path[300];
  struct wobj* f;
  uint32_t size = 0;
  int st, want_write = (access & 0x40000000u) || (access & 0x10000000u) || (access & 6u);
  if (wstreq(name, "CONOUT$") || wstreq(name, "CON")) return H_STDOUT;
  if (wstreq(name, "CONIN$")) return H_STDIN;
  if (wstreq(name, "NUL") || wstreq(name, "\\\\.\\NUL")) {
    f = wobj_new(OBJ_MISC);
    return f ? wh_new(f) : INVALID_HANDLE_VALUE;
  }
  wpath(name, path, (int)sizeof(path));
  st = wstat(path, &size);
  if (wine_debug) wlog(st ? "CreateFile " : "CreateFile (nao existe) ", path);
  {
    struct ramfile* r;
    ram_lock();
    r = ram_find(path);
    if (r) {
      if (disp == 1) {
        ram_unlock();
        wset_lasterr(ERROR_FILE_EXISTS);
        return INVALID_HANDLE_VALUE;
      }
      f = wobj_new(OBJ_FILE);
      if (!f) {
        ram_unlock();
        return INVALID_HANDLE_VALUE;
      }
      f->path = sdup(path);
      if (want_write || disp == 2 || disp == 5) {
        /* reabrir para escrever: o handle volta a ser dono dos dados
         * (so no processo que gravou) */
        if (r->users > 0 || r->owner != wp()) {
          ram_unlock();
          k_free(f->path);
          k_free(f);
          wset_lasterr(32); /* ERROR_SHARING_VIOLATION */
          return INVALID_HANDLE_VALUE;
        }
        f->writable = 1;
        f->data = r->data;
        f->cap = r->cap;
        f->size = (disp == 2 || disp == 5) ? 0 : r->size;
        f->dirty = 1;
        r->data = 0;
        ram_drop(r);
      } else {
        f->ram = r;
        f->data = r->data;
        f->size = r->size;
        r->users++;
      }
      ram_unlock();
      return wh_new(f);
    }
    ram_unlock();
  }
  if (st == 2) {
    if (!(flags & 0x02000000u) || disp == 1 || disp == 2) { /* BACKUP_SEMANTICS */
      wset_lasterr(disp == 1 ? ERROR_FILE_EXISTS : ERROR_ACCESS_DENIED);
      return INVALID_HANDLE_VALUE;
    }
    f = wobj_new(OBJ_FILE);
    if (!f) return INVALID_HANDLE_VALUE;
    f->is_dir = 1;
    f->path = sdup(path);
    return wh_new(f);
  }
  if (!st && (disp == 3 || disp == 5)) {
    wset_lasterr(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  if (st && disp == 1) {
    wset_lasterr(ERROR_FILE_EXISTS);
    return INVALID_HANDLE_VALUE;
  }
  f = wobj_new(OBJ_FILE);
  if (!f) return INVALID_HANDLE_VALUE;
  f->path = sdup(path);
  f->writable = want_write || disp == 2 || disp == 5 || (!st && (disp == 4 || disp == 1));
  f->append = (access & 4u) && !(access & 0x40000000u) && !(access & 2u);
  if (f->writable) {
    if (st && disp != 2 && disp != 5 && size) {
      if (!grow(f, size) || k_fload(path, (char*)f->data, (int)size + 1) < 0) {
        /* k_fload poe um 0 no fim: pede size+1 */
        if (f->data) k_free(f->data);
        f->data = (uint8_t*)k_malloc(size + 1);
        f->cap = size + 1;
        if (!f->data || k_fread(path, 0, (char*)f->data, (int)size) != (int)size) {
          wfile_close(f);
          k_free(f);
          wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
          return INVALID_HANDLE_VALUE;
        }
      }
      f->size = size;
    }
    if (!st || disp == 2 || disp == 5) {
      f->dirty = 1;
      /* o arquivo passa a existir ja (outro handle pode abrir) */
      if (!st) k_fsave(path, "", 0);
    }
  } else {
    f->size = size;
  }
  wset_lasterr(st && (disp == 2 || disp == 4) ? ERROR_ALREADY_EXISTS : 0);
  return wh_new(f);
}

static WINAPI HANDLE CreateFileA(const char* name, DWORD acc, DWORD share, void* sa, DWORD disp, DWORD fl,
                                 HANDLE tmpl) {
  (void)share;
  (void)sa;
  (void)tmpl;
  return open_file(name, acc, disp, fl);
}
static WINAPI HANDLE CreateFileW(const wchar16* name, DWORD acc, DWORD share, void* sa, DWORD disp, DWORD fl,
                                 HANDLE tmpl) {
  char n[400];
  int len = wutf16_to_8(name, -1, n, (int)sizeof(n) - 1);
  (void)share;
  (void)sa;
  (void)tmpl;
  n[len] = 0;
  return open_file(n, acc, disp, fl);
}

struct overlapped {
  uintptr_t internal, internal_high;
  DWORD off, off_high;
  HANDLE event;
};

int wsock_read(struct wobj* s, void* buf, DWORD n, DWORD* done, void* ov);  /* net.c */
int wsock_write(struct wobj* s, const void* buf, DWORD n, DWORD* done, void* ov);

static WINAPI BOOL ReadFile(HANDLE h, void* buf, DWORD n, DWORD* done, struct overlapped* ov) {
  struct wobj* f;
  if (done) *done = 0;
  if (h == H_STDIN) {
    char line[256];
    int len;
    if (!n) return 1;
    len = wcon_readline(line, (int)sizeof(line) - 2);
    if (len < 0) len = 0;
    line[len++] = '\r';
    line[len++] = '\n';
    if ((DWORD)len > n) len = (int)n;
    memcpy(buf, line, (size_t)len);
    if (done) *done = (DWORD)len;
    return 1;
  }
  f = wh_get(h, 0);
  if (f && f->type == OBJ_SOCKET) return wsock_read(f, buf, n, done, ov);
  if (!f || f->type != OBJ_FILE || f->is_dir) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  {
    uint64_t pos = ov ? ((uint64_t)ov->off_high << 32 | ov->off) : f->pos;
    DWORD got = 0;
    if (pos < f->size) {
      got = (uint64_t)n > f->size - pos ? (DWORD)(f->size - pos) : n;
      if (f->data || f->writable || f->ram) {
        memcpy(buf, f->data + pos, got);
      } else {
        int r = k_fread(f->path, (unsigned)pos, (char*)buf, (int)got);
        if (r < 0) {
          wset_lasterr(ERROR_ACCESS_DENIED);
          return 0;
        }
        got = (DWORD)r;
      }
    }
    if (!ov) f->pos = pos + got;
    if (done) *done = got;
    if (ov) {
      ov->internal = 0;
      ov->internal_high = got;
      if (!got && n) {
        wset_lasterr(ERROR_HANDLE_EOF);
        return 0;
      }
    }
    return 1;
  }
}

static WINAPI BOOL WriteFile(HANDLE h, const void* buf, DWORD n, DWORD* done, struct overlapped* ov) {
  struct wobj* f;
  if (done) *done = 0;
  if (h == H_STDOUT || h == H_STDERR) {
    wcon_write((const char*)buf, n);
    if (done) *done = n;
    return 1;
  }
  f = wh_get(h, 0);
  if (f && f->type == OBJ_SOCKET) return wsock_write(f, buf, n, done, ov);
  if (f && f->type == OBJ_MISC) {
    if (done) *done = n;
    return 1;
  }
  if (!f || f->type != OBJ_FILE || !f->writable) {
    wset_lasterr(f ? ERROR_ACCESS_DENIED : ERROR_INVALID_HANDLE);
    return 0;
  }
  {
    uint64_t pos = ov ? ((uint64_t)ov->off_high << 32 | ov->off) : f->pos;
    if (f->append && f->size <= 256u * 1024u) {
      /* log em modo append: outro processo (o filho "elevado") pode ter
       * escrito no mesmo arquivo; relê do disco antes de acrescentar */
      int dsz = k_fsize(f->path);
      if (dsz > (int)f->size && dsz <= 512 * 1024 && grow(f, (uint64_t)dsz + n)) {
        if (k_fread(f->path, 0, (char*)f->data, dsz) == dsz) f->size = (uint32_t)dsz;
      }
    }
    if (f->append || (ov && ov->off == 0xFFFFFFFFu && ov->off_high == 0xFFFFFFFFu)) pos = f->size;
    if (!grow(f, pos + n)) {
      wset_lasterr(112); /* ERROR_DISK_FULL */
      return 0;
    }
    if (pos > f->size) memset(f->data + f->size, 0, (size_t)(pos - f->size));
    memcpy(f->data + pos, buf, n);
    if (pos + n > f->size) f->size = (uint32_t)(pos + n);
    if (!ov) f->pos = pos + n;
    f->dirty = 1;
    /* arquivo pequeno (log): vai para o disco a cada escrita, para existir
     * mesmo se o programa nao fechar; os grandes so no fechamento */
    if (f->size <= 256u * 1024u) wfile_flush(f);
    if (done) *done = n;
    if (ov) {
      ov->internal = 0;
      ov->internal_high = n;
    }
    return 1;
  }
}

static WINAPI BOOL WriteConsoleA(HANDLE h, const void* buf, DWORD n, DWORD* done, void* r) {
  (void)r;
  (void)h;
  wcon_write((const char*)buf, n);
  if (done) *done = n;
  return 1;
}
static WINAPI BOOL WriteConsoleW(HANDLE h, const wchar16* buf, DWORD n, DWORD* done, void* r) {
  char tmp[512];
  DWORD i = 0;
  (void)r;
  (void)h;
  while (i < n) {
    DWORD k = n - i > 120 ? 120 : n - i;
    int b = wutf16_to_8(buf + i, (int)k, tmp, (int)sizeof(tmp));
    wcon_write(tmp, (size_t)b);
    i += k;
  }
  if (done) *done = n;
  return 1;
}

static WINAPI BOOL SetFilePointerEx(HANDLE h, int64_t dist, int64_t* out, DWORD how) {
  struct wobj* f = wh_get(h, OBJ_FILE);
  int64_t base;
  if (!f) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  base = how == 0 ? 0 : how == 1 ? (int64_t)f->pos : (int64_t)f->size;
  if (base + dist < 0) {
    wset_lasterr(131); /* ERROR_NEGATIVE_SEEK */
    return 0;
  }
  f->pos = (uint64_t)(base + dist);
  if (out) *out = (int64_t)f->pos;
  return 1;
}
static WINAPI DWORD SetFilePointer(HANDLE h, int32_t dist, int32_t* high, DWORD how) {
  int64_t d = high ? ((int64_t)*high << 32) | (uint32_t)dist : (int64_t)dist, out = 0;
  if (!SetFilePointerEx(h, d, &out, how)) return 0xFFFFFFFFu;
  if (high) *high = (int32_t)(out >> 32);
  wset_lasterr(0);
  return (DWORD)out;
}
static WINAPI BOOL GetFileSizeEx(HANDLE h, int64_t* size) {
  struct wobj* f = wh_get(h, OBJ_FILE);
  if (!f) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  *size = f->size;
  return 1;
}
static WINAPI DWORD GetFileSize(HANDLE h, DWORD* high) {
  struct wobj* f = wh_get(h, OBJ_FILE);
  if (!f) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0xFFFFFFFFu;
  }
  if (high) *high = 0;
  return f->size;
}
static WINAPI BOOL SetEndOfFile(HANDLE h) {
  struct wobj* f = wh_get(h, OBJ_FILE);
  if (!f || !f->writable) return 0;
  if (!grow(f, f->pos)) return 0;
  if (f->pos > f->size) memset(f->data + f->size, 0, (size_t)(f->pos - f->size));
  f->size = (uint32_t)f->pos;
  f->dirty = 1;
  return 1;
}
static WINAPI BOOL FlushFileBuffers(HANDLE h) {
  struct wobj* f = wh_get(h, OBJ_FILE);
  if (f) wfile_flush(f);
  return 1;
}
static WINAPI DWORD GetFileType(HANDLE h) {
  struct wobj* f;
  if (h == H_STDIN || h == H_STDOUT || h == H_STDERR) return 2; /* FILE_TYPE_CHAR */
  f = wh_get(h, 0);
  if (!f) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  if (f->type == OBJ_FILE) return 1; /* DISK */
  if (f->type == OBJ_SOCKET) return 3; /* PIPE */
  return 0;
}
static WINAPI BOOL LockFileEx(HANDLE h, DWORD f, DWORD r, DWORD lo, DWORD hi, void* ov) {
  (void)h; (void)f; (void)r; (void)lo; (void)hi; (void)ov;
  return 1;
}
static WINAPI BOOL UnlockFileEx(HANDLE h, DWORD r, DWORD lo, DWORD hi, void* ov) {
  (void)h; (void)r; (void)lo; (void)hi; (void)ov;
  return 1;
}

/* ---------- atributos ---------- */
static DWORD attrs_of(const char* path, uint32_t* size) {
  int st = wstat(path, size);
  if (!st) return INVALID_ATTRS;
  return st == 2 ? ATTR_DIR : ATTR_ARCHIVE;
}
static WINAPI DWORD GetFileAttributesA(const char* name) {
  char p[300];
  DWORD a = attrs_of(wpath(name, p, (int)sizeof(p)), 0);
  if (a == INVALID_ATTRS) wset_lasterr(ERROR_FILE_NOT_FOUND);
  return a;
}
static WINAPI DWORD GetFileAttributesW(const wchar16* name) {
  char p[300];
  DWORD a = attrs_of(wpath16(name, p, (int)sizeof(p)), 0);
  if (a == INVALID_ATTRS) wset_lasterr(ERROR_FILE_NOT_FOUND);
  return a;
}
struct attr_data {
  DWORD attrs;
  uint64_t ctime, atime, mtime;
  DWORD size_high, size_low;
} __attribute__((packed));
static WINAPI BOOL GetFileAttributesExW(const wchar16* name, int level, struct attr_data* d) {
  char p[300];
  uint32_t size = 0;
  DWORD a = attrs_of(wpath16(name, p, (int)sizeof(p)), &size);
  (void)level;
  if (a == INVALID_ATTRS) {
    wset_lasterr(ERROR_FILE_NOT_FOUND);
    return 0;
  }
  d->attrs = a;
  d->ctime = d->atime = d->mtime = wfiletime_now();
  d->size_high = 0;
  d->size_low = size;
  return 1;
}
static WINAPI BOOL SetFileAttributesW(const wchar16* name, DWORD a) {
  (void)name;
  (void)a;
  return 1;
}
struct by_handle_info {
  DWORD attrs;
  uint64_t ctime, atime, mtime;
  DWORD serial, size_high, size_low, nlinks, idx_high, idx_low;
} __attribute__((packed));
static WINAPI BOOL GetFileInformationByHandle(HANDLE h, struct by_handle_info* d) {
  struct wobj* f = wh_get(h, OBJ_FILE);
  if (!f) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  memset(d, 0, sizeof(*d));
  d->attrs = f->is_dir ? ATTR_DIR : ATTR_ARCHIVE;
  d->ctime = d->atime = d->mtime = wfiletime_now();
  d->serial = 0x50D05;
  d->size_low = f->size;
  d->nlinks = 1;
  d->idx_low = (DWORD)(uintptr_t)f;
  return 1;
}
static WINAPI BOOL GetFileInformationByHandleEx(HANDLE h, int cls, void* buf, DWORD len) {
  struct wobj* f = wh_get(h, OBJ_FILE);
  if (!f) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  memset(buf, 0, len);
  switch (cls) {
    case 0: /* FileBasicInfo: 4 tempos + atributos */
      if (len < 40) break;
      ((uint64_t*)buf)[0] = ((uint64_t*)buf)[1] = ((uint64_t*)buf)[2] = ((uint64_t*)buf)[3] = wfiletime_now();
      ((DWORD*)buf)[8] = f->is_dir ? ATTR_DIR : ATTR_ARCHIVE;
      return 1;
    case 1: /* FileStandardInfo */
      if (len < 24) break;
      ((uint64_t*)buf)[0] = f->size;
      ((uint64_t*)buf)[1] = f->size;
      ((DWORD*)buf)[4] = 1;
      ((uint8_t*)buf)[21] = (uint8_t)f->is_dir;
      return 1;
    case 9: /* FileAttributeTagInfo */
      if (len < 8) break;
      ((DWORD*)buf)[0] = f->is_dir ? ATTR_DIR : ATTR_ARCHIVE;
      return 1;
  }
  wset_lasterr(ERROR_INVALID_PARAMETER);
  return 0;
}
static WINAPI BOOL GetFileTime(HANDLE h, uint64_t* c, uint64_t* a, uint64_t* m) {
  (void)h;
  if (c) *c = wfiletime_now();
  if (a) *a = wfiletime_now();
  if (m) *m = wfiletime_now();
  return 1;
}
static WINAPI BOOL SetFileTime(HANDLE h, const void* c, const void* a, const void* m) {
  (void)h; (void)c; (void)a; (void)m;
  return 1;
}

/* ---------- apagar, mover, pastas ---------- */
static int ram_delete(const char* path) {
  struct ramfile* r;
  ram_lock();
  r = ram_find(path);
  if (r) ram_drop(r);
  ram_unlock();
  return r != 0;
}
static WINAPI BOOL DeleteFileW(const wchar16* name) {
  char p[300];
  wpath16(name, p, (int)sizeof(p));
  if (ram_delete(p)) return 1;
  if (k_fdelete(p) < 0) {
    wset_lasterr(ERROR_FILE_NOT_FOUND);
    return 0;
  }
  return 1;
}
static WINAPI BOOL DeleteFileA(const char* name) {
  char p[300];
  if (k_fdelete(wpath(name, p, (int)sizeof(p))) < 0) {
    wset_lasterr(ERROR_FILE_NOT_FOUND);
    return 0;
  }
  return 1;
}
static int copy_file(const char* from, const char* to) {
  int n = k_fsize(from), ok;
  char* buf;
  if (n < 0) return 0;
  buf = (char*)k_malloc((unsigned)n + 1);
  if (!buf) return 0;
  ok = k_fread(from, 0, buf, n) == n && k_fsave(to, buf, n) >= 0;
  k_free(buf);
  return ok;
}
static WINAPI BOOL MoveFileExW(const wchar16* from, const wchar16* to, DWORD fl) {
  char a[300], b[300];
  int r;
  wpath16(from, a, (int)sizeof(a));
  wpath16(to, b, (int)sizeof(b));
  {
    struct ramfile* rf;
    ram_lock();
    rf = ram_find(a);
    if (rf) {
      struct ramfile* dst = ram_find(b);
      if (dst && !(fl & 1)) {
        ram_unlock();
        wset_lasterr(ERROR_ALREADY_EXISTS);
        return 0;
      }
      if (dst) ram_drop(dst);
      if (is_temp_path(b)) {
        /* continua em RAM, com o nome novo */
        char* np = sdup(b);
        if (np) {
          k_free(rf->path);
          rf->path = np;
        }
        ram_unlock();
        if (k_fsize(b) >= 0) k_fdelete(b);
        return 1;
      }
      ram_unlock();
      /* saiu de TEMP: agora vai para o disco */
      if (k_fsave(b, (const char*)rf->data, (int)rf->size) < 0) {
        wset_lasterr(112);
        return 0;
      }
      ram_delete(a);
      return 1;
    }
    ram_unlock();
  }
  if ((fl & 1) && k_fsize(b) >= 0) k_fdelete(b); /* REPLACE_EXISTING */
  r = k_frename(a, b);
  if (r == 0) return 1;
  if ((r == -5 || r == -6 || r == -7) && copy_file(a, b)) {
    k_fdelete(a);
    return 1;
  }
  wset_lasterr(r == -1 ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
  return 0;
}
static WINAPI BOOL MoveFileW(const wchar16* from, const wchar16* to) { return MoveFileExW(from, to, 0); }
static WINAPI BOOL CopyFileW(const wchar16* from, const wchar16* to, BOOL fail_exists) {
  char a[300], b[300];
  wpath16(from, a, (int)sizeof(a));
  wpath16(to, b, (int)sizeof(b));
  if (fail_exists && k_fsize(b) >= 0) {
    wset_lasterr(ERROR_FILE_EXISTS);
    return 0;
  }
  if (!copy_file(a, b)) {
    wset_lasterr(ERROR_FILE_NOT_FOUND);
    return 0;
  }
  return 1;
}
static WINAPI BOOL CreateDirectoryW(const wchar16* name, void* sa) {
  char p[300];
  int r = k_mkdir(wpath16(name, p, (int)sizeof(p)));
  (void)sa;
  if (r == 1) {
    wset_lasterr(ERROR_ALREADY_EXISTS);
    return 0;
  }
  if (r != 0) {
    wset_lasterr(ERROR_PATH_NOT_FOUND);
    return 0;
  }
  return 1;
}
static WINAPI BOOL CreateDirectoryA(const char* name, void* sa) {
  char p[300];
  int r = k_mkdir(wpath(name, p, (int)sizeof(p)));
  (void)sa;
  if (r == 1) wset_lasterr(ERROR_ALREADY_EXISTS);
  else if (r) wset_lasterr(ERROR_PATH_NOT_FOUND);
  return r == 0;
}
static WINAPI BOOL RemoveDirectoryW(const wchar16* name) {
  char p[300];
  int r = k_rmdir(wpath16(name, p, (int)sizeof(p)));
  if (r) wset_lasterr(r == -2 ? 145 : ERROR_PATH_NOT_FOUND); /* DIR_NOT_EMPTY */
  return r == 0;
}

/* ---------- FindFirstFile ---------- */
struct find_dataw {
  DWORD attrs;
  uint64_t ctime, atime, mtime;
  DWORD size_high, size_low, res0, res1;
  wchar16 name[260];
  wchar16 alt[14];
} __attribute__((packed));

static int wild(const char* pat, const char* s) {
  for (; *pat; pat++, s++) {
    if (*pat == '*') {
      for (pat++;; s++) {
        if (wild(pat, s)) return 1;
        if (!*s) return 0;
      }
    }
    if (!*s) return 0;
    if (*pat != '?') {
      char a = *pat, b = *s;
      if (a >= 'a' && a <= 'z') a = (char)(a - 32);
      if (b >= 'a' && b <= 'z') b = (char)(b - 32);
      if (a != b) return 0;
    }
  }
  return *s == 0;
}

static int find_fill(struct wobj* f, struct find_dataw* d) {
  struct k_dirent* ents = (struct k_dirent*)f->data;
  const char* pat = (const char*)f->p2;
  while (f->pos < f->size) {
    struct k_dirent* e = &ents[f->pos++];
    int n;
    if (!wild(pat, e->name)) continue;
    memset(d, 0, sizeof(*d));
    d->attrs = e->dir ? ATTR_DIR : ATTR_ARCHIVE;
    d->ctime = d->atime = d->mtime = wfiletime_now();
    d->size_low = e->size;
    n = wutf8_to_16(e->name, -1, d->name, 259);
    d->name[n] = 0;
    return 1;
  }
  return 0;
}

static WINAPI HANDLE FindFirstFileW(const wchar16* spec, struct find_dataw* d) {
  char p[300];
  char* slash;
  const char* pat;
  struct wobj* f;
  struct k_dirent* ents;
  int n, max = 512;
  wpath16(spec, p, (int)sizeof(p));
  slash = strrchr(p, '\\');
  if (slash) {
    pat = slash + 1;
    *slash = 0;
  } else {
    pat = p;
  }
  f = wobj_new(OBJ_FIND);
  ents = (struct k_dirent*)k_malloc((unsigned)(sizeof(*ents) * (size_t)max));
  if (!f || !ents) return INVALID_HANDLE_VALUE;
  f->p2 = sdup(*pat ? pat : "*");
  if (slash) {
    char dir[300];
    strcpy(dir, p);
    if (strlen(dir) == 2 && dir[1] == ':') strcat(dir, "\\");
    n = k_dir(dir, ents, max);
  } else {
    n = k_dir(wp()->cwd, ents, max);
  }
  if (n < 0) {
    k_free(ents);
    k_free(f->p2);
    k_free(f);
    wset_lasterr(ERROR_PATH_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  f->data = (uint8_t*)ents;
  f->size = (uint32_t)n;
  f->pos = 0;
  if (!find_fill(f, d)) {
    wfile_close(f);
    k_free(f);
    wset_lasterr(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  return wh_new(f);
}
static WINAPI HANDLE FindFirstFileExW(const wchar16* spec, int lvl, struct find_dataw* d, int op, void* flt,
                                      DWORD fl) {
  (void)lvl; (void)op; (void)flt; (void)fl;
  return FindFirstFileW(spec, d);
}
static WINAPI BOOL FindNextFileW(HANDLE h, struct find_dataw* d) {
  struct wobj* f = wh_get(h, OBJ_FIND);
  if (!f) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  if (!find_fill(f, d)) {
    wset_lasterr(ERROR_NO_MORE_FILES);
    return 0;
  }
  return 1;
}
static WINAPI BOOL FindClose(HANDLE h) { return wh_close(h); }

/* ---------- pastas conhecidas ---------- */
static DWORD put16(const char* s, wchar16* buf, DWORD n) {
  int len = wutf8_to_16(s, -1, 0, 0);
  if ((DWORD)len + 1 > n) return (DWORD)len + 1;
  wutf8_to_16(s, -1, buf, len);
  buf[len] = 0;
  return (DWORD)len;
}
static DWORD put8(const char* s, char* buf, DWORD n) {
  DWORD len = (DWORD)strlen(s);
  if (len + 1 > n) return len + 1;
  memcpy(buf, s, len + 1);
  return len;
}
static WINAPI DWORD GetCurrentDirectoryW(DWORD n, wchar16* buf) { return put16(wp()->cwd, buf, n); }
static WINAPI DWORD GetCurrentDirectoryA(DWORD n, char* buf) { return put8(wp()->cwd, buf, n); }
static WINAPI BOOL SetCurrentDirectoryW(const wchar16* name) {
  char p[300];
  struct wproc* pr = wp();
  wpath16(name, p, (int)sizeof(p));
  if (wstat(p, 0) != 2) {
    wset_lasterr(ERROR_PATH_NOT_FOUND);
    return 0;
  }
  strncpy(pr->cwd, p, sizeof(pr->cwd) - 1);
  k_chdir(p);
  return 1;
}
static void full_path(const char* in, char* out, int max) {
  char p[300];
  wpath(in, p, (int)sizeof(p));
  if (p[0] && p[1] == ':') {
    strncpy(out, p, (size_t)max - 1);
    out[max - 1] = 0;
    if (!out[2] && max > 4) strcat(out, "\\");
    return;
  }
  {
    const char* cwd = wp()->cwd;
    int n;
    if (p[0] == '\\') {
      out[0] = cwd[0];
      out[1] = ':';
      out[2] = 0;
    } else {
      strncpy(out, cwd, (size_t)max - 1);
      out[max - 1] = 0;
      n = (int)strlen(out);
      if (n && out[n - 1] != '\\' && n < max - 1) strcat(out, "\\");
    }
    strncat(out, p, (size_t)max - strlen(out) - 1);
  }
}
static WINAPI DWORD GetFullPathNameW(const wchar16* name, DWORD n, wchar16* buf, wchar16** file) {
  char in[300], out[400];
  int len = wutf16_to_8(name, -1, in, (int)sizeof(in) - 1);
  DWORD r;
  in[len] = 0;
  full_path(in, out, (int)sizeof(out));
  r = put16(out, buf, n);
  if (file && r < n) {
    wchar16* q;
    *file = 0;
    for (q = buf; *q; q++)
      if (*q == '\\' && q[1]) *file = q + 1;
  }
  return r;
}
static WINAPI DWORD GetFullPathNameA(const char* name, DWORD n, char* buf, char** file) {
  char out[400];
  DWORD r;
  full_path(name, out, (int)sizeof(out));
  r = put8(out, buf, n);
  if (file && r < n) {
    char* q = strrchr(buf, '\\');
    *file = q && q[1] ? q + 1 : 0;
  }
  return r;
}
static WINAPI DWORD GetLongPathNameW(const wchar16* s, wchar16* buf, DWORD n) {
  DWORD len = (DWORD)wstrlen16(s);
  if (len + 1 > n) return len + 1;
  memcpy(buf, s, (len + 1) * 2);
  return len;
}

/* Pastas do "Windows" dentro do SODOS: D:\WINE\... (criadas quando pedidas). */
#define WINE_ROOT "D:\\WINE"
void wensure_dir(const char* path);
static void ensure_dir(const char* path) { wensure_dir(path); }
void wensure_dir(const char* path) {
  /* a ultima pasta garantida: extrair um pacote pede a mesma milhares de vezes */
  static char last[260];
  static volatile int last_lock;
  char tmp[260];
  int i, same;
  while (__sync_lock_test_and_set(&last_lock, 1)) k_thread_yield();
  same = wstreq(last, path);
  if (!same) {
    strncpy(last, path, sizeof(last) - 1);
    last[sizeof(last) - 1] = 0;
  }
  __sync_lock_release(&last_lock);
  if (same) return;
  for (i = 3; path[i]; i++) {
    if (path[i] == '\\') {
      memcpy(tmp, path, (size_t)i);
      tmp[i] = 0;
      k_mkdir(tmp);
    }
  }
  k_mkdir(path);
}
const char* wine_dir(const char* rel) {
  /* um buffer por thread nao vale a pena: quem chama copia na hora */
  static char buf[4][160];
  static int k;
  char* b = buf[k++ & 3];
  strcpy(b, WINE_ROOT);
  if (rel && *rel) {
    strcat(b, "\\");
    strcat(b, rel);
  }
  ensure_dir(b);
  return b;
}
static WINAPI DWORD GetTempPathW(DWORD n, wchar16* buf) {
  char p[200];
  strcpy(p, wine_dir("TEMP"));
  strcat(p, "\\");
  return put16(p, buf, n);
}
static WINAPI DWORD GetTempPathA(DWORD n, char* buf) {
  char p[200];
  strcpy(p, wine_dir("TEMP"));
  strcat(p, "\\");
  return put8(p, buf, n);
}
static WINAPI DWORD GetTempFileNameW(const wchar16* dir, const wchar16* pre, DWORD uniq, wchar16* out) {
  char d[300], name[340];
  static unsigned seq;
  unsigned u = uniq ? uniq : (k_ms() ^ (++seq * 7919u)) & 0xFFFF;
  static const char hex[] = "0123456789ABCDEF";
  int n;
  (void)pre;
  wpath16(dir, d, (int)sizeof(d));
  strcpy(name, d);
  n = (int)strlen(name);
  if (n && name[n - 1] != '\\') name[n++] = '\\';
  name[n++] = 'T';
  name[n++] = 'M';
  name[n++] = 'P';
  name[n++] = hex[(u >> 12) & 15];
  name[n++] = hex[(u >> 8) & 15];
  name[n++] = hex[(u >> 4) & 15];
  name[n++] = hex[u & 15];
  strcpy(name + n, ".TMP");
  if (!uniq) k_fsave(name, "", 0);
  put16(name, out, 260);
  return u ? u : 1;
}
static WINAPI DWORD GetSystemDirectoryW(wchar16* buf, DWORD n) { return put16(wine_dir("SYSTEM32"), buf, n); }
static WINAPI DWORD GetSystemDirectoryA(char* buf, DWORD n) { return put8(wine_dir("SYSTEM32"), buf, n); }
static WINAPI DWORD GetWindowsDirectoryW(wchar16* buf, DWORD n) { return put16(WINE_ROOT, buf, n); }
static WINAPI DWORD GetSystemWindowsDirectoryW(wchar16* buf, DWORD n) { return put16(WINE_ROOT, buf, n); }
static WINAPI DWORD GetDriveTypeW(const wchar16* root) {
  (void)root;
  return 3; /* DRIVE_FIXED */
}
static WINAPI DWORD GetLogicalDrives(void) { return k_drive_mask(); }
static WINAPI BOOL GetDiskFreeSpaceExW(const wchar16* dir, uint64_t* avail, uint64_t* total, uint64_t* freeb) {
  unsigned t = 0, f = 0;
  char p[300];
  wpath16(dir, p, (int)sizeof(p));
  if (k_dfree(p[0] && p[1] == ':' ? p[0] : wp()->cwd[0], &t, &f) < 0) {
    t = 64u << 20;
    f = 32u << 20;
  }
  if (avail) *avail = (uint64_t)f * 1024;
  if (total) *total = (uint64_t)t * 1024;
  if (freeb) *freeb = (uint64_t)f * 1024;
  return 1;
}
static WINAPI BOOL GetVolumeInformationW(const wchar16* root, wchar16* name, DWORD nn, DWORD* serial, DWORD* maxc,
                                         DWORD* flags, wchar16* fs, DWORD nfs) {
  (void)root;
  if (name && nn) put16("SODOS", name, nn);
  if (serial) *serial = 0x50D05;
  if (maxc) *maxc = 255;
  if (flags) *flags = 3;
  if (fs && nfs) put16("FAT32", fs, nfs);
  return 1;
}
static WINAPI DWORD GetFinalPathNameByHandleW(HANDLE h, wchar16* buf, DWORD n, DWORD fl) {
  struct wobj* f = wh_get(h, OBJ_FILE);
  char p[320];
  (void)fl;
  if (!f || !f->path) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  strcpy(p, "\\\\?\\");
  strcat(p, f->path);
  return put16(p, buf, n);
}

#define S(n) {#n, (void*)n}
const struct wine_sym file_syms[] = {
    S(CreateFileA), S(CreateFileW), S(ReadFile), S(WriteFile), S(WriteConsoleA), S(WriteConsoleW),
    S(SetFilePointerEx), S(SetFilePointer), S(GetFileSizeEx), S(GetFileSize), S(SetEndOfFile),
    S(FlushFileBuffers), S(GetFileType), S(LockFileEx), S(UnlockFileEx), S(GetFileAttributesA),
    S(GetFileAttributesW), S(GetFileAttributesExW), S(SetFileAttributesW),
    S(GetFileInformationByHandle), S(GetFileInformationByHandleEx), S(GetFileTime), S(SetFileTime),
    S(DeleteFileW), S(DeleteFileA), S(MoveFileExW), S(MoveFileW), S(CopyFileW), S(CreateDirectoryW),
    S(CreateDirectoryA), S(RemoveDirectoryW), S(FindFirstFileW), S(FindFirstFileExW),
    S(FindNextFileW), S(FindClose), S(GetCurrentDirectoryW), S(GetCurrentDirectoryA),
    S(SetCurrentDirectoryW), S(GetFullPathNameW), S(GetFullPathNameA), S(GetLongPathNameW),
    S(GetTempPathW), S(GetTempPathA), S(GetTempFileNameW), S(GetSystemDirectoryW),
    S(GetSystemDirectoryA), S(GetWindowsDirectoryW), S(GetSystemWindowsDirectoryW), S(GetDriveTypeW),
    S(GetLogicalDrives), S(GetDiskFreeSpaceExW), S(GetVolumeInformationW),
    S(GetFinalPathNameByHandleW),
    {0, 0}};
