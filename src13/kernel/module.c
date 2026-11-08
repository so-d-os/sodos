/* module.c - Carrega .EXE e .DLL no heap alto, aplica relocacoes e resolve
 * imports por nome ("KERNEL" = tabela do kernel, outro nome = arquivo .DLL). */
#include <stdint.h>
#include "module.h"
#include "mm.h"
#include "drives.h"
#include "shell.h"
#include "serial.h"
#include "reboot.h"
#include "kexports.h"
#include "c_compiler/sox.h"

void* kexport_find(const char* name);
int mod_call_entry(void* fn, int argc, char** argv, void* stack_top);
extern uint32_t mod_depth;
extern uint64_t cur_ctx;

#define MOD_STACK_DEFAULT 0x100000u
#define MOD_DEPTH_MAX 16
#define MOD_DEPS_MAX 16
#define LOAD_NEST_MAX 8

typedef int (*dllmain_fn)(int reason);

struct module {
  int used;
  int refs;
  int is_dll;
  int is64;
  uint32_t pid;
  char name[32];
  uint8_t* base;
  uint8_t* raw;
  uint32_t size;
  uint32_t entry;
  uint32_t stack_size;
  struct sox_export* exports;
  uint32_t nexports;
  char* strtab;
  uint32_t strtab_size;
  int deps[MOD_DEPS_MAX];
  int ndeps;
};

static struct module mods[MOD_MAX];
static char last_err[64];
static uint32_t next_pid = 1;
static uint32_t cur_pid;
static int load_nest;
static char exe_drive;

static void set_err(const char* a, const char* b) {
  int i = 0;
  while (a && *a && i < 63) last_err[i++] = *a++;
  while (b && *b && i < 63) last_err[i++] = *b++;
  last_err[i] = '\0';
}

const char* mod_last_error(void) { return last_err; }
uint32_t mod_current_pid(void) { return cur_pid; }
char mod_exe_drive(void) { return exe_drive; }

void mod_cpu_save(struct mod_cpu* s) {
  s->ctx = cur_ctx;
  s->pid = cur_pid;
  s->owner = um_owner();
  s->depth = mod_depth;
  s->drive = exe_drive;
}

void mod_cpu_load(const struct mod_cpu* s) {
  cur_ctx = s->ctx;
  cur_pid = s->pid;
  um_set_owner(s->owner);
  mod_depth = s->depth;
  exe_drive = s->drive;
}

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

/* "D:\X\LIBC.DLL" -> "LIBC.DLL" */
static void base_name(const char* path, char* out, int max) {
  const char* b = path;
  const char* p;
  int i = 0;
  for (p = path; *p; p++)
    if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
  while (*b && i < max - 1) out[i++] = up(*b++);
  out[i] = '\0';
}

static int has_dot(const char* s) {
  for (; *s; s++)
    if (*s == '.') return 1;
  return 0;
}

static int has_drive(const char* s) {
  return s[0] && s[1] == ':';
}

static void cat3(char* out, int max, const char* a, const char* b, const char* c) {
  int i = 0;
  while (a && *a && i < max - 1) out[i++] = *a++;
  while (b && *b && i < max - 1) out[i++] = *b++;
  while (c && *c && i < max - 1) out[i++] = *c++;
  out[i] = '\0';
}

/* Tenta 'name' como esta; sem unidade, procura na unidade dada, na corrente, em D: e C:. */
static int locate(const char* name, char pref, char* out, int max) {
  char drv[3];
  char order[4];
  int n = 0;
  int i;
  if (has_drive(name)) {
    cat3(out, max, name, 0, 0);
    return file_size(out) >= 0 ? 0 : -1;
  }
  if (pref) order[n++] = pref;
  order[n++] = drives_current();
  order[n++] = 'D';
  order[n++] = 'C';
  for (i = 0; i < n; i++) {
    int j;
    int dup = 0;
    for (j = 0; j < i; j++)
      if (up(order[j]) == up(order[i])) dup = 1;
    if (dup || !drives_is_mounted(order[i])) continue;
    drv[0] = order[i];
    drv[1] = ':';
    drv[2] = '\0';
    cat3(out, max, drv, name, 0);
    if (file_size(out) >= 0) return 0;
    cat3(out, max, drv, "\\", name);
    if (file_size(out) >= 0) return 0;
  }
  return -1;
}

