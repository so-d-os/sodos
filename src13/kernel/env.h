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

/* Pasta do sistema: PATHSODOS (padrao U:\SODOS). Os comandos do sistema
 * (NET WIFI START, HOSTS.CFG...) procuram as configuracoes nela. */
#define ENV_SODOS_DEFAULT "U:\\SODOS"
/* PATHSODOS + "\" + rel em out. Ex.: "NET\WIFI.CFG" -> U:\SODOS\NET\WIFI.CFG */
void env_sodos_path(const char* rel, char* out, int max);
/* index-esima pasta do PATH (PATH=C1;C2;C3) em out; 0 quando acaba. */
int env_path_at(int index, char* out, int max);
