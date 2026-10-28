/* mm.c - First-fit. Cada bloco guarda o tamanho total, inclusive o cabecalho.
 * Duas arenas: a do kernel (kmalloc, 2 MB em 0xA80000) e a alta (umalloc, logo
 * apos o kernel, ate o limite da identidade de 256 MB) para programas, DLLs e
 * buffers grandes.
 *
 * Desde o boot em dois estagios o kernel fica em 0x01000000.  A arena alta e
 * escolhida a partir do mapa E820 entregue no BootInfo: nunca testamos (nem
 * escrevemos) memoria reservada e nunca comecamos antes de __bss_end.
 */
#include "mm.h"
#include "bootinfo.h"

/* O kernel fica em 0x01000000.  A FAT ocupa 0xA00000-0xA59000 e o heap do
 * kernel fica em 0xA80000-0xC7FFFF, abaixo do kernel. */
#define MM_HEAP_START 0x00A80000u
#define MM_HEAP_END   0x00C80000u
#define MM_ALIGN      16u

/* Limite da identidade montada por kernel_entry (128 paginas de 2 MB). */
#define UM_PROBE_MAX  0x10000000u
#define UM_STEP       0x00100000u
#define UM_START      0x01000000u
#define UM_MIN_SIZE   0x00400000u
#define UM_ALIGN_BIG  0x00200000u

extern char __bss_end[];

struct mm_hdr {
  uint32_t size;
  uint32_t free;
  uint32_t owner;
  uint32_t pad;
};

struct arena {
  uint32_t start;
  uint32_t end;
  int ready;
};

static struct arena low;
static struct arena high;
static uint32_t cur_owner;

static uint32_t align_up(uint32_t n) {
  return (n + (MM_ALIGN - 1u)) & ~(MM_ALIGN - 1u);
}

static struct mm_hdr* blk_at(uint32_t addr) {
  return (struct mm_hdr*)addr;
}

static int range_ok(const struct arena* a, struct mm_hdr* h) {
  uint32_t p = (uint32_t)h;
  if (p < a->start || p >= a->end) return 0;
  if (h->size < sizeof(struct mm_hdr)) return 0;
  if (h->size > a->end - p) return 0;
  return 1;
}

static void arena_init(struct arena* a, uint32_t start, uint32_t end) {
  struct mm_hdr* h = blk_at(start);
  a->start = start;
  a->end = end;
  h->size = end - start;
  h->free = 1;
  h->owner = 0;
  a->ready = 1;
}

/* Fallback sem E820: testa cada MB acima do kernel.  Nunca comeca em UM_START
 * puro, porque o kernel ocupa 0x01000000 em diante. */
static uint32_t probe_ram_end(uint32_t start) {
  uint32_t a;
  if (start < UM_START) start = UM_START;
  start = (start + UM_STEP - 1u) & ~(UM_STEP - 1u);
  for (a = start; a < UM_PROBE_MAX; a += UM_STEP) {
    volatile uint32_t* p = (volatile uint32_t*)(a + UM_STEP - 4u);
    uint32_t old = *p;
    *p = 0x5A3CA55Au;
    if (*p != 0x5A3CA55Au) return a;
    *p = ~0x5A3CA55Au;
    if (*p != ~0x5A3CA55Au) return a;
    *p = old;
  }
  return UM_PROBE_MAX;
}

/* Verdadeiro se [start,end) cabe inteiro em uma entrada E820 utilizavel. */
static int range_usable(const struct BootInfo* bi, uint32_t start, uint32_t end) {
  const struct BootMemEntry* map;
  uint32_t i;
  if (!bi || bi->mem_count == 0) return 0;
  map = bootinfo_mem_map(bi);
  if (!map) return 0;
  for (i = 0; i < bi->mem_count; ++i) {
    uint64_t b = map[i].base;
    uint64_t len = map[i].length;
    uint64_t e;
    if (map[i].type != BOOTINFO_MEM_USABLE || len == 0) continue;
    if (b > 0xFFFFFFFFULL) continue;
    e = b + len;
    if (e < b) e = 0x100000000ULL;
    if (b <= start && e >= end) return 1;
  }
  return 0;
}

