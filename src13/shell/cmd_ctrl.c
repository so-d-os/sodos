/* cmd_ctrl.c - Comandos para comandar o SODOS por linha de comando: o Claude
 * (pelo WINE e pelo bash do WSL, via API HTTP), BAT, AGENT2, PONTE.
 *
 *   COMMAND [programa args]   abre um Command no WIN e roda a linha nele
 *   PRTSC [/T] [arquivo]      print da tela (a mesma rotina da tecla PrtSc)
 *   TECLA texto {ENTER}...    digita (o mesmo caminho do teclado de verdade)
 *   MOUSE x y [CLICK...]      move e clica (comando MOUSE com argumentos)
 *   JANELAS                   lista as janelas do WIN
 *
 * COMMAND e TECLA recebem a linha crua (shell_ctrl_raw, chamado antes do
 * shell separar | > e <): "command make > log.txt" roda o make com a saida
 * para o arquivo dentro do Command, e "tecla a>b" digita a>b. */
#include "shell.h"
#include "visual.h"
#include "prtscr.h"
#include "ps2in.h"
#include "keymap.h"
#include "serial.h"
#include "drives.h"
#include "mm.h"

static void put_num(int v) {
  char b[16];
  int32_to_string(v, b, (int)sizeof(b));
  puts_k(b);
}

static int lc(int c) {
  return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

/* a e b iguais sem caixa, b ate o fim e a com n letras */
static int word_eq(const char* a, int n, const char* b) {
  int i;
  for (i = 0; i < n; i++)
    if (!b[i] || lc((unsigned char)a[i]) != lc((unsigned char)b[i])) return 0;
  return b[n] == 0;
}

static const char* skip_sp(const char* s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

/* ---------------------------------------------------------------- COMMAND */

static int command_line(const char* rest) {
  int rc;
  rest = skip_sp(rest);
  if (rest[0] == '/' && rest[1] == '?') {
    puts_k("COMMAND [programa [argumentos]]\n"
           "  Abre um Command no WIN e roda a linha nele, como se fosse digitada:\n"
           "  programas de texto (TOP, AGENT2, EDIT) ganham tela e teclado proprios.\n"
           "  Sem linha, so abre o Command. Precisa do WIN aberto.\n");
    return 0;
  }
  rc = win_command(rest);
  if (rc == -1) {
    puts_k("COMMAND: o WIN nao esta aberto (rode WIN antes).\n");
    return 1;
  }
  if (rc != 0) {
    puts_k("COMMAND: o WIN ainda nao abriu o pedido anterior; tente de novo.\n");
    return 1;
  }
  serial_puts("[ctrl] command ");
  serial_puts(rest);
  serial_puts("\n");
  puts_k(rest[0] ? "COMMAND: abrindo no WIN: " : "COMMAND: abrindo no WIN\n");
  if (rest[0]) {
    puts_k(rest);
    puts_k("\n");
  }
  return 0;
}

/* argv[1..] de volta numa linha (so quando nao veio pelo shell_ctrl_raw) */
static void join_args(int argc, char** argv, char* line, int max) {
  int i, k, n = 0;
  for (i = 1; i < argc; i++) {
    if (n && n < max - 1) line[n++] = ' ';
    for (k = 0; argv[i][k] && n < max - 1; k++) line[n++] = argv[i][k];
  }
  line[n] = 0;
}

int cmd_command(int argc, char** argv) {
  char line[256];
  join_args(argc, argv, line, (int)sizeof(line));
  return command_line(line);
}

/* ------------------------------------------------------------------ PRTSC */

static int prtsc_err(int rc) {
  if (rc == -1) puts_k("PRTSC: o WIN nao esta na tela.\n");
  else if (rc == -2) puts_k("PRTSC: sem a unidade Z: (ramdisk).\n");
  else if (rc == -3) puts_k("PRTSC: sem memoria para o PNG.\n");
  else puts_k("PRTSC: nao consegui gravar o PNG.\n");
  return 1;
}

int cmd_prtsc(int argc, char** argv) {
  char path[96];
  const char* dest = 0;
  int text = 0, i, len;
  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '/' && (argv[i][1] == 't' || argv[i][1] == 'T') && !argv[i][2]) {
      text = 1;
    } else if (argv[i][0] == '/' && argv[i][1] == '?') {
      puts_k("PRTSC [/T] [arquivo.png]\n"
             "  Print da tela que se ve, pela mesma rotina da tecla PrtSc: o PNG vai\n"
             "  para Z:\\SAVE\\SCREENSHOT com copia em D:\\TEMP (SET PRTSCRCOPY).\n"
             "  /T: a tela de texto desta tela do DOS. arquivo: mais uma copia ali.\n");
      return 0;
    } else {
      dest = argv[i];
    }
  }
  path[0] = 0;
  if (!text && win_shown_size(0, 0)) len = prtscr_save(path, (int)sizeof(path));
  else len = prtscr_text_save(path, (int)sizeof(path));
  if (len <= 0) return prtsc_err(len);
  puts_k(path);
  puts_k(" (");
  put_num(len);
  puts_k(" bytes)\n");
  if (prtscr_last_copy()[0]) {
    puts_k(prtscr_last_copy());
    puts_k("\n");
  }
  if (dest) {
    char* buf = (char*)umalloc_owned((uint32_t)len + 1u, 0);
    int n = buf ? file_load(path, buf, len + 1) : -1;
    if (n != len || file_save(dest, buf, len) != 0) {
      if (buf) ufree(buf);
      puts_k("PRTSC: nao consegui copiar para ");
      puts_k(dest);
      puts_k("\n");
      return 1;
    }
    ufree(buf);
    puts_k(dest);
    puts_k("\n");
  }
  return 0;
}

