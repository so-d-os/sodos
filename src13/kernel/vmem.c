/*
 * vmem.c - Memoria virtual de verdade para os programas do Windows (W4).
 *
 * O SODOS roda em identidade: o PML4[0] cobre os primeiros 512 GB com paginas
 * de 2 MB/1 GB e todo endereco e fisico. Programas como o V8 (Chromium,
 * Electron) reservam 4 GB, 64 GB ou mais de ENDERECOS e so usam uma parte;
 * reservar isso em RAM e impossivel.
 *
 * Aqui fica uma regiao a partir de 1 TB (PML4[2] em diante), fora da
 * identidade, com tabelas de paginas de 4 KB montadas sob demanda:
 *
 *   reserva   so separa o intervalo de enderecos (nenhuma pagina)
 *   commit    mapeia quadros fisicos zerados (de 2 MB em 2 MB do heap alto)
 *   protect   liga/desliga presente e escrita; a pagina continua do dono
 *   decommit  devolve os quadros; release devolve a reserva inteira
 *
 * O CR3 e um so (todas as CPUs e tarefas): as reservas de processos
 * diferentes ficam em intervalos diferentes e o dono (pid) e guardado para
 * soltar tudo quando o processo termina. Sem nenhuma reserva viva, quadros e
 * tabelas voltam ao heap e as entradas do PML4 sao apagadas: com o WINE
 * desligado o custo e zero.
 *
 * Bit 9 da PTE (livre para o SO) = "commitada": a pagina tem quadro mesmo
 * quando nao esta presente (PAGE_NOACCESS / PAGE_GUARD).
 */
#include <stdint.h>
#include "vmem.h"
#include "mm.h"
#include "kthread.h"
#include "serial.h"

#define VM_BASE 0x0000010000000000ull /* 1 TB */
#define VM_END 0x00007F0000000000ull
#define PG 0x1000ull
#define PA_MASK 0x000FFFFFFFFFF000ull
#define PTE_P 0x1ull
#define PTE_W 0x2ull
#define PTE_C 0x200ull /* commitada (software) */
#define PTE_ALIAS 0x400ull /* quadro de outro dono (vmem_alias): nao volta para a lista */
#define CHUNK (2ull << 20)

/* ---------- reservas ---------- */
struct vres {
  uint64_t base, size;
  uint32_t pid;
  int used;
};
#define MAX_RES 4096
static struct vres* res;      /* tabela (umalloc), so existe com reservas vivas */
static int nres;

/* ---------- quadros e tabelas ---------- */
struct chunk {
  void* raw;
  int high;
};
#define MAX_CHUNKS 8192 /* 16 GB de quadros */
static struct chunk* chunks;
static int nchunks;
static uint64_t free_frames;  /* lista: o 1o uint64 do quadro aponta o proximo */
static uint64_t free_tables;  /* idem, quadros abaixo de 4 GB (tabelas) */
static uint64_t frames_used, tables_used;

/* Memoria do kernel (dono 0): o umalloc comum fica no nome do processo atual
 * e o exec_release a soltaria (ufree_owner) antes do vmem_release_pid. */
static void* kmem(uint32_t n) { return umalloc_owned(n, 0); }

static void zero_page(uint64_t pa) {
  uint64_t* p = (uint64_t*)(uintptr_t)pa;
  int i;
  for (i = 0; i < 512; i++) p[i] = 0;
}

