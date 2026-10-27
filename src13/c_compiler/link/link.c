/* link.c - Linker do SO: junta .OBJ (COBJ do cc ou ELF32 do gcc), .LIB e
 * exports de .DLL/.IMP num .EXE ou .DLL no formato SOX (sox.h).
 *
 *   link [/out:X.EXE] [/dll] [/entry:nome] [/export:nome] [/exportall]
 *        [/stack:bytes] [/map] arquivos...
 *   link /lib /out:X.LIB a.obj b.obj        (biblioteca estatica)
 *
 * Arquivos: .obj/.o (objetos), .lib (so entra o que for usado),
 * .dll (fornece imports), .imp (texto: "LIBRARY NOME" + um nome por linha).
 * Escrito em C simples para que o proprio cc do SO consiga compila-lo. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sox.h"

#define K_TEXT 1
#define K_DATA 2
#define K_BSS  3

#define SYM_UNDEF  (-1)
#define SYM_ABS    (-2)

struct sect {
  int obj;
  int kind;
  unsigned char* data;
  unsigned size;
  unsigned align;
  unsigned off;      /* offset dentro do segmento de saida */
};

struct lsym {
  char* name;
  int sect;          /* indice em sects, SYM_UNDEF ou SYM_ABS */
  unsigned value;
  int global;
  int weak;
  int exp;
};

struct lrel {
  int sect;
  unsigned offset;
  int sym;           /* indice global em syms */
  unsigned type;
};

struct obj {
  char name[64];
  int active;
  int sym0, nsym;
  int sect0, nsect;
  int rel0, nrel;
};

struct gsym {
  char* name;
  int sym;           /* definicao (indice em syms) ou -1 */
  int import;        /* indice em imps ou -1 */
  int referenced;
  int next;          /* encadeamento do hash */
};

struct imp {
  char* lib;
  char* name;
  unsigned thunk;    /* offset do thunk na imagem */
  unsigned slot;     /* offset do ponteiro na imagem */
  unsigned lib_off;  /* strtab de saida */
  unsigned name_off;
};

struct provider {
  char* lib;
  char* name;
};

static struct sect* sects;
static int nsects, capsects;
static struct lsym* syms;
static int nsyms, capsyms;
static struct lrel* rels;
static int nrels, caprels;
static struct obj* objs;
static int nobjs, capobjs;
static struct gsym* gs;
static int ngs, capgs;
static int ghash[4096];
static struct imp* imps;
static int nimps, capimps;
static struct provider* provs;
static int nprovs, capprovs;

static int errors;
static int opt_dll;
static int opt_map;
static int opt_exportall;
static unsigned opt_stack;
static char* opt_entry;
static char** opt_exports;
static int nexp_opt;

static void* xrealloc(void* p, unsigned n) {
  void* q = realloc(p, n ? n : 1);
  if (!q) {
    fprintf(stderr, "link: sem memoria\n");
    exit(2);
  }
  return q;
}

static char* xstrdup(const char* s) {
  int n = (int)strlen(s);
  char* d = (char*)xrealloc(0, (unsigned)n + 1);
  memcpy(d, s, (unsigned)n + 1);
  return d;
}

#define GROW(arr, n, cap, type) \
  if ((n) >= (cap)) { (cap) = (cap) ? (cap) * 2 : 64; (arr) = (type*)xrealloc((arr), (unsigned)(cap) * (unsigned)sizeof(type)); }

static int up(int c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  return c;
}

static int ieq(const char* a, const char* b) {
  while (*a && *b) {
    if (up(*a) != up(*b)) return 0;
    a++;
    b++;
  }
  return *a == *b;
}

static int ends_with(const char* s, const char* suf) {
  int a = (int)strlen(s);
  int b = (int)strlen(suf);
  if (b > a) return 0;
  return ieq(s + a - b, suf);
}

static const char* base_of(const char* p) {
  const char* b = p;
  for (; *p; p++)
    if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
  return b;
}

static unsigned rd32(const unsigned char* p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned rd16(const unsigned char* p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static void wr32(unsigned char* p, unsigned v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}

static unsigned char* read_file(const char* path, unsigned* size) {
  FILE* f = fopen(path, "rb");
  unsigned char* buf;
  long n;
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) {
    fclose(f);
    return 0;
  }
  buf = (unsigned char*)xrealloc(0, (unsigned)n + 1);
  if (n > 0 && fread(buf, 1, (unsigned)n, f) != (unsigned)n) {
    fclose(f);
    free(buf);
    return 0;
  }
  fclose(f);
  buf[n] = 0;
  *size = (unsigned)n;
  return buf;
}

static unsigned hash(const char* s) {
  unsigned h = 2166136261u;
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= 16777619u;
  }
  return h & 4095u;
}

