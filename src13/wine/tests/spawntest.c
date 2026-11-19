/* spawntest.c - CreateProcess em paralelo: o filho roda ao mesmo tempo que o
 * pai, herda um handle de evento (numero passado na linha de comando),
 * sinaliza o evento e sai com codigo 7. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc >= 3 && strcmp(argv[1], "filho") == 0) {
    HANDLE ev = (HANDLE)(ULONG_PTR)strtoull(argv[2], NULL, 16);
    Sleep(500);
    printf("filho: pid %lu, sinalizando o evento %p\n", GetCurrentProcessId(), ev);
    SetEvent(ev);
    Sleep(1500);
    return 7;
  }
  {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE ev = CreateEventW(&sa, TRUE, FALSE, NULL);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char line[300];
    DWORD code = 0, t0 = GetTickCount(), w;
    int ok = 1;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    snprintf(line, sizeof(line), "\"%s\" filho %llx", argv[0], (unsigned long long)(ULONG_PTR)ev);
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
      printf("CreateProcess falhou %lu\n", GetLastError());
      return 1;
    }
    printf("pai: pid %lu, filho pid %lu, CreateProcess voltou em %lu ms\n", GetCurrentProcessId(),
           GetProcessId(pi.hProcess), GetTickCount() - t0);
    w = WaitForSingleObject(ev, 10000);
    printf("pai: evento herdado %s em %lu ms\n", w == 0 ? "sinalizado pelo filho" : "NAO sinalizado",
           GetTickCount() - t0);
    if (w != 0) ok = 0;
    w = WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &code);
    printf("pai: filho terminou (%s), codigo %lu, em %lu ms\n", w == 0 ? "ok" : "timeout", code,
           GetTickCount() - t0);
    if (code != 7 || GetProcessId(pi.hProcess) == GetCurrentProcessId()) ok = 0;
    printf("%s\n", ok ? "SPAWNTEST OK" : "SPAWNTEST FALHOU");
    return ok ? 0 : 1;
  }
}
