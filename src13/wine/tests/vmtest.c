/* vmtest.c - memoria virtual do W4 como o V8 usa: reserva enorme so de
 * enderecos, commit sob demanda, protecao por pagina com falha tratada,
 * decommit/recommit zerado, reserva em endereco alinhado. */
#include <windows.h>
#include <stdio.h>

static volatile char* fault_page;
static volatile int faults;

static LONG CALLBACK on_fault(EXCEPTION_POINTERS* e) {
  DWORD old;
  printf("handler: codigo %lx endereco %p\n", e->ExceptionRecord->ExceptionCode, (void*)e->ExceptionRecord->ExceptionInformation[1]);
  if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  if ((char*)e->ExceptionRecord->ExceptionInformation[1] < (char*)fault_page ||
      (char*)e->ExceptionRecord->ExceptionInformation[1] >= (char*)fault_page + 4096)
    return EXCEPTION_CONTINUE_SEARCH;
  faults++;
  printf("handler: falha em %p (pagina %p)\n", (void*)e->ExceptionRecord->ExceptionInformation[1], (void*)fault_page);
  VirtualProtect((void*)fault_page, 4096, PAGE_READWRITE, &old);
  return EXCEPTION_CONTINUE_EXECUTION;
}

static int ok = 1;
static void check(int c, const char* what) {
  printf("%s %s\n", c ? "ok  " : "FALHOU", what);
  if (!c) ok = 0;
}

int main(void) {
  SIZE_T big = (SIZE_T)64 << 30, i;
  char* p = (char*)VirtualAlloc(NULL, big, MEM_RESERVE, PAGE_NOACCESS);
  char* q;
  MEMORY_BASIC_INFORMATION mi;
  DWORD old = 0, t0;
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("reserva de 64 GB em %p\n", p);
  check(p != NULL, "reserva de 64 GB");
  if (!p) return 1;

  q = (char*)VirtualAlloc(p + ((SIZE_T)1 << 30), 1 << 20, MEM_COMMIT, PAGE_READWRITE);
  check(q == p + ((SIZE_T)1 << 30), "commit de 1 MB no meio");
  check(q && q[0] == 0 && q[4095] == 0 && q[(1 << 20) - 1] == 0, "paginas novas zeradas");
  for (i = 0; i < (1 << 20); i++) q[i] = (char)i;
  for (i = 0; i < (1 << 20); i++)
    if (q[i] != (char)i) break;
  check(i == (1 << 20), "escrita e leitura de 1 MB");

  VirtualQuery(p, &mi, sizeof(mi));
  check(mi.State == MEM_RESERVE && mi.RegionSize == ((SIZE_T)1 << 30), "VirtualQuery: 1 GB so reservado");
  VirtualQuery(q, &mi, sizeof(mi));
  check(mi.State == MEM_COMMIT && mi.RegionSize == (1 << 20) && mi.Protect == PAGE_READWRITE,
        "VirtualQuery: 1 MB commitado RW");

  check(VirtualProtect(q, 4096, PAGE_READONLY, &old) && old == PAGE_READWRITE, "VirtualProtect RW -> RO");
  check(VirtualProtect(q, 4096, PAGE_READWRITE, &old) && old == PAGE_READONLY, "VirtualProtect RO -> RW");

  AddVectoredExceptionHandler(1, on_fault);
  fault_page = q + 8192;
  VirtualProtect((void*)fault_page, 4096, PAGE_NOACCESS, &old);
  fault_page[10] = 42; /* falha -> handler libera -> repete */
  check(faults == 1 && fault_page[10] == 42, "falha em pagina sem acesso tratada pelo handler");

  check(VirtualFree(q, 1 << 20, MEM_DECOMMIT), "decommit");
  VirtualQuery(q, &mi, sizeof(mi));
  check(mi.State == MEM_RESERVE, "VirtualQuery depois do decommit");
  q = (char*)VirtualAlloc(q, 4096, MEM_COMMIT, PAGE_READWRITE);
  check(q && q[10] == 0, "recommit volta zerado");

  check(VirtualFree(p, 0, MEM_RELEASE), "release dos 64 GB");

  /* padrao do V8/Go: reserva N + alinhamento, solta, reserva o trecho alinhado */
  p = (char*)VirtualAlloc(NULL, (SIZE_T)8 << 30, MEM_RESERVE, PAGE_NOACCESS);
  q = (char*)(((ULONG_PTR)p + ((ULONG_PTR)4 << 30) - 1) & ~(((ULONG_PTR)4 << 30) - 1));
  VirtualFree(p, 0, MEM_RELEASE);
  p = (char*)VirtualAlloc(q, (SIZE_T)4 << 30, MEM_RESERVE, PAGE_NOACCESS);
  check(p == q, "reserva de 4 GB alinhada em endereco pedido");

  t0 = GetTickCount();
  q = (char*)VirtualAlloc(p, 256 << 20, MEM_COMMIT, PAGE_READWRITE);
  for (i = 0; q && i < (256u << 20); i += 4096) q[i] = 1;
  printf("commit e toque de 256 MB: %lu ms\n", GetTickCount() - t0);
  check(q == p, "commit de 256 MB");
  check(VirtualFree(p, 0, MEM_RELEASE), "release");
  printf("%s\n", ok ? "VMTEST OK" : "VMTEST FALHOU");
  return ok ? 0 : 1;
}
