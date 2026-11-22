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
  }
}

int cmd_ver(int argc, char** argv) {
  (void)argc;
  (void)argv;
  version_print(1);
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