int mod_find_exe(const char* name, char* out, int max) {
  char tmp[DRV_PATH];
  if (!name || !name[0]) return -1;
  if (locate(name, 0, out, max) == 0) {
    char b[32];
    base_name(out, b, sizeof(b));
    if (has_dot(b)) return 0;
  }
  if (has_dot(name)) return -1;
  cat3(tmp, sizeof(tmp), name, ".EXE", 0);
  return locate(tmp, 0, out, max);
}

static int ends_ci(const char* s, const char* suf) {
  int ns = 0, nf = 0, i;
  if (!s || !suf) return 0;
  while (s[ns]) ns++;
  while (suf[nf]) nf++;
  if (ns < nf) return 0;
  for (i = 0; i < nf; i++)
    if (up(s[ns - nf + i]) != up(suf[i])) return 0;
  return 1;
}

int mod_find_batch(const char* name, char* out, int max) {
  char tmp[DRV_PATH];
  if (!name || !name[0]) return -1;
  if (ends_ci(name, ".BAT") || ends_ci(name, ".CMD"))
    return locate(name, 0, out, max);
  if (has_dot(name)) return -1;
  cat3(tmp, sizeof(tmp), name, ".BAT", 0);
  return locate(tmp, 0, out, max);
}

/* Cada programa tem a sua copia da DLL: a LIBC e a GUI guardam estado
 * (malloc, tamanho da janela) que nao pode ser compartilhado. */
static int find_loaded(const char* base) {
  int i;
  for (i = 0; i < MOD_MAX; i++)
    if (mods[i].used && mods[i].is_dll && mods[i].pid == cur_pid &&
        same_name(mods[i].name, base))
      return i;
  return -1;
}

static void* find_export(struct module* m, const char* name) {
  uint32_t i;
  for (i = 0; i < m->nexports; i++) {
    uint32_t off = m->exports[i].name;
    if (off < m->strtab_size && str_eq(m->strtab + off, name))
      return m->base + m->exports[i].value;
  }
  return 0;
}

static void unload(int id);

static void release(int id) {
  if (id < 0 || id >= MOD_MAX || !mods[id].used) return;
  if (--mods[id].refs <= 0) unload(id);
}

static void unload(int id) {
  struct module* m = &mods[id];
  int i;
  if (m->is_dll && m->entry != SOX_NO_ENTRY)
    ((dllmain_fn)(m->base + m->entry))(SOX_DLL_DETACH);
  for (i = 0; i < m->ndeps; i++) release(m->deps[i]);
  ufree(m->raw);
  ufree(m->exports);
  ufree(m->strtab);
  m->used = 0;
}

static int load(const char* req, int want_dll);
static void dyn_release_pid(uint32_t pid);

static int resolve_imports(int id, const struct sox_import* imp, uint32_t n, const char* st,
                           uint32_t st_size, int is64) {
  struct module* m = &mods[id];
  uint32_t i;
  for (i = 0; i < n; i++) {
    const char* lib;
    const char* fn;
    void* addr = 0;
    uint32_t need = is64 ? 8u : 4u;
    if (imp[i].lib >= st_size || imp[i].name >= st_size || imp[i].slot + need > m->size) {
      set_err("import invalido em ", m->name);
      return -1;
    }
    lib = st + imp[i].lib;
    fn = st + imp[i].name;
    if (same_name(lib, "KERNEL")) {
      addr = kexport_find(fn);
    } else {
      char b[32];
      int dep = -1;
      int k;
      base_name(lib, b, sizeof(b));
      for (k = 0; k < m->ndeps; k++)
        if (same_name(mods[m->deps[k]].name, b)) dep = m->deps[k];
      if (dep < 0) {
        if (m->ndeps >= MOD_DEPS_MAX) {
          set_err("DLLs demais em ", m->name);
          return -1;
        }
        dep = load(lib, 1);
        if (dep < 0) return -1;
        m->deps[m->ndeps++] = dep;
      }
      addr = find_export(&mods[dep], fn);
    }
    if (!addr) {
      set_err("falta ", fn);
      serial_puts("[mod] import nao achado: ");
      serial_puts(lib);
      serial_puts("!");
      serial_puts(fn);
      serial_puts("\n");
      return -1;
    }
    if (is64) *(uint64_t*)(m->base + imp[i].slot) = (uint64_t)(uintptr_t)addr;
    else *(uint32_t*)(m->base + imp[i].slot) = (uint32_t)(uintptr_t)addr;
  }
  return 0;
}

