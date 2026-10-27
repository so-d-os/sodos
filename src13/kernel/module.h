/* module.h - Loader de .EXE e .DLL (formato SOX, c_compiler/sox.h). */
#pragma once
#include <stdint.h>

#define MOD_MAX 32

/* Executa um .EXE: carrega, resolve DLLs, chama _start(argc, argv) numa pilha
 * propria e libera tudo ao voltar. Devolve o codigo de saida, ou
 * MOD_ERR_* (<= -1000) se nao conseguiu rodar. */
#define MOD_ERR_NOTFOUND -1000
#define MOD_ERR_FORMAT   -1001
#define MOD_ERR_MEMORY   -1002
#define MOD_ERR_IMPORT   -1003
#define MOD_ERR_DEPTH    -1004
int mod_exec(const char* path, int argc, char** argv);

/* Procura o programa como o shell faz: nome, nome.EXE, na unidade corrente e em D:. */
int mod_find_exe(const char* name, char* out, int max);

/* DLL em tempo de execucao. Handle > 0; 0 = falhou. */
int mod_dll_load(const char* name);
void* mod_dll_sym(int handle, const char* name);
void mod_dll_free(int handle);

/* Ultimo erro de carga (texto curto para o usuario). */
const char* mod_last_error(void);

/* Lista modulos carregados no console. */
void mod_list(void);

/* Programa em execucao (0 = nenhum). */
uint32_t mod_current_pid(void);

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
