/* wifi.cpp - Configurador do WiFi do SODOS (WIFI.EXE).
 *
 * Interface no padrao do TOP: barra de abas em ciano no alto, janelas com
 * sombra, barras de progresso e a barra de teclas do MC embaixo. Tudo em
 * 80x25, com os mesmos glifos trocados na fonte (0xC0-0xDC).
 *
 * Abas:
 *   Redes  - as redes do U:\NET\WIFI.CFG, na ordem do arquivo (a primeira no
 *            ar vence). Da para escolher, editar, subir/descer, apagar, criar
 *            e gravar. "Conectar" roda net wifi start e mostra a saida.
 *   No ar  - o que a ultima varredura viu (beacons do U:\NET\WIFI.LOG), com o
 *            canal de cada BSSID. Serve para trazer a rede vista para o cfg.
 *   Ajuda  - as teclas e o que cada campo significa.
 *
 * Compila com o CC do proprio sistema (ABI nativa, sem GUI.DLL):
 *   cc -I D:\ -L D:\ -o WIFI.EXE wifi.cpp      (ou rode BUILD.BAT)
 * Fora da tela de texto (WIN, saida desviada) ou com -l imprime a lista.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>

#define SCR_W 80
#define SCR_H 25

/* ---------- cores (fundo << 4 | frente), as mesmas do TOP ---------- */
#define COR(fundo, frente) (((fundo) << 4) | (frente))
#define PRETO 0
#define AZUL 1
#define VERDE 2
#define CIANO 3
#define VERMELHO 4
#define MAGENTA 5
#define MARROM 6
#define CINZA 7
#define ESCURO 8
#define AZUL_CLARO 9
#define VERDE_CLARO 10
#define CIANO_CLARO 11
#define VERMELHO_CLARO 12
#define MAGENTA_CLARO 13
#define AMARELO 14
#define BRANCO 15

#define C_JANELA COR(AZUL_CLARO, BRANCO)
#define C_BORDA COR(AZUL_CLARO, BRANCO)
#define C_TITULO COR(BRANCO, AZUL)
#define C_FORTE COR(AZUL_CLARO, BRANCO)
#define C_FRACO COR(AZUL_CLARO, AZUL)
#define C_SELECAO COR(BRANCO, AZUL)
#define C_FUNDO COR(PRETO, AZUL)
#define C_MENU COR(CIANO, PRETO)
#define C_MENUSEL COR(PRETO, BRANCO)
#define C_MENUHOT COR(CIANO, AMARELO)
#define C_TECLA COR(PRETO, CINZA)
#define C_ROTULO COR(CIANO, PRETO)
#define C_PAINEL COR(AZUL, CINZA)
#define C_CABECA COR(AZUL, AMARELO)
#define C_OK COR(PRETO, VERDE_CLARO)
#define C_ERRO COR(PRETO, VERMELHO_CLARO)
#define C_CAMPO COR(BRANCO, PRETO)
#define C_CAMPOSEL COR(PRETO, AMARELO)

/* ---------- glifos trocados na fonte (0xC0-0xDC, como no TOP) ---------- */
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
#define G_CHK 0xCB
#define G_CHKON 0xCC
#define G_RAD 0xCD
#define G_RADON 0xCE
#define G_TICK 0xCF
#define G_PB0 0xD0
#define G_MAPF 0xD5
#define G_MAPU 0xD6
#define G_MAPB 0xD7
#define G_ARROW 0xD8
#define G_DESK 0xD9
#define G_MAPH 0xDA
#define G_MAPR 0xDB
#define G_DOWN 0xDC
#define G_BASE 0xC0
#define N_GLIFOS 29

static const unsigned char glifos[N_GLIFOS][16] = {
  {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF},
  {0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0xFF, 0xFF},
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF},
  {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0},
  {0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03},
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0},
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0xFF, 0xFF, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03},
  {0x0F, 0x3F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x3F, 0x0F},
  {0xF0, 0xFC, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFC, 0xF0},
  {0x00, 0x00, 0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0x7E, 0x42, 0x42, 0x5A, 0x5A, 0x5A, 0x5A, 0x42, 0x42, 0x7E, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0x00, 0x3C, 0x42, 0x81, 0x81, 0x81, 0x81, 0x42, 0x3C, 0x00, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0x00, 0x3C, 0x42, 0x99, 0x99, 0x99, 0x99, 0x42, 0x3C, 0x00, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0x03, 0x06, 0x06, 0x0C, 0xCC, 0xD8, 0x78, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0xFF, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xFF, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0xFF, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xFF, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0xFF, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFF, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0xFF, 0x81, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0x81, 0xFF, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0x00, 0x20, 0x30, 0x38, 0x3C, 0x3C, 0x38, 0x30, 0x20, 0x00, 0x00, 0x00, 0x00},
  {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00},
  {0x00, 0x00, 0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x00, 0x00, 0x00},
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x7C, 0x38, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

/* ---------- teclas (os mesmos codigos do kernel) ---------- */
#define K_UP 0x100
#define K_DOWN 0x101
#define K_LEFT 0x102
#define K_RIGHT 0x103
#define K_PGUP 0x104
#define K_PGDN 0x105
#define K_HOME 0x106
#define K_END 0x107
#define K_F(n) (0x140 + (n))
#define K_ESC 27
#define K_ENTER '\n'
#define K_TAB '\t'
#define K_EOF 26
#define K_NADA (-1)

/* ---------- tela de celulas ---------- */
static unsigned char salvo[N_GLIFOS][16];
static unsigned short tela[25 * 80];
static unsigned short vista[25 * 80];
static unsigned short antes[25 * 80];

