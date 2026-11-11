/* elf.c - Carregador ELF64 dinamico: executaveis PIE e bibliotecas .so.
 *
 * E o "ld.so" do SODOS, dentro do kernel. Um objeto ET_DYN e mapeado no heap
 * alto, as bibliotecas de DT_NEEDED sao carregadas em largura (a ordem que
 * define o escopo global de simbolos), tudo e relocado e so entao o programa
 * comeca. Os inicializadores (DT_INIT/DT_INIT_ARRAY) nao rodam aqui: a libc do
 * programa os pede em ordem por k_dl_init_next, ja com pilha e TLS montados.
 *
 * Bibliotecas tem nome padrao (libc.so.6, libm.so.6, libfoo.so.N) e sao
 * procuradas no DT_RUNPATH, ao lado de quem pediu, ao lado do executavel e em
 * \LIB e \USR\LIB das unidades. "libkernel.so" nao e arquivo: e a tabela de
 * exports do kernel (kexports.c), consultada por ultimo em toda resolucao.
 *
 * Cada processo tem a sua copia de cada biblioteca (como nas DLLs SOX) e ela
 * vive ate o processo terminar: dlclose nao descarrega. */
#include <stdint.h>
#include "module_priv.h"
#include "mm.h"
#include "serial.h"
#include "kexports.h"

/* ---------- formato ELF64 (so o que o carregador le) ---------- */
typedef struct {
  uint8_t e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} Elf64_Phdr;

typedef struct {
  int64_t d_tag;
  uint64_t d_val;
} Elf64_Dyn;

typedef struct {
  uint32_t st_name;
  uint8_t st_info;
  uint8_t st_other;
  uint16_t st_shndx;
  uint64_t st_value;
  uint64_t st_size;
} Elf64_Sym;

typedef struct {
  uint64_t r_offset;
  uint64_t r_info;
  int64_t r_addend;
} Elf64_Rela;

#define ET_DYN 3
#define EM_X86_64 62

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_TLS 7
#define PT_GNU_STACK 0x6474e551u

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_STRSZ 10
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
#define DT_RPATH 15
#define DT_SYMBOLIC 16
#define DT_REL 17
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RUNPATH 29
#define DT_FLAGS 30
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_GNU_HASH 0x6ffffef5
#define DF_SYMBOLIC 2u

#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STB_GNU_UNIQUE 10
#define STT_FUNC 2
#define STT_TLS 6
#define STT_GNU_IFUNC 10

#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64 18
#define R_X86_64_IRELATIVE 37

#define ELF_PH_MAX 32
#define ELF_DEPS_MAX 64
/* Pilha de um executavel ELF sem PT_GNU_STACK com tamanho (ld -z stack-size). */
#define ELF_STACK_DEFAULT 0x800000u
/* Folga do TLS estatico: o bloco de cada thread ja nasce com espaco para as
 * bibliotecas com thread_local que o programa abrir depois, com dlopen. */
#define TLS_SURPLUS 0x10000u
#define MISSING_LOG_MAX 24

struct mod_elf {
  const Elf64_Dyn* dyn;
  const Elf64_Sym* sym;
  const char* str;
  uint64_t strsz;
  uint32_t nsym;
  const uint32_t* hash;
  const uint32_t* gnu;
  const Elf64_Rela* rela;
  uint64_t relasz;
  const Elf64_Rela* jmprel;
  uint64_t jmprelsz;
  const uint64_t* relr;
  uint64_t relrsz;
  uint64_t init, fini;
  uint64_t init_array, init_arraysz;
  uint64_t fini_array, fini_arraysz;
  uint64_t phdr;
  uint32_t phnum;
  uint64_t tls_image, tls_filesz, tls_memsz, tls_align;
  uint64_t tls_offset; /* distancia do ponteiro de thread ate o inicio do bloco */
  uint64_t tls_static; /* so no executavel: tamanho da area estatica do processo */
  uint32_t tls_modid;
  uint32_t seq;        /* ordem de carga: define o escopo global */
  uint32_t init_idx;   /* ordem de inicializacao: dependencias primeiro */
  int root;            /* executavel ou dlopen que trouxe o modulo */
  uint8_t global;
  uint8_t tls_closed;  /* so no executavel: a area estatica ja tem tamanho fixo */
  uint8_t inited;
  uint8_t finied;
  uint8_t symbolic;
  uint8_t mark;
  const char* soname;
  const char* runpath;
  int deps[ELF_DEPS_MAX];
  int ndeps;
  char path[DRV_PATH];
};

static uint32_t g_seq;
static uint32_t g_init_seq;
static int scope_ids[MOD_MAX];
static int scope_n;

/* ---------- utilitarios ---------- */
static char up(char c) {
  if (c >= 'a' && c <= 'z') return (char)(c - 32);
  return c;
}

static int same_name(const char* a, const char* b) {
  while (*a && *b) {
    if (up(*a) != up(*b)) return 0;
    a++;
    b++;
  }
  return *a == *b;
}

