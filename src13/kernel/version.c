/* version.c - versao do kernel (comando VER, banner, Gerenciador) e o
 * \LOG\BOOT.LOG, que diz a cada boot se o kernel mudou desde o anterior.
 *
 * A string vem de kernel/version.h, gerado por tools/mkversion.sh em todo
 * build do kernel (o numero de build sempre sobe). Assim da para ver no PC
 * real se o KERNEL.ELF que o GRUB carregou e mesmo o ultimo enviado.
 */
#include <stdint.h>
#include "version.h"
#include "shell.h"
#include "serial.h"
#include "smp.h"
#include "kexports.h"
#include "drives.h"
#include "vt.h"
#include "irq.h"

const char* sodos_version(void) { return SODOS_VERSION; }
int sodos_build(void) { return SODOS_VER_BUILD; }

/* Onde ficou o log deste boot ("D:\LOG\BOOT.LOG"); vazio se nao gravou. */
static char log_path[32];
/* Mensagem do boot: "kernel ATUALIZADO (build 12 -> 13)" ou "mesmo kernel". */
static char boot_note[96];

static int slen(const char* s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static void cat(char* d, int max, const char* s) {
  int n = slen(d);
  while (*s && n < max - 1) d[n++] = *s++;
  d[n] = 0;
}

static void cat_u(char* d, int max, unsigned v, int digits) {
  char b[12];
  int i = 0;
  do {
    b[i++] = (char)('0' + v % 10);
    v /= 10;
  } while (v && i < 11);
  while (i < digits && i < 11) b[i++] = '0';
  {
    char r[12];
    int j = 0;
    while (i > 0) r[j++] = b[--i];
    r[j] = 0;
    cat(d, max, r);
  }
}

static void cat_hex64(char* d, int max, uint64_t v) {
  static const char hx[] = "0123456789ABCDEF";
  char b[19];
  int i;
  b[0] = '0';
  b[1] = 'x';
  for (i = 0; i < 16; i++) b[2 + i] = hx[(v >> (60 - 4 * i)) & 15u];
  b[18] = 0;
  cat(d, max, b);
}

/* "1996,312 MHz" */
static void cat_mhz(char* d, int max, uint64_t hz) {
  unsigned khz = (unsigned)(hz / 1000u);
  cat_u(d, max, khz / 1000u, 1);
  cat(d, max, ",");
  cat_u(d, max, khz % 1000u, 3);
  cat(d, max, " MHz");
}

/* "build N" da ultima linha de boot do log (pula as linhas de reboot);
 * -1 se nao achar. */
static int last_build_line(const char* buf, int st, int end);
static int last_build(const char* buf, int len) {
  int end = len, st, v;
  for (;;) {
    while (end > 0 && (buf[end - 1] == '\n' || buf[end - 1] == '\r')) end--;
    if (end <= 0) return -1;
    st = end;
    while (st > 0 && buf[st - 1] != '\n') st--;
    v = last_build_line(buf, st, end);
    if (v >= 0) return v;
    end = st;
  }
}
static int last_build_line(const char* buf, int st, int end) {
  int i;
  for (i = st; i + 6 < end; i++) {
    if (buf[i] == 'b' && buf[i + 1] == 'u' && buf[i + 2] == 'i' && buf[i + 3] == 'l' &&
        buf[i + 4] == 'd' && buf[i + 5] == ' ') {
      int v = 0, k = i + 6, n = 0;
      while (k < end && buf[k] >= '0' && buf[k] <= '9') {
        v = v * 10 + (buf[k] - '0');
        k++;
        n++;
      }
      if (n) return v;
    }
  }
  return -1;
}

#define LOG_MAX 12288
static char logbuf[LOG_MAX + 256];

static int try_log(char letter, const char* line) {
  char dir[8] = "X:\\LOG";
  char path[32];
  int n, ll = slen(line), prev;
  if (!drives_is_mounted(letter)) return -1;
  dir[0] = letter;
  drives_mkdir(dir); /* 0 criou, 1 ja existia; outro erro aparece no save */
  path[0] = 0;
  cat(path, (int)sizeof path, dir);
  cat(path, (int)sizeof path, "\\BOOT.LOG");
  n = file_load(path, logbuf, LOG_MAX);
  if (n < 0) n = 0;
  prev = last_build(logbuf, n);
  /* Mantem so o fim: corta linhas velhas ate caber a nova. */
  if (n + ll > LOG_MAX) {
    int cut = n + ll - LOG_MAX, i;
    while (cut < n && logbuf[cut - 1] != '\n') cut++;
    for (i = cut; i < n; i++) logbuf[i - cut] = logbuf[i];
    n -= cut;
  }
  {
    int i;
    for (i = 0; i < ll; i++) logbuf[n + i] = line[i];
  }
  if (file_save(path, logbuf, n + ll) != 0) return -1;
  log_path[0] = 0;
  cat(log_path, (int)sizeof log_path, path);

  boot_note[0] = 0;
  if (prev < 0) {
    cat(boot_note, (int)sizeof boot_note, "primeiro boot registrado neste disco");
  } else if (prev == SODOS_VER_BUILD) {
    cat(boot_note, (int)sizeof boot_note, "MESMO kernel do boot anterior (build ");
    cat_u(boot_note, (int)sizeof boot_note, (unsigned)prev, 1);
    cat(boot_note, (int)sizeof boot_note, ")");
  } else {
    cat(boot_note, (int)sizeof boot_note, "kernel ATUALIZADO: build ");
    cat_u(boot_note, (int)sizeof boot_note, (unsigned)prev, 1);
    cat(boot_note, (int)sizeof boot_note, " -> ");
    cat_u(boot_note, (int)sizeof boot_note, (unsigned)SODOS_VER_BUILD, 1);
  }
  return 0;
}

/* Uma linha por boot: data/hora (RTC, UTC), versao e as CPUs ligadas. */
void version_boot_log(void) {
  static const char order[] = {'D', 'U', 'Z'};
  char line[200];
  int d = rtc_date(), t = rtc_time();
  int ncpu = smp_cpu_count(), on = 0, i;
  struct k_cpu_stat st;
  for (i = 0; i < ncpu; i++)
    if (smp_cpu_stat(i, &st) == 0 && st.online) on++;

  line[0] = 0;
  if (d > 0) {
    cat_u(line, (int)sizeof line, (unsigned)(d / 10000), 4);
    cat(line, (int)sizeof line, "-");
    cat_u(line, (int)sizeof line, (unsigned)(d / 100 % 100), 2);
    cat(line, (int)sizeof line, "-");
    cat_u(line, (int)sizeof line, (unsigned)(d % 100), 2);
  } else {
    cat(line, (int)sizeof line, "sem data RTC");
  }
  cat(line, (int)sizeof line, " ");
  if (t >= 0) {
    cat_u(line, (int)sizeof line, (unsigned)(t / 10000), 2);
    cat(line, (int)sizeof line, ":");
    cat_u(line, (int)sizeof line, (unsigned)(t / 100 % 100), 2);
    cat(line, (int)sizeof line, ":");
    cat_u(line, (int)sizeof line, (unsigned)(t % 100), 2);
  }
  cat(line, (int)sizeof line, " UTC  ");
  cat(line, (int)sizeof line, SODOS_VERSION);
  cat(line, (int)sizeof line, "  cpus ");
  cat_u(line, (int)sizeof line, (unsigned)on, 1);
  cat(line, (int)sizeof line, "/");
  cat_u(line, (int)sizeof line, (unsigned)ncpu, 1);
  {
    struct irq_clock ck;
    irq_clock_info(&ck);
    if (ck.tsc) {
      cat(line, (int)sizeof line, "  relogio tsc ");
      cat_u(line, (int)sizeof line, (unsigned)(ck.hz / 1000u), 1);
      cat(line, (int)sizeof line, " kHz");
    } else {
      cat(line, (int)sizeof line, "  relogio irq0");
    }
  }
  cat(line, (int)sizeof line, "\r\n");

  serial_puts("[ver] ");
  serial_puts(line);
  for (i = 0; i < (int)sizeof order; i++)
    if (try_log(order[i], line) == 0) break;
  if (!log_path[0]) {
    boot_note[0] = 0;
    cat(boot_note, (int)sizeof boot_note, "BOOT.LOG nao gravado (sem D:, U: ou Z:)");
  }
  serial_puts("[ver] ");
  serial_puts(boot_note);
  serial_puts("\n");
}

/* Banner antes do primeiro prompt e saida do VER. */
void version_print(int full) {
  puts_k(SODOS_VERSION);
  puts_k("\n");
  if (boot_note[0]) {
    puts_k("  ");
    puts_k(boot_note);
    if (log_path[0]) {
      puts_k("  [");
      puts_k(log_path);
      puts_k("]");
    }
    puts_k("\n");
  }
  if (full) {
    int ncpu = smp_cpu_count(), on = 0, i;
    struct k_cpu_stat st;
    for (i = 0; i < ncpu; i++)
      if (smp_cpu_stat(i, &st) == 0 && st.online) on++;
    puts_k("  CPUs online: ");
    {
      char b[16];
      b[0] = 0;
      cat_u(b, (int)sizeof b, (unsigned)on, 1);
      cat(b, (int)sizeof b, "/");
      cat_u(b, (int)sizeof b, (unsigned)ncpu, 1);
      puts_k(b);
    }
    puts_k(" (SMP; 'smp' mostra o uso de cada uma)\n");
    {
      struct irq_clock ck;
      char b[120];
      irq_clock_info(&ck);
      b[0] = 0;
      if (ck.tsc) {
        cat(b, (int)sizeof b, "  Relogio: TSC ");
        cat_mhz(b, (int)sizeof b, ck.hz);
        cat(b, (int)sizeof b, ", ");
        cat_u(b, (int)sizeof b, (unsigned)ck.lost, 1);
        cat(b, (int)sizeof b, " ms recuperados ('relogio' detalha)\n");
      } else {
        cat(b, (int)sizeof b, "  Relogio: contagem do IRQ0 (");
        cat(b, (int)sizeof b, ck.why);
        cat(b, (int)sizeof b, ")\n");
      }
      puts_k(b);
    }
  }
}

int cmd_ver(int argc, char** argv) {
  (void)argc;
  (void)argv;
  version_print(1);
  return 0;
}

/* RELOGIO: de onde vem o k_ms (irq.c) e quantos ticks o TSC recuperou.
 *   RELOGIO            estado
 *   RELOGIO TSC        acerta pelo TSC (calibra se preciso; vale ate numa VM)
 *   RELOGIO IRQ0       volta a so contar o IRQ0 (como antes)
 *   RELOGIO TESTE [ms] segura o IRQ0 (cli) por ms e mostra quanto o k_ms andou */
static int str_ieq(const char* a, const char* b) {
  while (*a && *b) {
    char x = *a++, y = *b++;
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
  }
  return *a == *b;
}

static void clock_ago(char* b, int max, uint64_t now, uint64_t at) {
  cat(b, max, "ha ");
  cat_u(b, max, (unsigned)((now - at) / 1000u), 1);
  cat(b, max, " s");
}

int cmd_relogio(int argc, char** argv) {
  struct irq_clock ck;
  char b[160];
  if (argc > 1 && str_ieq(argv[1], "tsc")) {
    if (irq_clock_set(1) != 0) {
      puts_k("RELOGIO: nao deu para calibrar o TSC contra o PIT\n");
      return 1;
    }
  } else if (argc > 1 && str_ieq(argv[1], "irq0")) {
    irq_clock_set(0);
  } else if (argc > 1 && str_ieq(argv[1], "teste")) {
    uint32_t ms = 200;
    uint64_t got;
    if (argc > 2) {
      const char* p = argv[2];
      ms = 0;
      while (*p >= '0' && *p <= '9') ms = ms * 10u + (uint32_t)(*p++ - '0');
      if (ms < 2) ms = 2;
      if (ms > 2000) ms = 2000;
    }
    if (irq_clock_test(ms, &got) != 0) {
      puts_k("RELOGIO TESTE: TSC nao calibrado\n");
      return 1;
    }
    b[0] = 0;
    cat(b, (int)sizeof b, "IRQ0 segurado ");
    cat_u(b, (int)sizeof b, ms, 1);
    cat(b, (int)sizeof b, " ms (cli): o relogio andou ");
    cat_u(b, (int)sizeof b, (unsigned)got, 1);
    cat(b, (int)sizeof b, " ms");
    cat(b, (int)sizeof b, got + 2u >= ms ? " (ok)\n" : " (ticks perdidos)\n");
    puts_k(b);
    return 0;
  } else if (argc > 1) {
    puts_k("Uso: RELOGIO [TSC | IRQ0 | TESTE [ms]]\n");
    return 1;
  }
  irq_clock_info(&ck);
  b[0] = 0;
  cat(b, (int)sizeof b, "Relogio (k_ms): ");
  if (ck.tsc) {
    cat(b, (int)sizeof b, "IRQ0 acertado pelo TSC, ");
    cat_mhz(b, (int)sizeof b, ck.hz);
    cat(b, (int)sizeof b, " (desvio da calibracao ");
    cat_u(b, (int)sizeof b, ck.spread_ppm, 1);
    cat(b, (int)sizeof b, " ppm; ");
    cat(b, (int)sizeof b, ck.why);
    cat(b, (int)sizeof b, ")\n");
  } else {
    cat(b, (int)sizeof b, "so a contagem do IRQ0 (");
    cat(b, (int)sizeof b, ck.why);
    cat(b, (int)sizeof b, ")\n");
  }
  puts_k(b);
  if (!ck.tsc) {
    b[0] = 0;
    cat(b, (int)sizeof b, ck.hz ? "  TSC calibrado; 'relogio tsc' liga o acerto. No ar ha "
                                : "  'relogio tsc' calibra o TSC e liga o acerto. No ar ha ");
    cat_u(b, (int)sizeof b, (unsigned)(ck.now / 1000u), 1);
    cat(b, (int)sizeof b, ",");
    cat_u(b, (int)sizeof b, (unsigned)(ck.now % 1000u), 3);
    cat(b, (int)sizeof b, " s\n");
    puts_k(b);
    if (!ck.lost) return 0;
  }
  b[0] = 0;
  cat(b, (int)sizeof b, "  recuperados ");
  cat_u(b, (int)sizeof b, (unsigned)ck.lost, 1);
  cat(b, (int)sizeof b, " ms em ");
  cat_u(b, (int)sizeof b, (unsigned)(ck.now / 1000u), 1);
  cat(b, (int)sizeof b, ",");
  cat_u(b, (int)sizeof b, (unsigned)(ck.now % 1000u), 3);
  cat(b, (int)sizeof b, " s; IRQ0 parado >= 2 ms: ");
  cat_u(b, (int)sizeof b, ck.stalls, 1);
  cat(b, (int)sizeof b, " vezes");
  if (ck.stalls) {
    cat(b, (int)sizeof b, ", a ultima ");
    clock_ago(b, (int)sizeof b, ck.now, ck.last_at);
  }
  cat(b, (int)sizeof b, "\n");
  puts_k(b);
  if (ck.max_ms) {
    b[0] = 0;
    cat(b, (int)sizeof b, "  maior parada: ");
    cat_u(b, (int)sizeof b, ck.max_ms + 1u, 1);
    cat(b, (int)sizeof b, " ms, ");
    clock_ago(b, (int)sizeof b, ck.now, ck.max_at);
    cat(b, (int)sizeof b, " (rip ");
    cat_hex64(b, (int)sizeof b, ck.max_rip);
    cat(b, (int)sizeof b, "); ticks segurados (PIT adiantado): ");
    cat_u(b, (int)sizeof b, (unsigned)ck.held, 1);
    cat(b, (int)sizeof b, "\n");
    puts_k(b);
  }
  if (ck.nlog) {
    uint32_t i;
    puts_k("  ultimas paradas >= 5 ms (o rip e onde o IRQ0 voltou):\n");
    for (i = 0; i < ck.nlog; i++) {
      b[0] = 0;
      cat(b, (int)sizeof b, "    ");
      cat_u(b, (int)sizeof b, ck.log[i].ms, 1);
      cat(b, (int)sizeof b, " ms  ");
      clock_ago(b, (int)sizeof b, ck.now, ck.log[i].at);
      cat(b, (int)sizeof b, "  rip ");
      cat_hex64(b, (int)sizeof b, ck.log[i].rip);
      cat(b, (int)sizeof b, "\n");
      puts_k(b);
    }
  }
  return 0;
}

/* REBOOT [motivo]: registra no BOOT.LOG, no TEMP.LOG e no SERIAL.LOG e
 * reinicia como o Ctrl+Alt+Del (sys_reboot). Assim, num desligamento sem
 * explicacao, da para ver se o boot anterior terminou por este comando. */
#include "reboot.h"
uint32_t net_ms(void);
int serial_log_save(void);
void thermal_note(const char* line);
void wine_dump_waits(void);
void puts_k(const char* s);

int cmd_reboot(int argc, char** argv) {
  static const char order[] = {'D', 'U', 'Z'};
  char line[200];
  int d = rtc_date(), t = rtc_time(), i;
  uint32_t up = net_ms() / 1000u;
  line[0] = 0;
  if (d > 0) {
    cat_u(line, (int)sizeof line, (unsigned)(d / 10000), 4);
    cat(line, (int)sizeof line, "-");
    cat_u(line, (int)sizeof line, (unsigned)(d / 100 % 100), 2);
    cat(line, (int)sizeof line, "-");
    cat_u(line, (int)sizeof line, (unsigned)(d % 100), 2);
  } else {
    cat(line, (int)sizeof line, "sem data RTC");
  }
  cat(line, (int)sizeof line, " ");
  if (t >= 0) {
    cat_u(line, (int)sizeof line, (unsigned)(t / 10000), 2);
    cat(line, (int)sizeof line, ":");
    cat_u(line, (int)sizeof line, (unsigned)(t / 100 % 100), 2);
    cat(line, (int)sizeof line, ":");
    cat_u(line, (int)sizeof line, (unsigned)(t % 100), 2);
  }
  cat(line, (int)sizeof line, " UTC  REBOOT pelo comando, no ar ha ");
  cat_u(line, (int)sizeof line, up / 60u, 1);
  cat(line, (int)sizeof line, " min ");
  cat_u(line, (int)sizeof line, up % 60u, 2);
  cat(line, (int)sizeof line, " s");
  if (argc > 1) {
    cat(line, (int)sizeof line, ":");
    for (i = 1; i < argc; i++) {
      cat(line, (int)sizeof line, " ");
      cat(line, (int)sizeof line, argv[i]);
    }
  }
  cat(line, (int)sizeof line, "\r\n");
  puts_k("Registrando e reiniciando...\n");
  serial_puts("[reboot] ");
  serial_puts(line);
  for (i = 0; i < (int)sizeof order; i++)
    if (try_log(order[i], line) == 0) break;
  thermal_note(line + 11);              /* sem a data: o TEMP.LOG so tem hora */
  wine_dump_waits();                    /* [wine-dump]: esperas do Claude etc. */
  syslog_stamp();
  {
    const char* r = line;                /* so "REBOOT ...": o log ja poe a hora */
    while (*r && !(r[0] == 'R' && r[1] == 'E' && r[2] == 'B')) r++;
    syslog_puts(*r ? r : line);
  }
  syslog_save_now();                    /* SET SYSTEMLOG=1: CONSOLE.LOG ja */
  (void)serial_log_save();
  sys_reboot();
}