/* ------------------------------------------------------------------ TECLA */

/* Latin-1 0xA0..0xFF -> CP850 (texto em UTF-8 vindo do Claude, do curl) */
static const uint8_t lat1_cp850[96] = {
  0xFF, 0xAD, 0xBD, 0x9C, 0xCF, 0xBE, 0xDD, 0xF5, 0xF9, 0xB8, 0xA6, 0xAE, 0xAA, 0xF0, 0xA9, 0xEE,
  0xF8, 0xF1, 0xFD, 0xFC, 0xEF, 0xE6, 0xF4, 0xFA, 0xF7, 0xFB, 0xA7, 0xAF, 0xAC, 0xAB, 0xF3, 0xA8,
  0xB7, 0xB5, 0xB6, 0xC7, 0x8E, 0x8F, 0x92, 0x80, 0xD4, 0x90, 0xD2, 0xD3, 0xDE, 0xD6, 0xD7, 0xD8,
  0xD1, 0xA5, 0xE3, 0xE0, 0xE2, 0xE5, 0x99, 0x9E, 0x9D, 0xEB, 0xE9, 0xEA, 0x9A, 0xED, 0xE8, 0xE1,
  0x85, 0xA0, 0x83, 0xC6, 0x84, 0x86, 0x91, 0x87, 0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,
  0xD0, 0xA4, 0x95, 0xA2, 0x93, 0xE4, 0x94, 0xF6, 0x9B, 0x97, 0xA3, 0x96, 0x81, 0xEC, 0xE7, 0x98,
};

#define TK_EXT 0x100 /* tecla com prefixo E0 */

struct tk_name {
  const char* name;
  int sc;
};

static const struct tk_name tk_names[] = {
  {"enter", 0x1C}, {"tab", 0x0F}, {"esc", 0x01}, {"bs", 0x0E}, {"backspace", 0x0E},
  {"espaco", 0x39}, {"space", 0x39}, {"del", TK_EXT | 0x53}, {"delete", TK_EXT | 0x53},
  {"ins", TK_EXT | 0x52}, {"insert", TK_EXT | 0x52}, {"home", TK_EXT | 0x47},
  {"end", TK_EXT | 0x4F}, {"pgup", TK_EXT | 0x49}, {"pgdn", TK_EXT | 0x51},
  {"cima", TK_EXT | 0x48}, {"up", TK_EXT | 0x48}, {"baixo", TK_EXT | 0x50},
  {"down", TK_EXT | 0x50}, {"esq", TK_EXT | 0x4B}, {"esquerda", TK_EXT | 0x4B},
  {"left", TK_EXT | 0x4B}, {"dir", TK_EXT | 0x4D}, {"direita", TK_EXT | 0x4D},
  {"right", TK_EXT | 0x4D}, {"prtsc", TK_EXT | 0x37}, {"print", TK_EXT | 0x37},
  {"printscreen", TK_EXT | 0x37}, {"win", TK_EXT | 0x5B}, {"menu", TK_EXT | 0x5D},
  {"f1", 0x3B}, {"f2", 0x3C}, {"f3", 0x3D}, {"f4", 0x3E}, {"f5", 0x3F}, {"f6", 0x40},
  {"f7", 0x41}, {"f8", 0x42}, {"f9", 0x43}, {"f10", 0x44}, {"f11", 0x57}, {"f12", 0x58},
};