static int load(const char* req, int want_dll) {
  char path[DRV_PATH];
  char bname[32];
  int fsz;
  uint8_t* file;
  struct sox_hdr* h;
  struct module* m;
  int id;
  int is64;
  uint32_t i;
  uint32_t total;
  if (want_dll) {
    base_name(req, bname, sizeof(bname));
    id = find_loaded(bname);
    if (id >= 0) {
      mods[id].refs++;
      return id;
    }
  }
  if (load_nest >= LOAD_NEST_MAX) {
    set_err("DLLs aninhadas demais: ", req);
    return -1;
  }
  if (locate(req, exe_drive, path, sizeof(path)) != 0) {
    set_err("nao achei ", req);
    return -1;
  }
  base_name(path, bname, sizeof(bname));
  fsz = file_size(path);
  if (fsz < (int)sizeof(struct sox_hdr)) {
    set_err("arquivo invalido: ", bname);
    return -1;
  }
  file = (uint8_t*)umalloc_owned((uint32_t)fsz + 1u, 0);
  if (!file) {
    set_err("sem memoria para ", bname);
    return -1;
  }
  if (file_load(path, (char*)file, fsz + 1) != fsz) {
    ufree(file);
    set_err("erro lendo ", bname);
    return -1;
  }
  h = (struct sox_hdr*)file;
  is64 = (h->magic == SOX_MAGIC2) || ((h->flags & SOX_FLAG_64) != 0);
  if ((h->magic != SOX_MAGIC && h->magic != SOX_MAGIC2) ||
      h->off_image + h->image_size > (uint32_t)fsz ||
      h->off_relocs + h->nrelocs * 4u > (uint32_t)fsz ||
      h->off_imports + h->nimports * sizeof(struct sox_import) > (uint32_t)fsz ||
      h->off_exports + h->nexports * sizeof(struct sox_export) > (uint32_t)fsz ||
      h->off_strtab + h->strtab_size > (uint32_t)fsz ||
      ((h->flags & SOX_FLAG_DLL) != 0) != (want_dll != 0)) {
    ufree(file);
    set_err(want_dll ? "nao e DLL SOX: " : "nao e EXE SOX: ", bname);
    return -1;
  }
  for (id = 0; id < MOD_MAX; id++)
    if (!mods[id].used) break;
  if (id >= MOD_MAX) {
    ufree(file);
    set_err("modulos demais", 0);
    return -1;
  }
  m = &mods[id];
  total = h->image_size + h->bss_size;
  /* PIEs tem dados com alinhamento de ate uma pagina. */
  m->raw = (uint8_t*)umalloc_owned((total ? total : 1u) + 4096u, 0);
  m->base = m->raw ? (uint8_t*)(((uintptr_t)m->raw + 4095u) & ~(uintptr_t)4095u) : 0;
  m->strtab = (char*)umalloc_owned(h->strtab_size + 1u, 0);
  m->exports = (struct sox_export*)umalloc_owned(h->nexports * sizeof(struct sox_export) + 1u, 0);
  if (!m->base || !m->strtab || !m->exports) {
    ufree(m->raw);
    ufree(m->strtab);
    ufree(m->exports);
    ufree(file);
    set_err("sem memoria para ", bname);
    return -1;
  }
  m->used = 1;
  m->refs = 1;
  m->is_dll = want_dll;
  m->is64 = is64;
  m->pid = cur_pid;
  for (i = 0; i < sizeof(m->name) - 1 && bname[i]; i++) m->name[i] = bname[i];
  m->name[i] = '\0';
  m->size = total;
  m->entry = h->entry;
  m->stack_size = h->stack_size;
  m->ndeps = 0;
  m->nexports = h->nexports;
  m->strtab_size = h->strtab_size;
  for (i = 0; i < h->image_size; i++) m->base[i] = file[h->off_image + i];
  for (; i < total; i++) m->base[i] = 0;
  for (i = 0; i < h->strtab_size; i++) m->strtab[i] = (char)file[h->off_strtab + i];
  m->strtab[h->strtab_size] = '\0';
  {
    const struct sox_export* ex = (const struct sox_export*)(file + h->off_exports);
    for (i = 0; i < h->nexports; i++) {
      m->exports[i] = ex[i];
      if (ex[i].value >= total) m->exports[i].value = 0;
    }
  }
  {
    const uint32_t* rel = (const uint32_t*)(file + h->off_relocs);
    for (i = 0; i < h->nrelocs; i++) {
      uint32_t width = is64 ? 8u : 4u;
      if (rel[i] + width > h->image_size) {
        set_err("relocacao invalida em ", bname);
        m->ndeps = 0;
        m->entry = SOX_NO_ENTRY;
        unload(id);
        ufree(file);
        return -1;
      }
      if (is64) *(uint64_t*)(m->base + rel[i]) += (uint64_t)(uintptr_t)m->base;
      else *(uint32_t*)(m->base + rel[i]) += (uint32_t)(uintptr_t)m->base;
    }
  }
  load_nest++;
  if (resolve_imports(id, (const struct sox_import*)(file + h->off_imports), h->nimports,
                      (const char*)(file + h->off_strtab), h->strtab_size, is64) != 0) {
    load_nest--;
    m->entry = SOX_NO_ENTRY;
    unload(id);
    ufree(file);
    return -1;
  }
  load_nest--;
  ufree(file);
  if (m->entry != SOX_NO_ENTRY && m->entry >= total) {
    set_err("entrada invalida em ", bname);
    m->entry = SOX_NO_ENTRY;
    unload(id);
    return -1;
  }
  if (want_dll && m->entry != SOX_NO_ENTRY) {
    if (((dllmain_fn)(m->base + m->entry))(SOX_DLL_ATTACH) == 0) {
      set_err("DllMain recusou ", bname);
      m->entry = SOX_NO_ENTRY;
      unload(id);
      return -1;
    }
  }
  return id;
}

