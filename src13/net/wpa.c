/* WPA2-PSK: SHA1/HMAC/PBKDF2, AES-128, key unwrap, CCMP e o suplicante
 * (handshake de 4 vias e de grupo). Ver wpa.h. */
#include "wpa.h"

static void wcopy(void* d, const void* s, int n) {
  uint8_t* a = (uint8_t*)d;
  const uint8_t* b = (const uint8_t*)s;
  while (n-- > 0) *a++ = *b++;
}

static void wzero(void* d, int n) {
  uint8_t* a = (uint8_t*)d;
  while (n-- > 0) *a++ = 0;
}

static int wdiff(const void* a, const void* b, int n) {
  const uint8_t* x = (const uint8_t*)a;
  const uint8_t* y = (const uint8_t*)b;
  uint8_t d = 0;
  while (n-- > 0) d |= (uint8_t)(*x++ ^ *y++);
  return d != 0;
}

static uint32_t rd_be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr_be32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* ---------- SHA1 ---------- */

typedef struct {
  uint32_t h[5];
  uint8_t buf[64];
  int n;
  uint64_t total;
} sha1_ctx;

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(sha1_ctx* c, const uint8_t* p) {
  uint32_t w[80];
  uint32_t a, b, cc, d, e, t;
  int i;
  for (i = 0; i < 16; i++) w[i] = rd_be32(p + i * 4);
  for (i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
  for (i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999u; }
    else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1u; }
    else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDCu; }
    else { f = b ^ cc ^ d; k = 0xCA62C1D6u; }
    t = rol(a, 5) + f + e + k + w[i];
    e = d; d = cc; cc = rol(b, 30); b = a; a = t;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(sha1_ctx* c) {
  c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
  c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
  c->n = 0;
  c->total = 0;
}

static void sha1_update(sha1_ctx* c, const void* data, int len) {
  const uint8_t* p = (const uint8_t*)data;
  c->total += (uint64_t)len;
  while (len > 0) {
    int k = 64 - c->n;
    if (k > len) k = len;
    wcopy(c->buf + c->n, p, k);
    c->n += k; p += k; len -= k;
    if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
  }
}

static void sha1_final(sha1_ctx* c, uint8_t out[20]) {
  uint64_t bits = c->total * 8u;
  uint8_t pad = 0x80;
  uint8_t lenb[8];
  int i;
  sha1_update(c, &pad, 1);
  pad = 0;
  while (c->n != 56) sha1_update(c, &pad, 1);
  for (i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i * 8));
  sha1_update(c, lenb, 8);
  for (i = 0; i < 5; i++) wr_be32(out + i * 4, c->h[i]);
}

void wpa_sha1(const void* data, int len, uint8_t out[20]) {
  sha1_ctx c;
  sha1_init(&c);
  sha1_update(&c, data, len);
  sha1_final(&c, out);
}

/* HMAC com a mensagem em ate 2 pedacos (PBKDF2 e PRF usam). */
static void hmac2(const uint8_t* key, int klen, const void* m1, int l1,
                  const void* m2, int l2, uint8_t out[20]) {
  uint8_t k[64];
  uint8_t pad[64];
  uint8_t inner[20];
  sha1_ctx c;
  int i;
  wzero(k, 64);
  if (klen > 64) wpa_sha1(key, klen, k);
  else wcopy(k, key, klen);
  for (i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x36);
  sha1_init(&c);
  sha1_update(&c, pad, 64);
  sha1_update(&c, m1, l1);
  if (l2) sha1_update(&c, m2, l2);
  sha1_final(&c, inner);
  for (i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x5C);
  sha1_init(&c);
  sha1_update(&c, pad, 64);
  sha1_update(&c, inner, 20);
  sha1_final(&c, out);
}

void wpa_hmac_sha1(const uint8_t* key, int klen, const void* msg, int mlen, uint8_t out[20]) {
  hmac2(key, klen, msg, mlen, 0, 0, out);
}

