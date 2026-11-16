/* sodos_mmap.c - mmap anonimo sobre o heap do kernel.
 *
 * Enquanto o kernel nao tem paginas sob demanda, o mapa anonimo e um bloco
 * do heap do processo, zerado e alinhado (4 KiB a 2 MiB, ver map_align; o coletor de lixo do
 * SpiderMonkey exige alinhamento e desmapeia cabeca/cauda para alinhar, por
 * isso munmap aceita sub-faixas). Sem MAP_FIXED de verdade, arquivo so como
 * copia (leitura/MAP_PRIVATE) e sem protecao real (mprotect/madvise sao no-op): o Gecko precisa disto
 * desligado (--disable-jemalloc) por enquanto.
 */
#include <so.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "sys/mman.h"
#include "sodos.h"

#ifdef __x86_64__
static void dbg_ch(unsigned char c) {
  __asm__ volatile("outb %0, %1" :: "a"(c), "d"((unsigned short)0x3F8));
}
static void dbg_txt(const char* s) {
  while (*s) dbg_ch((unsigned char)*s++);
}
static void dbg_hex(unsigned v) {
  int i;
  dbg_txt("0x");
  for (i = 0; i < 8; i++) {
    int d = (int)((v >> (28 - 4 * i)) & 0xFu);
    dbg_ch((unsigned char)(d < 10 ? '0' + d : 'a' + d - 10));
  }
}
#else
static void dbg_txt(const char* s) { (void)s; }
static void dbg_hex(unsigned v) { (void)v; }
#endif

/* Alinhamento das faixas devolvidas: >= qualquer granularidade que o GC use
 * (ChunkSize do SpiderMonkey e 256 KiB). */
#define MM_ALIGN 0x200000u

struct sodos_base {
  void* p;    /* bloco original devolvido pelo k_malloc / k_hmalloc */
  size_t live; /* bytes ainda visiveis deste bloco */
  int high;   /* 1 = acima de 4 GB (k_hmalloc), solta com k_hfree */
  /* Reserva grande nao zerada no mmap: tudo abaixo de zhi ja foi zerado
   * (no commit). Ver sodos_commit_zero. */
  char* zhi;
  char* end;
  struct sodos_base* next;
};

struct sodos_map {
  char* addr;
  size_t len;
  struct sodos_base* owner;
  struct sodos_map* next;
};

/* sodos_shm.c */
int sodos_shm_is(int fd);
void* sodos_shm_map(int fd, size_t len, off_t off);
int sodos_shm_unmap(void* addr, size_t len);

/* low = 1: mapa que o kernel vai ler/gravar (copia de arquivo) fica abaixo
 * de 4 GB, como antes (drivers de disco podem nao aceitar buffer alto). */
static void* mmap_impl(void* addr, size_t len, int prot, int flags, int fd, off_t off, int low);

static struct sodos_map* maps;
static struct sodos_base* bases;

/* MAP_SHARED gravavel de arquivo. O SODOS tem um processo so: o que precisa
 * ser "compartilhado" e cada mapa da mesma faixa do mesmo arquivo dentro do
 * processo, entao a faixa vira memoria anonima (lida do arquivo na primeira
 * vez) achada de novo por (inode, offset). Quem usa e o SQLite em modo WAL:
 * o arquivo -shm guarda o indice do WAL e e reconstruido quando falta, entao
 * nao e preciso gravar a memoria de volta no arquivo. Antes o mmap dava
 * ENOSYS, que o SQLite devolve como SQLITE_IOERR_SHMMAP ("disk I/O error")
 * em todo banco WAL (IndexedDB usa WAL). */
struct sodos_fmap {
  unsigned long ino;
  off_t off;
  size_t len;
  char* addr;
  int refs;
  struct sodos_fmap* next;
};
static struct sodos_fmap* fmaps;

