/* cmd_smp.c - CPUs do SMP: lista o estado e roda um comando numa CPU.
 *
 *   smp                      lista as CPUs e o uso de cada uma
 *   smp run <cpu> <comando>  roda o comando (interno ou .EXE) naquela CPU
 *   smp win <cpu>            abre o WIN naquela CPU
 *   smp wine                 grava na serial o que cada thread do Windows
 *                            espera (o [wine-dump] do REBOOT, sem reiniciar)
 *   smp travar laco|cli|bkl [s]  trava de proposito (teste do Ctrl+Alt+Del,
 *                            kernel/hang.c): o mundo num laco com IF=1, com
 *                            IF=0, ou uma thread com o BKL; s = espera antes
 *
 * Com run/win o "mundo" (shell, WIN, drivers) muda para aquela CPU enquanto
 * o comando roda; a CPU0 fica com as threads dos programas (ou em hlt).
 * Teclado, tela e rede continuam sendo de uma CPU de cada vez.
 * A lista tambem manda para a serial os contadores do escalonador ([kt]).
 */
#include "shell.h"
#include "smp.h"
#include "visual.h"
#include "kthread.h"
#include "hang.h"
#include "wine.h"

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
  kt_dump();
  {
    /* video/visual.c: tarefas do WIN acordadas por futex (thread principal
     * do Firefox) que passaram na frente e que tiraram a da vez */
    extern void gui_woke_stats(uint32_t* picks, uint32_t* preempts);
    extern int gui_woke_off(void);
    uint32_t picks = 0, preempts = 0;
    gui_woke_stats(&picks, &preempts);
    puts_k("  WIN: acordadas na frente ");
    put_u(picks);
    puts_k(", tiraram a da vez ");
    put_u(preempts);
    puts_k(gui_woke_off() ? " (desligado: SET WINWOKE=0)\n" : "\n");
  }
  puts_k("\n  smp run <cpu> <comando>   roda um comando naquela CPU\n");
  puts_k("  smp win <cpu>             abre o WIN naquela CPU\n");
  puts_k("  smp wine                  grava na serial o que cada thread do Windows espera\n");
}

extern uint64_t prof_buf[];
extern volatile uint32_t prof_i;
extern volatile int prof_on;
int file_save(const char* path, const char* data, int size);
void serial_puts(const char* s);

/* Grava sozinho: com o sistema travado nao da para digitar "smp prof save".
 * Uma thread fora da CPU0 grava a cada 60 s em D:\LOG\PROF0..4.BIN (rodizio);
 * cada arquivo tem os ultimos ~65 s (o anel e preenchido pela interrupcao do
 * timer, entao o trecho travado entra mesmo que a gravacao atrase). */
static volatile int prof_saver_word, prof_saver_on;
static int prof_saver(void* arg) {
  static char path[] = "D:\\LOG\\PROF0.BIN";
  int k = 0;
  (void)arg;
  while (prof_on) {
    uint32_t n;
    kt_futex_wait(&prof_saver_word, 0, 60000);
    if (!prof_on) break;
    n = prof_i < 131072u ? prof_i : 131072u;
    path[11] = (char)('0' + k);
    k = (k + 1) % 5;
    kt_bkl_enter_kernel();
    if (file_save(path, (const char*)prof_buf, (int)(n * 8u)) >= 0) {
      serial_puts("[prof] gravado ");
      serial_puts(path);
      serial_puts("\n");
    }
    kt_bkl_leave();
  }
  prof_saver_on = 0;
  return 0;
}

int cmd_smp(int argc, char** argv) {
  int cpu = 0, i;
  /* smp travar laco|cli|bkl [s]: teste do Ctrl+Alt+Del / HANG.LOG */
  if (argc >= 3 && seq(argv[1], "travar")) {
    int s = 0;
    if (argc >= 4 && num(argv[3], &s) != 0) s = 0;
    puts_k("Travando de proposito");
    if (s > 0) {
      puts_k(" daqui a ");
      put_u((unsigned)s);
      puts_k(" s");
    }
    puts_k(". Ctrl+Alt+Del: tela 1 e \\LOG\\HANG.LOG.\n");
    if (hang_test(argv[2], s) != 0) {
      puts_k("Uso: smp travar laco|cli|bkl [segundos]\n");
      return 1;
    }
    return 0;
  }
  /* smp wine: o [wine-dump] do REBOOT na hora, com os programas vivos (um
   * Claude que nao abre a janela: o que cada thread do Windows espera) */
  if (argc >= 2 && seq(argv[1], "wine")) {
    wine_dump_waits();
    puts_k("O que cada thread do Windows espera foi para a serial:\n");
    puts_k("D:\\LOG\\SERIAL.LOG, linhas [wine-dump].\n");
    return 0;
  }
  /* smp prof on|off|save: amostra onde as CPUs estao (isr.c) */
  if (argc >= 3 && seq(argv[1], "prof")) {
    if (seq(argv[2], "on")) {
      prof_i = 0;
      prof_on = 1;
      if (!prof_saver_on && kt_online_count() >= 2) {
        prof_saver_on = 1;
        if (kt_thread_create(prof_saver, 0, 16384, ~1u) <= 0) prof_saver_on = 0;
      }
      puts_k("Amostragem ligada: grava sozinha a cada 60 s em D:\\LOG\\PROF0..4.BIN\n");
      puts_k("(smp prof save grava agora em D:\\LOG\\PROF.BIN).\n");
      return 0;
    }
    if (seq(argv[2], "off")) {
      prof_on = 0;
      prof_saver_word = 1;
      kt_futex_wake(&prof_saver_word, -1);
      puts_k("Amostragem desligada.\n");
      return 0;
    }
    if (seq(argv[2], "save")) {
      uint32_t n = prof_i < 131072u ? prof_i : 131072u;
      const char* path = argc >= 4 ? argv[3] : "D:\\LOG\\PROF.BIN";
      if (file_save(path, (const char*)prof_buf, (int)(n * 8u)) < 0) {
        puts_k("Nao consegui gravar.\n");
        return 1;
      }
      puts_k("Gravadas ");
      put_u(n);
      puts_k(" amostras em ");
      puts_k(path);
      puts_k(".\n");
      return 0;
    }
  }
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
