#include "shell.h"
#include "disk.h"
#include "irq.h"
#include "serial.h"

/* ---------- quem le e grava o disco ----------
 * disk_read_sectors/disk_write_sectors (kernel.c) chamam dstat_io a cada
 * comando; as entradas do drives.c (file_load, file_save, k_fread, ...) dizem
 * o caminho com dstat_path enquanto seguram a trava do sistema de arquivos.
 * Cada linha da tabela e (programa ou thread do kernel, caminho). DSTAT
 * mostra; DSTAT ZERO limpa. Serve para achar quem acende a luz do disco. */

uint32_t mod_current_pid(void);
const char* mod_pid_name(uint32_t pid);
void* kt_cur_fn(void);

#define DS_N 48
#define DS_PATH 40
struct ds_row {
  uint32_t pid;
  void* fn;
  char who[16];
  char path[DS_PATH];
  uint32_t rd_ops, wr_ops;
  uint32_t rd_sec, wr_sec;
  uint32_t first_ms, last_ms;
  uint8_t drive;
};
static struct ds_row ds[DS_N];
static volatile int ds_lk;
static char ds_cur[DS_PATH];
static uint32_t ds_since;

static void cp(char* d, const char* s, int max) {
  int i = 0;
  if (s)
    for (; s[i] && i < max - 1; i++) d[i] = s[i];
  d[i] = 0;
}
static int eq(const char* a, const char* b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

/* Chamado com a trava do FS (drives.c); 0 limpa. Guarda o fim do caminho. */
void dstat_path(const char* p) {
  int n = 0;
  if (!p) {
    ds_cur[0] = 0;
    return;
  }
  while (p[n]) n++;
  cp(ds_cur, n > DS_PATH - 1 ? p + n - (DS_PATH - 1) : p, DS_PATH);
}

void dstat_io(uint32_t drive, int write, uint32_t count) {
  uint32_t now = (uint32_t)irq_ticks();
  uint32_t pid = mod_current_pid();
  void* fn = kt_cur_fn();
  int i, pick = -1, old = 0;
  uint64_t f;
  if (fn) pid = 0;                     /* thread do kernel: o pid e de quem? */
  __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
  while (__atomic_exchange_n(&ds_lk, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
  if (!ds_since) ds_since = now | 1u;
  for (i = 0; i < DS_N; i++) {
    struct ds_row* r = &ds[i];
    if (!r->first_ms) {
      if (pick < 0) pick = i;
      continue;
    }
    if (r->pid == pid && r->fn == fn && r->drive == drive && eq(r->path, ds_cur)) {
      pick = i;
      break;
    }
    if (ds[old].first_ms && r->last_ms - ds[old].last_ms > 0x80000000u) old = i;
  }
  if (pick < 0 || (i == DS_N && ds[pick].first_ms)) pick = old;
  if (i == DS_N) {                     /* linha nova (ou troca a mais antiga) */
    struct ds_row* r = &ds[pick];
    const char* nm = fn ? "kthread" : (pid ? mod_pid_name(pid) : "kernel");
    r->pid = pid;
    r->fn = fn;
    r->drive = (uint8_t)drive;
    cp(r->who, nm ? nm : "?", (int)sizeof r->who);
    cp(r->path, ds_cur, DS_PATH);
    r->rd_ops = r->wr_ops = r->rd_sec = r->wr_sec = 0;
    r->first_ms = now | 1u;
  }
  if (write) {
    ds[pick].wr_ops++;
    ds[pick].wr_sec += count;
  } else {
    ds[pick].rd_ops++;
    ds[pick].rd_sec += count;
  }
  ds[pick].last_ms = now;
  __atomic_store_n(&ds_lk, 0, __ATOMIC_RELEASE);
  if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
}

static void out(const char* s) {
  puts_k(s);
  serial_puts(s);
}
static void outn(uint32_t v, int w) {
  char num[16];
  int n = 0;
  uint32_to_string(v, num, sizeof(num));
  while (num[n]) n++;
  while (n++ < w) out(" ");
  out(num);
}
static void outhex(uint64_t v) {
  char h[19];
  int i;
  h[0] = '0';
  h[1] = 'x';
  for (i = 0; i < 16; i++) {
    int d = (int)((v >> ((15 - i) * 4)) & 15);
    h[2 + i] = (char)(d > 9 ? 'a' + d - 10 : '0' + d);
  }
  h[18] = 0;
  out(h);
}

static void dstat_table(void) {
  uint32_t now = (uint32_t)irq_ticks();
  int i, any = 0;
  out("[dstat] I/O de disco desde ");
  outn(ds_since ? (now - ds_since) / 1000u : 0, 1);
  out(" s (ops = comandos; KB lidos/gravados; ultimo = s atras)\n");
  out("[dstat] quem            drv  ops-L    KB-L  ops-G    KB-G ultimo caminho\n");
  for (i = 0; i < DS_N; i++) {
    struct ds_row* r = &ds[i];
    int k;
    if (!r->first_ms) continue;
    any = 1;
    out("[dstat] ");
    out(r->who);
    for (k = 0; r->who[k]; k++) { }
    while (k++ < 16) out(" ");
    outn(r->drive, 3);
    outn(r->rd_ops, 7);
    outn(r->rd_sec / 2u, 8);
    outn(r->wr_ops, 7);
    outn(r->wr_sec / 2u, 8);
    outn((now - r->last_ms) / 1000u, 7);
    out(" ");
    out(r->path[0] ? r->path : "-");
    if (r->fn) {
      out(" fn=");
      outhex((uint64_t)(uintptr_t)r->fn);
    }
    out("\n");
  }
  if (!any) out("[dstat] nenhum I/O de disco registrado\n");
}

int cmd_dstat(int argc, char** argv) {
  if (argc > 1 && (eq(argv[1], "zero") || eq(argv[1], "ZERO"))) {
    int i;
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    while (__atomic_exchange_n(&ds_lk, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
    for (i = 0; i < DS_N; i++) ds[i].first_ms = 0;
    ds_since = 0;
    __atomic_store_n(&ds_lk, 0, __ATOMIC_RELEASE);
    if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
    puts_k("dstat: contadores zerados\n");
    return 0;
  }
  dstat_table();
  puts_k("ATA last: drive=");
  char num[16];
  uint32_to_string(last_ata_drive, num, sizeof(num));
  puts_k(num);
  puts_k(" lba=");
  uint32_to_string(last_ata_lba, num, sizeof(num));
  puts_k(num);
  puts_k(" status=0x");
  {
    uint8_t v = last_ata_status;
    char h[3];
    h[0] = (char)((v >> 4) > 9 ? 'A' + (v >> 4) - 10 : '0' + (v >> 4));
    h[1] = (char)((v & 15) > 9 ? 'A' + (v & 15) - 10 : '0' + (v & 15));
    h[2] = '\0';
    puts_k(h);
  }
  puts_k(" error=0x");
  {
    uint8_t v = last_ata_error;
    char h[3];
    h[0] = (char)((v >> 4) > 9 ? 'A' + (v >> 4) - 10 : '0' + (v >> 4));
    h[1] = (char)((v & 15) > 9 ? 'A' + (v & 15) - 10 : '0' + (v & 15));
    h[2] = '\0';
    puts_k(h);
  }
  puts_k("\n");
  return 0;
}
