/*
 * crypt.c - CRYPT32: contextos de certificado, lojas em memoria e cadeias.
 *
 * O SODOS nao tem loja de autoridades raiz nem verificador de X.509 fora do
 * NSS do Firefox. Quem usa o verificador do Windows (o crypto/x509 do Go, por
 * exemplo) recebe aqui uma cadeia montada com o certificado e os
 * intermediarios que ele mesmo entregou, com o estado:
 *
 *   padrao                    CERT_TRUST_IS_UNTRUSTED_ROOT: a conexao TLS falha
 *   SET WINE_NOVERIFY=1   sem erro: o certificado NAO e conferido
 *
 * Com WINE_NOVERIFY=1 qualquer servidor passa (inclusive um impostor):
 * e uma escolha explicita de quem liga, avisada na serial a cada cadeia.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

struct cert_ctx {
  DWORD encoding;
  uint8_t* encoded;
  DWORD length;
  void* info;
  HANDLE store;
  /* nosso */
  int refs;
  struct cert_ctx* next;   /* na loja */
};

struct trust_status {
  DWORD error, info;
};
struct chain_elem {
  DWORD size;
  struct cert_ctx* cert;
  struct trust_status trust;
  void* revocation;
  void* issuance;
  void* application;
  const wchar16* ext_error;
};
struct simple_chain {
  DWORD size;
  struct trust_status trust;
  DWORD nelems;
  struct chain_elem** elems;
  void* trust_list;
  BOOL has_fresh;
  DWORD fresh;
};
struct chain_ctx {
  DWORD size;
  struct trust_status trust;
  DWORD nchains;
  struct simple_chain** chains;
  DWORD nlower;
  struct chain_ctx** lower;
  BOOL has_fresh;
  DWORD fresh;
  DWORD create_flags;
  uint8_t chain_id[16];
};

int wine_noverify(void); /* sig.c: SET WINE_NOVERIFY=1 */

static struct cert_ctx* ctx_new(const uint8_t* der, DWORD len, DWORD enc) {
  struct cert_ctx* c = (struct cert_ctx*)wzalloc(sizeof(*c) + len);
  if (!c) return 0;
  c->encoding = enc;
  c->encoded = (uint8_t*)(c + 1);
  c->length = len;
  c->refs = 1;
  memcpy(c->encoded, der, len);
  return c;
}

static WINAPI struct cert_ctx* CertCreateCertificateContext(DWORD enc, const uint8_t* der, DWORD len) {
  if (!der || !len) {
    wset_lasterr(ERROR_INVALID_PARAMETER);
    return 0;
  }
  return ctx_new(der, len, enc);
}
static WINAPI BOOL CertFreeCertificateContext(struct cert_ctx* c) {
  /* contextos de loja morrem com ela; os soltos, aqui */
  if (c && !c->store && --c->refs <= 0) k_free(c);
  return 1;
}
static WINAPI struct cert_ctx* CertDuplicateCertificateContext(struct cert_ctx* c) {
  if (c) c->refs++;
  return c;
}
static WINAPI HANDLE CertOpenStore(uintptr_t prov, DWORD enc, HANDLE cprov, DWORD fl, const void* para) {
  struct wobj* o = wobj_new(OBJ_MISC);
  (void)prov; (void)enc; (void)cprov; (void)fl; (void)para;
  return o ? wh_new(o) : 0;
}
static WINAPI HANDLE CertOpenSystemStoreW(HANDLE prov, const wchar16* name) {
  (void)prov;
  (void)name;
  return CertOpenStore(0, 0, 0, 0, 0); /* loja do sistema: vazia */
}
static WINAPI BOOL CertCloseStore(HANDLE h, DWORD fl) {
  struct wobj* o = wh_get(h, OBJ_MISC);
  struct cert_ctx* c;
  (void)fl;
  if (!o) return 0;
  c = (struct cert_ctx*)o->p1;
  while (c) {
    struct cert_ctx* n = c->next;
    k_free(c);
    c = n;
  }
  o->p1 = 0;
  wh_close(h);
  return 1;
}
static WINAPI BOOL CertAddCertificateContextToStore(HANDLE h, const struct cert_ctx* c, DWORD disp,
                                                    struct cert_ctx** out) {
  struct wobj* o = wh_get(h, OBJ_MISC);
  struct cert_ctx* n;
  struct cert_ctx** tail;
  (void)disp;
  if (!o || !c) {
    wset_lasterr(ERROR_INVALID_PARAMETER);
    return 0;
  }
  n = ctx_new(c->encoded, c->length, c->encoding);
  if (!n) return 0;
  n->store = h;
  for (tail = (struct cert_ctx**)&o->p1; *tail; tail = &(*tail)->next) {
  }
  *tail = n;
  if (out) *out = n;
  return 1;
}
static WINAPI struct cert_ctx* CertEnumCertificatesInStore(HANDLE h, struct cert_ctx* prev) {
  struct wobj* o = wh_get(h, OBJ_MISC);
  if (wine_debug && !prev) wlog("CertEnumCertificatesInStore", 0);
  if (!o) return 0;
  return prev ? prev->next : (struct cert_ctx*)o->p1;
}

