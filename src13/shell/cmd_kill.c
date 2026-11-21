/* cmd_kill.c - encerrar programas a forca, com a memoria que reservaram.
 *
 *   killall          encerra todos os programas abertos depois do inicio
 *                    (ficam o AUTOEXEC, os servicos e o que o WIN abriu
 *                    sozinho); sem EXE do Windows, o WINE.DLL tambem sai
 *   kill <pid>       encerra esse programa (e os que ele chamou)
 *   kill <nome>      encerra todos com esse nome (ex.: kill claude)
 *
 * Quem fecha as janelas e solta pilha, heap, DLLs, threads e soquetes e o
 * laco do WIN (visual.c, win_kill); fora do WIN nao ha outro programa rodando.
 */
#include "shell.h"
#include "visual.h"
#include "module.h"
#include "mm.h"

static void put_n(int n) {
  char b[16];
  uint32_to_string((uint32_t)(n < 0 ? 0 : n), b, (int)sizeof(b));
  puts_k(b);
}

static void free_line(void) {
  uint32_t total = 0, used = 0, freeb = 0, largest = 0;
  void um_stats(uint32_t* total, uint32_t* used, uint32_t* free_bytes, uint32_t* largest);
  um_stats(&total, &used, &freeb, &largest);
  puts_k("Memoria alta livre: ");
  put_n((int)(freeb >> 20));
  puts_k(" MB de ");
  put_n((int)(total >> 20));
  puts_k(" MB\n");
}

static int report(int n) {
  if (n == -1) {
    puts_k("O WIN nao esta aberto: fora dele so roda o programa da tela.\n");
    return 1;
  }
  if (n == -2) {
    puts_k("O WIN nao respondeu (esta em outra tela?). Tente de dentro do WIN.\n");
    return 1;
  }
  puts_k("Programas encerrados: ");
  put_n(n);
  puts_k("\n");
  free_line();
  return 0;
}

int cmd_killall(int argc, char** argv) {
  (void)argc;
  (void)argv;
  if (argc >= 2 && argv[1][0] != '*') {
    puts_k("Uso: killall   (ou killall *) - encerra tudo menos os programas de inicio\n");
    return 1;
  }
  return report(win_kill(0, 1));
}

int cmd_kill(int argc, char** argv) {
  const char* a;
  uint32_t pid = 0, pids[16];
  int i, n, total = 0;
  if (argc < 2) {
    puts_k("Uso: kill <pid> | kill <nome>   (killall: todos menos os de inicio)\n");
    return 1;
  }
  a = argv[1];
  if (a[0] == '*' && !a[1]) return report(win_kill(0, 1));
  for (i = 0; a[i] >= '0' && a[i] <= '9'; i++) pid = pid * 10u + (uint32_t)(a[i] - '0');
  if (i > 0 && !a[i]) return report(win_kill(pid, 0));
  n = win_task_pids(a, pids, 16);
  if (n < 0) return report(-1);
  if (n == 0) {
    puts_k("Nenhum programa com esse nome no WIN.\n");
    return 1;
  }
  for (i = 0; i < n; i++) {
    int r = win_kill(pids[i], 0);
    if (r < 0) return report(r);
    total += r;
  }
  return report(total);
}
