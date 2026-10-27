/* x509.c - le o certificado do handshake TLS 1.3 e confere tres coisas:
 * o nome do site, a assinatura do servidor (CertificateVerify) e a cadeia
 * ate uma raiz conhecida. RSA com SHA-256/384 (PKCS#1 no certificado, PSS
 * SHA-256 no TLS) e ECDSA P-256/P-384. A data vem de k_date (AAAA MM DD);
 * sem relogio, a validade nao e barrada. */
#include "x509.h"
#include "p256-m.h"
#include "p384.h"
#include "roots.h"
#include <so.h>
#include <string.h>

#define L 128 /* 4096 bits */

typedef struct {
  int kind; /* 1 = RSA, 2 = P-256, 3 = P-384 */
  u32 n[L];
  int nbytes;
  u32 e;
  u8 xy[96];
} pubkey;

typedef struct {
  const u8* tbs;
  int tbs_len;
  int sig_kind; /* 1 = RSA SHA-256, 2 = ECDSA SHA-256, 3 = ECDSA SHA-384, 4 = RSA SHA-384 */
  const u8* sig;
  int sig_len;
  pubkey key;
  const u8* issuer;
  int issuer_len;
  const u8* subject;
  int subject_len;
  char cn[256];
  const u8* san;
  int san_len;
  int not_before, not_after; /* AAAAMMDD, 0 se ausente */
} certinfo;

static void why_set(char* why, int max, const char* s) {
  if (!why || max < 2) return;
  strncpy(why, s, (size_t)max - 1);
  why[max - 1] = 0;
}

/* ---------- DER ---------- */
static int der_hdr(const u8* p, int n, int* len, int* hdr) {
  int nb, i, v;
  if (n < 2) return -1;
  if ((p[1] & 0x80) == 0) {
    *len = p[1];
    *hdr = 2;
  } else {
    nb = p[1] & 0x7f;
    if (nb < 1 || nb > 3 || n < 2 + nb) return -1;
    v = 0;
    for (i = 0; i < nb; i++) v = (v << 8) | p[2 + i];
    *len = v;
    *hdr = 2 + nb;
  }
  if (*hdr + *len > n) return -1;
  return 0;
}

static int der_take(const u8** p, int* n, int tag, const u8** body, int* blen) {
  int len, hdr;
  if (*n < 1 || (*p)[0] != tag) return -1;
  if (der_hdr(*p, *n, &len, &hdr) < 0) return -1;
  *body = *p + hdr;
  *blen = len;
  *p += hdr + len;
  *n -= hdr + len;
  return 0;
}

static int der_skip(const u8** p, int* n) {
  int len, hdr;
  if (*n < 1) return -1;
  if (der_hdr(*p, *n, &len, &hdr) < 0) return -1;
  *p += hdr + len;
  *n -= hdr + len;
  return 0;
}

static int oid_is(const u8* p, int n, const u8* oid, int on) {
  int len, hdr;
  if (n < 1 || p[0] != 0x06) return 0;
  if (der_hdr(p, n, &len, &hdr) < 0) return 0;
  return len == on && memcmp(p + hdr, oid, (size_t)on) == 0;
}

/* ---------- RSA ---------- */
static void bn_from_be(u32* a, const u8* p, int n) {
  int i;
  memset(a, 0, L * 4);
  for (i = 0; i < n && i < L * 4; i++) {
    int off = n - 1 - i;
    a[off / 4] |= (u32)p[i] << ((off % 4) * 8);
  }
}

static int bn_bitlen(const u32* a) {
  int i, b;
  for (i = L - 1; i >= 0; i--) {
    if (!a[i]) continue;
    for (b = 31; b >= 0; b--)
      if (a[i] & (1u << b)) return i * 32 + b + 1;
  }
  return 0;
}

static int bn_cmp(const u32* a, const u32* b, int n) {
  int i;
  for (i = n - 1; i >= 0; i--) {
    if (a[i] > b[i]) return 1;
    if (a[i] < b[i]) return -1;
  }
  return 0;
}