static uint32_t arg_len(const char* s) {
  uint32_t n = 0;
  if (!s) return 0;
  while (s[n]) n++;
  return n;
}

/* Copia o argv para o topo da pilha do programa. SOX1 usa vetor de u32;
 * SOX2 (LP64) usa u64. Devolve o endereco do vetor, que tambem e o topo util
 * da pilha. */
static void* pack_user_argv(int* argc_io, char** argv, uint8_t* stack, uint32_t ssz, int is64) {
  uint32_t ptrsz = is64 ? 8u : 4u;
  uint32_t bytes;
  uint32_t i;
  uint8_t* base;
  char* dst;
  int n = argc_io ? *argc_io : 0;
  if (n < 0) n = 0;
  if (n > 31) n = 31;
  bytes = (uint32_t)(n + 1) * ptrsz;
  for (i = 0; i < (uint32_t)n; i++) {
    const char* s = (argv && argv[i]) ? argv[i] : "";
    bytes += arg_len(s) + 1u;
  }
  bytes = (bytes + 15u) & ~15u;
  if (bytes + 256u > ssz) return 0;
  base = stack + ssz - bytes;
  dst = (char*)base + (uint32_t)(n + 1) * ptrsz;
  for (i = 0; i < (uint32_t)n; i++) {
    const char* s = (argv && argv[i]) ? argv[i] : "";
    uint32_t k = 0;
    if (is64) ((uint64_t*)base)[i] = (uint64_t)(uintptr_t)dst;
    else ((uint32_t*)base)[i] = (uint32_t)(uintptr_t)dst;
    while (s[k]) {
      dst[k] = s[k];
      k++;
    }
    dst[k] = 0;
    dst += k + 1;
  }
  if (is64) ((uint64_t*)base)[n] = 0;
  else ((uint32_t*)base)[n] = 0;
  if (argc_io) *argc_io = n;
  return base;
}

