/* mm.c - First-fit. Cada bloco guarda o tamanho total, inclusive o cabecalho.
 * Duas arenas: a do kernel (kmalloc, 2 MB em 0xA80000) e a alta (umalloc, a
 * maior regiao E820 abaixo de 4 GB, coberta pela identidade de kernel_entry)
 * para programas, DLLs e buffers grandes.
 *
 * Desde o boot em dois estagios o kernel fica em 0x01000000.  A arena alta e
 * escolhida a partir do mapa E820 entregue no BootInfo: nunca testamos (nem
 * escrevemos) memoria reservada e nunca comecamos antes de __bss_end.
 */
#include "mm.h"
#include "bootinfo.h"
#include "serial.h"

/* O kernel fica em 0x01000000.  A FAT ocupa 0xA00000-0xA59000 e o heap do
 * kernel fica em 0xA80000-0xC7FFFF, abaixo do kernel. */
#define MM_HEAP_START 0x00A80000u
#define MM_HEAP_END   0x00C80000u
#define MM_ALIGN      16u

/* Limite da identidade montada por kernel_entry (4 GB menos uma pagina de
 * 2 MB, para que fim de arena e ponteiros caibam em 32 bits).  Sem E820 o
 * teste de RAM para em 1 GB: acima disso pode haver MMIO. */
#define UM_MAP_MAX    0xFFE00000u
#define UM_PROBE_MAX  0x40000000u
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

/* ---------------- RAM acima de 4 GB ----------------
 * kernel_entry so monta identidade ate 4 GB.  Aqui completamos o mesmo PDPT
 * (GB 4..HM_MAX_GB-1) com paginas de 2 MB, so onde o E820 diz que ha RAM, e
 * entregamos essas regioes a um alocador de 64 bits (hmalloc).  Programas
 * x32 nao enxergam esses enderecos; quem usa e o kernel. */
#define HM_MAX_GB     64u
#define HM_FIRST_GB   4u
#define HM_MAX_ARENAS 8u
#define HM_PAGE       0x200000ULL
#define HM_GB         0x40000000ULL

struct hm_hdr {
  uint64_t size;
  uint32_t free;
  uint32_t owner;
};

struct harena {
  uint64_t start;
  uint64_t end;
};

static uint64_t hm_pd[HM_MAX_GB - HM_FIRST_GB][512] __attribute__((aligned(4096)));
static struct harena hm_arenas[HM_MAX_ARENAS];
static uint32_t hm_count;

static void hm_add_arena(uint64_t s, uint64_t e) {
  uint32_t i;
  if (e <= s + UM_MIN_SIZE) return;
  /* Regioes vizinhas no E820 viram uma so. */
  for (i = 0; i < hm_count; ++i) {
    if (hm_arenas[i].end == s) {
      hm_arenas[i].end = e;
      return;
    }
  }
  if (hm_count >= HM_MAX_ARENAS) return;
  hm_arenas[hm_count].start = s;
  hm_arenas[hm_count].end = e;
  hm_count++;
}

/* Mapeia [s,e) (alinhados a 2 MB) na identidade; devolve 0 se nao conseguiu. */
static int hm_map(uint64_t* pdpt, uint64_t s, uint64_t e) {
  uint64_t a;
  for (a = s; a < e; a += HM_PAGE) {
    uint32_t gb = (uint32_t)(a / HM_GB);
    uint32_t idx = (uint32_t)((a % HM_GB) / HM_PAGE);
    uint64_t* pd;
    if (gb < HM_FIRST_GB || gb >= HM_MAX_GB) return 0;
    if (pdpt[gb] & 1u) {
      if (pdpt[gb] & 0x80u) continue;  /* pagina de 1 GB ja montada */
      pd = (uint64_t*)(uintptr_t)(pdpt[gb] & 0x000FFFFFFFFFF000ULL);
    } else {
      pd = hm_pd[gb - HM_FIRST_GB];
      pdpt[gb] = (uint64_t)(uintptr_t)pd | 3u;
    }
    if (!(pd[idx] & 1u)) pd[idx] = a | 0x83u;  /* P | RW | PS, write-back */
  }
  return 1;
}

