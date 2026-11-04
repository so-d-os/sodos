/* link64.c - linker SOX2 para a ABI LP64 (x86_64).
 *
 * Le objetos ELF64 relocaveis (gcc -m64) e um .IMP, resolve simbolos e
 * imports do kernel, e escreve uma imagem SOX2. Diferente do link.c (SOX1/
 * x32), aqui os campos de relocacao e os slots de import tem 8 bytes.
 *
 *   link64 [/out:ARQ.EXE] [/entry:nome] a.o b.o KERNEL.IMP
 *
 * Suporta R_X86_64_64 (base+addend no load) e R_X86_64_PC32/PLT32 (resolvido
 * aqui). Relocacoes absolutas de 32 bits sao rejeitadas: compile com
 * -mcmodel=large. Sem DLLs/exports nesta primeira versao.
 */
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sox.h"

#define MAX_OBJ 256
#define MAX_SEC 131072
#define MAX_IMP 256
#define MAX_SYM 200000
#define MAX_REL 2000000
#define MAX_GOT 8192

/* Pilha pedida ao loader para executaveis SOX2 (0 = padrao do kernel). O
 * SpiderMonkey precisa de varios MB para compilar o self-hosting. */
#define SOX_STACK64 0x01000000u

struct obj {
  uint8_t* d;
  size_t sz;
  Elf64_Ehdr* eh;
  int nsec;
};

struct imp {
  char name[96];
  uint32_t thunk;
  uint32_t slot;
};

struct gsym {
  char name[128];
  uint32_t addr;
  int weak;
  int defined;
  int obj;
  int shndx;
  uint64_t stval;
};

static struct obj objs[MAX_OBJ];
static int nobj;
static uint32_t sec_off[MAX_OBJ][MAX_SEC];
static uint32_t sec_size[MAX_OBJ][MAX_SEC];
static int sec_bss[MAX_OBJ][MAX_SEC];
static struct imp imps[MAX_IMP];
static int nimp;
static struct gsym gsyms[MAX_SYM];
static int ngs;
static char imp_names[MAX_IMP][96];
static int nimp_names;
static uint32_t rel_abs[MAX_REL];
static int nrel_abs;

/* ---- SHN_XINDEX: com >0xFF00 secoes o indice real de uma secao fica na
 * tabela SHT_SYMTAB_SHNDX (e o simbolo usa st_shndx == SHN_XINDEX). ---- */
struct xtab {
  int symtab; /* indice da secao SHT_SYMTAB dona desta tabela */
  const uint8_t* data;
  uint32_t cnt;
};
static struct xtab xtabs[MAX_OBJ][4];
static int nxtab[MAX_OBJ];

static void init_xindex(int o) {
  int j;
  nxtab[o] = 0;
  for (j = 0; j < objs[o].nsec && nxtab[o] < 4; j++) {
    Elf64_Shdr* sh = (Elf64_Shdr*)(objs[o].d + objs[o].eh->e_shoff +
                                   (uint64_t)j * objs[o].eh->e_shentsize);
    if (sh->sh_type != SHT_SYMTAB_SHNDX) continue;
    xtabs[o][nxtab[o]].symtab = (int)sh->sh_link;
    xtabs[o][nxtab[o]].data = objs[o].d + sh->sh_offset;
    xtabs[o][nxtab[o]].cnt = (uint32_t)(sh->sh_size / 4u);
    nxtab[o]++;
  }
}

/* Indice real da secao do simbolo. ok=0 se nao foi possivel resolver. */
static uint32_t sym_shndx(int o, int symtab, uint32_t si, const Elf64_Sym* s, int* ok) {
  int k;
  if (s->st_shndx != SHN_XINDEX) {
    *ok = 1;
    return s->st_shndx;
  }
  for (k = 0; k < nxtab[o]; k++)
    if (xtabs[o][k].symtab == symtab && si < xtabs[o][k].cnt) {
      *ok = 1;
      return ((const uint32_t*)xtabs[o][k].data)[si];
    }
  *ok = 0;
  return 0;
}