static int gfind(const char* name, int create) {
  unsigned h = hash(name);
  int i = ghash[h] - 1;
  while (i >= 0) {
    if (strcmp(gs[i].name, name) == 0) return i;
    i = gs[i].next;
  }
  if (!create) return -1;
  GROW(gs, ngs, capgs, struct gsym);
  gs[ngs].name = xstrdup(name);
  gs[ngs].sym = -1;
  gs[ngs].import = -1;
  gs[ngs].referenced = 0;
  gs[ngs].next = ghash[h] - 1;
  ghash[h] = ngs + 1;
  return ngs++;
}

static int new_obj(const char* name) {
  int i;
  GROW(objs, nobjs, capobjs, struct obj);
  memset(&objs[nobjs], 0, sizeof(struct obj));
  for (i = 0; name[i] && i < 63; i++) objs[nobjs].name[i] = name[i];
  objs[nobjs].name[i] = 0;
  objs[nobjs].sym0 = nsyms;
  objs[nobjs].sect0 = nsects;
  objs[nobjs].rel0 = nrels;
  return nobjs++;
}

static int add_sect(int obj, int kind, const unsigned char* data, unsigned size, unsigned align) {
  struct sect* s;
  GROW(sects, nsects, capsects, struct sect);
  s = &sects[nsects];
  s->obj = obj;
  s->kind = kind;
  s->size = size;
  s->align = align ? align : 1;
  if (s->align > 4096) s->align = 4096;
  s->off = 0;
  s->data = 0;
  if (kind != K_BSS) {
    s->data = (unsigned char*)xrealloc(0, size ? size : 1);
    if (size) memcpy(s->data, data, size);
  }
  objs[obj].nsect++;
  return nsects++;
}

static int add_sym(int obj, const char* name, int sect, unsigned value, int global, int weak, int exp) {
  struct lsym* s;
  GROW(syms, nsyms, capsyms, struct lsym);
  s = &syms[nsyms];
  s->name = xstrdup(name);
  s->sect = sect;
  s->value = value;
  s->global = global;
  s->weak = weak;
  s->exp = exp;
  objs[obj].nsym++;
  return nsyms++;
}

static void add_rel(int obj, int sect, unsigned offset, int sym, unsigned type) {
  GROW(rels, nrels, caprels, struct lrel);
  rels[nrels].sect = sect;
  rels[nrels].offset = offset;
  rels[nrels].sym = sym;
  rels[nrels].type = type;
  objs[obj].nrel++;
  nrels++;
}

/* ---------------- COBJ ---------------- */
static int load_cobj(const char* name, const unsigned char* b, unsigned size) {
  unsigned th, dh, bh, ns, nr, sts;
  unsigned off;
  const unsigned char* sy;
  const unsigned char* rl;
  const char* st;
  int o, st_text, st_data, st_bss;
  unsigned i;
  if (size < 28) return -1;
  th = rd32(b + 4);
  dh = rd32(b + 8);
  bh = rd32(b + 12);
  ns = rd32(b + 16);
  nr = rd32(b + 20);
  sts = rd32(b + 24);
  off = 28;
  if (off + th + dh + ns * 16 + nr * 16 + sts > size) {
    fprintf(stderr, "link: %s: objeto truncado\n", name);
    errors++;
    return -1;
  }
  o = new_obj(name);
  st_text = add_sect(o, K_TEXT, b + off, th, 16);
  off += th;
  st_data = add_sect(o, K_DATA, b + off, dh, 16);
  off += dh;
  st_bss = add_sect(o, K_BSS, 0, bh, 16);
  sy = b + off;
  rl = sy + ns * 16;
  st = (const char*)(rl + nr * 16);
  for (i = 0; i < ns; i++) {
    unsigned nm = rd32(sy + i * 16);
    unsigned sc = rd32(sy + i * 16 + 4);
    unsigned val = rd32(sy + i * 16 + 8);
    unsigned fl = rd32(sy + i * 16 + 12);
    int s = SYM_UNDEF;
    if (nm >= sts) nm = 0;
    if (sc == COB_SECT_TEXT) s = st_text;
    else if (sc == COB_SECT_DATA) s = st_data;
    else if (sc == COB_SECT_BSS) s = st_bss;
    add_sym(o, st + nm, s, val, (fl & COB_SYM_GLOBAL) != 0 || s == SYM_UNDEF, 0,
            (fl & COB_SYM_EXPORT) != 0);
  }
  for (i = 0; i < nr; i++) {
    unsigned sc = rd32(rl + i * 16);
    unsigned ro = rd32(rl + i * 16 + 4);
    unsigned rs = rd32(rl + i * 16 + 8);
    unsigned rt = rd32(rl + i * 16 + 12);
    int s = sc == COB_SECT_TEXT ? st_text : st_data;
    if (rs >= ns || (sc != COB_SECT_TEXT && sc != COB_SECT_DATA)) {
      fprintf(stderr, "link: %s: relocacao invalida\n", name);
      errors++;
      continue;
    }
    add_rel(o, s, ro, objs[o].sym0 + (int)rs, rt);
  }
  return o;
}