/* modificadores: make (com E0 se TK_EXT) */
#define MOD_CTRL 1
#define MOD_ALT 2
#define MOD_SHIFT 4
#define MOD_ALTGR 8
#define MOD_WIN 16

static const struct tk_name mod_names[] = {
  {"ctrl", MOD_CTRL}, {"alt", MOD_ALT}, {"shift", MOD_SHIFT},
  {"altgr", MOD_ALTGR}, {"win", MOD_WIN},
};

struct tk_buf {
  uint8_t b[4096];
  int n;
  int keys;
  int bad;
};

static void tb_put(struct tk_buf* t, int v) {
  if (t->n < (int)sizeof(t->b)) t->b[t->n++] = (uint8_t)v;
  else t->bad = 1;
}

/* aperta (up = 0) ou solta (up = 1) a tecla sc (TK_EXT = com E0) */
static void tb_key(struct tk_buf* t, int sc, int up) {
  if (sc & TK_EXT) tb_put(t, 0xE0);
  tb_put(t, (sc & 0x7F) | (up ? 0x80 : 0));
}

static void tb_mods(struct tk_buf* t, int mods, int up) {
  /* soltar na ordem contraria */
  static const int order[5] = {MOD_CTRL, MOD_ALT, MOD_SHIFT, MOD_ALTGR, MOD_WIN};
  static const int code[5] = {0x1D, 0x38, 0x2A, TK_EXT | 0x38, TK_EXT | 0x5B};
  int i;
  for (i = 0; i < 5; i++) {
    int k = up ? 4 - i : i;
    if (mods & order[k]) tb_key(t, code[k], up);
  }
}

static void tb_tap(struct tk_buf* t, int sc, int mods) {
  tb_mods(t, mods, 0);
  tb_key(t, sc, 0);
  tb_key(t, sc, 1);
  tb_mods(t, mods, 1);
  t->keys++;
}

/* Um caractere CP850 pelas teclas do layout atual. 0 se nao tem tecla. */
static int tb_char(struct tk_buf* t, int ch, int extra) {
  uint8_t sc[2], km[2];
  int n, i;
  if (ch == '\n' || ch == '\r') {
    tb_tap(t, 0x1C, extra);
    return 1;
  }
  n = keymap_keys(ch, sc, km, 2);
  if (n <= 0) return 0;
  for (i = 0; i < n; i++) {
    int m = extra;
    if (km[i] & KEYM_SHIFT) m |= MOD_SHIFT;
    if (km[i] & KEYM_ALTGR) m |= MOD_ALTGR;
    tb_tap(t, sc[i], m);
  }
  return 1;
}

/* proximo caractere: UTF-8 de 2 bytes (C2/C3) vira CP850; o resto e CP850 */
static int next_char(const char** ps) {
  const unsigned char* s = (const unsigned char*)*ps;
  int c = s[0];
  if ((c == 0xC2 || c == 0xC3) && s[1] >= 0x80 && s[1] <= 0xBF) {
    int u = ((c & 0x1F) << 6) | (s[1] & 0x3F);
    *ps += 2;
    return u < 0xA0 ? 0 : lat1_cp850[u - 0xA0]; /* 0x80..0x9F: controle */
  }
  *ps += 1;
  return c;
}

static int tk_lookup(const struct tk_name* tab, int n, const char* w, int len) {
  int i;
  for (i = 0; i < n; i++)
    if (word_eq(w, len, tab[i].name)) return tab[i].sc;
  return -1;
}