struct gotent {
  char name[128];
  uint32_t off;
};
static void die(const char* msg);
static struct gotent gots[MAX_GOT];
static int ngot;
static uint32_t got_base;
static uint32_t init_start, init_end, fini_start, fini_end;

static struct gotent* got_find(const char* name) {
  int i;
  for (i = 0; i < ngot; i++)
    if (strcmp(gots[i].name, name) == 0) return &gots[i];
  return NULL;
}

static struct gotent* got_add(const char* name) {
  struct gotent* g = got_find(name);
  if (g) return g;
  if (ngot >= MAX_GOT) die("GOT cheio");
  g = &gots[ngot];
  g->off = (uint32_t)ngot * 8u;
  strncpy(g->name, name, sizeof(g->name) - 1);
  g->name[sizeof(g->name) - 1] = 0;
  ngot++;
  return g;
}

static void die(const char* msg) {
  fprintf(stderr, "link64: %s\n", msg);
  exit(1);
}

static uint32_t align_up(uint32_t v, uint32_t a) {
  if (a < 1) a = 1;
  return (v + a - 1u) & ~(a - 1u);
}

static Elf64_Shdr* shdr(int o, int i) {
  Elf64_Ehdr* eh = objs[o].eh;
  return (Elf64_Shdr*)(objs[o].d + eh->e_shoff + (uint64_t)i * eh->e_shentsize);
}

static const char* sec_name(int o, int i) {
  Elf64_Ehdr* eh = objs[o].eh;
  int sx = (int)eh->e_shstrndx;
  if (eh->e_shstrndx == SHN_XINDEX) sx = (int)shdr(o, 0)->sh_link;
  {
    Elf64_Shdr* sh = shdr(o, sx);
    return (const char*)(objs[o].d + sh->sh_offset + shdr(o, i)->sh_name);
  }
}

static int is_alloc(int o, int i) {
  return (shdr(o, i)->sh_flags & SHF_ALLOC) != 0;
}

static struct gsym* find_gsym(const char* name) {
  int i;
  for (i = 0; i < ngs; i++)
    if (strcmp(gsyms[i].name, name) == 0) return &gsyms[i];
  return NULL;
}

static int have_imp_name(const char* name) {
  int i;
  for (i = 0; i < nimp_names; i++)
    if (strcmp(imp_names[i], name) == 0) return 1;
  return 0;
}

static int add_import(const char* name) {
  int i;
  for (i = 0; i < nimp; i++)
    if (strcmp(imps[i].name, name) == 0) return i;
  if (nimp >= MAX_IMP) die("imports demais");
  if (!have_imp_name(name)) {
    fprintf(stderr, "link64: import '%s' nao esta no .IMP\n", name);
    exit(1);
  }
  strncpy(imps[nimp].name, name, sizeof(imps[nimp].name) - 1);
  imps[nimp].name[sizeof(imps[nimp].name) - 1] = 0;
  imps[nimp].thunk = 0;
  imps[nimp].slot = 0;
  return nimp++;
}

static void read_imp(const char* path) {
  FILE* f = fopen(path, "rb");
  char line[256];
  if (!f) {
    fprintf(stderr, "link64: nao abri %s\n", path);
    exit(1);
  }
  while (fgets(line, sizeof(line), f)) {
    char* p = line;
    size_t n;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!*p || *p == '#' || strncmp(p, "LIBRARY", 7) == 0) continue;
    n = strlen(p);
    while (n && (p[n - 1] == '\r' || p[n - 1] == '\n' || p[n - 1] == ' ')) p[--n] = 0;
    if (!n) continue;
    if (nimp_names >= MAX_IMP) die("nomes de import demais");
    if (n > sizeof(imp_names[0]) - 1) n = sizeof(imp_names[0]) - 1;
    memcpy(imp_names[nimp_names], p, n);
    imp_names[nimp_names][n] = 0;
    nimp_names++;
  }
  fclose(f);
}

