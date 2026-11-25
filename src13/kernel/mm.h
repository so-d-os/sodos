/* mm.h - Heap do kernel (2 MB em 0xA80000) e heap alto (acima de 16 MB)
 * para programas e DLLs. */
#pragma once
#include <stdint.h>

void mm_init(void);
/* Paginas de 2 MB de [phys, phys+size) viram write-combining (LFB). */
int mm_map_wc(uint64_t phys, uint64_t size);
/* Uma pagina de 4 KB em va passa a ser pa (um alias por vez); undo desfaz. */
int mm_alias_page(uint64_t va, uint64_t pa);
void mm_alias_undo(void);
/* 1 = os primeiros 4 KB saem do mapa (NULL falha), 0 = voltam */
void mm_null_guard(int on);
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
/* Bloco do kernel (dono 0) tirado do fim da arena: para caches que ficam. */
void* umalloc_top(uint32_t nbytes);
void* umalloc_aligned(uint32_t nbytes, uint32_t align);
/* Bloco no endereco addr (multiplo de 16) se a faixa estiver livre; 0 se nao. */
void* umalloc_at(uint32_t addr, uint32_t nbytes, uint32_t owner);
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
/* RAM acima de 4 GB para programas de 64 bits (k_hmalloc): teto em % da RAM
 * total (padrao 40, SET HIMEM=pct), blocos com dono soltos no fim do programa. */
void* hm_user_alloc(uint64_t nbytes, uint64_t align, uint32_t owner);
void hm_user_free(void* ptr);
void hm_free_owner(uint32_t owner);
void hm_set_user_pct(uint32_t pct);
/* MTRR: a BSP corrige no boot (TOM2 WB na AMD); cada AP repete. */
void mm_mtrr_ap(void);
void mm_mtrr_info(uint64_t* wb, uint64_t* uc, uint64_t* other, int* fixed);
void hm_user_info(uint64_t* quota, uint64_t* used, uint64_t* total_ram);

/* Percorre todos os blocos para o TOP. area: 0 = heap do kernel, 1 = arena
 * alta, 2 = acima de 4 GB. owner so vale para a arena alta. */
typedef void (*mm_walk_fn)(void* arg, int area, uint64_t addr, uint64_t size, int used,
                           uint32_t owner);
void mm_walk(mm_walk_fn fn, void* arg);

/* Confere a cadeia de blocos de uma area do jeito que o mm_walk, o TOP e os
 * alocadores andam nela (FREE). area: 0 = heap do kernel, 1 = arena alta,
 * 2 + i = regiao i acima de 4 GB. Devolve 0 se a area nao existe, 1 se a
 * cadeia chega ao fim e -1 se para antes: um cabecalho estragado em stop
 * deixa a RAM dali ate o fim fora de tudo (nem livre, nem usada, nem solta
 * quando o programa sai) ate o proximo boot. */
struct mm_chk {
  uint64_t start, end;    /* a area */
  uint64_t stop;          /* onde o percurso parou (= end se inteira) */
  uint64_t blocks;        /* blocos bons ate ali */
  uint64_t last;          /* ultimo bloco bom, o vizinho de antes da quebra (0: nenhum) */
  uint64_t last_size;
  uint32_t last_free, last_owner;
  uint64_t bad_size;      /* o cabecalho em stop, como o percurso o leu */
  uint32_t bad_free, bad_owner;
  int bad_len;            /* bytes copiados de stop (16, menos se o fim corta) */
  uint8_t bad_raw[16];
};
int mm_check(int area, struct mm_chk* c);
/* Fim da memoria ocupada pela imagem do kernel (com .bss). */
uint32_t mm_kernel_end(void);
