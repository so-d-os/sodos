/*
 * pe.c - Carregador PE32+ (x86-64) do WINE.DLL.
 *
 * A imagem vai para um bloco do programa (k_malloc_aligned, 64 KB), a base
 * preferida e ignorada e as relocacoes base sao aplicadas: o SODOS ainda nao
 * tem tabelas de paginas por processo, entao so roda EXE/DLL com .reloc
 * (os de hoje, com DYNAMIC_BASE). Imports resolvem primeiro nas DLLs
 * embutidas (k32.c, crt.c) e depois em arquivos .dll ao lado do EXE ou em
 * PATHSODOS\WINE\SYSTEM32. Funcao que falta vira um stub que avisa o nome.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

#define RD16(p) (*(const uint16_t*)(p))
#define RD32(p) (*(const uint32_t*)(p))
#define RD64(p) (*(const uint64_t*)(p))

/* Diretorios do OptionalHeader */
enum { DIR_EXPORT = 0, DIR_IMPORT = 1, DIR_EXCEPTION = 3, DIR_BASERELOC = 5, DIR_TLS = 9 };

struct pe_info {
  const uint8_t* nt;        /* "PE\0\0" */
  uint16_t nsect;
  uint16_t chars;
  const uint8_t* opt;
  const uint8_t* sect;
  uint64_t image_base;
  uint32_t size_image;
  uint32_t size_headers;
  uint32_t entry;
  uint32_t ndirs;
};

static int pe_parse(const uint8_t* f, uint32_t fsz, struct pe_info* pi, const char* name) {
  uint32_t lfanew;
  uint16_t optsz;
  if (fsz < 0x40 || f[0] != 'M' || f[1] != 'Z') goto bad;
  lfanew = RD32(f + 0x3C);
  if (lfanew + 24u > fsz || memcmp(f + lfanew, "PE\0\0", 4) != 0) goto bad;
  pi->nt = f + lfanew;
  if (RD16(pi->nt + 4) != 0x8664) {
    wsay(name, RD16(pi->nt + 4) == 0x14C ? ": programa de 32 bits (ainda nao suportado, so x64)"
                                         : ": maquina nao e x86-64");
    return -1;
  }
  pi->nsect = RD16(pi->nt + 6);
  optsz = RD16(pi->nt + 20);
  pi->chars = RD16(pi->nt + 22);
  pi->opt = pi->nt + 24;
  if (lfanew + 24u + optsz + pi->nsect * 40u > fsz || RD16(pi->opt) != 0x20B) goto bad;
  pi->sect = pi->opt + optsz;
  pi->entry = RD32(pi->opt + 16);
  pi->image_base = RD64(pi->opt + 24);
  pi->size_image = RD32(pi->opt + 56);
  pi->size_headers = RD32(pi->opt + 60);
  pi->ndirs = RD32(pi->opt + 108);
  if (pi->ndirs > 16) pi->ndirs = 16;
  if (!pi->size_image || pi->size_headers > fsz) goto bad;
  return 0;
bad:
  wsay(name, ": nao e um executavel PE32+ valido");
  return -1;
}

static void dir_of(const struct pe_info* pi, int i, uint32_t* rva, uint32_t* size) {
  *rva = 0;
  *size = 0;
  if ((uint32_t)i >= pi->ndirs) return;
  *rva = RD32(pi->opt + 112 + 8 * i);
  *size = RD32(pi->opt + 112 + 8 * i + 4);
}

