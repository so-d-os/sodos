/*
 * map.c - Secoes (CreateFileMapping / MapViewOfFile).
 *
 * Todos os processos do SODOS estao no mesmo espaco de enderecos, entao uma
 * secao e so um bloco de memoria: cada vista devolve um ponteiro para dentro
 * dele, e duas vistas (ou dois processos, pelo nome) enxergam os mesmos bytes.
 *
 *   arquivo   o conteudo e lido para a secao; vista com escrita grava de volta
 *             no arquivo em FlushViewOfFile/UnmapViewOfFile
 *   anonima   (INVALID_HANDLE_VALUE) memoria zerada, com nome opcional
 *
 * A secao vive enquanto houver handle ou vista (contagem em refs).
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE_READWRITE 0x40
#define FILE_MAP_WRITE 0x2
#define FILE_MAP_COPY 0x1

struct section {
  struct section* next;   /* lista global (nomes) */
  uint8_t* mem;
  uint64_t size;
  int refs;
  int writable;           /* PAGE_READWRITE em arquivo: grava de volta */
  struct wobj* file;      /* arquivo de origem (referencia propria) */
  struct wproc* owner;    /* quem alocou mem (VirtualFree so dali) */
  wchar16* name;
  uint8_t* raw;           /* != 0: mem em memoria sem dono (wshared_high_alloc ou k_malloc_shared) */
  uint32_t magic;         /* SEC_MAGIC enquanto viva (section_unref confere) */
};
/* O GPU do Chrome caiu em section_unref lendo next = -1: uma secao ja solta
 * continuava ligada na lista (soltura dupla?). A secao viva tem SEC_MAGIC; a
 * ultima referencia o apaga, e um section_unref a mais e registrado e
 * ignorado em vez de soltar de novo. */
#define SEC_MAGIC 0x54434553u /* "SECT" */
/* Vistas: lista unica para todos os processos (o WINE.DLL e um so), entao
 * cada vista sabe o dono e fica em memoria sem dono; ao sair, o processo
 * tira as dele (wmap_proc_exit). Antes a vista era memoria do processo e,
 * quando o renderer saia, o kernel a soltava ainda ligada na lista: o
 * navegador caia ao percorrer. Dois processos podem mapear a mesma secao no
 * mesmo endereco: a busca confere o dono. */
struct view {
  struct view* next;
  uint8_t* base;
  uint64_t off, size;
  struct section* s;
  int write;
  struct wproc* owner;
  uint64_t alias;         /* != 0: reserva propria (k_vm_alias) a soltar no Unmap */
  uint64_t dead_ms;       /* na lista grave: hora em que o dono saiu */
};
uint64_t k_vm_reserve(uint64_t size, uint64_t align, uint64_t at);
int k_vm_alias(uint64_t va, uint64_t phys, uint64_t size, int prot);
int k_vm_release(uint64_t va);
static struct section* sections;
static struct view* views;
static void grave_reap(void);
static struct view* grave; /* vistas de processos que sairam (ver wmap_proc_exit) */
static volatile int map_lock;

static void mlock(void) {
  while (__sync_lock_test_and_set(&map_lock, 1)) k_thread_yield();
}
static void munlock(void) { __sync_lock_release(&map_lock); }

typedef void*(WINAPI* va_fn)(void*, size_t, DWORD, DWORD);
typedef BOOL(WINAPI* vf_fn)(void*, size_t, DWORD);
typedef BOOL(WINAPI* sfp_fn)(HANDLE, int64_t, int64_t*, DWORD);
typedef BOOL(WINAPI* rw_fn)(HANDLE, void*, DWORD, DWORD*, void*);
typedef BOOL(WINAPI* gfs_fn)(HANDLE, int64_t*);

static int name_eq(const wchar16* a, const wchar16* b) {
  for (; *a && *a == *b; a++, b++) {
  }
  return *a == *b;
}
static const wchar16* name_base(const wchar16* n) {
  /* "Local\\x" e "Global\\x" sao o mesmo espaco no SODOS */
  const wchar16* p;
  for (p = n; *p; p++)
    if (*p == '\\') return p + 1;
  return n;
}

