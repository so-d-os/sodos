/* tls.c - cliente TLS 1.3 minimo (RFC 8446) para a TLS.DLL.
 * TLS_AES_128_GCM_SHA256, grupos x25519 e secp256r1 (P-256), SNI.
 * Le e descarta o certificado: a conexao fica cifrada, mas o servidor
 * nao e autenticado. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <so.h>
#include <tls.h>
#include "crypto.h"
#include "p256-m.h"
#include "x509.h"

/* o p256-m pede um gerador; so e usado se o servidor exigir P-256 */
int p256_generate_random(uint8_t* output, unsigned output_size) {
  k_random(output, (int)output_size);
  return 0;
}

#define REC_MAX (16384 + 256 + 5)

typedef struct {
  gcm_ctx gcm;
  u8 iv[12];
  u64 seq;
  u8 secret[32];
} dir_keys;

static struct {
  int open;
  u8 in[REC_MAX + 64];
  int in_len;
  u8* hs;             /* mensagens de handshake ainda nao processadas */
  int hs_len, hs_cap;
  u8* app;            /* texto claro do ultimo registro de dados */
  int app_pos, app_len;
  dir_keys rd, wr;
  sha256_ctx transcript;
  char err[96];
} T;

static void transcript_hash(u8 out[32]);

static u8* cert_msg;
static int cert_msg_len, cv_scheme, cv_sig_len, have_cv, cert_ok;
static u8 cv_sig[512], cv_th[32];
static char sec_label[96];

static void cert_reset(void) {
  cert_msg_len = 0;
  cv_scheme = 0;
  cv_sig_len = 0;
  have_cv = 0;
  cert_ok = 0;
  sec_label[0] = 0;
}

static void save_cert_msg(const u8* p, int n) {
  u8* b;
  if (n < 1 || n > 65536) return;
  b = (u8*)realloc(cert_msg, (size_t)n);
  if (!b) return;
  memcpy(b, p, (size_t)n);
  cert_msg = b;
  cert_msg_len = n;
}

static void save_cert_verify(const u8* p, int n) {
  int sl;
  if (n < 4) return;
  transcript_hash(cv_th);
  cv_scheme = (p[0] << 8) | p[1];
  sl = (p[2] << 8) | p[3];
  if (sl < 1 || sl > (int)sizeof(cv_sig) || 4 + sl > n) return;
  memcpy(cv_sig, p + 4, (size_t)sl);
  cv_sig_len = sl;
  have_cv = 1;
}

static void note_certificate(const char* host) {
  char why[64];
  why[0] = 0;
  cert_ok = 0;
  if (have_cv && x509_tls_check(host, cert_msg, cert_msg_len, cv_scheme, cv_sig, cv_sig_len,
                                cv_th, why, (int)sizeof(why)) == 0) {
    cert_ok = 1;
    strcpy(sec_label, "HTTPS");
  } else {
    snprintf(sec_label, sizeof(sec_label), "HTTPS, certificado NAO verificado%s%s",
             why[0] ? ": " : "", why);
  }
}

static int fail(const char* msg) {
  strncpy(T.err, msg, sizeof(T.err) - 1);
  T.err[sizeof(T.err) - 1] = 0;
  return -1;
}

__export const char* tls_error(void) { return T.err; }

static int fail_alert(int desc) {
  const char* name = "desconhecido";
  if (desc == 0) name = "close_notify";
  else if (desc == 10) name = "unexpected_message";
  else if (desc == 20) name = "bad_record_mac";
  else if (desc == 22) name = "record_overflow";
  else if (desc == 40) name = "handshake_failure: servidor sem TLS 1.3 compativel";
  else if (desc == 42) name = "bad_certificate";
  else if (desc == 47) name = "illegal_parameter";
  else if (desc == 50) name = "decode_error";
  else if (desc == 51) name = "decrypt_error";
  else if (desc == 70) name = "protocol_version";
  else if (desc == 80) name = "internal_error";
  else if (desc == 109) name = "missing_extension";
  else if (desc == 112) name = "unrecognized_name";
  else if (desc == 120) name = "no_application_protocol";
  snprintf(T.err, sizeof(T.err), "alerta TLS %d (%s)", desc, name);
  return -1;
}

