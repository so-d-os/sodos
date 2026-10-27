/* t_crypto.c - vetores oficiais: FIPS 180-4, RFC 4231, RFC 5869, FIPS 197,
 * NIST GCM (casos 2-4) e RFC 7748. */
#include <stdio.h>
#include <string.h>
#include <so.h>
#include "../tls/crypto.c"

static int fails, total;

static int unhex(const char* s, u8* out) {
  int n = 0;
  while (s[0] && s[1]) {
    int hi = s[0] <= '9' ? s[0] - '0' : (s[0] | 32) - 'a' + 10;
    int lo = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
    out[n++] = (u8)(hi * 16 + lo);
    s += 2;
  }
  return n;
}

static void check(const char* name, const u8* got, const char* want_hex) {
  u8 want[256];
  int n = unhex(want_hex, want);
  total++;
  if (memcmp(got, want, (size_t)n) != 0) {
    int i;
    fails++;
    printf("FALHOU %s\n  obtido: ", name);
    for (i = 0; i < n; i++) printf("%02x", got[i]);
    printf("\n");
  }
}

static void t_sha(void) {
  u8 h[32];
  sha256("abc", 3, h);
  check("sha256 abc", h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  sha256("", 0, h);
  check("sha256 vazio", h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
  check("sha256 448", h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  {
    sha256_ctx c;
    int i;
    sha256_init(&c);
    for (i = 0; i < 1000; i++) sha256_update(&c, "a", 1);
    for (i = 0; i < 9989; i++) sha256_update(&c, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 100);
    for (i = 0; i < 10; i++) sha256_update(&c, "aaaaaaaaaa", 10);
    sha256_final(&c, h);
    check("sha256 milhao de a", h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }
}

static void t_hmac(void) {
  u8 key[32], out[64], salt[16], info[16];
  memset(key, 0x0b, 20);
  hmac_sha256(key, 20, "Hi There", 8, out);
  check("hmac rfc4231 1", out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  hmac_sha256((const u8*)"Jefe", 4, "what do ya want for nothing?", 28, out);
  check("hmac rfc4231 2", out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  memset(key, 0x0b, 22);
  unhex("000102030405060708090a0b0c", salt);
  unhex("f0f1f2f3f4f5f6f7f8f9", info);
  hkdf_extract(salt, 13, key, 22, out);
  check("hkdf prk", out, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
  {
    u8 prk[32];
    memcpy(prk, out, 32);
    hkdf_expand(prk, info, 10, out, 42);
    check("hkdf okm", out, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
  }
}

static void t_aes(void) {
  u8 key[16], pt[16], ct[16];
  aes128_ctx c;
  unhex("000102030405060708090a0b0c0d0e0f", key);
  unhex("00112233445566778899aabbccddeeff", pt);
  aes128_init(&c, key);
  aes128_encrypt(&c, pt, ct);
  check("aes fips197", ct, "69c4e0d86a7b0430d8cdb78070b4c55a");
  unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
  unhex("3243f6a8885a308d313198a2e0370734", pt);
  aes128_init(&c, key);
  aes128_encrypt(&c, pt, ct);
  check("aes fips197 b", ct, "3925841d02dc09fbdc118597196a0b32");
}

static void t_gcm(void) {
  gcm_ctx g;
  u8 key[16], iv[12], pt[64], ct[64], tag[16], aad[20], back[64];
  memset(key, 0, 16);
  memset(iv, 0, 12);
  memset(pt, 0, 16);
  gcm_init(&g, key);
  gcm_crypt(&g, iv, 0, 0, pt, ct, 0, tag, 0);
  check("gcm caso 1 tag", tag, "58e2fccefa7e3061367f1d57a4e7455a");
  gcm_crypt(&g, iv, 0, 0, pt, ct, 16, tag, 0);
  check("gcm caso 2 ct", ct, "0388dace60b6a392f328c2b971b2fe78");
  check("gcm caso 2 tag", tag, "ab6e47d42cec13bdf53a67b21257bddf");

  unhex("feffe9928665731c6d6a8f9467308308", key);
  unhex("cafebabefacedbaddecaf888", iv);
  unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
        "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255", pt);
  gcm_init(&g, key);
  gcm_crypt(&g, iv, 0, 0, pt, ct, 64, tag, 0);
  check("gcm caso 3 ct", ct, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
                             "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985");
  check("gcm caso 3 tag", tag, "4d5c2af327cd64a62cf35abd2ba6fab4");

  unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad);
  gcm_crypt(&g, iv, aad, 20, pt, ct, 60, tag, 0);
  check("gcm caso 4 ct", ct, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
                             "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091");
  check("gcm caso 4 tag", tag, "5bc94fbc3221a5db94fae95ae7121a47");
  gcm_crypt(&g, iv, aad, 20, ct, back, 60, tag, 1);
  check("gcm caso 4 volta", back, "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                                  "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
  check("gcm caso 4 tag volta", tag, "5bc94fbc3221a5db94fae95ae7121a47");
}

static void t_x25519(void) {
  u8 k[32], u[32], out[32], a[32], b[32], pa[32], pb[32], s1[32], s2[32];
  unsigned t0;
  unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k);
  unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u);
  t0 = k_ms();
  x25519(out, k, u);
  printf("x25519: %u ms por operacao\n", k_ms() - t0);
  check("x25519 rfc7748 1", out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
  unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
  unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b);
  x25519_base(pa, a);
  check("x25519 pub alice", pa, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
  x25519_base(pb, b);
  check("x25519 pub bob", pb, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
  x25519(s1, a, pb);
  x25519(s2, b, pa);
  check("x25519 segredo a", s1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  check("x25519 segredo b", s2, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  {
    /* RFC 7748 5.2: 1000 iteracoes de k = X25519(k, u), u = k anterior */
    int i;
    memset(k, 0, 32);
    k[0] = 9;
    memcpy(u, k, 32);
    for (i = 0; i < 1000; i++) {
      x25519(out, k, u);
      memcpy(u, k, 32);
      memcpy(k, out, 32);
    }
    check("x25519 1000 iteracoes", k, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51");
  }
}

int main(void) {
  t_sha();
  t_hmac();
  t_aes();
  t_gcm();
  t_x25519();
  if (fails) {
    printf("%d de %d testes de cripto falharam\n", fails, total);
    return 1;
  }
  printf("ok: %d testes de cripto\n", total);
  return 0;
}