static int str_eq(const char* a, const char* b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static int str_len(const char* s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static int is_sep(char c) { return c == '\\' || c == '/'; }

static const char* path_base(const char* p) {
  const char* b = p;
  for (; *p; p++)
    if (is_sep(*p) || *p == ':') b = p + 1;
  return b;
}

static void zero(void* p, uint64_t n) {
  uint8_t* b = (uint8_t*)p;
  while (n && ((uintptr_t)b & 7u)) {
    *b++ = 0;
    n--;
  }
  for (; n >= 8; n -= 8, b += 8) *(uint64_t*)b = 0;
  while (n--) *b++ = 0;
}

static void copy(void* dst, const void* src, uint64_t n) {
  uint8_t* d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
  while (n--) *d++ = *s++;
}

/* file_read_at le com offset de 32 bits; pedacos de 64 MB mantem o int positivo. */
static int read_exact(const char* path, uint64_t off, void* buf, uint64_t len) {
  uint8_t* p = (uint8_t*)buf;
  while (len) {
    int chunk = len > 0x4000000u ? 0x4000000 : (int)len;
    if (off > 0xFFFFFFFFull || file_read_at(path, (uint32_t)off, (char*)p, chunk) != chunk) return -1;
    p += chunk;
    off += (uint64_t)chunk;
    len -= (uint64_t)chunk;
  }
  return 0;
}

static void log2s(const char* a, const char* b) {
  serial_puts(a);
  serial_puts(b);
}

static struct module* exe_of(uint32_t pid) {
  int i;
  for (i = 0; i < MOD_MAX; i++)
    if (mods[i].used && mods[i].elf && !mods[i].is_dll && mods[i].pid == pid) return &mods[i];
  return 0;
}

static int is_mine(int id) {
  return id >= 0 && id < MOD_MAX && mods[id].used && mods[id].elf &&
         mods[id].pid == mod_current_pid();
}

int elf_probe(const char* path) {
  uint8_t h[4];
  if (file_read_at(path, 0, (char*)h, 4) != 4) return 0;
  return h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F';
}

/* ---------- caminhos ---------- */
/* "/D/lib/x.so" -> "D:\lib\x.so" e '/' -> '\', como a camada POSIX dos programas. */
static void dos_path(const char* in, char* out, int max) {
  int i = 0, j = 0;
  if (in[0] == '/' && ((in[1] >= 'A' && in[1] <= 'Z') || (in[1] >= 'a' && in[1] <= 'z')) &&
      (in[2] == '/' || in[2] == 0)) {
    out[j++] = up(in[1]);
    out[j++] = ':';
    i = 2;
    if (!in[2]) out[j++] = '\\';
  }
  while (in[i] && j < max - 1) {
    out[j++] = in[i] == '/' ? '\\' : in[i];
    i++;
  }
  out[j] = '\0';
}

static void dir_of(const char* path, char* out, int max) {
  const char* b = path_base(path);
  int n = (int)(b - path);
  int i;
  if (n > max - 1) n = max - 1;
  for (i = 0; i < n; i++) out[i] = path[i];
  out[n] = '\0';
}

/* dir + nome, com uma barra entre eles; 0 se o arquivo existe. */
static int try_dir(const char* dir, int dir_len, const char* name, char* out, int max) {
  char tmp[DRV_PATH];
  int j = 0, i;
  for (i = 0; i < dir_len && j < DRV_PATH - 2; i++) tmp[j++] = dir[i];
  if (j > 0 && !is_sep(tmp[j - 1]) && tmp[j - 1] != ':') tmp[j++] = '\\';
  for (i = 0; name[i] && j < DRV_PATH - 1; i++) tmp[j++] = name[i];
  tmp[j] = '\0';
  dos_path(tmp, out, max);
  return file_size(out) >= 0 ? 0 : -1;
}

/* DT_RUNPATH: entradas separadas por ':' ou ';'. O ':' de "D:\LIB" nao separa.
 * $ORIGIN (ou ${ORIGIN}) e a pasta do modulo que pede. */
static int origin_len(const char* p) {
  static const char a[] = "$ORIGIN";
  static const char b[] = "${ORIGIN}";
  int i;
  for (i = 0; a[i] && p[i] == a[i]; i++) {}
  if (!a[i]) return i;
  for (i = 0; b[i] && p[i] == b[i]; i++) {}
  return b[i] ? 0 : i;
}

static int try_runpath(const char* rp, const char* origin, const char* name, char* out, int max) {
  char ent[DRV_PATH];
  while (*rp) {
    int n = 0;
    while (*rp && *rp != ';' && !(*rp == ':' && !(n == 1 && is_sep(rp[1])))) {
      int ol = origin_len(rp);
      if (ol) {
        const char* o = origin;
        while (*o && n < DRV_PATH - 1) ent[n++] = *o++;
        rp += ol;
        continue;
      }
      if (n < DRV_PATH - 1) ent[n++] = *rp;
      rp++;
    }
    if (*rp) rp++;
    if (n && try_dir(ent, n, name, out, max) == 0) return 0;
  }
  return -1;
}

static int elf_locate(const char* name, const struct module* req, char* out, int max) {
  static const char* const sys_dirs[] = { "\\LIB", "\\USR\\LIB" };
  char dir[DRV_PATH];
  char order[4];
  int n = 0, i, k;
  const char* p;
  int has_path = 0;
  for (p = name; *p; p++)
    if (is_sep(*p) || *p == ':') has_path = 1;
  if (has_path) {
    dos_path(name, out, max);
    return file_size(out) >= 0 ? 0 : -1;
  }
  if (req && req->elf) {
    dir_of(req->elf->path, dir, sizeof(dir));
    if (req->elf->runpath && try_runpath(req->elf->runpath, dir, name, out, max) == 0) return 0;
    if (try_dir(dir, str_len(dir), name, out, max) == 0) return 0;
  }
  {
    struct module* exe = exe_of(mod_current_pid());
    if (exe && exe != req) {
      dir_of(exe->elf->path, dir, sizeof(dir));
      if (exe->elf->runpath && try_runpath(exe->elf->runpath, dir, name, out, max) == 0) return 0;
      if (try_dir(dir, str_len(dir), name, out, max) == 0) return 0;
    }
  }
  if (mod_exe_drive()) order[n++] = mod_exe_drive();
  order[n++] = drives_current();
  order[n++] = 'D';
  order[n++] = 'C';
  for (i = 0; i < n; i++) {
    int dup = 0;
    for (k = 0; k < i; k++)
      if (up(order[k]) == up(order[i])) dup = 1;
    if (dup || !drives_is_mounted(order[i])) continue;
    for (k = 0; k < 2; k++) {
      dir[0] = order[i];
      dir[1] = ':';
      dir[2] = '\0';
      {
        int j = 2;
        const char* s = sys_dirs[k];
        while (*s) dir[j++] = *s++;
        dir[j] = '\0';
        if (try_dir(dir, j, name, out, max) == 0) return 0;
      }
    }
  }
  return mod_locate(name, mod_exe_drive(), out, max);
}

/* A biblioteca do kernel nao e um arquivo. */
static int is_kernel_lib(const char* name) {
  const char* b = path_base(name);
  return same_name(b, "libkernel.so") || same_name(b, "KERNEL");
}

static int elf_find_loaded(const char* name) {
  const char* b = path_base(name);
  uint32_t pid = mod_current_pid();
  int i;
  for (i = 0; i < MOD_MAX; i++) {
    if (!mods[i].used || !mods[i].elf || mods[i].pid != pid) continue;
    if (mods[i].elf->soname && same_name(mods[i].elf->soname, b)) return i;
    if (same_name(path_base(mods[i].elf->path), b)) return i;
  }
  return -1;
}

/* ---------- mapeamento ---------- */
static void elf_discard(int id) {
  struct module* m = &mods[id];
  if (!m->used) return;
  ufree(m->raw);
  ufree(m->elf);
  m->elf = 0;
  m->used = 0;
}

static uint32_t gnu_nsyms(const uint32_t* g) {
  uint32_t nb = g[0], symoff = g[1], bloom = g[2];
  const uint32_t* buckets = (const uint32_t*)((const uint64_t*)(g + 4) + bloom);
  const uint32_t* chain = buckets + nb;
  uint32_t max = 0, b;
  for (b = 0; b < nb; b++)
    if (buckets[b] > max) max = buckets[b];
  if (max < symoff) return symoff;
  while (!(chain[max - symoff] & 1u)) max++;
  return max + 1;
}

/* Le o arquivo para a memoria e interpreta o segmento dinamico. Nao reloca. */
static int elf_map(const char* path, int want_dll) {
  Elf64_Ehdr eh;
  Elf64_Phdr ph[ELF_PH_MAX];
  struct module* m;
  struct mod_elf* e;
  char bname[32];
  uint64_t lo = ~0ull, hi = 0, align = 4096, span;
  uint64_t dyn_off = 0, dyn_size = 0;
  uint32_t stack = 0;
  int id, i;
  mod_base_name(path, bname, sizeof(bname));
  if (file_read_at(path, 0, (char*)&eh, (int)sizeof(eh)) != (int)sizeof(eh) || eh.e_ident[0] != 0x7f ||
      eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F') {
    mod_set_err("nao e ELF: ", bname);
    return -1;
  }
  if (eh.e_ident[4] != 2 || eh.e_ident[5] != 1 || eh.e_machine != EM_X86_64) {
    mod_set_err("ELF de outra arquitetura: ", bname);
    return -1;
  }
  if (eh.e_type != ET_DYN) {
    mod_set_err("ELF sem relocacao (use -pie): ", bname);
    return -1;
  }
  if (eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0 || eh.e_phnum > ELF_PH_MAX ||
      read_exact(path, eh.e_phoff, ph, (uint64_t)eh.e_phnum * sizeof(Elf64_Phdr)) != 0) {
    mod_set_err("cabecalho ELF invalido: ", bname);
    return -1;
  }
  for (i = 0; i < eh.e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD) continue;
    if (ph[i].p_memsz < ph[i].p_filesz || ph[i].p_vaddr + ph[i].p_memsz < ph[i].p_vaddr) {
      mod_set_err("segmento ELF invalido: ", bname);
      return -1;
    }
    if ((ph[i].p_vaddr & ~4095ull) < lo) lo = ph[i].p_vaddr & ~4095ull;
    if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
    if (ph[i].p_align > align && ph[i].p_align <= 0x200000u && !(ph[i].p_align & (ph[i].p_align - 1)))
      align = ph[i].p_align;
  }
  if (hi == 0 || hi >= 0xF0000000ull || (!want_dll && eh.e_entry >= hi)) {
    mod_set_err("imagem ELF invalida: ", bname);
    return -1;
  }
  span = hi - lo;
  id = mod_free_slot();
  if (id < 0) {
    mod_set_err("modulos demais", 0);
    return -1;
  }
  m = &mods[id];
  e = (struct mod_elf*)umalloc_owned((uint32_t)sizeof(*e), 0);
  m->raw = (uint8_t*)umalloc_owned((uint32_t)(span + align), 0);
  if (!e || !m->raw) {
    ufree(e);
    ufree(m->raw);
    mod_set_err("sem memoria para ", bname);
    return -1;
  }
  zero(e, sizeof(*e));
  m->base = (uint8_t*)((((uintptr_t)m->raw + align - 1) & ~(uintptr_t)(align - 1)) - lo);
  zero(m->base + lo, span);
  m->used = 1;
  m->refs = 1;
  m->is_dll = want_dll;
  m->is64 = 1;
  m->pid = mod_current_pid();
  for (i = 0; i < (int)sizeof(m->name) - 1 && bname[i]; i++) m->name[i] = bname[i];
  m->name[i] = '\0';
  m->size = (uint32_t)hi;
  m->entry = want_dll ? SOX_NO_ENTRY : (uint32_t)eh.e_entry;
  m->exports = 0;
  m->nexports = 0;
  m->strtab = 0;
  m->strtab_size = 0;
  m->ndeps = 0;
  m->elf = e;
  for (i = 0; i < DRV_PATH - 1 && path[i]; i++) e->path[i] = path[i];
  e->seq = ++g_seq;
  e->root = id;
  for (i = 0; i < eh.e_phnum; i++) {
    const Elf64_Phdr* p = &ph[i];
    if (p->p_type == PT_LOAD) {
      if (p->p_filesz && read_exact(path, p->p_offset, m->base + p->p_vaddr, p->p_filesz) != 0) {
        mod_set_err("erro lendo ", bname);
        elf_discard(id);
        return -1;
      }
      /* Tabela de segmentos dentro da imagem: dl_iterate_phdr a devolve. */
      if (eh.e_phoff >= p->p_offset &&
          eh.e_phoff + (uint64_t)eh.e_phnum * sizeof(Elf64_Phdr) <= p->p_offset + p->p_filesz) {
        e->phdr = (uint64_t)(uintptr_t)m->base + p->p_vaddr + (eh.e_phoff - p->p_offset);
        e->phnum = eh.e_phnum;
      }
    } else if (p->p_type == PT_DYNAMIC) {
      dyn_off = p->p_vaddr;
      dyn_size = p->p_memsz;
    } else if (p->p_type == PT_TLS) {
      e->tls_image = (uint64_t)(uintptr_t)m->base + p->p_vaddr;
      e->tls_filesz = p->p_filesz;
      e->tls_memsz = p->p_memsz;
      e->tls_align = p->p_align ? p->p_align : 1;
    } else if (p->p_type == PT_GNU_STACK) {
      if (p->p_memsz && p->p_memsz < 0x40000000u) stack = (uint32_t)p->p_memsz;
    }
  }
  m->stack_size = stack ? stack : ELF_STACK_DEFAULT;
  if (dyn_size) {
    const Elf64_Dyn* d;
    uint64_t soname = ~0ull, rpath = ~0ull, runpath = ~0ull;
    uint64_t n = dyn_size / sizeof(Elf64_Dyn);
    int bad = dyn_off + dyn_size > hi;
    e->dyn = (const Elf64_Dyn*)(m->base + dyn_off);
    for (d = e->dyn; !bad && n && d->d_tag != DT_NULL; d++, n--) {
      uint8_t* at = m->base + d->d_val;
      switch (d->d_tag) {
        case DT_STRTAB: e->str = (const char*)at; break;
        case DT_STRSZ: e->strsz = d->d_val; break;
        case DT_SYMTAB: e->sym = (const Elf64_Sym*)at; break;
        case DT_HASH: e->hash = (const uint32_t*)at; break;
        case DT_GNU_HASH: e->gnu = (const uint32_t*)at; break;
        case DT_RELA: e->rela = (const Elf64_Rela*)at; break;
        case DT_RELASZ: e->relasz = d->d_val; break;
        case DT_JMPREL: e->jmprel = (const Elf64_Rela*)at; break;
        case DT_PLTRELSZ: e->jmprelsz = d->d_val; break;
        case DT_RELR: e->relr = (const uint64_t*)at; break;
        case DT_RELRSZ: e->relrsz = d->d_val; break;
        case DT_INIT: e->init = (uint64_t)(uintptr_t)at; break;
        case DT_FINI: e->fini = (uint64_t)(uintptr_t)at; break;
        case DT_INIT_ARRAY: e->init_array = (uint64_t)(uintptr_t)at; break;
        case DT_INIT_ARRAYSZ: e->init_arraysz = d->d_val; break;
        case DT_FINI_ARRAY: e->fini_array = (uint64_t)(uintptr_t)at; break;
        case DT_FINI_ARRAYSZ: e->fini_arraysz = d->d_val; break;
        case DT_SONAME: soname = d->d_val; break;
        case DT_RPATH: rpath = d->d_val; break;
        case DT_RUNPATH: runpath = d->d_val; break;
        case DT_SYMBOLIC: e->symbolic = 1; break;
        case DT_FLAGS: if (d->d_val & DF_SYMBOLIC) e->symbolic = 1; break;
        case DT_REL: bad = 1; break; /* x86-64 so usa RELA */
        default: break;
      }
      switch (d->d_tag) {
        case DT_STRTAB: case DT_SYMTAB: case DT_HASH: case DT_GNU_HASH: case DT_RELA:
        case DT_JMPREL: case DT_RELR: case DT_INIT: case DT_FINI: case DT_INIT_ARRAY:
        case DT_FINI_ARRAY:
          if (d->d_val >= hi) bad = 1;
          break;
        default: break;
      }
    }
    if (!bad && e->str) {
      if (soname < e->strsz) e->soname = e->str + soname;
      if (runpath < e->strsz) e->runpath = e->str + runpath;
      else if (rpath < e->strsz) e->runpath = e->str + rpath;
    }
    if (!bad && e->sym) {
      if (e->hash) e->nsym = e->hash[1];
      else if (e->gnu) e->nsym = gnu_nsyms(e->gnu);
    }
    if (bad || (e->sym && !e->str)) {
      mod_set_err("segmento dinamico invalido: ", bname);
      elf_discard(id);
      return -1;
    }
  }
  return id;
}

/* ---------- simbolos ---------- */
static uint32_t hash_sysv(const char* s) {
  uint32_t h = 0, g;
  while (*s) {
    h = (h << 4) + (uint8_t)*s++;
    g = h & 0xf0000000u;
    if (g) h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

static uint32_t hash_gnu(const char* s) {
  uint32_t h = 5381;
  while (*s) h = h * 33u + (uint8_t)*s++;
  return h;
}

static const Elf64_Sym* elf_lookup(const struct module* m, const char* name, uint32_t hs, uint32_t hg) {
  const struct mod_elf* e = m->elf;
  if (!e->sym) return 0;
  if (e->gnu) {
    uint32_t nb = e->gnu[0], symoff = e->gnu[1], bloom_n = e->gnu[2], shift = e->gnu[3];
    const uint64_t* bloom = (const uint64_t*)(e->gnu + 4);
    const uint32_t* buckets = (const uint32_t*)(bloom + bloom_n);
    const uint32_t* chain = buckets + nb;
    uint64_t mask = (1ull << (hg & 63u)) | (1ull << ((hg >> shift) & 63u));
    uint32_t i;
    if (!nb || !bloom_n) return 0;
    if ((bloom[(hg / 64u) % bloom_n] & mask) != mask) return 0;
    i = buckets[hg % nb];
    if (i < symoff) return 0;
    for (;; i++) {
      uint32_t h = chain[i - symoff];
      if ((h | 1u) == (hg | 1u) && str_eq(e->str + e->sym[i].st_name, name)) return &e->sym[i];
      if (h & 1u) break;
    }
    return 0;
  }
  if (e->hash) {
    uint32_t nb = e->hash[0];
    const uint32_t* bucket = e->hash + 2;
    const uint32_t* chain = bucket + nb;
    uint32_t i;
    if (!nb) return 0;
    for (i = bucket[hs % nb]; i; i = chain[i])
      if (str_eq(e->str + e->sym[i].st_name, name)) return &e->sym[i];
  }
  return 0;
}

static int sym_defined(const Elf64_Sym* s) {
  uint8_t bind = s->st_info >> 4;
  return s->st_shndx != SHN_UNDEF && (bind == STB_GLOBAL || bind == STB_WEAK || bind == STB_GNU_UNIQUE);
}

static uint64_t sym_value(const struct module* m, const Elf64_Sym* s) {
  uint8_t type = s->st_info & 15u;
  uint64_t v;
  if (type == STT_TLS) return s->st_value;
  if (s->st_shndx == SHN_ABS) return s->st_value;
  v = (uint64_t)(uintptr_t)m->base + s->st_value;
  if (type == STT_GNU_IFUNC) v = ((uint64_t (*)(void))(uintptr_t)v)();
  return v;
}

void* elf_export(struct module* m, const char* name) {
  const Elf64_Sym* s;
  if (!m->elf) return 0;
  s = elf_lookup(m, name, hash_sysv(name), hash_gnu(name));
  if (!s || !sym_defined(s) || (s->st_info & 15u) == STT_TLS) return 0;
  return (void*)(uintptr_t)sym_value(m, s);
}

/* ---------- escopo ---------- */
static void scope_add(int id) {
  int i;
  for (i = 0; i < scope_n; i++)
    if (scope_ids[i] == id) return;
  if (scope_n < MOD_MAX) scope_ids[scope_n++] = id;
}

/* O modulo e as dependencias dele, em largura. */
static void scope_closure(int root) {
  int q[MOD_MAX];
  int nq = 0, qi, i;
  for (i = 0; i < MOD_MAX; i++)
    if (mods[i].used && mods[i].elf) mods[i].elf->mark = 0;
  q[nq++] = root;
  mods[root].elf->mark = 1;
  for (qi = 0; qi < nq; qi++) {
    const struct mod_elf* e = mods[q[qi]].elf;
    scope_add(q[qi]);
    for (i = 0; i < e->ndeps; i++) {
      int d = e->deps[i];
      if (mods[d].used && mods[d].elf && !mods[d].elf->mark && nq < MOD_MAX) {
        mods[d].elf->mark = 1;
        q[nq++] = d;
      }
    }
  }
}

/* Onde 'ref' procura um simbolo: os globais do processo em ordem de carga
 * (executavel primeiro) e depois o grupo do dlopen que o trouxe. */
static void scope_build(const struct module* ref) {
  uint32_t last = 0;
  scope_n = 0;
  if (ref->elf->symbolic) scope_add((int)(ref - mods));
  for (;;) {
    int best = -1, i;
    for (i = 0; i < MOD_MAX; i++) {
      if (!mods[i].used || !mods[i].elf || mods[i].pid != ref->pid || !mods[i].elf->global) continue;
      if (mods[i].elf->seq <= last) continue;
      if (best < 0 || mods[i].elf->seq < mods[best].elf->seq) best = i;
    }
    if (best < 0) break;
    last = mods[best].elf->seq;
    scope_add(best);
  }
  if (mods[ref->elf->root].used && mods[ref->elf->root].elf) scope_closure(ref->elf->root);
  scope_add((int)(ref - mods));
}

struct sym_hit {
  uint64_t value;
  const Elf64_Sym* sym;
  const struct module* mod; /* 0 = kernel ou simbolo fraco sem definicao */
};

/* Procura no escopo montado. skip: modulo que nao conta (R_X86_64_COPY).
 * plt: relocacao de chamada, que nao aceita a PLT canonica de outro modulo. */
static int scope_find(const char* name, const struct module* skip, int plt, struct sym_hit* out) {
  uint32_t hs = hash_sysv(name), hg = hash_gnu(name);
  int k;
  for (k = 0; k < scope_n; k++) {
    const struct module* d = &mods[scope_ids[k]];
    const Elf64_Sym* t;
    if (d == skip) continue;
    t = elf_lookup(d, name, hs, hg);
    if (!t) continue;
    if (t->st_shndx == SHN_UNDEF) {
      /* Executavel nao-PIC: o endereco da funcao e a entrada dele na PLT. */
      uint8_t bind = t->st_info >> 4;
      if (plt || !t->st_value || (t->st_info & 15u) != STT_FUNC || bind == STB_LOCAL) continue;
    } else if (!sym_defined(t)) {
      continue;
    }
    out->value = sym_value(d, t);
    out->sym = t;
    out->mod = d;
    return 0;
  }
  {
    void* k_fn = kexport_find(name);
    if (k_fn) {
      out->value = (uint64_t)(uintptr_t)k_fn;
      out->sym = 0;
      out->mod = 0;
      return 0;
    }
  }
  return -1;
}

/* ---------- relocacao ---------- */
static int missing_n;

static int resolve(const struct module* m, uint32_t symi, int for_copy, int plt, struct sym_hit* out) {
  const struct mod_elf* e = m->elf;
  const Elf64_Sym* s;
  const char* name;
  if (!e->sym || (e->nsym && symi >= e->nsym)) return -1;
  s = &e->sym[symi];
  if ((s->st_info >> 4) == STB_LOCAL) {
    out->value = sym_value(m, s);
    out->sym = s;
    out->mod = m;
    return 0;
  }
  name = e->str + s->st_name;
  if (scope_find(name, for_copy ? m : 0, plt, out) == 0) return 0;
  if ((s->st_info >> 4) == STB_WEAK) {
    out->value = 0;
    out->sym = 0;
    out->mod = 0;
    return 0;
  }
  mod_set_err("falta ", name);
  if (missing_n < MISSING_LOG_MAX) {
    log2s("[elf] ", m->name);
    log2s(": simbolo nao achado: ", name);
    serial_puts("\n");
  }
  missing_n++;
  return -1;
}

static int apply_rela(struct module* m, const Elf64_Rela* r, uint64_t count) {
  const struct mod_elf* e = m->elf;
  uint64_t base = (uint64_t)(uintptr_t)m->base;
  uint32_t last_sym = 0;
  int last_plt = -1;
  struct sym_hit hit;
  int bad = 0;
  hit.value = 0;
  hit.sym = 0;
  hit.mod = 0;
  for (; count; count--, r++) {
    uint32_t type = (uint32_t)r->r_info;
    uint32_t symi = (uint32_t)(r->r_info >> 32);
    uint64_t* where = (uint64_t*)(m->base + r->r_offset);
    int plt = type == R_X86_64_JUMP_SLOT;
    if (type == R_X86_64_NONE) continue;
    if (r->r_offset + 8 > m->size) {
      mod_set_err("relocacao fora da imagem em ", m->name);
      return -1;
    }
    if (type == R_X86_64_RELATIVE) {
      *where = base + (uint64_t)r->r_addend;
      continue;
    }
    if (type == R_X86_64_IRELATIVE) {
      *where = ((uint64_t (*)(void))(uintptr_t)(base + (uint64_t)r->r_addend))();
      continue;
    }
    if (symi) {
      /* Relocacoes seguidas costumam citar o mesmo simbolo. */
      int copyrel = type == R_X86_64_COPY;
      if (copyrel || symi != last_sym || plt != last_plt) {
        last_sym = 0;
        if (resolve(m, symi, copyrel, plt, &hit) != 0) {
          bad = 1;
          continue;
        }
        if (!copyrel) last_sym = symi;
        last_plt = plt;
      }
    } else {
      last_sym = 0;
      hit.value = 0;
      hit.sym = 0;
      hit.mod = m;
    }
    switch (type) {
      case R_X86_64_64:
        *where = hit.value + (uint64_t)r->r_addend;
        break;
      case R_X86_64_GLOB_DAT:
      case R_X86_64_JUMP_SLOT:
        *where = hit.value;
        break;
      case R_X86_64_COPY:
        if (hit.sym && hit.mod) copy(where, (const void*)(uintptr_t)hit.value, e->sym[symi].st_size);
        break;
      case R_X86_64_DTPMOD64:
        *where = hit.mod ? hit.mod->elf->tls_modid : 0;
        break;
      case R_X86_64_DTPOFF64:
        *where = hit.value + (uint64_t)r->r_addend;
        break;
      case R_X86_64_TPOFF64:
        /* Variante II: o bloco do modulo fica abaixo do ponteiro de thread. */
        if (hit.mod) *where = hit.value + (uint64_t)r->r_addend - hit.mod->elf->tls_offset;
        else *where = 0;
        break;
      default:
        mod_set_err("relocacao ELF nao suportada em ", m->name);
        log2s("[elf] ", m->name);
        serial_puts(": relocacao tipo ");
        serial_hex32(type);
        serial_puts("\n");
        return -1;
    }
  }
  return bad ? -1 : 0;
}

/* DT_RELR: lista compacta de relocacoes relativas (ld -z pack-relative-relocs). */
static void apply_relr(struct module* m) {
  const struct mod_elf* e = m->elf;
  uint64_t base = (uint64_t)(uintptr_t)m->base;
  uint64_t n = e->relrsz / 8;
  uint64_t* where = 0;
  uint64_t i;
  for (i = 0; i < n; i++) {
    uint64_t v = e->relr[i];
    if (!(v & 1u)) {
      where = (uint64_t*)(m->base + v);
      *where++ += base;
    } else if (where) {
      uint64_t* w = where;
      for (v >>= 1; v; v >>= 1, w++)
        if (v & 1u) *w += base;
      where += 63;
    }
  }
}

static int elf_relocate(int id) {
  struct module* m = &mods[id];
  const struct mod_elf* e = m->elf;
  int rc = 0;
  scope_build(m);
  if (e->relr) apply_relr(m);
  if (e->rela && apply_rela(m, e->rela, e->relasz / sizeof(Elf64_Rela)) != 0) rc = -1;
  if (e->jmprel && apply_rela(m, e->jmprel, e->jmprelsz / sizeof(Elf64_Rela)) != 0) rc = -1;
  return rc;
}

/* ---------- TLS e ordem de inicializacao ---------- */
static int tls_assign(int id) {
  struct module* m = &mods[id];
  struct mod_elf* e = m->elf;
  const struct module* exe = exe_of(m->pid);
  uint64_t used = 0, align;
  uint32_t maxid = 0;
  int i;
  if (!e->tls_memsz) return 0;
  for (i = 0; i < MOD_MAX; i++) {
    const struct mod_elf* o = mods[i].used && mods[i].pid == m->pid ? mods[i].elf : 0;
    if (!o || !o->tls_modid) continue;
    if (o->tls_offset > used) used = o->tls_offset;
    if (o->tls_modid > maxid) maxid = o->tls_modid;
  }
  align = e->tls_align ? e->tls_align : 1;
  used = (used + e->tls_memsz + align - 1) & ~(align - 1);
  if (!exe || (exe->elf->tls_closed && used > exe->elf->tls_static)) {
    mod_set_err(exe ? "TLS estatico cheio: " : "TLS exige executavel ELF: ", m->name);
    return -1;
  }
  e->tls_offset = used;
  e->tls_modid = maxid + 1;
  return 0;
}

static void init_order(int id) {
  struct mod_elf* e = mods[id].elf;
  int i;
  if (e->init_idx || e->mark) return;
  e->mark = 1;
  for (i = 0; i < e->ndeps; i++)
    if (mods[e->deps[i]].used && mods[e->deps[i]].elf) init_order(e->deps[i]);
  e->init_idx = ++g_init_seq;
}

/* ---------- carga ---------- */
int elf_load(const char* path, int want_dll, int global) {
  int queue[MOD_MAX];
  int nq = 0, qi, i;
  int root = elf_map(path, want_dll);
  if (root < 0) return -1;
  mods[root].elf->global = (uint8_t)(global || !want_dll);
  queue[nq++] = root;
  for (qi = 0; qi < nq; qi++) {
    struct module* m = &mods[queue[qi]];
    struct mod_elf* e = m->elf;
    const Elf64_Dyn* d;
    if (!e->dyn || !e->str) continue;
    for (d = e->dyn; d->d_tag != DT_NULL; d++) {
      const char* need;
      char lpath[DRV_PATH];
      int dep;
      if (d->d_tag != DT_NEEDED || d->d_val >= e->strsz) continue;
      need = e->str + d->d_val;
      if (is_kernel_lib(need)) continue;
      dep = elf_find_loaded(need);
      if (dep < 0) {
        if (elf_locate(need, m, lpath, sizeof(lpath)) != 0) {
          mod_set_err("nao achei ", need);
          log2s("[elf] ", m->name);
          log2s(" pede ", need);
          serial_puts(": nao achei\n");
          goto fail;
        }
        dep = nq < MOD_MAX ? elf_map(lpath, 1) : -1;
        if (dep < 0) goto fail;
        mods[dep].elf->root = root;
        mods[dep].elf->global = mods[root].elf->global;
        queue[nq++] = dep;
      }
      if (e->ndeps >= ELF_DEPS_MAX) {
        mod_set_err("bibliotecas demais em ", m->name);
        goto fail;
      }
      e->deps[e->ndeps++] = dep;
    }
  }
  if (mods[root].elf->global) {
    /* Bibliotecas que ja estavam carregadas como locais passam a globais. */
    scope_n = 0;
    scope_closure(root);
    for (i = 0; i < scope_n; i++) mods[scope_ids[i]].elf->global = 1;
  }
  /* O executavel vem primeiro: o codigo local-exec dele assume o bloco de TLS
   * colado no ponteiro de thread. */
  for (i = 0; i < nq; i++)
    if (tls_assign(queue[i]) != 0) goto fail;
  if (!want_dll) {
    struct mod_elf* e = mods[root].elf;
    uint64_t used = 0;
    for (i = 0; i < nq; i++)
      if (mods[queue[i]].elf->tls_offset > used) used = mods[queue[i]].elf->tls_offset;
    e->tls_static = (used + 63u) & ~63ull;
    if (e->ndeps) e->tls_static += TLS_SURPLUS;
    e->tls_closed = 1;
  }
  /* Dependencias primeiro: R_X86_64_COPY copia dados ja relocados. */
  missing_n = 0;
  for (i = nq - 1; i >= 0; i--)
    if (elf_relocate(queue[i]) != 0) {
      if (missing_n > MISSING_LOG_MAX) serial_puts("[elf] ... e outros simbolos\n");
      goto fail;
    }
  for (i = 0; i < MOD_MAX; i++)
    if (mods[i].used && mods[i].elf) mods[i].elf->mark = 0;
  init_order(root);
  return root;
fail:
  for (i = 0; i < nq; i++) elf_discard(queue[i]);
  return -1;
}

void elf_release_pid(uint32_t pid) {
  int i;
  if (!pid) return;
  for (i = 0; i < MOD_MAX; i++)
    if (mods[i].used && mods[i].elf && mods[i].pid == pid) elf_discard(i);
}

/* ---------- API k_dl_* ---------- */
static void fill_info(int id, struct k_dl_info* o) {
  const struct module* m = &mods[id];
  const struct mod_elf* e = m->elf;
  o->base = (uint64_t)(uintptr_t)m->base;
  o->size = m->size;
  o->name = e->soname ? e->soname : path_base(e->path);
  o->path = e->path;
  o->phdr = e->phdr;
  o->phnum = e->phnum;
  o->flags = (m->is_dll ? 0u : K_DL_EXE) | (e->global ? K_DL_GLOBAL_SCOPE : 0u);
  o->dynamic = (uint64_t)(uintptr_t)e->dyn;
  o->init = e->init;
  o->init_array = e->init_array;
  o->init_arraysz = e->init_arraysz;
  o->fini = e->fini;
  o->fini_array = e->fini_array;
  o->fini_arraysz = e->fini_arraysz;
  o->tls_image = e->tls_image;
  o->tls_filesz = e->tls_filesz;
  o->tls_memsz = e->tls_memsz;
  o->tls_align = e->tls_align;
  o->tls_offset = e->tls_offset;
  o->tls_modid = e->tls_modid;
  o->handle = (uint32_t)(id + 1);
  o->tls_static = e->tls_static;
  o->stack_size = m->stack_size;
}

int mod_dl_open(const char* name, int flags) {
  char path[DRV_PATH];
  struct module* exe = exe_of(mod_current_pid());
  int id;
  if (!name) return exe ? (int)(exe - mods) + 1 : 0;
  id = elf_find_loaded(name);
  if (id >= 0) {
    if (flags & K_DL_GLOBAL) {
      int i;
      scope_n = 0;
      scope_closure(id);
      for (i = 0; i < scope_n; i++) mods[scope_ids[i]].elf->global = 1;
    }
    return id + 1;
  }
  if (flags & K_DL_NOLOAD) return 0;
  if (elf_locate(name, exe, path, sizeof(path)) != 0) {
    mod_set_err("nao achei ", name);
    return 0;
  }
  if (!elf_probe(path)) {
    mod_set_err("nao e ELF: ", name);
    return 0;
  }
  id = elf_load(path, 1, (flags & K_DL_GLOBAL) != 0);
  return id < 0 ? 0 : id + 1;
}

void* mod_dl_sym(int handle, const char* name, const void* caller) {
  struct sym_hit hit;
  const struct module* skip_to = 0;
  if (!name) return 0;
  if (handle > 0) {
    if (!is_mine(handle - 1)) return 0;
    if (mods[handle - 1].is_dll) {
      scope_n = 0;
      scope_closure(handle - 1);
    } else {
      scope_build(&mods[handle - 1]);
    }
  } else {
    struct module* exe = exe_of(mod_current_pid());
    if (!exe) return kexport_find(name);
    scope_build(exe);
    if (handle == -1) {
      /* RTLD_NEXT: so depois do modulo de quem chamou. */
      int i;
      for (i = 0; i < scope_n; i++) {
        const struct module* m = &mods[scope_ids[i]];
        if ((const uint8_t*)caller >= m->base && (const uint8_t*)caller < m->base + m->size) skip_to = m;
      }
      if (skip_to) {
        int k = 0;
        for (i = 0; i < scope_n; i++)
          if (&mods[scope_ids[i]] == skip_to) k = i + 1;
        for (i = k; i < scope_n; i++) scope_ids[i - k] = scope_ids[i];
        scope_n -= k;
      }
    }
  }
  if (scope_find(name, 0, 0, &hit) != 0) {
    mod_set_err("simbolo nao achado: ", name);
    return 0;
  }
  if (hit.sym && (hit.sym->st_info & 15u) == STT_TLS) return 0;
  return (void*)(uintptr_t)hit.value;
}

int mod_dl_info(int handle, struct k_dl_info* out) {
  if (!out || !is_mine(handle - 1)) return 0;
  fill_info(handle - 1, out);
  return handle;
}

int mod_dl_iter(int index, struct k_dl_info* out) {
  uint32_t pid = mod_current_pid();
  uint32_t last = 0;
  int n;
  if (index < 0) return 0;
  for (n = 0;; n++) {
    int best = -1, i;
    for (i = 0; i < MOD_MAX; i++) {
      if (!mods[i].used || !mods[i].elf || mods[i].pid != pid || mods[i].elf->seq <= last) continue;
      if (best < 0 || mods[i].elf->seq < mods[best].elf->seq) best = i;
    }
    if (best < 0) return 0;
    last = mods[best].elf->seq;
    if (n == index) {
      if (out) fill_info(best, out);
      return best + 1;
    }
  }
}

int mod_dl_init_next(struct k_dl_info* out) {
  uint32_t pid = mod_current_pid();
  int best = -1, i;
  for (i = 0; i < MOD_MAX; i++) {
    if (!mods[i].used || !mods[i].elf || mods[i].pid != pid || mods[i].elf->inited) continue;
    if (best < 0 || mods[i].elf->init_idx < mods[best].elf->init_idx) best = i;
  }
  if (best < 0) return 0;
  mods[best].elf->inited = 1;
  if (out) fill_info(best, out);
  return best + 1;
}

int mod_dl_fini_next(struct k_dl_info* out) {
  uint32_t pid = mod_current_pid();
  int best = -1, i;
  for (i = 0; i < MOD_MAX; i++) {
    const struct mod_elf* e = mods[i].elf;
    if (!mods[i].used || !e || mods[i].pid != pid || !e->inited || e->finied) continue;
    if (best < 0 || e->init_idx > mods[best].elf->init_idx) best = i;
  }
  if (best < 0) return 0;
  mods[best].elf->finied = 1;
  if (out) fill_info(best, out);
  return best + 1;
}

int mod_dl_addr(const void* addr, struct k_dl_addr* out) {
  uint32_t pid = mod_current_pid();
  const uint8_t* a = (const uint8_t*)addr;
  int i;
  if (!out) return 0;
  for (i = 0; i < MOD_MAX; i++) {
    const struct module* m = &mods[i];
    const struct mod_elf* e = m->elf;
    const Elf64_Sym* best = 0;
    uint64_t off;
    uint32_t k;
    if (!m->used || !e || m->pid != pid || a < m->base || a >= m->base + m->size) continue;
    off = (uint64_t)(a - m->base);
    for (k = 1; e->sym && k < e->nsym; k++) {
      const Elf64_Sym* s = &e->sym[k];
      if (s->st_shndx == SHN_UNDEF || s->st_shndx == SHN_ABS || (s->st_info & 15u) == STT_TLS) continue;
      if (s->st_value > off || (s->st_size && off >= s->st_value + s->st_size)) continue;
      if (!best || s->st_value > best->st_value) best = s;
    }
    out->fname = e->path;
    out->fbase = (uint64_t)(uintptr_t)m->base;
    out->sname = best ? e->str + best->st_name : 0;
    out->saddr = best ? (uint64_t)(uintptr_t)m->base + best->st_value : 0;
    return 1;
  }
  return 0;
}
