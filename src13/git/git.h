/* git.h - git minimo do SODOS: clone e pull (raso), commit e push por
 * HTTPS, status, diff e log. Os arquivos ficam com o nome longo de verdade;
 * a lista esta em <pasta>\.git\sodos\NAMES.TXT.
 *   cc -o GIT.EXE git.c http.c pack.c sha1.c zinf.c TLS.DLL */
#ifndef GIT_H
#define GIT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include <tls.h>

/* ---------- sha1.c ---------- */
typedef struct {
  unsigned h[5];
  unsigned len_lo;
  unsigned len_hi;
  unsigned char buf[64];
  int n;
} sha1_ctx;

void sha1_init(sha1_ctx* c);
void sha1_update(sha1_ctx* c, const void* data, int len);
void sha1_final(sha1_ctx* c, unsigned char out[20]);
/* sha do objeto: "blob 12\0" + dados */
void git_hash(const char* type, const unsigned char* data, int len, unsigned char out[20]);
void sha_hex(const unsigned char* sha, char* out);          /* 41 bytes */
int hex_sha(const char* hex, unsigned char* sha);           /* 0 ok */

/* ---------- zinf.c ---------- */
/* Fluxo zlib inteiro. Devolve os bytes gerados ou -1; *used = bytes lidos. */
int zinflate(const unsigned char* src, int slen, unsigned char* dst, int dlen, int* used);
/* zlib "guardado" (sem compressao, valido para qualquer zlib): dst precisa
 * de slen + 6 + 5 * (slen / 65535 + 1) bytes. Devolve o tamanho. */
int zstore(const unsigned char* src, int slen, unsigned char* dst);

/* ---------- http.c ---------- */
/* GET (body = 0) ou POST para https://... Segue redirecionamentos.
 * *out e malloc. Devolve o status HTTP, ou -1 com err preenchido. */
extern char http_auth[400];   /* "Authorization: Basic ..." ou vazio */
extern char http_ua[64];      /* User-Agent; vazio = o do git */
int http_req(const char* url, const char* ctype, const char* body, int blen, char** out, int* olen,
             char* err);

/* ---------- pack.c ---------- */
#define OBJ_COMMIT 1
#define OBJ_TREE   2
#define OBJ_BLOB   3
#define OBJ_TAG    4
#define OBJ_OFS    6
#define OBJ_REF    7

/* Le um pack inteiro em memoria e calcula o sha de todos os objetos. */
int pack_load(unsigned char* data, int len, char* err);
/* Objeto pelo sha. O ponteiro vale ate a proxima chamada. 0 se nao tem. */
unsigned char* pack_get(const unsigned char* sha, int* type, int* size);
void pack_free(void);
/* 1 se o pack carregado tem o objeto (sem descomprimir) */
int pack_has(const unsigned char* sha);
/* troca o pack carregado pelo guardado (o primeiro guarda o atual; nenhum
 * fica carregado). pack_get/pack_free valem para o carregado. */
void pack_swap(void);
/* Grava o .idx (versao 2) do pack carregado em path. 0 ok. */
int pack_write_idx(const char* path);
extern int pack_quiet;

#endif
