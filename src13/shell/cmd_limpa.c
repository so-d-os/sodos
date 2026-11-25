/* cmd_limpa.c - LIMPA: reinicio leve, sem reiniciar o PC.
 *
 *   limpa      fecha tudo e roda o fim do boot de novo
 *   limpa /t   so mostra o que fecharia (nao mexe em nada)
 *
 * Fecha todos os programas: os do WIN (inclusive os que ele abriu sozinho,
 * como o AIPANEL), os das telas do DOS (Ctrl+F1..F12: PONTE, BAT.EXE) e os
 * servicos (DISKSYNC, WSL). Fecha o WIN, solta o WINE.DLL, volta as
 * variaveis de ambiente ao que eram antes do AUTOEXEC.BAT e recomeca a tela
 * 1 como no boot: AUTOEXEC.BAT (que abre o WIN de novo), HTTPD.EXE e o
 * prompt. A memoria dos programas fechados volta ao sistema.
 *
 * Fica o que e do kernel: drivers, discos montados, rede, threads do kernel.
 *
 * Como: com o WIN aberto, o laco dele (visual.c, killq_service) encerra as
 * tarefas e sai; o vt_select da tarefa do WIN que fechou chama
 * vt_limpa_finish (vt.c). Sem o WIN, a tela que pediu chama direto. O
 * vt_limpa_finish larga todas as telas de texto e recomeca a tela 1 em
 * limpa_boot_entry, que encerra os programas delas e roda o boot.
 */
#include "shell.h"
#include "env.h"
#include "mm.h"
#include "module.h"
#include "serial.h"
#include "kthread.h"
#include "visual.h"
#include "vt.h"
#include "wine.h"

void syslog_puts(const char* s);
void syslog_stamp(void);
int file_size(const char* path);
int drives_is_mounted(char letter);
const char* mod_pid_name(uint32_t pid);   /* module.c */

/* ---- variaveis do boot ----
 * Antes do AUTOEXEC.BAT (kernel_boot_programs) a tabela tem so o que o
 * kernel pos. O LIMPA volta a ela antes de rodar o AUTOEXEC de novo: um SET
 * feito depois do boot (WSL=1, FAT32DLL, WINE...) nao sobrevive. */
#define SNAP_N 32

/* Todo o estado numa variavel so, com guardas nas pontas: o fim de um vetor
 * nunca coincide com o comeco de outra variavel, e o TROCA (docs/TROCA.md)
 * resolve todas as referencias deste arquivo sem ambiguidade. */
static struct {
  uint64_t guarda0[4];
  volatile int state;     /* 0 nada; 1 pedido (o WIN vai fechar); 2 o WIN ja encerrou as tarefas */
  uint32_t free_before_mb;
  uint64_t high_before_mb;
  int snap_n;             /* -1: ainda nao guardou */
  char snap_name[SNAP_N][ENV_NAME_MAX];
  char snap_val[SNAP_N][ENV_VAL_MAX];
  uint64_t guarda1[4];
} L = { {0, 0, 0, 0}, 0, 0, 0, -1, {{0}}, {{0}}, {0, 0, 0, 0} };

int limpa_pending(void) { return L.state; }
void limpa_win_closing(void) { L.state = 2; }

static void cp(char* d, const char* s, int max) {
  int i = 0;
  for (; s && s[i] && i < max - 1; i++) d[i] = s[i];
  d[i] = 0;
}

static int ieq(const char* a, const char* b) {
  while (*a && *b) {
    char x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
    a++;
    b++;
  }
  return *a == *b;
}

void limpa_env_snapshot(void) {
  const char* n;
  const char* v;
  int i;
  if (L.snap_n >= 0) return;
  L.snap_n = 0;
  for (i = 0; L.snap_n < SNAP_N && env_at(i, &n, &v); i++) {
    cp(L.snap_name[L.snap_n], n, ENV_NAME_MAX);
    cp(L.snap_val[L.snap_n], v, ENV_VAL_MAX);
    L.snap_n++;
  }
}

static void env_restore(void) {
  char names[SNAP_N][ENV_NAME_MAX];
  const char* n;
  const char* v;
  int i, k, m = 0;
  if (L.snap_n < 0) return;
  for (i = 0; m < SNAP_N && env_at(i, &n, &v); i++) cp(names[m++], n, ENV_NAME_MAX);
  for (i = 0; i < m; i++) {
    for (k = 0; k < L.snap_n && !ieq(names[i], L.snap_name[k]); k++) { }
    if (k == L.snap_n) env_set(names[i], "");   /* veio depois do boot */
  }
  for (k = 0; k < L.snap_n; k++) {
    const char* now = env_get(L.snap_name[k]);
    if (!now || !ieq(now, L.snap_val[k])) env_set(L.snap_name[k], L.snap_val[k]);
  }
}

/* ---- saida ---- */
static void put_n(uint64_t v) {
  char b[24];
  int i = 23;
  b[i] = 0;
  do {
    b[--i] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v && i > 0);
  puts_k(b + i);
}

static void mem_now(uint32_t* low_free_mb, uint64_t* high_free_mb) {
  uint32_t t = 0, u = 0, f = 0, l = 0;
  uint64_t ht = 0, hu = 0, hf = 0, hl = 0;
  um_stats(&t, &u, &f, &l);
  hm_stats(&ht, &hu, &hf, &hl);
  *low_free_mb = f >> 20;
  *high_free_mb = hf >> 20;
}