static void* fmap_get(int fd, size_t len, off_t off) {
  struct stat st;
  struct sodos_fmap* f;
  char* p;
  size_t got = 0;
  if (fstat(fd, &st) != 0) return MAP_FAILED;
  for (f = fmaps; f; f = f->next) {
    if (f->ino == (unsigned long)st.st_ino && f->off == off && f->len >= len) {
      f->refs++;
      return f->addr;
    }
  }
  f = (struct sodos_fmap*)malloc(sizeof(*f));
  if (!f) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  p = (char*)mmap_impl(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, 1);
  if (p == (char*)MAP_FAILED) {
    free(f);
    return MAP_FAILED;
  }
  while (got < len) {
    ssize_t r = pread(fd, p + got, len - got, (off_t)(off + got));
    if (r <= 0) break;
    got += (size_t)r;
  }
  f->ino = (unsigned long)st.st_ino;
  f->off = off;
  f->len = len;
  f->addr = p;
  f->refs = 1;
  f->next = fmaps;
  fmaps = f;
  return p;
}

/* 1 se addr era um mapa de fmap_get (ja solto). */
static int fmap_put(char* a) {
  struct sodos_fmap** pp;
  for (pp = &fmaps; *pp; pp = &(*pp)->next) {
    struct sodos_fmap* f = *pp;
    if (f->addr != a) continue;
    if (--f->refs == 0) {
      size_t l = f->len;
      *pp = f->next; /* fora da lista: o munmap abaixo solta o bloco anonimo */
      free(f);
      munmap(a, l);
    }
    return 1;
  }
  return 0;
}

/* Reservas gigantes (o JIT reserva ~2 GB de espaco de codigo) nao sao
 * zeradas: ficam PROT_NONE e as paginas so sao "commitadas" depois. Todo o
 * resto (chunks do GC, nursery) precisa vir zerado. */
#define MM_NO_ZERO_MIN 0x20000000u

/* Kernel novo: k_malloc_aligned devolve o bloco ja alinhado, sem a folga de
 * MM_ALIGN que fazia um chunk de 1 MB do GC custar 3 MB (OOM do Nursery no
 * google.com). Procurado em tempo de execucao para rodar em kernel antigo. */
static void* (*kmal)(unsigned, unsigned);
static int kmal_done;

/* Kernel novo: RAM acima de 4 GB (o port e LP64). O teto e uma fatia da RAM
 * do PC (padrao 40%, SET HIMEM=pct). Sem isso a memoria do wasm (o
 * canvaskit do Flutter reserva 4 GB de uma vez) e os chunks do GC brigavam
 * pelos ~3 GB abaixo de 4 GB: "Uncaught out of memory" no app do C8X. */
static void* (*khm)(unsigned long, unsigned long);
static void (*khf)(void*);

static void kmal_resolve(void) {
  if (kmal_done) return;
  kmal = (void* (*)(unsigned, unsigned))k_dl_sym(0, "k_malloc_aligned", 0);
  if (sizeof(void*) == 8) { /* x32 nao enxerga acima de 4 GB */
    khm = (void* (*)(unsigned long, unsigned long))k_dl_sym(0, "k_hmalloc", 0);
    khf = (void (*)(void*))k_dl_sym(0, "k_hfree", 0);
  }
  if (!khf) khm = 0;
  kmal_done = 1;
}


/* Alinhamento natural: a maior potencia de 2 <= len, entre 4 KiB e MM_ALIGN.
 * O GC pede chunks de ChunkSize alinhados a ChunkSize; reservas maiores ficam
 * a 2 MiB. Se algum chamador quiser mais, o MapAlignedPages remapeia com
 * folga e desmapeia cabeca/cauda (munmap aceita sub-faixas). */
static uint32_t map_align(size_t len) {
  uint32_t a = 0x1000u;
  while (a < MM_ALIGN && (size_t)a * 2u <= len) a *= 2u;
  return a;
}

static struct sodos_map* find_map(char* a, size_t len) {
  struct sodos_map* e;
  for (e = maps; e; e = e->next)
    if (a >= e->addr && a + len <= e->addr + e->len) return e;
  return 0;
}

/* Commit de paginas de uma reserva grande que nao foi zerada no mmap (o JIT
 * e a memoria do wasm comitam com mmap MAP_FIXED ou mprotect). Zera so o
 * trecho ainda nao zerado: de zhi ate o fim do commit. O buraco entre zhi e
 * o commit nunca foi comitado, entao zera-lo nao apaga nada vivo; abaixo de
 * zhi as paginas ja estao zeradas ou em uso (trocar a protecao do codigo do
 * JIT nao pode apagar o codigo). */
