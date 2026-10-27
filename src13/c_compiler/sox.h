/* sox.h - Formatos binarios do SO: SOX (.EXE/.DLL), COBJ (.OBJ), CLB (.LIB).
 * Usado pelo kernel (loader), pelo cc e pelo link. Tudo little-endian, u32. */
#ifndef SOX_H
#define SOX_H

/* ---------- SOX: executavel ou DLL relocavel ----------
 * Arquivo: sox_hdr | imagem (text+data) | relocs | imports | exports | strtab
 * A imagem e montada no endereco 0. O loader soma a base a cada u32 listado
 * em relocs e grava em cada 'slot' de import o endereco da funcao.
 * Chamadas a imports passam por um thunk 'jmp [slot]' gerado pelo link. */
#define SOX_MAGIC 0x31584F53u /* "SOX1" */
#define SOX_FLAG_DLL 1u
#define SOX_NO_ENTRY 0xFFFFFFFFu

struct sox_hdr {
  unsigned int magic;
  unsigned int flags;
  unsigned int image_size;   /* bytes da imagem no arquivo */
  unsigned int bss_size;     /* zeros depois da imagem */
  unsigned int entry;        /* EXE: _start(argc, argv); DLL: DllMain(motivo) */
  unsigned int text_size;
  unsigned int stack_size;   /* 0 = padrao do loader */
  unsigned int nrelocs;
  unsigned int off_relocs;
  unsigned int nimports;
  unsigned int off_imports;
  unsigned int nexports;
  unsigned int off_exports;
  unsigned int strtab_size;
  unsigned int off_strtab;
  unsigned int off_image;
};

struct sox_import {
  unsigned int lib;   /* strtab: "KERNEL", "LIBC.DLL" ... */
  unsigned int name;  /* strtab: nome da funcao */
  unsigned int slot;  /* offset na imagem do ponteiro a preencher */
};

struct sox_export {
  unsigned int name;  /* strtab */
  unsigned int value; /* offset na imagem */
};

/* DllMain(motivo) */
#define SOX_DLL_ATTACH 1
#define SOX_DLL_DETACH 0

/* ---------- COBJ: objeto gerado pelo cc ----------
 * Arquivo: cob_hdr | text | data | syms | relocs | strtab */
#define COB_MAGIC 0x31424F43u /* "COB1" */

#define COB_SECT_UNDEF 0
#define COB_SECT_TEXT  1
#define COB_SECT_DATA  2
#define COB_SECT_BSS   3

#define COB_SYM_GLOBAL 1u
#define COB_SYM_EXPORT 2u

/* Mesma semantica de R_386_32 e R_386_PC32 (addend no proprio campo). */
#define COB_REL_ABS32 1u
#define COB_REL_REL32 2u

struct cob_hdr {
  unsigned int magic;
  unsigned int text_size;
  unsigned int data_size;
  unsigned int bss_size;
  unsigned int nsyms;
  unsigned int nrelocs;
  unsigned int strtab_size;
};

struct cob_sym {
  unsigned int name;
  unsigned int sect;
  unsigned int value;
  unsigned int flags;
};

struct cob_rel {
  unsigned int sect;   /* secao onde esta o campo: TEXT ou DATA */
  unsigned int offset;
  unsigned int sym;    /* indice em syms */
  unsigned int type;
};

/* ---------- CLB: biblioteca estatica (varios .OBJ) ----------
 * Arquivo: clb_hdr | (clb_ent + bytes do objeto) * count */
#define CLB_MAGIC 0x31424C43u /* "CLB1" */

struct clb_hdr {
  unsigned int magic;
  unsigned int count;
};

struct clb_ent {
  char name[24];
  unsigned int size;
  unsigned int pad;
};

#endif