/* ---------- key schedule ---------- */
static void expand_label(const u8 secret[32], const char* label, const u8* ctx, int clen,
                         u8* out, int len) {
  u8 info[2 + 1 + 6 + 32 + 1 + 64];
  int ll = (int)strlen(label);
  int n = 0;
  info[n++] = (u8)(len >> 8);
  info[n++] = (u8)len;
  info[n++] = (u8)(6 + ll);
  memcpy(info + n, "tls13 ", 6);
  n += 6;
  memcpy(info + n, label, (size_t)ll);
  n += ll;
  info[n++] = (u8)clen;
  if (clen) memcpy(info + n, ctx, (size_t)clen);
  n += clen;
  hkdf_expand(secret, info, n, out, len);
}

static void transcript_hash(u8 out[32]) {
  sha256_ctx c = T.transcript;
  sha256_final(&c, out);
}

static void set_keys(dir_keys* k, const u8 secret[32]) {
  u8 key[16];
  memcpy(k->secret, secret, 32);
  expand_label(secret, "key", 0, 0, key, 16);
  expand_label(secret, "iv", 0, 0, k->iv, 12);
  gcm_init(&k->gcm, key);
  k->seq = 0;
}

static void nonce_for(const dir_keys* k, u8 nonce[12]) {
  int i;
  memcpy(nonce, k->iv, 12);
  for (i = 0; i < 8; i++) nonce[11 - i] ^= (u8)(k->seq >> (8 * i));
}

/* ---------- rede ---------- */
static int net_send(const u8* p, int n) {
  int r = k_tcp_send(p, n);
  if (r == -3) return fail("interrompido");
  return r == n ? 0 : fail("falha ao enviar");
}

/* garante pelo menos n bytes em T.in */
static int fill(int n) {
  while (T.in_len < n) {
    int r = k_tcp_recv(T.in + T.in_len, (int)sizeof(T.in) - T.in_len);
    if (r == -3) {
      fail("interrompido");
      return -3;
    }
    if (r == 0) return fail("servidor fechou a conexao");
    if (r < 0) return fail("tempo esgotado ou conexao perdida");
    T.in_len += r;
  }
  return 0;
}

/* le um registro: *type, payload copiado em out (ate REC_MAX) */
static int read_record(int* type, u8* out, int* len) {
  int n, f;
  f = fill(5);
  if (f < 0) return f;
  n = (T.in[3] << 8) | T.in[4];
  if (n > REC_MAX - 5) return fail("registro grande demais");
  f = fill(5 + n);
  if (f < 0) return f;
  *type = T.in[0];
  memcpy(out, T.in, (size_t)(5 + n));
  *len = n;
  memmove(T.in, T.in + 5 + n, (size_t)(T.in_len - 5 - n));
  T.in_len -= 5 + n;
  return 0;
}

/* decifra rec (cabecalho + payload) no lugar; devolve tipo interno e tamanho */
static int open_record(u8* rec, int n, int* inner, u8** plain, int* plen) {
  u8 nonce[12], tag[16];
  int m;
  if (n < 17) return fail("registro cifrado curto");
  nonce_for(&T.rd, nonce);
  gcm_crypt(&T.rd.gcm, nonce, rec, 5, rec + 5, rec + 5, n - 16, tag, 1);
  if (ct_memcmp(tag, rec + 5 + n - 16, 16) != 0) return fail("etiqueta GCM invalida");
  T.rd.seq++;
  m = n - 16;
  while (m > 0 && rec[5 + m - 1] == 0) m--;
  if (m == 0) return fail("registro sem tipo");
  *inner = rec[5 + m - 1];
  *plain = rec + 5;
  *plen = m - 1;
  return 0;
}

static int send_encrypted(int inner, const u8* data, int n) {
  static u8 rec[16384 + 5 + 1 + 16];
  u8 nonce[12];
  int len = n + 1 + 16;
  rec[0] = 23;
  rec[1] = 3;
  rec[2] = 3;
  rec[3] = (u8)(len >> 8);
  rec[4] = (u8)len;
  memcpy(rec + 5, data, (size_t)n);
  rec[5 + n] = (u8)inner;
  nonce_for(&T.wr, nonce);
  gcm_crypt(&T.wr.gcm, nonce, rec, 5, rec + 5, rec + 5, n + 1, rec + 5 + n + 1, 0);
  T.wr.seq++;
  return net_send(rec, 5 + len);
}

/* ---------- handshake ---------- */
static int put16(u8* p, int v) {
  p[0] = (u8)(v >> 8);
  p[1] = (u8)v;
  return 2;
}

/* xpub/ppub NULL omite aquela chave. cookie_len 0 omite o cookie.
 * rnd e sid tem de ser os mesmos no ClientHello repetido apos HRR. */
