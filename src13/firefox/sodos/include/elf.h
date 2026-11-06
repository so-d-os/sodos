/* elf.h (SODOS) - tipos ELF; o SODOS carrega SOX, isto so serve para compilar. */
#ifndef _ELF_H
#define _ELF_H
#include <stdint.h>
typedef uint16_t Elf32_Half; typedef uint32_t Elf32_Word; typedef int32_t Elf32_Sword;
typedef uint32_t Elf32_Addr; typedef uint32_t Elf32_Off;
typedef uint16_t Elf64_Half; typedef uint32_t Elf64_Word; typedef int32_t Elf64_Sword;
typedef uint64_t Elf64_Xword; typedef int64_t Elf64_Sxword;
typedef uint64_t Elf64_Addr; typedef uint64_t Elf64_Off;
#define EI_NIDENT 16
#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_VERSION 6
#define ELFDATA2MSB 2
#define SHT_DYNAMIC 6
#define SHT_RELA 4
#define SHT_REL 9
#define SHT_HASH 5
#define SHF_ALLOC 2
#define EI_CLASS 4
#define EI_DATA 5
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EM_X86_64 62
#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_NOTE 4
#define PT_TLS 7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_RELRO 0x6474e552
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define SHT_DYNSYM 11
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHN_UNDEF 0
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
#define DT_REL 17
#define DT_RELSZ 18
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_GNU_HASH 0x6ffffef5
#define NT_GNU_BUILD_ID 3
#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xf)
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
typedef struct {
  unsigned char e_ident[EI_NIDENT];
  Elf64_Half e_type, e_machine;
  Elf64_Word e_version;
  Elf64_Addr e_entry;
  Elf64_Off e_phoff, e_shoff;
  Elf64_Word e_flags;
  Elf64_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;
typedef struct {
  Elf64_Word p_type, p_flags;
  Elf64_Off p_offset;
  Elf64_Addr p_vaddr, p_paddr;
  Elf64_Xword p_filesz, p_memsz, p_align;
} Elf64_Phdr;
typedef struct {
  Elf64_Word sh_name, sh_type;
  Elf64_Xword sh_flags;
  Elf64_Addr sh_addr;
  Elf64_Off sh_offset;
  Elf64_Xword sh_size;
  Elf64_Word sh_link, sh_info;
  Elf64_Xword sh_addralign, sh_entsize;
} Elf64_Shdr;
typedef struct {
  Elf64_Word st_name;
  unsigned char st_info, st_other;
  Elf64_Half st_shndx;
  Elf64_Addr st_value;
  Elf64_Xword st_size;
} Elf64_Sym;
typedef struct {
  Elf64_Sxword d_tag;
  union {
    Elf64_Xword d_val;
    Elf64_Addr d_ptr;
  } d_un;
} Elf64_Dyn;
typedef struct {
  Elf64_Addr r_offset;
  Elf64_Xword r_info;
  Elf64_Sxword r_addend;
} Elf64_Rela;
typedef struct {
  Elf64_Addr r_offset;
  Elf64_Xword r_info;
} Elf64_Rel;
typedef struct {
  Elf64_Word n_namesz, n_descsz, n_type;
} Elf64_Nhdr;
/* lote2 */
typedef uint16_t Elf32_Section;
typedef uint16_t Elf64_Section;
typedef uint16_t Elf32_Versym;
typedef uint16_t Elf64_Versym;
typedef struct {
  unsigned char e_ident[EI_NIDENT];
  Elf32_Half e_type, e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff, e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;
typedef struct {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr, p_paddr;
  Elf32_Word p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;
typedef struct {
  Elf32_Word sh_name, sh_type, sh_flags;
  Elf32_Addr sh_addr;
  Elf32_Off sh_offset;
  Elf32_Word sh_size, sh_link, sh_info, sh_addralign, sh_entsize;
} Elf32_Shdr;
typedef struct {
  Elf32_Word st_name;
  Elf32_Addr st_value;
  Elf32_Word st_size;
  unsigned char st_info, st_other;
  Elf32_Section st_shndx;
} Elf32_Sym;
typedef struct {
  Elf32_Word n_namesz, n_descsz, n_type;
} Elf32_Nhdr;
typedef struct {
  Elf32_Sword d_tag;
  union {
    Elf32_Word d_val;
    Elf32_Addr d_ptr;
  } d_un;
} Elf32_Dyn;
typedef struct {
  Elf64_Half vd_version, vd_flags, vd_ndx, vd_cnt;
  Elf64_Word vd_hash, vd_aux, vd_next;
} Elf64_Verdef;
typedef struct {
  Elf64_Word vda_name, vda_next;
} Elf64_Verdaux;
typedef struct {
  uint64_t a_type;
  union {
    uint64_t a_val;
  } a_un;
} Elf64_auxv_t;
#define EM_386 3
#define EM_SPARC 2
#define EM_MIPS 8
#define EM_PPC 20
#define EM_PPC64 21
#define EM_S390 22
#define EM_ARM 40
#define EM_SPARCV9 43
#define EM_AARCH64 183
#define ET_DYN 3
#define EV_CURRENT 1
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHT_GNU_verdef 0x6ffffffd
#define SHT_GNU_versym 0x6fffffff
#define SHN_ABS 0xfff1
#define SHN_LORESERVE 0xff00
#define STN_UNDEF 0
#define STT_TLS 6
#define STT_GNU_IFUNC 10
#define VER_NDX_LOCAL 0
#define VER_NDX_GLOBAL 1
#define VER_FLG_BASE 1
#define VERSYM_HIDDEN 0x8000
#define VERSYM_VERSION 0x7fff
#define DT_VERSYM 0x6ffffff0
#define DT_VERDEF 0x6ffffffc
#define DT_VERDEFNUM 0x6ffffffd
#define AT_NULL 0
#define AT_SYSINFO_EHDR 33
#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((i) & 0xff)

#endif