/* um bloco de 2 MB vira 512 quadros na lista escolhida */
static int grow(int low) {
  void* raw = 0;
  int high = 0;
  uint64_t a, i;
  if (!chunks) {
    chunks = (struct chunk*)kmem(sizeof(struct chunk) * MAX_CHUNKS);
    if (!chunks) return 0;
  }
  if (nchunks >= MAX_CHUNKS) return 0;
  if (!low) {
    raw = hmalloc(CHUNK + PG);
    high = raw != 0;
  }
  if (!raw) raw = kmem((uint32_t)(low ? (256u << 10) + PG : CHUNK + PG));
  if (!raw) return 0;
  chunks[nchunks].raw = raw;
  chunks[nchunks].high = high;
  nchunks++;
  a = ((uint64_t)(uintptr_t)raw + PG - 1) & ~(PG - 1);
  for (i = 0; i < (low ? (256u << 10) : CHUNK) / PG; i++) {
    uint64_t f = a + i * PG;
    uint64_t* list = low ? &free_tables : &free_frames;
    *(uint64_t*)(uintptr_t)f = *list;
    *list = f;
  }
  return 1;
}
static uint64_t frame_get(void) {
  uint64_t f;
  if (!free_frames && !grow(0)) return 0;
  f = free_frames;
  free_frames = *(uint64_t*)(uintptr_t)f;
  zero_page(f);
  frames_used++;
  return f;
}
static void frame_put(uint64_t f) {
  *(uint64_t*)(uintptr_t)f = free_frames;
  free_frames = f;
  frames_used--;
}
/* tabelas ficam abaixo de 4 GB (o despejo de falhas do isr.c so le ali) */
static uint64_t table_get(void) {
  uint64_t f;
  if (!free_tables && !grow(1)) return 0;
  f = free_tables;
  free_tables = *(uint64_t*)(uintptr_t)f;
  zero_page(f);
  tables_used++;
  return f;
}

/* TLB desta CPU (recarrega o CR3) e das outras (IPI) */
static void tlb_flush(void) {
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
  kt_tlb_shootdown();
}

static uint64_t* pml4(void) {
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  return (uint64_t*)(uintptr_t)(cr3 & PA_MASK);
}
/* PTE de va; create = monta as tabelas que faltarem */
static uint64_t* pte_of(uint64_t va, int create) {
  uint64_t* t = pml4();
  int sh;
  for (sh = 39; sh > 12; sh -= 9) {
    uint64_t* e = &t[(va >> sh) & 511];
    if (!(*e & PTE_P)) {
      uint64_t n;
      if (!create) return 0;
      n = table_get();
      if (!n) return 0;
      *e = n | PTE_P | PTE_W;
    }
    t = (uint64_t*)(uintptr_t)(*e & PA_MASK);
  }
  return &t[(va >> 12) & 511];
}

/* tudo solto: devolve quadros, tabelas e as entradas do PML4 */
static void drop_all(void) {
  uint64_t* t = pml4();
  int i;
  for (i = (int)(VM_BASE >> 39); i <= (int)((VM_END - 1) >> 39); i++) t[i] = 0;
  tlb_flush();
  for (i = 0; i < nchunks; i++) {
    if (chunks[i].high) hfree(chunks[i].raw);
    else ufree(chunks[i].raw);
  }
  nchunks = 0;
  free_frames = free_tables = 0;
  frames_used = tables_used = 0;
  if (chunks) ufree(chunks);
  chunks = 0;
  if (res) ufree(res);
  res = 0;
  nres = 0;
  serial_puts("[vmem] sem reservas: quadros e tabelas devolvidos\n");
}

static struct vres* res_of(uint64_t va) {
  int i;
  for (i = 0; i < nres; i++)
    if (res[i].used && va >= res[i].base && va < res[i].base + res[i].size) return &res[i];
  return 0;
}
static int live(void) {
  int i, n = 0;
  for (i = 0; i < nres; i++) n += res[i].used;
  return n;
}

static int res_ready(void) {
  int i;
  if (res) return 1;
  res = (struct vres*)kmem(sizeof(struct vres) * MAX_RES);
  if (!res) return 0;
  for (i = 0; i < MAX_RES; i++) res[i].used = 0;
  return 1;
}