/* ---------------- ELF32 relocavel (gcc -m32 -c) ---------------- */
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_NOBITS   8
#define SHT_REL      9
#define SHF_WRITE    1
#define SHF_ALLOC    2
#define SHF_EXEC     4

static int load_elf(const char* name, const unsigned char* b, unsigned size) {
  unsigned shoff, shentsize, shnum, shstrndx;
  const unsigned char* sh;
  const char* shstr;
  int o;
  int* map;
  unsigned i;
  int symtab = -1;
  unsigned sym_count = 0;
  int sym_base;
  if (size < 52 || b[4] != 1 || b[5] != 1 || rd16(b + 16) != 1 || rd16(b + 18) != 3) {
    fprintf(stderr, "link: %s: ELF nao e relocavel i386\n", name);
    errors++;
    return -1;
  }
  shoff = rd32(b + 32);
  shentsize = rd16(b + 46);
  shnum = rd16(b + 48);
  shstrndx = rd16(b + 50);
  if (shoff + shnum * shentsize > size || shstrndx >= shnum) {
    fprintf(stderr, "link: %s: ELF truncado\n", name);
    errors++;
    return -1;
  }
  sh = b + shoff;
  shstr = (const char*)(b + rd32(sh + shstrndx * shentsize + 16));
  o = new_obj(name);
  map = (int*)xrealloc(0, shnum * (unsigned)sizeof(int));
  for (i = 0; i < shnum; i++) {
    const unsigned char* s = sh + i * shentsize;
    unsigned type = rd32(s + 4);
    unsigned flags = rd32(s + 8);
    unsigned soff = rd32(s + 16);
    unsigned ssize = rd32(s + 20);
    unsigned align = rd32(s + 32);
    const char* sname = shstr + rd32(s);
    int kind;
    map[i] = -1;
    if (type == SHT_SYMTAB) symtab = (int)i;
    if (!(flags & SHF_ALLOC)) continue;
    if (type != SHT_PROGBITS && type != SHT_NOBITS) continue;
    if (strncmp(sname, ".eh_frame", 9) == 0) continue;
    if (strncmp(sname, ".note", 5) == 0) continue;
    if (type == SHT_NOBITS) kind = K_BSS;
    else if (flags & SHF_EXEC) kind = K_TEXT;
    else kind = K_DATA;
    if (kind != K_BSS && soff + ssize > size) continue;
    map[i] = add_sect(o, kind, b + soff, ssize, align);
  }
  if (symtab < 0) {
    free(map);
    return o;
  }
  {
    const unsigned char* s = sh + (unsigned)symtab * shentsize;
    unsigned soff = rd32(s + 16);
    unsigned ssize = rd32(s + 20);
    unsigned link = rd32(s + 24);
    const char* str = (const char*)(b + rd32(sh + link * shentsize + 16));
    sym_count = ssize / 16;
    sym_base = nsyms;
    for (i = 0; i < sym_count; i++) {
      const unsigned char* e = b + soff + i * 16;
      unsigned nm = rd32(e);
      unsigned val = rd32(e + 4);
      unsigned sz = rd32(e + 8);
      unsigned info = e[12];
      unsigned shndx = rd16(e + 14);
      unsigned bind = info >> 4;
      unsigned typ = info & 15;
      const char* n = str + nm;
      int sect = SYM_UNDEF;
      if (typ == 3 && shndx < shnum) n = shstr + rd32(sh + shndx * shentsize);
      if (shndx == 0) sect = SYM_UNDEF;
      else if (shndx == 0xFFF1) sect = SYM_ABS;
      else if (shndx == 0xFFF2) {
        unsigned al = val ? val : 4;
        int bs = add_sect(o, K_BSS, 0, sz, al);
        sect = bs;
        val = 0;
      } else if (shndx < shnum) {
        sect = map[shndx];
        if (sect < 0) sect = SYM_ABS;
      }
      add_sym(o, n, sect, val, bind != 0 && typ != 3 && typ != 4, bind == 2, 0);
    }
  }
  for (i = 0; i < shnum; i++) {
    const unsigned char* s = sh + i * shentsize;
    unsigned type = rd32(s + 4);
    unsigned soff = rd32(s + 16);
    unsigned ssize = rd32(s + 20);
    unsigned target = rd32(s + 28);
    unsigned k;
    if (type != SHT_REL) continue;
    if (target >= shnum || map[target] < 0) continue;
    for (k = 0; k + 8 <= ssize; k += 8) {
      unsigned roff = rd32(b + soff + k);
      unsigned info = rd32(b + soff + k + 4);
      unsigned rsym = info >> 8;
      unsigned rtype = info & 255;
      unsigned t;
      if (rtype == 0) continue;
      if (rtype == 1) t = COB_REL_ABS32;
      else if (rtype == 2 || rtype == 4) t = COB_REL_REL32;
      else {
        fprintf(stderr, "link: %s: relocacao ELF tipo %u nao suportada (compile com -fno-pic)\n",
                name, rtype);
        errors++;
        continue;
      }
      if (rsym >= sym_count) continue;
      add_rel(o, map[target], roff, sym_base + (int)rsym, t);
    }
  }
  free(map);
  return o;
}

