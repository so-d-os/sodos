/* pipetest.c - named pipes entre processos (como o Mojo e o crashpad usam):
 * servidor OVERLAPPED + IOCP, cliente em outro processo, dados nos dois
 * sentidos, pipe quebrado quando o cliente sai; e CreatePipe anonimo. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define NAME L"\\\\.\\pipe\\sodos-pipetest"
static int ok = 1;
static void check(int c, const char* what) {
  printf("%s %s\n", c ? "ok  " : "FALHOU", what);
  if (!c) ok = 0;
}

static int cliente(void) {
  HANDLE h;
  char buf[64];
  DWORD n = 0;
  int i;
  for (i = 0; i < 50; i++) {
    h = CreateFileW(NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) break;
    WaitNamedPipeW(NAME, 100);
  }
  if (h == INVALID_HANDLE_VALUE) {
    printf("cliente: nao conectou (%lu)\n", GetLastError());
    return 1;
  }
  WriteFile(h, "ola do cliente", 14, &n, NULL);
  memset(buf, 0, sizeof(buf));
  ReadFile(h, buf, sizeof(buf) - 1, &n, NULL);
  printf("cliente: recebeu \"%s\"\n", buf);
  CloseHandle(h);
  return strcmp(buf, "resposta do servidor") == 0 ? 0 : 2;
}

int main(int argc, char** argv) {
  HANDLE pipe, port, rd, wr;
  OVERLAPPED ov;
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  char line[300], buf[64];
  DWORD n, code = 9;
  ULONG_PTR key;
  LPOVERLAPPED got;
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc >= 2 && strcmp(argv[1], "cliente") == 0) return cliente();

  /* anonimo, no mesmo processo */
  check(CreatePipe(&rd, &wr, NULL, 0), "CreatePipe");
  WriteFile(wr, "abc", 3, &n, NULL);
  memset(buf, 0, sizeof(buf));
  check(ReadFile(rd, buf, sizeof(buf), &n, NULL) && n == 3 && memcmp(buf, "abc", 3) == 0, "pipe anonimo: dados");
  CloseHandle(wr);
  check(!ReadFile(rd, buf, sizeof(buf), &n, NULL) && GetLastError() == ERROR_BROKEN_PIPE,
        "pipe anonimo: ERROR_BROKEN_PIPE depois de fechar a escrita");
  CloseHandle(rd);

  pipe = CreateNamedPipeW(NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096,
                          0, NULL);
  check(pipe != INVALID_HANDLE_VALUE, "CreateNamedPipe (OVERLAPPED)");
  port = CreateIoCompletionPort(pipe, NULL, 77, 0);
  check(port != NULL, "CreateIoCompletionPort no pipe");
  memset(&ov, 0, sizeof(ov));
  check(!ConnectNamedPipe(pipe, &ov) && GetLastError() == ERROR_IO_PENDING, "ConnectNamedPipe pendente");

  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  snprintf(line, sizeof(line), "\"%s\" cliente", argv[0]);
  check(CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi), "CreateProcess do cliente");

  check(GetQueuedCompletionStatus(port, &n, &key, &got, 10000) && key == 77 && got == &ov,
        "IOCP: cliente conectou");
  memset(&ov, 0, sizeof(ov));
  memset(buf, 0, sizeof(buf));
  if (!ReadFile(pipe, buf, sizeof(buf) - 1, &n, &ov)) check(GetLastError() == ERROR_IO_PENDING, "ReadFile pendente");
  check(GetQueuedCompletionStatus(port, &n, &key, &got, 10000) && got == &ov && n == 14, "IOCP: dados do cliente");
  check(strcmp(buf, "ola do cliente") == 0, "conteudo recebido");
  memset(&ov, 0, sizeof(ov));
  WriteFile(pipe, "resposta do servidor", 20, &n, &ov);
  check(GetQueuedCompletionStatus(port, &n, &key, &got, 10000) && n == 20, "IOCP: escrita concluida");

  WaitForSingleObject(pi.hProcess, 10000);
  GetExitCodeProcess(pi.hProcess, &code);
  check(code == 0, "cliente leu a resposta e saiu com 0");
  memset(&ov, 0, sizeof(ov));
  check(!ReadFile(pipe, buf, sizeof(buf), &n, &ov) && GetLastError() == ERROR_BROKEN_PIPE,
        "servidor: ERROR_BROKEN_PIPE depois que o cliente saiu");
  CloseHandle(pipe);
  CloseHandle(port);
  printf("%s\n", ok ? "PIPETEST OK" : "PIPETEST FALHOU");
  return ok ? 0 : 1;
}
