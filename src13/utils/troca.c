/*
 * troca.c - TROCA.EXE: troca o kernel e os DLLs do sistema sem reiniciar.
 * Lado do kernel: kernel/troca.c. Manual: docs/TROCA.md.
 *
 *   TROCA                              o que esta rodando (kernel, WINE.DLL, drivers)
 *   TROCA KERNEL [novo.elf] [atual.elf] [/T] [/F]
 *   TROCA WINE [caminho\WINE.DLL]
 *   TROCA DLL FAT32.DLL | LINKEXT.DLL
 *   TROCA arquivo.ELF | arquivo.DLL    o mesmo, pelo tipo do arquivo
 *
 * Kernel. O ELF novo precisa das relocacoes (o Makefile liga com ld -q). O
 * .text+.rodata dele vai inteiro para um bloco do kernel (k_troca_alloc,
 * abaixo de 2 GB); as relocacoes sao refeitas: codigo e constantes apontam
 * para o bloco, variaveis apontam para as que ja estao na memoria (mesma
 * chave: nome, ou arquivo:nome das static) e as que nao existiam ganham
 * memoria nova, com o valor inicial do ELF. Depois o kernel (k_troca_apply)
 * para as outras CPUs e poe um jmp no comeco de cada funcao antiga para a
 * nova. Programas, janelas, arquivos abertos e threads continuam.
 *
 * O que a troca nao cobre (reinicie): variavel que mudou de tamanho (/F
 * aceita, copiando o que cabe), struct que mudou de layout, laco que nunca
 * volta (o laco do WIN e o shell continuam no codigo antigo; as funcoes que
 * eles chamam mudam), driver de video.
 *
 * Referencias ambiguas: o .rela de uma static aponta para ".bss + desloc",
 * nao para a variavel. Perto de onde o layout mudou, "fim da A" e "comeco da
 * B" podem ser o mesmo endereco no ELF novo e enderecos diferentes na
 * memoria; uma RIP-relativa ainda tem o -4..-8 do fim da instrucao no
 * desloc. Para esses casos: tamanho da instrucao (decodificador x86-64), a
 * mesma instrucao no codigo antigo quando a funcao nao mudou, e as outras
 * referencias da funcao. Sem decisao segura a troca e recusada.
 */
#include <so.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include "../kernel/troca.h"

/* ---------- API do kernel (k_dl_sym: num kernel sem a troca, avisa) ---------- */
typedef int (*info_fn)(struct k_troca_info*);
typedef void* (*alloc_fn)(unsigned);
typedef void (*free_fn)(void*);
typedef int (*apply_fn)(struct k_troca_req*);
typedef int (*estado_fn)(char*, int);
typedef int (*dll_fn)(const char*, char*, int);
static info_fn kt_info;
static alloc_fn kt_alloc;
static free_fn kt_free;
static apply_fn kt_apply;
static estado_fn kt_estado;
static dll_fn kt_dll;

static int api(void) {
  kt_info = (info_fn)k_dl_sym(0, "k_troca_info", 0);
  kt_alloc = (alloc_fn)k_dl_sym(0, "k_troca_alloc", 0);
  kt_free = (free_fn)k_dl_sym(0, "k_troca_free", 0);
  kt_apply = (apply_fn)k_dl_sym(0, "k_troca_apply", 0);
  kt_estado = (estado_fn)k_dl_sym(0, "k_troca_estado", 0);
  kt_dll = (dll_fn)k_dl_sym(0, "k_troca_dll", 0);
  if (kt_info && kt_alloc && kt_free && kt_apply && kt_estado && kt_dll) return 1;
  printf("Este kernel nao tem a troca a quente (k_troca_*).\n"
         "Reinicie uma vez com um kernel que tenha (kernel/troca.c); dai em diante\n"
         "o TROCA troca kernel e DLLs sem reiniciar.\n");
  return 0;
}

/* ---------- mensagens ---------- */
static int nprob, navis;
static void problema(const char* fmt, ...) {
  char b[300];
  va_list ap;
  nprob++;
  if (nprob > 40) return;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  printf("  ! %s\n", b);
}
static void aviso(const char* fmt, ...) {
  char b[300];
  va_list ap;
  navis++;
  if (navis > 20) return;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  printf("  - %s\n", b);
}

static void* xmalloc(size_t n) {
  void* p = malloc(n ? n : 1);
  if (!p) {
    printf("TROCA: sem memoria (%lu bytes)\n", (unsigned long)n);
    exit(2);
  }
  return p;
}

static char* arena;
static size_t arena_left;
static char* sdup(const char* s) {
  size_t n = strlen(s) + 1;
  char* r;
  if (n > arena_left) {
    size_t c = n > 65536 ? n : 65536;
    arena = (char*)xmalloc(c);
    arena_left = c;
  }
  r = arena;
  memcpy(r, s, n);
  arena += n;
  arena_left -= n;
  return r;
}

static int ieq(const char* a, const char* b) {
  while (*a && *b) {
    char x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
    a++;
    b++;
  }
  return *a == *b;
}

static int termina(const char* s, const char* suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && ieq(s + a - b, suf);
}

static uint8_t* ler(const char* path, uint32_t* n) {
  int sz = k_fsize(path);
  uint8_t* b;
  if (sz <= 0) return 0;
  b = (uint8_t*)malloc((size_t)sz + 16);
  if (!b) return 0;
  if (k_fload(path, (char*)b, sz + 1) != sz) {
    free(b);
    return 0;
  }
  *n = (uint32_t)sz;
  return b;
}