static int relocate(uint8_t* base, const struct pe_info* pi, const char* name) {
  uint32_t rva, size, off = 0;
  int64_t delta = (int64_t)((uint64_t)(uintptr_t)base - pi->image_base);
  dir_of(pi, DIR_BASERELOC, &rva, &size);
  if (!delta) return 0;
  if (!rva || !size || (pi->chars & 1)) {
    wsay(name, ": sem relocacoes (base fixa); precisa de memoria virtual (marco W4)");
    return -1;
  }
  while (off + 8 <= size) {
    const uint8_t* blk = base + rva + off;
    uint32_t page = RD32(blk), bsz = RD32(blk + 4), i;
    if (bsz < 8) break;
    for (i = 8; i + 2 <= bsz; i += 2) {
      uint16_t e = RD16(blk + i);
      uint8_t* at = base + page + (e & 0xFFF);
      switch (e >> 12) {
        case 0: break;
        case 10: *(uint64_t*)at += (uint64_t)delta; break;
        case 3: *(uint32_t*)at += (uint32_t)delta; break;
        case 2: *(uint16_t*)at += (uint16_t)delta; break;
        case 1: *(uint16_t*)at += (uint16_t)((uint64_t)delta >> 16); break;
        default:
          wsay(name, ": tipo de relocacao desconhecido");
          return -1;
      }
    }
    off += bsz;
  }
  return 0;
}

/* ---------- lista de modulos ---------- */
static void norm_name(char* out, const char* in) {
  char b[WMOD_NAME];
  int n;
  wbase(b, in, (int)sizeof(b));
  n = (int)strlen(b);
  /* sem extensao: .dll (regra do LoadLibrary) */
  if (!strchr(b, '.') && n + 4 < WMOD_NAME) strcat(b, ".dll");
  strcpy(out, b);
}

struct wmod* pe_find(const char* name) {
  char n[WMOD_NAME];
  struct wmod* m;
  norm_name(n, name);
  for (m = wp()->mods; m; m = m->next)
    if (wstreq(m->name, n)) return m;
  return 0;
}

struct wmod* pe_from_handle(void* h) {
  struct wmod* m;
  if (!h) return wp()->exe;
  for (m = wp()->mods; m; m = m->next)
    if ((void*)m->base == h || (void*)m == h) return m;
  return 0;
}

void* pe_handle(struct wmod* m) { return m->base ? (void*)m->base : (void*)m; }

static struct wmod* mod_new(const char* name) {
  struct wmod* m = (struct wmod*)wzalloc(sizeof(*m));
  struct wproc* p = wp();
  struct wmod** tail;
  if (!m) return 0;
  norm_name(m->name, name);
  m->tls_index = -1;
  for (tail = &p->mods; *tail; tail = &(*tail)->next) {
  }
  *tail = m;
  return m;
}

/* "api-ms-win-crt-stdio-l1-1-0.dll", "C:\Windows\System32
tdll.dll" -> embutida. */
static struct wmod* builtin_load(const char* name) {
  char n[WMOD_NAME];
  struct wmod* m;
  char* dot;
  norm_name(n, name);
  dot = strrchr(n, '.');
  if (dot && wstreq(dot, ".dll")) *dot = 0;
  if (!builtin_known(n)) return 0;
  m = mod_new(name);
  if (!m) return 0;
  m->builtin = 1;
  m->is_dll = 1;
  return m;
}

struct wmod* pe_from_addr(uint64_t addr) {
  struct wmod* m;
  for (m = wp()->mods; m; m = m->next)
    if (m->base && addr >= (uint64_t)(uintptr_t)m->base && addr < (uint64_t)(uintptr_t)m->base + m->size)
      return m;
  return 0;
}

/* ---------- exports ---------- */
static void* export_lookup(struct wmod* m, const char* name, int depth);

void* pe_getproc(struct wmod* m, const char* name) { return m ? export_lookup(m, name, 0) : 0; }

