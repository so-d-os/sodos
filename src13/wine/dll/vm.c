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

/* Memoria virtual do kernel (kernel/vmem.c, W4): reserva so de enderecos,
 * commit com paginas de 4 KB, protecao por pagina. */
struct vmem_info {
  uint64_t base, size, alloc_base;
  int committed, prot;
};
uint64_t k_vm_reserve(uint64_t size, uint64_t align, uint64_t at);
int k_vm_commit(uint64_t va, uint64_t size, int prot);
int k_vm_decommit(uint64_t va, uint64_t size);
int k_vm_release(uint64_t va);
int k_vm_protect(uint64_t va, uint64_t size, int prot, int* old);
int k_vm_query(uint64_t va, struct vmem_info* out);
#define VIRT_MIN (64u << 20) /* reservas a partir daqui (sem commit) sao virtuais */

/* PAGE_* -> 0 sem acesso, 1 leitura, 2 leitura e escrita */
static int prot_k(DWORD prot) {
  if (prot & 0x100) return 0;                      /* PAGE_GUARD */
  switch (prot & 0xFF) {
    case 0x01: return 0;                           /* NOACCESS */
    case 0x02: case 0x10: case 0x20: return 1;    /* READONLY, EXECUTE(_READ) */
    default: return 2;
  }
}
static DWORD prot_w(int k) { return k == 0 ? 0x01 : k == 1 ? 0x02 : 0x04; }

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define PG 0x1000u

