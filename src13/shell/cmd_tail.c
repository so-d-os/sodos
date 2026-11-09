/* tail: as ultimas linhas, como o TAIL do Resource Kit do DOS/Windows.
 *   TAIL [/n] [[unidade:][caminho]arquivo ...]
 * n e quantas linhas (10 sem nada); -n, /N n e /N:n tambem valem.
 * Sem arquivo le o pipe (dir | tail /5). Arquivos aceitam * e ?; com mais
 * de um, cada um vem com "==> NOME <==". */
#include "shell.h"
#include "drives.h"
#include "reboot.h"
#include "mm.h"

#define TAIL_PIPE_MAX (256u * 1024u)

static uint32_t t_lines;
static int t_many;
static int t_err;
static int t_stop;
static char t_buf[1024];

/* Le len bytes de src (arquivo, ou o pipe guardado quando path e nulo). */
static const char* t_path;
static const char* t_mem;
static uint32_t t_size;

static int t_read(uint32_t off, int len) {
  int i;
  if (t_path) return file_read_at(t_path, off, t_buf, len);
  if (off >= t_size) return 0;
  if ((uint32_t)len > t_size - off) len = (int)(t_size - off);
  for (i = 0; i < len; i++) t_buf[i] = t_mem[off + i];
  return len;
}

/* Inicio das ultimas t_lines linhas. A quebra do fim nao conta. */
static uint32_t tail_start(void) {
  uint32_t pos = t_size, end = t_size, seen = 0;
  if (!t_lines) return t_size;
  if (end && t_read(end - 1, 1) == 1 && t_buf[0] == '\n') end--;
  while (pos > 0) {
    int chunk = pos < sizeof(t_buf) ? (int)pos : (int)sizeof(t_buf);
    int k;
    pos -= (uint32_t)chunk;
    if (t_read(pos, chunk) != chunk) return 0;
    for (k = chunk - 1; k >= 0; k--)
      if (t_buf[k] == '\n' && pos + (uint32_t)k < end && ++seen == t_lines)
        return pos + (uint32_t)k + 1;
  }
  return 0;
}

static void tail_src(const char* shown) {
  uint32_t off;
  if (t_many) {
    puts_k("==> ");
    puts_k(shown);
    puts_k(" <==\n");
  }
  text_begin(0);
  for (off = tail_start(); off < t_size;) {
    int n = t_read(off, (int)sizeof(t_buf));
    if (n <= 0) break;
    off += (uint32_t)n;
    text_put(t_buf, n);
    if (kbd_break_take()) {
      t_stop = 1;
      break;
    }
  }
  text_end();
  if (t_many) puts_k("\n");
}

static int tail_one(const char* path, const char* shown, void* ctx) {
  int sz = file_size(path);
  (void)ctx;
  if (sz < 0) {
    puts_k("File not found: ");
    puts_k(shown);
    puts_k("\n");
    t_err = 1;
    return 0;
  }
  t_path = path;
  t_size = (uint32_t)sz;
  tail_src(shown);
  return t_stop;
}

/* Numero inteiro de s; -1 se nao e. */
static int to_num(const char* s, uint32_t* out) {
  uint32_t v = 0;
  if (!*s) return -1;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return -1;
    v = v * 10u + (uint32_t)(*s - '0');
  }
  *out = v;
  return 0;
}

static void tail_help(void) {
  puts_k("Mostra as ultimas linhas de arquivos ou da saida de outro comando.\n\n");
  puts_k("TAIL [/n] [[unidade:][caminho]arquivo ...]\n\n");
  puts_k("  /n  Quantas linhas (10 se omitido). Tambem: -n, /N n, /N:n\n");
  puts_k("  arquivo  Arquivo(s); aceita * e ?.\n\n");
  puts_k("  tail /20 log.txt\n");
  puts_k("  dir | tail /5\n");
}

static int has_wild(const char* s) {
  for (; *s; s++)
    if (*s == '*' || *s == '?') return 1;
  return 0;
}

int cmd_tail(int argc, char** argv) {
  char* files[31];
  int nf = 0;
  int i;
  t_lines = 10;
  t_err = t_stop = 0;
  for (i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (a[0] == '/' || (a[0] == '-' && a[1])) {
      if (a[1] == '?') {
        tail_help();
        return 0;
      }
      if (to_num(a + 1, &t_lines) == 0) continue;
      if ((a[1] == 'n' || a[1] == 'N') && a[2] == ':' && to_num(a + 3, &t_lines) == 0)
        continue;
      if ((a[1] == 'n' || a[1] == 'N') && !a[2] && i + 1 < argc &&
          to_num(argv[i + 1], &t_lines) == 0) {
        i++;
        continue;
      }
      puts_k("TAIL: Invalid switch - ");
      puts_k(a);
      puts_k("\n");
      return 2;
    }
    files[nf++] = argv[i];
  }
  if (!nf) {
    char* mem;
    int c;
    if (!shell_pipe_active()) {
      puts_k("Uso: tail [/n] arquivo ...   ou   comando | tail [/n]   (tail /? ajuda)\n");
      return 1;
    }
    mem = (char*)kmalloc(TAIL_PIPE_MAX);
    if (!mem) {
      puts_k("TAIL: sem memoria\n");
      return 2;
    }
    t_size = 0;
    while ((c = shell_pipe_getc()) >= 0 && t_size < TAIL_PIPE_MAX) mem[t_size++] = (char)c;
    t_path = 0;
    t_mem = mem;
    t_many = 0;
    tail_src("");
    kfree(mem);
    return 0;
  }
  t_many = nf > 1;
  for (i = 0; i < nf; i++)
    if (has_wild(files[i])) t_many = 1;
  for (i = 0; i < nf && !t_stop; i++) {
    int n = drives_glob(files[i], tail_one, 0);
    if (n < 0) {
      puts_k("File not found: ");
      puts_k(files[i]);
      puts_k("\n");
      t_err = 1;
    }
  }
  return t_err;
}