static int build_client_hello(u8* out, int max, const char* host, const u8 rnd[32], const u8 sid[32],
                              const u8* xpub, const u8* ppub, const u8* cookie, int cookie_len) {
  /* so SHA-256: e o que a conferencia do certificado sabe verificar */
  static const u8 sigalgs[] = { 4, 3, 8, 4, 4, 1 };
  int hl = (int)strlen(host);
  int n = 0, ext_at, i, share_len;
  if (hl > 200 || cookie_len < 0 || cookie_len > 400) return -1;
  out[n++] = 1;           /* ClientHello */
  n += 3;                 /* tamanho depois */
  out[n++] = 3;
  out[n++] = 3;
  memcpy(out + n, rnd, 32);
  n += 32;
  out[n++] = 32;          /* session id: modo de compatibilidade */
  memcpy(out + n, sid, 32);
  n += 32;
  n += put16(out + n, 2);
  n += put16(out + n, 0x1301);
  out[n++] = 1;
  out[n++] = 0;
  ext_at = n;
  n += 2;
  /* server_name */
  n += put16(out + n, 0);
  n += put16(out + n, hl + 5);
  n += put16(out + n, hl + 3);
  out[n++] = 0;
  n += put16(out + n, hl);
  memcpy(out + n, host, (size_t)hl);
  n += hl;
  /* supported_groups: x25519 e secp256r1. Sem o NIST, alguns servidores
   * (Microsoft) fecham a conexao em vez de negociar. */
  n += put16(out + n, 10);
  n += put16(out + n, 6);
  n += put16(out + n, 4);
  n += put16(out + n, 0x001d);
  n += put16(out + n, 0x0017);
  /* signature_algorithms */
  n += put16(out + n, 13);
  n += put16(out + n, (int)sizeof(sigalgs) + 2);
  n += put16(out + n, (int)sizeof(sigalgs));
  for (i = 0; i < (int)sizeof(sigalgs); i++) out[n++] = sigalgs[i];
  /* supported_versions: TLS 1.3 */
  n += put16(out + n, 43);
  n += put16(out + n, 3);
  out[n++] = 2;
  n += put16(out + n, 0x0304);
  /* key_share: x25519 = 36 bytes, P-256 descompactado = 69 */
  share_len = (xpub ? 36 : 0) + (ppub ? 69 : 0);
  n += put16(out + n, 51);
  n += put16(out + n, 2 + share_len);
  n += put16(out + n, share_len);
  if (xpub) {
    n += put16(out + n, 0x001d);
    n += put16(out + n, 32);
    memcpy(out + n, xpub, 32);
    n += 32;
  }
  if (ppub) {
    n += put16(out + n, 0x0017);
    n += put16(out + n, 65);
    out[n++] = 4;
    memcpy(out + n, ppub, 64);
    n += 64;
  }
  if (cookie_len > 0) {
    n += put16(out + n, 44);
    n += put16(out + n, cookie_len);
    memcpy(out + n, cookie, (size_t)cookie_len);
    n += cookie_len;
  }
  /* ALPN http/1.1 */
  n += put16(out + n, 16);
  n += put16(out + n, 11);
  n += put16(out + n, 9);
  out[n++] = 8;
  memcpy(out + n, "http/1.1", 8);
  n += 8;
  if (n > max || n < 4) return -1;
  put16(out + ext_at, n - ext_at - 2);
  out[1] = 0;
  put16(out + 2, n - 4);
  return n;
}

/* 0 = ServerHello, 1 = HelloRetryRequest, -1 = erro.
 * key recebe 32 bytes (x25519) ou 64 (X||Y de P-256, sem o prefixo 04). */
