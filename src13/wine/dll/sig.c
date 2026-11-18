/*
 * sig.c - CRYPT32/WINTRUST: quem assinou um arquivo (CryptQueryObject,
 * CryptMsgGetParam, CertFindCertificateInStore, CertGetNameString).
 *
 * Le o PKCS#7 de assinatura: de um pacote .msix/.appx (AppxSignature.p7x
 * dentro do ZIP) ou de um arquivo .p7b/.p7x. Devolve os certificados e o
 * assinante de verdade do arquivo, mas NAO confere a assinatura (nao ha
 * RSA/ECDSA nem autoridades raiz no WINE.DLL). Por isso so responde com
 *
 *   SET WINE_NOVERIFY=1
 *
 * e avisa na serial. Sem a variavel, CryptQueryObject e WinVerifyTrust
 * falham (TRUST_E_NOSIGNATURE) e o programa trata como arquivo nao assinado.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"
#include "zip.h"

const uint8_t* wfile_map(const char* path, uint64_t* size, void** tofree); /* file.c */

int wine_noverify(void) {
  char v[8];
  if (k_getenv("WINE_NOVERIFY", v, (int)sizeof(v)) > 0 && v[0] == '1') return 1;
  return k_getenv("WINE_TLS_NOVERIFY", v, (int)sizeof(v)) > 0 && v[0] == '1';
}

/* ---------- DER ---------- */
struct tlv {
  uint8_t tag;
  const uint8_t* val;
  size_t len;
  const uint8_t* raw;   /* comeco do TLV */
  size_t rawlen;
};
static int der_next(const uint8_t** p, const uint8_t* end, struct tlv* t) {
  const uint8_t* s = *p;
  size_t len;
  if (end - s < 2) return 0;
  t->raw = s;
  t->tag = *s++;
  len = *s++;
  if (len & 0x80) {
    int n = (int)(len & 0x7F);
    if (n < 1 || n > 4 || end - s < n) return 0;
    len = 0;
    while (n--) len = (len << 8) | *s++;
  }
  if ((size_t)(end - s) < len) return 0;
  t->val = s;
  t->len = len;
  t->rawlen = (size_t)(s - t->raw) + len;
  *p = s + len;
  return 1;
}

/* ---------- contexto de certificado (mesmo layout do crypt.c) ---------- */
struct blob {
  DWORD cb;
  const uint8_t* pb;
};
struct algid {
  const char* oid;
  struct blob params;
};
struct bitblob {
  DWORD cb;
  const uint8_t* pb;
  DWORD unused;
};
struct cert_info {          /* CERT_INFO x64: 208 bytes */
  DWORD version;
  struct blob serial;
  struct algid sigalg;
  struct blob issuer;
  uint64_t not_before, not_after;
  struct blob subject;
  struct algid key_alg;     /* SubjectPublicKeyInfo.Algorithm */
  struct bitblob key;       /* SubjectPublicKeyInfo.PublicKey */
  struct bitblob issuer_uid, subject_uid;
  DWORD next;
  void* exts;
};
_Static_assert(sizeof(struct cert_info) == 208, "CERT_INFO");

