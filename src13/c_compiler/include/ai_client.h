/* ai_client.h - cliente de chat no formato OpenAI, lendo o mesmo AGENT.CFG.
 *
 * Usado pelo painel de IA (AIPANEL.EXE). O AGENT.EXE continua com a copia
 * dele; aqui fica so o necessario para uma conversa simples: configuracao,
 * POST com TLS/HTTP e leitura do stream (SSE + chunked).
 */
#ifndef AI_CLIENT_H
#define AI_CLIENT_H

typedef struct {
  char key[256];
  char url[256];
  char model[64];
  char path[64];   /* arquivo de onde veio ("" = padroes) */
} ai_cfg_t;

typedef struct {
  const char* role;  /* "system", "user" ou "assistant" */
  const char* text;  /* UTF-8 */
} ai_msg_t;

/* padroes: url do DeepSeek, model=deepseek-chat, key vazia */
void ai_cfg_defaults(ai_cfg_t* c);

/* Le AGENT.CFG da pasta atual, D:\, U:\ ou C:\. 0 = achou. */
int ai_cfg_load(ai_cfg_t* c);

/* 1 se da para conversar (url preenchida). */
int ai_cfg_ready(const ai_cfg_t* c);

const char* ai_last_error(void);

/* UTF-8 (API) -> CP850 (fonte do console); devolve o tamanho. */
int ai_utf8_to_cp850(char* dst, int cap, const char* src, int n);

/* Envia a conversa e chama on_token a cada pedaco de texto do modelo.
 * on_token pode ser 0 (so acumula). Devolve 0 = ok; -1 = erro. */
int ai_chat(const ai_cfg_t* c, const ai_msg_t* msgs, int n,
            void (*on_token)(void* ud, const char* s, int n), void* ud);

#endif
