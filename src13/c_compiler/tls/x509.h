/* x509.h - conferencia do certificado TLS 1.3/1.2 (RSA e ECDSA P-256/P-384, SHA-256/384). */
#ifndef X509_H
#define X509_H
#include "crypto.h"

/* 0 = nome, cadeia e CertificateVerify conferem.
 * why recebe um motivo curto quando devolve -1. */
int x509_tls_check(const char* host, const u8* cert_msg, int cert_msg_len,
                   int scheme, const u8* sig, int sig_len, const u8 th[32],
                   char* why, int why_max);

/* TLS 1.2: cert_msg sem contexto; a assinatura do ServerKeyExchange cobre
 * client_random || server_random || parametros ECDHE (signed_data). */
int x509_tls12_check(const char* host, const u8* cert_msg, int cert_msg_len,
                     int scheme, const u8* sig, int sig_len, const u8* signed_data,
                     int signed_len, char* why, int why_max);

#endif