static void hm_init(const struct BootInfo* bi) {
  const struct BootMemEntry* map;
  uint64_t cr3;
  uint64_t* pml4;
  uint64_t* pdpt;
  uint32_t i;
  uint64_t total = 0;

  hm_count = 0;
  map = bootinfo_mem_map(bi);
  if (!bi || !map || bi->mem_count == 0) return;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  pml4 = (uint64_t*)(uintptr_t)(cr3 & 0x000FFFFFFFFFF000ULL);
  if (!(pml4[0] & 1u)) return;
  pdpt = (uint64_t*)(uintptr_t)(pml4[0] & 0x000FFFFFFFFFF000ULL);

  for (i = 0; i < bi->mem_count; ++i) {
    uint64_t b = map[i].base;
    uint64_t e = b + map[i].length;
    if (map[i].type != BOOTINFO_MEM_USABLE || map[i].length == 0) continue;
    if (e < b) continue;
    if (b < 0x100000000ULL) b = 0x100000000ULL;
    if (e > (uint64_t)HM_MAX_GB * HM_GB) e = (uint64_t)HM_MAX_GB * HM_GB;
    b = (b + HM_PAGE - 1u) & ~(HM_PAGE - 1u);
    e &= ~(HM_PAGE - 1u);
    if (e <= b) continue;
    if (!hm_map(pdpt, b, e)) continue;
    hm_add_arena(b, e);
  }
  __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");

  for (i = 0; i < hm_count; ++i) {
    struct hm_hdr* h = (struct hm_hdr*)(uintptr_t)hm_arenas[i].start;
    h->size = hm_arenas[i].end - hm_arenas[i].start;
    h->free = 1;
    h->owner = 0;
    total += h->size;
  }
  serial_puts("[mm] RAM acima de 4 GB: ");
  serial_hex32((uint32_t)(total >> 20));
  serial_puts(" MB em ");
  serial_hex32(hm_count);
  serial_puts(" regioes\n");
}

static int hm_ok(const struct harena* a, const struct hm_hdr* h) {
  uint64_t p = (uint64_t)(uintptr_t)h;
  if (p < a->start || p >= a->end) return 0;
  if (h->size < sizeof(struct hm_hdr)) return 0;
  if (h->size > a->end - p) return 0;
  return 1;
}

static void hm_merge(const struct harena* a, struct hm_hdr* h) {
  for (;;) {
    struct hm_hdr* nxt = (struct hm_hdr*)((uint8_t*)h + h->size);
    if ((uint64_t)(uintptr_t)nxt >= a->end || !hm_ok(a, nxt) || !nxt->free) return;
    h->size += nxt->size;
  }
}

int hm_ready(void) { return hm_count != 0; }

void* hmalloc(uint64_t nbytes) {
  uint64_t need;
  uint32_t i;
  if (nbytes == 0) return 0;
  need = (nbytes + sizeof(struct hm_hdr) + (MM_ALIGN - 1u)) & ~(uint64_t)(MM_ALIGN - 1u);
  if (need < nbytes) return 0;
  for (i = 0; i < hm_count; ++i) {
    const struct harena* a = &hm_arenas[i];
    struct hm_hdr* h = (struct hm_hdr*)(uintptr_t)a->start;
    while (hm_ok(a, h)) {
      if (h->free) hm_merge(a, h);
      if (h->free && h->size >= need) {
        uint64_t rest = h->size - need;
        if (rest >= sizeof(struct hm_hdr) + MM_ALIGN) {
          struct hm_hdr* nxt = (struct hm_hdr*)((uint8_t*)h + need);
          nxt->size = rest;
          nxt->free = 1;
          nxt->owner = 0;
          h->size = need;
        }
        h->free = 0;
        h->owner = 0;
        return (uint8_t*)h + sizeof(struct hm_hdr);
      }
      h = (struct hm_hdr*)((uint8_t*)h + h->size);
    }
  }
  return 0;
}

void hfree(void* ptr) {
  uint32_t i;
  uint64_t p;
  if (!ptr) return;
  p = (uint64_t)(uintptr_t)ptr - sizeof(struct hm_hdr);
  for (i = 0; i < hm_count; ++i) {
    const struct harena* a = &hm_arenas[i];
    struct hm_hdr* h = (struct hm_hdr*)(uintptr_t)p;
    if (p < a->start || p >= a->end) continue;
    if (!hm_ok(a, h) || h->free) return;
    h->free = 1;
    hm_merge(a, h);
    return;
  }
}