/* ---------- ELF ---------- */
typedef struct {
  uint8_t ident[16];
  uint16_t type, machine;
  uint32_t version;
  uint64_t entry, phoff, shoff;
  uint32_t flags;
  uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Ehdr;
typedef struct {
  uint32_t name, type;
  uint64_t flags, addr, offset, size;
  uint32_t link, info;
  uint64_t addralign, entsize;
} Shdr;
typedef struct {
  uint32_t name;
  uint8_t info, other;
  uint16_t shndx;
  uint64_t value, size;
} Sym;
typedef struct {
  uint64_t offset, info;
  int64_t addend;
} Rela;

#define SHT_SYMTAB 2
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHN_UNDEF 0
#define SHN_LORES 0xFF00
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STT_FILE 4
#define STB_LOCAL 0

#define R_NONE 0
#define R_64 1
#define R_PC32 2
#define R_PLT32 4
#define R_32 10
#define R_32S 11
#define R_16 12
#define R_PC16 13
#define R_8 14
#define R_PC8 15
#define R_PC64 24

struct sym {
  const char* key;       /* "nome" (global) ou "arquivo#n:nome" (static) */
  uint64_t addr;         /* novo: no ELF; velho: na memoria */
  uint64_t size;         /* efetivo (rotulo: ate o proximo simbolo) */
  char kind;             /* 'F' funcao, 'L' rotulo no .text, 'R' .rodata, 'D' .data/.bss */
  char sec;              /* 't' 'r' 'd' 'b' */
  uint16_t shndx;
  int32_t alias;         /* D de tamanho 0 no endereco de um objeto: o objeto */
  int32_t par;           /* indice do par no velho, -1 */
  uint64_t rt;           /* D do novo: endereco na memoria */
  int32_t run;           /* D do novo: trecho em que o mapa e linear */
  uint8_t fresco;        /* D do novo: memoria nova */
  uint8_t ok;            /* D do novo: rt valido */
  int8_t igual;          /* F do novo: -1 nao visto, 0 mudou, 1 igual ao velho */
};

struct elf {
  const char* path;
  uint8_t* b;
  uint32_t n;
  const Ehdr* eh;
  const Shdr* sh;
  const char* shstr;
  int text, rodata, data, bss, symtab, strtab, rtext, rrodata, rdata;
  const Sym* st;
  int nst;
  const char* str;
  struct sym* s;
  int ns;
  int* symmap;           /* .symtab -> s */
};

static const char* sec_nome(const struct elf* e, int i) { return e->shstr + e->sh[i].name; }

static int elf_abre(struct elf* e, const char* path, int quieto) {
  int i;
  memset(e, 0, sizeof(*e));
  e->path = path;
  e->text = e->rodata = e->data = e->bss = e->symtab = e->strtab = -1;
  e->rtext = e->rrodata = e->rdata = -1;
  e->b = ler(path, &e->n);
  if (!e->b) {
    if (!quieto) printf("TROCA: nao consegui ler %s\n", path);
    return 0;
  }
  e->eh = (const Ehdr*)e->b;
  if (e->n < sizeof(Ehdr) || memcmp(e->eh->ident, "\177ELF", 4) || e->eh->ident[4] != 2 || e->eh->machine != 62 ||
      e->eh->shentsize != sizeof(Shdr) || e->eh->shoff + (uint64_t)e->eh->shnum * sizeof(Shdr) > e->n ||
      e->eh->shstrndx >= e->eh->shnum) {
    if (!quieto) printf("TROCA: %s nao e um ELF de 64 bits do x86-64\n", path);
    return 0;
  }
  e->sh = (const Shdr*)(e->b + e->eh->shoff);
  e->shstr = (const char*)(e->b + e->sh[e->eh->shstrndx].offset);
  for (i = 0; i < e->eh->shnum; i++) {
    const char* nm = sec_nome(e, i);
    if (e->sh[i].type != SHT_NOBITS && e->sh[i].offset + e->sh[i].size > e->n) continue;
    if (!strcmp(nm, ".text")) e->text = i;
    else if (!strcmp(nm, ".rodata")) e->rodata = i;
    else if (!strcmp(nm, ".data")) e->data = i;
    else if (!strcmp(nm, ".bss")) e->bss = i;
    else if (!strcmp(nm, ".symtab") && e->sh[i].type == SHT_SYMTAB) e->symtab = i;
    else if (!strcmp(nm, ".strtab")) e->strtab = i;
    else if (!strcmp(nm, ".rela.text")) e->rtext = i;
    else if (!strcmp(nm, ".rela.rodata")) e->rrodata = i;
    else if (!strcmp(nm, ".rela.data")) e->rdata = i;
  }
  if (e->text < 0 || e->rodata < 0 || e->data < 0 || e->bss < 0) {
    if (!quieto) printf("TROCA: %s nao tem .text/.rodata/.data/.bss: nao e um kernel do SODOS\n", path);
    return 0;
  }
  if (e->symtab < 0 || e->strtab < 0) {
    if (!quieto) printf("TROCA: %s nao tem tabela de simbolos (foi feito strip?)\n", path);
    return 0;
  }
  e->st = (const Sym*)(e->b + e->sh[e->symtab].offset);
  e->nst = (int)(e->sh[e->symtab].size / sizeof(Sym));
  e->str = (const char*)(e->b + e->sh[e->strtab].offset);
  return 1;
}

static void elf_fecha(struct elf* e) {
  free(e->b);
  free(e->s);
  free(e->symmap);
  memset(e, 0, sizeof(*e));
}

static char sec_de(const struct elf* e, int shndx) {
  if (shndx == e->text) return 't';
  if (shndx == e->rodata) return 'r';
  if (shndx == e->data) return 'd';
  if (shndx == e->bss) return 'b';
  return 0;
}

/* ---------- chaves e tabela de hash ---------- */
struct hash {
  int* t;
  int cap;
  const struct sym* s;
};

static uint32_t fnv(const char* k) {
  uint32_t h = 2166136261u;
  while (*k) h = (h ^ (uint8_t)*k++) * 16777619u;
  return h;
}

static void h_init(struct hash* h, const struct sym* s, int n) {
  int i;
  h->cap = 16;
  while (h->cap < n * 2 + 16) h->cap <<= 1;
  h->t = (int*)xmalloc(sizeof(int) * (size_t)h->cap);
  for (i = 0; i < h->cap; i++) h->t[i] = -1;
  h->s = s;
}

static int h_get(const struct hash* h, const char* key) {
  uint32_t i = fnv(key) & (uint32_t)(h->cap - 1);
  while (h->t[i] >= 0) {
    if (!strcmp(h->s[h->t[i]].key, key)) return h->t[i];
    i = (i + 1) & (uint32_t)(h->cap - 1);
  }
  return -1;
}

static void h_put(struct hash* h, int idx) {
  uint32_t i = fnv(h->s[idx].key) & (uint32_t)(h->cap - 1);
  while (h->t[i] >= 0) i = (i + 1) & (uint32_t)(h->cap - 1);
  h->t[i] = idx;
}

/* Arquivos de origem dos simbolos static: "wine.c" -> quantas vezes ja apareceu. */
struct arqs {
  const char* nome[512];
  int vezes[512];
  int n;
};

static int arq_vez(struct arqs* a, const char* nome) {
  int i;
  for (i = 0; i < a->n; i++)
    if (!strcmp(a->nome[i], nome)) return a->vezes[i]++;
  if (a->n < 512) {
    a->nome[a->n] = nome;
    a->vezes[a->n] = 1;
    a->n++;
  }
  return 0;
}

static int cmp_addr(const void* x, const void* y, const struct sym* s) {
  const struct sym* a = &s[*(const int*)x];
  const struct sym* b = &s[*(const int*)y];
  if (a->addr != b->addr) return a->addr < b->addr ? -1 : 1;
  if (a->size != b->size) return a->size > b->size ? -1 : 1;   /* o maior primeiro */
  return *(const int*)x - *(const int*)y;
}
static const struct sym* ordem_s;
static int cmp_ordem(const void* x, const void* y) { return cmp_addr(x, y, ordem_s); }

/* Indices de s com kind em kinds, por endereco. */
static int* ordena(struct sym* s, int ns, const char* kinds, int* n_out) {
  int* v = (int*)xmalloc(sizeof(int) * (size_t)(ns + 1));
  int i, n = 0;
  for (i = 0; i < ns; i++)
    if (strchr(kinds, s[i].kind)) v[n++] = i;
  ordem_s = s;
  qsort(v, (size_t)n, sizeof(int), cmp_ordem);
  *n_out = n;
  return v;
}

/* Tamanho efetivo: funcao ou rotulo sem tamanho vai ate o proximo simbolo da
 * mesma secao (ou o fim dela); rotulo no mesmo endereco de um objeto e um
 * apelido dele. */
static void tamanhos(struct elf* e, const char* kinds) {
  struct sym* s = e->s;
  int n, i, j;
  int* v = ordena(s, e->ns, kinds, &n);
  for (i = 0; i < n; i++) {
    struct sym* a = &s[v[i]];
    int sh = a->sec == 't' ? e->text : a->sec == 'r' ? e->rodata : a->sec == 'd' ? e->data : e->bss;
    uint64_t fim = e->sh[sh].addr + e->sh[sh].size;
    if (a->size) continue;
    /* objeto de tamanho > 0 no mesmo endereco? (vem antes na ordem) */
    for (j = i - 1; j >= 0 && s[v[j]].addr == a->addr; j--)
      if (s[v[j]].size && s[v[j]].sec == a->sec) {
        a->alias = v[j];
        break;
      }
    if (a->alias >= 0) continue;
    for (j = i + 1; j < n; j++)
      if (s[v[j]].addr > a->addr && s[v[j]].sec == a->sec) {
        fim = s[v[j]].addr;
        break;
      }
    a->size = fim > a->addr ? fim - a->addr : 0;
  }
  free(v);
}

/* Simbolos do ELF: funcoes, rotulos, constantes e variaveis. */
static void elf_simbolos(struct elf* e) {
  struct arqs* aq = (struct arqs*)xmalloc(sizeof(struct arqs));
  struct hash h;
  const char* arq = "?";
  int vez = 0, i;
  char k[400];
  aq->n = 0;
  e->s = (struct sym*)xmalloc(sizeof(struct sym) * (size_t)(e->nst + 1));
  e->symmap = (int*)xmalloc(sizeof(int) * (size_t)(e->nst + 1));
  e->ns = 0;
  h_init(&h, e->s, e->nst);
  for (i = 0; i < e->nst; i++) {
    const Sym* y = &e->st[i];
    int tipo = y->info & 15, bind = y->info >> 4;
    const char* nm = e->str + y->name;
    char sc;
    struct sym* s;
    e->symmap[i] = -1;
    if (tipo == STT_FILE) {
      arq = nm;
      vez = arq_vez(aq, nm);
      continue;
    }
    if (tipo == STT_SECTION || !nm[0] || y->shndx == SHN_UNDEF || y->shndx >= SHN_LORES) continue;
    if (tipo != STT_FUNC && tipo != STT_OBJECT && tipo != STT_NOTYPE) continue;
    sc = sec_de(e, y->shndx);
    if (!sc) continue;
    if (bind == STB_LOCAL) snprintf(k, sizeof(k), "%s#%d:%s", arq, vez, nm);
    else snprintf(k, sizeof(k), "%s", nm);
    if (h_get(&h, k) >= 0) {        /* repetida: a ordem desempata, igual nos dois kernels */
      int n = 2;
      char k2[420];
      do snprintf(k2, sizeof(k2), "%s~%d", k, n++); while (h_get(&h, k2) >= 0);
      strcpy(k, k2);
    }
    s = &e->s[e->ns];
    memset(s, 0, sizeof(*s));
    s->key = sdup(k);
    s->addr = y->value;
    s->size = y->size;
    s->sec = sc;
    s->shndx = y->shndx;
    s->alias = -1;
    s->par = -1;
    s->igual = -1;
    if (sc == 't') s->kind = tipo == STT_FUNC ? 'F' : 'L';
    else if (sc == 'r') s->kind = 'R';
    else s->kind = 'D';
    e->symmap[i] = e->ns;
    h_put(&h, e->ns);
    e->ns++;
  }
  free(h.t);
  free(aq);
  tamanhos(e, "FL");
  tamanhos(e, "D");
}

/* ---------- kernel atual (o "velho") ---------- */
struct velho {
  struct sym* s;
  int ns;
  struct hash h;
  int geracao;
};

#define MAPA_MAGIC "TROCAMP1"
struct mapa_hdr {
  char magic[8];
  uint32_t n, nomes;
  uint32_t geracao, build;
  uint64_t text_lo, text_hi;
};
struct mapa_ent {
  uint64_t addr, size;
  uint32_t nome;
  char kind, sec;
  uint16_t pad;
};

static int velho_do_mapa(struct velho* v, const void* blob, uint32_t size) {
  const struct mapa_hdr* h = (const struct mapa_hdr*)blob;
  const struct mapa_ent* en;
  const char* nomes;
  uint32_t i;
  if (!blob || size < sizeof(*h) || memcmp(h->magic, MAPA_MAGIC, 8) ||
      sizeof(*h) + (uint64_t)h->n * sizeof(*en) + h->nomes > size) {
    printf("TROCA: o mapa de simbolos guardado pelo kernel esta estragado.\n"
           "Reinicie para trocar o kernel de novo.\n");
    return 0;
  }
  en = (const struct mapa_ent*)(h + 1);
  nomes = (const char*)(en + h->n);
  v->s = (struct sym*)xmalloc(sizeof(struct sym) * (size_t)(h->n + 1));
  v->ns = (int)h->n;
  for (i = 0; i < h->n; i++) {
    struct sym* s = &v->s[i];
    memset(s, 0, sizeof(*s));
    s->key = en[i].nome < h->nomes ? nomes + en[i].nome : "?";
    s->addr = en[i].addr;
    s->size = en[i].size;
    s->kind = en[i].kind;
    s->sec = en[i].sec;
    s->alias = -1;
    s->par = -1;
  }
  return 1;
}

/* O .text e o .rodata do arquivo sao os da memoria? (geracao 0: o kernel do boot) */
static int bate_com_memoria(const struct elf* e) {
  int k;
  int secs[2];
  secs[0] = e->text;
  secs[1] = e->rodata;
  for (k = 0; k < 2; k++) {
    const Shdr* s = &e->sh[secs[k]];
    if (s->addr < 0x01000000u || s->addr + s->size > 0x80000000u) return 0;
    if (memcmp(e->b + s->offset, (const void*)(uintptr_t)s->addr, (size_t)s->size)) return 0;
  }
  return 1;
}

/* Primeiro 4 KB do .text: descarta candidatos sem ler a comparacao inteira. */
static int candidato(const char* path, struct elf* e) {
  if (!elf_abre(e, path, 1)) {
    elf_fecha(e);
    return 0;
  }
  {
    const Shdr* s = &e->sh[e->text];
    size_t n = s->size < 4096 ? (size_t)s->size : 4096;
    if (s->addr < 0x01000000u || s->addr + s->size > 0x80000000u ||
        memcmp(e->b + s->offset, (const void*)(uintptr_t)s->addr, n) || !bate_com_memoria(e)) {
      elf_fecha(e);
      return 0;
    }
  }
  return 1;
}

static int procura_boot(struct elf* e, const char* novo) {
  static const char* const pastas[] = {"D:\\BOOT", "D:\\TEMP", "D:", "Z:"};
  char* lista = (char*)xmalloc(65536);
  char p[300];
  int d;
  if (candidato("D:\\BOOT\\KERNEL.ELF", e)) {
    free(lista);
    return 1;
  }
  for (d = 0; d < 4; d++) {
    char* q;
    if (k_dirlist(pastas[d], lista, 65535) < 0) continue;
    lista[65535] = 0;
    for (q = lista; *q;) {
      char* fim = strchr(q, '\n');
      if (fim) *fim = 0;
      if (termina(q, ".ELF") && q[strlen(q) - 1] != '\\') {
        snprintf(p, sizeof(p), "%s\\%s", pastas[d], q);
        if (!ieq(p, "D:\\BOOT\\KERNEL.ELF") && !(novo && ieq(p, novo)) && candidato(p, e)) {
          e->path = sdup(p);
          free(lista);
          return 1;
        }
      }
      if (!fim) break;
      q = fim + 1;
    }
  }
  free(lista);
  return 0;
}

static int velho_do_elf(struct velho* v, struct elf* e) {
  int i, n = 0;
  elf_simbolos(e);
  v->s = (struct sym*)xmalloc(sizeof(struct sym) * (size_t)(e->ns + 1));
  for (i = 0; i < e->ns; i++)
    if (e->s[i].kind == 'F' || e->s[i].kind == 'D') {
      v->s[n] = e->s[i];
      v->s[n].par = -1;
      n++;
    }
  v->ns = n;
  return 1;
}

/* ---------- decodificador: tamanho de uma instrucao x86-64 ----------
 * So para achar o fim da instrucao que tem um campo de relocacao (o -4..-8
 * de uma RIP-relativa). Cobre o que o gcc -O2 -mno-sse e o nasm do kernel
 * geram; o resto (EVEX, opcodes invalidos) devolve 0 = nao sei. */
static int ilen(const uint8_t* p, int max) {
  int i = 0, rex_w = 0, opsz = 0, adsz = 0, op, modrm, has_modrm = 0, imm = 0, grp3 = 0;
  for (;;) {
    if (i >= max || i >= 14) return 0;
    op = p[i];
    if (op == 0x66) opsz = 1;
    else if (op == 0x67) adsz = 1;
    else if (op != 0xF0 && op != 0xF2 && op != 0xF3 && op != 0x2E && op != 0x36 && op != 0x3E && op != 0x26 &&
             op != 0x64 && op != 0x65)
      break;
    i++;
  }
  if ((p[i] & 0xF0) == 0x40) {
    rex_w = (p[i] >> 3) & 1;
    i++;
  }
  if (i >= max) return 0;
  op = p[i++];
  if (op == 0xC4 || op == 0xC5) {           /* VEX */
    int map = 1;
    if (op == 0xC5) {
      if (i >= max) return 0;
      i++;
    } else {
      if (i + 1 >= max) return 0;
      map = p[i] & 0x1F;
      i += 2;
    }
    if (i >= max) return 0;
    op = p[i++];
    has_modrm = 1;
    if (map == 3) imm = 1;
    else if (map == 1 && ((op >= 0x70 && op <= 0x73) || op == 0xC2 || (op >= 0xC4 && op <= 0xC6))) imm = 1;
  } else if (op == 0x62) {
    return 0;                                /* EVEX */
  } else if (op == 0x0F) {
    if (i >= max) return 0;
    op = p[i++];
    if (op == 0x38 || op == 0x3A) {
      if (i >= max) return 0;
      i++;
      has_modrm = 1;
      imm = op == 0x3A ? 1 : 0;
    } else if (op <= 0x03 || op == 0x0D || (op >= 0x10 && op <= 0x23) || (op >= 0x28 && op <= 0x2F) ||
               (op >= 0x40 && op <= 0x6F) || (op >= 0x74 && op <= 0x76) || (op >= 0x78 && op <= 0x7F) ||
               (op >= 0x90 && op <= 0x9F) || op == 0xA3 || op == 0xA5 || (op >= 0xAB && op <= 0xAF && op != 0xAC) ||
               (op >= 0xB0 && op <= 0xB9) || (op >= 0xBB && op <= 0xC1) || op == 0xC3 || op == 0xC7 || op >= 0xD0) {
      has_modrm = 1;
    } else if (op == 0x0F || (op >= 0x70 && op <= 0x73) || op == 0xA4 || op == 0xAC || op == 0xBA || op == 0xC2 ||
               (op >= 0xC4 && op <= 0xC6)) {
      has_modrm = 1;
      imm = 1;
    } else if (op >= 0x80 && op <= 0x8F) {
      imm = 4;
    } else if (op == 0x05 || op == 0x06 || op == 0x07 || op == 0x08 || op == 0x09 || op == 0x0B || op == 0x0E ||
               (op >= 0x30 && op <= 0x37) || op == 0x77 || op == 0xA0 || op == 0xA1 || op == 0xA2 || op == 0xA8 ||
               op == 0xA9 || op == 0xAA || (op >= 0xC8 && op <= 0xCF)) {
      /* sem ModRM */
    } else {
      return 0;
    }
  } else if (op < 0x40) {
    int lo = op & 7;
    if (lo < 4) has_modrm = 1;
    else if (lo == 4) imm = 1;
    else if (lo == 5) imm = opsz ? 2 : 4;
    else return 0;                           /* push/pop de segmento, BCD: invalidos em 64 bits */
  } else if (op >= 0x50 && op <= 0x5F) {
  } else if (op == 0x63) {
    has_modrm = 1;
  } else if (op == 0x68) {
    imm = opsz ? 2 : 4;
  } else if (op == 0x69) {
    has_modrm = 1;
    imm = opsz ? 2 : 4;
  } else if (op == 0x6A) {
    imm = 1;
  } else if (op == 0x6B) {
    has_modrm = 1;
    imm = 1;
  } else if (op >= 0x6C && op <= 0x6F) {
  } else if (op >= 0x70 && op <= 0x7F) {
    imm = 1;
  } else if (op == 0x80 || op == 0x83) {
    has_modrm = 1;
    imm = 1;
  } else if (op == 0x81) {
    has_modrm = 1;
    imm = opsz ? 2 : 4;
  } else if (op >= 0x84 && op <= 0x8F) {
    has_modrm = 1;
  } else if ((op >= 0x90 && op <= 0x99) || (op >= 0x9B && op <= 0x9F)) {
  } else if (op >= 0xA0 && op <= 0xA3) {
    imm = adsz ? 4 : 8;                      /* moffs */
  } else if ((op >= 0xA4 && op <= 0xA7) || (op >= 0xAA && op <= 0xAF)) {
  } else if (op == 0xA8) {
    imm = 1;
  } else if (op == 0xA9) {
    imm = opsz ? 2 : 4;
  } else if (op >= 0xB0 && op <= 0xB7) {
    imm = 1;
  } else if (op >= 0xB8 && op <= 0xBF) {
    imm = rex_w ? 8 : (opsz ? 2 : 4);
  } else if (op == 0xC0 || op == 0xC1 || op == 0xC6) {
    has_modrm = 1;
    imm = 1;
  } else if (op == 0xC7) {
    has_modrm = 1;
    imm = opsz ? 2 : 4;
  } else if (op == 0xC2 || op == 0xCA) {
    imm = 2;
  } else if (op == 0xC8) {
    imm = 3;
  } else if (op == 0xC3 || op == 0xC9 || op == 0xCB || op == 0xCC || op == 0xCF) {
  } else if (op == 0xCD) {
    imm = 1;
  } else if ((op >= 0xD0 && op <= 0xD3) || (op >= 0xD8 && op <= 0xDF)) {
    has_modrm = 1;
  } else if (op == 0xD7) {
  } else if (op >= 0xE0 && op <= 0xE7) {
    imm = 1;
  } else if (op == 0xE8 || op == 0xE9) {
    imm = 4;
  } else if (op == 0xEB) {
    imm = 1;
  } else if ((op >= 0xEC && op <= 0xEF) || op == 0xF1 || op == 0xF4 || op == 0xF5 || (op >= 0xF8 && op <= 0xFD)) {
  } else if (op == 0xF6 || op == 0xF7) {
    has_modrm = 1;
    grp3 = op == 0xF6 ? 1 : 2;
  } else if (op == 0xFE || op == 0xFF) {
    has_modrm = 1;
  } else {
    return 0;
  }
  if (has_modrm) {
    int mod, rm;
    if (i >= max) return 0;
    modrm = p[i++];
    mod = modrm >> 6;
    rm = modrm & 7;
    if (grp3 && ((modrm >> 3) & 7) <= 1) imm = grp3 == 1 ? 1 : (opsz ? 2 : 4);
    if (mod != 3) {
      if (rm == 4) {
        if (i >= max) return 0;
        if (mod == 0 && (p[i] & 7) == 5) i += 4;
        i++;
      } else if (mod == 0 && rm == 5) {
        i += 4;
      }
      if (mod == 1) i += 1;
      else if (mod == 2) i += 4;
    }
  }
  i += imm;
  if (i > max || i > 15) return 0;
  return i;
}

/* ---------- a troca do kernel ---------- */
struct rel {
  uint64_t off;
  uint32_t tipo, sym;
  int64_t add;
};

struct ctx {
  struct elf* e;               /* kernel novo */
  struct velho* v;
  int forca, teste;
  uint64_t lo, hi, delta;      /* .text..fim do .rodata no ELF; bloco = lo + delta */
  uint8_t* bloco;
  void* bloco_raw;
  int* dob;                    /* objetos de dados do novo (tamanho > 0), por endereco */
  int ndob;
  int* tob;                    /* funcoes e rotulos do .text do novo, por endereco */
  int ntob;
  struct rel* rt_;             /* .rela.text por deslocamento */
  int nrt;
  uint8_t* fresca;             /* memoria das variaveis novas */
  uint64_t fresca_n;
  struct k_troca_copy* copias;
  int ncopias;
  int ambiguas, decodificadas, pelo_velho, pela_funcao, chutadas;
};

static int is_pc(uint32_t t) { return t == R_PC32 || t == R_PLT32 || t == R_PC16 || t == R_PC8 || t == R_PC64; }
static int largura(uint32_t t) {
  switch (t) {
    case R_64:
    case R_PC64: return 8;
    case R_PC32:
    case R_PLT32:
    case R_32:
    case R_32S: return 4;
    case R_16:
    case R_PC16: return 2;
    case R_8:
    case R_PC8: return 1;
  }
  return 0;
}

/* Grava v no campo; 0 se nao cabe. */
static int grava(uint8_t* at, uint32_t t, int64_t v) {
  switch (t) {
    case R_64:
    case R_PC64: memcpy(at, &v, 8); return 1;
    case R_PC32:
    case R_PLT32:
    case R_32S: {
      int32_t x = (int32_t)v;
      if ((int64_t)x != v) return 0;
      memcpy(at, &x, 4);
      return 1;
    }
    case R_32: {
      uint32_t x = (uint32_t)v;
      if (v < 0 || (uint64_t)v > 0xFFFFFFFFu) return 0;
      memcpy(at, &x, 4);
      return 1;
    }
    case R_16:
    case R_PC16: {
      int16_t x = (int16_t)v;
      if (t == R_16 ? (v < 0 || v > 0xFFFF) : ((int64_t)x != v)) return 0;
      memcpy(at, &x, 2);
      return 1;
    }
    case R_8:
    case R_PC8: {
      int8_t x = (int8_t)v;
      if (t == R_8 ? (v < 0 || v > 0xFF) : ((int64_t)x != v)) return 0;
      *at = (uint8_t)x;
      return 1;
    }
  }
  return 0;
}

static const char* onde(struct ctx* c, uint64_t P) {
  static char b[160];
  int lo = 0, hi = c->ntob - 1, m, best = -1;
  while (lo <= hi) {
    m = (lo + hi) / 2;
    if (c->e->s[c->tob[m]].addr <= P) {
      best = m;
      lo = m + 1;
    } else {
      hi = m - 1;
    }
  }
  if (best >= 0) snprintf(b, sizeof(b), "%s+0x%lx", c->e->s[c->tob[best]].key, (unsigned long)(P - c->e->s[c->tob[best]].addr));
  else snprintf(b, sizeof(b), "0x%lx", (unsigned long)P);
  return b;
}

/* Objeto de dados (indice em dob) com addr <= t: o que contem t ou o ultimo antes. */
static int dobj(struct ctx* c, uint64_t t) {
  int lo = 0, hi = c->ndob - 1, m, best = -1;
  while (lo <= hi) {
    m = (lo + hi) / 2;
    if (c->e->s[c->dob[m]].addr <= t) {
      best = m;
      lo = m + 1;
    } else {
      hi = m - 1;
    }
  }
  /* varios no mesmo endereco: o primeiro (o maior) */
  while (best > 0 && c->e->s[c->dob[best - 1]].addr == c->e->s[c->dob[best]].addr) best--;
  return best;
}

static struct sym* ob(struct ctx* c, int i) { return &c->e->s[c->dob[i]]; }

/* t e o comeco do objeto i, encostado no fim do anterior, e os dois nao sao
 * vizinhos na memoria: "fim do anterior" e "comeco deste" dao enderecos
 * diferentes. */
static int fronteira(struct ctx* c, int i, uint64_t t) {
  struct sym *a, *b;
  if (i <= 0) return 0;
  a = ob(c, i - 1);
  b = ob(c, i);
  return b->addr == t && a->addr + a->size == t && a->run != b->run;
}

/* Interpretacoes do endereco t (alvo no ELF novo): ate duas (comeco do objeto,
 * fim do anterior). Valores = endereco na memoria - k; objs = o objeto. */
static int cand(struct ctx* c, uint64_t T, int k, uint64_t* vals, int* objs) {
  uint64_t t = T + (uint64_t)k;
  int i = dobj(c, t), n = 0;
  struct sym* o;
  if (i < 0) return 0;
  o = ob(c, i);
  if (!o->ok) return 0;
  vals[n] = o->rt + (t - o->addr) - (uint64_t)k;
  objs[n++] = i;
  if (fronteira(c, i, t) && ob(c, i - 1)->ok) {
    struct sym* a = ob(c, i - 1);
    vals[n] = a->rt + a->size - (uint64_t)k;
    objs[n++] = i - 1;
  }
  return n;
}

/* Funcao (F) do novo que contem P, -1 se nenhuma. */
static int func_de(struct ctx* c, uint64_t P) {
  int lo = 0, hi = c->ntob - 1, m, best = -1;
  while (lo <= hi) {
    m = (lo + hi) / 2;
    if (c->e->s[c->tob[m]].addr <= P) {
      best = m;
      lo = m + 1;
    } else {
      hi = m - 1;
    }
  }
  for (; best >= 0; best--) {
    struct sym* s = &c->e->s[c->tob[best]];
    if (s->kind == 'F' && s->addr <= P && P < s->addr + s->size) return c->tob[best];
    if (s->addr + 0x100000u < P) break;
  }
  return -1;
}

static const uint8_t* texto_arq(struct ctx* c, uint64_t a) {
  const Shdr* t = &c->e->sh[c->e->text];
  return c->e->b + t->offset + (a - t->addr);
}

/* k = fim da instrucao - P, pelo decodificador, a partir do simbolo antes de
 * P. *lea = 1 se a instrucao e um lea (so calcula o endereco: pode ser o fim
 * de uma variavel); qualquer outra le ou grava a memoria do alvo. */
static int decode_k(struct ctx* c, uint64_t P, int* lea) {
  const Shdr* t = &c->e->sh[c->e->text];
  int lo = 0, hi = c->ntob - 1, m, best = -1;
  uint64_t a;
  *lea = 1;
  if (P < t->addr || P >= t->addr + t->size) return 0;
  while (lo <= hi) {
    m = (lo + hi) / 2;
    if (c->e->s[c->tob[m]].addr <= P) {
      best = m;
      lo = m + 1;
    } else {
      hi = m - 1;
    }
  }
  if (best < 0) return 0;
  a = c->e->s[c->tob[best]].addr;
  while (a <= P && P - a < 0x100000u) {
    uint64_t resto = t->addr + t->size - a;
    int len = ilen(texto_arq(c, a), resto < 15 ? (int)resto : 15);
    if (!len) return 0;
    if (P > a && P < a + (uint64_t)len) {
      const uint8_t* q = texto_arq(c, a);
      int k = (int)(a + (uint64_t)len - P), j = 0;
      while (j < len && (q[j] == 0x66 || q[j] == 0x67 || q[j] == 0xF0 || q[j] == 0xF2 || q[j] == 0xF3 ||
                         q[j] == 0x2E || q[j] == 0x36 || q[j] == 0x3E || q[j] == 0x26 || q[j] == 0x64 || q[j] == 0x65))
        j++;
      if (j < len && (q[j] & 0xF0) == 0x40) j++;
      *lea = j < len && q[j] == 0x8D;
      return k >= 4 && k <= 8 ? k : 0;
    }
    a += (uint64_t)len;
  }
  return 0;
}

static int rel_desde(struct ctx* c, uint64_t a) {
  int lo = 0, hi = c->nrt;
  while (lo < hi) {
    int m = (lo + hi) / 2;
    if (c->rt_[m].off < a) lo = m + 1;
    else hi = m;
  }
  return lo;
}

/* A funcao F do novo tem o mesmo codigo da antiga (fora os campos de
 * relocacao)? Entao a mesma instrucao do codigo antigo diz o alvo certo. */
static int funcao_igual(struct ctx* c, int fi) {
  struct sym* f = &c->e->s[fi];
  struct sym* o;
  const uint8_t* nb;
  const uint8_t* vb;
  uint64_t x = 0;
  int r;
  if (f->igual >= 0) return f->igual;
  f->igual = 0;
  if (f->par < 0) return 0;
  o = &c->v->s[f->par];
  if (o->kind != 'F' || o->size != f->size || o->addr < 0x01000000u || o->addr + o->size > 0x80000000u) return 0;
  nb = texto_arq(c, f->addr);
  vb = (const uint8_t*)(uintptr_t)o->addr;
  r = rel_desde(c, f->addr);
  while (x < f->size) {
    if (r < c->nrt && c->rt_[r].off < f->addr + f->size && c->rt_[r].off - f->addr <= x) {
      uint64_t fim = c->rt_[r].off - f->addr + (uint64_t)largura(c->rt_[r].tipo);
      if (fim > x) x = fim;
      r++;
      continue;
    }
    if (nb[x] != vb[x]) return 0;
    x++;
  }
  f->igual = 1;
  return 1;
}

static int campo_velho(struct ctx* c, uint64_t P, uint32_t tipo, uint64_t* val) {
  int fi = func_de(c, P);
  struct sym *f, *o;
  uint64_t op;
  if (fi < 0 || !funcao_igual(c, fi)) return 0;
  f = &c->e->s[fi];
  o = &c->v->s[f->par];
  op = o->addr + (P - f->addr);
  switch (tipo) {
    case R_PC32:
    case R_PLT32: *val = op + (uint64_t)(int64_t)*(const int32_t*)(uintptr_t)op; return 1;
    case R_32S: *val = (uint64_t)(int64_t)*(const int32_t*)(uintptr_t)op; return 1;
    case R_32: *val = (uint64_t)*(const uint32_t*)(uintptr_t)op; return 1;
    case R_64: *val = *(const uint64_t*)(uintptr_t)op; return 1;
  }
  return 0;
}

static int dobj(struct ctx* c, uint64_t t);
static struct sym* ob(struct ctx* c, int i);

static uint64_t rt_sym(struct ctx* c, int idx, int* ok) {
  struct sym* s = &c->e->s[idx];
  if (s->ok) {
    *ok = 1;
    return s->rt;
  }
  if (s->alias >= 0 && c->e->s[s->alias].ok) {
    *ok = 1;
    return c->e->s[s->alias].rt + (s->addr - c->e->s[s->alias].addr);
  }
  /* rotulo novo sem tamanho (um ponto): segue o objeto de antes dele */
  if (!s->size && c->dob) {
    int i = dobj(c, s->addr);
    if (i >= 0 && ob(c, i)->ok) {
      *ok = 1;
      return ob(c, i)->rt + (s->addr - ob(c, i)->addr);
    }
  }
  *ok = 0;
  return 0;
}

/* Objeto (indice em dob) que uma relocacao do novo usa sem duvida; -1. */
static int obj_certo(struct ctx* c, const struct rel* r) {
  const Sym* y;
  uint64_t T;
  int i;
  if ((int)r->sym >= c->e->nst) return -1;
  y = &c->e->st[r->sym];
  T = y->value + (uint64_t)r->add;
  if (sec_de(c->e, y->shndx) != 'd' && sec_de(c->e, y->shndx) != 'b') return -1;
  if ((y->info & 15) != STT_SECTION && c->e->symmap[r->sym] >= 0) {
    /* simbolo com nome: e ele (ou o objeto de que ele e apelido) */
    i = dobj(c, y->value);
    return (i >= 0 && y->value < ob(c, i)->addr + ob(c, i)->size) ? i : -1;
  }
  if (is_pc(r->tipo)) {
    int a = dobj(c, T + 4), b = dobj(c, T + 8);
    if (a < 0 || a != b || T + 8 >= ob(c, a)->addr + ob(c, a)->size || T + 4 < ob(c, a)->addr) return -1;
    return a;
  }
  i = dobj(c, T);
  if (i < 0 || T >= ob(c, i)->addr + ob(c, i)->size || fronteira(c, i, T)) return -1;
  return i;
}

/* Entre o fim do objeto a e o comeco do b: qual a funcao usa? -1 = nao sei. */
static int pela_funcao(struct ctx* c, uint64_t P, int oa, int obb) {
  int fi = func_de(c, P), r, viu_a = 0, viu_b = 0;
  struct sym* f;
  if (fi < 0) return -1;
  f = &c->e->s[fi];
  for (r = rel_desde(c, f->addr); r < c->nrt && c->rt_[r].off < f->addr + f->size; r++) {
    int o;
    if (c->rt_[r].off == P) continue;
    o = obj_certo(c, &c->rt_[r]);
    if (o == oa) viu_a = 1;
    if (o == obb) viu_b = 1;
  }
  if (viu_a && !viu_b) return 0;
  if (viu_b && !viu_a) return 1;
  return -1;
}

static void add_val(uint64_t* v, int* n, uint64_t x) {
  int i;
  for (i = 0; i < *n; i++)
    if (v[i] == x) return;
  if (*n < 8) v[(*n)++] = x;
}

/* Valor a gravar numa referencia a variavel. 0 ok, -1 problema. */
static int ref_dado(struct ctx* c, const Sym* y, uint32_t si, uint64_t T, uint32_t tipo, uint64_t P, uint64_t Pr,
                    int64_t* v) {
  int pc = is_pc(tipo), idx = c->e->symmap[si];
  uint64_t vals[8], cv[2], pick = 0;
  int objs[2], nv = 0, n, j, k, ok, achou = 0;
  if ((y->info & 15) != STT_SECTION && idx >= 0 && c->e->s[idx].kind == 'D') {
    uint64_t r = rt_sym(c, idx, &ok);
    if (!ok) {
      problema("%s usa %s, que nao tem lugar na memoria", onde(c, P), c->e->s[idx].key);
      return -1;
    }
    r += T - c->e->s[idx].addr;
    *v = pc ? (int64_t)(r - Pr) : (int64_t)r;
    return 0;
  }
  if (pc) {
    static const int ks[4] = {4, 5, 6, 8};
    for (j = 0; j < 4; j++) {
      n = cand(c, T, ks[j], cv, objs);
      for (k = 0; k < n; k++) add_val(vals, &nv, cv[k]);
    }
  } else {
    n = cand(c, T, 0, cv, objs);
    for (k = 0; k < n; k++) add_val(vals, &nv, cv[k]);
  }
  if (!nv) {
    problema("%s aponta para 0x%lx, fora das variaveis conhecidas", onde(c, P), (unsigned long)T);
    return -1;
  }
  if (nv == 1) {
    pick = vals[0];
    achou = 1;
  }
  if (!achou) {
    /* n = interpretacoes com o tamanho certo da instrucao: 1 resolve, 2 e
     * "fim de uma / comeco da outra" (objs[0] = comeco do objeto i,
     * objs[1] = fim do i-1), 0 = nao deu para decodificar */
    uint64_t ov;
    int lea = 1;
    c->ambiguas++;
    n = 0;
    if (pc) {
      k = decode_k(c, P, &lea);
      if (k) n = cand(c, T, k, cv, objs);
    } else {
      n = cand(c, T, 0, cv, objs);
    }
    if (n == 1) {
      pick = cv[0];
      achou = 1;
      c->decodificadas++;
    } else if (n == 2 && pc && !lea) {
      /* le ou grava a memoria: e o comeco da variavel seguinte, nunca o fim
       * da anterior (um mov/cmp nao acessa "um depois do fim") */
      pick = cv[0];
      achou = 1;
      c->decodificadas++;
    }
    if (!achou && campo_velho(c, P, tipo, &ov)) {
      /* a funcao nao mudou: a mesma instrucao no codigo antigo diz o alvo */
      const uint64_t* lista = n ? cv : vals;
      int m = n ? n : nv;
      for (j = 0; j < m && !achou; j++)
        if (lista[j] == ov) {
          pick = ov;
          achou = 1;
          c->pelo_velho++;
        }
    }
    if (!achou && n == 2 && objs[0] != objs[1]) {
      int q = pela_funcao(c, P, objs[1], objs[0]);
      if (q >= 0) {
        pick = q == 0 ? cv[1] : cv[0];
        achou = 1;
        c->pela_funcao++;
      }
    }
    if (!achou) {
      int oi = dobj(c, T + (pc ? 4 : 0));
      const char* perto = oi >= 0 ? ob(c, oi)->key : "?";
      if (c->forca) {
        pick = n ? cv[0] : vals[0];
        achou = 1;
        c->chutadas++;
        aviso("%s: referencia ambigua perto de %s, usei o comeco da variavel (/F)", onde(c, P), perto);
      } else {
        problema("%s: nao sei se aponta para o fim de uma variavel ou o comeco da seguinte "
                 "(perto de %s, que mudou de lugar)",
                 onde(c, P), perto);
        return -1;
      }
    }
  }
  *v = pc ? (int64_t)(pick + 0 - Pr) : (int64_t)pick;
  return 0;
}

static int cmp_rel(const void* x, const void* y) {
  const struct rel* a = (const struct rel*)x;
  const struct rel* b = (const struct rel*)y;
  return a->off < b->off ? -1 : a->off > b->off;
}

static struct rel* le_rels(struct elf* e, int sec, int* n) {
  const Rela* r;
  struct rel* v;
  int i, m;
  *n = 0;
  if (sec < 0) return 0;
  r = (const Rela*)(e->b + e->sh[sec].offset);
  m = (int)(e->sh[sec].size / sizeof(Rela));
  v = (struct rel*)xmalloc(sizeof(struct rel) * (size_t)(m + 1));
  for (i = 0; i < m; i++) {
    v[i].off = r[i].offset;
    v[i].tipo = (uint32_t)(r[i].info & 0xFFFFFFFFu);
    v[i].sym = (uint32_t)(r[i].info >> 32);
    v[i].add = r[i].addend;
  }
  qsort(v, (size_t)m, sizeof(struct rel), cmp_rel);
  *n = m;
  return v;
}

/* Aplica as relocacoes de uma secao. base(P) = onde o campo esta agora (no
 * bloco ou na memoria nova); 0 = pula (variavel que ja existe). */
static int relocar(struct ctx* c, const struct rel* rs, int n, int secao) {
  int i, ruins = 0;
  for (i = 0; i < n; i++) {
    const struct rel* r = &rs[i];
    const Sym* y;
    uint64_t P = r->off, Pr, T;
    uint8_t* at;
    int64_t v;
    char cls;
    if (r->tipo == R_NONE) continue;
    if (!largura(r->tipo)) {
      problema("relocacao de tipo %u em 0x%lx: o kernel nao deveria ter (ld -q de um kernel -fno-pic?)",
               (unsigned)r->tipo, (unsigned long)P);
      ruins++;
      continue;
    }
    if ((int)r->sym >= c->e->nst) {
      problema("relocacao com simbolo invalido em 0x%lx", (unsigned long)P);
      ruins++;
      continue;
    }
    if (secao == 'd') {                      /* .data: so as variaveis novas */
      int oi = dobj(c, P);
      struct sym* o;
      if (oi < 0) continue;
      o = ob(c, oi);
      if (!o->fresco || P >= o->addr + o->size) continue;
      Pr = o->rt + (P - o->addr);
      at = (uint8_t*)(uintptr_t)Pr;
    } else {
      Pr = P + c->delta;
      at = c->bloco + (P - c->lo);
    }
    y = &c->e->st[r->sym];
    T = y->value + (uint64_t)r->add;
    if (y->shndx == SHN_UNDEF) {
      problema("%s usa um simbolo indefinido", onde(c, P));
      ruins++;
      continue;
    }
    cls = y->shndx >= SHN_LORES ? 0 : sec_de(c->e, y->shndx);
    if (!cls && y->shndx < SHN_LORES) {
      /* outra secao carregada: dentro do bloco anda junto, fora fica onde esta */
      const Shdr* s = &c->e->sh[y->shndx];
      if (s->addr >= c->lo && s->addr + s->size <= c->hi) cls = 't';
    }
    if (cls == 't' || cls == 'r') {
      uint64_t tr = T + c->delta;
      v = is_pc(r->tipo) ? (int64_t)(tr - Pr) : (int64_t)tr;
    } else if (cls == 'd' || cls == 'b') {
      if (ref_dado(c, y, r->sym, T, r->tipo, P, Pr, &v) < 0) {
        ruins++;
        continue;
      }
    } else {
      v = is_pc(r->tipo) ? (int64_t)(T - Pr) : (int64_t)T;
    }
    if (!grava(at, r->tipo, v)) {
      problema("%s: o valor 0x%lx nao cabe na relocacao (tipo %u)", onde(c, P), (unsigned long)v, (unsigned)r->tipo);
      ruins++;
    }
  }
  return ruins;
}

/* O nome (sem o arquivo) tem ponto? (foo.part.0, foo.cold, x.3): clone do gcc
 * ou static de funcao. Funcoes assim nao levam jmp: a convencao de chamada
 * de um clone pode mudar de um build para outro. */
static int nome_com_ponto(const char* key) {
  const char* n = strrchr(key, ':');
  n = n ? n + 1 : key;
  return strchr(n, '.') != 0;
}

/* "arquivo#0:buf.3" -> "arquivo#0:buf" (static de funcao: o gcc numera). */
static int sem_sufixo(const char* key, char* out, int max) {
  int n = (int)strlen(key), i = n;
  while (i > 0 && key[i - 1] >= '0' && key[i - 1] <= '9') i--;
  if (i == n || i == 0 || key[i - 1] != '.' || i - 1 >= max) return 0;
  memcpy(out, key, (size_t)(i - 1));
  out[i - 1] = 0;
  return 1;
}

static int par_por_base(struct velho* v, const char* key, char kind) {
  char b[400], b2[400];
  int i, achou = -1;
  if (!sem_sufixo(key, b, (int)sizeof(b))) return -1;
  for (i = 0; i < v->ns; i++) {
    if (v->s[i].kind != kind || !sem_sufixo(v->s[i].key, b2, (int)sizeof(b2)) || strcmp(b, b2)) continue;
    if (achou >= 0) return -1;              /* mais de um: nao arrisca */
    achou = i;
  }
  return achou;
}

static int build_de(struct elf* e) {
  int i;
  for (i = 0; i < e->ns; i++)
    if (e->s[i].kind == 'F' && !strcmp(e->s[i].key, "sodos_build")) {
      const uint8_t* p = e->b + e->sh[e->text].offset + (e->s[i].addr - e->sh[e->text].addr);
      if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x1E && p[3] == 0xFA) p += 4;   /* endbr64 */
      if (p[0] == 0xB8) return (int)(p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[4] << 24);
    }
  return 0;
}

