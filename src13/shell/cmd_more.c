/* more - mostra texto uma tela por vez: "more arquivo" ou "type arquivo | more".
 * Tambem a saida de texto de type e cat: arquivos UTF-8 (feitos no Linux)
 * viram CP850, a pagina da tela; bytes que nao sao UTF-8 valido ja sao
 * CP850 (digitados no SODOS) e passam como estao. */
#include "shell.h"
#include "drives.h"
#include "reboot.h"

#define PAGE_LINES 24

/* U+00A0..U+00FF -> CP850 */
static const uint8_t latin1_cp850[96] = {
  0xFF, 0xAD, 0xBD, 0x9C, 0xCF, 0xBE, 0xDD, 0xF5, 0xF9, 0xB8, 0xA6, 0xAE,
  0xAA, 0xF0, 0xA9, 0xEE, 0xF8, 0xF1, 0xFD, 0xFC, 0xEF, 0xE6, 0xF4, 0xFA,
  0xF7, 0xFB, 0xA7, 0xAF, 0xAC, 0xAB, 0xF3, 0xA8, 0xB7, 0xB5, 0xB6, 0xC7,
  0x8E, 0x8F, 0x92, 0x80, 0xD4, 0x90, 0xD2, 0xD3, 0xDE, 0xD6, 0xD7, 0xD8,
  0xD1, 0xA5, 0xE3, 0xE0, 0xE2, 0xE5, 0x99, 0x9E, 0x9D, 0xEB, 0xE9, 0xEA,
  0x9A, 0xED, 0xE8, 0xE1, 0x85, 0xA0, 0x83, 0xC6, 0x84, 0x86, 0x91, 0x87,
  0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B, 0xD0, 0xA4, 0x95, 0xA2,
  0x93, 0xE4, 0x94, 0xF6, 0x9B, 0x97, 0xA3, 0x96, 0x81, 0xEC, 0xE7, 0x98,
};

/* Aspas, tracos e molduras comuns; o resto vira '?'. */
static const struct { uint16_t u; uint8_t c; } uni_cp850[] = {
  {0x2018, 0x27}, {0x2019, 0x27}, {0x201C, 0x22}, {0x201D, 0x22}, {0x2013, 0x2D},
  {0x2014, 0x2D}, {0x2022, 0x2A}, {0x2026, 0x2E}, {0x20AC, 0x45}, {0x2500, 0xC4},
  {0x2502, 0xB3}, {0x250C, 0xDA}, {0x2510, 0xBF}, {0x2514, 0xC0}, {0x2518, 0xD9},
  {0x251C, 0xC3}, {0x2524, 0xB4}, {0x252C, 0xC2}, {0x2534, 0xC1}, {0x253C, 0xC5},
  {0x2550, 0xCD}, {0x2551, 0xBA}, {0x2554, 0xC9}, {0x2557, 0xBB}, {0x255A, 0xC8},
  {0x255D, 0xBC}, {0x2560, 0xCC}, {0x2563, 0xB9}, {0x2566, 0xCB}, {0x2569, 0xCA},
  {0x256C, 0xCE}, {0x2580, 0xDF}, {0x2584, 0xDC}, {0x2588, 0xDB}, {0x2591, 0xB0},
  {0x2592, 0xB1}, {0x2593, 0xB2}, {0x25A0, 0xFE}, {0x0131, 0xD5}, {0x0192, 0x9F},
  {0x2017, 0xF2},
};

/* 0 se nao ha equivalente */
static int to_cp850(uint32_t u) {
  unsigned i;
  if (u >= 0xA0 && u <= 0xFF) return latin1_cp850[u - 0xA0];
  for (i = 0; i < sizeof(uni_cp850) / sizeof(uni_cp850[0]); i++)
    if (uni_cp850[i].u == u) return uni_cp850[i].c;
  return 0;
}

static int t_more;     /* pagina? */
static int t_quit;     /* q/ESC/Ctrl+C no prompt */
static int t_col;
static int t_lines;
static uint8_t t_seq[4];
static int t_n;
static int t_need;