static void mem_line(const char* tit) {
  uint32_t f;
  uint64_t h;
  mem_now(&f, &h);
  puts_k(tit);
  puts_k("heap alto ");
  put_n(f);
  puts_k(" MB livres, acima de 4 GB ");
  put_n(h);
  puts_k(" MB livres\n");
}

static void fkey(int vt) {
  int n = vt < 6 ? vt + 1 : vt + 2;   /* F7 e o WIN */
  puts_k("Ctrl+F");
  put_n((uint64_t)n);
}

static const char* autoexec_path(void) {
  static char p[] = "D:\\AUTOEXEC.BAT";
  p[0] = drives_is_mounted('D') ? 'D' : 'U';
  return file_size(p) >= 0 ? p : 0;
}

/* /T: o que o LIMPA fecharia. */
static int limpa_show(void) {
  char names[48][32];
  uint32_t pids[48];
  int vt[VT_TASKS];
  uint32_t vp[VT_TASKS];
  int i, n;
  const char* ae = autoexec_path();
  puts_k("LIMPA /T: nada foi fechado. O LIMPA fecharia:\n");
  n = win_task_list(names, pids, 48);
  if (win_open()) {
    puts_k("  WIN (Ctrl+F7): ");
    put_n((uint64_t)(n < 0 ? 0 : n));
    puts_k(" programa(s)");
    for (i = 0; i < n; i++) {
      puts_k(i ? ", " : ": ");
      puts_k(names[i]);
      puts_k(" (");
      put_n(pids[i]);
      puts_k(")");
    }
    puts_k("\n");
  } else {
    puts_k("  WIN: fechado\n");
  }
  n = vt_limpa_list(vt, vp, VT_TASKS);
  for (i = 0; i < n; i++) {
    const char* nm = vp[i] ? mod_pid_name(vp[i]) : 0;
    puts_k("  tela ");
    fkey(vt[i]);
    puts_k(": ");
    if (vp[i]) {
      puts_k(nm ? nm : "?");
      puts_k(" (");
      put_n(vp[i]);
      puts_k(")");
    } else {
      puts_k(vt[i] == 10 ? "log do sistema" : "prompt");
    }
    if (vt[i] == vt_cur_index()) puts_k("  <- esta tela");
    puts_k("\n");
  }
  puts_k("  depois: variaveis do boot, ");
  puts_k(ae ? ae : "sem AUTOEXEC.BAT");
  puts_k(", HTTPD.EXE e o prompt na tela 1\n");
  mem_line("  memoria agora: ");
  return 0;
}

int cmd_limpa(int argc, char** argv) {
  uint32_t f;
  uint64_t h;
  if (argc >= 2 && (ieq(argv[1], "/t") || ieq(argv[1], "-t"))) return limpa_show();
  if (argc >= 2) {
    puts_k("Uso: limpa      fecha todos os programas e roda o boot de novo (AUTOEXEC.BAT)\n");
    puts_k("     limpa /t   so mostra o que seria fechado\n");
    return 1;
  }
  if (L.state) {
    puts_k("LIMPA ja pedido: esperando o WIN fechar.\n");
    return 0;
  }
  mem_now(&f, &h);
  L.free_before_mb = f;
  L.high_before_mb = h;
  syslog_stamp();
  syslog_puts("LIMPA: fechando tudo e rodando o boot de novo\n");
  serial_puts("[limpa] pedido\n");
  L.state = 1;
  if (win_open()) {
    puts_k("LIMPA: fechando os programas e o WIN; o AUTOEXEC.BAT roda de novo na tela 1.\n");
    /* O laco do WIN atende (killq_service). Se ele esta ao fundo (parado),
     * vem para a frente; do Command, de um programa ou da API so volta. */
    if (vt_cur_index() != VT_WIN && !kt_in_thread()) vt_select(VT_WIN);
    return 0;
  }
  if (kt_in_thread()) {
    L.state = 0;
    puts_k("LIMPA: sem o WIN aberto, rode no prompt de uma tela do DOS.\n");
    return 1;
  }
  puts_k("LIMPA: fechando os programas; o AUTOEXEC.BAT roda de novo na tela 1.\n");
  vt_limpa_finish(limpa_boot_entry);
  /* so volta se nao deu (sem memoria para a pilha da tela 1) */
  L.state = 0;
  puts_k("LIMPA: sem memoria para recomecar a tela 1; nada foi fechado.\n");
  return 1;
}

/* A tela 1 nova (pilha nova, vt_limpa_finish). Nao volta. */
void limpa_boot_entry(void) {
  uint32_t f;
  uint64_t h;
  vt_limpa_after();          /* programas das telas antigas e as pilhas delas */
  (void)wine_reset_if_idle();
  mod_mark_boot_reset();     /* o WIN novo marca os programas de inicio de novo */
  env_restore();
  L.state = 0;
  mem_now(&f, &h);
  puts_k("SO DOS - reinicio leve (LIMPA)\n");
  puts_k("Memoria livre: heap alto ");
  put_n(L.free_before_mb);
  puts_k(" -> ");
  put_n(f);
  puts_k(" MB, acima de 4 GB ");
  put_n(L.high_before_mb);
  puts_k(" -> ");
  put_n(h);
  puts_k(" MB\n\n");
  serial_puts("[limpa] telas recomecadas, rodando o boot\n");
  syslog_stamp();
  syslog_puts("LIMPA: programas fechados, rodando o AUTOEXEC.BAT\n");
  kernel_boot_programs(1);
  /* SET de um valor igual tambem religa, mas sem a linha no AUTOEXEC o
   * servico continua desligado: confere como no vt_init. */
  vt_service_check();
  vt_wsl_check();
  syslog_service_check();
  shell_loop();
}