static void tela_fim(void);

static int tela_inicia(void) {
  int i;
  if (k_vput(0, 0, tela, 0) != 0) return -1;
  if (k_blink(0) != 0) return -1;
  for (i = 0; i < N_GLIFOS; i++)
    if (k_font_get(G_BASE + i, salvo[i]) != 0) return -1;
  for (i = 0; i < N_GLIFOS; i++)
    if (k_font_set(G_BASE + i, glifos[i]) != 0) {
      tela_fim();
      return -1;
    }
  for (i = 0; i < 25 * 80; i++) vista[i] = 0xFFFF;
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
  for (r = 0; r < 25; r++) {
    int a = -1, b = -1;
    for (c = 0; c < 80; c++) {
      if (tela[r * 80 + c] != vista[r * 80 + c]) {
        if (a < 0) a = c;
        b = c;
      }
    }
    if (a < 0) continue;
    k_vput(r, a, tela + r * 80 + a, b - a + 1);
    for (c = a; c <= b; c++) vista[r * 80 + c] = tela[r * 80 + c];
  }
}

/* O console pode ter sido escrito por um comando (net wifi scan): a copia
 * local nao vale mais, entao a proxima tela_mostra repinta tudo. */
static void tela_suja(void) {
  int i;
  for (i = 0; i < 25 * 80; i++) vista[i] = 0xFFFF;
}