/* OID DER -> "1.2.840.113549.1.1.1" */
static void oid_str(const uint8_t* p, size_t len, char* out, int max) {
  size_t i;
  int o = 0;
  uint32_t v = 0;
  int first = 1;
  for (i = 0; i < len && o < max - 12; i++) {
    v = (v << 7) | (p[i] & 0x7Fu);
    if (p[i] & 0x80) continue;
    if (first) {
      uint32_t a = v < 80 ? v / 40 : 2, b = v < 80 ? v % 40 : v - 80;
      out[o++] = (char)('0' + a);
      out[o++] = '.';
      v = b;
      first = 0;
    } else {
      out[o++] = '.';
    }
    {
      char d[12];
      int k = 0;
      do {
        d[k++] = (char)('0' + v % 10);
        v /= 10;
      } while (v);
      while (k) out[o++] = d[--k];
    }
    v = 0;
  }
  out[o] = 0;
}
static void alg_parse(const struct tlv* seq, struct algid* a, char* oidbuf, int max) {
  const uint8_t* p = seq->val;
  const uint8_t* end = seq->val + seq->len;
  struct tlv oid, par;
  if (!der_next(&p, end, &oid) || oid.tag != 6) return;
  oid_str(oid.val, oid.len, oidbuf, max);
  a->oid = oidbuf;
  if (der_next(&p, end, &par)) {
    a->params.cb = (DWORD)par.rawlen;
    a->params.pb = par.raw;
  }
}
struct cert_ctx {
  DWORD encoding;
  uint8_t* encoded;
  DWORD length;
  struct cert_info* info;
  HANDLE store;
  int refs;
  struct cert_ctx* next;
};

/* Preenche serial/issuer/subject de um certificado DER. */
static int cert_parse(const uint8_t* der, size_t len, struct cert_info* ci, uint8_t* serial_le, size_t smax,
                      char* oids) {
  const uint8_t* p = der;
  const uint8_t* end = der + len;
  struct tlv cert, tbs, t;
  size_t i;
  if (!der_next(&p, end, &cert) || cert.tag != 0x30) return 0;
  p = cert.val;
  end = cert.val + cert.len;
  if (!der_next(&p, end, &tbs) || tbs.tag != 0x30) return 0;
  p = tbs.val;
  end = tbs.val + tbs.len;
  if (!der_next(&p, end, &t)) return 0;
  if (t.tag == 0xA0) { /* versao explicita */
    if (!der_next(&p, end, &t)) return 0;
  }
  if (t.tag != 0x02 || t.len > smax) return 0;
  /* CryptoAPI guarda o numero de serie em little-endian */
  for (i = 0; i < t.len; i++) serial_le[i] = t.val[t.len - 1 - i];
  ci->serial.cb = (DWORD)t.len;
  ci->serial.pb = serial_le;
  if (!der_next(&p, end, &t)) return 0; /* algoritmo */
  if (t.tag == 0x30) alg_parse(&t, &ci->sigalg, oids, 48);
  if (!der_next(&p, end, &t) || t.tag != 0x30) return 0;
  ci->issuer.cb = (DWORD)t.rawlen;
  ci->issuer.pb = t.raw;
  if (!der_next(&p, end, &t)) return 0; /* validade */
  if (!der_next(&p, end, &t) || t.tag != 0x30) return 0;
  ci->subject.cb = (DWORD)t.rawlen;
  ci->subject.pb = t.raw;
  ci->version = 2;
  /* SubjectPublicKeyInfo { AlgorithmIdentifier, BIT STRING } */
  if (der_next(&p, end, &t) && t.tag == 0x30) {
    const uint8_t* q = t.val;
    const uint8_t* qe = t.val + t.len;
    struct tlv alg, key;
    if (der_next(&q, qe, &alg) && alg.tag == 0x30) alg_parse(&alg, &ci->key_alg, oids + 48, 48);
    if (der_next(&q, qe, &key) && key.tag == 3 && key.len > 1) {
      ci->key.unused = key.val[0];
      ci->key.cb = (DWORD)key.len - 1;
      ci->key.pb = key.val + 1;
    }
  }
  return 1;
}

