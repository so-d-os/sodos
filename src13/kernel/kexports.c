/* kexports.c - Funcoes do kernel que .EXE/.DLL importam como "KERNEL".
 * Cada linha EXP(nome) vira uma linha de c_compiler/lib/KERNEL.IMP (Makefile),
 * que o link usa para saber o que o kernel oferece. Convencao: cdecl. */
#include <stdint.h>
#include "shell.h"
#include "drives.h"
#include "mm.h"
#include "net.h"
#include "module.h"
#include "kexports.h"
#include "visual.h"

void mod_exit(int code);
void shell_read_line(char* buf, int max);

static void (*con_hook)(char c);

void kexp_set_console_hook(void (*hook)(char c)) { con_hook = hook; }

/* ---------- console ---------- */
static void k_putc(int c) {
  if (con_hook) con_hook((char)c);
  else putc((char)c);
}

static void k_write(const char* s, int n) {
  int i;
  if (!s) return;
  for (i = 0; i < n; i++) k_putc(s[i]);
}

static void k_puts(const char* s) {
  if (!s) return;
  while (*s) k_putc(*s++);
}

static int k_getc(void) {
  int c;
  do {
    c = shell_get_char();
  } while (c == 0);
  return c;
}

static int k_readline(char* buf, int max) {
  int n = 0;
  if (!buf || max < 2) return -1;
  shell_read_line(buf, max);
  while (buf[n]) n++;
  return n;
}

static void k_clear(void) { clear_screen(); }

/* ---------- arquivos ---------- */
static int k_fsize(const char* path) { return file_size(path); }
static int k_fload(const char* path, char* buf, int max) { return file_load(path, buf, max); }
static int k_fread(const char* path, uint32_t offset, char* buf, int len) {
  return file_read_at(path, offset, buf, len);
}
static int k_fsave(const char* path, const char* data, int size) { return file_save(path, data, size); }
static int k_drive(void) { return drives_current(); }
static int k_set_drive(int letter) { return drives_set_current((char)letter); }

/* Nomes da unidade separados por '\n'. Devolve quantos. */
static int k_flist(int letter, char* out, int max) {
  char names[64][32];
  int n = drives_names((char)letter, names, 64);
  int pos = 0;
  int i;
  if (!out || max < 1) return n;
  for (i = 0; i < n; i++) {
    int k = 0;
    while (names[i][k] && pos < max - 2) out[pos++] = names[i][k++];
    if (pos < max - 1) out[pos++] = '\n';
  }
  out[pos] = '\0';
  return n;
}

/* ---------- memoria ---------- */
static void* k_malloc(uint32_t n) { return umalloc(n); }
static void k_free(void* p) { ufree(p); }
static void* k_realloc(void* p, uint32_t n) { return urealloc(p, n); }
static uint32_t k_msize(void* p) { return usize(p); }

static void k_meminfo(uint32_t* total, uint32_t* freeb) {
  um_stats(total, 0, freeb, 0);
}

/* ---------- processos e DLL ---------- */
static void k_exit(int code) { mod_exit(code); }
static int k_exec(const char* path, int argc, char** argv) {
  char full[64];
  char* wide[32];
  uint32_t* narrow = (uint32_t*)argv;
  int n, i;
  if (mod_find_exe(path, full, sizeof(full)) != 0) return MOD_ERR_NOTFOUND;
  /* O programa chamador guarda cada ponteiro em 4 bytes. */
  n = argc;
  if (n < 0) n = 0;
  if (n > 31) n = 31;
  for (i = 0; i < n; i++) {
    uint32_t p = narrow ? narrow[i] : 0;
    wide[i] = p ? (char*)(uintptr_t)p : "";
  }
  wide[n] = 0;
  return mod_exec(full, n, wide);
}
static const char* k_last_error(void) { return mod_last_error(); }
static int k_dll_load(const char* name) { return mod_dll_load(name); }
static void* k_dll_sym(int h, const char* name) { return mod_dll_sym(h, name); }
static void k_dll_free(int h) { mod_dll_free(h); }

/* ---------- tempo e aleatoriedade ---------- */
static uint32_t rdtsc_lo(uint32_t* hi) {
  uint32_t lo, h;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(h));
  if (hi) *hi = h;
  return lo;
}

static uint32_t k_rdtsc(uint32_t* hi) { return rdtsc_lo(hi); }
static uint32_t k_ms(void) { return net_ms(); }

static int cmos_in(int reg) {
  shell_outb(0x70, (uint8_t)reg);
  return shell_inb(0x71);
}

static int bcd_byte(int v) { return (v & 15) + ((v >> 4) & 15) * 10; }

