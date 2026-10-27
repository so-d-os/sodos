/* browser.h - partes do navegador: url.c, http.c, html.c, search.c, main.c */
#ifndef BROWSER_H
#define BROWSER_H

#define URL_MAX 1024

/* ---- url.c ---- */
typedef struct {
  char scheme[8];   /* http, https, about, file */
  char host[256];
  int port;
  char path[URL_MAX];
} url_t;

/* aceita "exemplo.com" (vira https://exemplo.com/), "D:ARQ.HTM", "about:..." */
int url_parse(const char* s, url_t* u);
void url_format(const url_t* u, char* out, int max);
/* resolve href relativo a base; devolve 0 e preenche out, -1 se nao navegavel */
int url_resolve(const char* base, const char* href, char* out, int max);

/* ---- http.c ---- */
typedef struct {
  int status;
  char* body;
  int body_len;
  char content_type[128];
  char url[URL_MAX];   /* depois dos redirecionamentos */
  char err[160];
  int tls;
  int tls_ok;
  char secure[96]; /* "HTTPS" ou o aviso do certificado */
  int from_cache;  /* 1 se leu D:\temp\net */
  int cache_saved; /* 1 se esta visita gravou o cache */
  int aborted;     /* 1 se o usuario teclou ou clicou no meio do pedido */
} http_resp_t;

/* devolve 1 se o usuario interrompeu (tecla, clique ou fechar) */
typedef int (*progress_fn)(const char* msg);
#define HTTP_FRESH   1 /* ignora o cache (botao Recarregar) e grava por cima */
#define HTTP_NOSTORE 2 /* nao grava no cache (paginas de busca) */
/* 0 = recebeu uma resposta (veja status); -1 = erro de rede (veja err).
 * Figuras (image/...) nunca vao para o cache. */
int http_get(const char* url, http_resp_t* r, progress_fn progress, int flags);
void http_free(http_resp_t* r);
/* Fecha a conexao que ficou aberta para o proximo pedido (fim da pagina). */
void http_idle_close(void);

/* ---- html.c ---- */
#define RUN_BOLD  1
#define RUN_ULINE 2
#define RUN_HR    4
#define RUN_IMG   8
#define RUN_BG    16

#define DOC_IMGS 32
#define DOC_ELS 24
#define DOC_SCRIPTS 16
#define DOC_FORMS 4
#define DOC_FIELDS 48

typedef struct {
  int line, col, len, off;
  int flags;
  unsigned color;
  int link;          /* -1 = nenhum */
  int field;         /* -1, ou indice em doc_t.fields */
  int img;           /* -1, ou indice em doc_t.imgs */
  int img_row;       /* linha do caractere dentro da imagem */
  unsigned bg;       /* usado com RUN_BG */
} run_t;

typedef struct {
  char url[URL_MAX];
  char alt[64];
  int cols, lines;   /* caixa reservada na grade */
  int state;         /* 0 pendente, 1 decodificada, -1 falhou */
} doc_img_t;

typedef struct {
  char id[32];
  int run0, run1; /* runs inclusivos; run1 < run0 se vazio */
} doc_el_t;

typedef struct {
  char* src;
  int len;
  char url[URL_MAX]; /* preenchido se ainda precisa buscar */
} doc_script_t;

/* Campo de um formulario. hidden nao aparece; submit e o botao [Enviar]. */
typedef struct {
  int line, col, len;
  int submit, hidden, form, box;
  char name[32];
  char value[128];
} doc_field_t;

typedef struct {
  char* text;
  int text_len, text_cap;
  run_t* runs;
  int nruns, runs_cap;
  char** links;
  int nlinks, links_cap;
  int nlines;
  char title[128];
  doc_img_t imgs[DOC_IMGS];
  int nimgs;
  doc_el_t els[DOC_ELS];
  int nels;
  doc_script_t scripts[DOC_SCRIPTS];
  int nscripts;
  char form_action[DOC_FORMS][URL_MAX];
  int nforms;
  doc_field_t fields[DOC_FIELDS];
  int nfields;
  char js_alert[160];
} doc_t;

/* cols = largura em caracteres; cp437 = 1 usa os acentos da fonte da BIOS.
 * extra_css vem de <link rel=stylesheet> ja buscado. */
void doc_layout(doc_t* d, const char* html, int len, int cols, const char* base_url,
                int latin1, int cp437, const char* extra_css, int extra_len);
/* executa os <script> guardados no documento (depois do layout) */
void js_run(doc_t* d);
/* texto puro (text/plain) */
void doc_layout_text(doc_t* d, const char* text, int len, int cols, int cp437);
void doc_free(doc_t* d);
int doc_first_run(const doc_t* d, int line);
int doc_link_at(const doc_t* d, int line, int col);
int doc_field_at(const doc_t* d, int line, int col);

/* ---- search.c ---- */
#define SEARCH_DDG    1
#define SEARCH_GOOGLE 2
#define SEARCH_IMAGES 3
#define SEARCH_BING   4

/* Texto da barra que nao e endereco ("gato", "g gato", "b gato", "i gato") ou
 * about:busca?q=...&em=... -> URL do buscador. 0 se nao e busca. */
int search_url(const char* input, char* out, int max);
/* Links de saida do DuckDuckGo (/l/?uddg=), do Google (/url?q=) e do Bing
 * (/ck/a?u=a1<base64>) -> destino */
int search_unwrap(const char* url, char* out, int max);
/* Qual buscador deu esta URL (0 = nenhum); q recebe os termos, em UTF-8 */
int search_kind(const char* url, char* q, int qmax);
/* 1 se o buscador nao deu resultados a este navegador: o Google pede
 * JavaScript; o DuckDuckGo pede um desafio anti-robo (status 202) */
int search_refused(int kind, int status, const char* body, int len);
/* Pagina de imagens do Bing -> grade de miniaturas em HTML (malloc).
 * cols = largura do documento em caracteres. */
char* search_images_html(const char* body, int len, const char* q, int cols, int* outlen);
/* Copia do Bing de uma imagem original da ultima busca de imagens */
int search_image_copy(const char* url, char* out, int max);
/* Escapa q para usar num link (about:busca?q=...) */
int search_enc(char* dst, int n, int max, const char* s);

#endif
