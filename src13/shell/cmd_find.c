/* find e grep: o FIND do MS-DOS.
 *   FIND [/V] [/C] [/N] [/I] "texto" [[unidade:][caminho]arquivo ...]
 * Sem arquivo le o pipe (dir | find "EXE"). Arquivos aceitam * e ?.
 * grep e o mesmo e tambem aceita -v -c -n -i.
 * Codigo de saida: 0 achou, 1 nao achou, 2 erro. */
#include "shell.h"
#include "drives.h"
#include "reboot.h"

#define FIND_LINE 4096

static int f_v, f_c, f_n, f_i;
static const char* f_str;
static int f_len;
static int f_found;
static int f_err;
static int f_stop;
static const char* f_cmd;   /* FIND ou GREP, nas mensagens */

static char f_line[FIND_LINE];
static char rd_buf[1024];
static const char* rd_path;  /* 0 = pipe */
static uint32_t rd_off;
static int rd_n;
static int rd_pos;

static int rd_getc(void) {
  if (!rd_path) return shell_pipe_getc();
  if (rd_pos >= rd_n) {
    rd_n = file_read_at(rd_path, rd_off, rd_buf, (int)sizeof(rd_buf));
    if (rd_n <= 0) return -1;
    rd_off += (uint32_t)rd_n;
    rd_pos = 0;
  }
  return (unsigned char)rd_buf[rd_pos++];
}

/* Proxima linha, sem \r\n, cortada em FIND_LINE-1. Tamanho; -1 no fim. */
static int rd_line(void) {
  int c = rd_getc();
  int n = 0;
  if (c < 0) return -1;
  while (c >= 0 && c != '\n') {
    if (n < FIND_LINE - 1) f_line[n++] = (char)c;
    c = rd_getc();
  }
  if (n && f_line[n - 1] == '\r') n--;
  f_line[n] = 0;
  return n;
}

static int up(int c) {
  return c >= 'a' && c <= 'z' ? c - 32 : c;
}

/* Texto vazio nao casa com nada: find /v /c "" conta as linhas. */
static int has_str(const char* s, int n) {
  int i, k;
  for (i = 0; i + f_len <= n && f_len; i++) {
    for (k = 0; k < f_len; k++) {
      int a = (unsigned char)s[i + k], b = (unsigned char)f_str[k];
      if (f_i ? up(a) != up(b) : a != b) break;
    }
    if (k == f_len) return 1;
  }
  return 0;
}

static void put_num(uint32_t v) {
  char t[12];
  uint32_to_string(v, t, (int)sizeof(t));
  puts_k(t);
}

static void put_upper(const char* s) {
  for (; *s; s++) putc((char)up((unsigned char)*s));
}

/* Uma entrada; shown nulo e o pipe (sem cabecalho). */
static void find_src(const char* shown) {
  uint32_t count = 0, num = 0;
  int n;
  if (shown && !f_c) {
    puts_k("\n---------- ");
    put_upper(shown);
    puts_k("\n");
  }
  text_begin(0);
  while ((n = rd_line()) >= 0) {
    num++;
    if (has_str(f_line, n) == f_v) continue;
    count++;
    if (!f_c) {
      if (f_n) {
        putc('[');
        put_num(num);
        putc(']');
      }
      text_put(f_line, n);
      text_put("\n", 1);
    }
    if (kbd_break_take()) {
      f_stop = 1;
      break;
    }
  }
  text_end();
  if (f_c) {
    if (shown) {
      puts_k("\n---------- ");
      put_upper(shown);
      puts_k(": ");
    }
    put_num(count);
    puts_k("\n");
  }
  if (count) f_found = 1;
}

static int find_one(const char* path, const char* shown, void* ctx) {
  (void)ctx;
  if (file_size(path) < 0) {
    puts_k("File not found - ");
    put_upper(shown);
    puts_k("\n");
    f_err = 1;
    return 0;
  }
  rd_path = path;
  rd_off = 0;
  rd_n = 0;
  rd_pos = 0;
  find_src(shown);
  return f_stop;
}