static int parse_server_hello(const u8* m, int n, int* group, u8* key, int* keylen,
                              u8* cookie, int* cookie_len, int cookie_max) {
  static const u8 hrr[32] = {
    0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c
  };
  int p = 4, sid, end, got_ver = 0, got_key = 0, is_hrr;
  if (n < 4 + 2 + 32 + 1) return fail("ServerHello curto");
  p += 2;
  is_hrr = memcmp(m + p, hrr, 32) == 0;
  p += 32;
  sid = m[p++];
  if (p + sid > n) return fail("ServerHello truncado");
  p += sid;
  if (p + 5 > n) return fail("ServerHello truncado");
  if (((m[p] << 8) | m[p + 1]) != 0x1301) return fail("servidor escolheu outra cifra");
  p += 3;
  end = p + 2 + ((m[p] << 8) | m[p + 1]);
  p += 2;
  if (end > n) return fail("extensoes truncadas");
  *cookie_len = 0;
  while (p + 4 <= end) {
    int type = (m[p] << 8) | m[p + 1];
    int len = (m[p + 2] << 8) | m[p + 3];
    const u8* d = m + p + 4;
    if (p + 4 + len > end) return fail("extensao truncada");
    if (type == 43 && len == 2 && d[0] == 3 && d[1] == 4) got_ver = 1;
    if (type == 44 && is_hrr) {
      if (len > cookie_max) return fail("cookie do servidor grande demais");
      memcpy(cookie, d, (size_t)len);
      *cookie_len = len;
    }
    if (type == 51 && is_hrr && len == 2) {
      *group = (d[0] << 8) | d[1];
      got_key = 1;
    }
    if (type == 51 && !is_hrr && len >= 4) {
      int gid = (d[0] << 8) | d[1];
      int glen = (d[2] << 8) | d[3];
      if (gid == 0x001d && glen == 32 && len >= 36) {
        memcpy(key, d + 4, 32);
        *keylen = 32;
        *group = gid;
        got_key = 1;
      } else if (gid == 0x0017 && glen == 65 && len >= 69 && d[4] == 4) {
        memcpy(key, d + 5, 64);
        *keylen = 64;
        *group = gid;
        got_key = 1;
      }
    }
    p += 4 + len;
  }
  if (!got_ver) return fail("servidor sem TLS 1.3");
  if (!got_key) return fail(is_hrr ? "HelloRetryRequest sem grupo" : "servidor sem chave de troca");
  return is_hrr ? 1 : 0;
}

static int hs_append(const u8* p, int n) {
  if (T.hs_len + n > T.hs_cap) {
    int cap = T.hs_cap ? T.hs_cap : 8192;
    u8* nb;
    while (cap < T.hs_len + n) cap *= 2;
    nb = (u8*)realloc(T.hs, (size_t)cap);
    if (!nb) return fail("sem memoria");
    T.hs = nb;
    T.hs_cap = cap;
  }
  memcpy(T.hs + T.hs_len, p, (size_t)n);
  T.hs_len += n;
  return 0;
}

static void hs_consume(int n) {
  memmove(T.hs, T.hs + n, (size_t)(T.hs_len - n));
  T.hs_len -= n;
}

static int send_handshake(u8* rec, const u8* msg, int n) {
  rec[0] = 22;
  rec[1] = 3;
  rec[2] = 1;
  put16(rec + 3, n);
  memcpy(rec + 5, msg, (size_t)n);
  return net_send(rec, 5 + n);
}

