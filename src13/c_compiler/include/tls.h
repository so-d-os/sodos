/* tls.h - cliente TLS 1.3 (TLS.DLL). Uma conexao por vez, sobre k_tcp_*.
 * Cifra: TLS_AES_128_GCM_SHA256, grupos x25519 e secp256r1.
 * O certificado e conferido (nome, cadeia, assinatura). tls_cert_ok diz
 * se conferiu; tls_security e o texto curto para a barra de status. */
#ifndef _TLS_H
#define _TLS_H

/* 0 = ok; <0 = erro (veja tls_error) */
int tls_connect(const char* host, int port);
/* devolve len ou -1 */
int tls_send(const void* data, int len);
/* devolve bytes lidos, 0 quando o servidor fecha, -1 em erro */
int tls_recv(void* data, int cap);
void tls_close(void);
const char* tls_error(void);
/* 1 se o certificado desta conexao foi conferido */
int tls_cert_ok(void);
/* "HTTPS" ou "HTTPS, certificado NAO verificado: ..." */
const char* tls_security(void);

#endif
