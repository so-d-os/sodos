/* x509.h - conferencia do certificado TLS 1.3 (RSA e ECDSA P-256/P-384, SHA-256/384). */
#ifndef X509_H
#define X509_H
#include "crypto.h"

/* 0 = nome, cadeia e CertificateVerify conferem.
 * why recebe um motivo curto quando devolve -1. */
int x509_tls_check(const char* host, const u8* cert_msg, int cert_msg_len,
                   int scheme, const u8* sig, int sig_len, const u8 th[32],
                   char* why, int why_max);

#endif
