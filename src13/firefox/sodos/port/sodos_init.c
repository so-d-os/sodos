/* sodos_init.c - inicializacao, log em arquivo e pânico da libsodos.
 *
 * O log fica em <unidade do exe>:\LOG\FIREFOX.LOG. Como o kernel nao tem
 * append, cada descarga le o arquivo, concatena e regrava (k_fload/k_fsave).
 * O buffer e pequeno e as descargas sao espacadas; para o volume de log do
 * Firefox isso sera trocado por um append de verdade no kernel (M1).
 */
#include <so.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sodos.h"
#include "sodos_internal.h"

#define LOG_BUF_SIZE (32 * 1024)
#define LOG_MAX_FILE (512 * 1024)

static int inited;
static int log_on;
static char log_path[64];
static char log_buf[LOG_BUF_SIZE];
static int log_len;
static unsigned log_lines_since_flush;
static uint64_t log_last_flush;

static const char* level_tag(int level) {
  switch (level) {
    case SODOS_LOG_ERROR: return "ERRO";
    case SODOS_LOG_WARN: return "AVISO";
    case SODOS_LOG_INFO: return "INFO";
    default: return "DEPUR";
  }
}

static void timestamp(char* out, int max) {
  time_t t = time(NULL);
  unsigned long s = (unsigned long)(t % 86400L);
  if (t <= 0) s = 0;
  snprintf(out, max, "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

static void log_console(const char* line) {
  /* k_puts e o caminho que funciona no console do SODOS e no soxrun. */
  k_puts(line);
}

void sodos_init(void) {
  char dir[16];
  char drive;
  if (inited) return;
  inited = 1;

  drive = (char)k_exe_drive();
  if (drive < 'A' || drive > 'Z') drive = (char)k_drive();
  if (drive < 'A' || drive > 'Z') drive = 'D';
  snprintf(dir, sizeof(dir), "%c:\\LOG", drive);
  k_mkdir(dir);
  snprintf(log_path, sizeof(log_path), "%s\\FIREFOX.LOG", dir);
  log_on = 1;

  sodos__sched_init();
  log_last_flush = sodos__ms();

  sodos_log(SODOS_LOG_INFO, "=== libsodos %s (%c:\\LOG) ===", SODOS_VERSION, drive);
  sodos_log_flush();
}

const char* sodos_log_path(void) { return log_path; }

void sodos_vlog(int level, const char* fmt, va_list ap) {
  char body[768];
  char line[900];
  char ts[16];

  if (!inited) sodos_init();

  vsnprintf(body, sizeof(body), fmt, ap);
  timestamp(ts, sizeof(ts));
  snprintf(line, sizeof(line), "[%s %s] %s\n", ts, level_tag(level), body);

  if (level <= SODOS_LOG_WARN) log_console(line);

  {
    int n = (int)strlen(line);
    if (n > LOG_BUF_SIZE - log_len - 1) sodos_log_flush();
    if (n > LOG_BUF_SIZE - 1) n = LOG_BUF_SIZE - 1;
    memcpy(log_buf + log_len, line, (size_t)n);
    log_len += n;
    log_buf[log_len] = 0;
  }

  log_lines_since_flush++;
  if (level <= SODOS_LOG_WARN || log_lines_since_flush >= 64 ||
      sodos__ms() - log_last_flush >= 2000) {
    sodos_log_flush();
  }
}

void sodos_log(int level, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  sodos_vlog(level, fmt, ap);
  va_end(ap);
}

void sodos_log_flush(void) {
  int old, total, n;
  char* buf;
  if (!log_on || log_len <= 0) return;

  old = k_fsize(log_path);
  if (old < 0) old = 0;          /* arquivo ainda nao existe */
  if (old > LOG_MAX_FILE) {
    k_fdelete(log_path);
    old = 0;
  }

  total = old + log_len;
  buf = (char*)malloc((size_t)total + 2);
  if (!buf) {
    log_len = 0;
    return;
  }
  n = 0;
  if (old > 0) {
    /* k_fload le no maximo max-1 bytes: pedir old+1 para trazer o ultimo
     * byte (o '\n' final) e nao colar a linha nova na anterior. */
    n = k_fload(log_path, buf, old + 1);
    if (n < 0) n = 0;
    if (n > old) n = old;
  }
  memcpy(buf + n, log_buf, (size_t)log_len);
  if (n + log_len > total) {
    log_len = total - n;
    if (log_len < 0) log_len = 0;
  }
  k_fsave(log_path, buf, n + log_len);
  free(buf);

  log_len = 0;
  log_lines_since_flush = 0;
  log_last_flush = sodos__ms();
}

void sodos_panic(const char* fmt, ...) {
  va_list ap;
  char msg[768];
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  sodos_log(SODOS_LOG_ERROR, "PANICO: %s", msg);
  log_console("PANICO: ");
  log_console(msg);
  log_console("\n");
  sodos_log_flush();
  k_exit(3);
  for (;;) {}
}

void sodos_mem_info(uint32_t* total, uint32_t* freeb) {
  unsigned t = 0, f = 0;
  k_meminfo(&t, &f);
  if (total) *total = t;
  if (freeb) *freeb = f;
}

/* Entrada da DLL SOX: o loader chama com SOX_DLL_ATTACH na carga e
 * SOX_DLL_DETACH ao descarregar. */
int DllMain(int reason) {
  if (reason == 1) sodos_init();
  else if (reason == 0) sodos_log_flush();
  return 1;
}
