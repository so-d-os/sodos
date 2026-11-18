/* hello.c - Primeiro EXE do Windows no SODOS (marco W0).
 * zig cc -target x86_64-windows-gnu: PE32+ de console, ucrt + KERNEL32. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static __thread int tls_counter = 5; /* .tls do PE */

int main(int argc, char** argv) {
  DWORD w;
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  char* buf;
  int i;
  printf("Ola do Windows no SODOS! argc=%d\n", argc);
  for (i = 0; i < argc; i++) printf("  argv[%d] = %s\n", i, argv[i]);
  printf("int %d, neg %d, hex 0x%08X, long long %lld, double %.3f, str [%-6s], char %c\n", 42, -7, 0xBEEFu,
         1234567890123LL, 3.14159, "ab", 'Z');
  WriteFile(out, "WriteFile ok\r\n", 14, &w, NULL);
  tls_counter += 2;
  printf("TLS: %d (esperado 7)\n", tls_counter);
  buf = (char*)malloc(64);
  strcpy(buf, "malloc ok");
  printf("%s, GetCommandLineA = %s\n", buf, GetCommandLineA());
  free(buf);
  printf("GetTickCount = %lu\n", (unsigned long)GetTickCount());
  return 42;
}