/* Atributo (OID DER de 3 bytes 55 04 xx) de um Name: copia em UTF-8. */
static int name_attr(const uint8_t* name, size_t len, uint8_t which, char* out, int max) {
  const uint8_t* p = name;
  const uint8_t* end = name + len;
  struct tlv seq, set, atv, oid, val;
  if (!der_next(&p, end, &seq) || seq.tag != 0x30) return 0;
  p = seq.val;
  end = seq.val + seq.len;
  while (der_next(&p, end, &set)) {
    const uint8_t* q = set.val;
    const uint8_t* qe = set.val + set.len;
    while (der_next(&q, qe, &atv)) {
      const uint8_t* r = atv.val;
      const uint8_t* re = atv.val + atv.len;
      if (!der_next(&r, re, &oid) || !der_next(&r, re, &val)) continue;
      if (oid.tag == 6 && oid.len == 3 && oid.val[0] == 0x55 && oid.val[1] == 4 && oid.val[2] == which) {
        int n = 0;
        size_t i;
        if (val.tag == 0x1E) { /* BMPString: UTF-16BE */
          for (i = 0; i + 1 < val.len && n < max - 1; i += 2) out[n++] = val.val[i] ? '?' : (char)val.val[i + 1];
        } else {
          for (i = 0; i < val.len && n < max - 1; i++) out[n++] = (char)val.val[i];
        }
        out[n] = 0;
        return n;
      }
    }
  }
  return 0;
}

/* ---------- loja + mensagem de um PKCS#7 ---------- */
struct sigmsg {
  struct cert_ctx* signer;
  struct blob issuer;
  uint8_t serial_le[40];
  DWORD serial_len;
};

static struct cert_ctx* store_add(struct wobj* store, HANDLE h, const uint8_t* der, size_t len) {
  struct cert_ctx* c = (struct cert_ctx*)wzalloc(sizeof(*c) + sizeof(struct cert_info) + 40 + 96 + len);
  uint8_t* serial;
  char* oids;
  struct cert_ctx** tail;
  if (!c) return 0;
  c->info = (struct cert_info*)(c + 1);
  serial = (uint8_t*)(c->info + 1);
  oids = (char*)serial + 40;
  c->encoded = serial + 40 + 96;
  memcpy(c->encoded, der, len);
  c->length = (DWORD)len;
  c->encoding = 0x10001;
  c->store = h;
  c->refs = 1;
  if (!cert_parse(c->encoded, len, c->info, serial, 40, oids)) {
    k_free(c);
    return 0;
  }
  for (tail = (struct cert_ctx**)&store->p1; *tail; tail = &(*tail)->next) {
  }
  *tail = c;
  return c;
}

/* PKCS#7 SignedData -> certificados na loja e o assinante em msg. */
static int pkcs7_load(const uint8_t* der, size_t len, struct wobj* store, HANDLE hstore, struct sigmsg* msg) {
  const uint8_t* p = der;
  const uint8_t* end = der + len;
  struct tlv ci, t, sd, certs, sis;
  int have_certs = 0;
  if (!der_next(&p, end, &ci) || ci.tag != 0x30) return 0;
  p = ci.val;
  end = ci.val + ci.len;
  if (!der_next(&p, end, &t) || t.tag != 6) return 0;      /* contentType */
  if (!der_next(&p, end, &t) || t.tag != 0xA0) return 0;   /* [0] content */
  p = t.val;
  end = t.val + t.len;
  if (!der_next(&p, end, &sd) || sd.tag != 0x30) return 0; /* SignedData */
  p = sd.val;
  end = sd.val + sd.len;
  if (!der_next(&p, end, &t)) return 0;                    /* version */
  if (!der_next(&p, end, &t)) return 0;                    /* digestAlgorithms */
  if (!der_next(&p, end, &t)) return 0;                    /* contentInfo */
  memset(&sis, 0, sizeof(sis));
  while (der_next(&p, end, &t)) {
    if (t.tag == 0xA0) {
      certs = t;
      have_certs = 1;
    } else if (t.tag == 0x31) {
      sis = t; /* signerInfos */
    }
  }
  if (!have_certs || !sis.val) return 0;
  {
    const uint8_t* q = certs.val;
    const uint8_t* qe = certs.val + certs.len;
    struct tlv c;
    while (der_next(&q, qe, &c))
      if (c.tag == 0x30) store_add(store, hstore, c.raw, c.rawlen);
  }
  {
    /* primeiro SignerInfo: version, issuerAndSerialNumber { issuer, serial } */
    const uint8_t* q = sis.val;
    const uint8_t* qe = sis.val + sis.len;
    struct tlv si, ias, iss, ser;
    struct cert_ctx* c;
    size_t i;
    if (!der_next(&q, qe, &si) || si.tag != 0x30) return 0;
    q = si.val;
    qe = si.val + si.len;
    if (!der_next(&q, qe, &t)) return 0;
    if (!der_next(&q, qe, &ias) || ias.tag != 0x30) return 0;
    q = ias.val;
    qe = ias.val + ias.len;
    if (!der_next(&q, qe, &iss) || !der_next(&q, qe, &ser) || ser.len > sizeof(msg->serial_le)) return 0;
    for (i = 0; i < ser.len; i++) msg->serial_le[i] = ser.val[ser.len - 1 - i];
    msg->serial_len = (DWORD)ser.len;
    msg->issuer.cb = (DWORD)iss.rawlen;
    msg->issuer.pb = iss.raw;
    for (c = (struct cert_ctx*)store->p1; c; c = c->next)
      if (c->info->serial.cb == msg->serial_len && memcmp(c->info->serial.pb, msg->serial_le, msg->serial_len) == 0 &&
          c->info->issuer.cb == msg->issuer.cb && memcmp(c->info->issuer.pb, msg->issuer.pb, msg->issuer.cb) == 0) {
        msg->signer = c;
        break;
      }
  }
  return msg->signer != 0;
}