/* {CTRL+C}, {ENTER}, {TAB 3}, {>}: o que esta entre as chaves. 0 = nao sei. */
static int tb_brace(struct tk_buf* t, const char* w, int len) {
  int mods = 0, reps = 1, i, start = 0;
  /* repeticao: "{TAB 3}" */
  for (i = len - 1; i > 0 && w[i] >= '0' && w[i] <= '9'; i--) {}
  if (i > 0 && i < len - 1 && w[i] == ' ') {
    int r = 0, k;
    for (k = i + 1; k < len; k++) r = r * 10 + (w[k] - '0');
    if (r >= 1 && r <= 200) reps = r;
    len = i;
  }
  /* modificadores: "CTRL+ALT+DEL"; um '+' sozinho (ou no fim) e a tecla + */
  for (i = 0; i < len; i++) {
    if (w[i] == '+' && i > start && i < len - 1) {
      int m = tk_lookup(mod_names, (int)(sizeof(mod_names) / sizeof(mod_names[0])), w + start, i - start);
      if (m < 0) return 0;
      mods |= m;
      start = i + 1;
    }
  }
  w += start;
  len -= start;
  while (reps-- > 0) {
    int sc;
    if (len == 1 || ((unsigned char)w[0] >= 0xC2 && len == 2)) {
      const char* p = w;
      int ch = next_char(&p);
      /* Ctrl+C, Alt+F: a letra minuscula, sem o Shift do maiusculo */
      if (mods & (MOD_CTRL | MOD_ALT | MOD_WIN)) ch = lc(ch);
      if (!tb_char(t, ch, mods)) return 0;
      continue;
    }
    sc = tk_lookup(tk_names, (int)(sizeof(tk_names) / sizeof(tk_names[0])), w, len);
    if (sc < 0) return 0;
    tb_tap(t, sc, mods);
  }
  return 1;
}

static int tecla_line(const char* s) {
  static struct tk_buf t;
  int miss = 0;
  s = skip_sp(s);
  if (!s[0] || (s[0] == '/' && s[1] == '?')) {
    puts_k("TECLA texto {TECLA} ...\n"
           "  Digita na janela com foco, pelo mesmo caminho do teclado de verdade\n"
           "  (layout de KEYB). Teclas: {ENTER} {TAB} {ESC} {BS} {DEL} {INS} {HOME}\n"
           "  {END} {PGUP} {PGDN} {CIMA} {BAIXO} {ESQ} {DIR} {F1}..{F12} {ESPACO}\n"
           "  {PRTSC} {WIN}; combinacoes {CTRL+C} {ALT+TAB} {CTRL+F12} {SHIFT+TAB};\n"
           "  repeticao {TAB 3}; caractere literal {>} {|} {<} {{} {}}.\n");
    return s[0] ? 0 : 1;
  }
  t.n = 0;
  t.keys = 0;
  t.bad = 0;
  while (*s) {
    if (*s == '{') {
      const char* e;
      if (s[1] == '}' && s[2] == '}') { /* {}} */
        if (!tb_char(&t, '}', 0)) miss++;
        s += 3;
        continue;
      }
      e = s + 2;
      while (*e && *e != '}') e++;
      if (*e != '}' || !tb_brace(&t, s + 1, (int)(e - s - 1))) {
        puts_k("TECLA: nao conheco ");
        while (s <= e && *s) putc(*s++);
        puts_k("\n");
        return 1;
      }
      s = e + 1;
      continue;
    }
    if (!tb_char(&t, next_char(&s), 0)) miss++;
  }
  if (t.bad) {
    puts_k("TECLA: texto grande demais (ate ~500 teclas por vez).\n");
    return 1;
  }
  if (ps2in_type(t.b, t.n) != 0) {
    puts_k("TECLA: a fila do teclado esta cheia; espere as teclas anteriores.\n");
    return 1;
  }
  put_num(t.keys);
  puts_k(" teclas");
  if (miss) {
    puts_k(", ");
    put_num(miss);
    puts_k(" caracteres sem tecla no layout (ficaram de fora)");
  }
  puts_k("\n");
  return miss ? 2 : 0;
}

int cmd_tecla(int argc, char** argv) {
  char line[256];
  join_args(argc, argv, line, (int)sizeof(line));
  return tecla_line(line);
}

/* ------------------------------------------------------------------ MOUSE */