static void sodos_commit_zero(struct sodos_map* e, char* a, size_t len) {
  struct sodos_base* b = e->owner;
  char* top = a + len;
  if (!b->zhi) return;
  if (top > b->end) top = b->end;
  if (top > b->zhi) {
    memset(b->zhi, 0, (size_t)(top - b->zhi));
    b->zhi = top;
  }
}

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
  return mmap_impl(addr, len, prot, flags, fd, off, 0);
}

static void* mmap_impl(void* addr, size_t len, int prot, int flags, int fd, off_t off, int low) {
  void* raw;
  uintptr_t al;
  struct sodos_base* b;
  struct sodos_map* e;

  (void)prot;
  (void)off;
  (void)fd;

  if (len == 0) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  if (flags & MAP_FIXED) {
    /* Commit/decommit de uma reserva ja feita. O SODOS nao tem protecao por
     * pagina, mas o contrato de mmap anonimo exige paginas zeradas: zeramos
     * quando a pagina e (re)commitada com acesso. */
    if (addr && (e = find_map((char*)addr, len)) != 0) {
      if ((flags & MAP_ANONYMOUS) && prot != PROT_NONE) {
        sodos_commit_zero(e, (char*)addr, len);
        memset(addr, 0, len);
      }
      return addr;
    }
    dbg_txt("!MFX addr=");
    dbg_hex((unsigned)(uintptr_t)addr);
    dbg_txt(" len=");
    dbg_hex((unsigned)len);
    dbg_txt("\n");
    errno = ENOMEM;
    return MAP_FAILED;
  }
  if (!(flags & MAP_ANONYMOUS)) {
    /* Arquivo: copia o conteudo para uma regiao anonima. Serve para leitura
     * e para MAP_PRIVATE (o GMappedFile do glib, o load_elf do QEMU). Um
     * MAP_SHARED gravavel precisaria devolver as escritas ao arquivo. */
    char* p;
    size_t got = 0;
    if (sodos_shm_is(fd)) return sodos_shm_map(fd, len, off);
    if ((flags & MAP_SHARED) && (prot & PROT_WRITE)) return fmap_get(fd, len, off);
    p = (char*)mmap_impl(addr, len, prot, (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS, -1,
                         0, 1);
    if (p == (char*)MAP_FAILED) return MAP_FAILED;
    while (got < len) {
      ssize_t r = pread(fd, p + got, len - got, (off_t)(off + got));
      if (r < 0) {
        munmap(p, len);
        return MAP_FAILED;
      }
      if (r == 0) break;
      got += (size_t)r;
    }
    if (got < len) memset(p + got, 0, len - got); /* depois do fim do arquivo */
    return p;
  }
  {
    uint32_t want = map_align(len);
    int high = 0;
    kmal_resolve();
    raw = 0;
    if (khm && !low) {
      raw = khm((unsigned long)len, want);
      high = raw != 0;
      al = (uintptr_t)raw;
    }
    if (!raw && len <= 0xF0000000u) { /* k_malloc usa uint32_t */
      if (kmal) {
        raw = kmal((unsigned)len, want);
        al = (uintptr_t)raw;
      } else {
        raw = k_malloc((unsigned)(len + want));
        al = ((uintptr_t)raw + (want - 1)) & ~(uintptr_t)(want - 1);
      }
    }
    if (!raw) {
      dbg_txt("!MM len=");
      dbg_hex((unsigned)(len >> 12));
      dbg_txt("p\n");
      errno = ENOMEM;
      return MAP_FAILED;
    }
    b = (struct sodos_base*)malloc(sizeof(*b));
    e = (struct sodos_map*)malloc(sizeof(*e));
    if (!b || !e) {
      free(b);
      free(e);
      if (high) khf(raw);
      else k_free(raw);
      errno = ENOMEM;
      return MAP_FAILED;
    }
    b->high = high;
  }

  if (len < MM_NO_ZERO_MIN) {
    memset((void*)al, 0, len);
    b->zhi = 0;
  } else {
    b->zhi = (char*)al; /* zera no commit (sodos_commit_zero) */
  }
  b->end = (char*)al + len;
  b->p = raw;
  b->live = len;
  b->next = bases;
  bases = b;

  e->addr = (char*)al;
  e->len = len;
  e->owner = b;
  e->next = maps;
  maps = e;

#ifdef SODOS_MMAP_TRACE
  if (len >= 0x10000u) {
    dbg_txt("!MP len=");
    dbg_hex((unsigned)len);
    dbg_txt(" p=");
    dbg_hex((unsigned)(uintptr_t)al);
    dbg_txt("\n");
  }
#endif
  return (void*)al;
}

int munmap(void* addr, size_t len) {
  struct sodos_map** pp = &maps;
  char* a = (char*)addr;

#ifdef SODOS_MMAP_TRACE
  if (len >= 0x10000u) {
    dbg_txt("!MU a=");
    dbg_hex((unsigned)(uintptr_t)addr);
    dbg_txt(" l=");
    dbg_hex((unsigned)len);
    dbg_txt("\n");
  }
#endif
  if (a && sodos_shm_unmap(a, len)) return 0;
  if (a && fmap_put(a)) return 0;
  if (!a || len == 0) {
    errno = EINVAL;
    return -1;
  }
  while (*pp) {
    struct sodos_map* e = *pp;
    if (a >= e->addr && a + len <= e->addr + e->len) {
      struct sodos_base* owner = e->owner;
      void* base = owner->p;
      if (a == e->addr && len == e->len) {
        *pp = e->next;
        free(e);
      } else if (a == e->addr) {
        e->addr += len;
        e->len -= len;
      } else if (a + len == e->addr + e->len) {
        e->len -= len;
      } else {
        /* Sub-faixa no meio: divide em duas. */
        struct sodos_map* r = (struct sodos_map*)malloc(sizeof(*r));
        if (!r) {
          errno = ENOMEM;
          return -1;
        }
        r->addr = a + len;
        r->len = (size_t)((e->addr + e->len) - (a + len));
        r->owner = owner;
        r->next = e->next;
        e->next = r;
        e->len = (size_t)(a - e->addr);
      }
      owner->live -= len;
      if (owner->live == 0) {
        struct sodos_base** bp = &bases;
        while (*bp) {
          if (*bp == owner) {
            *bp = owner->next;
            break;
          }
          bp = &(*bp)->next;
        }
        if (owner->high) khf(base);
        else k_free(base);
        free(owner);
      }
      return 0;
    }
    pp = &(*pp)->next;
  }
  errno = EINVAL;
  return -1;
}

int mprotect(void* addr, size_t len, int prot) {
#ifdef SODOS_MMAP_TRACE
  if (len >= 0x10000u) {
    dbg_txt("!MT a=");
    dbg_hex((unsigned)(uintptr_t)addr);
    dbg_txt(" l=");
    dbg_hex((unsigned)len);
    dbg_txt(" p=");
    dbg_hex((unsigned)prot);
    dbg_txt("\n");
  }
#endif
  /* Sem protecao por pagina no SODOS; mas o commit (PROT_NONE -> acesso) de
   * uma reserva grande precisa achar zeros, como no Linux. */
  if (prot != PROT_NONE && addr && len) {
    struct sodos_map* e = find_map((char*)addr, len);
    if (e) sodos_commit_zero(e, (char*)addr, len);
  }
  return 0;
}

int madvise(void* addr, size_t len, int advice) {
#ifdef SODOS_MMAP_TRACE
  if (len >= 0x10000u) {
    dbg_txt("!MV a=");
    dbg_hex((unsigned)(uintptr_t)addr);
    dbg_txt(" l=");
    dbg_hex((unsigned)len);
    dbg_txt(" adv=");
    dbg_hex((unsigned)advice);
    dbg_txt("\n");
  }
#endif
  /* So MADV_DONTNEED/MADV_FREE descartam: o chamador espera achar zeros no reuso.
   * Os demais sao dicas (o Gecko pede MADV_NOHUGEPAGE sobre a pilha em uso). */
  if ((advice == MADV_DONTNEED || advice == MADV_FREE) && addr && len) memset(addr, 0, len);
  return 0;
}

int msync(void* addr, size_t len, int flags) {
  (void)addr;
  (void)len;
  (void)flags;
  return 0;
}
