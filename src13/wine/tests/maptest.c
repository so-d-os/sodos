/* maptest.c - CreateFileMapping/MapViewOfFile: arquivo (leitura e escrita de
 * volta), secao anonima com nome aberta de novo pelo nome. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int ok = 1;
static void check(int c, const char* what) {
  printf("%s %s\n", c ? "ok  " : "FALHOU", what);
  if (!c) ok = 0;
}

int main(void) {
  const char* path = "D:\\WINE\\TEMP\\MAPTEST.DAT";
  HANDLE f, m, m2;
  DWORD n;
  char* v;
  char* v2;
  char buf[64];
  setvbuf(stdout, NULL, _IONBF, 0);
  CreateDirectoryA("D:\\WINE\\TEMP", NULL);

  f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  check(f != INVALID_HANDLE_VALUE, "cria arquivo");
  WriteFile(f, "ola secao do SODOS", 18, &n, NULL);

  m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
  check(m != NULL, "CreateFileMapping (leitura)");
  v = (char*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
  check(v && memcmp(v, "ola secao do SODOS", 18) == 0, "vista mostra o conteudo do arquivo");
  UnmapViewOfFile(v);
  CloseHandle(m);

  m = CreateFileMappingA(f, NULL, PAGE_READWRITE, 0, 0, NULL);
  v = (char*)MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 0);
  check(v != NULL, "vista com escrita");
  if (v) memcpy(v, "OLA", 3);
  check(UnmapViewOfFile(v), "UnmapViewOfFile");
  CloseHandle(m);
  SetFilePointer(f, 0, NULL, FILE_BEGIN);
  memset(buf, 0, sizeof(buf));
  ReadFile(f, buf, 18, &n, NULL);
  check(memcmp(buf, "OLA secao do SODOS", 18) == 0, "escrita da vista voltou para o arquivo");
  CloseHandle(f);
  DeleteFileA(path);

  m = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 1 << 20, L"Local\\sodos-maptest");
  check(m != NULL && GetLastError() == 0, "secao anonima com nome");
  v = (char*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  check(v && v[0] == 0 && v[(1 << 20) - 1] == 0, "secao anonima zerada");
  m2 = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\sodos-maptest");
  check(m2 != NULL, "OpenFileMapping pelo nome");
  v2 = (char*)MapViewOfFile(m2, FILE_MAP_ALL_ACCESS, 0, 0, 4096);
  if (v) strcpy(v, "compartilhado");
  check(v2 && strcmp(v2, "compartilhado") == 0, "as duas vistas enxergam os mesmos bytes");
  {
    HANDLE m3 = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, L"Local\\sodos-maptest");
    check(m3 != NULL && GetLastError() == ERROR_ALREADY_EXISTS, "nome repetido: ERROR_ALREADY_EXISTS");
    CloseHandle(m3);
  }
  UnmapViewOfFile(v2);
  UnmapViewOfFile(v);
  CloseHandle(m2);
  CloseHandle(m);
  check(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\sodos-maptest") == NULL,
        "secao some quando o ultimo handle e a ultima vista fecham");
  printf("%s\n", ok ? "MAPTEST OK" : "MAPTEST FALHOU");
  return ok ? 0 : 1;
}
