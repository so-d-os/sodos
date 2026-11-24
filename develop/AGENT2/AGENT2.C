/* agent2.c - AGENT2.EXE: agente de IA do SODOS com tela no padrao do TOP.
 *
 * Mesmo modelo de conexao do AGENT.EXE: le o AGENT.CFG (key/url/model,
 * anthropic=, deepseek=) e fala com a API no formato OpenAI via ai_client.c
 * (fonte unica, como o AIPANEL). A tela segue o SPECIAL/TOP: janela com
 * sombra, barra de menu em cima, barra de teclas embaixo, bolhas de conversa.
 *
 * O modelo responde com um bloco de acao (```cmd, ```write CAMINHO, ```open)
 * ou com as ferramentas nativas; o AGENT2 roda no prompt (k_shell_capture),
 * devolve a saida e pergunta de novo, ate o modelo responder sem acao.
 *
 * Sessoes: cada conversa nova ganha um arquivo SESSOES\dd-mm-aa-hhmmss.SES
 * (pasta de onde o AGENT2 foi aberto, ou sessoes= no AGENT.CFG). F2 lista
 * as sessoes salvas com uma previa e carrega a escolhida. F4 anexa arquivos
 * ou pastas: o conteudo vai junto no system prompt a cada pergunta.
 *
 *   cc -I D:\ -L D:\ -o AGENT2.EXE AGENT2.C
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include <tls.h>
#include "ai_client.c"

/* ==================== tela (padrao do TOP/SPECIAL) ==================== */
#define G_BL 0xC0
#define G_BR 0xC1
#define G_HB 0xC2
#define G_VL 0xC3
#define G_VR 0xC4
#define G_CLOSE 0xC5
#define G_SEPL 0xC6
#define G_SEPH 0xC7
#define G_SEPR 0xC8
#define G_BTNL 0xC9
#define G_BTNR 0xCA
#define G_DESK 0xD9
#define G_BASE 0xC0
#define N_GLIFOS 11
#define G_DIV 0xB3

#define COR(fundo, frente) (((fundo) << 4) | (frente))
#define PRETO 0
#define AZUL 1
#define CIANO 3
#define CINZA 7
#define AZUL_CLARO 9
#define AMARELO 14
#define BRANCO 15

#define C_JANELA COR(AZUL_CLARO, BRANCO)
#define C_BORDA COR(AZUL_CLARO, BRANCO)
#define C_TITULO COR(BRANCO, AZUL)
#define C_FORTE COR(AZUL_CLARO, BRANCO)
#define C_FRACO COR(AZUL_CLARO, AZUL)
#define C_FUNDO COR(PRETO, AZUL)
#define C_MENU COR(CIANO, PRETO)
#define C_MENUSEL COR(PRETO, BRANCO)
#define C_MENUHOT COR(CIANO, AMARELO)
#define C_TECLA COR(PRETO, CINZA)
#define C_ROTULO COR(CIANO, PRETO)
#define C_ME COR(BRANCO, AZUL)
#define C_AI COR(AZUL_CLARO, PRETO)
#define C_SYS COR(AZUL_CLARO, AZUL)

/* Codigos do kernel (shell.h, os mesmos do TOP). */
#define K_UP 0x100
#define K_DOWN 0x101
#define K_LEFT 0x102
#define K_RIGHT 0x103
#define K_PGUP 0x104
#define K_PGDN 0x105
#define K_HOME 0x106
#define K_END 0x107
#define K_DEL 0x108

#define K_F(n) (0x140 + (n))
#define K_ESC 27
#define K_ENTER '\n'
#define K_EOF 26
#define K_NADA (-1)
#define K_RESIZE 0x1F0

#define TMAXC 256
#define TMAXR 96
static int COLS = 80, ROWS = 25;

static const unsigned char glifos[N_GLIFOS][16] = {
  {0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFF,0xFF},
  {0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0xFF,0xFF},
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF},
  {0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0},
  {0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03},
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFF,0xFF,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0},
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x03,0x03,0x03,0x03,0x03,0x03,0x03,0xFF,0xFF,0x03,0x03,0x03,0x03,0x03,0x03,0x03},
  {0x0F,0x3F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x3F,0x0F},
  {0xF0,0xFC,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFC,0xF0},
};

static unsigned char salvo[N_GLIFOS][16];
static unsigned short tela[TMAXR * TMAXC];
static unsigned short vista[TMAXR * TMAXC];
static unsigned short copia[TMAXR * TMAXC];

static void tamanho(void) {
  int i, c = 80, r = 25;
  k_textsize(&c, &r);
  if (c < 40) c = 40;
  if (r < 12) r = 12;
  if (c > TMAXC) c = TMAXC;
  if (r > TMAXR) r = TMAXR;
  COLS = c;
  ROWS = r;
  for (i = 0; i < TMAXR * TMAXC; i++) vista[i] = 0xFFFF;
}

static int tela_inicia(void) {
  int i;
  if (k_vput(0, 0, tela, 0) != 0) return -1;
  tamanho();
  if (k_blink(0) != 0) return -1;
  for (i = 0; i < N_GLIFOS; i++)
    if (k_font_get(G_BASE + i, salvo[i]) != 0) return -1;
  for (i = 0; i < N_GLIFOS; i++)
    if (k_font_set(G_BASE + i, glifos[i]) != 0) {
      for (i = 0; i < N_GLIFOS; i++) k_font_set(G_BASE + i, salvo[i]);
      k_blink(1);
      return -1;
    }
  k_cursor(-1, 0);
  return 0;
}

static void tela_fim(void) {
  int i;
  for (i = 0; i < N_GLIFOS; i++) k_font_set(G_BASE + i, salvo[i]);
  k_blink(1);
  k_clear();
  k_cursor(0, 0);
}

static void tela_mostra(void) {
  int r, c;
  for (r = 0; r < ROWS; r++) {
    int a = -1, b = -1;
    unsigned short* t = tela + r * TMAXC;
    unsigned short* v = vista + r * TMAXC;
    for (c = 0; c < COLS; c++) {
      if (t[c] != v[c]) {
        if (a < 0) a = c;
        b = c;
      }
    }
    if (a < 0) continue;
    k_vput(r, a, t + a, b - a + 1);
    for (c = a; c <= b; c++) v[c] = t[c];
  }
}

static void poe(int r, int c, int ch, int cor) {
  if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
  tela[r * TMAXC + c] = (unsigned short)((ch & 0xFF) | ((cor & 0xFF) << 8));
}

static void texto(int r, int c, const char* s, int cor) {
  while (*s) poe(r, c++, (unsigned char)*s++, cor);
}

static void textow(int r, int c, const char* s, int w, int cor) {
  int i = 0;
  while (i < w && s[i]) {
    poe(r, c + i, (unsigned char)s[i], cor);
    i++;
  }
  while (i < w) poe(r, c + i++, ' ', cor);
}

static void linha(int r, int c, int n, int ch, int cor) {
  while (n-- > 0) poe(r, c++, ch, cor);
}

static int tam(const char* s) { return (int)strlen(s); }

static void fundo(void) {
  int r;
  for (r = 1; r < ROWS - 1; r++) linha(r, 0, COLS, G_DESK, C_FUNDO);
}

static void teclas(const char* const* rotulos) {
  int i, c = 0;
  char n[4];
  int kw = COLS / 10;
  linha(ROWS - 1, 0, COLS, ' ', C_ROTULO);
  for (i = 0; i < 10; i++) {
    int w = i == 9 ? COLS - c : kw;
    sprintf(n, "%d", i + 1);
    texto(ROWS - 1, c, n, C_TECLA);
    textow(ROWS - 1, c + tam(n), rotulos[i], w - tam(n), C_ROTULO);
    c += w;
  }
}

static void janela(int r, int c, int h, int w, const char* titulo) {
  int i;
  for (i = 1; i < h; i++) linha(r + i, c + w, 2, ' ', COR(PRETO, PRETO));
  linha(r + h, c + 2, w, ' ', COR(PRETO, PRETO));
  for (i = 1; i < h - 1; i++) {
    linha(r + i, c + 1, w - 2, ' ', C_JANELA);
    poe(r + i, c, G_VL, C_BORDA);
    poe(r + i, c + w - 1, G_VR, C_BORDA);
  }
  linha(r + h - 1, c + 1, w - 2, G_HB, C_BORDA);
  poe(r + h - 1, c, G_BL, C_BORDA);
  poe(r + h - 1, c + w - 1, G_BR, C_BORDA);
  linha(r, c, w, ' ', C_TITULO);
  poe(r, c, G_CLOSE, COR(PRETO, BRANCO));
  texto(r, c + (w - tam(titulo)) / 2, titulo, C_TITULO);
}

static void separador(int r, int c, int w) {
  poe(r, c, G_SEPL, C_BORDA);
  linha(r, c + 1, w - 2, G_SEPH, C_BORDA);
  poe(r, c + w - 1, G_SEPR, C_BORDA);
}

static int tecla(int ms) {
  unsigned t0 = k_ms();
  int k;
  tela_mostra();
  if (ms <= 0) {
    k = k_getc();
  } else {
    for (;;) {
      k = k_trygetc();
      if (k >= 0) break;
      if (k_ms() - t0 >= (unsigned)ms) return K_NADA;
      k_yield();
    }
  }
  if (k < 0) return K_EOF;
  if (k == K_RESIZE) {
    tamanho();
    return K_NADA;
  }
  if (k == '\r') return K_ENTER;
  return k;
}

/* Caixa Sim/Nao no padrao do TOP: 1 se Enter, S ou Y; 0 se Esc ou N. */
static int pergunta(const char* titulo, const char* msg) {
  int w = tam(msg) + 8, h = 7, r, c, k, sim = 0;
  if (w < 36) w = 36;
  if (w > COLS - 4) w = COLS - 4;
  r = (ROWS - 1 - h) / 2;
  c = (COLS - w) / 2;
  memcpy(copia, tela, sizeof tela);
  janela(r, c, h, w, titulo);
  textow(r + 2, c + 3, msg, w - 6, C_FORTE);
  separador(r + h - 3, c, w);
  texto(r + h - 2, c + 3, "Enter/S = sim     ESC/N = nao", C_FRACO);
  tela_mostra();
  for (;;) {
    k = tecla(0);
    if (k == K_ENTER || k == 's' || k == 'S' || k == 'y' || k == 'Y') {
      sim = 1;
      break;
    }
    if (k == K_ESC || k == K_EOF || k == 'n' || k == 'N') break;
  }
  memcpy(tela, copia, sizeof tela);
  return sim;
}

