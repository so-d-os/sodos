/* env.h - variaveis de ambiente do sistema (SET NOME=valor). Ver env.c. */
#pragma once
#include <stdint.h>

#define ENV_NAME_MAX 32
#define ENV_VAL_MAX  260

/* Valor ou 0 se nao existe. */
const char* env_get(const char* name);
/* val vazio ou 0 apaga. 0 ok, -1 nome invalido, -2 tabela cheia. */
int env_set(const char* name, const char* val);
/* Muda a cada env_set que altera algo. */
uint32_t env_gen(void);
/* index-esima variavel (para listar). 0 quando acaba. */
int env_at(int index, const char** name, const char** val);
int cmd_set(int argc, char** argv);
