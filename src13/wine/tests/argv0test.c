/* argv0test.c - CreateProcess com lpApplicationName e um argv[0] que nao e o
 * nome do programa (como o Bun do Claude Code roda o proprio claude.exe com
 * argv0 "rg" para o ripgrep embutido). O filho tem de ver o argv[0] e a
 * linha de comando do pai, nao o caminho do EXE; a saida dele vem pelo pipe
 * do stdout e o pipe termina (EOF) quando ele sai.
 *
 *   ARGV0TEST.EXE                       teste: filho com argv0 "rg-teste"
 *   ARGV0TEST.EXE run EXE NOME args...  roda EXE com argv[0]=NOME e mostra a
 *                                       saida (pipe), o codigo e o tempo
 *                                       (ex.: run D:\CC\CLAUDE.EXE rg --files D:\WSL)
 *
 * O resultado tambem vai para D:\ARGV0.TXT (para ler pela API).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HANDLE logf = INVALID_HANDLE_VALUE;
static void out(const char* s, DWORD n) {
  DWORD w;
  fwrite(s, 1, n, stdout);
  if (logf != INVALID_HANDLE_VALUE) WriteFile(logf, s, n, &w, NULL);
}
static void outs(const char* s) { out(s, (DWORD)strlen(s)); }

/* roda exe com a linha dada, stdout/stderr num pipe; devolve o codigo */
static DWORD roda(const wchar_t* exe, wchar_t* line, DWORD* ms, DWORD* bytes) {
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  HANDLE rd, wr;
  char buf[4096];
  DWORD n, code = 999, t0 = GetTickCount();
  *bytes = 0;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    outs("CreatePipe falhou\n");
    return 998;
  }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = NULL;
  si.hStdOutput = wr;
  si.hStdError = wr;
  if (!CreateProcessW(exe, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
    outs("CreateProcess falhou\n");
    return 997;
  }
  CloseHandle(wr);
  /* ate o EOF: so chega quando o filho sai e a ponta de escrita herdada fecha */
  while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n > 0) {
    if (*bytes < 3000) out(buf, n);
    *bytes += n;
  }
  WaitForSingleObject(pi.hProcess, 600000);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(rd);
  *ms = GetTickCount() - t0;
  return code;
}

int main(int argc, char** argv) {
  wchar_t self[MAX_PATH];
  static wchar_t line[8192];
  char msg[600];
  DWORD code, ms = 0, bytes = 0;
  setvbuf(stdout, NULL, _IONBF, 0);
  if (strcmp(argv[0], "rg-teste") == 0) {
    /* filho: o argv[0] e a linha que o pai pediu, pelo stdout herdado (como
     * o Bun escreve: WriteFile no handle do GetStdHandle) */
    const wchar_t* c = GetCommandLineW();
    DWORD w;
    int ok = wcsncmp(c, L"rg-teste ", 9) == 0 && argc == 2 && strcmp(argv[1], "filho") == 0;
    snprintf(msg, sizeof(msg), "filho: argv[0] = %s, linha = %ls\n", argv[0], c);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), msg, (DWORD)strlen(msg), &w, NULL);
    return ok ? 5 : 6;
  }
  logf = CreateFileW(L"D:\\ARGV0.TXT", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  if (argc >= 4 && (strcmp(argv[1], "run") == 0 || strcmp(argv[1], "run2") == 0)) {
    /* run2: roda duas vezes (a segunda com o EXE ja no cache de imagens do
     * WINE, como no PC com o Claude aberto) */
    wchar_t exe[MAX_PATH];
    int i, vez, vezes = strcmp(argv[1], "run2") == 0 ? 2 : 1;
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, exe, MAX_PATH);
    line[0] = 0;
    for (i = 3; i < argc; i++) {
      wchar_t a[1024];
      int q = strchr(argv[i], ' ') != NULL;
      MultiByteToWideChar(CP_ACP, 0, argv[i], -1, a, 1024);
      if (i > 3) wcscat(line, L" ");
      if (q) wcscat(line, L"\"");
      wcscat(line, a);
      if (q) wcscat(line, L"\"");
    }
    code = 0;
    for (vez = 0; vez < vezes; vez++) {
      static wchar_t l2[8192];
      wcscpy(l2, line); /* o CreateProcessW pode mexer na linha */
      snprintf(msg, sizeof(msg), "rodando %s com a linha: %ls\n", argv[2], l2);
      outs(msg);
      code = roda(exe, l2, &ms, &bytes);
      snprintf(msg, sizeof(msg), "\ncodigo %lu em %lu ms, %lu bytes de saida\n", code, ms, bytes);
      outs(msg);
    }
    CloseHandle(logf);
    return (int)code;
  }
  GetModuleFileNameW(NULL, self, MAX_PATH);
  wcscpy(line, L"rg-teste filho");
  code = roda(self, line, &ms, &bytes);
  snprintf(msg, sizeof(msg), "pai: filho saiu com %lu em %lu ms (esperado 5), %lu bytes pelo pipe\n%s\n", code, ms,
           bytes, code == 5 && bytes > 0 ? "ARGV0TEST OK" : "ARGV0TEST FALHOU");
  outs(msg);
  CloseHandle(logf);
  return code == 5 && bytes > 0 ? 0 : 1;
}