/* Acha o PKCS#7 do arquivo: ZIP com AppxSignature.p7x ("PKCX" + DER) ou DER puro. */
#define R16(p) ((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8))
#define R32(p) (R16(p) | (R16((p) + 2) << 16))
static uint8_t* find_pkcs7(const uint8_t* data, uint64_t size, size_t* len) {
  size_t* len_out = len;
  struct zip z;
  struct zip_entry en;
  if (size > 4 && data[0] == 'P' && data[1] == 'K' && zip_open(&z, data, size)) {
    while (zip_next(&z, &en)) {
      if (wstreq(en.name, "AppxSignature.p7x")) {
        uint8_t* raw = zip_extract(&z, &en);
        if (!raw) return 0;
        if (en.usize > 4 && memcmp(raw, "PKCX", 4) == 0) {
          memmove(raw, raw + 4, (size_t)en.usize - 4);
          *len = (size_t)en.usize - 4;
        } else {
          *len = (size_t)en.usize;
        }
        return raw;
      }
    }
    return 0;
  }
  /* PE com Authenticode: diretorio de seguranca (indice 4) aponta, por
   * offset de arquivo, para WIN_CERTIFICATE {len, rev, tipo, PKCS#7} */
  if (size > 0x200 && data[0] == 'M' && data[1] == 'Z') {
    uint32_t pe = R32(data + 0x3C);
    if (pe + 24 + 240 < size && memcmp(data + pe, "PE\0\0", 4) == 0) {
      const uint8_t* opt = data + pe + 24;
      uint32_t dirs = R16(opt) == 0x20B ? 112 : 96;
      uint32_t off = R32(opt + dirs + 8 * 4), len = R32(opt + dirs + 8 * 4 + 4);
      if (off && len > 8 && (uint64_t)off + len <= size && R16(data + off + 6) == 2) {
        uint32_t clen = R32(data + off) - 8;
        uint8_t* raw = (uint8_t*)k_malloc(clen);
        if (!raw) return 0;
        memcpy(raw, data + off + 8, clen);
        *len_out = clen;
        return raw;
      }
    }
    return 0;
  }
  if (size > 4 && size < (8u << 20)) {
    int off = memcmp(data, "PKCX", 4) == 0 ? 4 : 0;
    uint8_t* raw;
    if (data[off] != 0x30) return 0;
    raw = (uint8_t*)k_malloc((unsigned)size);
    if (!raw) return 0;
    memcpy(raw, data + off, (size_t)size - (size_t)off);
    *len = (size_t)size - (size_t)off;
    return raw;
  }
  return 0;
}

