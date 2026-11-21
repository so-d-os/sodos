/* gittest.c - como o Claude Desktop usa programas do SODOS pelo WINE:
 *   GITTEST git -C D:\REPO rev-parse --show-toplevel
 *     roda D:\WINE\BIN\GIT.EXE com stdout num pipe (CreatePipe +
 *     STARTF_USESTDHANDLES), le ate o fim e mostra a saida e o codigo
 *   GITTEST pick [pasta inicial]
 *     IFileOpenDialog com FOS_PICKFOLDERS (o "Choose folder"): abre o
 *     Explorer no modo selecionar pasta e mostra a escolhida
 * A saida vai tambem para D:\GITTEST.TXT (para ler de fora). */
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>

static FILE* g_log;
static void say(const char* fmt, const char* a, unsigned long n) {
  printf(fmt, a, n);
  if (g_log) {
    fprintf(g_log, fmt, a, n);
    fflush(g_log);
  }
}

static int run_git(int argc, char** argv) {
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  HANDLE rd, wr;
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  char line[1024] = "git", buf[4096];
  DWORD got, code = 0, total = 0, t0 = GetTickCount();
  int i;
  for (i = 2; i < argc; i++) {
    strcat(line, " ");
    if (strchr(argv[i], ' ')) {
      strcat(line, "\"");
      strcat(line, argv[i]);
      strcat(line, "\"");
    } else {
      strcat(line, argv[i]);
    }
  }
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    say("CreatePipe falhou %s%lu\n", "", GetLastError());
    return 1;
  }
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.hStdInput = NULL;
  say("linha: %s%lu\n", line, 0);
  if (!CreateProcessA("D:\\WINE\\BIN\\GIT.EXE", line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
    say("CreateProcess falhou %s%lu\n", "", GetLastError());
    return 1;
  }
  CloseHandle(wr);
  for (;;) {
    if (!ReadFile(rd, buf, sizeof(buf) - 1, &got, NULL) || !got) break;
    buf[got] = 0;
    total += got;
    say("%s%.0lu", buf, 0);
  }
  WaitForSingleObject(pi.hProcess, 10000);
  GetExitCodeProcess(pi.hProcess, &code);
  say("[fim: %s%lu bytes", "", total);
  say(", codigo %s%lu", "", code);
  say(", %s%lu ms]\n", "", GetTickCount() - t0);
  return 0;
}

static int run_pick(int argc, char** argv) {
  IFileOpenDialog* d = NULL;
  IShellItem* it = NULL;
  HRESULT hr;
  FILEOPENDIALOGOPTIONS o = 0;
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&d);
  if (FAILED(hr)) {
    say("CoCreateInstance falhou %s%lx\n", "", (unsigned long)hr);
    return 1;
  }
  IFileOpenDialog_GetOptions(d, &o);
  IFileOpenDialog_SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  IFileOpenDialog_SetTitle(d, L"Escolha a pasta do projeto");
  if (argc > 2) {
    WCHAR w[300];
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, w, 300);
    if (SUCCEEDED(SHCreateItemFromParsingName(w, NULL, &IID_IShellItem, (void**)&it))) {
      IFileOpenDialog_SetFolder(d, it);
      IShellItem_Release(it);
      it = NULL;
    }
  }
  hr = IFileOpenDialog_Show(d, NULL);
  if (FAILED(hr)) {
    say("Show: cancelado %s(0x%lx)\n", "", (unsigned long)hr);
    return 2;
  }
  if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
    PWSTR p = NULL;
    if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
      char a[600];
      WideCharToMultiByte(CP_UTF8, 0, p, -1, a, sizeof(a), NULL, NULL);
      say("pasta escolhida: %s%.0lu\n", a, 0);
      CoTaskMemFree(p);
    }
    IShellItem_Release(it);
  }
  IFileOpenDialog_Release(d);
  return 0;
}

int main(int argc, char** argv) {
  int rc;
  setvbuf(stdout, NULL, _IONBF, 0);
  g_log = fopen("D:\\GITTEST.TXT", "w");
  if (argc > 1 && strcmp(argv[1], "pick") == 0) rc = run_pick(argc, argv);
  else if (argc > 1 && strcmp(argv[1], "git") == 0) rc = run_git(argc, argv);
  else {
    say("uso: GITTEST git args... | GITTEST pick [pasta]%s%.0lu\n", "", 0);
    rc = 1;
  }
  if (g_log) fclose(g_log);
  return rc;
}
