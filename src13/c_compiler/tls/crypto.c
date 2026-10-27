/* crypto.c - SHA-256 (FIPS 180-4), HMAC/HKDF (RFC 2104/5869), AES-128 (FIPS 197),
 * GCM (SP 800-38D, tabela de 4 bits) e X25519 (RFC 7748, no estilo TweetNaCl). */
#include <string.h>
#include "crypto.h"

/* ---------------- SHA-256 ---------------- */
static const u32 sha_k[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static u32 be32(const u8* p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void put_be32(u8* p, u32 v) {
  p[0] = (u8)(v >> 24);
  p[1] = (u8)(v >> 16);
  p[2] = (u8)(v >> 8);
  p[3] = (u8)v;
}

static void sha256_block(sha256_ctx* c, const u8* p) {
  u32 w[64];
  u32 a, b, d, e, f, g, h, cc, t1, t2;
  int i;
  for (i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
  for (i = 16; i < 64; i++) {
    u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
  e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
  for (i = 0; i < 64; i++) {
    t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
    t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(sha256_ctx* c) {
  c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85; c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
  c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c; c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
  c->len = 0;
  c->n = 0;
}

void sha256_update(sha256_ctx* c, const void* data, int len) {
  const u8* p = (const u8*)data;
  c->len += (u64)len;
  while (len > 0) {
    if (c->n == 0 && len >= 64) {
      sha256_block(c, p);
      p += 64;
      len -= 64;
      continue;
    }
    c->buf[c->n++] = *p++;
    len--;
    if (c->n == 64) {
      sha256_block(c, c->buf);
      c->n = 0;
    }
  }
}

void sha256_final(sha256_ctx* c, u8 out[32]) {
  u64 bits = c->len * 8;
  u8 pad = 0x80;
  u8 zero = 0;
  u8 lenb[8];
  int i;
  sha256_update(c, &pad, 1);
  while (c->n != 56) sha256_update(c, &zero, 1);
  for (i = 0; i < 8; i++) lenb[i] = (u8)(bits >> (56 - 8 * i));
  sha256_update(c, lenb, 8);
  for (i = 0; i < 8; i++) put_be32(out + 4 * i, c->h[i]);
}

void sha256(const void* data, int len, u8 out[32]) {
  sha256_ctx c;
  sha256_init(&c);
  sha256_update(&c, data, len);
  sha256_final(&c, out);
}

/* ---------------- SHA-384 (FIPS 180-4, saida de 48 bytes) ---------------- */
typedef struct {
  u64 h[8];
  u64 len;
  u8 buf[128];
  u32 n;
} sha384_ctx;

static const u64 sha384_k[80] = {
  0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
  0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
  0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
  0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
  0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
  0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
  0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
  0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
  0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
  0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
  0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
  0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
  0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
  0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
  0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
  0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
  0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
  0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
  0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
  0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static u64 be64(const u8* p) {
  return ((u64)be32(p) << 32) | be32(p + 4);
}

static void put_be64(u8* p, u64 v) {
  put_be32(p, (u32)(v >> 32));
  put_be32(p + 4, (u32)v);
}

static void sha384_block(sha384_ctx* c, const u8* p) {
  u64 w[80];
  u64 a, b, d, e, f, g, h, cc, t1, t2;
  int i;
  for (i = 0; i < 16; i++) w[i] = be64(p + 8 * i);
  for (i = 16; i < 80; i++) {
    u64 s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
    u64 s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
  e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
  for (i = 0; i < 80; i++) {
    u64 S1 = ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);
    u64 ch = (e & f) ^ ((~e) & g);
    u64 S0 = ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39);
    u64 maj = (a & b) ^ (a & cc) ^ (b & cc);
    t1 = h + S1 + ch + sha384_k[i] + w[i];
    t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha384_init(sha384_ctx* c) {
  c->h[0] = 0xcbbb9d5dc1059ed8ULL;
  c->h[1] = 0x629a292a367cd507ULL;
  c->h[2] = 0x9159015a3070dd17ULL;
  c->h[3] = 0x152fecd8f70e5939ULL;
  c->h[4] = 0x67332667ffc00b31ULL;
  c->h[5] = 0x8eb44a8768581511ULL;
  c->h[6] = 0xdb0c2e0d64f98fa7ULL;
  c->h[7] = 0x47b5481dbefa4fa4ULL;
  c->len = 0;
  c->n = 0;
}

static void sha384_update(sha384_ctx* c, const void* data, int len) {
  const u8* p = (const u8*)data;
  c->len += (u64)len;
  while (len > 0) {
    c->buf[c->n++] = *p++;
    len--;
    if (c->n == 128) {
      sha384_block(c, c->buf);
      c->n = 0;
    }
  }
}

void sha384(const void* data, int len, u8 out[48]) {
  sha384_ctx c;
  u64 bits;
  u8 pad = 0x80, zero = 0, lenb[16];
  int i;
  sha384_init(&c);
  sha384_update(&c, data, len);
  bits = c.len * 8;
  sha384_update(&c, &pad, 1);
  while (c.n != 112) sha384_update(&c, &zero, 1);
  for (i = 0; i < 8; i++) lenb[i] = 0;
  for (i = 0; i < 8; i++) lenb[8 + i] = (u8)(bits >> (56 - 8 * i));
  sha384_update(&c, lenb, 16);
  for (i = 0; i < 6; i++) put_be64(out + 8 * i, c.h[i]);
}

/* ---------------- HMAC / HKDF ---------------- */
void hmac_sha256(const u8* key, int klen, const void* msg, int mlen, u8 out[32]) {
  u8 k[64];
  u8 pad[64];
  u8 inner[32];
  sha256_ctx c;
  int i;
  memset(k, 0, sizeof(k));
  if (klen > 64) sha256(key, klen, k);
  else if (klen > 0) memcpy(k, key, (size_t)klen);
  for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  sha256_init(&c);
  sha256_update(&c, pad, 64);
  sha256_update(&c, msg, mlen);
  sha256_final(&c, inner);
  for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  sha256_init(&c);
  sha256_update(&c, pad, 64);
  sha256_update(&c, inner, 32);
  sha256_final(&c, out);
}

void hkdf_extract(const u8* salt, int slen, const u8* ikm, int ilen, u8 prk[32]) {
  u8 zeros[32];
  if (!salt || slen == 0) {
    memset(zeros, 0, sizeof(zeros));
    salt = zeros;
    slen = 32;
  }
  hmac_sha256(salt, slen, ikm, ilen, prk);
}

void hkdf_expand(const u8 prk[32], const u8* info, int ilen, u8* out, int len) {
  u8 t[32];
  u8 msg[32 + 256 + 1];
  int tlen = 0;
  int done = 0;
  u8 ctr = 1;
  while (done < len) {
    int n = len - done;
    int m = 0;
    if (tlen) memcpy(msg, t, 32);
    m = tlen;
    memcpy(msg + m, info, (size_t)ilen);
    m += ilen;
    msg[m++] = ctr++;
    hmac_sha256(prk, 32, msg, m, t);
    tlen = 32;
    if (n > 32) n = 32;
    memcpy(out + done, t, (size_t)n);
    done += n;
  }
}

/* ---------------- AES-128 ---------------- */
static u8 sbox[256];
static u32 te0[256], te1[256], te2[256], te3[256];
static int aes_ready;

static u8 xtime(u8 x) { return (u8)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static u8 rotl8(u8 x, int n) { return (u8)((x << n) | (x >> (8 - n))); }

static void aes_tables(void) {
  u8 p = 1, q = 1;
  int i;
  /* p percorre o grupo multiplicativo (gerador 3) e q o seu inverso */
  do {
    u8 x;
    p = (u8)(p ^ xtime(p));
    q ^= (u8)(q << 1);
    q ^= (u8)(q << 2);
    q ^= (u8)(q << 4);
    if (q & 0x80) q ^= 0x09;
    x = (u8)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
    sbox[p] = (u8)(x ^ 0x63);
  } while (p != 1);
  sbox[0] = 0x63;
  for (i = 0; i < 256; i++) {
    u32 s = sbox[i];
    u32 s2 = xtime((u8)s);
    u32 s3 = s2 ^ s;
    u32 t = (s2 << 24) | (s << 16) | (s << 8) | s3;
    te0[i] = t;
    te1[i] = ROR(t, 8);
    te2[i] = ROR(t, 16);
    te3[i] = ROR(t, 24);
  }
  aes_ready = 1;
}

static u32 sub_word(u32 w) {
  return ((u32)sbox[w >> 24] << 24) | ((u32)sbox[(w >> 16) & 255] << 16) |
         ((u32)sbox[(w >> 8) & 255] << 8) | sbox[w & 255];
}

void aes128_init(aes128_ctx* c, const u8 key[16]) {
  u32 rcon = 1;
  int i;
  if (!aes_ready) aes_tables();
  for (i = 0; i < 4; i++) c->rk[i] = be32(key + 4 * i);
  for (i = 4; i < 44; i++) {
    u32 t = c->rk[i - 1];
    if ((i & 3) == 0) {
      t = sub_word((t << 8) | (t >> 24)) ^ (rcon << 24);
      rcon = xtime((u8)rcon);
    }
    c->rk[i] = c->rk[i - 4] ^ t;
  }
}

void aes128_encrypt(const aes128_ctx* c, const u8 in[16], u8 out[16]) {
  const u32* rk = c->rk;
  u32 s0 = be32(in) ^ rk[0];
  u32 s1 = be32(in + 4) ^ rk[1];
  u32 s2 = be32(in + 8) ^ rk[2];
  u32 s3 = be32(in + 12) ^ rk[3];
  u32 t0, t1, t2, t3;
  int r;
  for (r = 1; r < 10; r++) {
    rk += 4;
    t0 = te0[s0 >> 24] ^ te1[(s1 >> 16) & 255] ^ te2[(s2 >> 8) & 255] ^ te3[s3 & 255] ^ rk[0];
    t1 = te0[s1 >> 24] ^ te1[(s2 >> 16) & 255] ^ te2[(s3 >> 8) & 255] ^ te3[s0 & 255] ^ rk[1];
    t2 = te0[s2 >> 24] ^ te1[(s3 >> 16) & 255] ^ te2[(s0 >> 8) & 255] ^ te3[s1 & 255] ^ rk[2];
    t3 = te0[s3 >> 24] ^ te1[(s0 >> 16) & 255] ^ te2[(s1 >> 8) & 255] ^ te3[s2 & 255] ^ rk[3];
    s0 = t0; s1 = t1; s2 = t2; s3 = t3;
  }
  rk += 4;
  t0 = ((u32)sbox[s0 >> 24] << 24) | ((u32)sbox[(s1 >> 16) & 255] << 16) |
       ((u32)sbox[(s2 >> 8) & 255] << 8) | sbox[s3 & 255];
  t1 = ((u32)sbox[s1 >> 24] << 24) | ((u32)sbox[(s2 >> 16) & 255] << 16) |
       ((u32)sbox[(s3 >> 8) & 255] << 8) | sbox[s0 & 255];
  t2 = ((u32)sbox[s2 >> 24] << 24) | ((u32)sbox[(s3 >> 16) & 255] << 16) |
       ((u32)sbox[(s0 >> 8) & 255] << 8) | sbox[s1 & 255];
  t3 = ((u32)sbox[s3 >> 24] << 24) | ((u32)sbox[(s0 >> 16) & 255] << 16) |
       ((u32)sbox[(s1 >> 8) & 255] << 8) | sbox[s2 & 255];
  put_be32(out, t0 ^ rk[0]);
  put_be32(out + 4, t1 ^ rk[1]);
  put_be32(out + 8, t2 ^ rk[2]);
  put_be32(out + 12, t3 ^ rk[3]);
}

/* ---------------- GCM ---------------- */
static const u64 gcm_last4[16] = {
  0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
  0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0
};

void gcm_init(gcm_ctx* g, const u8 key[16]) {
  u8 h[16];
  u64 vh, vl;
  int i, j;
  aes128_init(&g->aes, key);
  memset(h, 0, 16);
  aes128_encrypt(&g->aes, h, h);
  vh = be64(h);
  vl = be64(h + 8);
  g->hl[8] = vl;
  g->hh[8] = vh;
  g->hl[0] = 0;
  g->hh[0] = 0;
  for (i = 4; i > 0; i >>= 1) {
    u64 t = (vl & 1) ? 0xe1000000u : 0;
    vl = (vh << 63) | (vl >> 1);
    vh = (vh >> 1) ^ (t << 32);
    g->hl[i] = vl;
    g->hh[i] = vh;
  }
  for (i = 2; i <= 8; i *= 2) {
    vh = g->hh[i];
    vl = g->hl[i];
    for (j = 1; j < i; j++) {
      g->hh[i + j] = vh ^ g->hh[j];
      g->hl[i + j] = vl ^ g->hl[j];
    }
  }
}

/* x = x * H em GF(2^128) */
static void gcm_mult(const gcm_ctx* g, u8 x[16]) {
  int i;
  u32 lo, hi, rem;
  u64 zh, zl;
  lo = x[15] & 15;
  zh = g->hh[lo];
  zl = g->hl[lo];
  for (i = 15; i >= 0; i--) {
    lo = x[i] & 15;
    hi = (x[i] >> 4) & 15;
    if (i != 15) {
      rem = (u32)(zl & 15);
      zl = (zh << 60) | (zl >> 4);
      zh = zh >> 4;
      zh ^= gcm_last4[rem] << 48;
      zh ^= g->hh[lo];
      zl ^= g->hl[lo];
    }
    rem = (u32)(zl & 15);
    zl = (zh << 60) | (zl >> 4);
    zh = zh >> 4;
    zh ^= gcm_last4[rem] << 48;
    zh ^= g->hh[hi];
    zl ^= g->hl[hi];
  }
  put_be32(x, (u32)(zh >> 32));
  put_be32(x + 4, (u32)zh);
  put_be32(x + 8, (u32)(zl >> 32));
  put_be32(x + 12, (u32)zl);
}

static void ghash_data(const gcm_ctx* g, u8 s[16], const u8* p, int len) {
  while (len > 0) {
    int n = len < 16 ? len : 16;
    int i;
    for (i = 0; i < n; i++) s[i] ^= p[i];
    gcm_mult(g, s);
    p += n;
    len -= n;
  }
}

void gcm_crypt(gcm_ctx* g, const u8 iv[12], const u8* aad, int alen,
               const u8* in, u8* out, int len, u8 tag[16], int decrypt) {
  u8 j0[16], ctr[16], ks[16], s[16], lenblk[16];
  u32 c;
  int done = 0;
  int i;
  memcpy(j0, iv, 12);
  j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
  memcpy(ctr, j0, 16);
  memset(s, 0, 16);
  ghash_data(g, s, aad, alen);
  while (done < len) {
    int n = len - done < 16 ? len - done : 16;
    c = be32(ctr + 12) + 1;
    put_be32(ctr + 12, c);
    aes128_encrypt(&g->aes, ctr, ks);
    if (decrypt) {
      for (i = 0; i < n; i++) s[i] ^= in[done + i];
      gcm_mult(g, s);
      for (i = 0; i < n; i++) out[done + i] = in[done + i] ^ ks[i];
    } else {
      for (i = 0; i < n; i++) out[done + i] = in[done + i] ^ ks[i];
      for (i = 0; i < n; i++) s[i] ^= out[done + i];
      gcm_mult(g, s);
    }
    done += n;
  }
  put_be32(lenblk, 0);
  put_be32(lenblk + 4, (u32)alen * 8);
  put_be32(lenblk + 8, 0);
  put_be32(lenblk + 12, (u32)len * 8);
  for (i = 0; i < 16; i++) s[i] ^= lenblk[i];
  gcm_mult(g, s);
  aes128_encrypt(&g->aes, j0, ks);
  for (i = 0; i < 16; i++) tag[i] = s[i] ^ ks[i];
}

int ct_memcmp(const void* a, const void* b, int n) {
  const u8* x = (const u8*)a;
  const u8* y = (const u8*)b;
  u8 d = 0;
  int i;
  for (i = 0; i < n; i++) d |= (u8)(x[i] ^ y[i]);
  return d;
}

/* ---------------- X25519 ---------------- */
typedef long long gf[16];

static void car25519(gf o) {
  int i;
  long long c;
  for (i = 0; i < 16; i++) {
    o[i] += (1LL << 16);
    c = o[i] >> 16;
    if (i < 15) o[i + 1] += c - 1;
    else o[0] += 38 * (c - 1);
    o[i] -= c << 16;
  }
}

static void sel25519(gf p, gf q, int b) {
  long long t;
  long long c = ~((long long)b - 1);
  int i;
  for (i = 0; i < 16; i++) {
    t = c & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}

static void pack25519(u8* o, const gf n) {
  int i, j, b;
  gf m, t;
  for (i = 0; i < 16; i++) t[i] = n[i];
  car25519(t);
  car25519(t);
  car25519(t);
  for (j = 0; j < 2; j++) {
    m[0] = t[0] - 0xffed;
    for (i = 1; i < 15; i++) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    b = (int)((m[15] >> 16) & 1);
    m[14] &= 0xffff;
    sel25519(t, m, 1 - b);
  }
  for (i = 0; i < 16; i++) {
    o[2 * i] = (u8)(t[i] & 0xff);
    o[2 * i + 1] = (u8)(t[i] >> 8);
  }
}

static void unpack25519(gf o, const u8* n) {
  int i;
  for (i = 0; i < 16; i++) o[i] = n[2 * i] + ((long long)n[2 * i + 1] << 8);
  o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b) {
  int i;
  for (i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void fsub(gf o, const gf a, const gf b) {
  int i;
  for (i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void fmul(gf o, const gf a, const gf b) {
  long long t[31];
  int i, j;
  for (i = 0; i < 31; i++) t[i] = 0;
  for (i = 0; i < 16; i++) {
    long long ai = a[i];
    for (j = 0; j < 16; j++) t[i + j] += ai * b[j];
  }
  for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
  for (i = 0; i < 16; i++) o[i] = t[i];
  car25519(o);
  car25519(o);
}

static void finv(gf o, const gf in) {
  gf c;
  int a;
  for (a = 0; a < 16; a++) c[a] = in[a];
  for (a = 253; a >= 0; a--) {
    fmul(c, c, c);
    if (a != 2 && a != 4) fmul(c, c, in);
  }
  for (a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]) {
  static const gf k121665 = { 0xDB41, 1 };
  u8 z[32];
  gf x, a, b, c, d, e, f;
  int i, r;
  for (i = 0; i < 32; i++) z[i] = scalar[i];
  z[31] = (u8)((z[31] & 127) | 64);
  z[0] &= 248;
  unpack25519(x, point);
  for (i = 0; i < 16; i++) {
    b[i] = x[i];
    a[i] = c[i] = d[i] = 0;
  }
  a[0] = d[0] = 1;
  for (i = 254; i >= 0; i--) {
    r = (z[i >> 3] >> (i & 7)) & 1;
    sel25519(a, b, r);
    sel25519(c, d, r);
    fadd(e, a, c);
    fsub(a, a, c);
    fadd(c, b, d);
    fsub(b, b, d);
    fmul(d, e, e);
    fmul(f, a, a);
    fmul(a, c, a);
    fmul(c, b, e);
    fadd(e, a, c);
    fsub(a, a, c);
    fmul(b, a, a);
    fsub(c, d, f);
    fmul(a, c, k121665);
    fadd(a, a, d);
    fmul(c, c, a);
    fmul(a, d, f);
    fmul(d, b, x);
    fmul(b, e, e);
    sel25519(a, b, r);
    sel25519(c, d, r);
  }
  finv(c, c);
  fmul(a, a, c);
  pack25519(out, a);
}

void x25519_base(u8 out[32], const u8 scalar[32]) {
  u8 nine[32];
  memset(nine, 0, 32);
  nine[0] = 9;
  x25519(out, scalar, nine);
}