static int handshake(const char* host) {
  static u8 rec[REC_MAX + 64];
  u8 xpriv[32], xpub[32], ppriv[32], ppub[64], spub[65], shared[32];
  u8 rnd[32], sid[32], cookie[256];
  u8 zeros[32], early[32], derived[32], hs_secret[32], master[32];
  u8 th[32], c_hs[32], s_hs[32], c_ap[32], s_ap[32];
  u8 ch[1400];
  int chn, type, len, i, group = 0, klen = 0, cookie_len = 0, retried = 0;
  int got_finished = 0;
  int have_p256 = 0;

  k_random(xpriv, 32);
  x25519_base(xpub, xpriv);
  k_random(rnd, 32);
  k_random(sid, 32);
  chn = build_client_hello(ch, (int)sizeof(ch), host, rnd, sid, xpub, 0, 0, 0);
  if (chn < 0 || send_handshake(rec, ch, chn) < 0) return chn < 0 ? fail("ClientHello grande demais") : -1;
  sha256_init(&T.transcript);
  sha256_update(&T.transcript, ch, chn);

  /* ServerHello em claro. Pode ser um HelloRetryRequest pedindo P-256. */
  for (;;) {
    int kind, mlen;
    if (read_record(&type, rec, &len) < 0) return -1;
    if (type == 20) continue;
    if (type == 21) return fail_alert(len >= 2 ? rec[6] : 0);
    if (type != 22) return fail("esperava ServerHello");
    if (hs_append(rec + 5, len) < 0) return -1;
    if (T.hs_len < 4) continue;
    mlen = (T.hs[1] << 16) | (T.hs[2] << 8) | T.hs[3];
    if (T.hs[0] != 2) return fail("esperava ServerHello");
    if (T.hs_len < 4 + mlen) continue;
    kind = parse_server_hello(T.hs, 4 + mlen, &group, spub, &klen, cookie, &cookie_len, (int)sizeof(cookie));
    if (kind < 0) return -1;
    if (kind == 1) {
      u8 mh[36];
      const u8* use_x = 0;
      const u8* use_p = 0;
      static const u8 ccs[6] = { 20, 3, 3, 0, 1, 1 };
      if (retried) return fail("HelloRetryRequest repetido");
      retried = 1;
      if (group == 0x001d) use_x = xpub;
      else if (group == 0x0017) {
        if (!have_p256) {
          if (p256_gen_keypair(ppriv, ppub) != 0) return fail("falha ao gerar chave P-256");
          have_p256 = 1;
        }
        use_p = ppub;
      } else {
        snprintf(T.err, sizeof(T.err), "servidor pediu o grupo %d, nao suportado", group);
        return -1;
      }
      /* transcript: message_hash(ClientHello1) || HRR || ClientHello2 */
      transcript_hash(th);
      sha256_init(&T.transcript);
      mh[0] = 254;
      mh[1] = 0;
      mh[2] = 0;
      mh[3] = 32;
      memcpy(mh + 4, th, 32);
      sha256_update(&T.transcript, mh, 36);
      sha256_update(&T.transcript, T.hs, 4 + mlen);
      hs_consume(4 + mlen);
      chn = build_client_hello(ch, (int)sizeof(ch), host, rnd, sid, use_x, use_p,
                              cookie_len ? cookie : 0, cookie_len);
      if (chn < 0) return fail("ClientHello grande demais");
      if (net_send(ccs, 6) < 0) return -1;
      if (send_handshake(rec, ch, chn) < 0) return -1;
      sha256_update(&T.transcript, ch, chn);
      continue;
    }
    sha256_update(&T.transcript, T.hs, 4 + mlen);
    hs_consume(4 + mlen);
    break;
  }

  if (group == 0x001d) x25519(shared, xpriv, spub);
  else if (group == 0x0017) {
    if (!have_p256 || p256_ecdh_shared_secret(shared, ppriv, spub) != 0)
      return fail("chave P-256 do servidor invalida");
  } else return fail("servidor sem chave de troca");
  memset(zeros, 0, 32);
  hkdf_extract(0, 0, zeros, 32, early);
  sha256("", 0, th);
  expand_label(early, "derived", th, 32, derived, 32);
  hkdf_extract(derived, 32, shared, 32, hs_secret);
  transcript_hash(th);
  expand_label(hs_secret, "c hs traffic", th, 32, c_hs, 32);
  expand_label(hs_secret, "s hs traffic", th, 32, s_hs, 32);
  sha256("", 0, th);
  expand_label(hs_secret, "derived", th, 32, derived, 32);
  hkdf_extract(derived, 32, zeros, 32, master);
  set_keys(&T.rd, s_hs);
  set_keys(&T.wr, c_hs);

  /* EncryptedExtensions, Certificate, CertificateVerify, Finished (cifrados) */
  while (!got_finished) {
    while (T.hs_len >= 4) {
      int mt = T.hs[0];
      int mlen = (T.hs[1] << 16) | (T.hs[2] << 8) | T.hs[3];
      if (T.hs_len < 4 + mlen) break;
      if (mt == 13) return fail("servidor pede certificado do cliente");
      if (mt == 11) save_cert_msg(T.hs + 4, mlen);
      if (mt == 15) save_cert_verify(T.hs + 4, mlen);
      if (mt == 20) {
        u8 fkey[32], want[32];
        transcript_hash(th);
        expand_label(s_hs, "finished", 0, 0, fkey, 32);
        hmac_sha256(fkey, 32, th, 32, want);
        if (mlen != 32 || ct_memcmp(want, T.hs + 4, 32) != 0) return fail("Finished do servidor invalido");
        got_finished = 1;
      }
      sha256_update(&T.transcript, T.hs, 4 + mlen);
      hs_consume(4 + mlen);
      if (got_finished) break;
    }
    if (got_finished) break;
    if (read_record(&type, rec, &len) < 0) return -1;
    if (type == 20) continue;  /* ChangeCipherSpec de compatibilidade */
    if (type == 21) return fail_alert(len >= 2 ? rec[6] : 0);
    if (type != 23) return fail("registro inesperado no handshake");
    {
      int inner, plen;
      u8* plain;
      if (open_record(rec, len, &inner, &plain, &plen) < 0) return -1;
      if (inner == 21) return fail_alert(plen >= 2 ? plain[1] : 0);
      if (inner != 22) return fail("dados antes do fim do handshake");
      if (hs_append(plain, plen) < 0) return -1;
    }
  }

  /* segredos de aplicacao: transcript ate o Finished do servidor */
  transcript_hash(th);
  expand_label(master, "c ap traffic", th, 32, c_ap, 32);
  expand_label(master, "s ap traffic", th, 32, s_ap, 32);

  {
    static const u8 ccs[6] = { 20, 3, 3, 0, 1, 1 };
    u8 fin[4 + 32], fkey[32];
    if (net_send(ccs, 6) < 0) return -1;
    expand_label(c_hs, "finished", 0, 0, fkey, 32);
    fin[0] = 20;
    fin[1] = 0;
    fin[2] = 0;
    fin[3] = 32;
    hmac_sha256(fkey, 32, th, 32, fin + 4);
    if (send_encrypted(22, fin, 36) < 0) return -1;
  }
  set_keys(&T.rd, s_ap);
  set_keys(&T.wr, c_ap);
  T.hs_len = 0;
  for (i = 0; i < 32; i++) {
    xpriv[i] = 0;
    ppriv[i] = 0;
  }
  note_certificate(host);
  return 0;
}