static void page_wait(void) {
  static const char prompt[] = "-- Mais --  (espaco: tela, Enter: linha, q: sai)";
  int i;
  int c;
  puts_k(prompt);
  for (;;) {
    c = shell_get_char();
    if (kbd_break_take()) c = 'q';
    if (c == ' ') {
      t_lines = 0;
      break;
    }
    if (c == '\n' || c == '\r') {
      t_lines = PAGE_LINES - 1;
      break;
    }
    if (c == 'q' || c == 'Q' || c == 27) {
      t_quit = 1;
      break;
    }
  }
  putc('\r');
  for (i = 0; prompt[i]; i++) putc(' ');
  putc('\r');
}

static void out_char(uint8_t c) {
  if (t_quit) return;
  if (c == '\t') {
    do out_char(' '); while (t_col % 8);
    return;
  }
  putc((char)c);
  if (c == '\n') {
    t_col = 0;
    t_lines++;
  } else if (c == '\r') {
    t_col = 0;
  } else if (++t_col >= 80) {
    t_col = 0;
    t_lines++;
  }
  if (t_more && t_lines >= PAGE_LINES) page_wait();
}

static void flush_raw(void) {
  int i;
  for (i = 0; i < t_n; i++) out_char(t_seq[i]);
  t_n = 0;
}

static void feed(uint8_t b) {
  if (t_n) {
    if ((b & 0xC0) == 0x80) {
      t_seq[t_n++] = b;
      if (t_n < t_need) return;
      {
        uint32_t u = t_seq[0] & (t_need == 2 ? 0x1F : t_need == 3 ? 0x0F : 0x07);
        int i;
        int c;
        for (i = 1; i < t_n; i++) u = (u << 6) | (t_seq[i] & 0x3F);
        c = to_cp850(u);
        /* Sem equivalente em 2 bytes: era CP850 (ex. ã seguido de ç) */
        if (!c && t_need == 2) {
          flush_raw();
          return;
        }
        t_n = 0;
        out_char(c ? (uint8_t)c : '?');
      }
      return;
    }
    flush_raw();
  }
  if (b >= 0xC2 && b <= 0xF4) {
    t_seq[0] = b;
    t_n = 1;
    t_need = b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    return;
  }
  out_char(b);
}

void text_begin(int more) {
  /* No WIN a saida vai para a janela, que nao tem prompt de pagina. */
  t_more = more && !shell_get_out();
  t_quit = 0;
  t_col = 0;
  t_lines = 0;
  t_n = 0;
  kbd_break_take();
}

int text_put(const char* s, int n) {
  int i;
  for (i = 0; i < n && !t_quit; i++) feed((uint8_t)s[i]);
  return t_quit;
}

void text_end(void) {
  flush_raw();
}

int text_file(const char* path, int more) {
  static char buf[1024];
  uint32_t off = 0;
  int n;
  text_begin(more);
  for (;;) {
    n = file_read_at(path, off, buf, (int)sizeof(buf));
    if (n < 0) {
      if (off) break;
      if (n == -2) {
        puts_k("Unidade nao montada. Use: mount D:\n");
      } else {
        puts_k("File not found: ");
        puts_k(path);
        puts_k("\n");
      }
      return 1;
    }
    if (n == 0) break;
    off += (uint32_t)n;
    if (text_put(buf, n)) return 0;
  }
  text_end();
  return 0;
}

int cmd_more(int argc, char** argv) {
  if (argc >= 2) return text_file(argv[1], 1);
  if (!shell_pipe_active()) {
    puts_k("Uso: more <arquivo>\n");
    puts_k("     type <arquivo> | more\n");
    puts_k("     dir | more\n");
    return 1;
  }
  {
    char c;
    int ch;
    text_begin(1);
    while ((ch = shell_pipe_getc()) >= 0) {
      c = (char)ch;
      if (text_put(&c, 1)) return 0;
    }
    text_end();
  }
  return 0;
}
