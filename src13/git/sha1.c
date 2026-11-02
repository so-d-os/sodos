/* sha1.c - SHA-1 (FIPS 180-1), o nome dos objetos do git. */
#include "git.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(sha1_ctx* c, const unsigned char* p) {
  unsigned w[80], a, b, d, e, f, k, t, cc;
  int i;
  for (i = 0; i < 16; i++)
    w[i] = ((unsigned)p[4 * i] << 24) | ((unsigned)p[4 * i + 1] << 16) | ((unsigned)p[4 * i + 2] << 8) |
           (unsigned)p[4 * i + 3];
  for (i = 16; i < 80; i++) {
    t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
    w[i] = ROL(t, 1);
  }
  a = c->h[0];
  b = c->h[1];
  cc = c->h[2];
  d = c->h[3];
  e = c->h[4];
  for (i = 0; i < 80; i++) {
    if (i < 20) {
      f = (b & cc) | (~b & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ cc ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & cc) | (b & d) | (cc & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ cc ^ d;
      k = 0xCA62C1D6u;
    }
    t = ROL(a, 5) + f + e + k + w[i];
    e = d;
    d = cc;
    cc = ROL(b, 30);
    b = a;
    a = t;
  }
  c->h[0] += a;
  c->h[1] += b;
  c->h[2] += cc;
  c->h[3] += d;
  c->h[4] += e;
}

void sha1_init(sha1_ctx* c) {
  c->h[0] = 0x67452301u;
  c->h[1] = 0xEFCDAB89u;
  c->h[2] = 0x98BADCFEu;
  c->h[3] = 0x10325476u;
  c->h[4] = 0xC3D2E1F0u;
  c->len_lo = 0;
  c->len_hi = 0;
  c->n = 0;
}

void sha1_update(sha1_ctx* c, const void* data, int len) {
  const unsigned char* p = (const unsigned char*)data;
  unsigned bits = (unsigned)len << 3;
  c->len_hi += (unsigned)len >> 29;
  if (c->len_lo + bits < c->len_lo) c->len_hi++;
  c->len_lo += bits;
  if (c->n) {
    while (len > 0 && c->n < 64) {
      c->buf[c->n++] = *p++;
      len--;
    }
    if (c->n < 64) return;
    block(c, c->buf);
    c->n = 0;
  }
  while (len >= 64) {
    block(c, p);
    p += 64;
    len -= 64;
  }
  while (len > 0) {
    c->buf[c->n++] = *p++;
    len--;
  }
}

void sha1_final(sha1_ctx* c, unsigned char out[20]) {
  unsigned hi = c->len_hi, lo = c->len_lo;
  unsigned char pad = 0x80, zero = 0, len8[8];
  int i;
  /* sha1_update mexe no tamanho: guarda antes de completar */
  sha1_update(c, &pad, 1);
  while (c->n != 56) sha1_update(c, &zero, 1);
  for (i = 0; i < 4; i++) {
    len8[i] = (unsigned char)(hi >> (24 - 8 * i));
    len8[4 + i] = (unsigned char)(lo >> (24 - 8 * i));
  }
  sha1_update(c, len8, 8);
  for (i = 0; i < 20; i++) out[i] = (unsigned char)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

void git_hash(const char* type, const unsigned char* data, int len, unsigned char out[20]) {
  char hdr[32];
  sha1_ctx c;
  int n = sprintf(hdr, "%s %d", type, len);
  sha1_init(&c);
  sha1_update(&c, hdr, n + 1);
  sha1_update(&c, data, len);
  sha1_final(&c, out);
}

void sha_hex(const unsigned char* sha, char* out) {
  static const char* h = "0123456789abcdef";
  int i;
  for (i = 0; i < 20; i++) {
    out[2 * i] = h[sha[i] >> 4];
    out[2 * i + 1] = h[sha[i] & 15];
  }
  out[40] = 0;
}

static int hexv(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int hex_sha(const char* hex, unsigned char* sha) {
  int i;
  for (i = 0; i < 20; i++) {
    int a = hexv((unsigned char)hex[2 * i]), b;
    if (a < 0) return -1;
    b = hexv((unsigned char)hex[2 * i + 1]);
    if (b < 0) return -1;
    sha[i] = (unsigned char)(a * 16 + b);
  }
  return 0;
}
