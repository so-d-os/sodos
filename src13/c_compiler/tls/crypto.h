/* crypto.h - primitivas do TLS 1.3: SHA-256, HMAC, HKDF, AES-128-GCM, X25519 */
#ifndef CRYPTO_H
#define CRYPTO_H

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef struct {
  u32 h[8];
  u64 len;
  u8 buf[64];
  u32 n;
} sha256_ctx;

void sha256_init(sha256_ctx* c);
void sha256_update(sha256_ctx* c, const void* data, int len);
void sha256_final(sha256_ctx* c, u8 out[32]);
void sha256(const void* data, int len, u8 out[32]);
void sha384(const void* data, int len, u8 out[48]);

void hmac_sha256(const u8* key, int klen, const void* msg, int mlen, u8 out[32]);
void hkdf_extract(const u8* salt, int slen, const u8* ikm, int ilen, u8 prk[32]);
void hkdf_expand(const u8 prk[32], const u8* info, int ilen, u8* out, int len);

typedef struct {
  u32 rk[44];
} aes128_ctx;

void aes128_init(aes128_ctx* c, const u8 key[16]);
void aes128_encrypt(const aes128_ctx* c, const u8 in[16], u8 out[16]);

typedef struct {
  aes128_ctx aes;
  u64 hl[16];
  u64 hh[16];
} gcm_ctx;

void gcm_init(gcm_ctx* g, const u8 key[16]);
/* cifra (decrypt=0) ou decifra (decrypt=1) len bytes; in e out podem ser o
 * mesmo buffer. tag recebe a etiqueta calculada (16 bytes). */
void gcm_crypt(gcm_ctx* g, const u8 iv[12], const u8* aad, int alen,
               const u8* in, u8* out, int len, u8 tag[16], int decrypt);

void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]);
void x25519_base(u8 out[32], const u8 scalar[32]);

/* compara sem vazar tempo; 0 = iguais */
int ct_memcmp(const void* a, const void* b, int n);

#endif