static int load_object(const char* name, const unsigned char* b, unsigned size) {
  if (size >= 4 && rd32(b) == COB_MAGIC) return load_cobj(name, b, size);
  if (size >= 4 && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F')
    return load_elf(name, b, size);
  fprintf(stderr, "link: %s: formato de objeto desconhecido\n", name);
  errors++;
  return -1;
}

/* ---------------- provedores de import ---------------- */
static void add_provider(const char* lib, const char* name) {
  GROW(provs, nprovs, capprovs, struct provider);
  provs[nprovs].lib = xstrdup(lib);
  provs[nprovs].name = xstrdup(name);
  nprovs++;
}

static void load_dll_exports(const char* path, const unsigned char* b, unsigned size) {
  unsigned ne, oe, sts, ost, i;
  char lib[32];
  const char* bn = base_of(path);
  if (size < 64 || rd32(b) != SOX_MAGIC || !(rd32(b + 4) & SOX_FLAG_DLL)) {
    fprintf(stderr, "link: %s: nao e uma DLL SOX\n", path);
    errors++;
    return;
  }
  for (i = 0; bn[i] && i < 31; i++) lib[i] = (char)up(bn[i]);
  lib[i] = 0;
  ne = rd32(b + 44);
  oe = rd32(b + 48);
  sts = rd32(b + 52);
  ost = rd32(b + 56);
  if (oe + ne * 8 > size || ost + sts > size) {
    fprintf(stderr, "link: %s: DLL truncada\n", path);
    errors++;
    return;
  }
  for (i = 0; i < ne; i++) {
    unsigned nm = rd32(b + oe + i * 8);
    if (nm < sts) add_provider(lib, (const char*)(b + ost + nm));
  }
}

static void load_imp(const char* path, unsigned char* b, unsigned size) {
  char lib[64];
  char* p = (char*)b;
  char* e = (char*)b + size;
  int first = 1;
  strcpy(lib, "KERNEL");
  while (p < e) {
    char* line = p;
    char* q;
    while (p < e && *p != '\n') p++;
    if (p < e) *p++ = 0;
    q = line + strlen(line);
    while (q > line && (q[-1] == '\r' || q[-1] == ' ' || q[-1] == '\t')) *--q = 0;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == ';' || *line == '#') continue;
    if (first && strncmp(line, "LIBRARY", 7) == 0) {
      int i = 0;
      line += 7;
      while (*line == ' ') line++;
      while (line[i] && i < 63) {
        lib[i] = (char)up(line[i]);
        i++;
      }
      lib[i] = 0;
      first = 0;
      continue;
    }
    first = 0;
    add_provider(lib, line);
  }
}

static void load_lib(const char* path, const unsigned char* b, unsigned size) {
  unsigned n, off, i;
  if (size < 8 || rd32(b) != CLB_MAGIC) {
    fprintf(stderr, "link: %s: nao e uma biblioteca .LIB\n", path);
    errors++;
    return;
  }
  n = rd32(b + 4);
  off = 8;
  for (i = 0; i < n; i++) {
    char name[96];
    unsigned sz;
    int o;
    if (off + 32 > size) break;
    sz = rd32(b + off + 24);
    sprintf(name, "%s(%.24s)", base_of(path), (const char*)(b + off));
    off += 32;
    if (off + sz > size) break;
    o = load_object(name, b + off, sz);
    if (o >= 0) objs[o].active = 0;
    off += (sz + 3u) & ~3u;
  }
}

/* ---------------- resolucao ---------------- */
static void define_globals(int o) {
  int i;
  for (i = objs[o].sym0; i < objs[o].sym0 + objs[o].nsym; i++) {
    struct lsym* s = &syms[i];
    int g;
    if (!s->global || s->sect == SYM_UNDEF) continue;
    g = gfind(s->name, 1);
    if (gs[g].sym >= 0) {
      struct lsym* old = &syms[gs[g].sym];
      if (s->weak) continue;
      if (old->weak) {
        gs[g].sym = i;
        continue;
      }
      fprintf(stderr, "link: simbolo duplicado: %s (em %s e %s)\n", s->name,
              objs[o].name, objs[sects[old->sect >= 0 ? old->sect : 0].obj].name);
      errors++;
      continue;
    }
    gs[g].sym = i;
  }
}

