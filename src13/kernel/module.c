/* module.c - Carrega .EXE e .DLL no heap alto, aplica relocacoes e resolve
 * imports por nome ("KERNEL" = tabela do kernel, outro nome = arquivo .DLL). */
#include <stdint.h>
#include "module.h"
#include "vt.h"
#include "kthread.h"
#include "mm.h"
#include "drives.h"
#include "shell.h"
#include "serial.h"
#include "reboot.h"
#include "kexports.h"
#include "module_priv.h"
#include "isr.h"
#include "env.h"
#include "vmem.h"
#include "wine.h"
#include "audio.h"

void* kexport_find(const char* name);
int mod_call_entry(void* fn, int argc, char** argv, void* stack_top);
extern uint32_t mod_depth;
extern uint64_t cur_ctx;

#define MOD_STACK_DEFAULT 0x100000u
#define MOD_DEPTH_MAX 16
#define LOAD_NEST_MAX 8

typedef int (*dllmain_fn)(int reason);

struct module mods[MOD_MAX];
static char last_err[64];
static uint32_t next_pid = 1;
static uint32_t cur_pid;
static int load_nest;
/* mod_dll_load_sys: carrega este caminho mesmo se ja ha um DLL de mesmo nome
 * (driver trocado por SET FAT32DLL=...). */
static int load_fresh;
static char exe_drive;

static void set_err(const char* a, const char* b) {
  int i = 0;
  while (a && *a && i < 63) last_err[i++] = *a++;
  while (b && *b && i < 63) last_err[i++] = *b++;
  last_err[i] = '\0';
}

void mod_set_err(const char* a, const char* b) { set_err(a, b); }

const char* mod_last_error(void) { return last_err; }
uint32_t mod_current_pid(void) { return cur_pid; }
/* Nome do EXE do pid (0 se nao ha modulo: tarefa do WIN sem SOX, WINE). */
const char* mod_pid_name(uint32_t pid) {
  int i;
  for (i = 0; i < MOD_MAX; i++)
    if (mods[i].used && !mods[i].is_dll && mods[i].pid == pid) return mods[i].name;
  return pid ? "pid?" : "kernel";
}
char mod_exe_drive(void) { return exe_drive; }

static uint64_t mod_fs_read(void);
static void mod_fs_write(uint64_t v);

void mod_thread_enter(struct mod_tsave* s, uint32_t pid, char drive) {
  s->pid = cur_pid;
  s->owner = um_owner();
  s->drive = exe_drive;
  cur_pid = pid;
  um_set_owner(pid);
  exe_drive = drive;
}

void mod_thread_leave(const struct mod_tsave* s) {
  cur_pid = s->pid;
  um_set_owner(s->owner);
  exe_drive = s->drive;
}

void mod_cpu_save(struct mod_cpu* s) {
  s->bkl = kt_bkl_depth();
  s->fs = mod_fs_read();
  s->teb = kt_win_teb_get();
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
  kt_bkl_set_depth(s->bkl);
  /* Dois programas com TLS (Firefox, LLAMA) nao podem dividir o FS. */
  if (s->fs != mod_fs_read()) mod_fs_write(s->fs);
  if (s->teb != kt_win_teb_get()) kt_win_teb_set(s->teb);
}