void wpa_pmk(const char* pass, const uint8_t* ssid, int ssid_len, uint8_t pmk[32]) {
  int plen = 0;
  int blk;
  while (pass[plen]) plen++;
  for (blk = 1; blk <= 2; blk++) {
    uint8_t cnt[4];
    uint8_t u[20];
    uint8_t t[20];
    int i, j;
    wr_be32(cnt, (uint32_t)blk);
    hmac2((const uint8_t*)pass, plen, ssid, ssid_len, cnt, 4, u);
    wcopy(t, u, 20);
    for (i = 1; i < 4096; i++) {
      hmac2((const uint8_t*)pass, plen, u, 20, 0, 0, u);
      for (j = 0; j < 20; j++) t[j] ^= u[j];
    }
    wcopy(pmk + (blk - 1) * 20, t, blk == 1 ? 20 : 12);
  }
}

/* PRF-n do 802.11i: HMAC-SHA1(key, label || 0 || data || i), i = 0, 1, ... */
static void prf_sha1(const uint8_t* key, int klen, const char* label,
                     const uint8_t* data, int dlen, uint8_t* out, int olen) {
  uint8_t msg[128];
  uint8_t h[20];
  int ll = 0;
  int pos = 0;
  uint8_t i = 0;
  while (label[ll]) ll++;
  wcopy(msg, label, ll);
  msg[ll] = 0;
  wcopy(msg + ll + 1, data, dlen);
  while (pos < olen) {
    int k = olen - pos < 20 ? olen - pos : 20;
    msg[ll + 1 + dlen] = i++;
    hmac2(key, klen, msg, ll + 2 + dlen, 0, 0, h);
    wcopy(out + pos, h, k);
    pos += k;
  }
}

/* ---------- AES-128 ---------- */

static uint8_t sbox[256];
static uint8_t isbox[256];
static int aes_ready;

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

static uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b) {
    if (b & 1) r ^= a;
    a = xt(a);
    b >>= 1;
  }
  return r;
}

static void aes_tables(void) {
  uint8_t p = 1, q = 1;
  if (aes_ready) return;
  /* p percorre o grupo multiplicativo (gerador 3), q = 1/p. */
  do {
    uint8_t x;
    p = (uint8_t)(p ^ xt(p));
    q ^= (uint8_t)(q << 1);
    q ^= (uint8_t)(q << 2);
    q ^= (uint8_t)(q << 4);
    if (q & 0x80) q ^= 0x09;
    x = (uint8_t)(q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^
                  (uint8_t)((q << 3) | (q >> 5)) ^ (uint8_t)((q << 4) | (q >> 4)));
    sbox[p] = (uint8_t)(x ^ 0x63);
  } while (p != 1);
  sbox[0] = 0x63;
  {
    int i;
    for (i = 0; i < 256; i++) isbox[sbox[i]] = (uint8_t)i;
  }
  aes_ready = 1;
}

void wpa_aes_init(wpa_aes* c, const uint8_t key[16]) {
  static const uint8_t rcon[10] = {1, 2, 4, 8, 16, 32, 64, 128, 0x1B, 0x36};
  int i;
  aes_tables();
  for (i = 0; i < 4; i++) c->rk[i] = rd_be32(key + i * 4);
  for (i = 4; i < 44; i++) {
    uint32_t t = c->rk[i - 1];
    if ((i & 3) == 0) {
      t = ((uint32_t)sbox[(t >> 16) & 255] << 24) | ((uint32_t)sbox[(t >> 8) & 255] << 16) |
          ((uint32_t)sbox[t & 255] << 8) | sbox[t >> 24];
      t ^= (uint32_t)rcon[i / 4 - 1] << 24;
    }
    c->rk[i] = c->rk[i - 4] ^ t;
  }
}

static void add_rk(uint8_t s[16], const uint32_t* rk) {
  int i;
  for (i = 0; i < 4; i++) {
    s[i * 4] ^= (uint8_t)(rk[i] >> 24);
    s[i * 4 + 1] ^= (uint8_t)(rk[i] >> 16);
    s[i * 4 + 2] ^= (uint8_t)(rk[i] >> 8);
    s[i * 4 + 3] ^= (uint8_t)rk[i];
  }
}