static void reference_undefs(int o) {
  int i;
  for (i = objs[o].sym0; i < objs[o].sym0 + objs[o].nsym; i++) {
    if (syms[i].sect == SYM_UNDEF && syms[i].name[0]) {
      int g = gfind(syms[i].name, 1);
      gs[g].referenced = 1;
    }
  }
}

static int defines_needed(int o) {
  int i;
  for (i = objs[o].sym0; i < objs[o].sym0 + objs[o].nsym; i++) {
    struct lsym* s = &syms[i];
    int g;
    if (!s->global || s->sect == SYM_UNDEF) continue;
    g = gfind(s->name, 0);
    if (g >= 0 && gs[g].referenced && gs[g].sym < 0) return 1;
  }
  return 0;
}

static int find_provider(const char* name) {
  int i;
  for (i = 0; i < nprovs; i++)
    if (strcmp(provs[i].name, name) == 0) return i;
  return -1;
}

/* Endereco (offset na imagem) de um simbolo local resolvido. */
static unsigned seg_base[4];

static int sym_target(int si, unsigned* addr, int* absolute) {
  struct lsym* s = &syms[si];
  *absolute = 0;
  if (s->sect == SYM_UNDEF) {
    int g = gfind(s->name, 0);
    if (g < 0) return -1;
    if (gs[g].sym >= 0) {
      s = &syms[gs[g].sym];
    } else if (gs[g].import >= 0) {
      *addr = imps[gs[g].import].thunk;
      return 0;
    } else if (s->weak) {
      *addr = 0;
      *absolute = 1;
      return 0;
    } else {
      return -1;
    }
  }
  if (s->sect == SYM_ABS) {
    *addr = s->value;
    *absolute = 1;
    return 0;
  }
  if (s->sect == SYM_UNDEF) return -1;
  *addr = seg_base[sects[s->sect].kind] + sects[s->sect].off + s->value;
  return 0;
}

static unsigned align_up(unsigned v, unsigned a) {
  return (v + a - 1) / a * a;
}

static int cmp_u32(const void* a, const void* b) {
  unsigned x = *(const unsigned*)a;
  unsigned y = *(const unsigned*)b;
  return x < y ? -1 : x > y;
}

/* ---------------- saida ---------------- */
struct sbuf {
  char* p;
  unsigned n, cap;
};

static unsigned sb_add(struct sbuf* sb, const char* s) {
  unsigned at = sb->n;
  unsigned len = (unsigned)strlen(s) + 1;
  if (sb->n + len > sb->cap) {
    sb->cap = (sb->n + len) * 2 + 64;
    sb->p = (char*)xrealloc(sb->p, sb->cap);
  }
  memcpy(sb->p + sb->n, s, len);
  sb->n += len;
  return at;
}

static int write_all(const char* path, const unsigned char* d, unsigned n) {
  FILE* f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "link: nao consegui criar %s\n", path);
    return -1;
  }
  if (n && fwrite(d, 1, n, f) != n) {
    fclose(f);
    fprintf(stderr, "link: erro gravando %s\n", path);
    return -1;
  }
  fclose(f);
  return 0;
}