static void libera(struct ctx* c) {
  if (c->bloco_raw) kt_free(c->bloco_raw);
  if (c->fresca) kt_free(c->fresca);
  c->bloco_raw = 0;
  c->fresca = 0;
}

static void espera_ms(uint32_t ms) {
  uint32_t t0 = k_ms();
  while (k_ms() - t0 < ms) k_yield();
}

static int cmd_kernel(const char* novo_path, const char* atual_path, int teste, int forca) {
  static struct elf novo, boot;
  static struct velho vv;
  static struct ctx c;
  struct k_troca_info info;
  struct k_troca_req req;
  struct k_troca_patch* pt;
  struct mapa_hdr* mh;
  struct mapa_ent* me;
  void* mapa;
  char* nomes;
  uint64_t mapa_n, nomes_n, txt_n;
  int i, np = 0, ruins = 0, novas_f = 0, sairam_f = 0, curtas = 0, mudou_tam = 0, frescas = 0, igual_mem = 0;
  int nmapa, tent, rc, build_novo;
  memset(&c, 0, sizeof(c));
  memset(&info, 0, sizeof(info));
  if (kt_info(&info) != 0 || info.abi != TROCA_ABI) {
    printf("TROCA: o kernel tem outra versao da troca (abi %u, este TROCA e %u).\n", info.abi, TROCA_ABI);
    return 1;
  }
  /* kernel novo */
  if (!elf_abre(&novo, novo_path, 0)) return 1;
  if (novo.rtext < 0) {
    printf("TROCA: %s nao tem as relocacoes (.rela.text).\n"
           "O kernel precisa ser ligado com ld -q (src13/Makefile, LDFLAGS).\n",
           novo_path);
    return 1;
  }
  build_novo = 0;
  /* kernel rodando */
  if (info.geracao == 0) {
    if (atual_path) {
      if (!elf_abre(&boot, atual_path, 0)) return 1;
      if (!bate_com_memoria(&boot)) {
        printf("TROCA: %s nao e o kernel que esta rodando (o .text na memoria e outro).\n", atual_path);
        return 1;
      }
    } else if (!procura_boot(&boot, novo_path)) {
      printf("TROCA: nao achei o ELF do kernel que esta rodando (build %u).\n"
             "Procurei D:\\BOOT\\KERNEL.ELF e os .ELF de D:\\BOOT, D:\\TEMP, D:\\ e Z:\\.\n"
             "Passe o caminho: TROCA KERNEL %s <ELF do kernel atual>\n",
             info.build, novo_path);
      return 1;
    }
    printf("Kernel rodando: build %u, %s\n", info.build, boot.path);
    velho_do_elf(&vv, &boot);
    if (!strcmp(boot.path, novo_path) || ieq(boot.path, novo_path)) igual_mem = 1;
  } else {
    printf("Kernel rodando: build %u (boot: build %u, %u troca%s a quente; ultima de %s)\n", info.build,
           info.build_boot, info.geracao, info.geracao == 1 ? "" : "s", info.origem);
    if (!velho_do_mapa(&vv, info.mapa, info.mapa_size)) return 1;
  }
  h_init(&vv.h, vv.s, vv.ns);
  for (i = 0; i < vv.ns; i++) h_put(&vv.h, i);
  if (igual_mem) {
    printf("TROCA: %s ja e o kernel que esta rodando.\n", novo_path);
    return 1;
  }
  elf_simbolos(&novo);
  build_novo = build_de(&novo);
  printf("Kernel novo:    build %d, %s\n", build_novo, novo_path);
  c.e = &novo;
  c.v = &vv;
  c.forca = forca;
  c.teste = teste;
  c.lo = novo.sh[novo.text].addr;
  c.hi = novo.sh[novo.rodata].addr + novo.sh[novo.rodata].size;
  if (c.hi <= c.lo || c.hi - c.lo > 0x08000000u) {
    printf("TROCA: .text/.rodata do ELF novo fora do esperado\n");
    return 1;
  }
  /* pares (mesma chave) */
  for (i = 0; i < novo.ns; i++) {
    struct sym* s = &novo.s[i];
    int o = h_get(&vv.h, s->key);
    if (o >= 0 && vv.s[o].kind != (s->kind == 'L' ? 'F' : s->kind)) o = -1;
    if (o < 0 && (s->kind == 'D' || s->kind == 'F')) o = par_por_base(&vv, s->key, s->kind);
    s->par = o;
    if (o >= 0) vv.s[o].par = i;
  }
  /* variaveis: as de mesmo tamanho ficam onde estao; as outras ganham memoria */
  printf("Variaveis...\n");
  {
    uint64_t tot = 0;
    for (i = 0; i < novo.ns; i++) {
      struct sym* s = &novo.s[i];
      if (s->kind != 'D') continue;
      /* mesma chave e mesmo tamanho: fica onde esta (inclui rotulos como o
       * __bss_start, que vale o endereco antigo e nao o do objeto ao lado) */
      if (s->par >= 0 && vv.s[s->par].size == s->size) {
        s->rt = vv.s[s->par].addr;
        s->ok = 1;
        continue;
      }
      if (s->alias >= 0) continue;           /* apelido sem par: segue o objeto */
      if (s->par >= 0) {
        struct sym* o = &vv.s[s->par];
        if (!forca) {
          problema("%s mudou de tamanho (%lu -> %lu bytes)", s->key, (unsigned long)o->size,
                   (unsigned long)s->size);
          mudou_tam++;
          continue;
        }
        aviso("%s mudou de tamanho (%lu -> %lu): memoria nova, com o que cabia da antiga (/F)", s->key,
              (unsigned long)o->size, (unsigned long)s->size);
        mudou_tam++;
      }
      if (!s->size) continue;
      s->fresco = 1;
      tot = (tot + 63u) & ~(uint64_t)63u;
      if (s->size >= 4096 && !(s->addr & 4095u)) tot = (tot + 4095u) & ~(uint64_t)4095u;
      tot += s->size;
      frescas++;
    }
    if (tot) {
      uint64_t pos = 0;
      if (tot > 0x10000000u) {
        printf("TROCA: variaveis novas demais (%lu bytes)\n", (unsigned long)tot);
        return 1;
      }
      c.fresca = (uint8_t*)kt_alloc((unsigned)tot);
      if (!c.fresca) {
        printf("TROCA: o kernel nao deu %lu bytes para as variaveis novas\n", (unsigned long)tot);
        return 1;
      }
      c.fresca_n = tot;
      for (i = 0; i < novo.ns; i++) {
        struct sym* s = &novo.s[i];
        if (!s->fresco) continue;
        pos = (pos + 63u) & ~(uint64_t)63u;
        if (s->size >= 4096 && !(s->addr & 4095u)) pos = (pos + 4095u) & ~(uint64_t)4095u;
        s->rt = (uint64_t)(uintptr_t)c.fresca + pos;
        s->ok = 1;
        if (s->sec == 'd') {
          const Shdr* d = &novo.sh[novo.data];
          memcpy(c.fresca + pos, novo.b + d->offset + (s->addr - d->addr), (size_t)s->size);
        }
        if (s->par >= 0 && forca) {          /* /F: o que cabia da antiga, com as CPUs paradas */
          struct sym* o = &vv.s[s->par];
          if (!c.copias) c.copias = (struct k_troca_copy*)xmalloc(sizeof(struct k_troca_copy) * 4096);
          if (c.ncopias < 4096) {
            c.copias[c.ncopias].dst = s->rt;
            c.copias[c.ncopias].src = o->addr;
            c.copias[c.ncopias].len = o->size < s->size ? o->size : s->size;
            c.ncopias++;
          }
        }
        pos += s->size;
      }
    }
  }
  /* trechos em que o mapa e linear */
  c.dob = ordena(novo.s, novo.ns, "D", &c.ndob);
  {
    int n = 0, run = 0;
    for (i = 0; i < c.ndob; i++)
      if (novo.s[c.dob[i]].size) c.dob[n++] = c.dob[i];
    c.ndob = n;
    for (i = 0; i < c.ndob; i++) {
      struct sym* s = ob(&c, i);
      if (i > 0) {
        struct sym* a = ob(&c, i - 1);
        if (!a->ok || !s->ok || s->rt - a->rt != s->addr - a->addr) run++;
      }
      s->run = run;
    }
  }
  c.tob = ordena(novo.s, novo.ns, "FL", &c.ntob);
  c.rt_ = le_rels(&novo, novo.rtext, &c.nrt);
  if (nprob) {
    printf("Kernel NAO trocado: variaveis mudaram de tamanho (%d). Reinicie, ou TROCA ... /F\n"
           "para copiar o que cabe (por sua conta: codigo antigo ainda rodando usa a variavel velha).\n",
           mudou_tam);
    libera(&c);
    return 1;
  }
  /* bloco de codigo: mesmo alinhamento de pagina do ELF */
  txt_n = c.hi - c.lo;
  c.bloco_raw = kt_alloc((unsigned)(txt_n + 4096));
  if (!c.bloco_raw) {
    printf("TROCA: o kernel nao deu %lu KB abaixo de 2 GB para o codigo novo\n", (unsigned long)(txt_n / 1024));
    libera(&c);
    return 1;
  }
  c.bloco = (uint8_t*)c.bloco_raw + (c.lo & 4095u);
  c.delta = (uint64_t)(uintptr_t)c.bloco - c.lo;
  for (i = 0; i < novo.eh->shnum; i++) {
    const Shdr* s = &novo.sh[i];
    if (!(s->flags & 2) || s->type == SHT_NOBITS || s->addr < c.lo || s->addr + s->size > c.hi) continue;
    memcpy(c.bloco + (s->addr - c.lo), novo.b + s->offset, (size_t)s->size);
  }
  printf("Codigo novo: %lu KB em 0x%lx\nRelocacoes...\n", (unsigned long)(txt_n / 1024),
         (unsigned long)(uintptr_t)c.bloco);
  ruins += relocar(&c, c.rt_, c.nrt, 't');
  {
    int n;
    struct rel* r = le_rels(&novo, novo.rrodata, &n);
    ruins += relocar(&c, r, n, 'r');
    free(r);
    r = le_rels(&novo, novo.rdata, &n);
    ruins += relocar(&c, r, n, 'd');
    free(r);
  }
  /* jmps: cada funcao do kernel atual que tem a mesma no novo */
  pt = (struct k_troca_patch*)xmalloc(sizeof(struct k_troca_patch) * (size_t)(vv.ns + 1));
  {
    int* ord;
    int n;
    ordem_s = vv.s;
    ord = ordena(vv.s, vv.ns, "F", &n);
    for (i = 0; i < n; i++) {
      struct sym* o = &vv.s[ord[i]];
      struct sym* s;
      if (nome_com_ponto(o->key)) continue;
      if (o->par < 0) {
        sairam_f++;
        continue;
      }
      s = &novo.s[o->par];
      if (s->kind != 'F') continue;
      if (o->size < 5 || (np && o->addr < pt[np - 1].at + 5)) {
        curtas++;
        continue;
      }
      pt[np].at = o->addr;
      pt[np].to = s->addr + c.delta;
      np++;
    }
    free(ord);
    for (i = 0; i < novo.ns; i++)
      if (novo.s[i].kind == 'F' && novo.s[i].par < 0) novas_f++;
  }
  /* mapa para a proxima troca: funcoes no bloco, variaveis onde ficaram */
  nmapa = 0;
  nomes_n = 0;
  for (i = 0; i < novo.ns; i++) {
    struct sym* s = &novo.s[i];
    int ok = 0;
    if (s->kind == 'F') ok = 1;
    else if (s->kind == 'D') rt_sym(&c, i, &ok);
    if (!ok) continue;
    nmapa++;
    nomes_n += strlen(s->key) + 1;
  }
  mapa_n = sizeof(struct mapa_hdr) + (uint64_t)nmapa * sizeof(struct mapa_ent) + nomes_n;
  mapa = kt_alloc((unsigned)mapa_n);
  if (!mapa) {
    printf("TROCA: sem memoria do kernel para o mapa de simbolos\n");
    libera(&c);
    return 1;
  }
  mh = (struct mapa_hdr*)mapa;
  me = (struct mapa_ent*)(mh + 1);
  nomes = (char*)(me + nmapa);
  memcpy(mh->magic, MAPA_MAGIC, 8);
  mh->n = (uint32_t)nmapa;
  mh->nomes = (uint32_t)nomes_n;
  mh->geracao = info.geracao + 1;
  mh->build = (uint32_t)build_novo;
  mh->text_lo = (uint64_t)(uintptr_t)c.bloco;
  mh->text_hi = (uint64_t)(uintptr_t)c.bloco + txt_n;
  {
    uint32_t pos = 0;
    int k = 0;
    for (i = 0; i < novo.ns; i++) {
      struct sym* s = &novo.s[i];
      int ok = 0;
      uint64_t a = 0;
      if (s->kind == 'F') {
        ok = 1;
        a = s->addr + c.delta;
      } else if (s->kind == 'D') {
        a = rt_sym(&c, i, &ok);
      }
      if (!ok) continue;
      me[k].addr = a;
      me[k].size = s->size;
      me[k].nome = pos;
      me[k].kind = s->kind;
      me[k].sec = s->sec;
      me[k].pad = 0;
      strcpy(nomes + pos, s->key);
      pos += (uint32_t)strlen(s->key) + 1;
      k++;
    }
  }
  printf("  %d funcoes trocadas, %d novas, %d que sairam (ficam onde estao), %d curtas demais para o jmp\n", np,
         novas_f, sairam_f, curtas);
  printf("  %d variaveis novas", frescas);
  if (c.ambiguas)
    printf(", %d referencias perto de variaveis que mudaram de lugar (%d pelo tamanho da instrucao, %d pelo codigo antigo, %d pela funcao%s)",
           c.ambiguas, c.decodificadas, c.pelo_velho, c.pela_funcao, c.chutadas ? ", algumas chutadas por /F" : "");
  printf("\n");
  if (ruins || nprob) {
    printf("Kernel NAO trocado: %d problema%s acima. Nada mudou.\n", nprob, nprob == 1 ? "" : "s");
    kt_free(mapa);
    libera(&c);
    return 1;
  }
  if (teste) {
    printf("Teste (/T): tudo pronto, nada foi trocado.\n");
    kt_free(mapa);
    libera(&c);
    return 0;
  }
  memset(&req, 0, sizeof(req));
  req.abi = TROCA_ABI;
  req.npatch = (uint32_t)np;
  req.patch = pt;
  req.ncopy = (uint32_t)c.ncopias;
  req.copy = c.copias;
  req.build = (uint32_t)build_novo;
  req.text_lo = (uint64_t)(uintptr_t)c.bloco;
  req.text_hi = (uint64_t)(uintptr_t)c.bloco + txt_n;
  req.mapa = mapa;
  req.mapa_size = (uint32_t)mapa_n;
  for (i = 0; i < novo.ns; i++)
    if (novo.s[i].kind == 'F' && !strcmp(novo.s[i].key, "troca_depois")) req.depois = novo.s[i].addr + c.delta;
  snprintf(req.origem, sizeof(req.origem), "%s", novo_path);
  rc = TROCA_EOCUPADO;
  for (tent = 1; tent <= 50; tent++) {
    rc = kt_apply(&req);
    if (rc != TROCA_EOCUPADO && rc != TROCA_ETEMPO) break;
    espera_ms(20);
  }
  if (rc != 0) {
    if (rc == TROCA_EOCUPADO)
      printf("Kernel NAO trocado: em %d tentativas sempre havia %s no comeco de uma funcao (rip 0x%lx).\n",
             tent - 1, req.cpu >= 0 ? "uma CPU" : "uma thread", (unsigned long)req.rip);
    else if (rc == TROCA_ETEMPO)
      printf("Kernel NAO trocado: a CPU %d nao parou em 50 ms (%d tentativas).\n", req.cpu, tent - 1);
    else
      printf("Kernel NAO trocado: o kernel recusou o pedido (%d).\n", rc);
    kt_free(mapa);
    libera(&c);
    return 1;
  }
  printf("Kernel trocado: build %u -> %d, sem reiniciar (CPUs paradas %u us%s).\n", info.build, build_novo, req.us,
         tent > 1 ? ", depois de esperar uma thread sair do caminho" : "");
  printf("Para o proximo boot usar este kernel, ele precisa estar em D:\\BOOT\\KERNEL.ELF.\n");
  /* o bloco, as variaveis e o mapa agora sao do kernel */
  (void)mudou_tam;
  return 0;
}