static void load_obj(const char* path) {
  FILE* f = fopen(path, "rb");
  long sz;
  struct obj* o;
  if (!f) {
    fprintf(stderr, "link64: nao abri %s\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < (long)sizeof(Elf64_Ehdr)) die("objeto pequeno demais");
  if (nobj >= MAX_OBJ) die("objetos demais");
  o = &objs[nobj];
  o->d = malloc((size_t)sz);
  if (!o->d) die("sem memoria");
  if (fread(o->d, 1, (size_t)sz, f) != (size_t)sz) die("leitura curta");
  fclose(f);
  o->sz = (size_t)sz;
  o->eh = (Elf64_Ehdr*)o->d;
  if (memcmp(o->eh->e_ident, ELFMAG, SELFMAG) != 0 || o->eh->e_ident[EI_CLASS] != ELFCLASS64 ||
      o->eh->e_machine != EM_X86_64 || o->eh->e_type != ET_REL)
    die("nao e ELF64 x86_64 relocavel (use gcc -m64)");
  o->nsec = (int)o->eh->e_shnum;
  if (o->nsec == 0) {
    Elf64_Shdr* s0 = (Elf64_Shdr*)(o->d + o->eh->e_shoff);
    o->nsec = (int)s0->sh_size;
  }
  if (o->nsec <= 0 || o->nsec >= MAX_SEC) die("secoes demais");
  nobj++;
}

static uint32_t sym_addr(int o, int symtab, uint32_t si, const Elf64_Sym* s,
                         const char* name) {
  struct gsym* g;
  int ok;
  uint32_t shndx = sym_shndx(o, symtab, si, s, &ok);
  if (!ok) return 0;
  if (s->st_shndx != SHN_XINDEX) {
    if (shndx == SHN_UNDEF) {
      g = find_gsym(name);
      if (g && g->defined) return g->addr;
      return 0;
    }
    if (shndx == SHN_ABS) return (uint32_t)s->st_value;
    if (shndx >= SHN_LORESERVE) return 0;
  }
  if (shndx >= MAX_SEC) return 0;
  return sec_off[o][shndx] + (uint32_t)s->st_value;
}

int main(int argc, char** argv) {
  const char* out = "OUT.EXE";
  const char* entry_name = "_start";
  uint32_t off = 0;
  uint32_t image_size, bss_size, thunk_off;
  uint8_t* image;
  int i, j;

  for (i = 1; i < argc; i++) {
    if (strncmp(argv[i], "/out:", 5) == 0) out = argv[i] + 5;
    else if (strncmp(argv[i], "/entry:", 7) == 0) entry_name = argv[i] + 7;
    else if (strstr(argv[i], ".imp") || strstr(argv[i], ".IMP")) read_imp(argv[i]);
    else load_obj(argv[i]);
  }
  if (nobj == 0) die("nenhum objeto");
  for (i = 0; i < nobj; i++) init_xindex(i);

  /* 1. layout das secoes com conteudo */
  for (i = 0; i < nobj; i++) {
    Elf64_Ehdr* eh = objs[i].eh;
    for (j = 0; j < objs[i].nsec; j++) {
      Elf64_Shdr* sh = shdr(i, j);
      if (!is_alloc(i, j)) continue;
      {
        const char* sn = sec_name(i, j);
        if (strstr(sn, ".init_array") || strstr(sn, ".fini_array")) continue;
      }
      if (sh->sh_type == SHT_NOBITS) {
        sec_size[i][j] = (uint32_t)sh->sh_size;
        sec_bss[i][j] = 1;
        continue;
      }
      off = align_up(off, (uint32_t)sh->sh_addralign);
      sec_off[i][j] = off;
      sec_size[i][j] = (uint32_t)sh->sh_size;
      off += (uint32_t)sh->sh_size;
    }
  }
  /* 1b. .init_array/.fini_array contiguos (construtores C++). */
  init_start = align_up(off, 8);
  for (i = 0; i < nobj; i++)
    for (j = 0; j < objs[i].nsec; j++) {
      Elf64_Shdr* sh = shdr(i, j);
      if (!is_alloc(i, j) || sh->sh_type == SHT_NOBITS) continue;
      if (!strstr(sec_name(i, j), ".init_array")) continue;
      off = align_up(off, (uint32_t)sh->sh_addralign);
      sec_off[i][j] = off;
      sec_size[i][j] = (uint32_t)sh->sh_size;
      off += (uint32_t)sh->sh_size;
    }
  init_end = off;
  fini_start = align_up(off, 8);
  for (i = 0; i < nobj; i++)
    for (j = 0; j < objs[i].nsec; j++) {
      Elf64_Shdr* sh = shdr(i, j);
      if (!is_alloc(i, j) || sh->sh_type == SHT_NOBITS) continue;
      if (!strstr(sec_name(i, j), ".fini_array")) continue;
      off = align_up(off, (uint32_t)sh->sh_addralign);
      sec_off[i][j] = off;
      sec_size[i][j] = (uint32_t)sh->sh_size;
      off += (uint32_t)sh->sh_size;
    }
  fini_end = off;
  {
    uint32_t text_end = align_up(off, 16);

    /* 2. simbolos globais definidos */
    for (i = 0; i < nobj; i++) {
      Elf64_Ehdr* eh = objs[i].eh;
      for (j = 0; j < objs[i].nsec; j++) {
        Elf64_Shdr* sh = shdr(i, j);
        Elf64_Sym* sym;
        const char* str;
        uint32_t n, s;
        if (sh->sh_type != SHT_SYMTAB) continue;
        sym = (Elf64_Sym*)(objs[i].d + sh->sh_offset);
        n = (uint32_t)(sh->sh_size / sizeof(Elf64_Sym));
        str = (const char*)(objs[i].d + shdr(i, (int)sh->sh_link)->sh_offset);
        for (s = 0; s < n; s++) {
          const char* nm = str + sym[s].st_name;
          struct gsym* g;
          int okx;
          uint32_t rst;
          if (!nm[0] || sym[s].st_shndx == SHN_UNDEF) continue;
          if (ELF64_ST_BIND(sym[s].st_info) == STB_LOCAL) continue;
          rst = sym_shndx(i, j, s, &sym[s], &okx);
          if (!okx) continue;
          if (sym[s].st_shndx != SHN_XINDEX && rst == SHN_COMMON)
            die("simbolo COMMON nao suportado");
          g = find_gsym(nm);
          if (g && g->defined && !g->weak) continue;
          if (!g) {
            if (ngs >= MAX_SYM) die("simbolos demais");
            g = &gsyms[ngs++];
            strncpy(g->name, nm, sizeof(g->name) - 1);
            g->name[sizeof(g->name) - 1] = 0;
          }
          g->addr = sym_addr(i, j, s, &sym[s], nm);
          g->obj = i;
          g->shndx = (int)rst;
          g->stval = sym[s].st_value;
          g->weak = ELF64_ST_BIND(sym[s].st_info) == STB_WEAK;
          g->defined = 1;
        }
      }
    }

    /* 2a. simbolos das arrays de construtores/destrutores. */
    {
      const char* names[4] = {"__init_array_start", "__init_array_end",
                              "__fini_array_start", "__fini_array_end"};
      uint32_t vals[4] = {init_start, init_end, fini_start, fini_end};
      int k;
      for (k = 0; k < 4; k++) {
        struct gsym* g = find_gsym(names[k]);
        if (!g) {
          if (ngs >= MAX_SYM) die("simbolos demais");
          g = &gsyms[ngs++];
          strncpy(g->name, names[k], sizeof(g->name) - 1);
          g->name[sizeof(g->name) - 1] = 0;
        }
        g->defined = 1;
        g->weak = 1;
        g->shndx = SHN_ABS;
        g->stval = vals[k];
        g->addr = vals[k];
      }
      /* Tamanho da pilha pedido ao loader, exposto como simbolo absoluto para
       * que o programa (ex.: limite de pilha do SpiderMonkey) o conheca. */
      {
        struct gsym* g = find_gsym("__sodos_stack_size");
        if (!g) {
          if (ngs >= MAX_SYM) die("simbolos demais");
          g = &gsyms[ngs++];
          strcpy(g->name, "__sodos_stack_size");
        }
        g->defined = 1;
        g->weak = 1;
        g->shndx = SHN_ABS;
        g->stval = SOX_STACK64;
        g->addr = SOX_STACK64;
      }
    }

    /* 2b. GOT: descobre simbolos referenciados por GOTPCREL. */
    for (i = 0; i < nobj; i++) {
      Elf64_Ehdr* eh = objs[i].eh;
      for (j = 0; j < objs[i].nsec; j++) {
        Elf64_Shdr* sh = shdr(i, j);
        Elf64_Rela* r;
        Elf64_Sym* sym;
        const char* str;
        uint32_t n, s;
        if (sh->sh_type != SHT_RELA || !is_alloc(i, (int)sh->sh_info)) continue;
        r = (Elf64_Rela*)(objs[i].d + sh->sh_offset);
        n = (uint32_t)(sh->sh_size / sizeof(Elf64_Rela));
        sym = (Elf64_Sym*)(objs[i].d + shdr(i, (int)sh->sh_link)->sh_offset);
        str = (const char*)(objs[i].d + shdr(i, (int)shdr(i, (int)sh->sh_link)->sh_link)->sh_offset);
        for (s = 0; s < n; s++) {
          uint32_t type = (uint32_t)ELF64_R_TYPE(r[s].r_info);
          uint32_t si = (uint32_t)ELF64_R_SYM(r[s].r_info);
          const char* nm = str + sym[si].st_name;
          if (type == 9 || type == 41 || type == 42 || type == 27 || type == 28 || type == 29) {
            if (nm[0]) got_add(nm);
          }
        }
      }
    }
    if (!find_gsym("_GLOBAL_OFFSET_TABLE_")) {
      struct gsym* g;
      if (ngs >= MAX_SYM) die("simbolos demais");
      g = &gsyms[ngs++];
      strcpy(g->name, "_GLOBAL_OFFSET_TABLE_");
      g->defined = 1;
      g->weak = 0;
      g->shndx = SHN_ABS;
      g->stval = 0;
      g->addr = 0;
    }

    /* 3. descobre imports olhando as relocacoes */
    for (i = 0; i < nobj; i++) {
      Elf64_Ehdr* eh = objs[i].eh;
      for (j = 0; j < objs[i].nsec; j++) {
        Elf64_Shdr* sh = shdr(i, j);
        Elf64_Rela* r;
        Elf64_Sym* sym;
        const char* str;
        uint32_t n, s;
        if (sh->sh_type != SHT_RELA) continue;
        if (!is_alloc(i, (int)sh->sh_info)) continue;
        r = (Elf64_Rela*)(objs[i].d + sh->sh_offset);
        n = (uint32_t)(sh->sh_size / sizeof(Elf64_Rela));
        sym = (Elf64_Sym*)(objs[i].d + shdr(i, (int)sh->sh_link)->sh_offset);
        str = (const char*)(objs[i].d + shdr(i, (int)shdr(i, (int)sh->sh_link)->sh_link)->sh_offset);
        for (s = 0; s < n; s++) {
          uint32_t si = (uint32_t)ELF64_R_SYM(r[s].r_info);
          const char* nm = str + sym[si].st_name;
          if (!nm[0] || sym[si].st_shndx != SHN_UNDEF) continue;
          if (find_gsym(nm)) continue;
          if (ELF64_ST_BIND(sym[si].st_info) == STB_WEAK) continue;
          add_import(nm);
        }
      }
    }

    /* 4. thunks: FF 25 00000000 + slot de 8 bytes */
    thunk_off = align_up(text_end, 16);
    image_size = thunk_off;
    for (i = 0; i < nimp; i++) {
      imps[i].thunk = image_size;
      imps[i].slot = image_size + 6;
      image_size += 14;
    }
    image_size = align_up(image_size, 16);

    /* 4b. tabela GOT depois dos thunks e antes da bss. */
    got_base = align_up(image_size, 16);
    if (ngot) image_size = got_base + (uint32_t)ngot * 8u;
    image_size = align_up(image_size, 16);

    /* 5. bss depois da imagem */
    bss_size = 0;
    for (i = 0; i < nobj; i++) {
      Elf64_Ehdr* eh = objs[i].eh;
      for (j = 0; j < objs[i].nsec; j++) {
        if (!is_alloc(i, j) || !sec_bss[i][j]) continue;
        bss_size = align_up(bss_size, (uint32_t)shdr(i, j)->sh_addralign);
        sec_off[i][j] = image_size + bss_size;
        bss_size += sec_size[i][j];
      }
    }
    bss_size = align_up(bss_size, 16);

    /* Os simbolos globais foram criados antes da bss; agora que as secoes
     * NOBITS tem endereco, recalcula quem esta nelas. */
    for (i = 0; i < ngs; i++) {
      struct gsym* g = &gsyms[i];
      if (!g->defined) continue;
      if (g->shndx == SHN_ABS) g->addr = (uint32_t)g->stval;
      else if (g->shndx >= 0 && g->shndx < MAX_SEC)
        g->addr = sec_off[g->obj][g->shndx] + (uint32_t)g->stval;
    }
    {
      struct gsym* g = find_gsym("_GLOBAL_OFFSET_TABLE_");
      if (g) g->addr = got_base;
    }
  }

  image = calloc(1, image_size);
  if (!image) die("sem memoria para imagem");

  if (getenv("LINK64_LAYOUT")) {
    for (i = 0; i < nobj; i++) {
      Elf64_Ehdr* eh = objs[i].eh;
      for (j = 0; j < objs[i].nsec; j++) {
        if (!is_alloc(i, j)) continue;
        fprintf(stderr, "layout %08x %08x %s\n", sec_off[i][j], sec_size[i][j],
                sec_name(i, j));
      }
    }
  }

  /* copia secoes */
  for (i = 0; i < nobj; i++) {
    Elf64_Ehdr* eh = objs[i].eh;
    for (j = 0; j < objs[i].nsec; j++) {
      Elf64_Shdr* sh = shdr(i, j);
      if (!is_alloc(i, j) || sh->sh_type == SHT_NOBITS) continue;
      memcpy(image + sec_off[i][j], objs[i].d + sh->sh_offset, (size_t)sh->sh_size);
    }
  }
  for (i = 0; i < nimp; i++) {
    uint8_t* t = image + imps[i].thunk;
    t[0] = 0xFF;
    t[1] = 0x25;
    t[2] = t[3] = t[4] = t[5] = 0;
  }
  /* 5b. preenche a GOT (endereco relativo a base; o loader soma a base). */
  for (i = 0; i < ngot; i++) {
    struct gsym* g = find_gsym(gots[i].name);
    uint64_t v;
    uint32_t P = got_base + gots[i].off;
    if (g && g->defined) {
      v = g->addr;
    } else {
      int ii = add_import(gots[i].name);
      v = imps[ii].thunk;
    }
    *(uint64_t*)(image + P) = v;
    if (nrel_abs >= MAX_REL) die("relocacoes demais");
    rel_abs[nrel_abs++] = P;
  }

  /* 6. relocacoes */
  for (i = 0; i < nobj; i++) {
    Elf64_Ehdr* eh = objs[i].eh;
    for (j = 0; j < objs[i].nsec; j++) {
      Elf64_Shdr* sh = shdr(i, j);
      Elf64_Rela* r;
      Elf64_Sym* sym;
      const char* str;
      uint32_t n, s;
      int target;
      if (sh->sh_type != SHT_RELA) continue;
      target = (int)sh->sh_info;
      if (target < 0 || target >= objs[i].nsec || !is_alloc(i, target)) continue;
      r = (Elf64_Rela*)(objs[i].d + sh->sh_offset);
      n = (uint32_t)(sh->sh_size / sizeof(Elf64_Rela));
      sym = (Elf64_Sym*)(objs[i].d + shdr(i, (int)sh->sh_link)->sh_offset);
      str = (const char*)(objs[i].d + shdr(i, (int)shdr(i, (int)sh->sh_link)->sh_link)->sh_offset);
      for (s = 0; s < n; s++) {
        uint32_t si = (uint32_t)ELF64_R_SYM(r[s].r_info);
        uint32_t type = (uint32_t)ELF64_R_TYPE(r[s].r_info);
        const char* nm = str + sym[si].st_name;
        uint32_t P = sec_off[i][target] + (uint32_t)r[s].r_offset;
        int64_t S = 0;
        int64_t A = r[s].r_addend;
        if (type == R_X86_64_NONE) continue;
        if (sym[si].st_shndx == SHN_UNDEF) {
          struct gsym* g = find_gsym(nm);
          if (g && g->defined) S = (int64_t)g->addr;
          else if (nm[0] && ELF64_ST_BIND(sym[si].st_info) != STB_WEAK) {
            int ii = add_import(nm);
            S = (int64_t)imps[ii].thunk;
          } else {
            static int wu;
            if (wu < 40 && type != R_X86_64_NONE) {
              uint32_t nsym = (uint32_t)(shdr(i, (int)sh->sh_link)->sh_size /
                                         sizeof(Elf64_Sym));
              fprintf(stderr,
                      "undef t=%u si=%u/%u nm='%s' stn=%u sty=%x val=%llx A=%lld P=%x sec=%s\n",
                      type, si, nsym, nm, (unsigned)sym[si].st_name,
                      (unsigned)sym[si].st_info, (unsigned long long)sym[si].st_value,
                      (long long)A, P, sec_name(i, target));
              wu++;
            }
            S = 0;
          }
        } else {
          S = (int64_t)sym_addr(i, (int)sh->sh_link, si, &sym[si], nm);
        }
        if (P + 8u > image_size) die("relocacao fora da imagem");
        switch (type) {
          case R_X86_64_64:
            if (S == 0 && A != 0) {
              static int warned;
              if (warned < 300) {
                fprintf(stderr, "warn64 fraca/indefinida '%s' A=%lld P=%x\n", nm,
                        (long long)A, P);
                warned++;
              }
            }
            *(uint64_t*)(image + P) = (uint64_t)(S + A);
            if (nrel_abs >= MAX_REL) die("relocacoes demais");
            rel_abs[nrel_abs++] = P;
            break;
          case R_X86_64_PC32:
          case R_X86_64_PLT32: {
            int64_t v = S + A - (int64_t)P;
            *(int32_t*)(image + P) = (int32_t)v;
            break;
          }
          case 24: /* R_X86_64_PC64 (Rust -mcmodel=large) */
            *(int64_t*)(image + P) = (int64_t)(S + A - (int64_t)P);
            break;
          case 9:   /* R_X86_64_GOTPCREL */
          case 41:  /* R_X86_64_GOTPCRELX */
          case 42: { /* R_X86_64_REX_GOTPCRELX */
            struct gotent* ge = got_find(nm);
            if (!ge) {
              fprintf(stderr, "link64: GOT sem entrada para '%s'\n", nm);
              return 1;
            }
            *(int32_t*)(image + P) = (int32_t)((int64_t)got_base + ge->off - (int64_t)P);
            break;
          }
          case 27: { /* R_X86_64_GOT64 */
            struct gotent* ge = got_find(nm);
            if (!ge) {
              fprintf(stderr, "link64: GOT64 sem entrada para '%s'\n", nm);
              return 1;
            }
            /* No modelo large o codigo e' "movabs $off,%rax; mov (%r15,%rax),%rax"
             * com r15 = _GLOBAL_OFFSET_TABLE_: o imediato e' o offset do slot
             * dentro da GOT, nao o endereco do slot. */
            *(int64_t*)(image + P) = (int64_t)ge->off + A;
            break;
          }
          case 25: /* R_X86_64_GOTOFF64 */
            *(int64_t*)(image + P) = (int64_t)(S + A - (int64_t)got_base);
            break;
          case 31: /* R_X86_64_PLTOFF64 */
            *(int64_t*)(image + P) = (int64_t)(S + A - (int64_t)got_base);
            break;
          case 28:  /* R_X86_64_GOTPCREL64 */
          case 29: { /* R_X86_64_GOTPC64 */
            int64_t off = 0;
            if (strcmp(nm, "_GLOBAL_OFFSET_TABLE_") != 0) {
              struct gotent* ge = got_find(nm);
              if (!ge) {
                fprintf(stderr, "link64: GOT64 sem entrada para '%s'\n", nm);
                return 1;
              }
              off = (int64_t)ge->off;
            }
            /* GOTPC64 carrega addend (ex.: +9) para acertar o PC da lea. */
            *(int64_t*)(image + P) = (int64_t)got_base + off + A - (int64_t)P;
            break;
          }
          case R_X86_64_32:
          case R_X86_64_32S:
            fprintf(stderr, "link64: reloc absoluta de 32 bits em '%s'; use -mcmodel=large\n", nm);
            return 1;
          default:
            fprintf(stderr, "link64: reloc tipo %u nao suportada (%s)\n", type, nm);
            return 1;
        }
      }
    }
  }

  /* 7. strtab + imports */
  {
    char strtab[16384];
    uint32_t strn = 0, name_off[MAX_IMP];
    uint32_t entry = SOX_NO_ENTRY;
    struct sox_hdr h;
    uint32_t off_relocs, off_imports, off_strtab;
    FILE* f;
    memset(&h, 0, sizeof(h));
    memset(strtab, 0, sizeof(strtab));
    strcpy(strtab, "KERNEL");
    strn = 7;
    for (i = 0; i < nimp; i++) {
      name_off[i] = strn;
      strcpy(strtab + strn, imps[i].name);
      strn += (uint32_t)strlen(imps[i].name) + 1;
    }
    for (i = 0; i < ngs; i++)
      if (strcmp(gsyms[i].name, entry_name) == 0) entry = gsyms[i].addr;
    if (getenv("LINK64_MAP")) {
      for (i = 0; i < ngs; i++)
        fprintf(stderr, "map %08x %s\n", gsyms[i].addr, gsyms[i].name);
    }
    {
      FILE* mf = fopen("/mnt/c/dev/so/sodos/src13/firefox/build/link64.map", "w");
      if (mf) {
        fprintf(mf, "# init %08x %08x fini %08x %08x\n", init_start, init_end, fini_start, fini_end);
        for (i = 0; i < ngs; i++)
          fprintf(mf, "%08x %s\n", gsyms[i].addr, gsyms[i].name);
        fclose(mf);
      }
    }

    off_relocs = sizeof(h) + image_size;
    off_imports = off_relocs + (uint32_t)nrel_abs * 4u;
    off_strtab = off_imports + (uint32_t)nimp * sizeof(struct sox_import);

    h.magic = SOX_MAGIC2;
    h.flags = 0;
    h.image_size = image_size;
    h.bss_size = bss_size;
    h.entry = entry;
    h.text_size = image_size;
    h.stack_size = SOX_STACK64;
    h.nrelocs = (uint32_t)nrel_abs;
    h.off_relocs = off_relocs;
    h.nimports = (uint32_t)nimp;
    h.off_imports = off_imports;
    h.nexports = 0;
    h.off_exports = 0;
    h.strtab_size = strn;
    h.off_strtab = off_strtab;
    h.off_image = sizeof(h);

    f = fopen(out, "wb");
    if (!f) die("nao consegui criar a saida");
    fwrite(&h, 1, sizeof(h), f);
    fwrite(image, 1, image_size, f);
    fwrite(rel_abs, 4, (size_t)nrel_abs, f);
    for (i = 0; i < nimp; i++) {
      struct sox_import im;
      im.lib = 0;
      im.name = name_off[i];
      im.slot = imps[i].slot;
      fwrite(&im, 1, sizeof(im), f);
    }
    fwrite(strtab, 1, strn, f);
    fclose(f);
    printf("link64: %s image=%u bss=%u imports=%d relocs=%d entry=%u\n", out, image_size,
           bss_size, nimp, nrel_abs, entry);
  }
  return 0;
}