/* Fim da memoria que o kernel ocupa: __bss_end e a verdade do link; o tamanho
 * declarado no BootInfo cobre carregadores que informem um initrd/arquivo. */
static uint32_t kernel_reserved_end(const struct BootInfo* bi) {
  uint32_t end = (uint32_t)(uintptr_t)__bss_end;
  if (bi && bi->kernel_phys <= 0xFFFFFFFFULL) {
    uint64_t kend = bi->kernel_phys + bi->kernel_size;
    if (kend > end && kend <= 0xFFFFFFFFULL) end = (uint32_t)kend;
  }
  return end;
}

void mm_init(void) {
  const struct BootInfo* bi = bootinfo_get();
  const struct BootMemEntry* map;
  uint32_t kend = kernel_reserved_end(bi);
  uint32_t start;
  uint32_t i;

  low.ready = 0;
  high.ready = 0;

  if (kend < UM_START) kend = UM_START;
  start = (kend + UM_ALIGN_BIG - 1u) & ~(UM_ALIGN_BIG - 1u);

  map = bootinfo_mem_map(bi);
  if (bi && map && bi->mem_count != 0) {
    for (i = 0; i < bi->mem_count; ++i) {
      uint64_t b = map[i].base;
      uint64_t len = map[i].length;
      uint64_t e;
      uint64_t s;
      if (map[i].type != BOOTINFO_MEM_USABLE || len == 0) continue;
      if (b > 0xFFFFFFFFULL) continue;
      e = b + len;
      if (e < b) e = 0x100000000ULL;
      if (e > UM_PROBE_MAX) e = UM_PROBE_MAX;
      if (e <= start) continue;
      s = (b > start) ? b : start;
      s = (s + UM_ALIGN_BIG - 1u) & ~(uint64_t)(UM_ALIGN_BIG - 1u);
      if (e > s + UM_MIN_SIZE) {
        arena_init(&high, (uint32_t)s, (uint32_t)e);
        break;
      }
    }
    /* A arena baixa e fixa; so a habilitamos se o E820 disser que e RAM. */
    if (kend >= MM_HEAP_END && range_usable(bi, MM_HEAP_START, MM_HEAP_END))
      arena_init(&low, MM_HEAP_START, MM_HEAP_END);
  } else {
    uint32_t top = probe_ram_end(start);
    if (top >= start + UM_MIN_SIZE)
      arena_init(&high, start, top);
    if (top >= MM_HEAP_END && kend >= MM_HEAP_END)
      arena_init(&low, MM_HEAP_START, MM_HEAP_END);
  }
}

int mm_ready(void) { return low.ready; }
uint32_t mm_heap_start(void) { return MM_HEAP_START; }
uint32_t mm_heap_end(void) { return MM_HEAP_END; }

static void merge_next(const struct arena* a, struct mm_hdr* h) {
  for (;;) {
    struct mm_hdr* nxt = blk_at((uint32_t)h + h->size);
    if ((uint32_t)nxt >= a->end || !range_ok(a, nxt) || !nxt->free) return;
    h->size += nxt->size;
  }
}

static void* arena_alloc(struct arena* a, uint32_t nbytes, uint32_t owner) {
  struct mm_hdr* h;
  uint32_t need;
  if (!a->ready || nbytes == 0) return 0;
  need = align_up(nbytes + (uint32_t)sizeof(struct mm_hdr));
  if (need < nbytes) return 0;
  h = blk_at(a->start);
  while (range_ok(a, h)) {
    if (h->free) merge_next(a, h);
    if (h->free && h->size >= need) {
      uint32_t rest = h->size - need;
      if (rest >= sizeof(struct mm_hdr) + MM_ALIGN) {
        struct mm_hdr* nxt = blk_at((uint32_t)h + need);
        nxt->size = rest;
        nxt->free = 1;
        nxt->owner = 0;
        h->size = need;
      }
      h->free = 0;
      h->owner = owner;
      return (uint8_t*)h + sizeof(struct mm_hdr);
    }
    h = blk_at((uint32_t)h + h->size);
  }
  return 0;
}