void hm_stats(uint64_t* total, uint64_t* used, uint64_t* free_bytes, uint64_t* largest) {
  uint64_t t = 0, u = 0, f = 0, big = 0;
  uint32_t i;
  for (i = 0; i < hm_count; ++i) {
    const struct harena* a = &hm_arenas[i];
    struct hm_hdr* h = (struct hm_hdr*)(uintptr_t)a->start;
    t += a->end - a->start;
    while (hm_ok(a, h)) {
      if (h->free) {
        f += h->size;
        if (h->size > big) big = h->size;
      } else {
        u += h->size;
      }
      h = (struct hm_hdr*)((uint8_t*)h + h->size);
    }
  }
  if (total) *total = t;
  if (used) *used = u;
  if (free_bytes) *free_bytes = f;
  if (largest) *largest = big;
}

uint32_t hm_regions(void) { return hm_count; }
uint64_t hm_region_start(uint32_t i) { return i < hm_count ? hm_arenas[i].start : 0; }
uint64_t hm_region_end(uint32_t i) { return i < hm_count ? hm_arenas[i].end : 0; }

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
    /* A maior regiao livre ate UM_MAP_MAX: firmwares de notebook deixam
     * buracos reservados que cortariam a primeira regiao. */
    uint64_t best_s = 0, best_e = 0;
    for (i = 0; i < bi->mem_count; ++i) {
      uint64_t b = map[i].base;
      uint64_t len = map[i].length;
      uint64_t e;
      uint64_t s;
      if (map[i].type != BOOTINFO_MEM_USABLE || len == 0) continue;
      if (b > 0xFFFFFFFFULL) continue;
      e = b + len;
      if (e < b) e = 0x100000000ULL;
      if (e > UM_MAP_MAX) e = UM_MAP_MAX;
      if (e <= start) continue;
      s = (b > start) ? b : start;
      s = (s + UM_ALIGN_BIG - 1u) & ~(uint64_t)(UM_ALIGN_BIG - 1u);
      if (e > s + UM_MIN_SIZE && e - s > best_e - best_s) {
        best_s = s;
        best_e = e;
      }
    }
    if (best_e) arena_init(&high, (uint32_t)best_s, (uint32_t)best_e);
    /* A arena baixa e fixa; so a habilitamos se o E820 disser que e RAM. */
    if (kend >= MM_HEAP_END && range_usable(bi, MM_HEAP_START, MM_HEAP_END))
      arena_init(&low, MM_HEAP_START, MM_HEAP_END);
    hm_init(bi);
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

/* Do fim do ultimo bloco livre que cabe: caches do kernel que ficam (buffers
 * de soquete, cache de pastas do FAT) nao picam o comeco da arena, onde os
 * programas pedem blocos grandes e contiguos (o JIT do Firefox reserva 2 GB). */
void* umalloc_top(uint32_t nbytes) {
  struct arena* a = &high;
  struct mm_hdr* h;
  struct mm_hdr* best = 0;
  uint32_t need;
  if (!a->ready || nbytes == 0) return 0;
  need = align_up(nbytes + (uint32_t)sizeof(struct mm_hdr));
  if (need < nbytes) return 0;
  h = blk_at(a->start);
  while (range_ok(a, h)) {
    if (h->free) merge_next(a, h);
    if (h->free && h->size >= need) best = h;
    h = blk_at((uint32_t)h + h->size);
  }
  if (!best) return 0;
  if (best->size - need >= sizeof(struct mm_hdr) + MM_ALIGN) {
    struct mm_hdr* top = blk_at((uint32_t)best + best->size - need);
    best->size -= need;
    top->size = need;
    top->free = 0;
    top->owner = 0;
    return (uint8_t*)top + sizeof(struct mm_hdr);
  }
  best->free = 0;
  best->owner = 0;
  return (uint8_t*)best + sizeof(struct mm_hdr);
}
/* Bloco cujo endereco util e multiplo de align (potencia de 2, >= 16). O
 * mmap anonimo do port do Firefox pedia len + 2 MB para alinhar sozinho: um
 * chunk de 1 MB do GC custava 3 MB e o heap acabava (OOM no Nursery). Aqui a
 * folga antes do endereco alinhado volta a ser um bloco livre. */