static void* export_lookup(struct wmod* m, const char* name, int depth) {
  uint32_t rva, size, nfun, nnames, ordbase, idx = 0xFFFFFFFFu;
  const uint8_t* ed;
  const struct pe_info* unused = 0;
  (void)unused;
  if (m->builtin) {
    if ((uintptr_t)name < 0x10000) return builtin_ordinal(m->name, (unsigned)(uintptr_t)name);
    return builtin_lookup(name);
  }
  {
    struct pe_info pi;
    if (pe_parse(m->base, m->size, &pi, m->name) != 0) return 0;
    dir_of(&pi, DIR_EXPORT, &rva, &size);
  }
  if (!rva) return 0;
  ed = m->base + rva;
  ordbase = RD32(ed + 0x10);
  nfun = RD32(ed + 0x14);
  nnames = RD32(ed + 0x18);
  if ((uintptr_t)name < 0x10000) {
    idx = (uint32_t)(uintptr_t)name - ordbase;
  } else {
    const uint32_t* names = (const uint32_t*)(m->base + RD32(ed + 0x20));
    const uint16_t* ords = (const uint16_t*)(m->base + RD32(ed + 0x24));
    /* nomes em ordem: busca binaria */
    uint32_t lo = 0, hi = nnames;
    while (lo < hi) {
      uint32_t mid = (lo + hi) / 2;
      int c = strcmp((const char*)(m->base + names[mid]), name);
      if (c == 0) {
        idx = ords[mid];
        break;
      }
      if (c < 0) lo = mid + 1;
      else hi = mid;
    }
  }
  if (idx >= nfun) return 0;
  {
    uint32_t frva = ((const uint32_t*)(m->base + RD32(ed + 0x1C)))[idx];
    if (!frva) return 0;
    if (frva >= rva && frva < rva + size) {
      /* forwarder "NTDLL.RtlAllocateHeap" ou "DLL.#12" */
      const char* fw = (const char*)(m->base + frva);
      const char* dot = strrchr(fw, '.');
      char dll[WMOD_NAME];
      struct wmod* t;
      int n;
      if (!dot || depth > 8) return 0;
      n = (int)(dot - fw);
      if (n >= WMOD_NAME - 5) return 0;
      memcpy(dll, fw, (size_t)n);
      dll[n] = 0;
      t = pe_load(dll, 0);
      if (!t) return 0;
      if (dot[1] == '#') {
        uint32_t o = 0;
        const char* q;
        for (q = dot + 2; *q >= '0' && *q <= '9'; q++) o = o * 10 + (uint32_t)(*q - '0');
        return export_lookup(t, (const char*)(uintptr_t)o, depth + 1);
      }
      return export_lookup(t, dot + 1, depth + 1);
    }
    return m->base + frva;
  }
}

/* ---------- imports ---------- */
static int resolve_imports(struct wmod* m, const struct pe_info* pi) {
  uint32_t rva, size;
  const uint8_t* d;
  dir_of(pi, DIR_IMPORT, &rva, &size);
  if (!rva) return 0;
  for (d = m->base + rva; RD32(d + 12); d += 20) {
    const char* dll = (const char*)(m->base + RD32(d + 12));
    uint32_t oft = RD32(d), ft = RD32(d + 16);
    uint64_t* iat = (uint64_t*)(m->base + ft);
    const uint64_t* names = (const uint64_t*)(m->base + (oft ? oft : ft));
    struct wmod* dep = pe_load(dll, 0);
    int i;
    if (!dep) wlog("DLL ausente (imports viram stubs): ", dll);
    for (i = 0; names[i]; i++) {
      uint64_t t = names[i];
      const char* fname;
      char ordtxt[16];
      void* fn = 0;
      if (t >> 63) {
        uint32_t o = (uint32_t)(t & 0xFFFF);
        int k = 15;
        ordtxt[k] = 0;
        do {
          ordtxt[--k] = (char)('0' + o % 10);
          o /= 10;
        } while (o && k > 1);
        ordtxt[--k] = '#';
        fname = ordtxt + k;
        if (dep) fn = export_lookup(dep, (const char*)(uintptr_t)(t & 0xFFFF), 0);
      } else {
        fname = (const char*)(m->base + (uint32_t)t + 2);
        if (dep) fn = export_lookup(dep, fname, 0);
      }
      if (!fn) {
        if (wine_debug) wlog("nao implementado (stub): ", fname);
        fn = (t >> 63) ? 0 : wine_api_stub(dll, fname);
        if (!fn) fn = wine_make_stub(dll, fname);
        if (!fn) return -1;
      }
      iat[i] = (uint64_t)(uintptr_t)fn;
    }
  }
  return 0;
}

