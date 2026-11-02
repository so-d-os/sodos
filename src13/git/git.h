/* git.h - git minimo do SODOS: clone e pull (raso, so leitura) por HTTPS,
 * status e log. Os arquivos ficam com nomes 8.3; o nome longo de cada um
 * esta em <pasta>\_GIT\NAMES.TXT.
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

/* ---------- http.c ---------- */
/* GET (body = 0) ou POST para https://... Segue redirecionamentos.
 * *out e malloc. Devolve o status HTTP, ou -1 com err preenchido. */
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

#endif
