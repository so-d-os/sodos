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

/* Uma chamada de ferramenta devolvida pela API (formato OpenAI). */
typedef struct {
  int kind;        /* 0 nenhuma, 'w' gravar, 'c' comando, 'o' abrir */
  char arg[128];   /* caminho/programa/comando */
  char body[1400]; /* conteudo do write */
  char id[80];     /* tool_call id */
} ai_tool_t;

/* Igual a ai_chat, mas declara as ferramentas (write/run/open) e devolve as
 * chamadas em tools[0..n-1]. Devolve quantas chamadas (0 = so texto) ou -1. */
int ai_chat_tools(const ai_cfg_t* c, const ai_msg_t* msgs, int n,
                  void (*on_token)(void* ud, const char* s, int n), void* ud,
                  ai_tool_t* tools, int max_tools);

/* padroes: url do DeepSeek, model=deepseek-chat, key vazia */
void ai_cfg_defaults(ai_cfg_t* c);

/* Le AGENT.CFG da pasta atual, D:\, U:\ ou C:\. 0 = achou. */
int ai_cfg_load(ai_cfg_t* c);

/* 1 se da para conversar (url preenchida). */
int ai_cfg_ready(const ai_cfg_t* c);

const char* ai_last_error(void);

/* UTF-8 (API) -> CP850 (fonte do console); devolve o tamanho. */
int ai_utf8_to_cp850(char* dst, int cap, const char* src, int n);
/* CP850 (teclado) -> UTF-8 (o que a API entende). */
int ai_cp850_to_utf8(char* dst, int cap, const char* src, int n);

/* Envia a conversa e chama on_token a cada pedaco de texto do modelo.
 * on_token pode ser 0 (so acumula). Devolve 0 = ok; -1 = erro. */
int ai_chat(const ai_cfg_t* c, const ai_msg_t* msgs, int n,
            void (*on_token)(void* ud, const char* s, int n), void* ud);

#endif
