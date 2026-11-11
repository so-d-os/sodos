/* module.h - Loader de .EXE e .DLL (formato SOX, c_compiler/sox.h). */
#pragma once
#include <stdint.h>

/* Um programa ELF traz varias bibliotecas (.so); cada programa tem as suas copias. */
#define MOD_MAX 96

/* Executa um .EXE: carrega, resolve DLLs, chama _start(argc, argv) numa pilha
 * propria e libera tudo ao voltar. Devolve o codigo de saida, ou
 * MOD_ERR_* (<= -1000) se nao conseguiu rodar. */
#define MOD_ERR_NOTFOUND -1000
#define MOD_ERR_FORMAT   -1001
#define MOD_ERR_MEMORY   -1002
#define MOD_ERR_IMPORT   -1003
#define MOD_ERR_DEPTH    -1004
int mod_exec(const char* path, int argc, char** argv);
/* Codigo de saida de um programa DOS encerrado com Ctrl+C (ASCII ETX). */
#define MOD_EXIT_BREAK 3

/* Procura o programa como o shell faz: nome, nome.EXE, na unidade corrente e em D:. */
int mod_find_exe(const char* name, char* out, int max);
/* Acha um script: nome.BAT, nome.CMD, ou nome sem extensao virando nome.BAT.
 * out recebe o caminho encontrado. -1 se nao ha. */
int mod_find_batch(const char* name, char* out, int max);

/* DLL em tempo de execucao. Handle > 0; 0 = falhou. */
int mod_dll_load(const char* name);
void* mod_dll_sym(int handle, const char* name);
void mod_dll_free(int handle);

/* ---- Bibliotecas ELF (.so) em tempo de execucao: a base de dlopen/dlsym.
 * Os tipos sao os mesmos de c_compiler/include/so.h (k_dl_*). Handle > 0. */
#define K_DL_NOLOAD 4       /* RTLD_NOLOAD: so devolve se ja estiver carregada */
#define K_DL_GLOBAL 0x100   /* RTLD_GLOBAL: simbolos visiveis as proximas cargas */
#define K_DL_EXE 1u          /* k_dl_info.flags: e o executavel */
#define K_DL_GLOBAL_SCOPE 2u /* k_dl_info.flags: esta no escopo global */

struct k_dl_info {
  uint64_t base;          /* endereco de carga (soma aos enderecos do ELF) */
  uint64_t size;
  const char* name;       /* DT_SONAME, ou o nome do arquivo */
  const char* path;       /* caminho completo no SODOS */
  uint64_t phdr;          /* tabela de segmentos na memoria; 0 se nao foi mapeada */
  uint32_t phnum;
  uint32_t flags;
  uint64_t dynamic;
  uint64_t init, init_array, init_arraysz;
  uint64_t fini, fini_array, fini_arraysz;
  uint64_t tls_image, tls_filesz, tls_memsz, tls_align;
  uint64_t tls_offset;    /* o bloco comeca em (ponteiro de thread - tls_offset) */
  uint32_t tls_modid;     /* 0 = sem TLS */
  uint32_t handle;
  uint64_t tls_static;    /* so no executavel: bytes de TLS estatico por thread */
  uint64_t stack_size;    /* so no executavel */
};

struct k_dl_addr {
  const char* fname;
  uint64_t fbase;
  const char* sname;      /* 0 se nenhum simbolo cobre o endereco */
  uint64_t saddr;
};

/* name 0 devolve o executavel. Carrega tambem as dependencias (DT_NEEDED). */
int mod_dl_open(const char* name, int flags);
/* handle 0 = escopo global (RTLD_DEFAULT); -1 = RTLD_NEXT a partir de caller. */
void* mod_dl_sym(int handle, const char* name, const void* caller);
int mod_dl_info(int handle, struct k_dl_info* out);
/* index-esimo modulo ELF do processo, em ordem de carga; 0 quando acaba. */
int mod_dl_iter(int index, struct k_dl_info* out);
/* Proximo modulo a inicializar (dependencias primeiro) ou a finalizar (ordem
 * inversa). Cada chamada marca o modulo; 0 quando nao ha mais. */
int mod_dl_init_next(struct k_dl_info* out);
int mod_dl_fini_next(struct k_dl_info* out);
int mod_dl_addr(const void* addr, struct k_dl_addr* out);

/* Ultimo erro de carga (texto curto para o usuario). */
const char* mod_last_error(void);

/* Um modulo carregado, para o TOP. raw/exports/strtab sao os blocos da arena
 * alta que ele ocupa (dono 0). */
struct mod_info {
  char name[32];
  uint32_t pid;
  int is_dll;
  int refs;
  uint32_t base;
  uint32_t size;
  uint32_t blk[3];
};
/* i de 0 a MOD_MAX-1; 1 se o slot esta em uso. */
int mod_get(int i, struct mod_info* out);

/* Lista modulos carregados no console. */
void mod_list(void);

/* Programa em execucao (0 = nenhum). */
uint32_t mod_current_pid(void);
/* Unidade de onde o .EXE atual foi carregado ('C', 'D', ...; 0 = nenhuma).
 * Programas usam para achar arquivos/log ao lado do proprio exe. */
char mod_exe_drive(void);

/* Estado do carregador trocado quando o desktop alterna entre programas.
 * depth e o aninhamento deste programa, nao a soma das janelas paradas. */
struct mod_cpu {
  uint64_t ctx;
  uint32_t pid;
  uint32_t owner;
  uint32_t depth;
  char drive;
};

void mod_cpu_save(struct mod_cpu* s);
void mod_cpu_load(const struct mod_cpu* s);
/* void ctx_switch(uint64_t* save_rsp, uint64_t next_rsp) */
void ctx_switch(uint64_t* save_rsp, uint64_t next_rsp);