/* ---------- DLLs e estado ---------- */
static int cmd_dll(const char* nome) {
  char msg[1024];
  int r;
  msg[0] = 0;
  r = kt_dll(nome, msg, (int)sizeof(msg));
  printf("%s", msg);
  return r < 0 ? 1 : 0;
}

/* TROCA WINE caminho: o kernel reconhece o WINE.DLL pelo nome do arquivo.
 * Com outro nome (D:\TEMP\WINEPOP.DLL) vai uma copia para Z:\TROCA\WINE.DLL. */
static int cmd_wine(const char* path) {
  const char* b;
  const char* q;
  uint8_t* buf;
  uint32_t n = 0;
  static const char dst[] = "Z:\\TROCA\\WINE.DLL";
  if (!path) return cmd_dll("WINE.DLL");
  for (b = q = path; *q; q++)
    if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
  if (ieq(b, "WINE.DLL")) return cmd_dll(path);
  buf = ler(path, &n);
  if (!buf) {
    printf("TROCA: nao consegui ler %s\n", path);
    return 1;
  }
  k_mkdir("Z:\\TROCA");
  if (k_fsave(dst, (const char*)buf, (int)n) < 0) {
    printf("TROCA: nao consegui gravar %s\n", dst);
    free(buf);
    return 1;
  }
  free(buf);
  printf("%s copiado para %s (o kernel reconhece o WINE.DLL pelo nome).\n", path, dst);
  return cmd_dll(dst);
}