static int do_link(const char* out) {
  unsigned text_end, thunk_base, data_base, iat_base, image_size, bss_base, bss_size;
  unsigned char* image;
  unsigned* brel = 0;
  int nbrel = 0, capbrel = 0;
  int i, changed;
  int entry_sym = -1;
  unsigned entry = SOX_NO_ENTRY;
  struct sbuf st;
  int nexports = 0;
  unsigned* exp_name = 0;
  unsigned* exp_val = 0;
  struct sox_hdr h;
  unsigned char* file;
  unsigned fsize, off;

  for (i = 0; i < nobjs; i++)
    if (objs[i].active) {
      define_globals(i);
      reference_undefs(i);
    }
  if (!opt_dll) {
    int g = gfind(opt_entry ? opt_entry : "_start", 1);
    gs[g].referenced = 1;
  }
  for (i = 0; i < nexp_opt; i++) gs[gfind(opt_exports[i], 1)].referenced = 1;
  do {
    changed = 0;
    for (i = 0; i < nobjs; i++) {
      if (objs[i].active || !defines_needed(i)) continue;
      objs[i].active = 1;
      define_globals(i);
      reference_undefs(i);
      changed = 1;
    }
  } while (changed);

  for (i = 0; i < ngs; i++) {
    int p;
    if (!gs[i].referenced || gs[i].sym >= 0) continue;
    p = find_provider(gs[i].name);
    if (p < 0) continue;
    GROW(imps, nimps, capimps, struct imp);
    imps[nimps].lib = provs[p].lib;
    imps[nimps].name = gs[i].name;
    gs[i].import = nimps++;
  }

  for (i = 0; i < nobjs; i++) {
    int k;
    if (!objs[i].active) continue;
    for (k = objs[i].sym0; k < objs[i].sym0 + objs[i].nsym; k++) {
      int g;
      if (syms[k].sect != SYM_UNDEF || !syms[k].name[0] || syms[k].weak) continue;
      g = gfind(syms[k].name, 0);
      if (g >= 0 && (gs[g].sym >= 0 || gs[g].import >= 0)) continue;
      fprintf(stderr, "link: simbolo indefinido: %s (usado em %s)\n", syms[k].name, objs[i].name);
      errors++;
      if (g >= 0) gs[g].import = -2;
    }
  }
  if (errors) return -1;

  /* layout: text | thunks | data | iat | bss */
  {
    unsigned pos[4];
    pos[K_TEXT] = pos[K_DATA] = pos[K_BSS] = 0;
    for (i = 0; i < nsects; i++) {
      struct sect* s = &sects[i];
      if (!objs[s->obj].active) continue;
      pos[s->kind] = align_up(pos[s->kind], s->align);
      s->off = pos[s->kind];
      pos[s->kind] += s->size;
    }
    text_end = pos[K_TEXT];
    thunk_base = align_up(text_end, 8);
    data_base = align_up(thunk_base + (unsigned)nimps * 8u, 16);
    iat_base = align_up(data_base + pos[K_DATA], 8);
    image_size = iat_base + (unsigned)nimps * 8u;
    bss_base = align_up(image_size, 16);
    bss_size = (bss_base - image_size) + pos[K_BSS];
    seg_base[K_TEXT] = 0;
    seg_base[K_DATA] = data_base;
    seg_base[K_BSS] = bss_base;
  }
  image = (unsigned char*)xrealloc(0, image_size + 1);
  memset(image, 0, image_size + 1);
  for (i = 0; i < nsects; i++) {
    struct sect* s = &sects[i];
    if (!objs[s->obj].active || s->kind == K_BSS || !s->size) continue;
    memcpy(image + seg_base[s->kind] + s->off, s->data, s->size);
  }
  for (i = thunk_base > text_end ? (int)text_end : (int)thunk_base; i < (int)thunk_base; i++)
    image[i] = 0xCC;
  for (i = 0; i < nimps; i++) {
    unsigned t = thunk_base + (unsigned)i * 8u;
    imps[i].thunk = t;
    imps[i].slot = iat_base + (unsigned)i * 8u;
    /* FF 25 disp32 em long mode e jmp qword [rip+disp]. O slot tem 8 bytes
     * (o loader grava o ponteiro nos 4 de baixo; o resto fica zero). */
    image[t] = 0xFF;
    image[t + 1] = 0x25;
    wr32(image + t + 2, (unsigned)(imps[i].slot - (t + 6u)));
    image[t + 6] = 0xCC;
    image[t + 7] = 0xCC;
  }

  for (i = 0; i < nrels; i++) {
    struct lrel* r = &rels[i];
    struct sect* s = &sects[r->sect];
    unsigned p, a, target;
    int absolute;
    if (!objs[s->obj].active) continue;
    if (s->kind == K_BSS || r->offset + 4 > s->size) {
      fprintf(stderr, "link: %s: relocacao fora da secao\n", objs[s->obj].name);
      errors++;
      continue;
    }
    p = seg_base[s->kind] + s->off + r->offset;
    if (sym_target(r->sym, &target, &absolute) != 0) {
      fprintf(stderr, "link: %s: simbolo nao resolvido: %s\n", objs[s->obj].name, syms[r->sym].name);
      errors++;
      continue;
    }
    a = rd32(image + p);
    if (r->type == COB_REL_ABS32) {
      wr32(image + p, a + target);
      if (!absolute) {
        if (nbrel >= capbrel) {
          capbrel = capbrel ? capbrel * 2 : 256;
          brel = (unsigned*)xrealloc(brel, (unsigned)capbrel * 4u);
        }
        brel[nbrel++] = p;
      }
    } else if (r->type == COB_REL_REL32) {
      if (absolute) {
        fprintf(stderr, "link: %s: chamada relativa para simbolo absoluto %s\n", objs[s->obj].name,
                syms[r->sym].name);
        errors++;
        continue;
      }
      wr32(image + p, a + target - p);
    } else {
      fprintf(stderr, "link: %s: tipo de relocacao %u desconhecido\n", objs[s->obj].name, r->type);
      errors++;
    }
  }
  if (errors) return -1;
  if (nbrel > 1) qsort(brel, (unsigned)nbrel, 4, cmp_u32);

  st.p = 0;
  st.n = st.cap = 0;
  sb_add(&st, "");

  /* exports */
  exp_name = (unsigned*)xrealloc(0, (unsigned)(ngs + 1) * 4u);
  exp_val = (unsigned*)xrealloc(0, (unsigned)(ngs + 1) * 4u);
  for (i = 0; i < ngs; i++) {
    int want = 0;
    int k;
    unsigned addr;
    int absolute;
    if (gs[i].sym < 0) continue;
    if (syms[gs[i].sym].exp) want = 1;
    if (opt_exportall && opt_dll && syms[gs[i].sym].sect != SYM_ABS) want = 1;
    for (k = 0; k < nexp_opt; k++)
      if (strcmp(opt_exports[k], gs[i].name) == 0) want = 1;
    if (!want) continue;
    if (strcmp(gs[i].name, "DllMain") == 0) continue;
    if (sym_target(gs[i].sym, &addr, &absolute) != 0) continue;
    exp_name[nexports] = sb_add(&st, gs[i].name);
    exp_val[nexports] = addr;
    nexports++;
  }
  for (i = 0; i < nexp_opt; i++) {
    int g = gfind(opt_exports[i], 0);
    if (g < 0 || gs[g].sym < 0) {
      fprintf(stderr, "link: /export:%s nao esta definido\n", opt_exports[i]);
      errors++;
    }
  }

  {
    const char* en = opt_entry ? opt_entry : (opt_dll ? "DllMain" : "_start");
    int g = gfind(en, 0);
    if (g >= 0 && gs[g].sym >= 0) entry_sym = gs[g].sym;
    if (entry_sym >= 0) {
      int absolute;
      sym_target(entry_sym, &entry, &absolute);
    } else if (!opt_dll || opt_entry) {
      fprintf(stderr, "link: entrada '%s' nao definida\n", en);
      errors++;
    }
  }
  if (errors) return -1;

  for (i = 0; i < nimps; i++) {
    imps[i].lib_off = sb_add(&st, imps[i].lib);
    imps[i].name_off = sb_add(&st, imps[i].name);
  }

  memset(&h, 0, sizeof(h));
  h.magic = SOX_MAGIC;
  h.flags = opt_dll ? SOX_FLAG_DLL : 0;
  h.image_size = image_size;
  h.bss_size = bss_size;
  h.entry = entry;
  h.text_size = text_end;
  h.stack_size = opt_stack;
  h.nrelocs = (unsigned)nbrel;
  h.nimports = (unsigned)nimps;
  h.nexports = (unsigned)nexports;
  h.strtab_size = st.n;
  off = 64;
  h.off_image = off;
  off += align_up(image_size, 4);
  h.off_relocs = off;
  off += (unsigned)nbrel * 4u;
  h.off_imports = off;
  off += (unsigned)nimps * 12u;
  h.off_exports = off;
  off += (unsigned)nexports * 8u;
  h.off_strtab = off;
  off += st.n;
  fsize = off;
  file = (unsigned char*)xrealloc(0, fsize);
  memset(file, 0, fsize);
  {
    unsigned* hp = (unsigned*)&h;
    for (i = 0; i < 16; i++) wr32(file + i * 4, hp[i]);
  }
  memcpy(file + h.off_image, image, image_size);
  for (i = 0; i < nbrel; i++) wr32(file + h.off_relocs + (unsigned)i * 4u, brel[i]);
  for (i = 0; i < nimps; i++) {
    unsigned char* e = file + h.off_imports + (unsigned)i * 12u;
    wr32(e, imps[i].lib_off);
    wr32(e + 4, imps[i].name_off);
    wr32(e + 8, imps[i].slot);
  }
  for (i = 0; i < nexports; i++) {
    wr32(file + h.off_exports + (unsigned)i * 8u, exp_name[i]);
    wr32(file + h.off_exports + (unsigned)i * 8u + 4, exp_val[i]);
  }
  memcpy(file + h.off_strtab, st.p, st.n);

  if (opt_map) {
    printf("text %u  thunks %u  data %u  iat %u  bss %u\n", text_end, (unsigned)nimps * 8u,
           iat_base - data_base, (unsigned)nimps * 4u, bss_size);
    printf("entrada %08X  relocacoes %d  imports %d  exports %d\n", entry, nbrel, nimps, nexports);
    for (i = 0; i < nimps; i++) printf("  import %s!%s\n", imps[i].lib, imps[i].name);
    for (i = 0; i < nexports; i++) printf("  export %08X %s\n", exp_val[i], st.p + exp_name[i]);
  }
  return write_all(out, file, fsize);
}