/* AAAAMMDD. O seculo vem do registrador 0x32; sem ele, assume 2000. */
static int k_date(void) {
  int century, year, month, day, binary, guard = 0;
  while ((cmos_in(0x0A) & 0x80) && guard++ < 1000) {}
  binary = cmos_in(0x0B) & 0x04;
  day = cmos_in(0x07);
  month = cmos_in(0x08);
  year = cmos_in(0x09);
  century = cmos_in(0x32);
  if (!binary) {
    day = bcd_byte(day);
    month = bcd_byte(month);
    year = bcd_byte(year);
    century = bcd_byte(century);
  }
  if (century < 19 || century > 22) century = 20;
  if (month < 1 || month > 12 || day < 1 || day > 31) return 0;
  return (century * 100 + year) * 10000 + month * 100 + day;
}

/* Junta o TSC com a fase do PIT; o tempo entre teclas e pacotes varia de verdade. */
static uint32_t mix_state = 0x9E3779B9u;
static void k_random(uint8_t* out, int n) {
  int i;
  for (i = 0; i < n; i++) {
    uint32_t hi;
    uint32_t lo = rdtsc_lo(&hi);
    uint32_t pit;
    int k;
    shell_outb(0x43, 0x00);
    pit = shell_inb(0x40);
    pit |= (uint32_t)shell_inb(0x40) << 8;
    mix_state ^= lo + (hi << 7) + (pit << 16);
    for (k = 0; k < 4; k++) {
      mix_state ^= mix_state << 13;
      mix_state ^= mix_state >> 17;
      mix_state ^= mix_state << 5;
      mix_state += 0x6D2B79F5u;
    }
    out[i] = (uint8_t)(mix_state >> 24);
  }
}

/* ---------- rede ---------- */
static int k_net_ready(void) { return net_ready(); }
static int k_dns(const char* host, uint32_t* ip) { return net_resolve(host, ip); }
static int k_tcp_connect(const char* host, int port) { return net_tcp_connect(host, port); }
static int k_tcp_send(const void* d, int n) { return net_tcp_send(d, n); }
static int k_tcp_recv(void* d, int cap) { return net_tcp_recv(d, cap); }
static void k_tcp_close(void) { net_tcp_close(); }

/* ---------- janela canvas do WIN (visual.c) ---------- */
static int k_gui_open(const char* title) { return gui_open(title); }
static void k_gui_close(void) { gui_close(); }
static void k_gui_title(const char* title) { gui_title(title); }
static void k_gui_size(int* w, int* h) { gui_size(w, h); }
static void k_gui_font(int* cw, int* ch) { gui_font(cw, ch); }
static void k_gui_fill(int x, int y, int w, int h, uint32_t rgb) { gui_fill(x, y, w, h, rgb); }
static void k_gui_text(int x, int y, const char* s, int n, uint32_t fg, uint32_t bg, int flags) {
  gui_text(x, y, s, n, fg, bg, flags);
}
static void k_gui_present(void) { gui_present(); }
static int k_gui_event(int* ev, int timeout_ms) { return gui_event(ev, timeout_ms); }
static void k_gui_pixels(int x, int y, int w, int h, const void* rgba, int stride) {
  gui_pixels(x, y, w, h, rgba, stride);
}

struct kexp {
  const char* name;
  void* fn;
};

#define EXP(n) { #n, (void*)n }

static const struct kexp table[] = {
  EXP(k_putc),
  EXP(k_write),
  EXP(k_puts),
  EXP(k_getc),
  EXP(k_readline),
  EXP(k_clear),
  EXP(k_fsize),
  EXP(k_fload),
  EXP(k_fread),
  EXP(k_fsave),
  EXP(k_flist),
  EXP(k_drive),
  EXP(k_set_drive),
  EXP(k_malloc),
  EXP(k_free),
  EXP(k_realloc),
  EXP(k_msize),
  EXP(k_meminfo),
  EXP(k_exit),
  EXP(k_exec),
  EXP(k_last_error),
  EXP(k_dll_load),
  EXP(k_dll_sym),
  EXP(k_dll_free),
  EXP(k_rdtsc),
  EXP(k_ms),
  EXP(k_date),
  EXP(k_random),
  EXP(k_net_ready),
  EXP(k_dns),
  EXP(k_tcp_connect),
  EXP(k_tcp_send),
  EXP(k_tcp_recv),
  EXP(k_tcp_close),
  EXP(k_gui_open),
  EXP(k_gui_close),
  EXP(k_gui_title),
  EXP(k_gui_size),
  EXP(k_gui_font),
  EXP(k_gui_fill),
  EXP(k_gui_text),
  EXP(k_gui_present),
  EXP(k_gui_event),
  EXP(k_gui_pixels),
};

void* kexport_find(const char* name) {
  unsigned i;
  for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    const char* a = table[i].name;
    const char* b = name;
    while (*a && *a == *b) {
      a++;
      b++;
    }
    if (*a == *b) return table[i].fn;
  }
  return 0;
}