/* ==================== conversa ==================== */
#define MSG_MAX 80
#define MSG_LEN 2400
#define ROLE_ME 1
#define ROLE_AI 0
#define ROLE_SYS 2
#define ROLE_OBS 3
#define ROLE_INFO 4   /* aviso so da tela: nao vai para a API nem para o arquivo */
#define STEPS_MAX 8
#define WRAP_MAX 400

struct msg {
  int role;
  int nl;      /* linhas de texto na tela (cache) */
  int nl_cpl;  /* largura do cache; 0 = recalcular */
  char text[MSG_LEN];
};

static struct msg g_msgs[MSG_MAX];
static int g_n;
static ai_cfg_t g_cfg;
static int g_waiting;
static char g_stream[MSG_LEN];
static int g_stream_len;
static char g_edit[MSG_LEN];
static int g_edit_len;
static char g_status[120];
static ai_tool_t g_tools[4];
static int g_busy;
static int g_scroll;   /* linhas acima do fim da conversa */
static int g_nav;      /* 1 = navegando na conversa (Espaco volta a digitar) */
static int g_vis = 10; /* linhas visiveis da conversa (do ultimo desenho) */

/* sessao */
static char g_pasta0[160];   /* pasta de onde o AGENT2 foi aberto */
static char g_sesdir[160];   /* pasta das sessoes */
static char g_sesname[40];   /* dd-mm-aa-hhmmss */
static char g_sespath[220];  /* arquivo da sessao atual */
static int g_dirty;          /* ha mudanca a gravar */

/* anexos */
#define ANX_MAX 8
#define ANX_TOTAL 32000    /* bytes de anexos no system prompt */
#define ANX_ARQ 12000      /* por arquivo anexado */
#define ANX_NAPASTA 4000   /* por arquivo dentro de pasta anexada */
struct anexo {
  char path[160];
  int dir;
};
static struct anexo g_anx[ANX_MAX];
static int g_nanx;
static char g_brdir[160];    /* ultima pasta aberta no Anexar ("" = unidades) */

static const char* SYS_BASE =
    "You are AGENT2, the assistant that runs natively inside SODOS (a small "
    "DOS-like OS: 64-bit kernel, FAT16 on C:/D:/U:, the WIN desktop, its own "
    "C compiler CC.EXE). You act on the system and never say you cannot. "
    "Answer in Brazilian Portuguese, short.\n"
    "To act, end the reply with exactly ONE fenced block and stop:\n"
    "```cmd\n<one DOS command line>\n```\n"
    "```write D:\\PATH\\FILE.EXT\n<file content>\n```\n"
    "```open D:\\PATH\\PROGRAM.EXE\n```\n"
    "After the block you get its result and may act again; when done, or if "
    "you need the user, reply with NO block. Use DOS commands (dir, type, "
    "copy, del, mkdir), 8.3 names; compile with: cc -I D:\\ -L D:\\ -o "
    "PROG.EXE PROG.C. Never run edit, mc, win or other interactive programs.\n";

/* Buffer do system prompt montado a cada chamada: texto base + contexto
 * do SODOS (pasta atual, listagem, hardware, memoria, disco, rede). */
static char g_sys[4096];

static void sys_add(char* d, int cap, int* n, const char* s) {
  while (*s && *n < cap - 1) d[(*n)++] = *s++;
  d[*n] = 0;
}

static void build_sys(void) {
  char pwd[128];
  char lst[1024];
  char tmp[256];
  unsigned tot = 0, fre = 0;
  int n = 0, dr, i, cnt;
  struct k_hwstat hw;

  g_sys[0] = 0;
  sys_add(g_sys, (int)sizeof(g_sys), &n, SYS_BASE);
  sys_add(g_sys, (int)sizeof(g_sys), &n,
          "\n=== CONTEXTO DO SODOS (agora) ===\n");

  /* pasta e unidade atuais */
  pwd[0] = 0;
  k_pwd(pwd, (int)sizeof(pwd));
  dr = k_drive();
  sys_add(g_sys, (int)sizeof(g_sys), &n, "Pasta atual: ");
  sys_add(g_sys, (int)sizeof(g_sys), &n, pwd[0] ? pwd : "?");
  sys_add(g_sys, (int)sizeof(g_sys), &n, "\n");
  snprintf(tmp, sizeof(tmp), "Unidade atual: %c:  (EXE em %c:)\n",
           dr ? dr : '?', k_exe_drive() ? k_exe_drive() : '?');
  sys_add(g_sys, (int)sizeof(g_sys), &n, tmp);
  sys_add(g_sys, (int)sizeof(g_sys), &n,
          "Comandos sem caminho usam a pasta atual; use caminho completo "
          "quando precisar de outra pasta.\n");

  /* listagem resumida da pasta atual */
  lst[0] = 0;
  cnt = k_dirlist(pwd[0] ? pwd : ".", lst, (int)sizeof(lst));
  if (cnt > 0) {
    sys_add(g_sys, (int)sizeof(g_sys), &n, "Conteudo da pasta atual:\n");
    sys_add(g_sys, (int)sizeof(g_sys), &n, lst);
    sys_add(g_sys, (int)sizeof(g_sys), &n, "\n");
  }

  /* hardware e memoria */
  memset(&hw, 0, sizeof(hw));
  if (k_hwstat(&hw) == 0) {
    snprintf(tmp, sizeof(tmp),
             "CPUs: %d  carga: %d%%  temp CPU: %d C  uptime: %u s\n",
             hw.ncpu, hw.load_pct, hw.cpu_c, hw.uptime_s);
    sys_add(g_sys, (int)sizeof(g_sys), &n, tmp);
    snprintf(tmp, sizeof(tmp), "Memoria: %u MB total, %u MB livre\n",
             hw.mem_total_mb, hw.mem_free_mb);
    sys_add(g_sys, (int)sizeof(g_sys), &n, tmp);
  }

  /* discos */
  for (i = 0; i < 26; i++) {
    if (!(k_drive_mask() & (1u << i))) continue;
    if (k_dfree('A' + i, &tot, &fre) == 0) {
      snprintf(tmp, sizeof(tmp), "Disco %c: %u KB livres de %u KB\n",
               'A' + i, fre, tot);
      sys_add(g_sys, (int)sizeof(g_sys), &n, tmp);
    }
  }

  /* rede */
  sys_add(g_sys, (int)sizeof(g_sys), &n,
          k_net_ready() ? "Rede: pronta\n" : "Rede: NAO pronta (rode net)\n");
  sys_add(g_sys, (int)sizeof(g_sys), &n,
          "=== fim do contexto ===\n");
}

static void set_status(const char* s) {
  int i = 0;
  while (s[i] && i < (int)sizeof(g_status) - 1) {
    g_status[i] = s[i];
    i++;
  }
  g_status[i] = 0;
}

static void add_msg_n(int role, const char* text, int n) {
  int i;
  if (g_n >= MSG_MAX) {
    for (i = 1; i < MSG_MAX; i++) g_msgs[i - 1] = g_msgs[i];
    g_n = MSG_MAX - 1;
  }
  if (n < 0) n = 0;
  if (n > MSG_LEN - 1) n = MSG_LEN - 1;
  g_msgs[g_n].role = role;
  g_msgs[g_n].nl = 0;
  g_msgs[g_n].nl_cpl = 0;
  memcpy(g_msgs[g_n].text, text, (size_t)n);
  g_msgs[g_n].text[n] = 0;
  g_n++;
  if (role != ROLE_INFO) g_dirty = 1;
}

static void add_msg(int role, const char* text) {
  add_msg_n(role, text, (int)strlen(text));
}

/* Quebra o texto em linhas de cpl colunas; devolve quantas. */
static int wrap_lines(const char* text, int cpl, int* start, int* len, int maxl) {
  int n = 0, i = 0, col = 0, s = 0;
  if (cpl < 1) cpl = 1;
  while (text[i] && n < maxl) {
    if (text[i] == '\n') {
      start[n] = s;
      len[n] = i - s;
      n++;
      i++;
      s = i;
      col = 0;
      continue;
    }
    if (col >= cpl) {
      start[n] = s;
      len[n] = i - s;
      n++;
      s = i;
      col = 0;
      continue;
    }
    i++;
    col++;
  }
  if (n < maxl && i > s) {
    start[n] = s;
    len[n] = i - s;
    n++;
  }
  return n;
}

/* CP850 -> UTF-8 so com sequencias validas: o que nao e Latin-1 vira '?'.
 * (O JSON da API recusa UTF-8 invalido; o texto dos anexos vem em CP850.) */
static int utf8_seguro(char* dst, int cap, const char* src, int n) {
  int i, o = 0;
  for (i = 0; i < n; i++) {
    unsigned c = (unsigned char)src[i];
    unsigned u = 0;
    unsigned k;
    if (c < 0x80) {
      if (o + 1 >= cap) break;
      dst[o++] = (char)c;
      continue;
    }
    for (k = 0; k < 96; k++)
      if ((unsigned)latin1_cp850[k] == c) {
        u = 0xA0 + k;
        break;
      }
    if (!u) {
      if (o + 1 >= cap) break;
      dst[o++] = '?';
      continue;
    }
    if (o + 2 >= cap) break;
    dst[o++] = (char)(0xC0 | (u >> 6));
    dst[o++] = (char)(0x80 | (u & 0x3F));
  }
  dst[o] = 0;
  return o;
}

/* Mensagem com texto em CP850 (saida de comando, caminhos do disco): guarda
 * em UTF-8, como as outras, para a tela e para o JSON da API. */
static void add_msg_cp850(int role, const char* text) {
  static char u[MSG_LEN];
  utf8_seguro(u, (int)sizeof(u), text, (int)strlen(text));
  add_msg(role, u);
}

