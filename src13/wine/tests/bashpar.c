/* bashpar.c - bash.exe do Claude Code em paralelo (wine/dll/bashbox.c).
 *
 * O Claude Code abre bash, git e o rg ao mesmo tempo. Antes o bash.exe (o
 * WSL.EXE do SODOS) ocupava a tarefa principal do processo ate o comando
 * acabar, e todo outro CreateProcess falhava em 60 s (ENOTSUP no uv_spawn).
 *
 *   BASHPAR.EXE [D:\WINE\BIN\BASH.EXE]
 *
 * 1. bash -c "sleep 4; echo lento" fica rodando;
 * 2. bash -c "echo rapido" tem de voltar antes dele (< 3 s);
 * 3. bash -c "sleep 60" + TerminateProcess(h, 9): sai em poucos segundos com 9;
 * 4. o primeiro termina com "lento" no pipe e codigo 0.
 * Resultado tambem em D:\BASHPAR.TXT. Precisa do SET WSL=1 com a imagem.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HANDLE logf = INVALID_HANDLE_VALUE;
static void outs(const char* s) {
  DWORD w;
  fputs(s, stdout);
  if (logf != INVALID_HANDLE_VALUE) WriteFile(logf, s, (DWORD)strlen(s), &w, NULL);
}

struct kid {
  HANDLE proc, rd;
  char buf[2048];
  DWORD n;
};

static int start(struct kid* k, const wchar_t* exe, const wchar_t* cmd) {
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  HANDLE wr;
  static wchar_t line[1024];
  memset(k, 0, sizeof(*k));
  if (!CreatePipe(&k->rd, &wr, &sa, 0)) return 0;
  SetHandleInformation(k->rd, HANDLE_FLAG_INHERIT, 0);
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = wr;
  swprintf(line, 1024, L"bash -c -l \"%ls\"", cmd);
  if (!CreateProcessW(exe, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
    CloseHandle(wr);
    return 0;
  }
  CloseHandle(wr);
  CloseHandle(pi.hThread);
  k->proc = pi.hProcess;
  return 1;
}

/* le ate o EOF e espera o fim; devolve o codigo */
static DWORD finish(struct kid* k) {
  DWORD n, code = 999;
  while (k->n < sizeof(k->buf) - 1 && ReadFile(k->rd, k->buf + k->n, (DWORD)(sizeof(k->buf) - 1 - k->n), &n, NULL) && n > 0)
    k->n += n;
  k->buf[k->n] = 0;
  WaitForSingleObject(k->proc, 120000);
  GetExitCodeProcess(k->proc, &code);
  CloseHandle(k->rd);
  CloseHandle(k->proc);
  return code;
}

int main(int argc, char** argv) {
  wchar_t exe[MAX_PATH] = L"D:\\WINE\\BIN\\BASH.EXE";
  struct kid lento, rapido, morto;
  char m[2600];
  DWORD c1, c2, c3, t0, t_rapido, t_morto, t_lento;
  int ok = 1;
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc > 1) MultiByteToWideChar(CP_ACP, 0, argv[1], -1, exe, MAX_PATH);
  logf = CreateFileW(L"D:\\BASHPAR.TXT", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  t0 = GetTickCount();
  if (!start(&lento, exe, L"sleep 4; echo lento; pwd; gcc --version | head -1")) {
    outs("CreateProcess (lento) falhou\n");
    return 1;
  }
  if (!start(&rapido, exe, L"echo rapido")) {
    snprintf(m, sizeof m, "CreateProcess (rapido) falhou, erro %lu\n", GetLastError());
    outs(m);
    return 1;
  }
  c2 = finish(&rapido);
  t_rapido = GetTickCount() - t0;
  snprintf(m, sizeof m, "rapido: codigo %lu em %lu ms, saida: %s", c2, t_rapido, rapido.buf);
  outs(m);
  if (c2 != 0 || !strstr(rapido.buf, "rapido") || t_rapido > 3000) ok = 0;

  if (!start(&morto, exe, L"sleep 60; echo nao devia")) {
    outs("CreateProcess (morto) falhou\n");
    return 1;
  }
  Sleep(1000);
  t_morto = GetTickCount();
  if (!TerminateProcess(morto.proc, 9)) outs("TerminateProcess devolveu FALSE\n");
  c3 = finish(&morto);
  t_morto = GetTickCount() - t_morto;
  snprintf(m, sizeof m, "morto: codigo %lu %lu ms depois do TerminateProcess, saida: [%s]\n", c3, t_morto, morto.buf);
  outs(m);
  if (c3 != 9 || t_morto > 10000 || strstr(morto.buf, "nao devia")) ok = 0;

  c1 = finish(&lento);
  t_lento = GetTickCount() - t0;
  snprintf(m, sizeof m, "lento: codigo %lu em %lu ms, saida:\n%s", c1, t_lento, lento.buf);
  outs(m);
  if (c1 != 0 || !strstr(lento.buf, "lento")) ok = 0;
  outs(ok ? "BASHPAR OK\n" : "BASHPAR FALHOU\n");
  CloseHandle(logf);
  return ok ? 0 : 1;
}