/* ---------- TLS ---------- */
static const uint8_t* tls_dir(struct wmod* m) {
  struct pe_info pi;
  uint32_t rva, size;
  if (!m->base || pe_parse(m->base, m->size, &pi, m->name) != 0) return 0;
  dir_of(&pi, DIR_TLS, &rva, &size);
  return rva ? m->base + rva : 0;
}

void pe_tls_attach(struct wmod* m, uint8_t* teb) {
  const uint8_t* td = tls_dir(m);
  uint64_t start, end, zfill;
  void** arr;
  uint8_t* blk;
  if (!td || m->tls_index < 0) return;
  start = RD64(td);
  end = RD64(td + 8);
  zfill = RD32(td + 32);
  arr = *(void***)(teb + TEB_TLSPTR);
  if (!arr) return;
  blk = (uint8_t*)wzalloc((size_t)(end - start + zfill + 16));
  if (!blk) return;
  memcpy(blk, (const void*)(uintptr_t)start, (size_t)(end - start));
  arr[m->tls_index] = blk;
}

void pe_call_tls_callbacks(struct wmod* m, uint32_t reason) {
  const uint8_t* td = tls_dir(m);
  const uint64_t* cb;
  typedef void (WINAPI * tls_cb)(void*, uint32_t, void*);
  if (!td || !RD64(td + 24)) return;
  for (cb = (const uint64_t*)(uintptr_t)RD64(td + 24); *cb; cb++)
    ((tls_cb)(uintptr_t)*cb)(m->base, reason, 0);
}

static void tls_setup(struct wmod* m) {
  const uint8_t* td = tls_dir(m);
  struct wproc* p = wp();
  if (!td) return;
  if (p->tls_next >= TLS_SLOTS) {
    wsay(m->name, ": TLS demais");
    return;
  }
  m->tls_index = p->tls_next++;
  if (RD64(td + 16)) *(uint32_t*)(uintptr_t)RD64(td + 16) = (uint32_t)m->tls_index;
  /* A thread que esta carregando ja recebe o bloco; as novas, ao nascer. */
  pe_tls_attach(m, wteb());
}

void pe_tls_attach_all(uint8_t* teb) {
  struct wmod* m;
  for (m = wp()->mods; m; m = m->next)
    if (m->tls_index >= 0) pe_tls_attach(m, teb);
}

/* DLL_THREAD_ATTACH (2) / DLL_THREAD_DETACH (3) na thread corrente. */
void pe_thread_notify(uint32_t reason) {
  struct wmod* m;
  typedef int (WINAPI * dllmain_fn)(void*, uint32_t, void*);
  for (m = wp()->mods; m; m = m->next) {
    if (!m->base) continue;
    pe_call_tls_callbacks(m, reason);
    if (m->is_dll && m->entry) ((dllmain_fn)(uintptr_t)m->entry)(m->base, reason, 0);
  }
}

/* ---------- carga ---------- */
static int find_file(const char* name, char* out, int max) {
  char dir[160];
  char cand[200];
  struct wproc* p = wp();
  int i;
  if (strchr(name, '\\') || strchr(name, ':') || strchr(name, '/')) {
    char np[300];
    wpath(name, np, (int)sizeof(np)); /* tira o \\?\ e o \??\, troca / por \ */
    if (k_fsize(np) > 0) {
      strncpy(out, np, (size_t)max - 1);
      out[max - 1] = 0;
      return 0;
    }
    if (wine_debug) wlog("DLL nao encontrada: ", np);
    return -1;
  }
  /* pasta do EXE */
  if (p->exe && p->exe->path[0]) {
    strcpy(dir, p->exe->path);
    for (i = (int)strlen(dir) - 1; i >= 0 && dir[i] != '\\'; i--) {
    }
    dir[i + 1] = 0;
    strcpy(cand, dir);
    strcat(cand, name);
    if (k_fsize(cand) > 0) goto ok;
  }
  /* PATHSODOS\WINE\SYSTEM32, depois D:\SODOS\WINE\SYSTEM32 */
  if (k_getenv("PATHSODOS", dir, (int)sizeof(dir)) <= 0) strcpy(dir, "U:\\SODOS");
  strcpy(cand, dir);
  strcat(cand, "\\WINE\\SYSTEM32\\");
  strcat(cand, name);
  if (k_fsize(cand) > 0) goto ok;
  strcpy(cand, "D:\\SODOS\\WINE\\SYSTEM32\\");
  strcat(cand, name);
  if (k_fsize(cand) > 0) goto ok;
  return -1;
ok:
  strncpy(out, cand, (size_t)max - 1);
  out[max - 1] = 0;
  return 0;
}

