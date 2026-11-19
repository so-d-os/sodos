/* fetch.c - FETCH.EXE http://host:porta/caminho ARQUIVO
 * Baixa um arquivo por HTTP/1.0 (sem TLS) e grava em ARQUIVO. Serve para a
 * bancada de teste trocar o WINE.DLL da VM sem reiniciar (o host serve a
 * pasta wine/build e a VM le de 10.0.2.2). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
  WSADATA w;
  char host[128], path[512], req[700], buf[65536];
  int port = 80, n, total = 0, hdr_done = 0;
  const char* u;
  char* p;
  SOCKET s;
  struct sockaddr_in a;
  HANDLE out;
  DWORD wr;
  if (argc < 3 || strncmp(argv[1], "http://", 7) != 0) {
    printf("uso: FETCH http://host:porta/caminho ARQUIVO\n");
    return 2;
  }
  u = argv[1] + 7;
  p = strchr(u, '/');
  n = p ? (int)(p - u) : (int)strlen(u);
  if (n >= (int)sizeof(host)) return 2;
  memcpy(host, u, (size_t)n);
  host[n] = 0;
  strncpy(path, p ? p : "/", sizeof(path) - 1);
  path[sizeof(path) - 1] = 0;
  if ((p = strchr(host, ':')) != NULL) {
    port = atoi(p + 1);
    *p = 0;
  }
  WSAStartup(MAKEWORD(2, 2), &w);
  s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((u_short)port);
  a.sin_addr.s_addr = inet_addr(host);
  if (s == INVALID_SOCKET || connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
    printf("FETCH: sem conexao com %s:%d\n", host, port);
    return 1;
  }
  n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
  send(s, req, n, 0);
  out = CreateFileA(argv[2], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  if (out == INVALID_HANDLE_VALUE) {
    printf("FETCH: nao abre %s\n", argv[2]);
    return 1;
  }
  {
    char head[4096];
    int hn = 0;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
      int k = 0;
      if (!hdr_done) {
        for (k = 0; k < n && !hdr_done; k++) {
          if (hn < (int)sizeof(head) - 1) head[hn++] = buf[k];
          if (hn >= 4 && memcmp(head + hn - 4, "\r\n\r\n", 4) == 0) hdr_done = 1;
        }
        head[hn] = 0;
        if (hdr_done && strncmp(head + 9, "200", 3) != 0) {
          printf("FETCH: resposta %.12s\n", head + 9);
          CloseHandle(out);
          return 1;
        }
        if (!hdr_done) continue;
      }
      WriteFile(out, buf + k, (DWORD)(n - k), &wr, NULL);
      total += n - k;
    }
  }
  CloseHandle(out);
  closesocket(s);
  printf("FETCH: %d bytes em %s\n", total, argv[2]);
  return total > 0 ? 0 : 1;
}