void wpa_aes_enc(const wpa_aes* c, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16], t[16];
  int r, i;
  wcopy(s, in, 16);
  add_rk(s, c->rk);
  for (r = 1; r <= 10; r++) {
    /* SubBytes + ShiftRows: coluna c, linha l vem de coluna (c+l)%4. */
    for (i = 0; i < 16; i++) t[i] = sbox[s[((i / 4 + i % 4) % 4) * 4 + i % 4]];
    if (r != 10) {
      for (i = 0; i < 4; i++) {
        uint8_t* a = t + i * 4;
        uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
        a[0] = (uint8_t)(a0 ^ all ^ xt((uint8_t)(a0 ^ a1)));
        a[1] = (uint8_t)(a1 ^ all ^ xt((uint8_t)(a1 ^ a2)));
        a[2] = (uint8_t)(a2 ^ all ^ xt((uint8_t)(a2 ^ a3)));
        a[3] = (uint8_t)(a3 ^ all ^ xt((uint8_t)(a3 ^ a0)));
      }
    }
    wcopy(s, t, 16);
    add_rk(s, c->rk + r * 4);
  }
  wcopy(out, s, 16);
}

void wpa_aes_dec(const wpa_aes* c, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16], t[16];
  int r, i;
  wcopy(s, in, 16);
  add_rk(s, c->rk + 40);
  for (r = 9; r >= 0; r--) {
    /* InvShiftRows + InvSubBytes: linha l da coluna c vai para (c+l)%4. */
    for (i = 0; i < 16; i++) t[((i / 4 + i % 4) % 4) * 4 + i % 4] = isbox[s[i]];
    add_rk(t, c->rk + r * 4);
    if (r != 0) {
      for (i = 0; i < 4; i++) {
        uint8_t* a = t + i * 4;
        uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        a[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
        a[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
        a[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
        a[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
      }
    }
    wcopy(s, t, 16);
  }
  wcopy(out, s, 16);
}

int wpa_aes_unwrap(const uint8_t kek[16], const uint8_t* in, int n, uint8_t* out) {
  wpa_aes c;
  uint8_t a[8], b[16];
  int blocks = n / 8 - 1;
  int i, j;
  if (n < 16 || (n & 7)) return -1;
  wpa_aes_init(&c, kek);
  wcopy(a, in, 8);
  wcopy(out, in + 8, n - 8);
  for (j = 5; j >= 0; j--) {
    for (i = blocks; i >= 1; i--) {
      uint32_t t = (uint32_t)(blocks * j + i);
      wcopy(b, a, 8);
      b[4] ^= (uint8_t)(t >> 24);
      b[5] ^= (uint8_t)(t >> 16);
      b[6] ^= (uint8_t)(t >> 8);
      b[7] ^= (uint8_t)t;
      wcopy(b + 8, out + (i - 1) * 8, 8);
      wpa_aes_dec(&c, b, b);
      wcopy(a, b, 8);
      wcopy(out + (i - 1) * 8, b + 8, 8);
    }
  }
  for (i = 0; i < 8; i++)
    if (a[i] != 0xA6) return -1;
  return 0;
}

/* ---------- CCMP ---------- */

/* AAD e nonce a partir do cabecalho 802.11 (IEEE 802.11-2016 12.5.3.3). */
static int ccmp_aad(const uint8_t* h, int hlen, uint8_t aad[32], uint8_t nonce[13],
                    const uint8_t pn[6]) {
  int qos = (h[0] & 0x0C) == 0x08 && (h[0] & 0x80);
  int a4 = (h[1] & 3) == 3;
  int mgmt = (h[0] & 0x0C) == 0x00;
  int n = 0;
  uint8_t prio = 0;
  int i;
  aad[n++] = (uint8_t)(mgmt ? h[0] : (h[0] & 0x8F));
  aad[n++] = (uint8_t)((h[1] & 0xC7) | 0x40);
  if (qos) aad[1] &= 0x7F;  /* bit Order mascarado em QoS */
  wcopy(aad + n, h + 4, 18);
  n += 18;
  aad[n++] = (uint8_t)(h[22] & 0x0F);
  aad[n++] = 0;
  if (a4) {
    wcopy(aad + n, h + 24, 6);
    n += 6;
  }
  if (qos) {
    int q = a4 ? 30 : 24;
    if (q + 2 > hlen) return -1;
    prio = (uint8_t)(h[q] & 0x0F);
    aad[n++] = prio;
    aad[n++] = 0;
  }
  nonce[0] = (uint8_t)(prio | (mgmt ? 0x10 : 0));
  wcopy(nonce + 1, h + 10, 6);
  for (i = 0; i < 6; i++) nonce[7 + i] = pn[5 - i];
  return n;
}

/* CBC-MAC e CTR do CCM com M = 8, L = 2. */
static void ccm(const wpa_aes* c, const uint8_t nonce[13], const uint8_t* aad, int alen,
                uint8_t* data, int dlen, int enc, uint8_t mic[8]) {
  uint8_t x[16], blk[16], a[16], s[16];
  int i, pos;
  x[0] = 0x59;
  wcopy(x + 1, nonce, 13);
  x[14] = (uint8_t)(dlen >> 8);
  x[15] = (uint8_t)dlen;
  wpa_aes_enc(c, x, x);
  /* AAD com o tamanho em 2 bytes na frente. */
  wzero(blk, 16);
  blk[0] = (uint8_t)(alen >> 8);
  blk[1] = (uint8_t)alen;
  pos = 2;
  for (i = 0; i < alen; i++) {
    blk[pos++] = aad[i];
    if (pos == 16) {
      for (pos = 0; pos < 16; pos++) x[pos] ^= blk[pos];
      wpa_aes_enc(c, x, x);
      wzero(blk, 16);
      pos = 0;
    }
  }
  if (pos) {
    for (i = 0; i < 16; i++) x[i] ^= blk[i];
    wpa_aes_enc(c, x, x);
  }
  a[0] = 0x01;
  wcopy(a + 1, nonce, 13);
  for (pos = 0; pos < dlen; pos += 16) {
    int k = dlen - pos < 16 ? dlen - pos : 16;
    uint16_t ctr = (uint16_t)(pos / 16 + 1);
    a[14] = (uint8_t)(ctr >> 8);
    a[15] = (uint8_t)ctr;
    wpa_aes_enc(c, a, s);
    if (enc)
      for (i = 0; i < k; i++) x[i] ^= data[pos + i];
    for (i = 0; i < k; i++) data[pos + i] ^= s[i];
    if (!enc)
      for (i = 0; i < k; i++) x[i] ^= data[pos + i];
    wpa_aes_enc(c, x, x);
  }
  a[14] = 0;
  a[15] = 0;
  wpa_aes_enc(c, a, s);
  for (i = 0; i < 8; i++) mic[i] = (uint8_t)(x[i] ^ s[i]);
}

void wpa_ccmp_encrypt(const uint8_t tk[16], uint64_t pn, int keyid,
                      uint8_t* mpdu, int hlen, int plen) {
  wpa_aes c;
  uint8_t aad[32], nonce[13], pnb[6];
  uint8_t* ch = mpdu + hlen;
  int alen, i;
  for (i = 0; i < 6; i++) pnb[i] = (uint8_t)(pn >> (8 * i));
  mpdu[1] |= 0x40;
  ch[0] = pnb[0];
  ch[1] = pnb[1];
  ch[2] = 0;
  ch[3] = (uint8_t)(0x20 | ((keyid & 3) << 6));
  ch[4] = pnb[2];
  ch[5] = pnb[3];
  ch[6] = pnb[4];
  ch[7] = pnb[5];
  alen = ccmp_aad(mpdu, hlen, aad, nonce, pnb);
  wpa_aes_init(&c, tk);
  ccm(&c, nonce, aad, alen, ch + 8, plen, 1, ch + 8 + plen);
}

int wpa_ccmp_decrypt(const uint8_t tk[16], uint8_t* mpdu, int hlen, int plen, uint64_t* pn_out) {
  wpa_aes c;
  uint8_t aad[32], nonce[13], pnb[6], mic[8];
  uint8_t* ch = mpdu + hlen;
  int alen, i;
  if (plen < 0 || !(ch[3] & 0x20)) return -1;
  pnb[0] = ch[0];
  pnb[1] = ch[1];
  pnb[2] = ch[4];
  pnb[3] = ch[5];
  pnb[4] = ch[6];
  pnb[5] = ch[7];
  alen = ccmp_aad(mpdu, hlen, aad, nonce, pnb);
  if (alen < 0) return -1;
  wpa_aes_init(&c, tk);
  ccm(&c, nonce, aad, alen, ch + 8, plen, 0, mic);
  if (pn_out) {
    *pn_out = 0;
    for (i = 5; i >= 0; i--) *pn_out = (*pn_out << 8) | pnb[i];
  }
  return wdiff(mic, ch + 8 + plen, 8) ? -1 : 0;
}

/* ---------- suplicante ---------- */

const uint8_t wpa_rsn_ie[22] = {
  0x30, 0x14, 0x01, 0x00,
  0x00, 0x0F, 0xAC, 0x04,              /* grupo: CCMP */
  0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,  /* par: CCMP */
  0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,  /* AKM: PSK */
  0x00, 0x00                           /* capacidades */
};

#define KI_VER     0x0007
#define KI_PAIR    0x0008
#define KI_INSTALL 0x0040
#define KI_ACK     0x0080
#define KI_MIC     0x0100
#define KI_SECURE  0x0200
#define KI_ENCDATA 0x1000

/* Deslocamentos dentro do EAPOL-Key (a partir do cabecalho 802.1X). */
#define EK_INFO   5
#define EK_REPLAY 9
#define EK_NONCE  17
#define EK_MIC    81
#define EK_DLEN   97
#define EK_DATA   99

static uint64_t rand_state;

static uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

/* Nonce: SHA1 do TSC, de um contador e do PMK, duas vezes. */
static void make_nonce(const struct wpa_sm* sm, uint8_t out[32]) {
  uint8_t in[48];
  uint8_t h[20];
  int i;
  for (i = 0; i < 2; i++) {
    uint64_t t = rdtsc() ^ (++rand_state * 0x9E3779B97F4A7C15ull);
    wcopy(in, &t, 8);
    wcopy(in + 8, sm->pmk, 32);
    wcopy(in + 40, sm->aa, 6);
    in[46] = (uint8_t)i;
    in[47] = (uint8_t)rand_state;
    wpa_sha1(in, 48, h);
    wcopy(out + i * 16, h, 16);
  }
}

static int mac_lt(const uint8_t* a, const uint8_t* b, int n) {
  int i;
  for (i = 0; i < n; i++)
    if (a[i] != b[i]) return a[i] < b[i];
  return 0;
}

static void derive_ptk(struct wpa_sm* sm) {
  uint8_t d[76];
  int lo = mac_lt(sm->aa, sm->spa, 6);
  int nlo = mac_lt(sm->anonce, sm->snonce, 32);
  wcopy(d, lo ? sm->aa : sm->spa, 6);
  wcopy(d + 6, lo ? sm->spa : sm->aa, 6);
  wcopy(d + 12, nlo ? sm->anonce : sm->snonce, 32);
  wcopy(d + 44, nlo ? sm->snonce : sm->anonce, 32);
  prf_sha1(sm->pmk, 32, "Pairwise key expansion", d, 76, sm->ptk, 48);
}

static void eapol_mic(const uint8_t kck[16], uint8_t* f, int len) {
  uint8_t h[20];
  wzero(f + EK_MIC, 16);
  hmac2(kck, 16, f, len, 0, 0, h);
  wcopy(f + EK_MIC, h, 16);
}

static int mic_ok(const uint8_t kck[16], const uint8_t* p, int len) {
  uint8_t tmp[512];
  uint8_t h[20];
  if (len > (int)sizeof(tmp)) return 0;
  wcopy(tmp, p, len);
  wzero(tmp + EK_MIC, 16);
  hmac2(kck, 16, tmp, len, 0, 0, h);
  return !wdiff(h, p + EK_MIC, 16);
}

/* Monta um EAPOL-Key de resposta. */
static int build_key(struct wpa_sm* sm, uint16_t info, const uint8_t* nonce,
                     const uint8_t* data, int dlen, uint8_t* out, int cap) {
  int len = EK_DATA + dlen;
  if (len > cap) return 0;
  wzero(out, len);
  out[0] = 1;  /* 802.1X-2001: o que os APs mais aceitam */
  out[1] = 3;
  out[2] = (uint8_t)((len - 4) >> 8);
  out[3] = (uint8_t)(len - 4);
  out[4] = 2;  /* RSN */
  out[EK_INFO] = (uint8_t)(info >> 8);
  out[EK_INFO + 1] = (uint8_t)info;
  wcopy(out + EK_REPLAY, sm->replay, 8);
  if (nonce) wcopy(out + EK_NONCE, nonce, 32);
  out[EK_DLEN] = (uint8_t)(dlen >> 8);
  out[EK_DLEN + 1] = (uint8_t)dlen;
  if (dlen) wcopy(out + EK_DATA, data, dlen);
  eapol_mic(sm->ptk, out, len);
  return len;
}

/* Procura o KDE do GTK (dd .. 00 0f ac 01) nos dados decifrados. */
static int take_gtk(struct wpa_sm* sm, const uint8_t* d, int n) {
  int i = 0;
  while (i + 2 <= n) {
    uint8_t t = d[i], l = d[i + 1];
    if (t == 0xDD && l == 0 && i + 2 == n) break;  /* preenchimento */
    if (i + 2 + l > n) return -1;
    if (t == 0xDD && l >= 6 + 16 && d[i + 2] == 0x00 && d[i + 3] == 0x0F &&
        d[i + 4] == 0xAC && d[i + 5] == 0x01) {
      int glen = l - 6;
      if (glen > 32) return -1;
      sm->gtk_idx = d[i + 6] & 3;
      wcopy(sm->gtk, d + i + 8, glen);
      sm->gtk_len = glen;
      return 0;
    }
    i += 2 + l;
  }
  return -1;
}

static int key_data(struct wpa_sm* sm, const uint8_t* p, int len, uint8_t* plain, int* plen) {
  int dlen = (p[EK_DLEN] << 8) | p[EK_DLEN + 1];
  if (EK_DATA + dlen > len || dlen > 256 || dlen < 24) return -1;
  if (wpa_aes_unwrap(sm->ptk + 16, p + EK_DATA, dlen, plain) != 0) return -1;
  *plen = dlen - 8;
  return 0;
}

void wpa_sm_init(struct wpa_sm* sm, const uint8_t pmk[32], const uint8_t aa[6],
                 const uint8_t spa[6]) {
  wzero(sm, (int)sizeof(*sm));
  wcopy(sm->pmk, pmk, 32);
  wcopy(sm->aa, aa, 6);
  wcopy(sm->spa, spa, 6);
  sm->tx_pn = 1;
}

int wpa_eapol_rx(struct wpa_sm* sm, const uint8_t* p, int len, uint8_t* out, int cap) {
  uint16_t info;
  int blen;
  uint8_t plain[256];
  int plen = 0;
  if (len < EK_DATA || p[1] != 3 || p[4] != 2) return -1;
  blen = ((p[2] << 8) | p[3]) + 4;
  if (blen < EK_DATA || blen > len) return -1;
  len = blen;
  info = (uint16_t)((p[EK_INFO] << 8) | p[EK_INFO + 1]);
  if ((info & KI_VER) != 2 || !(info & KI_ACK)) return -1;
  /* O contador de replay so pode subir. */
  if (sm->have_replay && (info & KI_MIC)) {
    int i, older = 1;
    for (i = 0; i < 8; i++)
      if (p[EK_REPLAY + i] != sm->replay[i]) { older = p[EK_REPLAY + i] < sm->replay[i]; break; }
    if (i < 8 && older) return -1;
  }
  if ((info & KI_PAIR) && !(info & KI_MIC)) {
    /* msg 1/4: ANonce. Responde com SNonce e o RSN IE. Uma msg 1 repetida
     * (mesmo ANonce) reaproveita o SNonce, como manda o 802.11. */
    int same = sm->state == WPA_PTK_START && !wdiff(sm->anonce, p + EK_NONCE, 32);
    wcopy(sm->anonce, p + EK_NONCE, 32);
    wcopy(sm->replay, p + EK_REPLAY, 8);
    sm->have_replay = 1;
    if (!same) {
      make_nonce(sm, sm->snonce);
      derive_ptk(sm);
    }
    sm->state = WPA_PTK_START;
    return build_key(sm, (uint16_t)(2 | KI_PAIR | KI_MIC), sm->snonce,
                     wpa_rsn_ie, (int)sizeof(wpa_rsn_ie), out, cap);
  }
  if ((info & KI_PAIR) && (info & KI_MIC)) {
    /* msg 3/4: confere MIC, pega o GTK, responde msg 4/4. */
    if (sm->state < WPA_PTK_START || wdiff(p + EK_NONCE, sm->anonce, 32)) return -1;
    if (!mic_ok(sm->ptk, p, len)) return -1;
    wcopy(sm->replay, p + EK_REPLAY, 8);
    if ((info & KI_ENCDATA) && key_data(sm, p, len, plain, &plen) == 0) take_gtk(sm, plain, plen);
    sm->state = sm->gtk_len ? WPA_DONE : WPA_PTK_DONE;
    return build_key(sm, (uint16_t)(2 | KI_PAIR | KI_MIC | KI_SECURE), 0, 0, 0, out, cap);
  }
  if (!(info & KI_PAIR) && (info & KI_MIC)) {
    /* Grupo 1/2: GTK novo. */
    if (sm->state < WPA_PTK_DONE || !mic_ok(sm->ptk, p, len)) return -1;
    wcopy(sm->replay, p + EK_REPLAY, 8);
    if (key_data(sm, p, len, plain, &plen) != 0 || take_gtk(sm, plain, plen) != 0) return -1;
    sm->state = WPA_DONE;
    return build_key(sm, (uint16_t)(2 | KI_MIC | KI_SECURE), 0, 0, 0, out, cap);
  }
  return -1;
}

/* ---------- testes ---------- */

static int hexeq(const uint8_t* got, const char* hex, int n) {
  int i;
  for (i = 0; i < n; i++) {
    int hi = hex[i * 2], lo = hex[i * 2 + 1];
    hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10;
    lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
    if (got[i] != (uint8_t)(hi * 16 + lo)) return 0;
  }
  return 1;
}

static void unhex(uint8_t* out, const char* hex, int n) {
  int i;
  for (i = 0; i < n; i++) {
    int hi = hex[i * 2], lo = hex[i * 2 + 1];
    hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10;
    lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
    out[i] = (uint8_t)(hi * 16 + lo);
  }
}

int wpa_selftest(void) {
  uint8_t h[64];
  uint8_t key[16], blk[16];
  wpa_aes c;
  int i;
  wpa_sha1("abc", 3, h);
  if (!hexeq(h, "a9993e364706816aba3e25717850c26c9cd0d89d", 20)) return 1;
  wpa_hmac_sha1((const uint8_t*)"Jefe", 4, "what do ya want for nothing?", 28, h);
  if (!hexeq(h, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", 20)) return 2;
  prf_sha1((const uint8_t*)"Jefe", 4, "prefix", (const uint8_t*)"what do ya want for nothing?",
           28, h, 64);
  if (!hexeq(h, "51f4de5b33f249adf81aeb713a3c20f4fe631446fabdfa58244759ae58ef9009"
                "a99abf4eac2ca5fa87e692c440eb40023e7babb206d61de7b92f41529092b8fc", 64)) return 3;
  for (i = 0; i < 16; i++) { key[i] = (uint8_t)i; blk[i] = (uint8_t)(i * 0x11); }
  wpa_aes_init(&c, key);
  wpa_aes_enc(&c, blk, h);
  if (!hexeq(h, "69c4e0d86a7b0430d8cdb78070b4c55a", 16)) return 4;
  wpa_aes_dec(&c, h, h);
  if (wdiff(h, blk, 16)) return 5;
  {
    uint8_t w[24];
    unhex(w, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", 24);
    if (wpa_aes_unwrap(key, w, 24, h) != 0) return 6;
    if (!hexeq(h, "00112233445566778899aabbccddeeff", 16)) return 6;
  }
  {
    /* IEEE 802.11-2016 J.6.4 (CCMP, MPDU de dados sem QoS). */
    uint8_t m[24 + 8 + 20 + 8];
    uint8_t tk[16];
    uint64_t pn = 0;
    unhex(tk, "c97c1f67ce371185514a8a19f2bdd52f", 16);
    unhex(m, "0848c32c0fd2e128a57c5030f1844408abaea5b8fcba8033", 24);
    unhex(m + 32, "f8ba1a55d02f85ae967bb62fb6cda8eb7e78a050", 20);
    wpa_ccmp_encrypt(tk, 0xB5039776E70Cull, 0, m, 24, 20);
    if (!hexeq(m + 24, "0ce70020769703b5f3d0a2fe9a3dbf2342a643e43246e80c3c04d019"
                       "7845ce0b16f97623", 36)) return 7;
    if (wpa_ccmp_decrypt(tk, m, 24, 20, &pn) != 0 || pn != 0xB5039776E70Cull) return 8;
    if (!hexeq(m + 32, "f8ba1a55d02f85ae967bb62fb6cda8eb7e78a050", 20)) return 8;
  }
  wpa_pmk("password", (const uint8_t*)"IEEE", 4, h);
  if (!hexeq(h, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", 32)) return 9;
  return 0;
}