void* k_win_lowmem(unsigned addr, unsigned size);

/* ---------- cache de arquivos de imagem grandes ----------
 * O Chromium abre o mesmo Claude.exe (250 MB) em varios processos (crashpad,
 * renderer, GPU). Ler do disco de novo, numa tarefa do WIN e disputando a CPU,
 * levava minutos. O arquivo fica em memoria sem dono (compartilhada) enquanto
 * houver processo do WINE; cada processo ainda monta e reloca a sua imagem. */
void* k_malloc_shared(uint32_t n);
void k_free_shared(void* p);
#define IMG_CACHE_MIN (8u << 20)
#define IMG_CACHE_MAX 8
static struct {
  char path[160];
  int size;
  uint8_t* data;
} img_cache[IMG_CACHE_MAX];
static volatile int img_lock, live_procs;
static void ic_lock(void) {
  while (__sync_lock_test_and_set(&img_lock, 1)) k_thread_yield();
}
static void ic_unlock(void) { __sync_lock_release(&img_lock); }
static void* img_cache_get(const char* path, int size) {
  int i;
  void* d = 0;
  if (size < (int)IMG_CACHE_MIN) return 0;
  ic_lock();
  for (i = 0; i < IMG_CACHE_MAX; i++)
    if (img_cache[i].data && img_cache[i].size == size && wstreq(img_cache[i].path, path)) d = img_cache[i].data;
  ic_unlock();
  return d;
}
/* file ja e memoria compartilhada: passa a ser do cache (1) ou nao coube (0) */
static int img_cache_put(const char* path, int size, uint8_t* file) {
  int i, done = 0;
  ic_lock();
  for (i = 0; i < IMG_CACHE_MAX && !done; i++)
    if (!img_cache[i].data) {
      strncpy(img_cache[i].path, path, sizeof(img_cache[i].path) - 1);
      img_cache[i].size = size;
      img_cache[i].data = file;
      done = 1;
    }
  ic_unlock();
  return done;
}
/* main.c: um processo do WINE a mais / a menos; sem nenhum, o cache sai */
void wimg_cache_proc(int delta) {
  int i;
  if (__sync_add_and_fetch(&live_procs, delta) > 0) return;
  ic_lock();
  for (i = 0; i < IMG_CACHE_MAX; i++)
    if (img_cache[i].data) {
      k_free_shared(img_cache[i].data);
      img_cache[i].data = 0;
    }
  ic_unlock();
}