static WINAPI BOOL CryptQueryObject(DWORD type, const void* obj, DWORD content, DWORD format, DWORD fl,
                                    DWORD* enc, DWORD* ctype, DWORD* ftype, HANDLE* hstore, HANDLE* hmsg,
                                    const void** ctx) {
  char path[300];
  uint64_t size = 0;
  void* tofree = 0;
  const uint8_t* data;
  uint8_t* p7;
  size_t p7len = 0;
  struct wobj* store;
  struct wobj* msg;
  struct sigmsg* sm;
  HANDLE hs;
  (void)content; (void)format; (void)fl;
  if (hstore) *hstore = 0;
  if (hmsg) *hmsg = 0;
  if (ctx) *ctx = 0;
  if (type != 1) { /* CERT_QUERY_OBJECT_FILE */
    wset_lasterr(0x80092009u);
    return 0;
  }
  wpath16((const wchar16*)obj, path, (int)sizeof(path));
  if (!wine_noverify()) {
    wlog("CryptQueryObject: assinatura nao pode ser conferida no SODOS (SET WINE_NOVERIFY=1 le o assinante sem conferir): ",
         path);
    wset_lasterr(0x800B0100u); /* TRUST_E_NOSIGNATURE */
    return 0;
  }
  {
    /* DIAGNOSTICO (SET WINE_SIGTEST=1): responde com a assinatura do proprio
     * EXE para qualquer arquivo. Serve so para saber se um instalador compara
     * a identidade do certificado; nunca use fora de teste. */
    char v[8];
    if (k_getenv("WINE_SIGTEST", v, (int)sizeof(v)) > 0 && v[0] == '1' && wp()->exe) {
      wlog("DIAGNOSTICO WINE_SIGTEST=1: assinatura do EXE devolvida no lugar da de ", path);
      strncpy(path, wp()->exe->path, sizeof(path) - 1);
    }
  }
  data = wfile_map(path, &size, &tofree);
  if (!data) {
    wset_lasterr(ERROR_FILE_NOT_FOUND);
    return 0;
  }
  p7 = find_pkcs7(data, size, &p7len);
  if (tofree) k_free(tofree);
  store = wobj_new(OBJ_MISC);
  msg = wobj_new(OBJ_MISC);
  sm = (struct sigmsg*)wzalloc(sizeof(*sm));
  if (!p7 || !store || !msg || !sm) {
    wlog("CryptQueryObject: sem assinatura PKCS#7 em ", path);
    wset_lasterr(0x80092009u); /* CRYPT_E_NO_MATCH */
    return 0;
  }
  hs = wh_new(store);
  if (!pkcs7_load(p7, p7len, store, hs, sm)) {
    wlog("CryptQueryObject: PKCS#7 invalido em ", path);
    wset_lasterr(0x80092009u);
    return 0;
  }
  /* os certificados apontam para dentro de p7 (issuer do assinante): fica */
  msg->p1 = sm;
  msg->p2 = 0;
  {
    char cn[128];
    cn[0] = 0;
    name_attr(sm->signer->info->subject.pb, sm->signer->info->subject.cb, 3, cn, (int)sizeof(cn));
    wlog("AVISO: assinatura NAO conferida (WINE_NOVERIFY=1); assinante declarado: ", cn);
    if (wine_debug) {
      uint64_t ser = 0, key = 0;
      DWORD i;
      int ncert = 0;
      struct cert_ctx* c;
      for (i = 0; i < sm->serial_len && i < 8; i++) ser |= (uint64_t)sm->serial_le[i] << (8 * i);
      for (i = 0; i < 8 && i < sm->signer->info->key.cb; i++) key = (key << 8) | sm->signer->info->key.pb[i];
      for (c = (struct cert_ctx*)store->p1; c; c = c->next) ncert++;
      wlog("  arquivo ", path);
      wlogx("  serie (8 bytes baixos) ", ser);
      wlogx("  chave publica, bytes ", sm->signer->info->key.cb);
      wlogx("  chave publica, inicio ", key);
      {
        void wsha256(const uint8_t* data, size_t len, uint8_t out[32]);
        uint8_t h[32];
        uint64_t hk = 0, hc = 0;
        char val[40];
        const uint8_t* vp = sm->signer->info->issuer.pb + sm->signer->info->issuer.cb; /* validade vem depois */
        int k;
        wsha256(sm->signer->info->key.pb, sm->signer->info->key.cb, h);
        for (i = 0; i < 8; i++) hk = (hk << 8) | h[i];
        wsha256(sm->signer->encoded, sm->signer->length, h);
        for (i = 0; i < 8; i++) hc = (hc << 8) | h[i];
        wlogx("  sha256(chave publica) ", hk);
        wlogx("  sha256(certificado) ", hc);
        /* SEQUENCE { UTCTime notBefore, UTCTime notAfter } */
        for (k = 0; k < 36 && k < (int)vp[1]; k++) val[k] = (vp[2 + k] >= 32 && vp[2 + k] < 127) ? (char)vp[2 + k] : '|';
        val[k] = 0;
        wlog("  validade ", val);
        {
          uint64_t hs = 0, hi = 0;
          const uint8_t* q = sm->signer->info->subject.pb;
          const uint8_t* qe = q + sm->signer->info->subject.cb;
          struct tlv seq, set, atv, oid, v;
          wsha256(sm->signer->info->subject.pb, sm->signer->info->subject.cb, h);
          for (i = 0; i < 8; i++) hs = (hs << 8) | h[i];
          wsha256(sm->signer->info->issuer.pb, sm->signer->info->issuer.cb, h);
          for (i = 0; i < 8; i++) hi = (hi << 8) | h[i];
          wlogx("  subject, bytes ", sm->signer->info->subject.cb);
          wlogx("  sha256(subject) ", hs);
          wlogx("  sha256(issuer) ", hi);
          /* cada RDN: OID e valor */
          if (der_next(&q, qe, &seq)) {
            q = seq.val;
            qe = seq.val + seq.len;
            while (der_next(&q, qe, &set)) {
              const uint8_t* r = set.val;
              const uint8_t* re = set.val + set.len;
              while (der_next(&r, re, &atv)) {
                const uint8_t* a = atv.val;
                const uint8_t* ae = atv.val + atv.len;
                char line[160], o2[48];
                size_t n, m;
                if (!der_next(&a, ae, &oid) || !der_next(&a, ae, &v)) continue;
                oid_str(oid.val, oid.len, o2, (int)sizeof(o2));
                strcpy(line, o2);
                strcat(line, v.tag == 0x13 ? " (Printable) = " : v.tag == 0x0C ? " (UTF8) = " : " (outro) = ");
                n = strlen(line);
                for (m = 0; m < v.len && n < sizeof(line) - 1; m++) line[n++] = (char)v.val[m];
                line[n] = 0;
                wlog("    rdn ", line);
              }
            }
          }
        }
      }
      wlogx("  certificados no PKCS#7 ", (uint64_t)ncert);
    }
  }
  if (enc) *enc = 0x10001;
  if (ctype) *ctype = 10; /* CERT_QUERY_CONTENT_PKCS7_SIGNED_EMBED */
  if (ftype) *ftype = 1;  /* BINARY */
  if (hstore) *hstore = hs;
  if (hmsg) *hmsg = wh_new(msg);
  return 1;
}