static void file_io(struct wobj* f, int write, uint64_t off, uint8_t* buf, uint64_t n) {
  sfp_fn sfp = (sfp_fn)builtin_lookup("SetFilePointerEx");
  rw_fn io = (rw_fn)builtin_lookup(write ? "WriteFile" : "ReadFile");
  HANDLE h;
  int64_t cur = 0;
  if (!f || !sfp || !io) return;
  h = wh_new(f); /* handle temporario (a secao tem a propria referencia) */
  if (!h) return;
  sfp(h, 0, &cur, 1);
  sfp(h, (int64_t)off, 0, 0);
  while (n) {
    DWORD chunk = n > 0x1000000u ? 0x1000000u : (DWORD)n, done = 0;
    if (!io(h, buf, chunk, &done, 0) || !done) break;
    buf += done;
    n -= done;
  }
  sfp(h, cur, 0, 0);
  wh_close(h);
}

void wfile_close(struct wobj* o); /* file.c */
static void section_unref(struct section* s) {
  int last;
  mlock();
  if (!s || s->magic != SEC_MAGIC || s->refs <= 0) {
    munlock();
    wlogx("section_unref: secao invalida ou ja solta, ignorada ", (uint64_t)(uintptr_t)s);
    wwait_note("  chamado de", __builtin_return_address(0), 0);
    return;
  }
  last = --s->refs <= 0;
  if (last) {
    struct section** pp;
    for (pp = &sections; *pp; pp = &(*pp)->next)
      if (*pp == s) {
        *pp = s->next;
        break;
      }
    s->magic = 0;
  }
  munlock();
  if (!last) return;
  if (s->raw) {
    wshared_free(s->raw);
  } else if (s->mem && s->owner == wp()) {
    vf_fn vf = (vf_fn)builtin_lookup("VirtualFree");
    if (vf) vf(s->mem, 0, 0x8000);
  }
  if (s->file) {
    wlock();
    last = --s->file->refs <= 0;
    wunlock();
    if (last) {
      wfile_close(s->file);
      k_free(s->file);
    }
  }
  k_free(s->name);
  k_free(s);
}

/* chamado pelo wh_close (obj.c) quando o handle da secao some */
void wsection_close(struct wobj* o) {
  if (o->p1) section_unref((struct section*)o->p1);
  o->p1 = 0;
}

static HANDLE section_handle(struct section* s) {
  struct wobj* o = wobj_new(OBJ_SECTION);
  HANDLE h;
  if (!o) return 0;
  o->p1 = s;
  h = wh_new(o);
  if (!h) k_free(o);
  return h;
}