static uint8_t mod_tls_block[4096] __attribute__((aligned(16)));
static uint64_t mod_fs_read(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
  return ((uint64_t)hi << 32) | lo;
}
static void mod_fs_write(uint64_t v) {
  __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

int mod_exec(const char* path, int argc, char** argv) {
  int id;
  int rc;
  uint8_t* stack;
  uint32_t ssz;
  uint32_t pid;
  uint32_t prev_owner;
  uint32_t prev_pid;
  char prev_drive;
  if (mod_depth >= MOD_DEPTH_MAX) {
    set_err("programas aninhados demais", 0);
    return MOD_ERR_DEPTH;
  }
  if (!um_ready()) {
    set_err("sem memoria alta (use -m 128)", 0);
    return MOD_ERR_MEMORY;
  }
  pid = next_pid++;
  if (next_pid == 0) next_pid = 1;
  prev_owner = um_owner();
  prev_pid = cur_pid;
  prev_drive = exe_drive;
  cur_pid = pid;
  um_set_owner(pid);
  exe_drive = has_drive(path) ? up(path[0]) : drives_current();
  id = load(path, 0);
  if (id < 0) {
    um_set_owner(prev_owner);
    cur_pid = prev_pid;
    exe_drive = prev_drive;
    return MOD_ERR_IMPORT;
  }
  if (mods[id].entry == SOX_NO_ENTRY) {
    set_err("EXE sem _start: ", mods[id].name);
    unload(id);
    um_set_owner(prev_owner);
    cur_pid = prev_pid;
    exe_drive = prev_drive;
    return MOD_ERR_FORMAT;
  }
  ssz = mods[id].stack_size ? mods[id].stack_size : MOD_STACK_DEFAULT;
  stack = (uint8_t*)umalloc_owned(ssz, pid);
  if (!stack) {
    set_err("sem memoria para a pilha", 0);
    unload(id);
    um_set_owner(prev_owner);
    cur_pid = prev_pid;
    exe_drive = prev_drive;
    return MOD_ERR_MEMORY;
  }
  {
    int nargc = argc;
    void* av = pack_user_argv(&nargc, argv, stack, ssz, mods[id].is64);
    if (!av) {
      set_err("argumentos nao cabem na pilha", 0);
      unload(id);
      um_set_owner(prev_owner);
      cur_pid = prev_pid;
      ufree_owner(pid);
      exe_drive = prev_drive;
      return MOD_ERR_MEMORY;
    }
    kbd_break_take();   /* Ctrl+C antigo (do prompt) nao mata o novo */
    {
      uint64_t prev_fs = mod_fs_read();
      uint64_t canary = 0x9e3779b97f4a7c15ull;
      int ti;
      for (ti = 0; ti < (int)sizeof(mod_tls_block); ti++) mod_tls_block[ti] = 0;
      for (ti = 0; ti < 8; ti++) mod_tls_block[0x28 + ti] = (uint8_t)(canary >> (ti * 8));
      mod_fs_write((uint64_t)(uintptr_t)mod_tls_block);
      serial_puts("[mod] exec ");
    serial_puts(mods[id].name);
    serial_puts(" base=");
    serial_hex32((uint32_t)(uintptr_t)mods[id].base);
    serial_puts(" entry=");
    serial_hex32((uint32_t)mods[id].entry);
    serial_puts(" size=");
    serial_hex32((uint32_t)mods[id].size);
    serial_puts(" stack=");
    serial_hex32((uint32_t)(uintptr_t)stack);
    serial_puts(" ssz=");
    serial_hex32(ssz);
    serial_puts(" av=");
    serial_hex32((uint32_t)(uintptr_t)av);
    serial_puts("\n");
      rc = mod_call_entry(mods[id].base + mods[id].entry, nargc, av, av);
      mod_fs_write(prev_fs);
      serial_puts("[mod] saiu ");
      serial_puts(mods[id].name);
      serial_puts(" rc=");
      serial_hex32((uint32_t)rc);
      serial_puts("\n");
    }
  }
  dyn_release_pid(pid);
  kexp_release_pid(pid);
  unload(id);
  um_set_owner(prev_owner);
  cur_pid = prev_pid;
  ufree_owner(pid);
  exe_drive = prev_drive;
  return rc;
}

/* dll_load feito por um programa: liberado sozinho quando ele termina. */
#define DYN_MAX 16
static uint32_t dyn_pid[DYN_MAX];
static int dyn_id[DYN_MAX];

static void dyn_release_pid(uint32_t pid) {
  int i;
  for (i = 0; i < DYN_MAX; i++) {
    if (dyn_pid[i] == pid && pid != 0) {
      dyn_pid[i] = 0;
      release(dyn_id[i]);
    }
  }
}

int mod_dll_load(const char* name) {
  int id;
  int i;
  if (!name) return 0;
  if (!exe_drive) exe_drive = drives_current();
  id = load(name, 1);
  if (id < 0) return 0;
  if (cur_pid) {
    for (i = 0; i < DYN_MAX; i++) {
      if (dyn_pid[i] == 0) {
        dyn_pid[i] = cur_pid;
        dyn_id[i] = id;
        break;
      }
    }
  }
  return id + 1;
}

void* mod_dll_sym(int handle, const char* name) {
  int id = handle - 1;
  if (id < 0 || id >= MOD_MAX || !mods[id].used || !name) return 0;
  return find_export(&mods[id], name);
}

void mod_dll_free(int handle) {
  int id = handle - 1;
  int i;
  if (id < 0 || id >= MOD_MAX || !mods[id].used) return;
  for (i = 0; i < DYN_MAX; i++) {
    if (dyn_pid[i] == cur_pid && cur_pid != 0 && dyn_id[i] == id) {
      dyn_pid[i] = 0;
      break;
    }
  }
  release(id);
}

int mod_get(int i, struct mod_info* out) {
  const struct module* m;
  int k;
  if (i < 0 || i >= MOD_MAX || !mods[i].used || !out) return 0;
  m = &mods[i];
  for (k = 0; k < 31 && m->name[k]; k++) out->name[k] = m->name[k];
  out->name[k] = 0;
  out->pid = m->pid;
  out->is_dll = m->is_dll;
  out->refs = m->refs;
  out->base = (uint32_t)(uintptr_t)m->base;
  out->size = m->size;
  out->blk[0] = (uint32_t)(uintptr_t)m->raw;
  out->blk[1] = (uint32_t)(uintptr_t)m->exports;
  out->blk[2] = (uint32_t)(uintptr_t)m->strtab;
  return 1;
}

static void put_hex(uint32_t v) {
  static const char hx[] = "0123456789ABCDEF";
  int i;
  for (i = 7; i >= 0; i--) putc(hx[(v >> (i * 4)) & 15]);
}

void mod_list(void) {
  int i;
  int any = 0;
  for (i = 0; i < MOD_MAX; i++) {
    char n[12];
    if (!mods[i].used) continue;
    any = 1;
    puts_k(mods[i].is_dll ? "  DLL " : "  EXE ");
    puts_k(mods[i].name);
    puts_k("  base ");
    put_hex((uint32_t)mods[i].base);
    puts_k("  ");
    uint32_to_string(mods[i].size, n, sizeof(n));
    puts_k(n);
    puts_k(" bytes  refs ");
    uint32_to_string((uint32_t)mods[i].refs, n, sizeof(n));
    puts_k(n);
    puts_k("\n");
  }
  if (!any) puts_k("  nenhum modulo carregado\n");
}