static void bn_mul(u32* dst, const u32* a, const u32* b) {
  int i, j;
  memset(dst, 0, L * 2 * 4);
  for (i = 0; i < L; i++) {
    u32 carry = 0;
    if (!a[i]) continue;
    for (j = 0; j < L; j++) {
      u64 t = (u64)a[i] * b[j] + dst[i + j] + carry;
      dst[i + j] = (u32)t;
      carry = (u32)(t >> 32);
    }
    dst[i + L] = carry;
  }
}

/* resto de prod (2L limbs) modulo mod (L limbs). rem recebe L limbs. */
static void bn_mod(u32* rem, const u32* prod, const u32* mod) {
  u32 acc[L + 1];
  int bits = 0, i, b;
  memset(acc, 0, sizeof acc);
  for (i = L * 2 - 1; i >= 0; i--)
    if (prod[i]) {
      bits = i * 32;
      for (b = 31; b >= 0; b--)
        if (prod[i] & (1u << b)) {
          bits += b + 1;
          break;
        }
      break;
    }
  for (i = bits - 1; i >= 0; i--) {
    u32 c = 0;
    int k;
    for (k = 0; k < L + 1; k++) {
      u32 v = (acc[k] << 1) | c;
      c = acc[k] >> 31;
      acc[k] = v;
    }
    if ((prod[i / 32] >> (i % 32)) & 1) acc[0] |= 1;
    if (acc[L] || bn_cmp(acc, mod, L) >= 0) {
      u32 borrow = 0;
      int k;
      for (k = 0; k < L; k++) {
        u64 d = (u64)acc[k] - mod[k] - borrow;
        acc[k] = (u32)d;
        borrow = (d >> 32) ? 1u : 0u;
      }
      acc[L] -= borrow;
    }
  }
  memcpy(rem, acc, L * 4);
}

static void bn_modexp_e(u32* out, const u32* base, u32 e, const u32* mod) {
  u32 result[L], b[L], prod[L * 2];
  memset(result, 0, sizeof result);
  result[0] = 1;
  memcpy(b, base, sizeof b);
  while (e) {
    if (e & 1) {
      bn_mul(prod, result, b);
      bn_mod(result, prod, mod);
    }
    e >>= 1;
    if (e) {
      bn_mul(prod, b, b);
      bn_mod(b, prod, mod);
    }
  }
  memcpy(out, result, sizeof result);
}

static void bn_to_be(const u32* a, u8* out, int n) {
  int i;
  for (i = 0; i < n; i++) {
    int off = n - 1 - i;
    out[i] = (u8)(a[off / 4] >> ((off % 4) * 8));
  }
}

static const u8 SHA256_INFO[] = {
  0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01,
  0x05, 0x00, 0x04, 0x20
};
static const u8 SHA384_INFO[] = {
  0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02,
  0x05, 0x00, 0x04, 0x30
};

static int rsa_pkcs1(const pubkey* k, const u8* hash, int hlen, const u8* info, int ilen,
                     const u8* sig, int sig_len) {
  u32 s[L], m[L];
  u8 em[512];
  int klen, i, ps;
  if (k->kind != 1 || k->nbytes < 64 || k->nbytes > 512 || sig_len != k->nbytes) return 0;
  bn_from_be(s, sig, sig_len);
  if (bn_cmp(s, k->n, L) >= 0) return 0;
  bn_modexp_e(m, s, k->e, k->n);
  klen = k->nbytes;
  bn_to_be(m, em, klen);
  if (em[0] != 0x00 || em[1] != 0x01) return 0;
  i = 2;
  while (i < klen && em[i] == 0xff) i++;
  ps = i - 2;
  if (ps < 8 || i >= klen || em[i] != 0x00) return 0;
  i++;
  if (klen - i != ilen + hlen) return 0;
  if (memcmp(em + i, info, (size_t)ilen) != 0) return 0;
  return memcmp(em + i + ilen, hash, (size_t)hlen) == 0;
}

static int rsa_pkcs1_sha256(const pubkey* k, const u8 hash[32], const u8* sig, int sig_len) {
  return rsa_pkcs1(k, hash, 32, SHA256_INFO, (int)sizeof SHA256_INFO, sig, sig_len);
}

