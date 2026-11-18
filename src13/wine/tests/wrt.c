/* wrt.c - Mede a gravacao de arquivos por um EXE do Windows no SODOS.
 * WRT.EXE [pasta]: 20 arquivos de 4 KB e um de 8 MB, com os tempos. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

static DWORD save(const char* path, const char* buf, DWORD n) {
  DWORD t0 = GetTickCount(), w = 0;
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    printf("CreateFile falhou: %s (%lu)\n", path, GetLastError());
    return 0;
  }
  WriteFile(h, buf, n, &w, NULL);
  CloseHandle(h);
  return GetTickCount() - t0;
}

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : "D:\\WINE\\TEMP\\WRT";
  char path[260];
  char* big = (char*)malloc(8 << 20);
  DWORD t, total = 0;
  int i;
  memset(big, 'x', 8 << 20);
  CreateDirectoryA("D:\\WINE", NULL);
  CreateDirectoryA("D:\\WINE\\TEMP", NULL);
  CreateDirectoryA(dir, NULL);
  for (i = 0; i < 20; i++) {
    sprintf(path, "%s\\arquivo-pequeno-%02d.txt", dir, i);
    t = save(path, big, 4096);
    total += t;
    if (i < 3) printf("4 KB: %lu ms\n", (unsigned long)t);
  }
  printf("20 arquivos de 4 KB: %lu ms\n", (unsigned long)total);
  sprintf(path, "%s\\grande.bin", dir);
  t = save(path, big, 8 << 20);
  printf("8 MB: %lu ms (%lu KB/s)\n", (unsigned long)t, t ? (unsigned long)(8192u * 1000u / t) : 0);
  return 0;
}