int mod_rip_preemptible(uint64_t rip) {
  int i;
  if (!cur_pid) return 0;
  for (i = 0; i < MOD_MAX; i++) {
    const struct module* m = &mods[i];
    uint64_t b;
    if (!m->used || !m->base || m->pid != cur_pid) continue;
    b = (uint64_t)(uintptr_t)m->base;
    if (rip >= b && rip < b + m->size) return 1;
  }
  return 0;
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

/* Sem unidade nem pasta no nome: cada pasta do PATH (SET PATH=C:\\BIN;U:\\SODOS\\BIN)
 * e por fim a pasta do sistema (PATHSODOS, padrao U:\\SODOS). Assim o BAT.EXE
 * e as DLLs de U:\\SODOS sao achados antes de qualquer SET PATH (START.BAT). */
static int locate_dirs(const char* name, char* out, int max) {
  char dir[DRV_PATH];
  const char* p;
  int i;
  if (has_drive(name)) return -1;
  for (p = name; *p; p++)
    if (*p == '\\' || *p == '/') return -1;
  for (i = 0; env_path_at(i, dir, (int)sizeof(dir)); i++) {
    int n = 0;
    while (dir[n]) n++;
    cat3(out, max, dir, (dir[n - 1] == '\\' || dir[n - 1] == ':') ? 0 : "\\", name);
    if (file_size(out) >= 0) return 0;
  }
  env_sodos_path(name, out, max);
  if (file_size(out) >= 0) return 0;
  return -1;
}

static int locate_path(const char* name, char* out, int max) {
  if (locate(name, 0, out, max) == 0) return 0;
  return locate_dirs(name, out, max);
}

int mod_locate(const char* name, char pref, char* out, int max) {
  if (locate(name, pref, out, max) == 0) return 0;
  return locate_dirs(name, out, max);
}

void mod_base_name(const char* path, char* out, int max) { base_name(path, out, max); }

int mod_free_slot(void) {
  int id;
  for (id = 0; id < MOD_MAX; id++)
    if (!mods[id].used) return id;
  return -1;
}

int mod_find_exe(const char* name, char* out, int max) {
  char tmp[DRV_PATH];
  if (!name || !name[0]) return -1;
  if (locate_path(name, out, max) == 0) {
    char b[32];
    base_name(out, b, sizeof(b));
    if (has_dot(b)) return 0;
  }
  if (has_dot(name)) return -1;
  cat3(tmp, sizeof(tmp), name, ".EXE", 0);
  return locate_path(tmp, out, max);
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
    return locate_path(name, out, max);
  if (has_dot(name)) return -1;
  cat3(tmp, sizeof(tmp), name, ".BAT", 0);
  return locate_path(tmp, out, max);
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
  if (m->elf) return elf_export(m, name);
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
  /* Biblioteca ELF vive ate o processo acabar (elf_release_pid). */
  if (mods[id].elf && mods[id].is_dll) return;
  if (--mods[id].refs <= 0) unload(id);
}

static void unload(int id) {
  struct module* m;
  int i;
  if (id < 0 || id >= MOD_MAX) return;   /* EXE do Windows: sem modulo SOX */
  m = &mods[id];
  if (m->is_dll && m->entry != SOX_NO_ENTRY)
    ((dllmain_fn)(m->base + m->entry))(SOX_DLL_DETACH);
  for (i = 0; i < m->ndeps; i++) release(m->deps[i]);
  ufree(m->raw);
  ufree(m->exports);
  ufree(m->strtab);
  ufree(m->elf);
  m->elf = 0;
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
  if (want_dll && !load_fresh) {
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
  if (locate(req, exe_drive, path, sizeof(path)) != 0 &&
      locate_dirs(req, path, sizeof(path)) != 0) {
    set_err("nao achei ", req);
    return -1;
  }
  /* PIE ou .so: o carregador ELF traz tambem as bibliotecas de DT_NEEDED. */
  if (elf_probe(path)) return elf_load(path, want_dll, !want_dll);
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
  m->elf = 0;
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
/* Base do FS (TLS dos programas). Com CR4.FSGSBASE (ligado no kernel_entry
 * quando o CPU tem) usa rdfsbase/wrfsbase: numa VM o rdmsr/wrmsr saem para o
 * emulador, e isto roda em toda troca de tarefa (k_yield, preempcao). */
static int fsgsbase_on(void) {
  static int on = -1;
  if (on < 0) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    on = (cr4 >> 16) & 1;
  }
  return on;
}
static uint64_t mod_fs_read(void) {
  uint32_t lo, hi;
  if (fsgsbase_on()) {
    uint64_t v;
    __asm__ volatile("rdfsbase %0" : "=r"(v));
    return v;
  }
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
  return ((uint64_t)hi << 32) | lo;
}
static void mod_fs_write(uint64_t v) {
  if (fsgsbase_on()) {
    __asm__ volatile("wrfsbase %0" :: "r"(v) : "memory");
    return;
  }
  __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* Programas rodando agora (um por mod_exec em andamento). Uma tarefa do WIN
 * encerrada a forca (Ctrl+Backspace, Sair) nunca volta ao fim do mod_exec:
 * mod_kill faz por ela a mesma limpeza. prev = programa que a chamou. */
#define LIVE_MAX 4096 /* = TASK_MAX do WIN (video/visual.c) */
static struct {
  uint32_t pid;
  uint32_t prev;
  int id;
} live[LIVE_MAX];

static void live_add(uint32_t pid, uint32_t prev, int id) {
  int i;
  for (i = 0; i < LIVE_MAX; i++) {
    if (live[i].pid) continue;
    live[i].pid = pid;
    live[i].prev = prev;
    live[i].id = id;
    return;
  }
}

static int live_find(uint32_t pid) {
  int i;
  if (!pid) return -1;
  for (i = 0; i < LIVE_MAX; i++)
    if (live[i].pid == pid) return i;
  return -1;
}

/* O que o fim do mod_exec devolve: DLLs, exports, imagem, .so e o heap. */
static void exec_release(uint32_t pid, int id) {
  kt_release_pid(pid);   /* threads param antes do heap sumir */
  wine_pid_gone(pid);    /* registro de esperas do WINE.DLL (antes do ufree_owner) */
  audio_pid_gone(pid);   /* som aberto pelo programa: para */
  dyn_release_pid(pid);
  kexp_release_pid(pid);
  unload(id);
  elf_release_pid(pid);
  vmem_release_pid(pid);          /* memoria virtual do Windows (sem reservas: so um if) */
  ufree_owner(pid);
  hm_free_owner(pid);
  isr_fault_release(pid);
  wine_low_release_pid(pid);      /* imagem de base fixa (sem pedidos: so um if) */
  if (id < 0) wine_exec_done_pid(pid);   /* EXE do Windows: pode soltar o WINE.DLL */
}

/* Programas de inicio (AUTOEXEC, servicos e o que o WIN abre sozinho) tem
 * pid menor que a marca; KILLALL encerra so os de depois. O WIN marca uns
 * segundos depois de abrir (mod_mark_boot). */
static uint32_t boot_mark;
void mod_mark_boot(void) {
  if (boot_mark) return;
  boot_mark = next_pid;
  serial_puts("[mod] programas de inicio: pid < ");
  serial_hex32(boot_mark);
  serial_puts("\n");
}
uint32_t mod_boot_mark(void) { return boot_mark; }
/* LIMPA: o boot roda de novo; o WIN novo marca outra vez. */
void mod_mark_boot_reset(void) { boot_mark = 0; }

/* "gesture" ou "GESTURE.EXE" contra o nome do modulo (sem caixa, ate o '.'). */
static int name_is(const char* mod, const char* name) {
  int i;
  for (i = 0; name[i] && mod[i]; i++) {
    char x = name[i], y = mod[i];
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
  }
  return !name[i] && (!mod[i] || mod[i] == '.');
}

/* KILL <nome>: pids vivos cujo EXE do SODOS (SOX) tem esse nome, pela tabela
 * de modulos (o pid que a tarefa do WIN guardou pode nao ser o dela). */
int mod_pids_named(const char* name, uint32_t* out, int max) {
  int i, n = 0;
  if (!name || !name[0]) return 0;
  for (i = 0; i < LIVE_MAX && n < max; i++) {
    int id = live[i].id;
    if (!live[i].pid || id < 0 || id >= MOD_MAX || !mods[id].used || mods[id].is_dll) continue;
    if (name_is(mods[id].name, name)) out[n++] = live[i].pid;
  }
  return n;
}

/* 1 se o pid esta vivo e e um EXE do Windows (roda pelo WINE.DLL). */
int mod_pid_is_pe(uint32_t pid) {
  int i = live_find(pid);
  return i >= 0 && live[i].id < 0;
}

/* Primeiro programa da cadeia (quem chamou quem, ate o de fora). */
uint32_t mod_root_pid(uint32_t pid) {
  int i, guard = 0;
  while ((i = live_find(pid)) >= 0 && live[i].prev && live_find(live[i].prev) >= 0 && guard++ < LIVE_MAX)
    pid = live[i].prev;
  return pid;
}

/* 1 se want esta na cadeia de pid (ele mesmo ou quem o chamou). */
int mod_chain_has(uint32_t pid, uint32_t want) {
  int i, guard = 0;
  if (!want) return 0;
  while (pid && guard++ <= LIVE_MAX) {
    if (pid == want) return 1;
    i = live_find(pid);
    if (i < 0) return 0;
    pid = live[i].prev;
  }
  return 0;
}

int mod_kill(uint32_t pid) {
  int n = 0;
  int i;
  while ((i = live_find(pid)) >= 0) {
    uint32_t prev = live[i].prev;
    int id = live[i].id;
    live[i].pid = 0;
    serial_puts("[mod] encerrado a forca pid=");
    serial_hex32(pid);
    serial_puts("\n");
    exec_release(pid, id);
    n++;
    pid = prev;
  }
  return n;
}

static int mod_exec_run(const char* path, int argc, char** argv);

/* Toda execucao vai para o log do sistema (Ctrl+F12): linha de comando e
 * codigo de saida. */
int mod_exec(const char* path, int argc, char** argv) {
  const char* base = path ? path : "?";
  const char* p;
  int a, rc, room = 200;
  for (p = base; *p; p++)
    if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
  syslog_stamp();
  syslog_puts("> ");
  syslog_puts(path ? path : "?");
  for (a = 1; argv && a < argc && room > 0; a++) {
    const char* s = argv[a] ? argv[a] : "";
    syslog_putc(' ');
    while (*s && room-- > 0) syslog_putc(*s++);
  }
  if (room <= 0) syslog_puts(" ...");
  syslog_putc('\n');
  rc = mod_exec_run(path, argc, argv);
  syslog_stamp();
  syslog_puts("< ");
  syslog_puts(base);
  if (rc == 0) syslog_puts(" OK (0)\n");
  else {
    syslog_puts(" retornou ");
    syslog_num(rc);
    if (rc <= MOD_ERR_NOTFOUND && rc >= MOD_ERR_DEPTH) {
      syslog_puts(": ");
      syslog_puts(mod_last_error());
    }
    syslog_putc('\n');
  }
  return rc;
}

static int mod_exec_run(const char* path, int argc, char** argv) {
  int id;
  int rc;
  uint8_t* stack;
  uint32_t ssz;
  uint32_t pid;
  uint32_t prev_owner;
  uint32_t prev_pid;
  char prev_drive;
  int pe;
  int is64 = 0;
  void* entry = 0;
  const char* pname = "?";
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
  /* EXE do Windows (MZ/PE): roda pelo WINE.DLL, so com SET WINE=1. */
  pe = wine_is_pe(path);
  if (pe) {
    id = -1;
    /* filho de um EXE do Windows: a mesma copia do WINE.DLL (TROCA WINE) */
    entry = wine_entry_from(prev_pid, argc, argv);
    if (!entry) {
      um_set_owner(prev_owner);
      cur_pid = prev_pid;
      exe_drive = prev_drive;
      return MOD_ERR_FORMAT;
    }
    is64 = 1;
    ssz = WINE_STACK;
    pname = "WINE";
  } else {
    id = load(path, 0);
  }
  if (!pe && id < 0) {
    um_set_owner(prev_owner);
    cur_pid = prev_pid;
    exe_drive = prev_drive;
    return MOD_ERR_IMPORT;
  }
  if (!pe) {
    entry = mods[id].base + mods[id].entry;
    is64 = mods[id].is64;
    ssz = mods[id].stack_size ? mods[id].stack_size : MOD_STACK_DEFAULT;
    pname = mods[id].name;
  }
  if (!pe && mods[id].entry == SOX_NO_ENTRY) {
    set_err("EXE sem _start: ", mods[id].name);
    unload(id);
    um_set_owner(prev_owner);
    cur_pid = prev_pid;
    exe_drive = prev_drive;
    return MOD_ERR_FORMAT;
  }
  stack = (uint8_t*)umalloc_owned(ssz, pid);
  if (!stack) {
    set_err("sem memoria para a pilha", 0);
    if (pe) wine_exec_done_pid(pid);
    unload(id);
    um_set_owner(prev_owner);
    cur_pid = prev_pid;
    exe_drive = prev_drive;
    return MOD_ERR_MEMORY;
  }
  {
    int nargc = argc;
    /* argc=0 (EXPLORER, HTTPD no boot) quebra programas POSIX: o Firefox le
     * argv[1..] alem do NULL e cai em #GP. Sempre entrega argv[0] = o EXE. */
    /* argv[0] e sempre o caminho completo (U:\FIREFOX\FIREFOX.EXE), nao o que
     * foi digitado ("firefox"): readlink("/proc/self/exe") sai dele e o Gecko
     * acha a propria pasta por ali. */
    char* self_av[32];
    char self_path[DRV_PATH];
    void* av;
    if (nargc <= 0 || !argv) nargc = 1;
    if (nargc > 31) nargc = 31;
    {
      int ai;
      char L;
      /* "U:FIREFOX.EXE" (relativo a pasta atual) -> "U:\FIREFOX\FIREFOX.EXE" */
      self_path[0] = 0;
      path_parse(path, &L, self_path + 3, DRV_PATH - 3);
      self_path[0] = L;
      self_path[1] = ':';
      self_path[2] = '\\';
      self_av[0] = self_path[3] ? self_path : (char*)path;
      for (ai = 1; ai < nargc; ai++) self_av[ai] = argv[ai];
      self_av[nargc] = 0;
      argv = self_av;
    }
    av = pack_user_argv(&nargc, argv, stack, ssz, is64);
    if (!av) {
      set_err("argumentos nao cabem na pilha", 0);
      if (pe) wine_exec_done_pid(pid);
      unload(id);
      um_set_owner(prev_owner);
      cur_pid = prev_pid;
      ufree_owner(pid);
      exe_drive = prev_drive;
      return MOD_ERR_MEMORY;
    }
    kbd_break_take();   /* Ctrl+C antigo (do prompt) nao mata o novo */
    live_add(pid, prev_pid, id);
    {
      uint64_t prev_fs = mod_fs_read();
      uint64_t prev_teb = kt_win_teb_get();
      uint64_t canary = 0x9e3779b97f4a7c15ull;
      int ti;
      for (ti = 0; ti < (int)sizeof(mod_tls_block); ti++) mod_tls_block[ti] = 0;
      for (ti = 0; ti < 8; ti++) mod_tls_block[0x28 + ti] = (uint8_t)(canary >> (ti * 8));
      mod_fs_write((uint64_t)(uintptr_t)mod_tls_block);
      serial_puts("[mod] exec ");
    serial_puts(pname);
    serial_puts(" entry=");
    serial_hex32((uint32_t)(uintptr_t)entry);
    serial_puts(" stack=");
    serial_hex32((uint32_t)(uintptr_t)stack);
    serial_puts(" ssz=");
    serial_hex32(ssz);
    serial_puts(" av=");
    serial_hex32((uint32_t)(uintptr_t)av);
    serial_puts("\n");
      /* PC sem serial: o log ate aqui vai para D:\LOG\SERIAL.LOG antes de o
       * programa rodar (se ele derrubar a maquina, o log mostra quem foi). */
      {
        /* no maximo a cada 30 s: o Claude abre ~9 processos WINE e cada
         * gravacao (1,4 MB com o final do log) segurava o disco e o BKL */
        uint32_t net_ms(void);
        static uint32_t last_save;
        static int saved_once;
        uint32_t now = net_ms();
        if (!saved_once || now - last_save >= 30000u) {
          saved_once = 1;
          last_save = now;
          (void)serial_log_save();
        }
      }
      {
        /* Codigo do programa roda sem o BKL: threads dele (e de outros)
         * entram no kernel enquanto isso. Volta com a mesma profundidade,
         * inclusive quando o mod_exit pula direto para ca. */
        int bkl = kt_bkl_suspend();
        rc = mod_call_entry(entry, nargc, av, av);
        kt_bkl_resume(bkl);
      }
      mod_fs_write(prev_fs);
      if (kt_win_teb_get() != prev_teb) kt_win_teb_set(prev_teb);
      serial_puts("[mod] saiu ");
      serial_puts(pname);
      serial_puts(" rc=");
      serial_hex32((uint32_t)rc);
      serial_puts("\n");
    }
  }
  {
    int li = live_find(pid);
    if (li >= 0) live[li].pid = 0;
  }
  um_set_owner(prev_owner);
  cur_pid = prev_pid;
  exec_release(pid, id);
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

/* DLL do proprio kernel (driver de sistema de arquivos): pid 0, nao entra na
 * lista do programa corrente e nao some quando ele termina. */
int mod_dll_load_sys(const char* path) {
  uint32_t pid = cur_pid;
  char drv = exe_drive;
  int id;
  if (!path) return 0;
  cur_pid = 0;
  exe_drive = path[0] && path[1] == ':' ? path[0] : drives_current();
  load_fresh = 1;
  id = load(path, 1);
  load_fresh = 0;
  cur_pid = pid;
  exe_drive = drv;
  return id < 0 ? 0 : id + 1;
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
  out->blk[1] = (uint32_t)(uintptr_t)(m->elf ? (void*)m->elf : (void*)m->exports);
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
