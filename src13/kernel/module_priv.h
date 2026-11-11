/* module_priv.h - Estado interno do carregador, dividido entre module.c (SOX)
 * e elf.c (ELF64 dinamico). Nao e API: quem usa modulos inclui module.h. */
#pragma once
#include <stdint.h>
#include "module.h"
#include "drives.h"
#include "c_compiler/sox.h"

#define MOD_DEPS_MAX 16

struct mod_elf;

struct module {
  int used;
  int refs;
  int is_dll;
  int is64;
  uint32_t pid;
  char name[32];
  uint8_t* base;
  uint8_t* raw;
  uint32_t size;
  uint32_t entry;
  uint32_t stack_size;
  struct sox_export* exports;
  uint32_t nexports;
  char* strtab;
  uint32_t strtab_size;
  int deps[MOD_DEPS_MAX];
  int ndeps;
  struct mod_elf* elf; /* so em modulos ELF (elf.c); 0 em SOX */
};

extern struct module mods[MOD_MAX];

void mod_set_err(const char* a, const char* b);
/* Procura como o shell: nome como esta, depois nas unidades pref, corrente, D: e C:. */
int mod_locate(const char* name, char pref, char* out, int max);
/* "D:\X\LIBC.DLL" -> "LIBC.DLL" (maiusculas, para a lista de modulos). */
void mod_base_name(const char* path, char* out, int max);
/* Indice de um slot livre de mods[], ou -1. */
int mod_free_slot(void);

/* ---- elf.c ---- */
/* 1 se o arquivo comeca com um cabecalho ELF. */
int elf_probe(const char* path);
/* Carrega o objeto e tudo o que ele pede em DT_NEEDED, reloca e devolve o
 * indice em mods[]; -1 com o erro em mod_last_error(). global: os simbolos
 * entram no escopo global do processo (executavel, RTLD_GLOBAL). */
int elf_load(const char* path, int want_dll, int global);
/* Endereco de um simbolo exportado pelo modulo (sem olhar as dependencias). */
void* elf_export(struct module* m, const char* name);
/* Libera o que o modulo tem de ELF (chamado por unload). */
void elf_unload(struct module* m);
/* Fim do processo: libera as bibliotecas ELF que ele carregou. */
void elf_release_pid(uint32_t pid);