void* umalloc_aligned(uint32_t nbytes, uint32_t align) {
  struct arena* a = &high;
  struct mm_hdr* h;
  uint32_t need;
  const uint32_t hs = (uint32_t)sizeof(struct mm_hdr);
  if (align <= MM_ALIGN) return umalloc(nbytes);
  if (!a->ready || nbytes == 0 || (align & (align - 1u))) return 0;
  need = align_up(nbytes + hs);
  if (need < nbytes) return 0;
  h = blk_at(a->start);
  while (range_ok(a, h)) {
    if (h->free) merge_next(a, h);
    if (h->free && h->size >= need) {
      uint32_t start = (uint32_t)h;
      uint32_t end = start + h->size;
      uint32_t p = (start + hs + align - 1u) & ~(align - 1u);
      /* A folga da frente precisa caber como bloco livre valido. */
      if (p - hs != start && p - hs - start < hs + MM_ALIGN) p += align;
      if (p > start && p - hs >= start && p - hs <= end && end - (p - hs) >= need) {
        struct mm_hdr* n = blk_at(p - hs);
        if ((uint32_t)n != start) {
          h->size = (uint32_t)n - start; /* cabeca continua livre */
          n->size = end - (uint32_t)n;
        }
        if (n->size - need >= hs + MM_ALIGN) {
          struct mm_hdr* t = blk_at((uint32_t)n + need);
          t->size = n->size - need;
          t->free = 1;
          t->owner = 0;
          n->size = need;
        }
        n->free = 0;
        n->owner = cur_owner;
        return (uint8_t*)n + hs;
      }
    }
    h = blk_at((uint32_t)h + h->size);
  }
  return 0;
}

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

static void arena_walk(const struct arena* a, int area, mm_walk_fn fn, void* arg) {
  struct mm_hdr* h;
  if (!a->ready) return;
  h = blk_at(a->start);
  while (range_ok(a, h)) {
    fn(arg, area, (uint32_t)h, h->size, !h->free, h->free ? 0 : h->owner);
    h = blk_at((uint32_t)h + h->size);
  }
}

void mm_walk(mm_walk_fn fn, void* arg) {
  uint32_t i;
  if (!fn) return;
  arena_walk(&low, 0, fn, arg);
  arena_walk(&high, 1, fn, arg);
  for (i = 0; i < hm_count; ++i) {
    const struct harena* a = &hm_arenas[i];
    struct hm_hdr* h = (struct hm_hdr*)(uintptr_t)a->start;
    while (hm_ok(a, h)) {
      fn(arg, 2, (uint64_t)(uintptr_t)h, h->size, !h->free, 0);
      h = (struct hm_hdr*)((uint8_t*)h + h->size);
    }
  }
}

uint32_t mm_kernel_end(void) { return kernel_reserved_end(bootinfo_get()); }

/* Marca [phys, phys+size) como write-combining (PAT 1: PWT=1, PCD=0, que o
 * kernel_entry programa no MSR do PAT). Para o LFB que o WIN acha depois do
 * boot (Bochs VBE do QEMU/VirtualBox em 0xFD000000): o mapeamento padrao e
 * write-back, mas o MTRR do SeaBIOS deixa o buraco PCI como UC, e UC vence WB.
 * Com o PAT WC, a copia do quadro deixa de ser uncached (era 40-200 MB/s).
 * So mexe em paginas de 2 MB ja presentes; devolve quantas mudou. */
int mm_map_wc(uint64_t phys, uint64_t size) {
  uint64_t cr3, a, end = phys + size;
  uint64_t* pml4;
  uint64_t* pdpt;
  int n = 0;
  if (!size) return 0;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  pml4 = (uint64_t*)(uintptr_t)(cr3 & 0x000FFFFFFFFFF000ULL);
  if (!(pml4[0] & 1u)) return 0;
  pdpt = (uint64_t*)(uintptr_t)(pml4[0] & 0x000FFFFFFFFFF000ULL);
  for (a = phys & ~(HM_PAGE - 1u); a < end; a += HM_PAGE) {
    uint32_t gb = (uint32_t)(a / HM_GB);
    uint64_t* pd;
    uint64_t* e;
    if (gb >= 512 || !(pdpt[gb] & 1u) || (pdpt[gb] & 0x80u)) continue;
    pd = (uint64_t*)(uintptr_t)(pdpt[gb] & 0x000FFFFFFFFFF000ULL);
    e = &pd[(a % HM_GB) / HM_PAGE];
    if ((*e & 0x81u) != 0x81u) continue;           /* presente e de 2 MB */
    *e = (*e & ~(0x10ULL | 0x1000ULL)) | 0x08ULL;  /* PCD=0, PAT=0, PWT=1 */
    n++;
  }
  if (n) __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
  return n;
}
