#ifndef VMEM_H
#define VMEM_H
#include <stdint.h>
/* Memoria virtual (reserva/commit) dos programas do Windows: ver vmem.c. */
enum { VMEM_NOACCESS = 0, VMEM_READ = 1, VMEM_RW = 2 };
struct vmem_info {
  uint64_t base, size, alloc_base;
  int committed, prot;
};
uint64_t vmem_reserve(uint64_t size, uint64_t align, uint32_t pid);
uint64_t vmem_reserve_at(uint64_t va, uint64_t size, uint32_t pid);
int vmem_owns(uint64_t va);
uint32_t vmem_owner(uint64_t va);
int vmem_commit(uint64_t va, uint64_t size, int prot);
int vmem_decommit(uint64_t va, uint64_t size);
int vmem_alias(uint64_t va, uint64_t phys, uint64_t size, int prot);
int vmem_release(uint64_t va);
int vmem_protect(uint64_t va, uint64_t size, int prot, int* old);
int vmem_query(uint64_t va, struct vmem_info* out);
void vmem_release_pid(uint32_t pid);
void vmem_stats(uint64_t* reserved, uint64_t* committed);
#endif