static void poe(int r, int c, int ch, int cor) {
  if (r < 0 || r >= 25 || c < 0 || c >= 80) return;
  tela[r * 80 + c] = (unsigned short)((ch & 0xFF) | ((cor & 0xFF) << 8));
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

static void pinta(int r, int c, int n, int cor) {
  for (; n > 0; n--, c++) {
    if (r < 0 || r >= 25 || c < 0 || c >= 80) continue;
    tela[r * 80 + c] = (unsigned short)((tela[r * 80 + c] & 0xFF) | ((cor & 0xFF) << 8));
  }
}

static int tam(const char* s) { return (int)strlen(s); }

static void fundo(void) {
  int r;
  for (r = 1; r < 24; r++) linha(r, 0, 80, G_DESK, C_FUNDO);
}

/* barra de cima com as abas, como no TOP */
static void menu_topo(int aba) {
  static const char* abas[3] = { " Redes ", " No ar ", " Ajuda " };
  char buf[40];
  int c = 6, i;
  linha(0, 0, 80, ' ', C_MENU);
  texto(0, 1, "WIFI", COR(CIANO, BRANCO));
  for (i = 0; i < 3; i++) {
    int cor = i == aba ? C_MENUSEL : C_MENU;
    texto(0, c, abas[i], cor);
    poe(0, c + 1, (unsigned char)abas[i][1], i == aba ? COR(PRETO, AMARELO) : C_MENUHOT);
    c += tam(abas[i]) + 1;
  }
  {
    int t = k_time();
    if (t >= 0) {
      sprintf(buf, "%02d:%02d:%02d UTC", t / 10000, t / 100 % 100, t % 100);
      texto(0, 66, buf, C_MENU);
    }
  }
}

/* barra de teclas do MC: numero no cinza e rotulo no ciano */
static void teclas(const char** rotulos) {
  int i, c = 0;
  char n[4];
  linha(24, 0, 80, ' ', C_ROTULO);
  for (i = 0; i < 10; i++) {
    int w = i == 9 ? 80 - c : 8;
    const char* rot = rotulos ? rotulos[i] : "";
    sprintf(n, "%d", i + 1);
    texto(24, c, n, C_TECLA);
    textow(24, c + tam(n), rot, w - tam(n), C_ROTULO);
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

static void botao(int r, int c, const char* s, int sel) {
  int f = sel ? BRANCO : CINZA;
  poe(r, c, G_BTNL, COR(AZUL_CLARO, f));
  linha(r, c + 1, tam(s) + 2, ' ', COR(f, sel ? AZUL : PRETO));
  texto(r, c + 2, s, COR(f, sel ? AZUL : PRETO));
  poe(r, c + tam(s) + 3, G_BTNR, COR(AZUL_CLARO, f));
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
  if (k == '\r') return K_ENTER;
  return k;
}

/* ---------- dados das redes ---------- */
#define MAX_REDES 8
#define MAX_VISTAS 48

struct rede {
  char ssid[33];
  char psk[64];
  char bssid[18];
  char ip[24];
  int canal;
  int tem_bssid;
};

struct vista {
  char bssid[18];
  char ssid[33];
  int canal;
  unsigned beacons;
  int casa;      /* "*" no log: bateu com alguma rede do cfg */
};

static struct rede redes[MAX_REDES];
static int n_redes;
static int sel;

static struct vista vistas[MAX_VISTAS];
static int n_vistas;
static int sel_vista;

static char cfg_path[32] = "U:\\NET\\WIFI.CFG";
static char log_path[32] = "U:\\NET\\WIFI.LOG";
static char msg[80] = "escolha uma rede e tecle 2 para conectar";
static int aba;

/* ---------- utilidades de texto ---------- */
static void copia(char* d, const char* s, int max) {
  int i = 0;
  while (s[i] && i < max - 1) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

static void diz(const char* s) { copia(msg, s, (int)sizeof(msg)); }

static int hexa(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int so_digitos(const char* s) {
  if (!*s) return 0;
  while (*s) {
    if (*s < '0' || *s > '9') return 0;
    s++;
  }
  return 1;
}

static int psk_ok(const char* p) {
  int n = tam(p);
  return n >= 8 && n <= 63;
}

static void mac_texto(const char* s, char* out) {
  static const char* h = "0123456789ABCDEF";
  int i, k = 0;
  for (i = 0; i < 6; i++) {
    int a = hexa(s[0]), b = hexa(s[1]);
    if (a < 0 || b < 0) {
      out[0] = 0;
      return;
    }
    out[k++] = h[a];
    out[k++] = h[b];
    if (i != 5) out[k++] = ':';
    s += 2;
    if (*s == ':' || *s == '-') s++;
  }
  out[k] = 0;
}

/* ---------- leitura e gravacao ---------- */
static int le_arquivo(const char* prim, const char* seg, char* buf, int max) {
  FILE* f = fopen(prim, "rb");
  int n;
  if (!f && seg && seg[0]) f = fopen(seg, "rb");
  if (!f) return -1;
  n = (int)fread(buf, 1, (size_t)(max - 1), f);
  fclose(f);
  if (n < 0) n = 0;
  buf[n] = 0;
  return n;
}

/* Mesmas regras do wifi.c: [nome] ou um ssid= novo comecam outra rede. */
static void carrega_cfg(void) {
  static char texto_cfg[4096];
  int n = le_arquivo("U:\\NET\\WIFI.CFG", "D:\\NET\\WIFI.CFG", texto_cfg, (int)sizeof(texto_cfg));
  char* p;
  struct rede r;
  int tem = 0;
  n_redes = 0;
  if (n < 0) {
    diz("nao achei U:\\NET\\WIFI.CFG (nem em D:)");
    return;
  }
  copia(cfg_path, "U:\\NET\\WIFI.CFG", (int)sizeof(cfg_path));
  memset(&r, 0, sizeof(r));
  copia(r.ip, "dhcp", (int)sizeof(r.ip));
  p = texto_cfg;
  while (*p) {
    char* linha_ini = p;
    char* q;
    while (*p && *p != '\n') p++;
    if (*p == '\n') *p++ = 0;
    q = linha_ini;
    while (*q == ' ' || *q == '\t') q++;
    if (*q == '#' || *q == ';' || !*q) continue;
    if (*q == '[') {
      if (tem && r.ssid[0] && n_redes < MAX_REDES) redes[n_redes++] = r;
      memset(&r, 0, sizeof(r));
      copia(r.ip, "dhcp", (int)sizeof(r.ip));
      tem = 1;
      continue;
    }
    {
      char* eq = q;
      while (*eq && *eq != '=') eq++;
      if (!*eq) continue;
      *eq = 0;
      {
        char* val = eq + 1;
        char* fim = eq - 1;
        while (fim >= q && (*fim == ' ' || *fim == '\t')) *fim-- = 0;
        if (!strcmp(q, "ssid")) {
          if (tem && r.ssid[0] && n_redes < MAX_REDES) redes[n_redes++] = r;
          memset(&r, 0, sizeof(r));
          copia(r.ip, "dhcp", (int)sizeof(r.ip));
          copia(r.ssid, val, (int)sizeof(r.ssid));
          tem = 1;
        } else if (!strcmp(q, "psk")) {
          copia(r.psk, val, (int)sizeof(r.psk));
        } else if (!strcmp(q, "bssid")) {
          mac_texto(val, r.bssid);
          r.tem_bssid = r.bssid[0] ? 1 : 0;
        } else if (!strcmp(q, "channel")) {
          r.canal = (int)strtoul(val, 0, 10);
        } else if (!strcmp(q, "ip")) {
          copia(r.ip, val, (int)sizeof(r.ip));
        }
      }
    }
  }
  if (r.ssid[0] && n_redes < MAX_REDES) redes[n_redes++] = r;
  if (n_redes == 0) diz("WIFI.CFG sem nenhuma rede (ssid= e psk=)");
}

static int grava_cfg(void) {
  FILE* f;
  int i, k;
  f = fopen(cfg_path, "wb");
  if (!f) return -1;
  fputs("# SODOS - redes WiFi (gravado pelo WIFI.EXE)\n", f);
  fputs("# Cada [nome] comeca uma rede; net wifi start usa a primeira, na ordem\n", f);
  fputs("# do arquivo, que estiver no ar, com a senha dela.\n", f);
  for (i = 0; i < n_redes; i++) {
    const struct rede* r = &redes[i];
    fprintf(f, "\n[rede %d]\n", i + 1);
    fprintf(f, "ssid=%s\n", r->ssid);
    fprintf(f, "psk=%s\n", r->psk);
    fprintf(f, "security=wpa2-psk\n");
    if (r->tem_bssid && r->bssid[0]) fprintf(f, "bssid=%s\n", r->bssid);
    if (r->canal > 0) fprintf(f, "channel=%d\n", r->canal);
    if (r->canal > 0) fputs("band=5g\n", f);
    fprintf(f, "ip=%s\n", r->ip[0] ? r->ip : "dhcp");
  }
  fflush(f);
  fclose(f);
  for (i = 0; i < n_redes; i++)
    if (!psk_ok(redes[i].psk)) {
      char b[80];
      sprintf(b, "gravado, mas a rede %d esta com senha fora de 8..63", i + 1);
      diz(b);
      return 0;
    }
  k = n_redes;
  {
    char b[80];
    sprintf(b, "gravado em %s (%d rede(s))", cfg_path, k);
    diz(b);
  }
  return 0;
}

/* ---------- WIFI.LOG: o que a varredura viu ---------- */

/* Uma rede vista; o mesmo BSSID em outro canal atualiza a entrada. */
static void add_vista(const char* bssid, const char* ssid, int canal, unsigned beacons, int casa) {
  int i;
  for (i = 0; i < n_vistas; i++)
    if (!strcmp(vistas[i].bssid, bssid)) {
      vistas[i].canal = canal;
      vistas[i].beacons = beacons;
      return;
    }
  if (n_vistas >= MAX_VISTAS) return;
  {
    struct vista* v = &vistas[n_vistas++];
    memset(v, 0, sizeof(*v));
    copia(v->bssid, bssid, (int)sizeof(v->bssid));
    copia(v->ssid, ssid, (int)sizeof(v->ssid));
    v->canal = canal;
    v->beacons = beacons;
    v->casa = casa;
  }
}

static void carrega_log(void) {
  static char texto_log[32768];
  int n = le_arquivo("U:\\NET\\WIFI.LOG", "D:\\NET\\WIFI.LOG", texto_log, (int)sizeof(texto_log));
  char* p;
  int canal = 0;
  n_vistas = 0;
  if (n < 0) {
    diz("nao achei o WIFI.LOG: rode net wifi scan uma vez");
    return;
  }
  copia(log_path, "U:\\NET\\WIFI.LOG", (int)sizeof(log_path));
  p = texto_log;
  while (*p) {
    char* l = p;
    while (*p && *p != '\n') p++;
    if (*p == '\n') *p++ = 0;
    if (l[0] == '[') {
      if (!strncmp(l, "[rtw] radio no canal ", 21)) {
        canal = (int)strtoul(l + 21, 0, 16);
      } else if (!strncmp(l, "[wifi] ", 7) && (l[7] == '*' || l[7] == ' ')) {
        const char* b = l + 9;
        char mac[18], nome[33];
        const char* q;
        int i, ok = 1;
        for (i = 0; i < 17; i++)
          if (i % 3 == 2 ? b[i] != ':' : hexa(b[i]) < 0) ok = 0;
        if (!ok) continue;
        for (i = 0; i < 17; i++) mac[i] = b[i];
        mac[17] = 0;
        nome[0] = 0;
        q = b + 18;
        if (*q == '"') {
          int k = 0;
          q++;
          while (*q && *q != '"' && k < 32) nome[k++] = *q++;
          nome[k] = 0;
          if (*q == '"') q++;
        }
        while (*q == ' ') q++;
        add_vista(mac, nome, canal, (unsigned)strtoul(q, 0, 16), l[7] == '*');
      }
    }
  }
  if (n_vistas == 0) diz("WIFI.LOG sem beacons: use 9 (Buscar) para varrer");
}

/* ---------- desenho das abas ---------- */
static void desenha_rede_linha(int r, int i, int selecionada) {
  char b[96];
  const struct rede* p = &redes[i];
  int cor = selecionada ? C_SELECAO : C_JANELA;
  if (selecionada) textow(r, 2, "", 76, cor);
  sprintf(b, "%2d", i + 1);
  texto(r, 3, b, selecionada ? cor : C_FRACO);
  textow(r, 6, p->ssid, 32, cor);
  if (p->canal > 0) sprintf(b, "%d", p->canal);
  else copia(b, "-", 2);
  texto(r, 39, b, selecionada ? cor : C_FRACO);
  texto(r, 45, p->tem_bssid ? p->bssid : "(auto)", cor);
  if (!psk_ok(p->psk)) texto(r, 68, "senha!", C_ERRO);
}

static void desenha_redes(void) {
  char b[128];
  int i, prim, linhas = 12;
  fundo();
  janela(1, 1, 15, 78, "Redes do WIFI.CFG (a 1a no ar vence)");
  texto(2, 3, "#", C_CABECA);
  texto(2, 6, "SSID", C_CABECA);
  texto(2, 39, "CANAL", C_CABECA);
  texto(2, 45, "BSSID", C_CABECA);
  separador(2, 2, 76);
  if (n_redes == 0) {
    texto(4, 4, "nenhuma rede no arquivo - tecle 4 para criar", C_FRACO);
  }
  prim = 0;
  if (sel >= linhas) prim = sel - linhas + 1;
  for (i = prim; i < n_redes && i < prim + linhas; i++)
    desenha_rede_linha(3 + (i - prim), i, i == sel);

  janela(16, 1, 7, 78, "Detalhe");
  if (n_redes > 0) {
    const struct rede* p = &redes[sel];
    sprintf(b, "%d de %d   \"%s\"", sel + 1, n_redes, p->ssid);
    texto(17, 3, b, C_FORTE);
    if (p->psk[0]) {
      int k;
      int m = tam(p->psk);
      for (k = 0; k < m && k < 40; k++) b[k] = '*';
      b[k] = 0;
      sprintf(b + k, "   (%d caracteres)", m);
      texto(18, 3, "senha  ", C_FRACO);
      texto(18, 10, b, psk_ok(p->psk) ? C_JANELA : C_ERRO);
    } else {
      texto(18, 3, "senha  (vazia - o net wifi start recusa)", C_ERRO);
    }
    sprintf(b, "bssid  %s", p->tem_bssid ? p->bssid : "(o primeiro que responder)");
    texto(19, 3, b, C_JANELA);
    sprintf(b, "canal  %d      ip  %s", p->canal, p->ip[0] ? p->ip : "dhcp");
    texto(20, 3, b, C_JANELA);
  } else {
    texto(18, 3, "tecle 4 (Nova) para cadastrar a primeira", C_FRACO);
  }
}

static void desenha_no_ar(void) {
  char b[128];
  int i, prim, linhas = 15;
  fundo();
  janela(1, 1, 19, 78, "No ar - beacons do ultimo net wifi scan");
  texto(2, 3, "BSSID", C_CABECA);
  texto(2, 22, "SSID", C_CABECA);
  texto(2, 52, "CANAL", C_CABECA);
  texto(2, 59, "QUADROS", C_CABECA);
  separador(2, 2, 76);
  if (n_vistas == 0) {
    texto(4, 4, "nada no WIFI.LOG - tecle 9 (Buscar) para varrer", C_FRACO);
  }
  prim = 0;
  if (sel_vista >= linhas) prim = sel_vista - linhas + 1;
  for (i = prim; i < n_vistas && i < prim + linhas; i++) {
    const struct vista* v = &vistas[i];
    int r = 3 + (i - prim);
    int cor = i == sel_vista ? C_SELECAO : C_JANELA;
    if (i == sel_vista) textow(r, 2, "", 76, cor);
    texto(r, 3, v->casa ? "*" : " ", i == sel_vista ? cor : C_FORTE);
    texto(r, 4, v->bssid, cor);
    textow(r, 22, v->ssid[0] ? v->ssid : "(oculta)", 28, cor);
    sprintf(b, "%d", v->canal);
    texto(r, 52, b, i == sel_vista ? cor : C_FRACO);
    sprintf(b, "%u", v->beacons);
    texto(r, 60, b, cor);
  }
  janela(20, 1, 3, 78, "Dica");
  texto(21, 3, "Enter traz a rede vista para o WIFI.CFG   9 busca de novo", C_JANELA);
}

static void desenha_ajuda(void) {
  static const char* txt[16] = {
    "WIFI - configurador das redes do SODOS",
    "",
    "ABAS: Tab ou F3 passa para a proxima (Redes, No ar, Ajuda).",
    "9 BUSCAR: roda net wifi scan (5 GHz todo), poe a lista na aba No ar.",
    "4 NOVA: busca primeiro; escolha na lista e Enter abre o formulario ja",
    "        com ssid, canal e bssid - so falta a senha.",
    "REDES: setas escolhem; Enter edita a rede escolhida.",
    "1 Sair  2 Ligar  3 Gravar  4 Nova  5 Apagar  6 Editar",
    "7 Subir  8 Descer  9 Buscar  10 Ajuda",
    "",
    "A ordem do arquivo e a prioridade: net wifi start usa a primeira",
    "rede que estiver no ar, com a senha dela. Se a rede mudou de nome",
    "ou de canal, apague o bssid= e acerte o ssid= e o channel=.",
    "NO AR: beacons e canal do WIFI.LOG; \"*\" casou com o cfg.",
    "CONECTAR roda net wifi start e mostra a saida na tela.",
    "",
  };
  int i;
  fundo();
  janela(1, 1, 22, 78, "Ajuda");
  for (i = 0; i < 16; i++)
    texto(2 + i, 3, txt[i], i == 0 ? C_FORTE : C_JANELA);
}

static void desenha(void) {
  static const char* rot[10] = { "Sair", "Ligar", "Gravar", "Nova", "Apagar",
                                 "Editar", "Subir", "Descer", "Buscar", "Ajuda" };
  menu_topo(aba);
  if (aba == 0) desenha_redes();
  else if (aba == 1) desenha_no_ar();
  else desenha_ajuda();
  linha(23, 0, 80, ' ', C_PAINEL);
  texto(23, 1, msg, C_PAINEL);
  if (aba == 0) {
    char b[24];
    sprintf(b, "redes %d/%d", n_redes, MAX_REDES);
    texto(23, 80 - tam(b) - 1, b, C_CAMPO);
  } else if (aba == 1) {
    char b[24];
    sprintf(b, "vistas %d", n_vistas);
    texto(23, 80 - tam(b) - 1, b, C_CAMPO);
  }
  teclas(rot);
}

/* ---------- caixas de dialogo ---------- */
static void aviso(const char* titulo, const char* m) {
  int n = 1, w = tam(titulo) + 8, l = 0, i, r, c, h;
  const char* p;
  for (p = m; *p; p++) {
    if (*p == '\n') {
      n++;
      l = 0;
    } else if (++l + 8 > w) {
      w = l + 8;
    }
  }
  if (w > 76) w = 76;
  if (w < 24) w = 24;
  h = n + 6;
  r = (24 - h) / 2;
  c = (80 - w) / 2;
  memcpy(antes, tela, sizeof tela);
  janela(r, c, h, w, titulo);
  for (i = 0, p = m; i < n; i++) {
    int k = 0;
    while (p[k] && p[k] != '\n' && k < w - 8) {
      poe(r + 2 + i, c + 4 + k, (unsigned char)p[k], i == 0 ? C_FORTE : C_JANELA);
      k++;
    }
    p += k;
    if (*p) p++;
  }
  separador(r + h - 3, c, w);
  botao(r + h - 2, c + (w - 6) / 2, "OK", 1);
  tela_mostra();
  for (;;) {
    int k = tecla(0);
    if (k == K_ENTER || k == K_ESC || k == K_EOF || k == ' ') break;
  }
  memcpy(tela, antes, sizeof tela);
}

/* Pergunta Sim/Nao. Devolve 1 para sim. */
static int confirma(const char* titulo, const char* m) {
  int n = 1, w = tam(titulo) + 8, l = 0, i, r, c, h;
  const char* p;
  for (p = m; *p; p++) {
    if (*p == '\n') {
      n++;
      l = 0;
    } else if (++l + 8 > w) {
      w = l + 8;
    }
  }
  if (w > 76) w = 76;
  if (w < 30) w = 30;
  h = n + 6;
  r = (24 - h) / 2;
  c = (80 - w) / 2;
  memcpy(antes, tela, sizeof tela);
  janela(r, c, h, w, titulo);
  for (i = 0, p = m; i < n; i++) {
    int k = 0;
    while (p[k] && p[k] != '\n' && k < w - 8) {
      poe(r + 2 + i, c + 4 + k, (unsigned char)p[k], i == 0 ? C_FORTE : C_JANELA);
      k++;
    }
    p += k;
    if (*p) p++;
  }
  separador(r + h - 3, c, w);
  botao(r + h - 2, c + (w - 14) / 2, "Sim", 1);
  botao(r + h - 2, c + (w - 14) / 2 + 9, "Nao", 0);
  tela_mostra();
  for (;;) {
    int k = tecla(0);
    if (k == K_ENTER || k == 's' || k == 'S' || k == 'y' || k == 'Y') {
      memcpy(tela, antes, sizeof tela);
      return 1;
    }
    if (k == K_ESC || k == 'n' || k == 'N' || k == K_EOF) break;
  }
  memcpy(tela, antes, sizeof tela);
  return 0;
}

/* texto corrido com rolagem: a saida do net wifi start */
static void rolagem(const char* titulo, char* txt) {
  static char* linhas[512];
  int n = 0, i, topo = 0, visiveis = 18;
  char* p = txt;
  linhas[0] = p;
  {
    char* q = p;
    while (*q && n < 511) {
      if (*q == '\n') {
        *q = 0;
        q++;
        n++;
        linhas[n] = q;
        continue;
      }
      q++;
    }
  }
  memcpy(antes, tela, sizeof tela);
  for (;;) {
    int k;
    fundo();
    janela(1, 1, 22, 78, titulo);
    for (i = 0; i < visiveis; i++) {
      if (topo + i > n) break;
      textow(2 + i, 3, linhas[topo + i], 74, C_JANELA);
    }
    linha(23, 0, 80, ' ', C_PAINEL);
    texto(23, 1, "setas/PgUp/PgDn rolam, Esc ou Enter fecha", C_PAINEL);
    teclas(0);
    tela_mostra();
    k = tecla(0);
    if (k == K_ESC || k == K_ENTER || k == 'q' || k == 'Q' || k == K_EOF) break;
    if (k == K_UP && topo > 0) topo--;
    else if (k == K_DOWN && topo + visiveis <= n) topo++;
    else if (k == K_PGUP && topo > 0) topo -= visiveis;
    else if (k == K_PGDN && topo + visiveis <= n) topo += visiveis;
    else if (k == K_HOME) topo = 0;
    else if (k == K_END && n > visiveis) topo = n - visiveis;
  }
  memcpy(tela, antes, sizeof tela);
}

/* ---------- formulario de uma rede ---------- */
static char campo[5][64];
static int campo_atual;

static void form_campos(const struct rede* r) {
  copia(campo[0], r->ssid, 64);
  copia(campo[1], r->psk, 64);
  if (r->canal > 0) sprintf(campo[2], "%d", r->canal);
  else copia(campo[2], "", 64);
  copia(campo[3], r->bssid, 64);
  copia(campo[4], r->ip[0] ? r->ip : "dhcp", 64);
}

static void form_campo_linha(int r, const char* rotulo, int i, int senha) {
  int cor = i == campo_atual ? C_CAMPOSEL : C_CAMPO;
  char b[72];
  int k, n;
  texto(r, 11, rotulo, i == campo_atual ? C_CAMPOSEL : C_FRACO);
  n = tam(campo[i]);
  if (senha) {
    for (k = 0; k < n && k < 48; k++) b[k] = '*';
    b[k] = 0;
  } else {
    copia(b, campo[i], (int)sizeof(b));
  }
  textow(r, 18, b, 50, cor);
  if (i == campo_atual) poe(r, 18 + (n < 50 ? n : 49), '_', cor);
}

static void form_desenha(const char* titulo) {
  fundo();
  janela(3, 8, 19, 64, titulo);
  form_campo_linha(5, "SSID ", 0, 0);
  form_campo_linha(7, "Senha", 1, campo_atual != 1 ? 1 : 0);
  form_campo_linha(9, "Canal", 2, 0);
  form_campo_linha(11, "BSSID", 3, 0);
  form_campo_linha(13, "IP   ", 4, 0);
  separador(16, 9, 62);
  texto(17, 10, "espacos no SSID valem; BSSID vazio = o 1o que responder", C_FRACO);
  texto(18, 10, "IP: dhcp (fixo ainda nao aplicado pelo wifi)", C_FRACO);
  botao(19, 12, "Gravar", 1);
  botao(19, 26, "Cancelar", 0);
  linha(23, 0, 80, ' ', C_PAINEL);
  texto(23, 1, "setas/Tab mudam de campo, Enter confirma, Esc cancela", C_PAINEL);
  teclas(0);
}

/* Devolve 1 se confirmou; o struct r sai preenchido. */
static int formulario(struct rede* r, const char* titulo) {
  int rodando = 1, ok = 0;
  form_campos(r);
  campo_atual = 0;
  while (rodando) {
    int k;
    form_desenha(titulo);
    tela_mostra();
    k = tecla(0);
    if (k == K_ESC || k == K_EOF) break;
    if (k == K_ENTER) {
      if (!campo[0][0]) {
        aviso("Falta o SSID", "O ssid= nao pode ficar vazio.");
        continue;
      }
      if (!psk_ok(campo[1])) {
        aviso("Senha", "A senha WPA2 precisa de 8 a 63 caracteres.");
        continue;
      }
      if (campo[2][0] && (!so_digitos(campo[2]) || strtoul(campo[2], 0, 10) < 1 ||
                          strtoul(campo[2], 0, 10) > 196)) {
        aviso("Canal", "O canal vai de 1 a 196 (ex.: 36, 52, 149).\nVazio = 56, o padrao do wifi.c.");
        continue;
      }
      ok = 1;
      break;
    }
    if (k == K_UP || k == K_TAB) {
      campo_atual = (campo_atual + 4) % 5;
      continue;
    }
    if (k == K_DOWN) {
      campo_atual = (campo_atual + 1) % 5;
      continue;
    }
    if (k == 8 || k == 127) {
      int n = tam(campo[campo_atual]);
      if (n > 0) campo[campo_atual][n - 1] = 0;
      continue;
    }
    if (k >= 32 && k < 127) {
      int n = tam(campo[campo_atual]);
      if (n < 62) {
        campo[campo_atual][n] = (char)k;
        campo[campo_atual][n + 1] = 0;
      }
      continue;
    }
  }
  if (ok) {
    copia(r->ssid, campo[0], (int)sizeof(r->ssid));
    copia(r->psk, campo[1], (int)sizeof(r->psk));
    r->canal = campo[2][0] ? (int)strtoul(campo[2], 0, 10) : 0;
    mac_texto(campo[3], r->bssid);
    r->tem_bssid = r->bssid[0] ? 1 : 0;
    copia(r->ip, campo[4][0] ? campo[4] : "dhcp", (int)sizeof(r->ip));
  }
  return ok;
}

/* ---------- acoes ---------- */
/* Roda net wifi scan no console e recarrega a lista do WIFI.LOG. A saida do
 * comando rola na tela; ao voltar, a copia local e descartada e a tela e
 * repintada inteira. */
static void roda_scan(void) {
  char cmd[32];
  copia(cmd, "net wifi scan", (int)sizeof(cmd));
  diz("procurando redes no ar: net wifi scan ...");
  desenha();
  tela_mostra();
  k_shell(cmd);
  tela_suja();
  carrega_log();
}

static void conecta(void) {
  static char saida[16384];
  char cmd[64];
  int len = 0, rc;
  copia(cmd, "net wifi start", (int)sizeof(cmd));
  diz("conectando: net wifi start ...");
  desenha();
  tela_mostra();
  rc = k_shell_capture(cmd, saida, (int)sizeof(saida), &len);
  copia(saida + (len > 0 ? len : 0), "", 1);
  rolagem(rc == 0 ? "net wifi start - ok" : "net wifi start - falhou", saida);
  tela_suja();
  carrega_log();
  diz(rc == 0 ? "conectado (veja a aba No ar)" : "net wifi start falhou - veja a saida");
}

/* Nova: procura primeiro; a rede escolhida ja vem com ssid, canal e bssid. */
static void nova_rede(void) {
  struct rede r;
  if (n_redes >= MAX_REDES) {
    aviso("Sem espaco", "O WIFI.CFG aceita ate 8 redes.");
    return;
  }
  roda_scan();
  aba = 1;
  if (!n_vistas) {
    memset(&r, 0, sizeof(r));
    copia(r.ip, "dhcp", (int)sizeof(r.ip));
    diz("nada no ar: preencha a mao");
    aviso("Nenhuma rede no ar", "O scan nao ouviu beacon nenhum.\n"
          "Confira o radio (net wifi status) ou preencha a mao.");
    if (formulario(&r, "Nova rede")) {
      redes[n_redes++] = r;
      sel = n_redes - 1;
      aba = 0;
      diz("rede criada - tecle 3 para gravar");
    }
    return;
  }
  diz("escolha a rede no ar e Enter: a Nova ja vem preenchida");
}

static void acao(int n) {
  if (n == 1) return;
  if (n == 9) {
    roda_scan();
    aba = 1;
    diz(n_vistas ? "escolha com as setas; Enter leva a rede para o cfg"
                 : "nada no ar (veja a saida do scan acima)");
    return;
  }
  if (n == 4) {
    nova_rede();
    return;
  }
  if (n == 10) {
    desenha_ajuda();
    aba = 2;
    diz("Ajuda");
    return;
  }
  if (aba == 2) {
    aba = 0;
    return;
  }
  if (aba == 1) {
    if (n == 2 && n_vistas > 0) {
      struct rede r;
      memset(&r, 0, sizeof(r));
      copia(r.ssid, vistas[sel_vista].ssid, (int)sizeof(r.ssid));
      copia(r.bssid, vistas[sel_vista].bssid, (int)sizeof(r.bssid));
      r.tem_bssid = 1;
      r.canal = vistas[sel_vista].canal;
      copia(r.ip, "dhcp", (int)sizeof(r.ip));
      if (!r.ssid[0]) {
        aviso("Rede oculta", "O beacon veio sem SSID (rede oculta):\ndigite o nome no campo SSID.");
      }
      if (formulario(&r, "Nova rede (vista no ar)")) {
        if (n_redes < MAX_REDES) {
          redes[n_redes++] = r;
          sel = n_redes - 1;
          aba = 0;
          diz("rede vista foi para o fim do WIFI.CFG - tecle 3 para gravar");
        } else {
          aviso("Sem espaco", "O WIFI.CFG aceita ate 8 redes.");
        }
      }
    } else {
      diz("setas escolhem; Enter ou 2 usa a rede vista");
    }
    return;
  }

  /* aba Redes */
  if (n == 2) {
    if (n_redes == 0) {
      aviso("Nada para conectar", "Cadastre uma rede com a tecla 4.");
      return;
    }
    conecta();
    return;
  }
  if (n == 3) {
    if (grava_cfg() < 0) aviso("Gravar", "Nao consegui escrever o WIFI.CFG.");
    return;
  }
  if (n == 5) {
    int i;
    if (n_redes == 0) return;
    {
      char b[96];
      sprintf(b, "Apagar \"%s\" do WIFI.CFG?", redes[sel].ssid);
      if (!confirma("Apagar", b)) return;
    }
    for (i = sel; i + 1 < n_redes; i++) redes[i] = redes[i + 1];
    n_redes--;
    if (sel >= n_redes && sel > 0) sel = n_redes - 1;
    diz("rede apagada - tecle 3 para gravar");
    return;
  }
  if (n == 6) {
    if (n_redes == 0) return;
    if (formulario(&redes[sel], "Editar rede")) diz("editada - tecle 3 para gravar");
    return;
  }
  if (n == 7) {
    if (sel > 0) {
      struct rede t = redes[sel];
      redes[sel] = redes[sel - 1];
      redes[sel - 1] = t;
      sel--;
      diz("subiu na prioridade - tecle 3 para gravar");
    }
    return;
  }
  if (n == 8) {
    if (sel + 1 < n_redes) {
      struct rede t = redes[sel];
      redes[sel] = redes[sel + 1];
      redes[sel + 1] = t;
      sel++;
      diz("desceu na prioridade - tecle 3 para gravar");
    }
    return;
  }
}

/* ---------- modo lista (sem tela de texto) ---------- */
static void lista(void) {
  int i;
  printf("WIFI - redes do WIFI.CFG\n");
  printf(" #  SSID                             CANAL  BSSID\n");
  for (i = 0; i < n_redes; i++) {
    const struct rede* r = &redes[i];
    printf("%2d  %-32s %5d  %s\n", i + 1, r->ssid, r->canal,
           r->tem_bssid ? r->bssid : "(auto)");
  }
  if (n_vistas) {
    printf("\nNo ar (ultimo net wifi start):\n");
    for (i = 0; i < n_vistas; i++)
      printf(" %c %s  %-28s %5d  %u beacons\n", vistas[i].casa ? '*' : ' ',
             vistas[i].bssid, vistas[i].ssid[0] ? vistas[i].ssid : "(oculta)",
             vistas[i].canal, vistas[i].beacons);
  }
}

/* ---------- main ---------- */
int main(int argc, char** argv) {
  int so_lista = 0;
  if (argc > 1 && argv[1][0] == '-' && (argv[1][1] == 'l' || argv[1][1] == 'L')) so_lista = 1;
  carrega_cfg();
  carrega_log();
  if (so_lista || tela_inicia() != 0) {
    lista();
    return 0;
  }
  for (;;) {
    int k;
    desenha();
    tela_mostra();
    k = tecla(0);
    if (k == K_NADA) continue;
    if (k == K_EOF) break;
    if (k == K_ESC || k == 'q' || k == 'Q') {
      if (aba == 2) {
        aba = 0;
        continue;
      }
      break;
    }
    if (k == K_TAB || k == K_F(3)) {
      aba = (aba + 1) % 3;
      diz("aba trocada");
      continue;
    }
    if (k == K_UP) {
      if (aba == 0 && sel > 0) sel--;
      else if (aba == 1 && sel_vista > 0) sel_vista--;
      continue;
    }
    if (k == K_DOWN) {
      if (aba == 0 && sel + 1 < n_redes) sel++;
      else if (aba == 1 && sel_vista + 1 < n_vistas) sel_vista++;
      continue;
    }
    if (k == K_HOME) {
      if (aba == 0) sel = 0;
      else if (aba == 1) sel_vista = 0;
      continue;
    }
    if (k == K_END) {
      if (aba == 0 && n_redes) sel = n_redes - 1;
      else if (aba == 1 && n_vistas) sel_vista = n_vistas - 1;
      continue;
    }
    if (k == K_PGUP) {
      if (aba == 0 && sel > 0) sel = sel > 10 ? sel - 10 : 0;
      else if (aba == 1 && sel_vista > 0) sel_vista = sel_vista > 12 ? sel_vista - 12 : 0;
      continue;
    }
    if (k == K_PGDN) {
      if (aba == 0 && sel + 10 < n_redes) sel += 10;
      else if (aba == 0 && n_redes) sel = n_redes - 1;
      else if (aba == 1 && sel_vista + 12 < n_vistas) sel_vista += 12;
      else if (aba == 1 && n_vistas) sel_vista = n_vistas - 1;
      continue;
    }
    if (k == K_ENTER) {
      if (aba == 1) acao(2);
      else if (aba == 0 && n_redes) acao(6);
      else aba = 0;
      continue;
    }
    if (k == '1') break;
    if (k >= '1' && k <= '9') {
      acao(k - '0');
      continue;
    }
    if (k == '0') {
      acao(10);
      continue;
    }
    if (k == 'a' || k == 'A') acao(4);
    else if (k == 'd' || k == 'D') acao(5);
    else if (k == 'e' || k == 'E') acao(6);
    else if (k == 'g' || k == 'G') acao(3);
    else if (k == 'c' || k == 'C') acao(2);
  }
  tela_fim();
  return 0;
}
