/*
 * vm.c - VirtualAlloc/VirtualFree/VirtualQuery sem tabelas de paginas.
 *
 * O SODOS roda em memoria fisica em identidade: uma "reserva" do Windows e um
 * bloco de RAM de verdade (acima de 4 GB por k_hmalloc quando e grande), e o
 * "commit" so zera as paginas na primeira vez (mapa de 1 bit por pagina). Nao
 * ha protecao (VirtualProtect so responde) nem endereco escolhido: pedir um
 * endereco livre falha, e quem pede (o runtime do Go, por exemplo) cai no
 * caminho de "qualquer endereco".
 *
 * Um caso de endereco fixo e atendido: o Go reserva N+alinhamento, libera
 * tudo e reserva de novo so o trecho alinhado. O ultimo bloco liberado fica
 * guardado (wproc.spare) e a nova reserva dentro dele o reaproveita.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

void* k_hmalloc(unsigned long long n, unsigned long long align);
void k_hfree(void* p);

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define PG 0x1000u

static int bit_get(const uint8_t* m, size_t i) { return (m[i >> 3] >> (i & 7)) & 1; }
static void bit_set(uint8_t* m, size_t i) { m[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static void bit_clr(uint8_t* m, size_t i) { m[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static void reg_free(struct vmreg* r) {
  if (r->raw) {
    if (r->high) k_hfree(r->raw);
    else k_free(r->raw);
  }
  if (r->commit) k_free(r->commit);
  k_free(r);
}

static void reg_link(struct vmreg* r) {
  struct wproc* p = wp();
  r->next = p->regs;
  p->regs = r;
}

static int reg_unlink(struct vmreg* r) {
  struct vmreg** pp;
  for (pp = &wp()->regs; *pp; pp = &(*pp)->next)
    if (*pp == r) {
      *pp = r->next;
      return 1;
    }
  return 0;
}

struct vmreg* vm_find(const void* addr) {
  struct vmreg* r;
  const uint8_t* a = (const uint8_t*)addr;
  for (r = wp()->regs; r; r = r->next)
    if (a >= r->base && a < r->base + r->size) return r;
  return 0;
}

static struct vmreg* reg_new(size_t n, int kind, int committed) {
  struct vmreg* r = (struct vmreg*)wzalloc(sizeof(*r));
  if (!r) return 0;
  r->size = n;
  r->kind = kind;
  if (!committed) {
    r->commit = (uint8_t*)wzalloc((n / PG + 7) / 8 + 1);
    if (!r->commit) {
      k_free(r);
      return 0;
    }
  }
  return r;
}

struct vmreg* vm_reserve(size_t n, int kind, int committed) {
  struct vmreg* r;
  void* raw = 0;
  int high = 0;
  n = (n + 0xFFFFu) & ~(size_t)0xFFFFu;
  if (!n) return 0;
  r = reg_new(n, kind, committed);
  if (!r) return 0;
  /* pequeno: arena abaixo de 4 GB; grande: acima (cota HIMEM do kernel) */
  if (n <= (16u << 20)) raw = k_malloc_aligned((unsigned)n, 0x10000);
  if (!raw) {
    raw = k_hmalloc(n, 0x10000);
    high = raw != 0;
  }
  if (!raw && n < 0xFFF00000u) raw = k_malloc_aligned((unsigned)n, 0x10000);
  if (!raw) {
    if (r->commit) k_free(r->commit);
    k_free(r);
    return 0;
  }
  r->raw = raw;
  r->base = (uint8_t*)raw;
  r->high = high;
  if (committed) memset(raw, 0, n);
  wlock();
  reg_link(r);
  wunlock();
  return r;
}

void vm_register_image(uint8_t* base, size_t size) {
  struct vmreg* r = reg_new((size + 0xFFF) & ~(size_t)0xFFF, VM_IMAGE, 1);
  if (!r) return;
  r->base = base;   /* raw = 0: a imagem e do pe.c */
  wlock();
  reg_link(r);
  wunlock();
}

