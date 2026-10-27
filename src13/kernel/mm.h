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