static void mgf1(u8* out, int len, const u8* seed, int slen) {
  u8 block[32 + 4];
  u8 dig[32];
  int off = 0, c = 0;
  memcpy(block, seed, (size_t)slen);
  while (off < len) {
    int n = len - off;
    block[slen] = (u8)(c >> 24);
    block[slen + 1] = (u8)(c >> 16);
    block[slen + 2] = (u8)(c >> 8);
    block[slen + 3] = (u8)c;
    sha256(block, slen + 4, dig);
    if (n > 32) n = 32;
    memcpy(out + off, dig, (size_t)n);
    off += n;
    c++;
  }
}

static int rsa_pss_sha256(const pubkey* k, const u8 mhash[32], const u8* sig, int sig_len) {
  u32 s[L], m[L];
  u8 em[512], db[512], mask[512], mp[8 + 32 + 32], h2[32];
  int em_bits, em_len, db_len, i, pad, hlen = 32, slen = 32;
  if (k->kind != 1 || sig_len != k->nbytes || k->nbytes > 512) return 0;
  bn_from_be(s, sig, sig_len);
  if (bn_cmp(s, k->n, L) >= 0) return 0;
  bn_modexp_e(m, s, k->e, k->n);
  em_bits = bn_bitlen(k->n) - 1;
  em_len = (em_bits + 7) / 8;
  if (em_len > k->nbytes || em_len < hlen + slen + 2) return 0;
  {
    u8 full[512];
    int zbits = 8 * em_len - em_bits;
    bn_to_be(m, full, k->nbytes);
    memcpy(em, full + (k->nbytes - em_len), (size_t)em_len);
    if (zbits > 0 && (em[0] >> (8 - zbits)) != 0) return 0;
  }
  if (em[em_len - 1] != 0xbc) return 0;
  db_len = em_len - hlen - 1;
  mgf1(mask, db_len, em + db_len, hlen);
  for (i = 0; i < db_len; i++) db[i] = (u8)(em[i] ^ mask[i]);
  if (8 * em_len != em_bits) {
    int z = 8 * em_len - em_bits;
    db[0] &= (u8)(0xff >> z);
  }
  pad = db_len - slen - 1;
  for (i = 0; i < pad; i++)
    if (db[i] != 0) return 0;
  if (db[pad] != 0x01) return 0;
  memset(mp, 0, 8);
  memcpy(mp + 8, mhash, 32);
  memcpy(mp + 40, db + pad + 1, 32);
  sha256(mp, 72, h2);
  return memcmp(h2, em + db_len, 32) == 0;
}

/* ---------- certificado ---------- */
static int parse_time(const u8* p, int n, int* ymd) {
  int len, hdr, y, mo, d;
  char b[16];
  if (n < 2 || (p[0] != 0x17 && p[0] != 0x18)) return -1;
  if (der_hdr(p, n, &len, &hdr) < 0 || len < 8 || len > 15) return -1;
  memcpy(b, p + hdr, (size_t)len);
  b[len] = 0;
  if (p[0] == 0x17) {
    y = (b[0] - '0') * 10 + (b[1] - '0');
    y += y >= 50 ? 1900 : 2000;
    mo = (b[2] - '0') * 10 + (b[3] - '0');
    d = (b[4] - '0') * 10 + (b[5] - '0');
  } else {
    y = (b[0] - '0') * 1000 + (b[1] - '0') * 100 + (b[2] - '0') * 10 + (b[3] - '0');
    mo = (b[4] - '0') * 10 + (b[5] - '0');
    d = (b[6] - '0') * 10 + (b[7] - '0');
  }
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return -1;
  *ymd = y * 10000 + mo * 100 + d;
  return hdr + len;
}