static void commit_range(struct vmreg* r, uint8_t* a, size_t n) {
  size_t first = (size_t)(a - r->base) / PG;
  size_t last = (size_t)(a - r->base + n + PG - 1) / PG;
  size_t i, run = 0;
  if (!r->commit) return;
  /* zera em trechos contiguos (o Go commita dezenas de MB de uma vez) */
  for (i = first; i <= last; i++) {
    if (i < last && !bit_get(r->commit, i)) {
      bit_set(r->commit, i);
      run++;
    } else if (run) {
      memset(r->base + (i - run) * PG, 0, run * PG);
      run = 0;
    }
  }
}

void* WINAPI wVirtualAlloc(void* addr, size_t n, DWORD type, DWORD prot) {
  struct wproc* p = wp();
  struct vmreg* r;
  (void)prot;
  if (!n) {
    wset_lasterr(ERROR_INVALID_PARAMETER);
    return 0;
  }
  if (!addr) {
    r = vm_reserve(n, VM_PRIVATE, (type & MEM_COMMIT) != 0);
    if (!r) {
      wlogx("VirtualAlloc: sem memoria para ", n);
      wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
      return 0;
    }
    return r->base;
  }
  wlock();
  r = vm_find(addr);
  if (r) {
    uint8_t* a = (uint8_t*)((uintptr_t)addr & ~(uintptr_t)(PG - 1));
    size_t len = (size_t)((uint8_t*)addr - a) + n;
    if (a + len > r->base + r->size) {
      wunlock();
      wset_lasterr(ERROR_INVALID_ADDRESS);
      return 0;
    }
    if (type & MEM_COMMIT) commit_range(r, a, len);
    wunlock();
    return a;
  }
  /* reserva em endereco escolhido: so dentro do bloco liberado ha pouco */
  r = p->spare;
  if (r && (type & MEM_RESERVE) && (uint8_t*)addr >= r->base &&
      (uint8_t*)addr + n <= r->base + r->size && !((uintptr_t)addr & 0xFFFF)) {
    size_t sz = (n + 0xFFFFu) & ~(size_t)0xFFFFu;
    if ((uint8_t*)addr + sz > r->base + r->size) sz = (size_t)(r->base + r->size - (uint8_t*)addr);
    p->spare = 0;
    if (r->commit) k_free(r->commit);
    r->commit = (uint8_t*)wzalloc((sz / PG + 7) / 8 + 1);
    r->base = (uint8_t*)addr;
    r->size = sz;
    reg_link(r);
    if (type & MEM_COMMIT) commit_range(r, r->base, n);
    wunlock();
    return addr;
  }
  wunlock();
  wset_lasterr(ERROR_INVALID_ADDRESS);
  return 0;
}

static WINAPI void* VirtualAllocEx(HANDLE proc, void* addr, size_t n, DWORD type, DWORD prot) {
  (void)proc;
  return wVirtualAlloc(addr, n, type, prot);
}

static WINAPI BOOL VirtualFree(void* addr, size_t n, DWORD type) {
  struct wproc* p = wp();
  struct vmreg* r;
  struct vmreg* old = 0;
  wlock();
  r = vm_find(addr);
  if (!r || r->kind == VM_IMAGE) {
    wunlock();
    wset_lasterr(ERROR_INVALID_ADDRESS);
    return 0;
  }
  if (type & MEM_DECOMMIT) {
    if (r->commit) {
      size_t first = (size_t)((uint8_t*)addr - r->base + PG - 1) / PG;
      size_t last = n ? (size_t)((uint8_t*)addr - r->base + n) / PG : r->size / PG;
      size_t i;
      for (i = first; i < last && i < r->size / PG; i++) bit_clr(r->commit, i);
    }
    wunlock();
    return 1;
  }
  if (!(type & MEM_RELEASE) || r->base != (uint8_t*)addr) {
    wunlock();
    wset_lasterr(ERROR_INVALID_PARAMETER);
    return 0;
  }
  reg_unlink(r);
  if (r->kind == VM_PRIVATE) {
    old = p->spare;
    p->spare = r;
  } else {
    old = r;
  }
  wunlock();
  if (old) reg_free(old);
  return 1;
}

