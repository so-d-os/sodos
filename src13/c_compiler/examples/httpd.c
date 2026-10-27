#include <stdio.h>

/* O servico ja escuta no boot. Este EXE so confirma a API. */
int main(void) {
  printf("HTTPD na porta 8797 (10.0.2.15).\n");
  printf("GET /win/printscreen\n");
  printf("GET /win/mouse/coord\n");
  printf("GET /win/windows\n");
  printf("GET /win/close?program=Gerenciador\n");
  printf("GET /win/execute?cmd=help\n");
  return 0;
}
