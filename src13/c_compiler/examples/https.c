/* https.c - GET por TLS 1.3: cc https.c tls.dll
 * Uso: https example.com [/caminho]   (mostra a resposta inteira) */
#include <stdio.h>
#include <string.h>
#include <so.h>
#include <tls.h>

int main(int argc, char** argv) {
  const char* host = argc > 1 ? argv[1] : "example.com";
  const char* path = argc > 2 ? argv[2] : "/";
  char req[512];
  char buf[1024];
  unsigned t0 = k_ms();
  int n, total = 0;
  printf("conectando em %s:443 ...\n", host);
  if (tls_connect(host, 443) != 0) {
    printf("TLS falhou: %s\n", tls_error());
    return 1;
  }
  printf("TLS 1.3 ok em %u ms (certificado NAO verificado)\n", k_ms() - t0);
  snprintf(req, sizeof(req),
           "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SO-https/1.0\r\nConnection: close\r\n\r\n",
           path, host);
  if (tls_send(req, (int)strlen(req)) < 0) {
    printf("envio falhou: %s\n", tls_error());
    return 1;
  }
  while ((n = tls_recv(buf, (int)sizeof(buf) - 1)) > 0) {
    buf[n] = 0;
    fputs(buf, stdout);
    total += n;
  }
  if (n < 0) printf("\n[erro: %s]\n", tls_error());
  printf("\n[%d bytes em %u ms]\n", total, k_ms() - t0);
  tls_close();
  return n < 0;
}
