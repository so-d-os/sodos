/* fstest.c - operacoes de arquivo que o Claude Desktop e o Claude Code fazem
 * e que falhavam no D: (FAT32.DLL) e no Z: (FAT16 do kernel, ramdisk) do PC:
 * criar em pasta que nao existe, nome longo com varios pontos, renomear para
 * nome longo, renomear por cima (MOVEFILE_REPLACE_EXISTING), renomear e mover
 * pasta, renomear pelo handle, apagar pasta (RemoveDirectory e FileDisposition)
 * e muitos arquivos numa pasta so (Code Cache do Chromium).
 *
 *   FSTEST.EXE E:\FST [n]    n = arquivos no teste da pasta cheia (padrao 0)
 *
 * Imprime "ok" ou "FALHOU" por item e o total no fim; sai com o numero de
 * falhas. Cada rodada usa a pasta base nova (apague antes de repetir). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* o mingw do zig nao tem os nomes do Windows 10 */
#define FileDispositionInfoEx_ ((FILE_INFO_BY_HANDLE_CLASS)21)
struct disp_ex {
  DWORD Flags; /* 1 = DELETE, 2 = POSIX_SEMANTICS */
};

static int falhas;
static wchar_t base[200];

static void check(int c, const char* what) {
  if (c) printf("ok      %s\n", what);
  else {
    printf("FALHOU  %s (erro %lu)\n", what, GetLastError());
    falhas++;
  }
}

static const wchar_t* P(const wchar_t* rel) {
  static wchar_t buf[4][400];
  static int k;
  wchar_t* b = buf[k++ & 3];
  swprintf(b, 400, L"%ls\\%ls", base, rel);
  return b;
}

static int put(const wchar_t* path, const char* data) {
  HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  DWORD n = 0;
  int ok;
  if (h == INVALID_HANDLE_VALUE) return 0;
  ok = WriteFile(h, data, (DWORD)strlen(data), &n, NULL) && n == strlen(data);
  ok = CloseHandle(h) && ok;
  return ok;
}

/* 1 se path existe e tem exatamente data */
static int has(const wchar_t* path, const char* data) {
  char buf[512];
  DWORD n = 0;
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, 0, NULL);
  if (h == INVALID_HANDLE_VALUE) return 0;
  memset(buf, 0, sizeof(buf));
  ReadFile(h, buf, sizeof(buf) - 1, &n, NULL);
  CloseHandle(h);
  return n == strlen(data) && memcmp(buf, data, n) == 0;
}