static int cmd_estado(void) {
  char* b = (char*)xmalloc(8192);
  kt_estado(b, 8192);
  printf("%s", b);
  free(b);
  return 0;
}

static void uso(void) {
  printf("TROCA - troca o kernel e os DLLs do sistema sem reiniciar\n\n"
         "  TROCA                            mostra o que esta rodando\n"
         "  TROCA KERNEL [novo.elf] [atual.elf] [/T] [/F]\n"
         "      novo.elf   kernel ligado com ld -q (padrao D:\\BOOT\\KERNEL.ELF)\n"
         "      atual.elf  ELF do kernel do boot (so na primeira troca; o TROCA procura)\n"
         "      /T         so testa: monta tudo e diz o que faria\n"
         "      /F         aceita variaveis que mudaram de tamanho (por sua conta)\n"
         "  TROCA WINE [caminho\\WINE.DLL]     os proximos EXEs do Windows usam o WINE.DLL\n"
         "                                   novo; os abertos ficam no antigo ate fechar\n"
         "  TROCA DLL FAT32.DLL | LINKEXT.DLL\n"
         "  TROCA arquivo.ELF | arquivo.DLL  o mesmo, pelo tipo do arquivo\n\n"
         "Detalhes: \\SRC\\DOCS\\TROCA.MD (src13/docs/TROCA.md)\n");
}