static int num_arg(const char* s, int* v) {
  int x = 0, any = 0, neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  }
  while (*s >= '0' && *s <= '9') {
    x = x * 10 + (*s++ - '0');
    any = 1;
  }
  if (!any || *s) return 0;
  *v = neg ? -x : x;
  return 1;
}

/* MOUSE com argumentos (sem argumentos continua o teste do mouse). */
int mouse_ctrl(int argc, char** argv) {
  static const uint8_t click[2] = {1, 0}, right[2] = {2, 0}, dbl[4] = {1, 0, 1, 0};
  static const uint8_t down[1] = {1}, up[1] = {0}, mid[2] = {4, 0};
  const uint8_t* seq = 0;
  int n = 0, x = 0, y = 0, move = 0, i = 1;
  if (argc >= 3 && num_arg(argv[1], &x) && num_arg(argv[2], &y)) {
    move = 1;
    i = 3;
  }
  for (; i < argc; i++) {
    const char* a = argv[i];
    int len = 0;
    while (a[len]) len++;
    if (word_eq(a, len, "click") || word_eq(a, len, "clique") || word_eq(a, len, "c")) {
      seq = click;
      n = 2;
    } else if (word_eq(a, len, "direito") || word_eq(a, len, "right") || word_eq(a, len, "r")) {
      seq = right;
      n = 2;
    } else if (word_eq(a, len, "duplo") || word_eq(a, len, "double") || word_eq(a, len, "d")) {
      seq = dbl;
      n = 4;
    } else if (word_eq(a, len, "meio") || word_eq(a, len, "middle")) {
      seq = mid;
      n = 2;
    } else if (word_eq(a, len, "desce") || word_eq(a, len, "down")) {
      seq = down;
      n = 1;
    } else if (word_eq(a, len, "sobe") || word_eq(a, len, "up")) {
      seq = up;
      n = 1;
    } else if (word_eq(a, len, "pos")) {
      /* so mostra */
    } else {
      puts_k("MOUSE x y [CLICK|DIREITO|DUPLO|MEIO|DESCE|SOBE]   move e clica no WIN\n"
             "MOUSE CLICK (no lugar)   MOUSE POS (mostra)   MOUSE (teste do mouse)\n"
             "  DESCE, MOUSE x y, SOBE: arrasta com o botao esquerdo.\n");
      return 1;
    }
  }
  if ((move || n) && mouse_api_seq(x, y, move, seq, n) != 0) {
    puts_k("MOUSE: fila cheia; o WIN ainda nao leu os cliques anteriores.\n");
    return 1;
  }
  /* o destino pedido: o cursor so anda quando o WIN le a fila */
  if (!move) mouse_api_pos(&x, &y);
  puts_k("x ");
  put_num(x);
  puts_k(" y ");
  put_num(y);
  puts_k("\n");
  if ((move || n) && !win_shown_size(0, 0))
    puts_k("aviso: o WIN nao esta na tela; o mouse vale quando ele voltar.\n");
  return 0;
}

/* ---------------------------------------------------------------- JANELAS */

int cmd_janelas(int argc, char** argv) {
  static char out[4096];
  int w = 0, h = 0;
  (void)argc;
  (void)argv;
  if (win_windows(out, (int)sizeof(out)) < 0) {
    puts_k("JANELAS: o WIN nao esta aberto.\n");
    return 1;
  }
  if (win_shown_size(&w, &h)) {
    puts_k("tela ");
    put_num(w);
    puts_k(" x ");
    put_num(h);
    puts_k(" (* = foco; titulo x y largura altura)\n");
  } else {
    puts_k("(o WIN esta aberto atras de uma tela do DOS)\n");
  }
  puts_k(out);
  return 0;
}

/* ------------------------------------------------------- linha crua (shell) */

/* Chamado pelo shell antes de separar | > <: COMMAND e TECLA levam a linha
 * inteira. *rc = codigo; devolve 1 se a linha era de um deles. */
int shell_ctrl_raw(const char* line, int* rc) {
  const char* s = skip_sp(line);
  int n = 0;
  while (s[n] && s[n] != ' ' && s[n] != '\t') n++;
  if (word_eq(s, n, "command") || word_eq(s, n, "command.exe")) {
    *rc = command_line(s + n);
    return 1;
  }
  if (word_eq(s, n, "tecla")) {
    *rc = tecla_line(s + n);
    return 1;
  }
  return 0;
}
