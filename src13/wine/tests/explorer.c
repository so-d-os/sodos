/* explorer.c - abre um app instalado como os instaladores fazem:
 *   explorer.exe shell:AppsFolder\<familia>!<app>   e   explorer.exe <proto>://
 * (o explorer.exe interno do WINE.DLL resolve pelo AppxManifest.xml). */
#include <windows.h>
#include <stdio.h>

static void run(const wchar_t* line) {
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  wchar_t buf[512];
  DWORD code = 99;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  lstrcpyW(buf, line);
  if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    printf("CreateProcess falhou: %lu\n", GetLastError());
    return;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  GetExitCodeProcess(pi.hProcess, &code);
  printf("explorer terminou, codigo %lu\n", code);
}

int main(void) {
  wchar_t sys[300], line[512];
  GetEnvironmentVariableW(L"SystemRoot", sys, 300);
  lstrcpyW(line, L"\"");
  lstrcatW(line, sys);
  lstrcatW(line, L"\\SYSTEM32\\explorer.exe\" shell:AppsFolder\\Teste_abc123!App");
  run(line);
  lstrcpyW(line, L"\"");
  lstrcatW(line, sys);
  lstrcatW(line, L"\\SYSTEM32\\explorer.exe\" testeproto://abrir");
  run(line);
  return 0;
}