struct alg_id {
  const char* oid;
  struct blob params;
};
struct signer_info { /* CMSG_SIGNER_INFO */
  DWORD version;
  struct blob issuer;
  struct blob serial;
  struct alg_id hash_alg, enc_alg;
  struct blob enc_hash;
  struct {
    DWORD n;
    void* attrs;
  } auth, unauth;
};
static WINAPI BOOL CryptMsgGetParam(HANDLE hmsg, DWORD type, DWORD idx, void* data, DWORD* size) {
  struct wobj* o = wh_get(hmsg, OBJ_MISC);
  struct sigmsg* sm = o ? (struct sigmsg*)o->p1 : 0;
  (void)idx;
  if (wine_debug) wlogx("CryptMsgGetParam tipo ", type);
  if (!sm || !size) {
    wset_lasterr(ERROR_INVALID_PARAMETER);
    return 0;
  }
  if (type == 6) { /* CMSG_SIGNER_INFO_PARAM */
    DWORD need = sizeof(struct signer_info) + sm->issuer.cb + sm->serial_len;
    struct signer_info* si = (struct signer_info*)data;
    if (!data) {
      *size = need;
      return 1;
    }
    if (*size < need) {
      *size = need;
      wset_lasterr(ERROR_MORE_DATA);
      return 0;
    }
    memset(si, 0, sizeof(*si));
    si->version = 1;
    si->issuer.cb = sm->issuer.cb;
    si->issuer.pb = (uint8_t*)(si + 1);
    memcpy((uint8_t*)(si + 1), sm->issuer.pb, sm->issuer.cb);
    si->serial.cb = sm->serial_len;
    si->serial.pb = (uint8_t*)(si + 1) + sm->issuer.cb;
    memcpy((uint8_t*)(si + 1) + sm->issuer.cb, sm->serial_le, sm->serial_len);
    si->hash_alg.oid = "2.16.840.1.101.3.4.2.1"; /* sha256 */
    si->enc_alg.oid = "1.2.840.113549.1.1.1";    /* rsa */
    *size = need;
    return 1;
  }
  if (type == 5) { /* CMSG_SIGNER_COUNT_PARAM */
    if (data && *size >= 4) *(DWORD*)data = 1;
    *size = 4;
    return 1;
  }
  wset_lasterr(ERROR_INVALID_PARAMETER);
  return 0;
}
static WINAPI BOOL CryptMsgClose(HANDLE hmsg) {
  if (wh_get(hmsg, OBJ_MISC)) wh_close(hmsg);
  return 1;
}