static struct mm_hdr* arena_hdr(const struct arena* a, void* ptr) {
  uint32_t p;
  struct mm_hdr* h;
  if (!a->ready || !ptr) return 0;
  p = (uint32_t)ptr - (uint32_t)sizeof(struct mm_hdr);
  if (p < a->start || p >= a->end) return 0;
  h = blk_at(p);
  if (!range_ok(a, h) || h->free) return 0;
  return h;
}

static void arena_free(struct arena* a, void* ptr) {
  struct mm_hdr* h = arena_hdr(a, ptr);
  if (!h) return;
  h->free = 1;
  h->owner = 0;
  merge_next(a, h);
}

static void arena_stats(const struct arena* a, uint32_t* total, uint32_t* used,
                        uint32_t* free_bytes, uint32_t* largest) {
  struct mm_hdr* h;
  uint32_t t = 0, u = 0, f = 0, big = 0;
  if (a->ready) {
    t = a->end - a->start;
    h = blk_at(a->start);
    while (range_ok(a, h)) {
      if (h->free) {
        f += h->size;
        if (h->size > big) big = h->size;
      } else {
        u += h->size;
      }
      h = blk_at((uint32_t)h + h->size);
    }
  }
  if (total) *total = t;
  if (used) *used = u;
  if (free_bytes) *free_bytes = f;
  if (largest) *largest = big;
}

void* kmalloc(uint32_t nbytes) { return arena_alloc(&low, nbytes, 0); }
void kfree(void* ptr) { arena_free(&low, ptr); }

void mm_stats(uint32_t* total, uint32_t* used, uint32_t* free_bytes, uint32_t* largest) {
  arena_stats(&low, total, used, free_bytes, largest);
}

int um_ready(void) { return high.ready; }
uint32_t um_start(void) { return high.start; }
uint32_t um_end(void) { return high.end; }

void um_set_owner(uint32_t owner) { cur_owner = owner; }
uint32_t um_owner(void) { return cur_owner; }

void* umalloc(uint32_t nbytes) { return arena_alloc(&high, nbytes, cur_owner); }
void* umalloc_owned(uint32_t nbytes, uint32_t owner) { return arena_alloc(&high, nbytes, owner); }
void ufree(void* ptr) { arena_free(&high, ptr); }

uint32_t usize(void* ptr) {
  struct mm_hdr* h = arena_hdr(&high, ptr);
  if (!h) return 0;
  return h->size - (uint32_t)sizeof(struct mm_hdr);
}

void* urealloc(void* ptr, uint32_t nbytes) {
  uint32_t old;
  uint8_t* n;
  uint32_t i;
  struct mm_hdr* h;
  if (!ptr) return umalloc(nbytes);
  if (nbytes == 0) {
    ufree(ptr);
    return 0;
  }
  h = arena_hdr(&high, ptr);
  if (!h) return 0;
  old = h->size - (uint32_t)sizeof(struct mm_hdr);
  if (old >= nbytes) return ptr;
  merge_next(&high, h);
  if (h->size - (uint32_t)sizeof(struct mm_hdr) >= nbytes) return ptr;
  n = (uint8_t*)arena_alloc(&high, nbytes, h->owner);
  if (!n) return 0;
  for (i = 0; i < old; i++) n[i] = ((uint8_t*)ptr)[i];
  arena_free(&high, ptr);
  return n;
}

void ufree_owner(uint32_t owner) {
  struct mm_hdr* h;
  if (!high.ready || owner == 0) return;
  h = blk_at(high.start);
  while (range_ok(&high, h)) {
    if (!h->free && h->owner == owner) {
      h->free = 1;
      h->owner = 0;
    }
    h = blk_at((uint32_t)h + h->size);
  }
}

void um_stats(uint32_t* total, uint32_t* used, uint32_t* free_bytes, uint32_t* largest) {
  arena_stats(&high, total, used, free_bytes, largest);
}
