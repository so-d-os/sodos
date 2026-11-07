/* mm.h - Heap do kernel (2 MB em 0xA80000) e heap alto (acima de 16 MB)
 * para programas e DLLs. */
#pragma once
#include <stdint.h>

void mm_init(void);
void* kmalloc(uint32_t nbytes);
void kfree(void* ptr);
int mm_ready(void);
uint32_t mm_heap_start(void);
uint32_t mm_heap_end(void);
void mm_stats(uint32_t* total, uint32_t* used, uint32_t* free_bytes, uint32_t* largest);

/* Cada bloco alto guarda um dono (id do programa). 0 = kernel/DLL compartilhada. */
int um_ready(void);
uint32_t um_start(void);
uint32_t um_end(void);
void um_set_owner(uint32_t owner);
uint32_t um_owner(void);
void* umalloc(uint32_t nbytes);
void* umalloc_owned(uint32_t nbytes, uint32_t owner);
void* urealloc(void* ptr, uint32_t nbytes);
void ufree(void* ptr);
uint32_t usize(void* ptr);
/* Libera tudo que o programa 'owner' deixou alocado ao sair. */
void ufree_owner(uint32_t owner);
void um_stats(uint32_t* total, uint32_t* used, uint32_t* free_bytes, uint32_t* largest);

/* RAM acima de 4 GB, mapeada em identidade por mm_init.  Ponteiros de 64 bits:
 * so o kernel usa (programas x32 nao alcancam esses enderecos). */
int hm_ready(void);
void* hmalloc(uint64_t nbytes);
void hfree(void* ptr);
void hm_stats(uint64_t* total, uint64_t* used, uint64_t* free_bytes, uint64_t* largest);
uint32_t hm_regions(void);
uint64_t hm_region_start(uint32_t i);
uint64_t hm_region_end(uint32_t i);

/* Percorre todos os blocos para o TOP. area: 0 = heap do kernel, 1 = arena
 * alta, 2 = acima de 4 GB. owner so vale para a arena alta. */
typedef void (*mm_walk_fn)(void* arg, int area, uint64_t addr, uint64_t size, int used,
                           uint32_t owner);
void mm_walk(mm_walk_fn fn, void* arg);
/* Fim da memoria ocupada pela imagem do kernel (com .bss). */
uint32_t mm_kernel_end(void);