/* Acha um certificado da loja. CERT_FIND_SUBJECT_CERT (0xB0000): pelo
 * emissor + numero de serie de um CERT_INFO. Outros tipos: o primeiro. */
static WINAPI struct cert_ctx* CertFindCertificateInStore(HANDLE hstore, DWORD enc, DWORD fl, DWORD type,
                                                          const struct cert_info* para, struct cert_ctx* prev) {
  struct wobj* o = wh_get(hstore, OBJ_MISC);
  struct cert_ctx* c;
  (void)enc;
  (void)fl;
  if (wine_debug) wlogx("CertFindCertificateInStore tipo ", type);
  if (!o) return 0;
  for (c = prev ? prev->next : (struct cert_ctx*)o->p1; c; c = c->next) {
    if (type == 0xB0000u && para && c->info) {
      if (c->info->serial.cb == para->serial.cb && memcmp(c->info->serial.pb, para->serial.pb, para->serial.cb) == 0 &&
          c->info->issuer.cb == para->issuer.cb && memcmp(c->info->issuer.pb, para->issuer.pb, para->issuer.cb) == 0)
        return c;
      continue;
    }
    return c;
  }
  wset_lasterr(0x80092004u); /* CRYPT_E_NOT_FOUND */
  return 0;
}

static DWORD put16(const char* s, wchar16* buf, DWORD n) {
  int len = wutf8_to_16(s, -1, 0, 0);
  if (!buf || !n) return (DWORD)len + 1;
  if ((DWORD)len + 1 > n) len = (int)n - 1;
  wutf8_to_16(s, len, buf, len);
  buf[len] = 0;
  return (DWORD)len + 1;
}
/* type 4 (SIMPLE_DISPLAY): CN, senao O. type 3 (ATTR): OID em para ("2.5.4.3"...). */
static WINAPI DWORD CertGetNameStringW(const struct cert_ctx* c, DWORD type, DWORD fl, const void* para, wchar16* buf,
                                       DWORD n) {
  char out[200];
  const struct blob* name;
  uint8_t which = 3;
  out[0] = 0;
  if (wine_debug) {
    wlogx("CertGetNameString tipo ", type);
    wlogx("  flags ", fl);
    if (type == 3 && para) wlog("  oid ", (const char*)para);
  }
  if (!c || !c->info) return put16("", buf, n);
  name = (fl & 1) ? &c->info->issuer : &c->info->subject; /* CERT_NAME_ISSUER_FLAG */
  if (type == 3 && para) {
    const char* oid = (const char*)para;
    if (strcmp(oid, "2.5.4.10") == 0) which = 10;
    else if (strcmp(oid, "2.5.4.11") == 0) which = 11;
    else if (strcmp(oid, "2.5.4.6") == 0) which = 6;
    else if (strcmp(oid, "2.5.4.7") == 0) which = 7;
    else if (strcmp(oid, "2.5.4.8") == 0) which = 8;
  }
  if (!name_attr(name->pb, name->cb, which, out, (int)sizeof(out)) && type != 3)
    name_attr(name->pb, name->cb, 10, out, (int)sizeof(out));
  return put16(out, buf, n);
}
/* "CN=..., O=..., L=..., S=..., C=..." */
static WINAPI DWORD CertNameToStrW(DWORD enc, const struct blob* name, DWORD type, wchar16* buf, DWORD n) {
  static const struct {
    uint8_t id;
    const char* key;
  } parts[] = {{3, "CN="}, {10, "O="}, {11, "OU="}, {7, "L="}, {8, "S="}, {6, "C="}};
  char out[400], v[128];
  size_t i;
  (void)enc;
  if (wine_debug) wlogx("CertNameToStr tipo ", type);
  out[0] = 0;
  for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
    int quote;
    if (!name_attr(name->pb, name->cb, parts[i].id, v, (int)sizeof(v))) continue;
    if (out[0]) strcat(out, ", ");
    strcat(out, parts[i].key);
    quote = strchr(v, ',') != 0;
    if (quote) strcat(out, "\"");
    strcat(out, v);
    if (quote) strcat(out, "\"");
  }
  return put16(out, buf, n);
}

/* WinVerifyTrust: so "confia" com o opt-in, e avisa. */
static WINAPI long WinVerifyTrust(HANDLE w, const void* action, void* data) {
  (void)w; (void)action; (void)data;
  if (wine_noverify()) {
    wlog("AVISO: WinVerifyTrust respondeu OK SEM conferir a assinatura (WINE_NOVERIFY=1)", 0);
    return 0;
  }
  wlog("WinVerifyTrust: assinatura nao conferida (sem Authenticode no SODOS; SET WINE_NOVERIFY=1 aceita)", 0);
  return (long)0x800B0100; /* TRUST_E_NOSIGNATURE */
}
static WINAPI long WinVerifyTrustEx(HANDLE w, const void* action, void* data) { return WinVerifyTrust(w, action, data); }

#define S(n) {#n, (void*)n}
const struct wine_sym sig_syms[] = {S(CryptQueryObject), S(CryptMsgGetParam), S(CryptMsgClose),
                                    S(CertFindCertificateInStore), S(CertGetNameStringW), S(CertNameToStrW),
                                    S(WinVerifyTrust), S(WinVerifyTrustEx), {0, 0}};