static void find_help(void) {
  puts_k("Procura um texto em arquivos ou na saida de outro comando.\n\n");
  put_upper(f_cmd);
  puts_k(" [/V] [/C] [/N] [/I] \"texto\" [[unidade:][caminho]arquivo ...]\n\n");
  puts_k("  /V  Mostra as linhas que NAO tem o texto.\n");
  puts_k("  /C  Mostra so quantas linhas tem o texto.\n");
  puts_k("  /N  Mostra o numero de cada linha.\n");
  puts_k("  /I  Nao diferencia maiusculas de minusculas.\n");
  puts_k("  \"texto\"  O texto procurado.\n");
  puts_k("  arquivo  Arquivo(s) onde procurar; aceita * e ?.\n\n");
  puts_k("Sem arquivo le o pipe:  dir | ");
  puts_k(f_cmd);
  puts_k(" \"EXE\"\n");
}

/* /V, /C, /N, /I, /OFF[LINE] e /? (juntos como /V/C tambem). 0 se nao e
 * opcao, 1 ok, -1 invalida. grep aceita -vcni. */
static int find_switch(const char* a, int dash) {
  const char* p = a;
  if (dash && a[0] == '-' && a[1]) {
    for (p = a + 1; *p; p++)
      if (up(*p) != 'V' && up(*p) != 'C' && up(*p) != 'N' && up(*p) != 'I') return 0;
    for (p = a + 1; *p; p++) {
      if (up(*p) == 'V') f_v = 1;
      if (up(*p) == 'C') f_c = 1;
      if (up(*p) == 'N') f_n = 1;
      if (up(*p) == 'I') f_i = 1;
    }
    return 1;
  }
  if (a[0] != '/' || !a[1]) return 0;
  while (*p == '/') {
    p++;
    switch (up(*p)) {
    case 'V': f_v = 1; p++; break;
    case 'C': f_c = 1; p++; break;
    case 'N': f_n = 1; p++; break;
    case 'I': f_i = 1; p++; break;
    case '?': return 2;
    case 'O':
      if (up(p[1]) == 'F' && up(p[2]) == 'F') {
        p += 3;
        if (up(p[0]) == 'L' && up(p[1]) == 'I' && up(p[2]) == 'N' && up(p[3]) == 'E') p += 4;
        break;
      }
      return -1;
    default:
      return -1;
    }
  }
  return *p ? -1 : 1;
}

static int find_main(int argc, char** argv, const char* cmd, int dash) {
  char* files[31];
  int nf = 0;
  int i;
  f_v = f_c = f_n = f_i = 0;
  f_str = 0;
  f_found = f_err = f_stop = 0;
  f_cmd = cmd;
  for (i = 1; i < argc; i++) {
    int s = find_switch(argv[i], dash);
    if (s == 2) {
      find_help();
      return 0;
    }
    if (s < 0) {
      put_upper(cmd);
      puts_k(": Invalid switch - ");
      puts_k(argv[i]);
      puts_k("\n");
      return 2;
    }
    if (s) continue;
    if (!f_str) f_str = argv[i];
    else files[nf++] = argv[i];
  }
  if (!f_str) {
    put_upper(cmd);
    puts_k(": Parameter format not correct\n");
    puts_k("Uso: ");
    puts_k(cmd);
    puts_k(" [/V] [/C] [/N] [/I] \"texto\" [arquivo ...]   (");
    puts_k(cmd);
    puts_k(" /? ajuda)\n");
    return 2;
  }
  for (f_len = 0; f_str[f_len]; f_len++) {}
  if (!nf) {
    if (!shell_pipe_active()) {
      put_upper(cmd);
      puts_k(": falta o arquivo. Ex.: ");
      puts_k(cmd);
      puts_k(" \"texto\" arquivo.txt  ou  type arquivo.txt | ");
      puts_k(cmd);
      puts_k(" \"texto\"\n");
      return 2;
    }
    rd_path = 0;
    find_src(0);
  }
  for (i = 0; i < nf && !f_stop; i++) {
    int n = drives_glob(files[i], find_one, 0);
    if (n < 0) {
      puts_k("File not found - ");
      put_upper(files[i]);
      puts_k("\n");
      f_err = 1;
    }
  }
  if (f_found) return 0;
  return f_err ? 2 : 1;
}

int cmd_find(int argc, char** argv) {
  return find_main(argc, argv, "find", 0);
}

int cmd_grep(int argc, char** argv) {
  return find_main(argc, argv, "grep", 1);
}