static WINAPI BOOL VirtualProtect(void* a, size_t n, DWORD prot, DWORD* old) {
  (void)a;
  (void)n;
  (void)prot;
  if (old) *old = 4; /* PAGE_READWRITE: o SODOS nao protege paginas */
  return 1;
}
static WINAPI BOOL VirtualLock(void* a, size_t n) {
  (void)a;
  (void)n;
  return 1;
}
static WINAPI BOOL FlushInstructionCache(HANDLE h, const void* a, size_t n) {
  (void)h;
  (void)a;
  (void)n;
  return 1;
}

struct mbi {
  void* base;
  void* alloc_base;
  DWORD alloc_protect;
  uint16_t part;
  size_t region;
  DWORD state, protect, type;
};

static WINAPI size_t VirtualQuery(const void* addr, struct mbi* m, size_t len) {
  uintptr_t pg = (uintptr_t)addr & ~(uintptr_t)(PG - 1);
  struct vmreg* r;
  if (len < sizeof(*m)) return 0;
  memset(m, 0, sizeof(*m));
  m->base = (void*)pg;
  wlock();
  r = vm_find(addr);
  if (!r) {
    wunlock();
    m->region = 0x10000;
    m->state = 0x10000; /* MEM_FREE */
    m->protect = 1;     /* PAGE_NOACCESS */
    return sizeof(*m);
  }
  {
    size_t i = (pg - (uintptr_t)r->base) / PG, npg = r->size / PG, j;
    int c = r->commit ? bit_get(r->commit, i) : 1;
    for (j = i + 1; j < npg && (r->commit ? bit_get(r->commit, j) : 1) == c; j++) {
    }
    m->alloc_base = r->base;
    m->alloc_protect = r->kind == VM_IMAGE ? 0x80 : 4;
    m->region = (j - i) * PG;
    m->state = c ? MEM_COMMIT : MEM_RESERVE;
    m->protect = c ? (r->kind == VM_IMAGE ? 0x40 : 4) : 0;
    m->type = r->kind == VM_IMAGE ? 0x1000000 : 0x20000;
  }
  wunlock();
  return sizeof(*m);
}

static WINAPI size_t VirtualQueryEx(HANDLE h, const void* addr, struct mbi* m, size_t len) {
  (void)h;
  return VirtualQuery(addr, m, len);
}

struct memstatusex {
  DWORD len, load;
  uint64_t total_phys, avail_phys, total_page, avail_page, total_virt, avail_virt, avail_ext;
};
static WINAPI BOOL GlobalMemoryStatusEx(struct memstatusex* s) {
  unsigned total = 0, freeb = 0;
  k_meminfo(&total, &freeb);
  s->load = 50;
  s->total_phys = (uint64_t)total + (4ull << 30);
  s->avail_phys = (uint64_t)freeb + (2ull << 30);
  s->total_page = s->total_phys;
  s->avail_page = s->avail_phys;
  s->total_virt = 0x7FFFFFFE0000ull;
  s->avail_virt = 0x7FFF00000000ull;
  s->avail_ext = 0;
  return 1;
}

#define S(n) {#n, (void*)n}
const struct wine_sym vm_syms[] = {{"VirtualAlloc", (void*)wVirtualAlloc},
                                   S(VirtualAllocEx),
                                   S(VirtualFree),
                                   S(VirtualProtect),
                                   S(VirtualLock),
                                   S(FlushInstructionCache),
                                   S(VirtualQuery),
                                   S(VirtualQueryEx),
                                   S(GlobalMemoryStatusEx),
                                   {0, 0}};