static WINAPI BOOL CertGetCertificateChain(HANDLE engine, struct cert_ctx* leaf, const void* time, HANDLE store,
                                           const void* para, DWORD fl, void* res, struct chain_ctx** out) {
  struct wobj* o = wh_get(store, OBJ_MISC);
  struct cert_ctx* c;
  struct chain_ctx* cc;
  struct simple_chain* sc;
  struct chain_elem* el;
  DWORD n = 1, i = 0;
  int ok = wine_noverify();
  DWORD err = ok ? 0 : 0x20; /* CERT_TRUST_IS_UNTRUSTED_ROOT */
  (void)engine; (void)time; (void)para; (void)fl; (void)res;
  if (!leaf || !out) {
    wset_lasterr(ERROR_INVALID_PARAMETER);
    return 0;
  }
  for (c = o ? (struct cert_ctx*)o->p1 : 0; c; c = c->next)
    if (c != leaf && !(c->length == leaf->length && memcmp(c->encoded, leaf->encoded, c->length) == 0)) n++;
  cc = (struct chain_ctx*)wzalloc(sizeof(*cc) + sizeof(void*) + sizeof(*sc) + n * (sizeof(void*) + sizeof(*el)));
  if (!cc) return 0;
  cc->size = sizeof(*cc);
  cc->trust.error = err;
  cc->nchains = 1;
  cc->chains = (struct simple_chain**)(cc + 1);
  sc = (struct simple_chain*)(cc->chains + 1);
  cc->chains[0] = sc;
  sc->size = sizeof(*sc);
  sc->trust.error = err;
  sc->nelems = n;
  sc->elems = (struct chain_elem**)(sc + 1);
  el = (struct chain_elem*)(sc->elems + n);
  /* folha primeiro, depois os intermediarios na ordem em que chegaram */
  sc->elems[i] = &el[i];
  el[i].size = sizeof(*el);
  el[i].cert = leaf;
  i++;
  for (c = o ? (struct cert_ctx*)o->p1 : 0; c; c = c->next) {
    if (c == leaf || (c->length == leaf->length && memcmp(c->encoded, leaf->encoded, c->length) == 0)) continue;
    sc->elems[i] = &el[i];
    el[i].size = sizeof(*el);
    el[i].cert = c;
    i++;
  }
  if (ok) wlog("AVISO: certificado TLS aceito SEM verificacao (WINE_NOVERIFY=1)", 0);
  else wlog("certificado TLS recusado: sem autoridades raiz no SODOS (SET WINE_NOVERIFY=1 aceita sem conferir)", 0);
  *out = cc;
  return 1;
}
static WINAPI void CertFreeCertificateChain(struct chain_ctx* cc) {
  if (cc) k_free(cc);
}
struct policy_status {
  DWORD size, error;
  int32_t chain_index, elem_index;
  void* extra;
};
static WINAPI BOOL CertVerifyCertificateChainPolicy(uintptr_t policy, struct chain_ctx* cc, const void* para,
                                                    struct policy_status* st) {
  (void)policy;
  (void)para;
  st->error = (cc && cc->trust.error == 0) ? 0 : 0x800B0109u; /* CERT_E_UNTRUSTEDROOT */
  st->chain_index = st->error ? 0 : -1;
  st->elem_index = st->error ? 0 : -1;
  return 1;
}
static WINAPI BOOL CryptAcquireContextW(HANDLE* prov, const wchar16* cont, const wchar16* name, DWORD type, DWORD fl) {
  (void)cont; (void)name; (void)type; (void)fl;
  *prov = (HANDLE)(uintptr_t)0x10040;
  return 1;
}
static WINAPI BOOL CryptReleaseContext(HANDLE prov, DWORD fl) {
  (void)prov;
  (void)fl;
  return 1;
}
static WINAPI BOOL CryptGenRandom(HANDLE prov, DWORD n, uint8_t* buf) {
  (void)prov;
  while (n) {
    int k = n > 4096 ? 4096 : (int)n;
    k_random(buf, k);
    buf += k;
    n -= (DWORD)k;
  }
  return 1;
}

#define S(n) {#n, (void*)n}
const struct wine_sym crypt_syms[] = {
    S(CertCreateCertificateContext), S(CertFreeCertificateContext), S(CertDuplicateCertificateContext),
    S(CertOpenStore), S(CertOpenSystemStoreW), S(CertCloseStore), S(CertAddCertificateContextToStore),
    S(CertEnumCertificatesInStore), S(CertGetCertificateChain), S(CertFreeCertificateChain),
    S(CertVerifyCertificateChainPolicy), S(CryptAcquireContextW), S(CryptReleaseContext),
    S(CryptGenRandom),
    {0, 0}};