static WINAPI HANDLE CreateFileMappingW(HANDLE f, void* sa, DWORD prot, DWORD hi, DWORD lo, const wchar16* name) {
  va_fn va = (va_fn)builtin_lookup("VirtualAlloc");
  struct section* s;
  struct wobj* fo = 0;
  uint64_t size = ((uint64_t)hi << 32) | lo;
  HANDLE h;
  (void)sa;
  if (wine_debug) wlogx("CreateFileMapping prot ", prot);
  if (name && *name) { /* ja existe: o mesmo objeto, com ERROR_ALREADY_EXISTS */
    mlock();
    for (s = sections; s; s = s->next)
      if (s->name && name_eq(s->name, name_base(name))) {
        s->refs++;
        break;
      }
    munlock();
    if (s) {
      h = section_handle(s);
      if (!h) section_unref(s);
      wset_lasterr(183);
      return h;
    }
  }
  if (f && f != (HANDLE)(intptr_t)-1) {
    gfs_fn gfs = (gfs_fn)builtin_lookup("GetFileSizeEx");
    int64_t fsz = 0;
    fo = wh_get(f, OBJ_FILE);
    if (!fo || !gfs || !gfs(f, &fsz)) {
      wlogx("CreateFileMapping: handle de arquivo invalido ", (uint64_t)(uintptr_t)f);
      wset_lasterr(ERROR_INVALID_HANDLE);
      return 0;
    }
    if (!size) size = (uint64_t)fsz;
    if (!size) {
      wset_lasterr(1006); /* ERROR_FILE_INVALID: arquivo vazio */
      return 0;
    }
  } else if (!size) {
    wset_lasterr(ERROR_INVALID_PARAMETER);
    return 0;
  }
  /* A secao (e a memoria) nao tem dono: o Chromium passa secoes aos filhos
   * (field trials, Mojo) e o pai pode sair antes; memoria de processo o
   * kernel solta quando ele termina. */
  s = (struct section*)k_malloc_shared((uint32_t)sizeof(*s));
  if (!s) {
    wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  memset(s, 0, sizeof(*s));
  s->size = size;
  if (size < 0xF0000000ull) {
    /* acima de 4 GB quando o kernel deixa, ja alinhada em 64 KB: os pipes de
     * 2 MB do Mojo enchiam a arena baixa. Senao, na arena baixa com folga
     * para alinhar. */
    s->raw = (uint8_t*)wshared_high_alloc(size ? size : 1, 0x10000);
    if (s->raw) {
      s->mem = s->raw;
    } else {
      s->raw = (uint8_t*)k_malloc_shared((uint32_t)size + 0xFFFFu);
      if (s->raw) s->mem = (uint8_t*)(((uintptr_t)s->raw + 0xFFFFu) & ~(uintptr_t)0xFFFFu);
    }
  }
  if (s->raw) {
    memset(s->mem, 0, (size_t)size);
  } else {
    s->mem = va ? (uint8_t*)va(0, (size_t)size, 0x3000, PAGE_READWRITE) : 0; /* zerada */
  }
  if (!s->mem) {
    wlogx("CreateFileMapping: sem memoria para ", size);
    k_free(s);
    wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  s->owner = wp();
  s->refs = 1;
  s->magic = SEC_MAGIC;
  if (fo) {
    gfs_fn gfs = (gfs_fn)builtin_lookup("GetFileSizeEx");
    int64_t fsz = 0;
    gfs(f, &fsz);
    wlock();
    fo->refs++;
    wunlock();
    s->file = fo;
    s->writable = (prot & 0xFF) == PAGE_READWRITE || (prot & 0xFF) == PAGE_EXECUTE_READWRITE;
    file_io(fo, 0, 0, s->mem, (uint64_t)fsz < size ? (uint64_t)fsz : size);
  }
  if (name && *name) {
    const wchar16* b = name_base(name);
    size_t n = (wstrlen16(b) + 1) * 2;
    s->name = (wchar16*)k_malloc_shared((uint32_t)n);
    if (s->name) memcpy(s->name, b, n);
  }
  mlock();
  s->next = sections;
  sections = s;
  munlock();
  h = section_handle(s);
  if (!h) section_unref(s);
  wset_lasterr(0);
  return h;
}
static WINAPI HANDLE CreateFileMappingA(HANDLE f, void* sa, DWORD prot, DWORD hi, DWORD lo, const char* name) {
  wchar16* w = name ? wdup8to16(name) : 0;
  HANDLE h = CreateFileMappingW(f, sa, prot, hi, lo, w);
  k_free(w);
  return h;
}
static WINAPI HANDLE OpenFileMappingW(DWORD acc, BOOL inh, const wchar16* name) {
  struct section* s;
  HANDLE h;
  (void)acc;
  (void)inh;
  if (!name) return 0;
  mlock();
  for (s = sections; s; s = s->next)
    if (s->name && name_eq(s->name, name_base(name))) {
      s->refs++;
      break;
    }
  munlock();
  if (!s) {
    wset_lasterr(ERROR_FILE_NOT_FOUND);
    return 0;
  }
  h = section_handle(s);
  if (!h) section_unref(s);
  return h;
}

/* DuplicateHandle de uma secao sem FILE_MAP_WRITE (obj.c): outro objeto
 * para a mesma secao, marcado somente leitura (o Chromium confere que uma
 * regiao "read-only" nao mapeia para escrita). */
HANDLE wsection_dup(struct wobj* o, int readonly) {
  struct section* s = (struct section*)o->p1;
  struct wobj* n;
  HANDLE h;
  if (!s) return 0;
  n = wobj_new(OBJ_SECTION);
  if (!n) return 0;
  mlock();
  s->refs++;
  munlock();
  n->p1 = s;
  n->u1 = (readonly || (o->u1 & 1)) ? 1 : 0;
  h = wh_new(n);
  if (!h) {
    section_unref(s);
    k_free(n);
  }
  return h;
}

/* gdi.c (CreateDIBSection com hSection): endereco dentro da secao */
uint8_t* wsection_ptr(HANDLE sec, uint64_t off, uint64_t need) {
  struct wobj* o = wh_get(sec, OBJ_SECTION);
  struct section* s = o ? (struct section*)o->p1 : 0;
  if (!s || !s->mem || off > s->size || need > s->size - off) return 0;
  return s->mem + off;
}

/* SectionBasicInformation: {BaseAddress, AllocationAttributes, MaximumSize} */
static WINAPI long NtQuerySection(HANDLE m, int cls, void* buf, DWORD len, DWORD* ret) {
  struct wobj* o = wh_get(m, OBJ_SECTION);
  struct section* s = o ? (struct section*)o->p1 : 0;
  uint8_t* b = (uint8_t*)buf;
  if (!s) return (long)0xC0000008; /* STATUS_INVALID_HANDLE */
  if (cls == 1) return (long)0xC0000049; /* SectionImageInformation: STATUS_SECTION_NOT_IMAGE */
  if (cls != 0) return (long)0xC0000003;
  if (ret) *ret = 24;
  if (!b || len < 24) return (long)0xC0000004;
  memset(b, 0, 24);
  *(DWORD*)(b + 8) = s->file ? 0x8000000u : 0x8000000u; /* SEC_COMMIT */
  *(uint64_t*)(b + 16) = s->size;
  return 0;
}

static WINAPI void* MapViewOfFileEx(HANDLE m, DWORD acc, DWORD hi, DWORD lo, size_t n, void* base) {
  struct wobj* o = wh_get(m, OBJ_SECTION);
  struct section* s = o ? (struct section*)o->p1 : 0;
  uint64_t off = ((uint64_t)hi << 32) | lo;
  struct view* v;
  if (!s) {
    wlogx("MapViewOfFile: handle de secao invalido ", (uint64_t)(uintptr_t)m);
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  if ((o->u1 & 1) && (acc & FILE_MAP_WRITE)) {
    wset_lasterr(5); /* ERROR_ACCESS_DENIED: handle somente leitura */
    return 0;
  }
  if (wine_debug) wlogx("MapViewOfFile: vista de ", n ? (uint64_t)n : s->size - off);
  if (off > s->size || (n && off + n > s->size)) {
    wset_lasterr(8); /* ERROR_NOT_ENOUGH_MEMORY, como o Windows */
    return 0;
  }
  if (base && (uint8_t*)base != s->mem + off) {
    wlogx("MapViewOfFile: endereco pedido ", (uint64_t)(uintptr_t)base);
    wset_lasterr(487); /* ERROR_INVALID_ADDRESS: so onde a secao ja esta */
    return 0;
  }
  grave_reap();
  v = (struct view*)k_malloc_shared((uint32_t)sizeof(*v));
  if (!v) return 0;
  memset(v, 0, sizeof(*v));
  v->owner = wp();
  v->base = s->mem + off;
  v->off = off;
  v->size = n ? n : s->size - off;
  v->s = s;
  v->write = (acc & FILE_MAP_WRITE) && !(acc & FILE_MAP_COPY);
  mlock();
  if (!base && s->raw && !((uintptr_t)(s->mem + off) & 0xFFF)) {
    /* este processo ja tem uma vista da secao? a nova ganha endereco proprio */
    struct view* w;
    int again = 0;
    for (w = views; w; w = w->next)
      if (w->owner == v->owner && w->s == s) again = 1;
    if (again) {
      uint64_t len = (v->size + 0xFFF) & ~0xFFFull;
      uint64_t va = k_vm_reserve(len, 0x10000, 0);
      if (va && k_vm_alias(va, (uint64_t)(uintptr_t)(s->mem + off), len, 2 /* VMEM_RW */)) {
        v->alias = va;
        v->base = (uint8_t*)(uintptr_t)va;
        if (wine_debug >= 2) wlogx("MapViewOfFile: vista repetida em endereco proprio ", va);
      } else if (va) {
        k_vm_release(va);
      }
    }
  }
  s->refs++;
  v->next = views;
  views = v;
  munlock();
  return v->base;
}
static WINAPI void* MapViewOfFile(HANDLE m, DWORD acc, DWORD hi, DWORD lo, size_t n) {
  return MapViewOfFileEx(m, acc, hi, lo, n, 0);
}

static struct view* view_of(const void* p, int unlink) {
  struct view** pp;
  struct wproc* me = wp();
  for (pp = &views; *pp; pp = &(*pp)->next)
    if ((*pp)->owner == me && (const uint8_t*)p >= (*pp)->base && (const uint8_t*)p < (*pp)->base + (*pp)->size) {
      struct view* v = *pp;
      if (unlink) *pp = v->next;
      return v;
    }
  return 0;
}
/* VirtualQuery (vm.c) num endereco de vista: a vista e a "alocacao". O
 * Chromium mede o mapeamento por RegionSize - (endereco - AllocationBase);
 * antes vinha MEM_FREE com base 0 e o DiscardableSharedMemory do navegador
 * via tamanho absurdo e recusava a memoria (renderer: sem memoria, imagens do
 * captcha em branco). 1 = era vista. */
int wview_query(const void* p, uint64_t* base, uint64_t* size, int* write) {
  struct view* v;
  int ok = 0;
  mlock();
  v = view_of(p, 0);
  if (v) {
    *base = (uint64_t)(uintptr_t)v->base & ~0xFFFull;
    *size = (((uint64_t)(uintptr_t)v->base + v->size + 0xFFF) & ~0xFFFull) - *base;
    *write = v->write;
    ok = 1;
  }
  munlock();
  return ok;
}
static void writeback(struct view* v, uint64_t off, uint64_t n) {
  if (v->write && v->s->writable && v->s->file) file_io(v->s->file, 1, v->off + off, v->base + off, n);
}
static WINAPI BOOL FlushViewOfFile(const void* p, size_t n) {
  struct view* v;
  uint64_t off;
  mlock();
  v = view_of(p, 0);
  munlock();
  if (!v) return 1;
  off = (uint64_t)((const uint8_t*)p - v->base);
  if (!n || off + n > v->size) n = (size_t)(v->size - off);
  writeback(v, off, n);
  return 1;
}
static void grave_reap(void);
static WINAPI BOOL UnmapViewOfFile(const void* p) {
  struct view* v;
  grave_reap();
  mlock();
  v = view_of(p, 1);
  munlock();
  if (!v) {
    wset_lasterr(487);
    return 0;
  }
  writeback(v, 0, v->size);
  if (v->alias) {
    if (wine_debug >= 2) {
      wlogx("UnmapViewOfFile: solta vista repetida ", v->alias);
      wwait_note("  chamado de", __builtin_return_address(0), 0);
    }
    k_vm_release(v->alias);
  }
  section_unref(v->s);
  k_free(v);
  return 1;
}
/* main.c (wexit): as vistas deste processo saem da lista. As outras threads
 * do processo ainda rodam aqui (o kernel so as para no fim do exec) e a de
 * E/S do Mojo escrevia na vista depois de solta: ACCESS_VIOLATION "fora de
 * qualquer reserva" no renderer saindo, ou escrita em memoria ja liberada.
 * Entao as vistas vao para a lista grave e so sao soltas 10 s depois
 * (grave_reap). A reserva da vista repetida nunca e solta aqui: o kernel solta
 * as reservas do pid no fim (vmem_release_pid); um k_vm_release atrasado
 * soltaria o endereco ja reaproveitado por outro processo. */
void wmap_proc_exit(void) {
  struct wproc* me = wp();
  uint64_t now = wnow_ms();
  for (;;) {
    struct view** pp;
    struct view* v = 0;
    mlock();
    for (pp = &views; *pp; pp = &(*pp)->next)
      if ((*pp)->owner == me) {
        v = *pp;
        *pp = v->next;
        break;
      }
    if (v) {
      v->owner = 0;
      v->dead_ms = now ? now : 1;
      v->next = grave;
      grave = v;
    }
    munlock();
    if (!v) return;
    writeback(v, 0, v->size);
  }
}
/* solta as vistas da lista grave com mais de 10 s */
static void grave_reap(void) {
  uint64_t now = wnow_ms();
  for (;;) {
    struct view** pp;
    struct view* v = 0;
    if (!grave) return;
    mlock();
    for (pp = &grave; *pp; pp = &(*pp)->next)
      if (now - (*pp)->dead_ms > 10000) {
        v = *pp;
        *pp = v->next;
        break;
      }
    munlock();
    if (!v) return;
    section_unref(v->s);
    k_free(v);
  }
}

static WINAPI BOOL UnmapViewOfFileEx(const void* p, DWORD fl) {
  (void)fl;
  return UnmapViewOfFile(p);
}

#define S(n) {#n, (void*)n}
const struct wine_sym map_syms[] = {S(CreateFileMappingW), S(CreateFileMappingA), S(OpenFileMappingW),
                                    S(MapViewOfFileEx),    S(MapViewOfFile),      S(FlushViewOfFile),
                                    S(UnmapViewOfFile),    S(UnmapViewOfFileEx),  S(NtQuerySection),
                                    {"ZwQuerySection", (void*)NtQuerySection}, {0, 0}};