int main(int argc, char** argv) {
  const char* arqs[3] = {0, 0, 0};
  int na = 0, teste = 0, forca = 0, i;
  for (i = 1; i < argc; i++) {
    if (ieq(argv[i], "/T") || ieq(argv[i], "-t")) teste = 1;
    else if (ieq(argv[i], "/F") || ieq(argv[i], "-f")) forca = 1;
    else if (ieq(argv[i], "/?") || ieq(argv[i], "-h") || ieq(argv[i], "--help")) {
      uso();
      return 0;
    } else if (na < 3) arqs[na++] = argv[i];
  }
  if (!api()) return 1;
  if (!na) return cmd_estado();
  if (ieq(arqs[0], "KERNEL")) return cmd_kernel(na > 1 ? arqs[1] : "D:\\BOOT\\KERNEL.ELF", na > 2 ? arqs[2] : 0, teste, forca);
  if (ieq(arqs[0], "WINE")) return cmd_wine(na > 1 ? arqs[1] : 0);
  if (ieq(arqs[0], "DLL")) {
    if (na < 2) {
      uso();
      return 1;
    }
    return cmd_dll(arqs[1]);
  }
  if (termina(arqs[0], ".ELF")) return cmd_kernel(arqs[0], na > 1 ? arqs[1] : 0, teste, forca);
  if (termina(arqs[0], ".DLL")) return cmd_dll(arqs[0]);
  uso();
  return 1;
}