struct wmod* pe_load(const char* name, int is_exe) {
  int fixed = 0;
  char path[300];
  struct wmod* m;
  struct pe_info pi;
  uint8_t* file;
  uint8_t* base;
  int fsz, i, cached = 0, high = 0;
  if (!is_exe) {
    m = pe_find(name);
    if (m) return m;
    /* DLLs do sistema sao embutidas; .dll em disco so se nao houver a embutida */
    m = builtin_load(name);
    if (m) return m;
  }
  if (find_file(name, path, (int)sizeof(path)) != 0) {
    if (is_exe) wsay("nao achei ", name);
    return 0;
  }
  fsz = k_fsize(path);
  file = (uint8_t*)img_cache_get(path, fsz);
  cached = file != 0;
  /* grande: lido direto em memoria compartilhada, que vira o cache */
  if (!file) file = (uint8_t*)(fsz >= (int)IMG_CACHE_MIN ? k_malloc_shared((unsigned)fsz + 1u)
                                                          : k_malloc((unsigned)fsz + 1u));
  if (!file) {
    wsay("sem memoria para ", path);
    return 0;
  }
  if (!cached && k_fload(path, (char*)file, fsz + 1) != fsz) {
    wsay("erro lendo ", path);
    k_free(file);
    return 0;
  }
  if (!cached && fsz >= (int)IMG_CACHE_MIN && img_cache_put(path, fsz, file)) cached = 1;
  if (pe_parse(file, (uint32_t)fsz, &pi, path) != 0) {
    if (!cached) k_free(file);
    return 0;
  }
  if (!is_exe != !!(pi.chars & 0x2000)) {
    wsay(path, is_exe ? ": e uma DLL, nao um EXE" : ": nao e uma DLL");
    if (!cached) k_free(file);
    return 0;
  }
  {
    uint32_t rrva, rsz;
    dir_of(&pi, DIR_BASERELOC, &rrva, &rsz);
    if ((!rrva || !rsz || (pi.chars & 1)) && pi.image_base < 0x100000000ull) {
      /* sem relocacoes: so serve no endereco pedido (kernel/wine.h) */
      base = (uint8_t*)k_win_lowmem((unsigned)pi.image_base, pi.size_image);
      if (!base) {
        wsay(path, ": base fixa e o endereco nao esta livre no SODOS (outro EXE de base fixa rodando, ou imagem grande demais)");
        if (!cached) k_free(file);
        return 0;
      }
      fixed = 1;
    } else {
      /* acima de 4 GB quando der (codigo x64 com relocacoes roda em qualquer
       * endereco): a arena baixa e pouca para 4 processos do Chromium com
       * 260 MB de imagem cada */
      base = (uint8_t*)k_hmalloc(pi.size_image, 0x10000);
      high = base != 0;
      if (!base) base = (uint8_t*)k_malloc_aligned(pi.size_image, 0x10000);
    }
  }
  if (!base) {
    wsay("sem memoria para a imagem de ", path);
    if (!cached) k_free(file);
    return 0;
  }
  memset(base, 0, pi.size_image);
  memcpy(base, file, pi.size_headers);
  for (i = 0; i < pi.nsect; i++) {
    const uint8_t* s = pi.sect + 40 * i;
    uint32_t vsz = RD32(s + 8), va = RD32(s + 12), rsz = RD32(s + 16), roff = RD32(s + 20);
    uint32_t n = rsz < vsz || !vsz ? rsz : vsz;
    if (va + (vsz > rsz ? vsz : rsz) > pi.size_image || roff + n > (uint32_t)fsz) {
      wsay(path, ": secao fora da imagem");
      if (!fixed) {
        if (high) k_hfree(base);
        else k_free(base);
      }
      if (!cached) k_free(file);
      return 0;
    }
    memcpy(base + va, file + roff, n);
  }
  if (!cached) k_free(file);
  /* daqui em diante a imagem mapeada tem os cabecalhos */
  if (pe_parse(base, pi.size_image, &pi, path) != 0 || relocate(base, &pi, path) != 0) {
    if (!fixed) {
      if (high) k_hfree(base);
      else k_free(base);
    }
    return 0;
  }
  m = mod_new(is_exe ? path : name);
  if (!m) return 0;
  strncpy(m->path, path, sizeof(m->path) - 1);
  m->base = base;
  m->size = pi.size_image;
  m->entry = pi.entry ? (uint64_t)(uintptr_t)(base + pi.entry) : 0;
  m->is_dll = !is_exe;
  m->stack_reserve = (uint32_t)RD64(pi.opt + 72);
  dir_of(&pi, DIR_EXCEPTION, &m->pdata_rva, &m->pdata_size);
  vm_register_image(base, pi.size_image);
  { /* VirtualQuery responde a protecao de cada secao (o Chromium confere a
     * secao "somente leitura" do base::ProtectedMemory) */
    void vm_image_prot(uint8_t* a, size_t n, DWORD characteristics); /* vm.c */
    vm_image_prot(base, pi.size_headers, 0x40000000u); /* cabecalhos: leitura */
    for (i = 0; i < pi.nsect; i++) {
      const uint8_t* s = pi.sect + 40 * i;
      uint32_t vsz = RD32(s + 8), va = RD32(s + 12), rsz = RD32(s + 16);
      vm_image_prot(base + va, vsz ? vsz : rsz, RD32(s + 36));
    }
  }
  if (is_exe) {
    wp()->exe = m;
    *(uint64_t*)(wp()->peb + PEB_IMAGEBASE) = (uint64_t)(uintptr_t)base;
  }
  wlog("carregado ", path);
  if (resolve_imports(m, &pi) != 0) return 0;
  tls_setup(m);
  if (m->is_dll) {
    typedef int (WINAPI * dllmain_fn)(void*, uint32_t, void*);
    pe_call_tls_callbacks(m, 1);
    if (m->entry && !((dllmain_fn)(uintptr_t)m->entry)(base, 1, 0)) {
      wsay(m->name, ": DllMain recusou");
      return 0;
    }
  }
  return m;
}