static int maiuscula(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int igual_sem_caixa(const char* a, const char* b) {
  while (*a && *b) {
    if (maiuscula((unsigned char)*a) != maiuscula((unsigned char)*b)) return 0;
    a++;
    b++;
  }
  return *a == *b;
}

static int cmp_sem_caixa(const char* a, const char* b) {
  while (*a && *b) {
    int x = maiuscula((unsigned char)*a), y = maiuscula((unsigned char)*b);
    if (x != y) return x - y;
    a++;
    b++;
  }
  return maiuscula((unsigned char)*a) - maiuscula((unsigned char)*b);
}

/* ==================== pastas e sessoes ==================== */
static void junta(char* out, int cap, const char* dir, const char* nome) {
  int n = (int)strlen(dir);
  if (n > 0 && dir[n - 1] == '\\')
    snprintf(out, (size_t)cap, "%s%s", dir, nome);
  else
    snprintf(out, (size_t)cap, "%s\\%s", dir, nome);
}

/* Cria a pasta e as de cima que faltarem (k_mkdir so cria um nivel). */
static void cria_pastas(const char* p) {
  char t[160];
  int i;
  for (i = 0; p[i] && i < (int)sizeof(t) - 1; i++) {
    if (p[i] == '\\' && i > 2) {
      t[i] = 0;
      k_mkdir(t);
    }
    t[i] = p[i];
  }
  t[i] = 0;
  k_mkdir(t);
}

/* Pasta atual sempre como "X:\CAMINHO" (sem barra no fim, salvo na raiz). */
static void pasta_atual(char* out, int cap) {
  char p[160];
  int n;
  p[0] = 0;
  k_pwd(p, (int)sizeof(p));
  if (p[0] && p[1] == ':') {
    snprintf(out, (size_t)cap, "%s", p);
  } else {
    int d = k_drive();
    snprintf(out, (size_t)cap, "%c:%s%s", d ? d : 'C', p[0] == '\\' ? "" : "\\", p);
  }
  n = (int)strlen(out);
  while (n > 3 && out[n - 1] == '\\') out[--n] = 0;
}

/* sessoes=CAMINHO no AGENT.CFG troca a pasta das sessoes. */
static void le_cfg_sessoes(void) {
  char buf[2048];
  int n, i = 0;
  if (!g_cfg.path[0]) return;
  n = k_fload(g_cfg.path, buf, (int)sizeof(buf) - 1);
  if (n <= 0) return;
  if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
  buf[n] = 0;
  while (buf[i]) {
    char* l = buf + i;
    int e = i;
    while (buf[e] && buf[e] != '\n') e++;
    if (buf[e]) buf[e++] = 0;
    i = e;
    while (*l == ' ') l++;
    if (!strncmp(l, "sessoes", 7)) {
      char* v = l + 7;
      int m;
      while (*v == ' ') v++;
      if (*v != '=') continue;
      v++;
      while (*v == ' ') v++;
      m = (int)strlen(v);
      while (m > 0 && (v[m - 1] == ' ' || v[m - 1] == '\r')) v[--m] = 0;
      while (m > 3 && v[m - 1] == '\\') v[--m] = 0;
      if (m > 0) snprintf(g_sesdir, sizeof(g_sesdir), "%s", v);
    }
  }
}

/* "23-11-26-191530" -> "23/11/26 19:15:30" ("-2" no fim vira " #2"). */
static void nome_bonito(const char* nm, char* out, int cap) {
  char t[48];
  int i, o = 0;
  if ((int)strlen(nm) < 15 || nm[2] != '-' || nm[5] != '-' || nm[8] != '-') {
    snprintf(out, (size_t)cap, "%s", nm);
    return;
  }
  t[o++] = nm[0];
  t[o++] = nm[1];
  t[o++] = '/';
  t[o++] = nm[3];
  t[o++] = nm[4];
  t[o++] = '/';
  t[o++] = nm[6];
  t[o++] = nm[7];
  t[o++] = ' ';
  t[o++] = nm[9];
  t[o++] = nm[10];
  t[o++] = ':';
  t[o++] = nm[11];
  t[o++] = nm[12];
  t[o++] = ':';
  t[o++] = nm[13];
  t[o++] = nm[14];
  if (nm[15] == '-') {
    t[o++] = ' ';
    t[o++] = '#';
    for (i = 16; nm[i] && o < 40; i++) t[o++] = nm[i];
  }
  t[o] = 0;
  snprintf(out, (size_t)cap, "%s", t);
}

/* Chave para ordenar: aammddhhmmss + sufixo de 2 digitos ("" se o nome
 * nao segue o padrao: vai para o fim da lista). */
static void chave_de(const char* nm, char* k) {
  int i, s = 1;
  k[0] = 0;
  if ((int)strlen(nm) < 15 || nm[2] != '-' || nm[5] != '-' || nm[8] != '-') return;
  k[0] = nm[6];
  k[1] = nm[7];
  k[2] = nm[3];
  k[3] = nm[4];
  k[4] = nm[0];
  k[5] = nm[1];
  for (i = 0; i < 6; i++) k[6 + i] = nm[9 + i];
  if (nm[15] == '-') {
    s = 0;
    for (i = 16; nm[i] >= '0' && nm[i] <= '9'; i++) s = s * 10 + (nm[i] - '0');
  }
  k[12] = (char)('0' + (s / 10) % 10);
  k[13] = (char)('0' + s % 10);
  k[14] = 0;
}

/* Sessao nova: nome dd-mm-aa-hhmmss pela data e hora de agora. O arquivo so
 * e gravado quando a conversa tiver a primeira pergunta. */
static void sessao_nova(void) {
  char base[24], nome[48];
  int d = k_date(), t = k_time(), n;
  if (d < 0) d = 0;
  if (t < 0) t = 0;
  snprintf(base, sizeof(base), "%02d-%02d-%02d-%06d", d % 100, d / 100 % 100,
           d / 10000 % 100, t);
  for (n = 1; n < 100; n++) {
    if (n == 1)
      snprintf(g_sesname, sizeof(g_sesname), "%s", base);
    else
      snprintf(g_sesname, sizeof(g_sesname), "%s-%d", base, n);
    snprintf(nome, sizeof(nome), "%s.SES", g_sesname);
    junta(g_sespath, (int)sizeof(g_sespath), g_sesdir, nome);
    if (k_fsize(g_sespath) < 0) break;
  }
  g_dirty = 0;
}

/* Arquivo da sessao (texto):
 *   AGENT2 SESSAO 1
 *   data=23/11/26 19:15:30
 *   modelo=deepseek-chat
 *   arquivo=D:\X\Y.C          (um por anexo)
 *   pasta=D:\X\SRC
 *   #M <papel> <bytes>        (uma por mensagem, texto UTF-8 logo abaixo)
 *   <texto>
 * Devolve 0 (gravou ou nada a gravar) ou -1. */
static int sessao_grava(void) {
  char* buf;
  char bon[48];
  int cap, n = 0, i, rc, tem = 0;
  if (!g_dirty) return 0;
  for (i = 0; i < g_n; i++)
    if (g_msgs[i].role == ROLE_ME) tem = 1;
  if (!tem) return 0;
  cap = 512 + g_nanx * 200;
  for (i = 0; i < g_n; i++)
    if (g_msgs[i].role != ROLE_INFO) cap += (int)strlen(g_msgs[i].text) + 32;
  buf = (char*)malloc((size_t)cap);
  if (!buf) return -1;
  nome_bonito(g_sesname, bon, (int)sizeof(bon));
  n += snprintf(buf + n, (size_t)(cap - n), "AGENT2 SESSAO 1\ndata=%s\nmodelo=%s\n",
                bon, g_cfg.model);
  for (i = 0; i < g_nanx; i++)
    n += snprintf(buf + n, (size_t)(cap - n), "%s=%s\n",
                  g_anx[i].dir ? "pasta" : "arquivo", g_anx[i].path);
  for (i = 0; i < g_n; i++) {
    int len;
    if (g_msgs[i].role == ROLE_INFO) continue;
    len = (int)strlen(g_msgs[i].text);
    n += snprintf(buf + n, (size_t)(cap - n), "#M %d %d\n", g_msgs[i].role, len);
    memcpy(buf + n, g_msgs[i].text, (size_t)len);
    n += len;
    buf[n++] = '\n';
  }
  cria_pastas(g_sesdir);
  rc = k_fsave(g_sespath, buf, n);
  free(buf);
  if (rc != 0) return -1;
  g_dirty = 0;
  return 0;
}

/* Proximo item do arquivo da sessao a partir de *pp: 'M' mensagem (papel,
 * texto e tamanho), 'A' arquivo anexado, 'P' pasta anexada; 0 no fim. */
static int sess_item(const char** pp, const char* end, int* role, const char** v, int* n) {
  const char* p = *pp;
  while (p < end) {
    const char* nl = p;
    int ll;
    while (nl < end && *nl != '\n') nl++;
    ll = (int)(nl - p);
    if (ll > 3 && p[0] == '#' && p[1] == 'M' && p[2] == ' ') {
      const char* q = p + 3;
      int r = 0, len = 0;
      while (q < nl && *q >= '0' && *q <= '9') r = r * 10 + (*q++ - '0');
      while (q < nl && *q == ' ') q++;
      while (q < nl && *q >= '0' && *q <= '9') len = len * 10 + (*q++ - '0');
      q = nl < end ? nl + 1 : end;
      if (len > (int)(end - q)) len = (int)(end - q);
      *role = r;
      *v = q;
      *n = len;
      q += len;
      if (q < end && *q == '\n') q++;
      *pp = q;
      return 'M';
    }
    if ((ll > 8 && !memcmp(p, "arquivo=", 8)) || (ll > 6 && !memcmp(p, "pasta=", 6))) {
      int tipo = p[0] == 'a' ? 'A' : 'P';
      int k = tipo == 'A' ? 8 : 6;
      *v = p + k;
      *n = ll - k;
      if (*n > 0 && p[ll - 1] == '\r') (*n)--;
      *pp = nl < end ? nl + 1 : end;
      return tipo;
    }
    p = nl < end ? nl + 1 : end;
  }
  *pp = end;
  return 0;
}

/* Poe um anexo na lista: 0 pos, 1 ja estava, -1 cheio ou vazio. */
static int anexo_poe(const char* v, int len, int dir) {
  int i;
  if (len <= 0) return -1;
  if (len > (int)sizeof(g_anx[0].path) - 1) len = (int)sizeof(g_anx[0].path) - 1;
  for (i = 0; i < g_nanx; i++)
    if ((int)strlen(g_anx[i].path) == len && !memcmp(g_anx[i].path, v, (size_t)len)) return 1;
  if (g_nanx >= ANX_MAX) return -1;
  memcpy(g_anx[g_nanx].path, v, (size_t)len);
  g_anx[g_nanx].path[len] = 0;
  g_anx[g_nanx].dir = dir;
  g_nanx++;
  return 0;
}

/* Le a sessao do arquivo para a memoria. 0 ok; <0 erro (nada muda). */
static int sessao_carrega(const char* path) {
  char* buf;
  const char* p;
  const char* end;
  const char* v;
  int sz, n, tipo, role, len;
  sz = k_fsize(path);
  if (sz <= 0) return -1;
  buf = (char*)malloc((size_t)sz + 1);
  if (!buf) return -1;
  n = k_fload(path, buf, sz + 1);
  if (n <= 0) {
    free(buf);
    return -1;
  }
  if (n > sz) n = sz;
  buf[n] = 0;
  if (strncmp(buf, "AGENT2 SESSAO", 13)) {
    free(buf);
    return -2;
  }
  g_n = 0;
  g_nanx = 0;
  p = buf;
  end = buf + n;
  while ((tipo = sess_item(&p, end, &role, &v, &len)) != 0) {
    if (tipo == 'M') {
      if (role >= ROLE_AI && role <= ROLE_OBS) add_msg_n(role, v, len);
    } else {
      anexo_poe(v, len, tipo == 'P');
    }
  }
  free(buf);
  g_dirty = 0;
  return 0;
}

/* ==================== anexos no system prompt ==================== */
struct bufx {
  char* p;
  int n;
  int cap;
};

static void bx_add(struct bufx* b, const char* s, int n) {
  if (n < 0) n = (int)strlen(s);
  if (n > b->cap - 1 - b->n) n = b->cap - 1 - b->n;
  if (n <= 0) return;
  memcpy(b->p + b->n, s, (size_t)n);
  b->n += n;
  b->p[b->n] = 0;
}

static int ext_binaria(const char* nome) {
  static const char* const ex[] = {
    "EXE", "OBJ", "DLL", "BIN", "IMG", "ISO", "ZIP", "PNG", "JPG", "BMP",
    "GIF", "ICN", "FNT", "SYS", "COM", "LIB", "DAT", "MP3", "WAV", "PDF",
    "SES", 0
  };
  const char* p = strrchr(nome, '.');
  int i;
  if (!p) return 0;
  p++;
  for (i = 0; ex[i]; i++)
    if (igual_sem_caixa(p, ex[i])) return 1;
  return 0;
}

/* Poe ate max bytes do arquivo (sem os \r). 0 pos, 1 binario, 2 sem espaco,
 * -1 nao leu. */
static int bx_arquivo(struct bufx* b, const char* path, int max) {
  int sz = k_fsize(path), n, i, ruins = 0;
  char* t;
  if (sz < 0) return -1;
  if (max > b->cap - 1 - b->n) max = b->cap - 1 - b->n;
  if (max <= 0) return 2;
  n = sz < max ? sz : max;
  if (n == 0) return 0;
  t = (char*)malloc((size_t)n + 1);
  if (!t) return -1;
  n = k_fread(path, 0, t, n);
  if (n < 0) {
    free(t);
    return -1;
  }
  for (i = 0; i < n; i++) {
    unsigned c = (unsigned char)t[i];
    if (c == 0) {
      ruins = n;
      break;
    }
    if (c < 32 && c != '\n' && c != '\r' && c != '\t' && c != 12 && c != 26) ruins++;
  }
  if (ruins * 20 > n) {
    free(t);
    return 1;
  }
  for (i = 0; i < n; i++)
    if (t[i] != '\r') bx_add(b, t + i, 1);
  free(t);
  if (sz > n) bx_add(b, "\n[... resto do arquivo cortado ...]", -1);
  bx_add(b, "\n", 1);
  return 0;
}

static void bx_pasta(struct bufx* b, const char* dir) {
  struct k_dirent* de;
  char ln[240];
  char path[240];
  int m, i, rc;
  snprintf(ln, sizeof(ln), "\n--- pasta anexada %s ---\n", dir);
  bx_add(b, ln, -1);
  de = (struct k_dirent*)malloc(sizeof(struct k_dirent) * 200);
  if (!de) {
    bx_add(b, "(sem memoria para listar)\n", -1);
    return;
  }
  m = k_dir(dir, de, 200);
  if (m < 0) {
    bx_add(b, "(nao consegui listar a pasta)\n", -1);
    free(de);
    return;
  }
  for (i = 0; i < m; i++) {
    if (de[i].dir)
      snprintf(ln, sizeof(ln), "  %s\\\n", de[i].name);
    else
      snprintf(ln, sizeof(ln), "  %s  %u bytes\n", de[i].name, de[i].size);
    bx_add(b, ln, -1);
  }
  for (i = 0; i < m; i++) {
    if (de[i].dir || ext_binaria(de[i].name)) continue;
    if (b->cap - 1 - b->n < 600) {
      bx_add(b, "[... sem espaco para o conteudo dos outros arquivos ...]\n", -1);
      break;
    }
    junta(path, (int)sizeof(path), dir, de[i].name);
    snprintf(ln, sizeof(ln), "\n--- %s (%u bytes) ---\n", path, de[i].size);
    bx_add(b, ln, -1);
    rc = bx_arquivo(b, path, ANX_NAPASTA);
    if (rc == 1) bx_add(b, "(binario, omitido)\n", -1);
    else if (rc < 0) bx_add(b, "(nao consegui ler)\n", -1);
  }
  free(de);
}

/* System prompt da chamada (UTF-8, malloc): contexto + anexos lidos agora. */
static char* monta_sistema(void) {
  struct bufx b;
  char ln[240];
  char* u;
  int i, rc, ucap;
  build_sys();
  b.cap = (int)strlen(g_sys) + 1024 + (g_nanx ? ANX_TOTAL : 0);
  b.p = (char*)malloc((size_t)b.cap);
  if (!b.p) return 0;
  b.n = 0;
  b.p[0] = 0;
  bx_add(&b, g_sys, -1);
  if (g_nanx > 0) {
    bx_add(&b, "\n=== ANEXOS: arquivos e pastas que o usuario anexou a esta "
               "conversa (conteudo lido agora) ===\n", -1);
    for (i = 0; i < g_nanx; i++) {
      if (g_anx[i].dir) {
        bx_pasta(&b, g_anx[i].path);
        continue;
      }
      snprintf(ln, sizeof(ln), "\n--- arquivo anexado %s ---\n", g_anx[i].path);
      bx_add(&b, ln, -1);
      rc = bx_arquivo(&b, g_anx[i].path, ANX_ARQ);
      if (rc == 1) bx_add(&b, "(binario, omitido)\n", -1);
      else if (rc == 2) bx_add(&b, "(sem espaco)\n", -1);
      else if (rc < 0) bx_add(&b, "(nao consegui ler)\n", -1);
    }
    bx_add(&b, "\n=== fim dos anexos ===\n", -1);
  }
  ucap = b.n * 2 + 1;
  u = (char*)malloc((size_t)ucap);
  if (u) utf8_seguro(u, ucap, b.p, b.n);
  free(b.p);
  return u;
}

/* Resumo dos anexos para a linha de separacao: " anexos: A.C, SRC\ ". */
static void anexos_resumo(char* out, int cap) {
  int i, n;
  n = snprintf(out, (size_t)cap, " anexos: ");
  for (i = 0; i < g_nanx && n < cap - 1; i++) {
    const char* p = g_anx[i].path;
    const char* b = strrchr(p, '\\');
    int l = (int)strlen(p);
    int barra = g_anx[i].dir && l > 0 && p[l - 1] != '\\';
    if (b && b[1]) b++;
    else b = p;
    n += snprintf(out + n, (size_t)(cap - n), "%s%s%s", i ? ", " : "", b, barra ? "\\" : "");
  }
  if (n > cap - 2) n = cap - 2;
  out[n++] = ' ';
  out[n] = 0;
}

static void anexa(const char* path, int dir) {
  char m[300];
  int rc = anexo_poe(path, (int)strlen(path), dir);
  if (rc < 0) {
    set_status("limite de anexos (8): F5 tira os anexos");
    return;
  }
  if (rc > 0) {
    set_status("ja estava anexado");
    return;
  }
  snprintf(m, sizeof(m), "anexou %s %s", dir ? "a pasta" : "o arquivo", path);
  add_msg_cp850(ROLE_SYS, m);
  set_status("anexado: vai junto com as proximas perguntas");
}

/* ==================== ferramentas ==================== */
struct tool {
  int kind;
  char arg[128];
  char body[1600];
};

static int dsml_param(const char* a, const char* b, const char* name, char* out, int cap) {
  int nl = (int)strlen(name);
  const char* p = a;
  while (p < b) {
    const char* q = strstr(p, "name=\"");
    const char* v, * e;
    int n;
    if (!q || q >= b) return 0;
    q += 6;
    if (q + nl <= b && !memcmp(q, name, (size_t)nl) && q[nl] == '"') {
      v = q + nl + 1;
      while (v < b && *v != '>') v++;
      if (v >= b) return 0;
      v++;
      e = v;
      while (e < b && !(*e == '<' && e + 1 < b && e[1] == '/')) e++;
      n = (int)(e - v);
      if (n > cap - 1) n = cap - 1;
      ai_utf8_to_cp850(out, cap, v, n);
      return 1;
    }
    p = q + 1;
  }
  return 0;
}

static int parse_tool(const char* s, const char** next, struct tool* t) {
  t->kind = 0;
  t->arg[0] = 0;
  t->body[0] = 0;
  {
    const char* inv = strstr(s, "DSML");
    if (inv) {
      const char* nm = strstr(inv, "invoke name=\"");
      const char* ce = strstr(inv, "calls>");
      const char* end = ce ? ce + 6 : s + strlen(s);
      if (nm) {
        nm += 13;
        if (!strncmp(nm, "write", 5) || !strncmp(nm, "create", 6)) {
          if (dsml_param(inv, end, "path", t->arg, sizeof(t->arg)) ||
              dsml_param(inv, end, "filename", t->arg, sizeof(t->arg))) {
            if (!dsml_param(inv, end, "content", t->body, sizeof(t->body)))
              if (!dsml_param(inv, end, "text", t->body, sizeof(t->body)))
                dsml_param(inv, end, "body", t->body, sizeof(t->body));
            t->kind = 'w';
            if (next) *next = end;
            return 1;
          }
        } else if (!strncmp(nm, "open", 4) || !strncmp(nm, "launch", 6) ||
                   !strncmp(nm, "run", 3)) {
          if (dsml_param(inv, end, "path", t->arg, sizeof(t->arg)) ||
              dsml_param(inv, end, "file", t->arg, sizeof(t->arg)) ||
              dsml_param(inv, end, "program", t->arg, sizeof(t->arg))) {
            t->kind = 'o';
            if (next) *next = end;
            return 1;
          }
        } else if (!strncmp(nm, "cmd", 3) || !strncmp(nm, "shell", 5) ||
                   !strncmp(nm, "exec", 4)) {
          if (dsml_param(inv, end, "command", t->arg, sizeof(t->arg)) ||
              dsml_param(inv, end, "cmd", t->arg, sizeof(t->arg)) ||
              dsml_param(inv, end, "line", t->arg, sizeof(t->arg))) {
            t->kind = 'c';
            if (next) *next = end;
            return 1;
          }
        }
      }
    }
  }
  {
    const char* p = strstr(s, "```");
    while (p) {
      const char* tag = p + 3;
      const char* nl = strchr(tag, '\n');
      if (!nl) break;
      if (!strncmp(tag, "cmd", 3)) {
        const char* a = nl + 1;
        const char* e = strchr(a, '\n');
        int n = e ? (int)(e - a) : (int)strlen(a);
        if (n > (int)sizeof(t->arg) - 1) n = (int)sizeof(t->arg) - 1;
        ai_utf8_to_cp850(t->arg, sizeof(t->arg), a, n);
        t->kind = 'c';
        if (next) {
          const char* q = strstr(p + 3, "```");
          *next = q ? q + 3 : p + strlen(p);
        }
        return 1;
      }
      if (!strncmp(tag, "open ", 5)) {
        const char* a = tag + 5;
        const char* e = (nl > a) ? nl : a;
        int n = (int)(e - a);
        if (n > (int)sizeof(t->arg) - 1) n = (int)sizeof(t->arg) - 1;
        ai_utf8_to_cp850(t->arg, sizeof(t->arg), a, n);
        t->kind = 'o';
        if (next) {
          const char* q = strstr(p + 3, "```");
          *next = q ? q + 3 : p + strlen(p);
        }
        return 1;
      }
      if (!strncmp(tag, "write ", 6)) {
        const char* a = tag + 6;
        const char* e = (nl > a) ? nl : a;
        int n = (int)(e - a);
        const char* b = nl + 1;
        const char* c = strstr(b, "```");
        int bn = c ? (int)(c - b) : (int)strlen(b);
        if (n > (int)sizeof(t->arg) - 1) n = (int)sizeof(t->arg) - 1;
        if (bn > (int)sizeof(t->body) - 1) bn = (int)sizeof(t->body) - 1;
        ai_utf8_to_cp850(t->arg, sizeof(t->arg), a, n);
        ai_utf8_to_cp850(t->body, sizeof(t->body), b, bn);
        t->kind = 'w';
        if (next) {
          const char* q = strstr(p + 3, "```");
          *next = q ? q + 3 : p + strlen(p);
        }
        return 1;
      }
      p = nl + 1;
      {
        const char* q = strstr(p, "```");
        if (!q) break;
        p = q + 3;
      }
    }
  }
  return 0;
}

static void run_tool(struct tool* t, char* obs, int cap, char* label, int lcap) {
  if (t->kind == 'w') {
    int rc = k_fsave(t->arg, t->body, (int)strlen(t->body));
    snprintf(obs, (size_t)cap, "write %s: %d bytes, result %d (%s)", t->arg,
             (int)strlen(t->body), rc, rc == 0 ? "ok" : "falhou");
    snprintf(label, (size_t)lcap, "gravou %s (%d bytes)", t->arg, (int)strlen(t->body));
    return;
  }
  if (t->kind == 'o') {
    int rc = k_win_launch(t->arg);
    snprintf(obs, (size_t)cap, "open %s: %d (%s)", t->arg, rc,
             rc == 0 ? "pedido aceito" : "falhou");
    snprintf(label, (size_t)lcap, "abriu %s", t->arg);
    return;
  }
  if (t->kind == 'c') {
    char* out = (char*)malloc(16384);
    int len = 0, rc;
    if (!out) {
      snprintf(obs, (size_t)cap, "sem memoria");
      return;
    }
    rc = k_shell_capture(t->arg, out, 16384, &len);
    if (len < 0) len = -len;
    snprintf(obs, (size_t)cap, "exit %d, saida:\n%s", rc, out);
    snprintf(label, (size_t)lcap, "rodou %s (exit %d)", t->arg, rc);
    free(out);
    return;
  }
  snprintf(obs, (size_t)cap, "nenhuma acao");
}

static void strip_tool_markup(const char* s, char* out, int cap) {
  int i = 0, o = 0, hide = 0;
  while (s[i] && o < cap - 1) {
    if (hide) {
      if (s[i] == '>' && i >= 5 && !memcmp(s + i - 5, "calls", 5)) hide = 0;
      i++;
      continue;
    }
    if (s[i] == '<' && (s[i + 1] == '|' || s[i + 1] == '?')) {
      hide = 1;
      i += 2;
      continue;
    }
    if (s[i] == '`' && s[i + 1] == '`' && s[i + 2] == '`') {
      const char* close = strstr(s + i + 3, "```");
      if (!close) break;
      i = (int)(close - s) + 3;
      if (s[i] == '\n') i++;
      continue;
    }
    out[o++] = s[i++];
  }
  out[o] = 0;
}

/* ==================== desenho da conversa ==================== */
static char g_disp[MSG_LEN * 2];
static int g_ls[WRAP_MAX];
static int g_ll[WRAP_MAX];

static void menu_topo(void) {
  char buf[64];
  char bon[48];
  int t;
  linha(0, 0, COLS, ' ', C_MENU);
  texto(0, 1, "AGENT2", COR(CIANO, BRANCO));
  texto(0, 9, " Assistente SODOS ", C_MENUSEL);
  if (COLS >= 70 && g_sesname[0]) {
    nome_bonito(g_sesname, bon, (int)sizeof(bon));
    snprintf(buf, sizeof(buf), "sessao %s", bon);
    texto(0, 29, buf, C_MENU);
  }
  t = k_time();
  if (t >= 0) {
    sprintf(buf, "%02d:%02d:%02d UTC", t / 10000, t / 100 % 100, t % 100);
    texto(0, COLS - 14, buf, C_MENU);
  }
}

/* Converte a mensagem i (ou o streaming, i < 0) para CP850 em g_disp e
 * quebra em linhas de cpl colunas (g_ls/g_ll). Devolve quantas linhas. */
static int bloco(int i, int cpl) {
  const char* s;
  if (i < 0)
    s = g_stream_len ? g_stream : "...";
  else
    s = g_msgs[i].text;
  ai_utf8_to_cp850(g_disp, (int)sizeof(g_disp), s, (int)strlen(s));
  return wrap_lines(g_disp, cpl, g_ls, g_ll, WRAP_MAX);
}

/* Linhas da mensagem i, com cache (so recalcula se a largura mudou). */
static int bloco_nl(int i, int cpl) {
  if (g_msgs[i].nl_cpl != cpl) {
    g_msgs[i].nl = bloco(i, cpl);
    g_msgs[i].nl_cpl = cpl;
  }
  return g_msgs[i].nl;
}

static int msg_cor(int role) {
  if (role == ROLE_ME) return C_ME;
  if (role == ROLE_SYS || role == ROLE_INFO) return C_SYS;
  return C_AI;
}

static const char* msg_rotulo(int role) {
  if (role == ROLE_ME) return "voce";
  if (role == ROLE_SYS || role == ROLE_INFO) return "sistema";
  return "SODOS";
}

static void desenha(void) {
  static const char* const rotulos[10] = {
    "Ajuda", "Sessoes", "Novo", "Anexar", "TiraAnx", "", "", "", "", "Sair"
  };
  int r, c, w, h, i, ytop, ybot, cpl, vis, total, nb, first, yoff, ln, maxs;
  int fw, ini;
  char info[200];

  fundo();
  menu_topo();
  teclas(rotulos);
  r = 1;
  c = 0;
  h = ROWS - 3;
  w = COLS;
  janela(r, c, h, w, g_nav ? " Conversa - navegando " : " Conversa ");

  ytop = r + 1;
  ybot = r + h - 4;
  vis = ybot - ytop + 1;
  if (vis < 1) vis = 1;
  g_vis = vis;
  cpl = w - 8;
  if (cpl < 10) cpl = 10;

  /* 1a passada: quantas linhas a conversa inteira ocupa. Cada bloco e o
   * rotulo + as linhas do texto, com uma linha vazia entre os blocos. */
  total = 0;
  nb = 0;
  for (i = 0; i < g_n; i++) {
    if (g_msgs[i].role == ROLE_OBS) continue;
    total += 1 + bloco_nl(i, cpl);
    nb++;
  }
  if (g_waiting) {
    total += 1 + bloco(-1, cpl);
    nb++;
  }
  if (nb > 1) total += nb - 1;

  maxs = total - vis;
  if (maxs < 0) maxs = 0;
  if (g_scroll > maxs) g_scroll = maxs;
  if (g_scroll < 0) g_scroll = 0;
  first = total - vis - g_scroll;
  if (first < 0) first = 0;
  yoff = total < vis ? vis - total : 0;   /* pouca conversa: fica embaixo */

  /* 2a passada: desenha so as linhas que caem na janela */
  ln = 0;
  nb = 0;
  for (i = 0; i <= g_n && ln < first + vis; i++) {
    int k, nl, cor, role;
    if (i == g_n) {
      if (!g_waiting) break;
      role = ROLE_AI;
    } else {
      if (g_msgs[i].role == ROLE_OBS) continue;
      role = g_msgs[i].role;
    }
    if (nb > 0) ln++;
    nb++;
    nl = i == g_n ? bloco(-1, cpl) : bloco_nl(i, cpl);
    if (ln + 1 + nl <= first) {   /* bloco todo acima da janela */
      ln += 1 + nl;
      continue;
    }
    if (i < g_n) nl = bloco(i, cpl);   /* enche g_disp/g_ls/g_ll */
    cor = msg_cor(role);
    if (ln >= first && ln < first + vis)
      texto(ytop + yoff + ln - first, c + 2, msg_rotulo(role), C_FRACO);
    ln++;
    k = 0;
    while (k < nl) {
      if (ln >= first && ln < first + vis) {
        int y = ytop + yoff + ln - first;
        int n = g_ll[k];
        if (n > cpl) n = cpl;
        textow(y, c + 2, g_disp + g_ls[k], n, cor);
        if (n < cpl) linha(y, c + 2 + n, cpl - n, ' ', cor);
      }
      k++;
      ln++;
    }
  }

  separador(r + h - 3, c, w);
  if (g_nanx > 0) {
    int mw = w / 2 - 2;
    anexos_resumo(info, (int)sizeof(info));
    if (mw > (int)sizeof(info) - 1) mw = (int)sizeof(info) - 1;
    if (tam(info) > mw) info[mw] = 0;
    texto(r + h - 3, c + 2, info, C_TITULO);
  }
  if (g_nav) {
    if (g_scroll > 0)
      snprintf(info, sizeof(info), " %d linhas acima do fim | Espaco: digitar ", g_scroll);
    else
      snprintf(info, sizeof(info), " fim da conversa | Espaco: digitar ");
    texto(r + h - 3, c + w - 2 - tam(info), info, C_TITULO);
  }

  /* campo de digitacao: se o texto passa da largura, mostra o fim */
  fw = w - 12;
  if (fw < 4) fw = 4;
  ini = g_edit_len > fw - 1 ? g_edit_len - (fw - 1) : 0;
  texto(r + h - 2, c + 2, "voce>", g_nav ? C_FRACO : C_FORTE);
  textow(r + h - 2, c + 8, g_edit + ini, fw, C_JANELA);
  if (!g_busy && !g_nav) poe(r + h - 2, c + 8 + g_edit_len - ini, '_', C_FORTE);
  texto(r + h - 1, c + 2, g_status, C_FRACO);
  tela_mostra();
}

/* ==================== API ==================== */
static void on_token(void* ud, const char* s, int n) {
  (void)ud;
  if (n <= 0) return;
  if (g_stream_len + n < MSG_LEN - 1) {
    memcpy(g_stream + g_stream_len, s, (size_t)n);
    g_stream_len += n;
    g_stream[g_stream_len] = 0;
  }
  if (g_waiting) desenha();
}

static int call_api(void) {
  ai_msg_t* list;
  char* sys;
  int i, k = 0, rc;
  g_stream_len = 0;
  g_stream[0] = 0;
  sys = monta_sistema();
  if (!sys) {
    set_status("sem memoria");
    return -1;
  }
  list = (ai_msg_t*)malloc((size_t)(g_n + 1) * sizeof(ai_msg_t));
  if (!list) {
    free(sys);
    set_status("sem memoria");
    return -1;
  }
  list[k].role = "system";
  list[k].text = sys;
  k++;
  for (i = 0; i < g_n; i++) {
    if (g_msgs[i].role == ROLE_SYS || g_msgs[i].role == ROLE_INFO) continue;
    list[k].role = (g_msgs[i].role == ROLE_ME || g_msgs[i].role == ROLE_OBS)
                       ? "user" : "assistant";
    list[k].text = g_msgs[i].text;
    k++;
  }
  rc = ai_chat_tools(&g_cfg, list, k, on_token, 0, g_tools, 4);
  free(list);
  free(sys);
  return rc;
}

static void grava_e_avisa(void) {
  char st[200];
  if (sessao_grava() != 0) {
    snprintf(st, sizeof(st), "nao consegui gravar a sessao em %s", g_sespath);
    set_status(st);
  }
}

static void send_now(void) {
  char q[MSG_LEN];
  char obs[1200];
  char label[160];
  char shown[MSG_LEN * 2];
  struct tool tl;
  const char* cur;
  int step;

  ai_cp850_to_utf8(q, (int)sizeof(q), g_edit, (int)strlen(g_edit));
  if (!q[0]) {
    set_status("escreva uma mensagem");
    return;
  }
  if (!g_cfg.key[0]) {
    set_status("falta key no AGENT.CFG");
    return;
  }
  add_msg(ROLE_ME, q);
  g_edit[0] = 0;
  g_edit_len = 0;
  g_busy = 1;
  g_nav = 0;
  g_scroll = 0;

  for (step = 0; step < STEPS_MAX; step++) {
    int rc;
    g_waiting = 1;
    set_status(step ? "executando e perguntando de novo..." : "esperando a API...");
    desenha();
    rc = call_api();
    if (rc < 0) {
      char st[200];
      if (g_stream_len > 0) {
        strip_tool_markup(g_stream, shown, (int)sizeof(shown));
        if (shown[0]) add_msg(ROLE_AI, shown);
      }
      snprintf(st, sizeof(st), "[%s]",
               ai_last_error()[0] ? ai_last_error() : "sem resposta");
      add_msg(ROLE_SYS, st);
      set_status(ai_last_error());
      g_waiting = 0;
      g_busy = 0;
      grava_e_avisa();
      return;
    }
    strip_tool_markup(g_stream, shown, (int)sizeof(shown));
    if (shown[0]) add_msg(ROLE_AI, shown);
    if (rc == 0) {
      cur = g_stream;
      if (!parse_tool(cur, &cur, &tl)) break;
      obs[0] = 0;
      label[0] = 0;
      run_tool(&tl, obs, (int)sizeof(obs), label, (int)sizeof(label));
      if (label[0]) add_msg_cp850(ROLE_SYS, label);
      add_msg_cp850(ROLE_OBS, obs);
    } else {
      int ti;
      for (ti = 0; ti < rc; ti++) {
        memset(&tl, 0, sizeof(tl));
        tl.kind = g_tools[ti].kind;
        snprintf(tl.arg, sizeof(tl.arg), "%s", g_tools[ti].arg);
        snprintf(tl.body, sizeof(tl.body), "%s", g_tools[ti].body);
        obs[0] = 0;
        label[0] = 0;
        run_tool(&tl, obs, (int)sizeof(obs), label, (int)sizeof(label));
        if (label[0]) add_msg_cp850(ROLE_SYS, label);
        add_msg_cp850(ROLE_OBS, obs);
      }
    }
  }
  if (step == STEPS_MAX) add_msg(ROLE_SYS, "[limite de acoes neste pedido]");
  g_busy = 0;
  g_waiting = 0;
  g_stream_len = 0;
  g_stream[0] = 0;
  set_status("ok");
  grava_e_avisa();
}

/* ==================== entrada ==================== */
static void edit_key(int k) {
  if (k == 8 || k == 127) {
    if (g_edit_len > 0) g_edit[--g_edit_len] = 0;
  } else if (k >= 32 && k < 256 && g_edit_len < MSG_LEN - 1) {
    g_edit[g_edit_len++] = (char)k;
    g_edit[g_edit_len] = 0;
  }
}

static void ajuda(void) {
  int r, c, h, w, i, nl;
  char ln[200];
  static const char* linhas[] = {
    "AGENT2 - assistente de IA do SODOS",
    "",
    "Escreva o pedido e tecle Enter. O modelo pode rodar",
    "comandos, gravar arquivos e abrir programas.",
    "",
    "Setas, PgUp/PgDn, Home/End: navegam na conversa",
    "Espaco (ou ESC/Enter) volta para a digitacao",
    "",
    "F2 sessoes salvas (lista e previa)   F3 conversa nova",
    "F4 anexa arquivo ou pasta   F5 tira os anexos",
    "F1 ajuda   F10 sai (a sessao fica salva)",
    "",
    "Cada conversa vira SESSOES\\dd-mm-aa-hhmmss.SES"
  };
  nl = (int)(sizeof(linhas) / sizeof(linhas[0]));
  h = nl + 7;
  w = 62;
  if (w > COLS - 4) w = COLS - 4;
  if (h > ROWS - 2) h = ROWS - 2;
  r = (ROWS - 1 - h) / 2;
  c = (COLS - w) / 2;
  memcpy(copia, tela, sizeof tela);
  janela(r, c, h, w, " Ajuda ");
  for (i = 0; i < nl && r + 2 + i < r + h - 4; i++)
    textow(r + 2 + i, c + 3, linhas[i], w - 6, i == 0 ? C_FORTE : C_JANELA);
  snprintf(ln, sizeof(ln), "Pasta: %s", g_sesdir);
  textow(r + h - 4, c + 3, ln, w - 6, C_FRACO);
  separador(r + h - 3, c, w);
  texto(r + h - 2, c + 3, "Enter ou ESC fecha", C_FRACO);
  tela_mostra();
  for (;;) {
    int k = tecla(0);
    if (k == K_ENTER || k == K_ESC || k == K_EOF || k == ' ') break;
  }
  memcpy(tela, copia, sizeof tela);
}

/* ==================== tela de sessoes (F2) ==================== */
#define SES_MAX 400
#define PV_LER 12000

struct sesitem {
  char nome[32];   /* nome do arquivo sem o .SES */
  char chave[16];
};

static char g_pv[8000];   /* previa da sessao marcada (CP850) */

static void pv_add(int* o, const char* s, int n) {
  if (n < 0) n = (int)strlen(s);
  while (n-- > 0 && *o < (int)sizeof(g_pv) - 1) g_pv[(*o)++] = *s++;
  g_pv[*o] = 0;
}

/* Lista os .SES da pasta das sessoes, da mais nova para a mais velha. */
static int lista_sessoes(struct sesitem* L, int max) {
  struct k_dirent* de;
  struct sesitem t;
  int n, i, j, m = 0;
  de = (struct k_dirent*)malloc(sizeof(struct k_dirent) * (size_t)max);
  if (!de) return 0;
  n = k_dir(g_sesdir, de, max);
  for (i = 0; i < n && m < max; i++) {
    int l = (int)strlen(de[i].name);
    if (de[i].dir || l < 5 || l - 4 > 31) continue;
    if (!igual_sem_caixa(de[i].name + l - 4, ".SES")) continue;
    memcpy(L[m].nome, de[i].name, (size_t)(l - 4));
    L[m].nome[l - 4] = 0;
    chave_de(L[m].nome, L[m].chave);
    m++;
  }
  free(de);
  for (i = 1; i < m; i++) {
    t = L[i];
    j = i - 1;
    while (j >= 0 && strcmp(L[j].chave, t.chave) < 0) {
      L[j + 1] = L[j];
      j--;
    }
    L[j + 1] = t;
  }
  return m;
}

/* Monta em g_pv o comeco da sessao: data, anexos e as primeiras mensagens.
 * Linha que comeca com o byte 1 e rotulo (desenhada em destaque). */
static void monta_previa(const char* nome) {
  char fn[48], path[220], tmp[240], bon[48], mk[2];
  char* raw;
  const char* p;
  const char* end;
  const char* v;
  int n, o = 0, tipo, role, len, sz;
  g_pv[0] = 0;
  mk[0] = 1;
  mk[1] = 0;
  snprintf(fn, sizeof(fn), "%s.SES", nome);
  junta(path, (int)sizeof(path), g_sesdir, fn);
  sz = k_fsize(path);
  nome_bonito(nome, bon, (int)sizeof(bon));
  pv_add(&o, mk, 1);
  snprintf(tmp, sizeof(tmp), "Sessao %s\n", bon);
  pv_add(&o, tmp, -1);
  if (sz >= 0) {
    snprintf(tmp, sizeof(tmp), "%s  (%d KB)\n", fn, (sz + 1023) / 1024);
    pv_add(&o, tmp, -1);
  }
  raw = (char*)malloc(PV_LER + 1);
  if (!raw) {
    pv_add(&o, "(sem memoria)\n", -1);
    return;
  }
  n = k_fread(path, 0, raw, PV_LER);
  if (n < 0) n = 0;
  raw[n] = 0;
  if (n == 0 || strncmp(raw, "AGENT2 SESSAO", 13)) {
    pv_add(&o, "(nao consegui ler esta sessao)\n", -1);
    free(raw);
    return;
  }
  end = raw + n;
  p = raw;
  while ((tipo = sess_item(&p, end, &role, &v, &len)) != 0) {
    if (tipo == 'M') continue;
    pv_add(&o, tipo == 'P' ? "pasta anexada: " : "arquivo anexado: ", -1);
    pv_add(&o, v, len);
    pv_add(&o, "\n", 1);
  }
  p = raw;
  while (o < (int)sizeof(g_pv) - 64 && (tipo = sess_item(&p, end, &role, &v, &len)) != 0) {
    if (tipo != 'M' || role == ROLE_OBS) continue;
    pv_add(&o, "\n", 1);
    pv_add(&o, mk, 1);
    pv_add(&o, role == ROLE_ME ? "voce:" : role == ROLE_SYS ? "sistema:" : "SODOS:", -1);
    pv_add(&o, "\n", 1);
    o += ai_utf8_to_cp850(g_pv + o, (int)sizeof(g_pv) - o, v, len);
    pv_add(&o, "\n", 1);
  }
  free(raw);
}

static void carrega_sessao(const char* nome) {
  char fn[48], path[220], bon[48], m[120];
  snprintf(fn, sizeof(fn), "%s.SES", nome);
  junta(path, (int)sizeof(path), g_sesdir, fn);
  grava_e_avisa();   /* a conversa atual fica salva antes */
  if (sessao_carrega(path) != 0) {
    set_status("nao consegui ler essa sessao");
    return;
  }
  snprintf(g_sesname, sizeof(g_sesname), "%s", nome);
  snprintf(g_sespath, sizeof(g_sespath), "%s", path);
  g_scroll = 0;
  g_nav = 0;
  g_edit[0] = 0;
  g_edit_len = 0;
  nome_bonito(nome, bon, (int)sizeof(bon));
  snprintf(m, sizeof(m), "Sessao de %s carregada. Continue a conversa.", bon);
  add_msg(ROLE_INFO, m);
  set_status("sessao carregada");
}

static void tela_sessoes(void) {
  static const char* const rot[10] = {
    "", "", "", "", "", "", "", "Apaga", "", "Volta"
  };
  struct sesitem* L;
  int n, i, sel = 0, top = 0, pvsel = -1, sai = 0;
  L = (struct sesitem*)malloc(sizeof(struct sesitem) * SES_MAX);
  if (!L) {
    set_status("sem memoria");
    return;
  }
  n = lista_sessoes(L, SES_MAX);
  for (i = 0; i < n; i++)
    if (!strcmp(L[i].nome, g_sesname)) sel = i;

  while (!sai) {
    int r = 1, c = 0, h = ROWS - 3, w = COLS, lw = 22, rows, ytop, px, pw, k, nl;
    char ln[240], bon[48];
    fundo();
    menu_topo();
    teclas(rot);
    janela(r, c, h, w, " Sessoes salvas ");
    ytop = r + 1;
    rows = h - 4;
    if (rows < 1) rows = 1;
    px = c + 2 + lw + 3;
    pw = w - (lw + 7);
    if (pw < 8) pw = 8;
    for (i = 0; i < rows; i++) poe(ytop + i, c + 2 + lw + 1, G_DIV, C_BORDA);

    if (n == 0) {
      textow(ytop, c + 2, "(nenhuma)", lw, C_FRACO);
      textow(ytop, px, "Nenhuma sessao salva ainda.", pw, C_JANELA);
      textow(ytop + 2, px, "Elas sao gravadas sozinhas a cada", pw, C_FRACO);
      textow(ytop + 3, px, "resposta, na pasta:", pw, C_FRACO);
      textow(ytop + 4, px, g_sesdir, pw, C_FORTE);
    } else {
      if (sel < 0) sel = 0;
      if (sel > n - 1) sel = n - 1;
      if (sel < top) top = sel;
      if (sel >= top + rows) top = sel - rows + 1;
      for (i = 0; i < rows && top + i < n; i++) {
        int cor = top + i == sel ? C_MENUSEL : C_JANELA;
        nome_bonito(L[top + i].nome, bon, (int)sizeof(bon));
        snprintf(ln, sizeof(ln), "%c%s", strcmp(L[top + i].nome, g_sesname) ? ' ' : '*', bon);
        textow(ytop + i, c + 2, ln, lw, cor);
      }
      if (pvsel != sel) {
        monta_previa(L[sel].nome);
        pvsel = sel;
      }
      nl = wrap_lines(g_pv, pw, g_ls, g_ll, WRAP_MAX);
      for (i = 0; i < rows && i < nl; i++) {
        const char* s = g_pv + g_ls[i];
        int len = g_ll[i];
        int cor = C_JANELA;
        if (len > 0 && s[0] == 1) {
          s++;
          len--;
          cor = C_FORTE;
        }
        if (len > pw) len = pw;
        memcpy(ln, s, (size_t)len);
        ln[len] = 0;
        textow(ytop + i, px, ln, pw, cor);
      }
    }

    separador(r + h - 3, c, w);
    textow(r + h - 2, c + 2, "Enter carrega   Del apaga   Setas/PgUp/PgDn escolhem   ESC volta",
           w - 4, C_FRACO);
    snprintf(ln, sizeof(ln), " %d sessoes em %s ", n, g_sesdir);
    if (tam(ln) > w - 4) ln[w - 4] = 0;
    texto(r + h - 1, c + 2, ln, C_FRACO);
    tela_mostra();

    k = tecla(0);
    if (k == K_NADA) continue;
    if (k == K_EOF || k == K_ESC || k == K_F(10) || k == K_F(2)) break;
    if (n == 0) continue;
    if (k == K_UP) sel--;
    else if (k == K_DOWN) sel++;
    else if (k == K_PGUP) sel -= rows;
    else if (k == K_PGDN) sel += rows;
    else if (k == K_HOME) sel = 0;
    else if (k == K_END) sel = n - 1;
    else if (k == K_ENTER) {
      carrega_sessao(L[sel].nome);
      sai = 1;
    } else if (k == K_DEL || k == K_F(8)) {
      char fn[48], path[220], msg[120];
      nome_bonito(L[sel].nome, bon, (int)sizeof(bon));
      snprintf(msg, sizeof(msg), "Apagar a sessao de %s?", bon);
      if (pergunta(" Apagar sessao ", msg)) {
        snprintf(fn, sizeof(fn), "%s.SES", L[sel].nome);
        junta(path, (int)sizeof(path), g_sesdir, fn);
        if (k_fdelete(path) == 0) {
          n = lista_sessoes(L, SES_MAX);
          pvsel = -1;
          set_status("sessao apagada");
        } else {
          set_status("nao consegui apagar a sessao");
        }
      }
    }
    if (sel < 0) sel = 0;
    if (sel > n - 1) sel = n - 1;
  }
  free(L);
}

/* ==================== Anexar (F4): escolher arquivo ou pasta ==================== */
#define FE_MAX 400
#define FE_ARQ 0
#define FE_PASTA 1
#define FE_ESTA 2    /* "." = anexar a pasta aberta */
#define FE_SOBE 3    /* ".." */
#define FE_DISCO 4   /* "D:\" na lista de unidades */

struct fent {
  char nome[32];
  unsigned tam;
  int tipo;
};

static int fe_ordem(int tipo) {
  if (tipo == FE_ESTA) return 0;
  if (tipo == FE_SOBE) return 1;
  if (tipo == FE_DISCO) return 2;
  if (tipo == FE_PASTA) return 3;
  return 4;
}

static int fe_antes(const struct fent* a, const struct fent* b) {
  int x = fe_ordem(a->tipo), y = fe_ordem(b->tipo);
  if (x != y) return x < y;
  return cmp_sem_caixa(a->nome, b->nome) < 0;
}

/* Pasta de cima; da raiz "D:\" vai para a lista de unidades (""). */
static void sobe_pasta(char* d) {
  int n = (int)strlen(d);
  if (n <= 3) {
    d[0] = 0;
    return;
  }
  while (n > 0 && d[n - 1] != '\\') n--;
  if (n <= 3)
    d[3] = 0;
  else
    d[n - 1] = 0;
}

static int lista_pasta(const char* dir, struct fent* L, int max) {
  struct k_dirent* de;
  struct fent t;
  int n = 0, i, j, m;
  if (!dir[0]) {
    unsigned mask = k_drive_mask();
    for (i = 0; i < 26 && n < max; i++) {
      if (!(mask & (1u << i))) continue;
      snprintf(L[n].nome, sizeof(L[n].nome), "%c:\\", 'A' + i);
      L[n].tam = 0;
      L[n].tipo = FE_DISCO;
      n++;
    }
    return n;
  }
  snprintf(L[0].nome, sizeof(L[0].nome), ".");
  L[0].tam = 0;
  L[0].tipo = FE_ESTA;
  snprintf(L[1].nome, sizeof(L[1].nome), "..");
  L[1].tam = 0;
  L[1].tipo = FE_SOBE;
  n = 2;
  de = (struct k_dirent*)malloc(sizeof(struct k_dirent) * (size_t)(max - 2));
  if (!de) return n;
  m = k_dir(dir, de, max - 2);
  for (i = 0; i < m && n < max; i++) {
    snprintf(L[n].nome, sizeof(L[n].nome), "%s", de[i].name);
    L[n].tam = de[i].size;
    L[n].tipo = de[i].dir ? FE_PASTA : FE_ARQ;
    n++;
  }
  free(de);
  for (i = 3; i < n; i++) {
    t = L[i];
    j = i - 1;
    while (j >= 2 && fe_antes(&t, &L[j])) {
      L[j + 1] = L[j];
      j--;
    }
    L[j + 1] = t;
  }
  return n;
}

static void anexar(void) {
  struct fent* L;
  int n = 0, sel = 0, top = 0, rec = 1, fim = 0;
  char path[220];
  L = (struct fent*)malloc(sizeof(struct fent) * FE_MAX);
  if (!L) {
    set_status("sem memoria");
    return;
  }
  memcpy(copia, tela, sizeof tela);
  while (!fim) {
    int r, c, h, w, rows, i, k;
    struct fent* e;
    char ln[120];
    if (rec) {
      n = lista_pasta(g_brdir, L, FE_MAX);
      sel = 0;
      top = 0;
      rec = 0;
    }
    w = COLS - 8;
    if (w > 72) w = 72;
    h = ROWS - 4;
    if (h > 22) h = 22;
    r = (ROWS - 1 - h) / 2;
    c = (COLS - w) / 2;
    rows = h - 5;
    if (rows < 1) rows = 1;
    if (sel < 0) sel = 0;
    if (sel > n - 1) sel = n - 1;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    if (top < 0) top = 0;

    memcpy(tela, copia, sizeof tela);
    janela(r, c, h, w, " Anexar arquivo ou pasta ");
    textow(r + 1, c + 2, g_brdir[0] ? g_brdir : "Unidades", w - 4, C_FORTE);
    for (i = 0; i < rows && top + i < n; i++) {
      int cor = top + i == sel ? C_MENUSEL : C_JANELA;
      e = &L[top + i];
      if (e->tipo == FE_ESTA)
        snprintf(ln, sizeof(ln), " .   (anexar esta pasta)");
      else if (e->tipo == FE_SOBE)
        snprintf(ln, sizeof(ln), " ..  (pasta de cima)");
      else if (e->tipo == FE_PASTA)
        snprintf(ln, sizeof(ln), " %s\\", e->nome);
      else
        snprintf(ln, sizeof(ln), " %s", e->nome);
      textow(r + 2 + i, c + 2, ln, w - 4, cor);
      if (e->tipo == FE_ARQ && w > 40) {
        snprintf(ln, sizeof(ln), "%u", e->tam);
        texto(r + 2 + i, c + w - 3 - tam(ln), ln, cor);
      }
    }
    separador(r + h - 3, c, w);
    textow(r + h - 2, c + 2, "Enter abre/anexa  Espaco anexa  Backspace sobe  ESC sai",
           w - 4, C_FRACO);
    tela_mostra();

    k = tecla(0);
    if (k == K_NADA) continue;
    if (k == K_EOF || k == K_ESC || k == K_F(10)) break;
    if (k == K_UP) sel--;
    else if (k == K_DOWN) sel++;
    else if (k == K_PGUP) sel -= rows;
    else if (k == K_PGDN) sel += rows;
    else if (k == K_HOME) sel = 0;
    else if (k == K_END) sel = n - 1;
    else if (k == 8 || k == 127) {
      if (g_brdir[0]) {
        sobe_pasta(g_brdir);
        rec = 1;
      }
    } else if ((k == K_ENTER || k == ' ') && n > 0) {
      e = &L[sel];
      if (e->tipo == FE_ESTA) {
        anexa(g_brdir, 1);
        fim = 1;
      } else if (e->tipo == FE_SOBE) {
        if (k == K_ENTER) {
          sobe_pasta(g_brdir);
          rec = 1;
        }
      } else if (e->tipo == FE_DISCO) {
        if (k == K_ENTER) {
          snprintf(g_brdir, sizeof(g_brdir), "%s", e->nome);
          rec = 1;
        }
      } else if (e->tipo == FE_PASTA) {
        junta(path, (int)sizeof(path), g_brdir, e->nome);
        if (k == K_ENTER) {
          snprintf(g_brdir, sizeof(g_brdir), "%s", path);
          rec = 1;
        } else {
          anexa(path, 1);
          fim = 1;
        }
      } else {
        junta(path, (int)sizeof(path), g_brdir, e->nome);
        anexa(path, 0);
        fim = 1;
      }
    }
  }
  memcpy(tela, copia, sizeof tela);
  free(L);
}

static void tira_anexos(void) {
  char msg[80];
  if (g_nanx == 0) {
    set_status("nao ha anexos; F4 anexa");
    return;
  }
  snprintf(msg, sizeof(msg), "Tirar os %d anexos desta conversa?", g_nanx);
  if (!pergunta(" Anexos ", msg)) return;
  g_nanx = 0;
  add_msg(ROLE_SYS, "anexos retirados");
  set_status("anexos retirados");
}

static void nova_conversa(void) {
  grava_e_avisa();
  g_n = 0;
  g_nanx = 0;
  g_edit[0] = 0;
  g_edit_len = 0;
  g_scroll = 0;
  g_nav = 0;
  sessao_nova();
  add_msg(ROLE_INFO, "Conversa nova. Enter envia, F1 ajuda, F2 sessoes, F10 sai.");
  set_status("conversa nova (a anterior ficou salva)");
}

static void volta_digitar(void) {
  g_nav = 0;
  g_scroll = 0;
}

int main(void) {
  int run = 1;
  if (tela_inicia() != 0) {
    printf("AGENT2: precisa da tela de texto (rode no DOS, nao no WIN)\n");
    return 1;
  }
  ai_cfg_load(&g_cfg);
  pasta_atual(g_pasta0, (int)sizeof(g_pasta0));
  snprintf(g_brdir, sizeof(g_brdir), "%s", g_pasta0);
  junta(g_sesdir, (int)sizeof(g_sesdir), g_pasta0, "SESSOES");
  le_cfg_sessoes();
  sessao_nova();
  if (g_cfg.key[0]) set_status(g_cfg.model);
  else set_status("falta key no AGENT.CFG");
  add_msg(ROLE_INFO, "Converse com o SODOS. Enter envia, F1 ajuda, F2 sessoes, F10 sai.");
  if (!k_net_ready()) set_status("rede nao esta pronta; use net antes");

  while (run) {
    int k, pag;
    desenha();
    k = tecla(200);
    if (k == K_NADA) continue;
    if (k == K_EOF || k == K_F(10)) break;
    pag = g_vis > 2 ? g_vis - 1 : 1;

    /* teclas que valem nos dois modos */
    if (k == K_F(1)) {
      ajuda();
      continue;
    }
    if (k == K_F(2)) {
      tela_sessoes();
      continue;
    }
    if (k == K_F(3)) {
      nova_conversa();
      continue;
    }
    if (k == K_F(4)) {
      anexar();
      continue;
    }
    if (k == K_F(5)) {
      tira_anexos();
      continue;
    }
    if (k == K_UP || k == K_PGUP || k == K_HOME) {
      g_nav = 1;
      if (k == K_UP) g_scroll++;
      else if (k == K_PGUP) g_scroll += pag;
      else g_scroll = 1000000;   /* desenha() limita ao comeco */
      continue;
    }
    if (k == K_DOWN || k == K_PGDN || k == K_END) {
      if (!g_nav) continue;
      if (k == K_DOWN) g_scroll--;
      else if (k == K_PGDN) g_scroll -= pag;
      else g_scroll = 0;
      if (g_scroll < 0) g_scroll = 0;
      continue;
    }

    /* navegando: Espaco, ESC ou Enter voltam; uma letra volta e ja digita */
    if (g_nav) {
      if (k == ' ' || k == K_ESC || k == K_ENTER) {
        volta_digitar();
      } else if (k == 8 || k == 127 || (k > 32 && k < 256)) {
        volta_digitar();
        edit_key(k);
      }
      continue;
    }

    /* digitando */
    if (k == K_ESC) {
      if (g_edit_len == 0) break;
      edit_key(8);
    } else if (k == '?' && g_edit_len == 0) {
      ajuda();
    } else if (k == K_ENTER) {
      if (!g_busy) send_now();
    } else if (k == 8 || k == 127) {
      edit_key(k);
    } else if (k >= 32 && k < 256) {
      edit_key(k);
    }
  }
  grava_e_avisa();
  tela_fim();
  return 0;
}
