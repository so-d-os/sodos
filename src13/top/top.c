/* top.c - TOP do SODOS. Tres telas: Processos (o que esta na memoria e
 * quanto ocupa), Mapa (a RAM toda num quadro) e Video (placa e VRAM).
 * F3 ou Tab passa para a proxima.
 * Fora da tela de texto (WIN, saida desviada) ou com -l imprime a lista. */
#include "top.h"

/* Lista simples, como o top em modo batch. */
static void lista(void) {
  char a[16], b[16], c[16];
  int i;
  dados_le();
  fmt_kb(a, res.ram_kb);
  fmt_kb(b, res.ram_kb - res.livre_kb);
  fmt_kb(c, res.livre_kb);
  printf("RAM %s, em uso %s, livre %s\n", a, b, c);
  printf(" PID NOME         TIPO    ESTADO    BASE      IMAGEM   DADOS\n");
  for (i = 0; i < n_procs; i++) {
    const struct proc* p = &procs[i];
    char pid[8], base[12];
    if (p->k.pid) sprintf(pid, "%u", p->k.pid);
    else strcpy(pid, "-");
    if (p->k.base) sprintf(base, "%08X", p->k.base);
    else strcpy(base, "-");
    fmt_kb(a, p->k.image_kb);
    fmt_kb(b, p->k.data_kb);
    printf("%4s %-12s %-7s %-9s %-8s %7s %7s\n", pid, p->k.name, nome_tipo(p->k.kind),
           nome_estado(p->k.state), base, p->k.image_kb ? a : "-", p->k.data_kb ? b : "-");
  }
}

int main(int argc, char** argv) {
  unsigned destaque = 0;
  int aba = 0;
  if ((argc > 1 && (!strcmp(argv[1], "-l") || !strcmp(argv[1], "-L"))) || tela_inicia() != 0) {
    lista();
    return 0;
  }
  for (;;) {
    int r = aba == 0 ? tela_procs(&destaque) : aba == 1 ? tela_mapa(&destaque) : tela_video();
    if (!r) break;
    aba = (aba + 1) % 3;
  }
  tela_fim();
  return 0;
}