static int gone(const wchar_t* path) {
  return GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
}
static int is_dir(const wchar_t* path) {
  DWORD a = GetFileAttributesW(path);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Quantos itens (sem . e ..) a pasta tem e se name esta entre eles. */
static int listed(const wchar_t* dir, const wchar_t* name, int* count) {
  WIN32_FIND_DATAW fd;
  wchar_t pat[420];
  HANDLE f;
  int found = 0, n = 0;
  swprintf(pat, 420, L"%ls\\*", dir);
  f = FindFirstFileW(pat, &fd);
  if (f != INVALID_HANDLE_VALUE) {
    do {
      if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
      n++;
      if (!wcscmp(fd.cFileName, name)) found = 1;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
  }
  if (count) *count = n;
  return found;
}

/* rename pelo handle (o Bun e o libuv usam FileRenameInfo) */
static int rename_by_handle(const wchar_t* from, const wchar_t* to, int replace) {
  char buf[sizeof(FILE_RENAME_INFO) + 800];
  FILE_RENAME_INFO* ri = (FILE_RENAME_INFO*)buf;
  HANDLE h = CreateFileW(from, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  int ok;
  if (h == INVALID_HANDLE_VALUE) return 0;
  memset(buf, 0, sizeof(buf));
  ri->ReplaceIfExists = (BOOLEAN)replace;
  ri->FileNameLength = (DWORD)(wcslen(to) * 2);
  memcpy(ri->FileName, to, ri->FileNameLength);
  ok = SetFileInformationByHandle(h, FileRenameInfo, ri, (DWORD)sizeof(buf));
  CloseHandle(h);
  return ok;
}

/* apagar pelo handle, como o Zig/Bun fazem no rmdir e no unlink:
 * abrir com DELETE, FileDisposition(Ex), fechar. Devolve o BOOL do Set. */
static int delete_by_handle(const wchar_t* path, int ex, DWORD* err) {
  HANDLE h = CreateFileW(path, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  int ok;
  *err = 0;
  if (h == INVALID_HANDLE_VALUE) {
    *err = GetLastError();
    return 0;
  }
  if (ex) {
    struct disp_ex d = {1 | 2};
    ok = SetFileInformationByHandle(h, FileDispositionInfoEx_, &d, sizeof(d));
  } else {
    FILE_DISPOSITION_INFO d = {TRUE};
    ok = SetFileInformationByHandle(h, FileDispositionInfo, &d, sizeof(d));
  }
  if (!ok) *err = GetLastError();
  CloseHandle(h);
  return ok;
}

static void test_create_and_replace(void) {
  HANDLE h;
  DWORD n, e;
  printf("-- criar em pasta que nao existe, nome com varios pontos, rename por cima\n");
  h = CreateFileW(P(L"jobs\\pins.json.tmp.d0d98f0b"), GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
  e = GetLastError();
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  SetLastError(e);
  check(h == INVALID_HANDLE_VALUE && e == ERROR_PATH_NOT_FOUND, "CreateFile em pasta que nao existe: ERROR_PATH_NOT_FOUND");
  check(gone(P(L"jobs")), "  ... e a pasta continua sem existir");

  check(CreateDirectoryW(P(L"jobs"), NULL), "CreateDirectory jobs");
  h = CreateFileW(P(L"jobs\\pins.json.tmp.d0d98f0b"), GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
  check(h != INVALID_HANDLE_VALUE, "CreateFile CREATE_NEW pins.json.tmp.d0d98f0b");
  if (h != INVALID_HANDLE_VALUE) {
    check(WriteFile(h, "{\"pins\":[1]}", 12, &n, NULL) && n == 12, "  WriteFile");
    check(FlushFileBuffers(h), "  FlushFileBuffers");
    check(CloseHandle(h), "  CloseHandle");
  }
  check(has(P(L"jobs\\pins.json.tmp.d0d98f0b"), "{\"pins\":[1]}"), "  conteudo lido de volta");
  check(put(P(L"jobs\\pins.json"), "{\"pins\":[]}"), "pins.json antigo");
  SetLastError(0);
  check(!MoveFileExW(P(L"jobs\\pins.json.tmp.d0d98f0b"), P(L"jobs\\pins.json"), 0) &&
            GetLastError() == ERROR_ALREADY_EXISTS,
        "MoveFileEx sem REPLACE_EXISTING por cima: ERROR_ALREADY_EXISTS");
  check(MoveFileExW(P(L"jobs\\pins.json.tmp.d0d98f0b"), P(L"jobs\\pins.json"), MOVEFILE_REPLACE_EXISTING),
        "MoveFileEx REPLACE_EXISTING .tmp -> pins.json");
  check(has(P(L"jobs\\pins.json"), "{\"pins\":[1]}"), "  pins.json tem o conteudo novo");
  check(gone(P(L"jobs\\pins.json.tmp.d0d98f0b")), "  o .tmp sumiu");
  check(put(P(L"jobs\\a.b.c.d.e.f.g"), "pontos") && has(P(L"jobs\\a.b.c.d.e.f.g"), "pontos"),
        "arquivo a.b.c.d.e.f.g");
  check(put(P(L"jobs\\.lock.tmp.1.2"), "x") && has(P(L"jobs\\.lock.tmp.1.2"), "x"), "arquivo .lock.tmp.1.2");
}

static void test_rename_long(void) {
  printf("-- renomear para nome longo (cache do Chromium)\n");
  check(CreateDirectoryW(P(L"Code Cache"), NULL) && CreateDirectoryW(P(L"Code Cache\\js"), NULL) &&
            CreateDirectoryW(P(L"Code Cache\\js\\index-dir"), NULL),
        "pastas Code Cache\\js\\index-dir");
  check(put(P(L"Code Cache\\js\\index-dir\\the-real-index"), "indice"), "the-real-index");
  check(MoveFileW(P(L"Code Cache\\js\\index-dir\\the-real-index"), P(L"Code Cache\\js\\index-dir\\todelete_8de4ca6b56563904")),
        "MoveFile -> todelete_8de4ca6b56563904");
  check(has(P(L"Code Cache\\js\\index-dir\\todelete_8de4ca6b56563904"), "indice") &&
            gone(P(L"Code Cache\\js\\index-dir\\the-real-index")),
        "  conteudo no nome novo, o antigo sumiu");
  check(MoveFileW(P(L"Code Cache\\js\\index-dir\\todelete_8de4ca6b56563904"),
                  P(L"Code Cache\\js\\index-dir\\3ad020ad-8502-4977-ac0f-69e5ae6a98fb.tmp")),
        "MoveFile -> 3ad020ad-8502-4977-ac0f-69e5ae6a98fb.tmp");
  check(has(P(L"Code Cache\\js\\index-dir\\3ad020ad-8502-4977-ac0f-69e5ae6a98fb.tmp"), "indice"), "  conteudo");
  SetLastError(0);
  check(!MoveFileW(P(L"Code Cache\\js\\index-dir\\nao-existe"), P(L"Code Cache\\js\\index-dir\\todelete_1")) &&
            GetLastError() == ERROR_FILE_NOT_FOUND,
        "MoveFile de quem nao existe: ERROR_FILE_NOT_FOUND");
  check(DeleteFileW(P(L"Code Cache\\js\\index-dir\\3ad020ad-8502-4977-ac0f-69e5ae6a98fb.tmp")), "DeleteFile");
  check(rename_by_handle(P(L"jobs\\pins.json"), P(L"jobs\\pins.json.bak.20261127"), 0) &&
            has(P(L"jobs\\pins.json.bak.20261127"), "{\"pins\":[1]}"),
        "rename pelo handle (FileRenameInfo) para nome com pontos");
  check(put(P(L"jobs\\pins.json"), "outro") && rename_by_handle(P(L"jobs\\pins.json"), P(L"jobs\\pins.json.bak.20261127"), 1) &&
            has(P(L"jobs\\pins.json.bak.20261127"), "outro") && gone(P(L"jobs\\pins.json")),
        "rename pelo handle por cima (ReplaceIfExists)");
}

static void test_session(void) {
  int n;
  printf("-- sessao do Claude Code: .jsonl e pasta para *.superseded-N, pasta para outra pasta\n");
  check(CreateDirectoryW(P(L"projects"), NULL) && CreateDirectoryW(P(L"projects\\Z--WINEAPP-scratch-2026-11-27-669858"), NULL) &&
            CreateDirectoryW(P(L"projects\\D--SOURCE-SODOS"), NULL),
        "pastas de projeto");
  check(CreateDirectoryW(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222"), NULL) &&
            put(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222\\custom-title.json"), "{\"t\":1}") &&
            put(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222.jsonl"), "{\"a\":1}\n"),
        "sessao antiga (pasta + .jsonl)");
  check(MoveFileExW(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222.jsonl"),
                    P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222.jsonl.superseded-1795796481524"),
                    MOVEFILE_REPLACE_EXISTING),
        "rename .jsonl -> .jsonl.superseded-1795796481524");
  check(has(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222.jsonl.superseded-1795796481524"), "{\"a\":1}\n"),
        "  conteudo");
  check(MoveFileExW(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222"),
                    P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222.superseded-1795796481549"),
                    MOVEFILE_REPLACE_EXISTING),
        "rename da pasta -> .superseded-1795796481549");
  check(has(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222.superseded-1795796481549\\custom-title.json"), "{\"t\":1}") &&
            gone(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222")),
        "  arquivo dentro da pasta renomeada");

  /* troca de pasta: a sessao nova vem do projeto scratch para o D--SOURCE-SODOS */
  check(CreateDirectoryW(P(L"projects\\Z--WINEAPP-scratch-2026-11-27-669858\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222"), NULL) &&
            CreateDirectoryW(P(L"projects\\Z--WINEAPP-scratch-2026-11-27-669858\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222\\subagents"), NULL) &&
            put(P(L"projects\\Z--WINEAPP-scratch-2026-11-27-669858\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222\\custom-title.json"), "{\"t\":2}") &&
            put(P(L"projects\\Z--WINEAPP-scratch-2026-11-27-669858\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222.jsonl"), "{\"b\":2}\n"),
        "sessao nova no projeto scratch");
  check(MoveFileExW(P(L"projects\\Z--WINEAPP-scratch-2026-11-27-669858\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222.jsonl"),
                    P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222.jsonl"), MOVEFILE_REPLACE_EXISTING),
        "mover .jsonl para outra pasta");
  check(MoveFileExW(P(L"projects\\Z--WINEAPP-scratch-2026-11-27-669858\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222"),
                    P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222"), MOVEFILE_REPLACE_EXISTING),
        "mover a pasta da sessao para outra pasta");
  check(has(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222\\custom-title.json"), "{\"t\":2}") &&
            is_dir(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222\\subagents")),
        "  conteudo no lugar novo");
  check(gone(P(L"projects\\Z--WINEAPP-scratch-2026-11-27-669858\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222")) &&
            !listed(P(L"projects\\Z--WINEAPP-scratch-2026-11-27-669858"), L"ff061e9d-fb1b-4fb5-90b7-91d13a7a6222", &n) && n == 0,
        "  o lugar antigo ficou vazio");
  check(put(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222\\subagents\\agent-1.jsonl"), "x") &&
            has(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222\\subagents\\agent-1.jsonl"), "x"),
        "  gravar dentro da pasta movida");
  check(MoveFileExW(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222\\subagents"),
                    P(L"projects\\subagents-movida"), 0) &&
            has(P(L"projects\\subagents-movida\\agent-1.jsonl"), "x") &&
            MoveFileExW(P(L"projects\\subagents-movida"),
                        P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222\\subagents"), 0),
        "mover pasta para cima e de volta");
  SetLastError(0);
  check(!MoveFileExW(P(L"projects\\D--SOURCE-SODOS"), P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222\\x"), 0),
        "mover pasta para dentro dela mesma: recusado");
  check(is_dir(P(L"projects\\D--SOURCE-SODOS\\ff061e9d-fb1b-4fb5-90b7-91d13a7a6222\\subagents")), "  nada mudou");
}

static void test_rmdir(void) {
  DWORD e;
  printf("-- remover pasta\n");
  check(CreateDirectoryW(P(L"rm"), NULL) && CreateDirectoryW(P(L"rm\\session-env"), NULL) &&
            CreateDirectoryW(P(L"rm\\cheia"), NULL) && put(P(L"rm\\cheia\\x.txt"), "x") &&
            CreateDirectoryW(P(L"rm\\model-catalog"), NULL) && CreateDirectoryW(P(L"rm\\vazia-ex"), NULL),
        "pastas rm\\...");
  SetLastError(0);
  check(!RemoveDirectoryW(P(L"rm\\cheia")) && GetLastError() == ERROR_DIR_NOT_EMPTY,
        "RemoveDirectory de pasta com arquivo: ERROR_DIR_NOT_EMPTY");
  check(RemoveDirectoryW(P(L"rm\\session-env")) && gone(P(L"rm\\session-env")), "RemoveDirectory de pasta vazia");
  SetLastError(0);
  check(!delete_by_handle(P(L"rm\\cheia"), 0, &e) && e == ERROR_DIR_NOT_EMPTY,
        "FileDispositionInfo em pasta com arquivo: ERROR_DIR_NOT_EMPTY");
  check(has(P(L"rm\\cheia\\x.txt"), "x"), "  a pasta e o arquivo continuam");
  SetLastError(e);
  check(!delete_by_handle(P(L"rm\\cheia"), 1, &e) && e == ERROR_DIR_NOT_EMPTY,
        "FileDispositionInfoEx em pasta com arquivo: ERROR_DIR_NOT_EMPTY");
  SetLastError(e);
  check(delete_by_handle(P(L"rm\\model-catalog"), 0, &e) && gone(P(L"rm\\model-catalog")),
        "FileDispositionInfo em pasta vazia: removida ao fechar");
  SetLastError(e);
  check(delete_by_handle(P(L"rm\\vazia-ex"), 1, &e) && gone(P(L"rm\\vazia-ex")),
        "FileDispositionInfoEx em pasta vazia: removida ao fechar");
  SetLastError(e);
  check(delete_by_handle(P(L"rm\\cheia\\x.txt"), 1, &e) && gone(P(L"rm\\cheia\\x.txt")),
        "FileDispositionInfoEx em arquivo: apagado ao fechar");
  check(RemoveDirectoryW(P(L"rm\\cheia")) && RemoveDirectoryW(P(L"rm")) && gone(P(L"rm")), "RemoveDirectory do resto");
}

/* Code Cache do Chromium: centenas de "<16 hex>_0" numa pasta so. */
static void test_many(int n) {
  char data[3000], what[160];
  wchar_t name[64], path[420];
  int i, bad = 0, first_bad = -1, cnt = 0;
  DWORD t0 = GetTickCount();
  if (n <= 0) return;
  printf("-- %d arquivos numa pasta (Code Cache\\js)\n", n);
  for (i = 0; i < (int)sizeof(data); i++) data[i] = (char)('a' + i % 26);
  data[sizeof(data) - 1] = 0;
  for (i = 0; i < n; i++) {
    unsigned long long hx = 0x9E3779B97F4A7C15ull * (unsigned long long)(i + 1);
    HANDLE h;
    DWORD w = 0;
    swprintf(name, 64, L"%016llx_0", hx);
    swprintf(path, 420, L"%ls\\Code Cache\\js\\%ls", base, name);
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
      if (first_bad < 0) first_bad = i;
      bad++;
      continue;
    }
    if (!WriteFile(h, data, 1000 + (DWORD)(i % 2000), &w, NULL) || !FlushFileBuffers(h)) {
      if (first_bad < 0) first_bad = i;
      bad++;
    }
    CloseHandle(h);
  }
  snprintf(what, sizeof(what), "%d arquivos gravados (%d falharam, o primeiro no %d) em %lu ms", n - bad, bad,
           first_bad, GetTickCount() - t0);
  SetLastError(0);
  check(bad == 0, what);
  for (i = 0; i < n; i += 97) {
    unsigned long long hx = 0x9E3779B97F4A7C15ull * (unsigned long long)(i + 1);
    WIN32_FILE_ATTRIBUTE_DATA ad;
    swprintf(path, 420, L"%ls\\Code Cache\\js\\%016llx_0", base, hx);
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &ad) || ad.nFileSizeLow != 1000u + (DWORD)(i % 2000)) {
      bad++;
      break;
    }
  }
  check(bad == 0, "  tamanhos conferidos");
  listed(P(L"Code Cache\\js"), L"", &cnt);
  snprintf(what, sizeof(what), "  FindFirstFile lista %d itens (index-dir + %d)", cnt, n);
  check(cnt == n + 1, what);
}

int main(int argc, char** argv) {
  int n = argc > 2 ? atoi(argv[2]) : 0;
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 2) {
    printf("uso: FSTEST E:\\FST [arquivos]\n");
    return 99;
  }
  MultiByteToWideChar(CP_ACP, 0, argv[1], -1, base, 200);
  printf("fstest em %ls\n", base);
  if (!CreateDirectoryW(base, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
    printf("FALHOU  criar %ls (erro %lu)\n", base, GetLastError());
    return 98;
  }
  test_create_and_replace();
  test_rename_long();
  test_session();
  test_rmdir();
  test_many(n);
  printf("fstest: %d falha(s)\n", falhas);
  return falhas;
}