uint64_t vmem_reserve(uint64_t size, uint64_t align, uint32_t pid) {
  uint64_t a, best = 0;
  int i, slot = -1;
  if (!size) return 0;
  if (align < 0x10000) align = 0x10000;
  size = (size + 0xFFFF) & ~0xFFFFull;
  if (!res_ready()) return 0;
  /* primeiro buraco: comeca no inicio e pula reservas que encostam */
  a = VM_BASE;
  for (;;) {
    int hit = 0;
    a = (a + align - 1) & ~(align - 1);
    if (a + size > VM_END || a + size < a) return 0;
    for (i = 0; i < nres; i++)
      if (res[i].used && a < res[i].base + res[i].size && res[i].base < a + size) {
        a = res[i].base + res[i].size;
        hit = 1;
        break;
      }
    if (!hit) {
      best = a;
      break;
    }
  }
  for (i = 0; i < nres; i++)
    if (!res[i].used) {
      slot = i;
      break;
    }
  if (slot < 0) {
    if (nres >= MAX_RES) return 0;
    slot = nres++;
  }
  res[slot].base = best;
  res[slot].size = size;
  res[slot].pid = pid;
  res[slot].used = 1;
  return best;
}

/* reserva num endereco pedido (dentro da regiao, sem sobrepor outra) */
uint64_t vmem_reserve_at(uint64_t va, uint64_t size, uint32_t pid) {
  int i, slot = -1;
  if (!size || (va & 0xFFFF) || va < VM_BASE || va + size > VM_END) return 0;
  size = (size + 0xFFFF) & ~0xFFFFull;
  if (!res_ready()) return 0;
  for (i = 0; i < nres; i++)
    if (res[i].used && va < res[i].base + res[i].size && res[i].base < va + size) return 0;
  for (i = 0; i < nres; i++)
    if (!res[i].used) {
      slot = i;
      break;
    }
  if (slot < 0) {
    if (nres >= MAX_RES) return 0;
    slot = nres++;
  }
  res[slot].base = va;
  res[slot].size = size;
  res[slot].pid = pid;
  res[slot].used = 1;
  return va;
}

int vmem_owns(uint64_t va) { return va >= VM_BASE && va < VM_END && res && res_of(va) != 0; }

static uint64_t prot_bits(int prot) {
  if (prot == VMEM_NOACCESS) return 0;
  if (prot == VMEM_READ) return PTE_P;
  return PTE_P | PTE_W;
}

int vmem_commit(uint64_t va, uint64_t size, int prot) {
  struct vres* r;
  uint64_t a, end;
  if (!res) return 0;
  r = res_of(va);
  end = (va + size + PG - 1) & ~(PG - 1);
  va &= ~(PG - 1);
  if (!r || end > r->base + r->size) return 0;
  for (a = va; a < end; a += PG) {
    uint64_t* e = pte_of(a, 1);
    if (!e) return 0;
    if (*e & PTE_C) {
      /* ja commitada: so a protecao muda (como no Windows) */
      *e = (*e & ~(PTE_P | PTE_W)) | prot_bits(prot);
      continue;
    }
    {
      uint64_t f = frame_get();
      if (!f) return 0;
      *e = f | PTE_C | prot_bits(prot);
    }
  }
  tlb_flush();
  return 1;
}

/* Liga [va, va+size) de uma reserva a memoria fisica ja existente (phys, que
 * aqui e o proprio endereco: a memoria baixa e mapeada 1:1). Para a segunda
 * vista de uma secao no mesmo processo (MapViewOfFile): no Windows cada vista
 * tem endereco proprio, e o Chromium indexa os mapeamentos pelo endereco. */
int vmem_alias(uint64_t va, uint64_t phys, uint64_t size, int prot) {
  struct vres* r;
  uint64_t a, end;
  if (!res || (va & (PG - 1)) || (phys & (PG - 1))) return 0;
  r = res_of(va);
  end = (va + size + PG - 1) & ~(PG - 1);
  if (!r || end > r->base + r->size) return 0;
  for (a = va; a < end; a += PG) {
    uint64_t* e = pte_of(a, 1);
    if (!e || *e) return 0;
    *e = (phys + (a - va)) | PTE_C | PTE_ALIAS | prot_bits(prot);
  }
  tlb_flush();
  return 1;
}