static int do_lib(const char* out, char** files, int n) {
  unsigned char* buf = 0;
  unsigned size = 8, cap = 0;
  int i, count = 0;
  for (i = 0; i < n; i++) {
    unsigned fs;
    unsigned char* d = read_file(files[i], &fs);
    const char* bn = base_of(files[i]);
    unsigned need;
    int k;
    if (!d) {
      fprintf(stderr, "link: nao consegui ler %s\n", files[i]);
      return -1;
    }
    need = size + 32 + ((fs + 3u) & ~3u);
    if (need > cap) {
      cap = need * 2;
      buf = (unsigned char*)xrealloc(buf, cap);
    }
    memset(buf + size, 0, 32 + ((fs + 3u) & ~3u));
    for (k = 0; bn[k] && k < 23; k++) buf[size + k] = (unsigned char)bn[k];
    wr32(buf + size + 24, fs);
    memcpy(buf + size + 32, d, fs);
    size = need;
    count++;
    free(d);
  }
  if (!buf) buf = (unsigned char*)xrealloc(0, 8);
  wr32(buf, CLB_MAGIC);
  wr32(buf + 4, (unsigned)count);
  return write_all(out, buf, size);
}

static void usage(void) {
  printf("Uso: link [/out:X.EXE] [/dll] [/entry:nome] [/export:nome] [/exportall]\n");
  printf("            [/stack:bytes] [/map] arquivos.obj .lib .dll .imp\n");
  printf("     link /lib /out:X.LIB a.obj b.obj\n");
}

