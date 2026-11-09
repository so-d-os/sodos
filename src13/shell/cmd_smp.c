/* cmd_smp.c - CPUs do SMP: lista o estado e roda um comando numa CPU.
 *
 *   smp                      lista as CPUs e o uso de cada uma
 *   smp run <cpu> <comando>  roda o comando (interno ou .EXE) naquela CPU
 *   smp win <cpu>            abre o WIN naquela CPU
 *
 * A BSP fica parada (hlt) enquanto a tarefa roda, entao ela tem a maquina
 * so para si: teclado, tela e rede sao usados por uma CPU de cada vez.
 */
#include "shell.h"
#include "smp.h"
#include "visual.h"

extern int shell_exec_line(char* line);

static char linha[200];

static void put_u(unsigned n) {
  char b[12];
  int i = 0;
  if (!n) {
    putc('0');
    return;
  }
  while (n && i < 11) {
    b[i++] = (char)('0' + n % 10);
    n /= 10;
  }
  while (i > 0) putc(b[--i]);
}

static int seq(const char* a, const char* b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static int num(const char* s, int* out) {
  int v = 0, n = 0;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    s++;
    n++;
  }
  if (!n) return -1;
  *out = v;
  return 0;
}

static void job_shell(void* arg) {
  (void)arg;
  shell_exec_line(linha);
}

static void job_win(void* arg) {
  (void)arg;
  win_run();
}

static void lista(void) {
  struct k_cpu_stat st;
  int n = smp_cpu_count();
  int i;
  puts_k("CPUs: ");
  put_u((unsigned)n);
  puts_k(" (CPU0 = BSP)\n");
  for (i = 0; i < n; i++) {
    unsigned long long t;
    unsigned pct = 0;
    if (smp_cpu_stat(i, &st) != 0) continue;
    t = (unsigned long long)st.busy_ms + st.idle_ms;
    if (t) pct = (unsigned)((unsigned long long)st.busy_ms * 100u / t);
    puts_k("  CPU");
    put_u((unsigned)i);
    puts_k(i == 0 ? " BSP " : " AP  ");
    puts_k(!st.online ? "offline" : (st.state ? "ativa  " : "hlt    "));
    puts_k(" uso ");
    put_u(pct);
    puts_k("%  tempo ");
    put_u((st.busy_ms + st.idle_ms) / 1000u);
    puts_k("s  apic ");
    put_u(st.id);
    puts_k("\n");
  }
  puts_k("\n  smp run <cpu> <comando>   roda um comando naquela CPU\n");
  puts_k("  smp win <cpu>             abre o WIN naquela CPU\n");
}

int cmd_smp(int argc, char** argv) {
  int cpu = 0, i;
  if (argc >= 4 && seq(argv[1], "run")) {
    char* d = linha;
    int len = 0;
    if (num(argv[2], &cpu) != 0) {
      puts_k("CPU invalida.\n");
      return 1;
    }
    for (i = 3; i < argc && len < (int)sizeof(linha) - 2; i++) {
      const char* s = argv[i];
      if (i > 3) {
        *d++ = ' ';
        len++;
      }
      while (*s && len < (int)sizeof(linha) - 2) {
        *d++ = *s++;
        len++;
      }
    }
    *d = 0;
    puts_k("Rodando '");
    puts_k(linha);
    puts_k("' na CPU ");
    put_u((unsigned)cpu);
    puts_k("...\n");
    if (smp_run(cpu, job_shell, 0) != 0) {
      puts_k("CPU indisponivel (use 1..N-1).\n");
      return 1;
    }
    puts_k("Tarefa concluida na CPU ");
    put_u((unsigned)cpu);
    puts_k(".\n");
    return 0;
  }
  if (argc >= 3 && (seq(argv[1], "win") || seq(argv[1], "WIN"))) {
    if (num(argv[2], &cpu) != 0) {
      puts_k("CPU invalida.\n");
      return 1;
    }
    puts_k("Abrindo o WIN na CPU ");
    put_u((unsigned)cpu);
    puts_k("...\n");
    if (smp_run(cpu, job_win, 0) != 0) {
      puts_k("CPU indisponivel (use 1..N-1).\n");
      return 1;
    }
    puts_k("WIN fechado.\n");
    return 0;
  }
  lista();
  return 0;
}