static void grab_cn(certinfo* c, const u8* name, int nlen) {
  const u8* p = name;
  int n = nlen;
  const u8* setb;
  int setl;
  static const u8 cn_oid[] = { 0x55, 0x04, 0x03 };
  while (n > 0 && der_take(&p, &n, 0x31, &setb, &setl) == 0) {
    const u8* q = setb;
    int qn = setl;
    const u8* atb;
    int atl;
    while (qn > 0 && der_take(&q, &qn, 0x30, &atb, &atl) == 0) {
      const u8* a = atb;
      int an = atl;
      const u8* vb;
      int vl;
      if (!oid_is(a, an, cn_oid, 3)) continue;
      if (der_skip(&a, &an) < 0 || an < 2) continue;
      if (der_hdr(a, an, &vl, &vl) < 0) continue;
      {
        int hdr, len;
        if (der_hdr(a, an, &len, &hdr) < 0) continue;
        vb = a + hdr;
        if (len > (int)sizeof(c->cn) - 1) len = (int)sizeof(c->cn) - 1;
        memcpy(c->cn, vb, (size_t)len);
        c->cn[len] = 0;
      }
    }
  }
}

static int parse_spki(pubkey* k, const u8* spki, int n) {
  const u8* p = spki, *alg, *bits, *inner;
  int pn = n, al, bl, il, hdr, len;
  static const u8 rsa[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
  static const u8 ec[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
  static const u8 p256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
  static const u8 p384[] = { 0x2b, 0x81, 0x04, 0x00, 0x22 };
  const u8* nb;
  const u8* eb;
  int nl, el;
  memset(k, 0, sizeof(*k));
  if (der_take(&p, &pn, 0x30, &alg, &al) < 0) return -1;
  if (der_take(&p, &pn, 0x03, &bits, &bl) < 0 || bl < 2 || bits[0] != 0) return -1;
  if (oid_is(alg, al, rsa, 9)) {
    const u8* r = bits + 1;
    int rn = bl - 1;
    if (der_take(&r, &rn, 0x30, &inner, &il) < 0) return -1;
    r = inner;
    rn = il;
    if (der_take(&r, &rn, 0x02, &nb, &nl) < 0) return -1;
    if (der_take(&r, &rn, 0x02, &eb, &el) < 0 || el < 1 || el > 4) return -1;
    if (nl > 1 && nb[0] == 0) {
      nb++;
      nl--;
    }
    if (nl < 64 || nl > 512) return -1;
    k->kind = 1;
    k->nbytes = nl;
    bn_from_be(k->n, nb, nl);
    k->e = 0;
    for (nl = 0; nl < el; nl++) k->e = (k->e << 8) | eb[nl];
    return k->e ? 0 : -1;
  }
  if (oid_is(alg, al, ec, 7)) {
    const u8* a = alg;
    int an = al;
    if (der_skip(&a, &an) < 0) return -1;
    if (oid_is(a, an, p256, 8)) {
      if (bl != 66 || bits[1] != 0x04) return -1;
      k->kind = 2;
      memcpy(k->xy, bits + 2, 64);
      return 0;
    }
    if (oid_is(a, an, p384, 5)) {
      if (bl != 98 || bits[1] != 0x04) return -1;
      k->kind = 3;
      memcpy(k->xy, bits + 2, 96);
      return 0;
    }
    return -1;
  }
  (void)hdr;
  (void)len;
  return -1;
}

static int parse_cert(const u8* der, int der_len, certinfo* c) {
  const u8* p = der;
  int n = der_len;
  const u8* top, *alg, *bits, *q;
  int topl, al, bl, qn, len, hdr;
  static const u8 sha256rsa[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
  static const u8 sha384rsa[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c };
  static const u8 sha256ec[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
  static const u8 sha384ec[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03 };
  static const u8 san_oid[] = { 0x55, 0x1d, 0x11 };
  memset(c, 0, sizeof(*c));
  if (der_take(&p, &n, 0x30, &top, &topl) < 0) return -1;
  q = top;
  qn = topl;
  if (qn < 1 || q[0] != 0x30) return -1;
  if (der_hdr(q, qn, &len, &hdr) < 0) return -1;
  c->tbs = q;
  c->tbs_len = hdr + len;
  q += c->tbs_len;
  qn -= c->tbs_len;
  if (der_take(&q, &qn, 0x30, &alg, &al) < 0) return -1;
  if (der_take(&q, &qn, 0x03, &bits, &bl) < 0 || bl < 2 || bits[0] != 0) return -1;
  c->sig = bits + 1;
  c->sig_len = bl - 1;
  if (oid_is(alg, al, sha256rsa, 9)) c->sig_kind = 1;
  else if (oid_is(alg, al, sha256ec, 8)) c->sig_kind = 2;
  else if (oid_is(alg, al, sha384ec, 8)) c->sig_kind = 3;
  else if (oid_is(alg, al, sha384rsa, 9)) c->sig_kind = 4;
  else return -1;
  q = c->tbs;
  qn = c->tbs_len;
  if (der_take(&q, &qn, 0x30, &top, &topl) < 0) return -1;
  q = top;
  qn = topl;
  if (qn > 0 && q[0] == 0xa0 && der_skip(&q, &qn) < 0) return -1;
  if (der_skip(&q, &qn) < 0) return -1;                 /* serial */
  if (der_skip(&q, &qn) < 0) return -1;                 /* sigalg */
  if (qn < 1 || q[0] != 0x30) return -1;
  if (der_hdr(q, qn, &len, &hdr) < 0) return -1;
  c->issuer = q;
  c->issuer_len = hdr + len;
  q += hdr + len;
  qn -= hdr + len;
  {
    const u8* val;
    int vl, used;
    if (der_take(&q, &qn, 0x30, &val, &vl) < 0) return -1;
    used = parse_time(val, vl, &c->not_before);
    if (used < 0 || parse_time(val + used, vl - used, &c->not_after) < 0) return -1;
  }
  if (qn < 1 || q[0] != 0x30) return -1;
  if (der_hdr(q, qn, &len, &hdr) < 0) return -1;
  c->subject = q;
  c->subject_len = hdr + len;
  grab_cn(c, q + hdr, len);
  q += hdr + len;
  qn -= hdr + len;
  if (qn < 1 || q[0] != 0x30) return -1;
  if (der_hdr(q, qn, &len, &hdr) < 0) return -1;
  if (parse_spki(&c->key, q + hdr, len) < 0) return -1;
  q += hdr + len;
  qn -= hdr + len;
  while (qn > 0) {
    if (q[0] == 0xa3) {
      const u8* ext, *eb, *one, *ob;
      int el, ebl, ol, obl;
      if (der_take(&q, &qn, 0xa3, &ext, &el) < 0) return -1;
      if (der_take(&ext, &el, 0x30, &eb, &ebl) < 0) return -1;
      while (ebl > 0 && der_take(&eb, &ebl, 0x30, &one, &ol) == 0) {
        const u8* a = one;
        int an = ol;
        const u8* oct;
        int octl;
        if (!oid_is(a, an, san_oid, 3)) continue;
        if (der_skip(&a, &an) < 0) continue;
        if (an > 0 && a[0] == 0x01 && der_skip(&a, &an) < 0) continue;
        if (der_take(&a, &an, 0x04, &oct, &octl) < 0) continue;
        c->san = oct;
        c->san_len = octl;
      }
      (void)ob;
      (void)obl;
    } else if (der_skip(&q, &qn) < 0) return -1;
  }
  return 0;
}

static int ci_eq(const char* a, int an, const char* b) {
  int i;
  for (i = 0; i < an; i++) {
    int ca = a[i], cb = b[i];
    if (!cb) return 0;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb) return 0;
  }
  return b[an] == 0;
}

static int dns_match(const char* host, const char* pat, int pn) {
  char tmp[256];
  const char* dot;
  if (pn < 1 || pn > 253) return 0;
  if (pat[0] == '*' && pn > 2 && pat[1] == '.') {
    dot = strchr(host, '.');
    if (!dot || dot == host) return 0;
    return ci_eq(pat + 2, pn - 2, dot + 1);
  }
  if (pn > 255) return 0;
  memcpy(tmp, pat, (size_t)pn);
  tmp[pn] = 0;
  return ci_eq(tmp, pn, host);
}

static int name_ok(const certinfo* c, const char* host) {
  const u8* p;
  int n;
  int saw = 0;
  if (c->san && c->san_len > 0) {
    const u8* body;
    int bl;
    p = c->san;
    n = c->san_len;
    if (der_take(&p, &n, 0x30, &body, &bl) < 0) return 0;
    p = body;
    n = bl;
    while (n > 0) {
      int len, hdr;
      if (der_hdr(p, n, &len, &hdr) < 0) return 0;
      if (p[0] == 0x82) {
        saw = 1;
        if (dns_match(host, (const char*)p + hdr, len)) return 1;
      }
      p += hdr + len;
      n -= hdr + len;
    }
    if (saw) return 0;
  }
  return c->cn[0] && dns_match(host, c->cn, (int)strlen(c->cn));
}

static int date_ok(const certinfo* c) {
  int today = k_date();
  if (today < 20200101 || today > 21000101) return 1;
  if (c->not_before && today < c->not_before) return 0;
  if (c->not_after && today > c->not_after) return 0;
  return 1;
}

static int ecdsa_der_to_raw(const u8* sig, int n, u8* raw, int comp) {
  const u8* p = sig, *body, *rb, *sb;
  int pn = n, bl, rl, sl;
  if (der_take(&p, &pn, 0x30, &body, &bl) < 0) return -1;
  p = body;
  pn = bl;
  if (der_take(&p, &pn, 0x02, &rb, &rl) < 0) return -1;
  if (der_take(&p, &pn, 0x02, &sb, &sl) < 0) return -1;
  while (rl > 1 && rb[0] == 0) {
    rb++;
    rl--;
  }
  while (sl > 1 && sb[0] == 0) {
    sb++;
    sl--;
  }
  if (rl > comp || sl > comp || rl < 1 || sl < 1) return -1;
  memset(raw, 0, (size_t)comp * 2);
  memcpy(raw + comp - rl, rb, (size_t)rl);
  memcpy(raw + comp * 2 - sl, sb, (size_t)sl);
  return 0;
}

static int sig_ok(const certinfo* child, const pubkey* issuer) {
  u8 hash[48];
  int hlen;
  if (child->sig_kind == 1 || child->sig_kind == 2) {
    sha256(child->tbs, child->tbs_len, hash);
    hlen = 32;
  } else if (child->sig_kind == 3 || child->sig_kind == 4) {
    sha384(child->tbs, child->tbs_len, hash);
    hlen = 48;
  } else return 0;
  if (child->sig_kind == 1) return rsa_pkcs1_sha256(issuer, hash, child->sig, child->sig_len);
  if (child->sig_kind == 4)
    return rsa_pkcs1(issuer, hash, 48, SHA384_INFO, (int)sizeof SHA384_INFO, child->sig, child->sig_len);
  if (issuer->kind == 2) {
    u8 raw[64];
    if (ecdsa_der_to_raw(child->sig, child->sig_len, raw, 32) < 0) return 0;
    return p256_ecdsa_verify(raw, issuer->xy, hash, (size_t)hlen) == 0;
  }
  if (issuer->kind == 3) {
    u8 raw[96];
    if (ecdsa_der_to_raw(child->sig, child->sig_len, raw, 48) < 0) return 0;
    return p384_ecdsa_verify(raw, issuer->xy, hash, (size_t)hlen) == 0;
  }
  return 0;
}

static int cv_ok(const pubkey* key, int scheme, const u8* sig, int sig_len, const u8 th[32]) {
  /* 64 vezes 0x20, o texto de 33 bytes, um 0x00 e o hash do transcript. */
  u8 msg[64 + 33 + 1 + 32];
  u8 hash[32];
  memset(msg, 0x20, 64);
  memcpy(msg + 64, "TLS 1.3, server CertificateVerify", 33);
  msg[97] = 0;
  memcpy(msg + 98, th, 32);
  sha256(msg, 130, hash);
  if (scheme == 0x0403 && key->kind == 2) {
    u8 raw[64];
    /* TLS 1.3 manda o ECDSA em DER (SEQUENCE de r e s), nao os 64 bytes crus. */
    if (ecdsa_der_to_raw(sig, sig_len, raw, 32) < 0) {
      if (sig_len != 64) return 0;
      memcpy(raw, sig, 64);
    }
    return p256_ecdsa_verify(raw, key->xy, hash, 32) == 0;
  }
  if (scheme == 0x0804 && key->kind == 1) return rsa_pss_sha256(key, hash, sig, sig_len);
  return 0;
}

static pubkey root_key[160];
static const u8* root_issuer_match[160];
static int root_issuer_len[160];
static int nroots = -1;

static void load_roots(void) {
  int i;
  if (nroots >= 0) return;
  nroots = 0;
  for (i = 0; i < ca_root_n && nroots < 160; i++) {
    certinfo c;
    if (parse_cert(ca_roots[i].der, ca_roots[i].len, &c) != 0) continue;
    root_key[nroots] = c.key;
    root_issuer_match[nroots] = c.subject;
    root_issuer_len[nroots] = c.subject_len;
    nroots++;
  }
}

static int key_eq(const pubkey* a, const pubkey* b) {
  if (a->kind != b->kind || a->kind == 0) return 0;
  if (a->kind == 1)
    return a->nbytes == b->nbytes && a->e == b->e && bn_cmp(a->n, b->n, L) == 0;
  return memcmp(a->xy, b->xy, a->kind == 2 ? 64 : 96) == 0;
}

static int root_ok(const certinfo* top) {
  int i, any = 0;
  load_roots();
  /* copia cruzada de uma raiz: a chave publica e a de uma ancora, mesmo que
   * quem assinou essa copia nao esteja mais na lista. */
  for (i = 0; i < nroots; i++)
    if (key_eq(&top->key, &root_key[i])) return 1;
  for (i = 0; i < nroots; i++) {
    if (root_issuer_len[i] == top->issuer_len &&
        memcmp(root_issuer_match[i], top->issuer, (size_t)top->issuer_len) == 0) {
      any = 1;
      if (sig_ok(top, &root_key[i])) return 1;
    }
  }
  if (any) return 0;
  for (i = 0; i < nroots; i++)
    if (sig_ok(top, &root_key[i])) return 1;
  return 0;
}

static int read_u24(const u8* p) { return (p[0] << 16) | (p[1] << 8) | p[2]; }

int x509_tls_check(const char* host, const u8* msg, int msg_len, int scheme, const u8* sig,
                   int sig_len, const u8 th[32], char* why, int why_max) {
  certinfo chain[6];
  int nc = 0, p, i, ctx, list, end;
  if (!msg || msg_len < 4 || !sig || sig_len < 1) {
    why_set(why, why_max, "sem certificado");
    return -1;
  }
  ctx = msg[0];
  p = 1 + ctx;
  if (p + 3 > msg_len) {
    why_set(why, why_max, "certificado ilegivel");
    return -1;
  }
  list = read_u24(msg + p);
  p += 3;
  end = p + list;
  if (ctx > 32 || end > msg_len) {
    why_set(why, why_max, "certificado ilegivel");
    return -1;
  }
  while (p + 3 <= end && nc < 6) {
    int clen = read_u24(msg + p);
    int ext;
    p += 3;
    if (clen < 1 || p + clen + 2 > end) {
      why_set(why, why_max, "certificado ilegivel");
      return -1;
    }
    if (parse_cert(msg + p, clen, &chain[nc]) != 0) {
      why_set(why, why_max, "algoritmo do certificado nao suportado");
      return -1;
    }
    nc++;
    p += clen;
    ext = (msg[p] << 8) | msg[p + 1];
    p += 2 + ext;
  }
  if (nc < 1) {
    why_set(why, why_max, "sem certificado");
    return -1;
  }
  if (!name_ok(&chain[0], host)) {
    why_set(why, why_max, "nome nao confere");
    return -1;
  }
  for (i = 0; i < nc; i++) {
    if (!date_ok(&chain[i])) {
      why_set(why, why_max, "fora da validade");
      return -1;
    }
  }
  if (scheme != 0x0403 && scheme != 0x0804) {
    why_set(why, why_max, "assinatura TLS nao suportada");
    return -1;
  }
  if (!cv_ok(&chain[0].key, scheme, sig, sig_len, th)) {
    why_set(why, why_max, "assinatura do servidor invalida");
    return -1;
  }
  for (i = 0; i < nc - 1; i++) {
    if (!sig_ok(&chain[i], &chain[i + 1].key)) {
      why_set(why, why_max, "cadeia invalida");
      return -1;
    }
  }
  if (!root_ok(&chain[nc - 1])) {
    why_set(why, why_max, "raiz desconhecida");
    return -1;
  }
  why_set(why, why_max, "");
  return 0;
}