void pe_detach_all(void) {
  struct wmod* m;
  typedef int (WINAPI * dllmain_fn)(void*, uint32_t, void*);
  for (m = wp()->mods; m; m = m->next) {
    if (!m->base || !m->is_dll) continue;
    pe_call_tls_callbacks(m, 0);
    if (m->entry) ((dllmain_fn)(uintptr_t)m->entry)(m->base, 0, (void*)1);
  }
}

/* ---------- recursos ---------- */
static const uint8_t* res_entry(const uint8_t* rsrc, const uint8_t* dir, uintptr_t id, int any) {
  uint32_t nnamed = RD16(dir + 12), nid = RD16(dir + 14), i;
  const uint8_t* e = dir + 16;
  for (i = 0; i < nnamed + nid; i++, e += 8) {
    uint32_t name = RD32(e);
    if (any) return e;
    if (id < 0x10000) {
      if (!(name & 0x80000000u) && name == (uint32_t)id) return e;
    } else if (name & 0x80000000u) {
      const uint8_t* ns = rsrc + (name & 0x7FFFFFFFu);
      const wchar16* want = (const wchar16*)id;
      uint32_t len = RD16(ns), k;
      for (k = 0; k < len; k++) {
        wchar16 a = ((const wchar16*)(ns + 2))[k], b = want[k];
        if (a >= 'a' && a <= 'z') a = (wchar16)(a - 32);
        if (b >= 'a' && b <= 'z') b = (wchar16)(b - 32);
        if (a != b || !b) break;
      }
      if (k == len && !want[len]) return e;
    }
  }
  return 0;
}

const uint8_t* pe_find_resource(struct wmod* m, uintptr_t type, uintptr_t name, uint32_t* size) {
  struct pe_info pi;
  uint32_t rva, rsz;
  const uint8_t* rsrc;
  const uint8_t* e;
  if (!m || !m->base || pe_parse(m->base, m->size, &pi, m->name) != 0) return 0;
  dir_of(&pi, 2, &rva, &rsz);
  if (!rva) return 0;
  rsrc = m->base + rva;
  e = res_entry(rsrc, rsrc, type, 0);
  if (!e || !(RD32(e + 4) & 0x80000000u)) return 0;
  e = res_entry(rsrc, rsrc + (RD32(e + 4) & 0x7FFFFFFFu), name, 0);
  if (!e || !(RD32(e + 4) & 0x80000000u)) return 0;
  e = res_entry(rsrc, rsrc + (RD32(e + 4) & 0x7FFFFFFFu), 0, 1);
  if (!e || (RD32(e + 4) & 0x80000000u)) return 0;
  e = rsrc + RD32(e + 4); /* IMAGE_RESOURCE_DATA_ENTRY: rva, size, codepage, 0 */
  if (size) *size = RD32(e + 4);
  return e;
}
