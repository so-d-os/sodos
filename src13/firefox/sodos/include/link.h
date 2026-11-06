/* link.h (SODOS) - declaracoes; o SODOS nao tem ligador dinamico ELF. */
#ifndef _LINK_H
#define _LINK_H
#include <stddef.h>
#include <elf.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ElfW(type) Elf64_##type
#define __WORDSIZE 64
struct link_map {
  ElfW(Addr) l_addr;
  char* l_name;
  ElfW(Dyn)* l_ld;
  struct link_map* l_next;
  struct link_map* l_prev;
};
struct dl_phdr_info {
  ElfW(Addr) dlpi_addr;
  const char* dlpi_name;
  const ElfW(Phdr)* dlpi_phdr;
  ElfW(Half) dlpi_phnum;
};
int dl_iterate_phdr(int (*cb)(struct dl_phdr_info*, size_t, void*), void* data);
#ifdef __cplusplus
}
#endif
#endif