__export int tls_connect(const char* host, int port) {
  tls_close();
  cert_reset();
  T.err[0] = 0;
  if (!host || !host[0] || strlen(host) > 200) return fail("host invalido");
  {
    int c = k_tcp_connect(host, port);
    if (c == -3) {
      k_tcp_close();
      return fail("interrompido");
    }
    if (c != 0) return fail("nao conectou (DNS/TCP)");
  }
  T.open = 1;
  if (handshake(host) < 0) {
    k_tcp_close();
    T.open = 0;
    return -1;
  }
  if (!T.app) T.app = (u8*)malloc(REC_MAX + 64);
  if (!T.app) {
    tls_close();
    return fail("sem memoria");
  }
  return 0;
}

__export int tls_send(const void* data, int len) {
  const u8* p = (const u8*)data;
  int left = len;
  if (!T.open) return fail("sem conexao");
  while (left > 0) {
    int n = left > 16384 ? 16384 : left;
    if (send_encrypted(23, p, n) < 0) return -1;
    p += n;
    left -= n;
  }
  return len;
}

/* KeyUpdate do servidor: so a chave de leitura muda */
static void key_update_read(void) {
  u8 next[32];
  expand_label(T.rd.secret, "traffic upd", 0, 0, next, 32);
  set_keys(&T.rd, next);
}

__export int tls_recv(void* data, int cap) {
  int type, len, inner, plen, n;
  u8* plain;
  if (!T.open) return T.err[0] ? -1 : 0;
  while (T.app_pos >= T.app_len) {
    {
      int rr = read_record(&type, T.app, &len);
      if (rr == -3) return -3;
      if (rr < 0) {
        if (strcmp(T.err, "servidor fechou a conexao") == 0) {
          T.open = 0;
          return 0;
        }
        return -1;
      }
    }
    if (type == 20) continue;
    if (type != 23) return fail("registro inesperado");
    if (open_record(T.app, len, &inner, &plain, &plen) < 0) return -1;
    if (inner == 23) {
      T.app_pos = 5;
      T.app_len = 5 + plen;
    } else if (inner == 21) {
      if (plen >= 2 && plain[1] == 0) {  /* close_notify */
        T.open = 0;
        k_tcp_close();
        return 0;
      }
      return fail_alert(plen >= 2 ? plain[1] : 0);
    } else if (inner == 22) {
      /* NewSessionTicket (4) e ignorado; KeyUpdate (24) troca a chave */
      if (plen >= 4 && plain[0] == 24) key_update_read();
    }
  }
  n = T.app_len - T.app_pos;
  if (n > cap) n = cap;
  memcpy(data, T.app + T.app_pos, (size_t)n);
  T.app_pos += n;
  return n;
}

__export void tls_close(void) {
  if (T.open) {
    static const u8 close_notify[2] = { 1, 0 };
    send_encrypted(21, close_notify, 2);
    k_tcp_close();
  }
  T.open = 0;
  T.in_len = 0;
  T.hs_len = 0;
  T.app_pos = 0;
  T.app_len = 0;
}

__export int tls_cert_ok(void) { return cert_ok; }

__export const char* tls_security(void) {
  return sec_label[0] ? sec_label : "HTTPS, certificado NAO verificado";
}