static int bit_get(const uint8_t* m, size_t i) { return (m[i >> 3] >> (i & 7)) & 1; }
static void bit_set(uint8_t* m, size_t i) { m[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static void bit_clr(uint8_t* m, size_t i) { m[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static void reg_free(struct vmreg* r) {
  if (r->virt) {
    k_vm_release((uint64_t)(uintptr_t)r->base);
    k_free(r);
    return;
  }
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

/* Reserva virtual (enderecos sem RAM); at = endereco pedido ou 0. */
static struct vmreg* vm_reserve_virt(uintptr_t at, size_t n, DWORD type, DWORD prot) {
  struct vmreg* r;
  uint64_t va;
  n = (n + 0xFFFFu) & ~(size_t)0xFFFFu;
  if (!n) return 0;
  va = k_vm_reserve(n, 0x10000, at);
  if (!va) return 0;
  r = (struct vmreg*)wzalloc(sizeof(*r));
  if (!r) {
    k_vm_release(va);
    return 0;
  }
  r->base = (uint8_t*)(uintptr_t)va;
  r->size = n;
  r->kind = VM_PRIVATE;
  r->virt = 1;
  if ((type & MEM_COMMIT) && !k_vm_commit(va, n, prot_k(prot))) {
    k_vm_release(va);
    k_free(r);
    return 0;
  }
  wlock();
  reg_link(r);
  wunlock();
  return r;
}

/* Pilha de thread: o Windows so reserva o tamanho do EXE (8 MB no Claude) e
 * commita sob demanda. Aqui a reserva e so de enderecos (memoria virtual,
 * acima de 1 TB) e o topo de "commit" bytes ganha quadros do heap alto. Antes
 * eram 8 MB de RAM abaixo de 4 GB por thread: com 4 processos do Chromium
 * (dezenas de threads cada) a arena baixa acabava e o CreateThread falhava
 * (o Chromium encerra como falta de memoria). A thread so enxerga a parte
 * commitada (TEB.StackLimit/DeallocationStack, obj.c). */
/* pilha de uma thread que terminou (obj.c thread_tramp): solta a reserva */
void vm_stack_free(struct vmreg* r) {
  if (!r) return;
  wlock();
  reg_unlink(r);
  wunlock();
  reg_free(r);
}

struct vmreg* vm_stack_alloc(size_t reserve, size_t commit) {
  struct vmreg* r;
  uint64_t va;
  reserve = (reserve + 0xFFFFu) & ~(size_t)0xFFFFu;
  commit = (commit + 0xFFFFu) & ~(size_t)0xFFFFu;
  if (commit > reserve) commit = reserve;
  va = k_vm_reserve(reserve, 0x10000, 0);
  if (va && !k_vm_commit(va + reserve - commit, commit, prot_k(0x04u /* PAGE_READWRITE */))) {
    k_vm_release(va);
    va = 0;
  }
  if (!va) return vm_reserve(commit, VM_STACK, 2); /* sem memoria virtual: so o commit, em RAM */
  r = (struct vmreg*)wzalloc(sizeof(*r));
  if (!r) {
    k_vm_release(va);
    return 0;
  }
  r->base = (uint8_t*)(uintptr_t)va;
  r->size = reserve;
  r->kind = VM_STACK;
  r->virt = 1;
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

static void vm_oom_log(const char* what, uint64_t n) {
  void k_hminfo(uint64_t* quota, uint64_t* used, uint64_t* total_ram);
  uint64_t q = 0, u = 0, t = 0;
  unsigned lt = 0, lf = 0;
  wlogx(what, n);
  k_hminfo(&q, &u, &t);
  k_meminfo(&lt, &lf);
  wlogx("  HIMEM cota (MB) ", q >> 20);
  wlogx("  HIMEM usado (MB) ", u >> 20);
  wlogx("  RAM total (MB) ", t >> 20);
  wlogx("  arena baixa livre (MB) ", (uint64_t)(lf >> 20));
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
    r = 0;
    /* memoria virtual primeiro (quadros do heap alto, pagina a pagina): a
     * arena abaixo de 4 GB acabava com o PartitionAlloc/V8 de 4 processos
     * do Chromium. Reserva sem commit grande: so enderecos. */
    r = vm_reserve_virt(0, n, type, prot);
    if (!r) r = vm_reserve(n, VM_PRIVATE, (type & MEM_COMMIT) != 0);
    if (!r) r = vm_reserve_virt(0, n, type, prot); /* nao cabe na RAM de uma vez */
    if (!r) {
      vm_oom_log("VirtualAlloc: sem memoria para ", n);
      wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
      return 0;
    }
    return r->base;
  }
  wlock();
  r = vm_find(addr);
  if (r && (type & MEM_RESERVE)) {
    /* reservar onde ja ha reserva falha no Windows. O V8 tenta enderecos
     * aleatorios e, se isso "desse certo", tomava um pedaco de outra reserva
     * como seu (CHECK no VirtualFree, de vez em quando). */
    wunlock();
    if (wine_debug >= 2) wlogx("VirtualAlloc: reserva sobre reserva em ", (uint64_t)(uintptr_t)addr);
    wset_lasterr(ERROR_INVALID_ADDRESS);
    return 0;
  }
  if (r) {
    uint8_t* a = (uint8_t*)((uintptr_t)addr & ~(uintptr_t)(PG - 1));
    size_t len = (size_t)((uint8_t*)addr - a) + n;
    if (a + len > r->base + r->size) {
      wunlock();
      wlogx("VirtualAlloc: commit alem da reserva em ", (uint64_t)(uintptr_t)addr);
      wlogx("  tamanho ", n);
      wset_lasterr(ERROR_INVALID_ADDRESS);
      return 0;
    }
    if (r->virt) {
      int ok = 1;
      wunlock();
      if (type & MEM_COMMIT) ok = k_vm_commit((uint64_t)(uintptr_t)a, len, prot_k(prot));
      if (!ok) {
        vm_oom_log("VirtualAlloc: commit sem memoria para ", len);
        wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
      }
      return a;
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
  /* endereco pedido fora de tudo: a regiao virtual atende se estiver livre */
  if ((type & MEM_RESERVE) && !((uintptr_t)addr & 0xFFFF)) {
    r = vm_reserve_virt((uintptr_t)addr, n, type, prot);
    if (r) return r->base;
  }
  if (n >= 0x10000 || !(type & MEM_RESERVE)) {
    wlogx("VirtualAlloc: endereco recusado ", (uint64_t)(uintptr_t)addr);
    wlogx("  tamanho ", n);
    wlogx("  tipo ", type);
  }
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
    wlogx("VirtualFree: endereco fora de reserva ", (uint64_t)(uintptr_t)addr);
    wlogx("  tipo ", type);
    wset_lasterr(ERROR_INVALID_ADDRESS);
    return 0;
  }
  if (r->virt) {
    if (type & MEM_DECOMMIT) {
      wunlock();
      return k_vm_decommit((uint64_t)(uintptr_t)addr, n);
    }
    if (!(type & MEM_RELEASE) || r->base != (uint8_t*)addr) {
      wunlock();
      wlogx("VirtualFree(virtual): release fora do inicio ", (uint64_t)(uintptr_t)addr);
      wlogx("  inicio da reserva ", (uint64_t)(uintptr_t)r->base);
      wlogx("  tipo ", type);
      wset_lasterr(ERROR_INVALID_PARAMETER);
      return 0;
    }
    reg_unlink(r);
    wunlock();
    reg_free(r);
    return 1;
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
    wlogx("VirtualFree: release fora do inicio da reserva ", (uint64_t)(uintptr_t)addr);
    wlogx("  inicio da reserva ", (uint64_t)(uintptr_t)r->base);
    wlogx("  tipo ", type);
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

/* Protecao pedida em RAM de identidade (imagem, arena baixa): a pagina
 * continua gravavel de fato, mas o VirtualQuery responde o que o programa
 * pediu. O Chromium confere (base::ProtectedMemory: CHECK de que a secao
 * ficou PAGE_READONLY). Ultima entrada que cobre o endereco vale. */
#define PROT_MAX 512
static struct {
  uintptr_t lo, hi;
  DWORD prot;
} prot_tab[PROT_MAX];
static int prot_n;
static int prot_find(uintptr_t a, uintptr_t* lo, uintptr_t* hi) {
  int i;
  for (i = prot_n - 1; i >= 0; i--)
    if (a >= prot_tab[i].lo && a < prot_tab[i].hi) {
      if (lo) *lo = prot_tab[i].lo;
      if (hi) *hi = prot_tab[i].hi;
      return i;
    }
  return -1;
}
static void prot_set(uintptr_t lo, uintptr_t hi, DWORD prot) {
  int i;
  for (i = 0; i < prot_n; i++) /* mesmo intervalo: troca */
    if (prot_tab[i].lo == lo && prot_tab[i].hi == hi) {
      prot_tab[i].prot = prot;
      return;
    }
  if (prot_n == PROT_MAX) { /* cheia: descarta a mais antiga */
    memmove(&prot_tab[0], &prot_tab[1], sizeof(prot_tab[0]) * (PROT_MAX - 1));
    prot_n--;
  }
  prot_tab[prot_n].lo = lo;
  prot_tab[prot_n].hi = hi;
  prot_tab[prot_n].prot = prot;
  prot_n++;
}

/* pe.c: protecao de cada secao da imagem, pelas caracteristicas do PE */
void vm_image_prot(uint8_t* a, size_t n, DWORD characteristics) {
  int x = (characteristics & 0x20000000u) != 0, w = (characteristics & 0x80000000u) != 0;
  DWORD prot = x ? (w ? 0x40u : 0x20u) : (w ? 0x04u : 0x02u);
  uintptr_t lo = (uintptr_t)a & ~(uintptr_t)(PG - 1);
  uintptr_t hi = ((uintptr_t)a + n + PG - 1) & ~(uintptr_t)(PG - 1);
  if (hi <= lo) return;
  wlock();
  prot_set(lo, hi, prot);
  wunlock();
}

static WINAPI BOOL VirtualProtect(void* a, size_t n, DWORD prot, DWORD* old) {
  struct vmreg* r;
  int virt;
  wlock();
  r = vm_find(a);
  virt = r && r->virt;
  wunlock();
  if (virt) {
    int o = 2;
    if (!k_vm_protect((uint64_t)(uintptr_t)a, n, prot_k(prot), &o)) {
      wset_lasterr(ERROR_INVALID_ADDRESS);
      return 0;
    }
    if (old) *old = prot_w(o);
    return 1;
  }
  /* RAM em identidade: sem protecao por pagina; so lembra o pedido */
  {
    uintptr_t lo = (uintptr_t)a & ~(uintptr_t)(PG - 1);
    uintptr_t hi = ((uintptr_t)a + n + PG - 1) & ~(uintptr_t)(PG - 1);
    int k;
    wlock();
    k = prot_find((uintptr_t)a, 0, 0);
    if (old) *old = k >= 0 ? prot_tab[k].prot : (r && r->kind == VM_IMAGE ? 0x40u : 4u);
    if (hi > lo) prot_set(lo, hi, prot);
    wunlock();
  }
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

/* Da para ler [p, p+n)? A memoria virtual (de 1 TB para cima) pode estar so
 * reservada; a de baixo e mapeada 1:1. Para logs que testam ponteiros
 * quaisquer (argumentos de metodos COM, por exemplo). */
int wmem_readable(const void* p, size_t n) {
  uint64_t a = (uint64_t)(uintptr_t)p, e;
  struct vmem_info vi;
  if (!wptr_ok(p) || !n) return 0;
  e = a + n - 1;
  if (a < 0x10000000000ull) return e < 0x10000000000ull;
  if (!k_vm_query(a & ~0xFFFull, &vi) || !vi.committed) return 0;
  if ((e & ~0xFFFull) != (a & ~0xFFFull) && (!k_vm_query(e & ~0xFFFull, &vi) || !vi.committed)) return 0;
  return 1;
}

/* Para o relatorio de excecao: estado da pagina de um endereco >= 1 TB. */
void wvm_note(uint64_t a) {
  struct vmem_info vi;
  if (a < 0x10000000000ull) return;
  if (!k_vm_query(a & ~0xFFFull, &vi)) {
    wlog("  pagina: fora de qualquer reserva", 0);
    return;
  }
  wlogx("  pagina: reserva em ", vi.alloc_base);
  wlogx("  pagina: commitada ", (uint64_t)vi.committed);
  wlogx("  pagina: protecao (0 nada, 1 leitura, 2 leitura e escrita) ", (uint64_t)vi.prot);
}

static WINAPI size_t VirtualQuery(const void* addr, struct mbi* m, size_t len) {
  uintptr_t pg = (uintptr_t)addr & ~(uintptr_t)(PG - 1);
  struct vmreg* r;
  if (len < sizeof(*m)) return 0;
  memset(m, 0, sizeof(*m));
  m->base = (void*)pg;
  wlock();
  r = vm_find(addr);
  if (!r) {
    int wview_query(const void* p, uint64_t* base, uint64_t* size, int* write); /* map.c */
    uint64_t vb = 0, vs = 0;
    int vw = 0;
    wunlock();
    if (wview_query(addr, &vb, &vs, &vw)) { /* vista de secao (MapViewOfFile) */
      m->alloc_base = (void*)(uintptr_t)vb;
      m->alloc_protect = vw ? 4 : 2;
      m->region = vb + vs - pg;
      m->state = MEM_COMMIT;
      m->protect = vw ? 4 : 2;
      m->type = 0x40000; /* MEM_MAPPED */
      return sizeof(*m);
    }
    m->region = 0x10000;
    m->state = 0x10000; /* MEM_FREE */
    m->protect = 1;     /* PAGE_NOACCESS */
    return sizeof(*m);
  }
  if (r->virt) {
    struct vmem_info vi;
    wunlock();
    if (!k_vm_query(pg, &vi)) {
      m->region = PG;
      m->state = 0x10000;
      m->protect = 1;
      return sizeof(*m);
    }
    m->alloc_base = (void*)(uintptr_t)vi.alloc_base;
    m->alloc_protect = 4;
    m->region = vi.size;
    m->state = vi.committed ? MEM_COMMIT : MEM_RESERVE;
    m->protect = vi.committed ? prot_w(vi.prot) : 0;
    m->type = 0x20000; /* MEM_PRIVATE */
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
    if (c) { /* VirtualProtect pedido neste trecho */
      uintptr_t lo, hi;
      int k = prot_find(pg, &lo, &hi);
      if (k >= 0) {
        m->protect = prot_tab[k].prot;
        if (hi - pg < m->region) m->region = hi - pg;
      }
    }
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