int vmem_decommit(uint64_t va, uint64_t size) {
  struct vres* r;
  uint64_t a, end;
  if (!res) return 0;
  r = res_of(va);
  if (!r) return 0;
  if (!size) {
    va = r->base;
    end = r->base + r->size;
  } else {
    end = (va + size + PG - 1) & ~(PG - 1);
    va &= ~(PG - 1);
    if (end > r->base + r->size) return 0;
  }
  for (a = va; a < end; a += PG) {
    uint64_t* e = pte_of(a, 0);
    if (!e) {
      a = (a | 0x1FFFFF) + 1 - PG; /* sem tabela: pula o trecho de 2 MB */
      continue;
    }
    if ((*e & PTE_C) && (*e & PTE_ALIAS)) {
      /* so a ligacao some: o quadro e de quem o emprestou */
    } else if ((*e & PTE_C) && (*e & PA_MASK) < (1ull << 40)) {
      frame_put(*e & PA_MASK);
    } else if (*e) {
      serial_puts("[vmem] PTE estranha em ");
      serial_hex32((uint32_t)(a >> 32));
      serial_hex32((uint32_t)a);
      serial_puts(" = ");
      serial_hex32((uint32_t)(*e >> 32));
      serial_hex32((uint32_t)*e);
      serial_puts("\n");
    }
    *e = 0;
  }
  tlb_flush();
  return 1;
}

int vmem_release(uint64_t va) {
  struct vres* r;
  if (!res) return 0;
  r = res_of(va);
  if (!r || r->base != va) return 0;
  vmem_decommit(r->base, r->size);
  r->used = 0;
  if (!live()) drop_all();
  return 1;
}

int vmem_protect(uint64_t va, uint64_t size, int prot, int* old) {
  struct vres* r;
  uint64_t a, end;
  int first = 1;
  if (!res) return 0;
  r = res_of(va);
  end = (va + size + PG - 1) & ~(PG - 1);
  va &= ~(PG - 1);
  if (!r || end > r->base + r->size) return 0;
  for (a = va; a < end; a += PG) {
    uint64_t* e = pte_of(a, 0);
    if (!e || !(*e & PTE_C)) return 0; /* Windows: so paginas commitadas */
    if (first && old) *old = !(*e & PTE_P) ? VMEM_NOACCESS : (*e & PTE_W) ? VMEM_RW : VMEM_READ;
    first = 0;
    *e = (*e & ~(PTE_P | PTE_W)) | prot_bits(prot);
  }
  tlb_flush();
  return 1;
}

/* Estado de va e o tamanho do trecho igual a partir dele (VirtualQuery). */
int vmem_query(uint64_t va, struct vmem_info* out) {
  struct vres* r;
  uint64_t a, st0 = 0, end;
  int first = 1;
  if (!res || !out) return 0;
  r = res_of(va);
  if (!r) return 0;
  va &= ~(PG - 1);
  out->alloc_base = r->base;
  out->base = va;
  end = r->base + r->size;
  for (a = va; a < end;) {
    uint64_t* e = pte_of(a, 0);
    uint64_t st = e ? (*e & (PTE_C | PTE_P | PTE_W)) : 0;
    if (!(st & PTE_C)) st = 0;
    if (first) {
      st0 = st;
      first = 0;
    } else if (st != st0) {
      break;
    }
    /* sem tabela: o trecho de 2 MB inteiro esta so reservado */
    a = e ? a + PG : (a | 0x1FFFFF) + 1;
  }
  if (a > end) a = end;
  out->size = a - va;
  out->committed = (st0 & PTE_C) != 0;
  out->prot = !out->committed ? VMEM_NOACCESS
              : !(st0 & PTE_P) ? VMEM_NOACCESS
              : (st0 & PTE_W) ? VMEM_RW
                              : VMEM_READ;
  return 1;
}

void vmem_release_pid(uint32_t pid) {
  int i, n = 0;
  if (!res) return;
  for (i = 0; i < nres; i++)
    if (res[i].used && res[i].pid == pid) {
      vmem_decommit(res[i].base, res[i].size);
      res[i].used = 0;
      n++;
    }
  if (n && !live()) drop_all();
}

void vmem_stats(uint64_t* reserved, uint64_t* committed) {
  int i;
  uint64_t r = 0;
  for (i = 0; res && i < nres; i++)
    if (res[i].used) r += res[i].size;
  if (reserved) *reserved = r;
  if (committed) *committed = frames_used * PG;
}