/* "/out:/mnt/x.exe" e opcao; "/mnt/x.obj" (caminho do Linux) e arquivo. */
static int is_opt(const char* a) {
  const char* p;
  if (a[0] == '-') return a[1] != 0;
  if (a[0] != '/') return 0;
  for (p = a + 1; *p && *p != ':'; p++)
    if (*p == '/') return 0;
  return 1;
}

int main(int argc, char** argv) {
  char* out = 0;
  int make_lib = 0;
  char** files;
  int nfiles = 0;
  int i;
  char defout[64];
  files = (char**)xrealloc(0, (unsigned)(argc + 1) * (unsigned)sizeof(char*));
  opt_exports = (char**)xrealloc(0, (unsigned)(argc + 1) * (unsigned)sizeof(char*));
  for (i = 1; i < argc; i++) {
    char* a = argv[i];
    if (is_opt(a)) {
      char* o = a + 1;
      if (strncmp(o, "out:", 4) == 0 || strncmp(o, "OUT:", 4) == 0) out = o + 4;
      else if (strcmp(o, "o") == 0 && i + 1 < argc) out = argv[++i];
      else if (ieq(o, "dll") || strcmp(o, "shared") == 0) opt_dll = 1;
      else if (ieq(o, "lib")) make_lib = 1;
      else if (ieq(o, "map")) opt_map = 1;
      else if (ieq(o, "exportall")) opt_exportall = 1;
      else if (strncmp(o, "entry:", 6) == 0) opt_entry = o + 6;
      else if (strncmp(o, "export:", 7) == 0) opt_exports[nexp_opt++] = o + 7;
      else if (strncmp(o, "stack:", 6) == 0) opt_stack = (unsigned)strtoul(o + 6, 0, 0);
      else if (ieq(o, "?") || ieq(o, "help") || ieq(o, "h")) {
        usage();
        return 0;
      } else {
        fprintf(stderr, "link: opcao desconhecida %s\n", a);
        return 1;
      }
    } else {
      files[nfiles++] = a;
    }
  }
  if (nfiles == 0) {
    usage();
    return 1;
  }
  if (!out) {
    const char* bn = base_of(files[0]);
    int k = 0;
    while (bn[k] && bn[k] != '.' && k < 50) {
      defout[k] = bn[k];
      k++;
    }
    strcpy(defout + k, make_lib ? ".LIB" : opt_dll ? ".DLL" : ".EXE");
    out = defout;
  }
  if (make_lib) return do_lib(out, files, nfiles) == 0 ? 0 : 1;
  for (i = 0; i < nfiles; i++) {
    unsigned size;
    unsigned char* b = read_file(files[i], &size);
    if (!b) {
      fprintf(stderr, "link: nao consegui ler %s\n", files[i]);
      errors++;
      continue;
    }
    if (ends_with(files[i], ".dll")) load_dll_exports(files[i], b, size);
    else if (ends_with(files[i], ".imp")) load_imp(files[i], b, size);
    else if (ends_with(files[i], ".lib") || (size >= 4 && rd32(b) == CLB_MAGIC)) load_lib(files[i], b, size);
    else {
      int o = load_object(base_of(files[i]), b, size);
      if (o >= 0) objs[o].active = 1;
    }
  }
  if (errors) return 1;
  if (do_link(out) != 0) return 1;
  return 0;
}
