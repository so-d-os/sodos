/* sodos_mmap.c - mmap anonimo sobre o heap do kernel.
 *
 * Enquanto o kernel nao tem paginas sob demanda, o mapa anonimo e um bloco
 * do heap do processo, zerado e alinhado a 2 MiB (o coletor de lixo do
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
  void* p;    /* bloco original devolvido pelo k_malloc */
  size_t live; /* bytes ainda visiveis deste bloco */
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

static struct sodos_map* maps;
static struct sodos_base* bases;

/* Reservas gigantes (o JIT reserva ~2 GB de espaco de codigo) nao sao
 * zeradas: ficam PROT_NONE e as paginas so sao "commitadas" depois. Todo o
 * resto (chunks do GC, nursery) precisa vir zerado. */
#define MM_NO_ZERO_MIN 0x20000000u

static struct sodos_map* find_map(char* a, size_t len) {
  struct sodos_map* e;
  for (e = maps; e; e = e->next)
    if (a >= e->addr && a + len <= e->addr + e->len) return e;
  return 0;
}

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
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
    if (addr && find_map((char*)addr, len)) {
      if ((flags & MAP_ANONYMOUS) && prot != PROT_NONE) memset(addr, 0, len);
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
    if ((flags & MAP_SHARED) && (prot & PROT_WRITE)) {
      errno = ENOSYS;
      return MAP_FAILED;
    }
    p = (char*)mmap(addr, len, prot, (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
  if (len > 0xF0000000u) { /* k_malloc usa uint32_t e len + MM_ALIGN nao pode estourar */
    errno = ENOMEM;
    return MAP_FAILED;
  }

  raw = k_malloc((unsigned)(len + MM_ALIGN));
  if (!raw) {
    dbg_txt("!MM len=");
    dbg_hex((unsigned)len);
    dbg_txt("\n");
    errno = ENOMEM;
    return MAP_FAILED;
  }
  al = ((uintptr_t)raw + (MM_ALIGN - 1)) & ~(uintptr_t)(MM_ALIGN - 1);

  b = (struct sodos_base*)malloc(sizeof(*b));
  e = (struct sodos_map*)malloc(sizeof(*e));
  if (!b || !e) {
    free(b);
    free(e);
    k_free(raw);
    errno = ENOMEM;
    return MAP_FAILED;
  }

  if (len < MM_NO_ZERO_MIN) memset((void*)al, 0, len);

  b->p = raw;
  b->live = len;
  b->next = bases;
  bases = b;

  e->addr = (char*)al;
  e->len = len;
  e->owner = b;
  e->next = maps;
  maps = e;

  if (len >= 0x10000u) {
    dbg_txt("!MP len=");
    dbg_hex((unsigned)len);
    dbg_txt(" p=");
    dbg_hex((unsigned)(uintptr_t)al);
    dbg_txt("\n");
  }
  return (void*)al;
}

int munmap(void* addr, size_t len) {
  struct sodos_map** pp = &maps;
  char* a = (char*)addr;

  if (len >= 0x10000u) {
    dbg_txt("!MU a=");
    dbg_hex((unsigned)(uintptr_t)addr);
    dbg_txt(" l=");
    dbg_hex((unsigned)len);
    dbg_txt("\n");
  }
  if (a && sodos_shm_unmap(a, len)) return 0;
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
            free(owner);
            break;
          }
          bp = &(*bp)->next;
        }
        k_free(base);
      }
      return 0;
    }
    pp = &(*pp)->next;
  }
  errno = EINVAL;
  return -1;
}

int mprotect(void* addr, size_t len, int prot) {
  if (len >= 0x10000u) {
    dbg_txt("!MT a=");
    dbg_hex((unsigned)(uintptr_t)addr);
    dbg_txt(" l=");
    dbg_hex((unsigned)len);
    dbg_txt(" p=");
    dbg_hex((unsigned)prot);
    dbg_txt("\n");
  }
  return 0; /* sem protecao por pagina no SODOS */
}

int madvise(void* addr, size_t len, int advice) {
  if (len >= 0x10000u) {
    dbg_txt("!MV a=");
    dbg_hex((unsigned)(uintptr_t)addr);
    dbg_txt(" l=");
    dbg_hex((unsigned)len);
    dbg_txt(" adv=");
    dbg_hex((unsigned)advice);
    dbg_txt("\n");
  }
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
