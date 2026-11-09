/* tela.c - Tela de celulas, fonte com os desenhos e as pecas do padrao do
 * SPECIAL: janela com sombra, separador, botao, barra e caixa de aviso.
 * Desenha numa copia em memoria; tela_mostra manda so as linhas que mudaram. */
#include "top.h"

/* Um byte por linha, bit 7 a esquerda. Os 26 primeiros sao os do SPECIAL. */
static const unsigned char glifos[N_GLIFOS][16] = {
  /* canto inferior esquerdo */
  {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF},
  /* canto inferior direito */
  {0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0xFF, 0xFF},
  /* risco de baixo, na ultima linha da celula */
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF},
  /* risco esquerdo, na primeira coluna */
  {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0},
  /* risco direito, na ultima coluna */
  {0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03},
  /* caixa do canto do titulo */
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  /* separador, ponta esquerda */
  {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0},
  /* separador */
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  /* separador, ponta direita */
  {0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0xFF, 0xFF, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03},
  /* botao, ponta redonda */
  {0x0F, 0x3F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x3F, 0x0F},
  /* botao, ponta redonda */
  {0xF0, 0xFC, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFC, 0xF0},
  /* caixa vazia */
  {0x00, 0x00, 0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00, 0x00, 0x00},
  /* caixa marcada */
  {0x00, 0x00, 0x00, 0x7E, 0x42, 0x42, 0x5A, 0x5A, 0x5A, 0x5A, 0x42, 0x42, 0x7E, 0x00, 0x00, 0x00},
  /* opcao vazia */
  {0x00, 0x00, 0x00, 0x00, 0x3C, 0x42, 0x81, 0x81, 0x81, 0x81, 0x42, 0x3C, 0x00, 0x00, 0x00, 0x00},
  /* opcao marcada */
  {0x00, 0x00, 0x00, 0x00, 0x3C, 0x42, 0x99, 0x99, 0x99, 0x99, 0x42, 0x3C, 0x00, 0x00, 0x00, 0x00},
  /* fase concluida */
  {0x00, 0x00, 0x00, 0x03, 0x06, 0x06, 0x0C, 0xCC, 0xD8, 0x78, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00},
  /* barra: 0/4 */
  {0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00},
  /* barra: 1/4 */
  {0x00, 0x00, 0x00, 0xFF, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xFF, 0x00, 0x00, 0x00},
  /* barra: 2/4 */
  {0x00, 0x00, 0x00, 0xFF, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xFF, 0x00, 0x00, 0x00},
  /* barra: 3/4 */
  {0x00, 0x00, 0x00, 0xFF, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFF, 0x00, 0x00, 0x00},
  /* barra: 4/4 */
  {0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00},
  /* bloco livre */
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  /* bloco ocupado */
  {0x00, 0x00, 0x00, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x00, 0x00, 0x00},
  /* bloco marcado: moldura cheia, para o cursor */
  {0x00, 0x00, 0xFF, 0x81, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0x81, 0xFF, 0x00, 0x00},
  /* seta do menu */
  {0x00, 0x00, 0x00, 0x00, 0x20, 0x30, 0x38, 0x3C, 0x3C, 0x38, 0x30, 0x20, 0x00, 0x00, 0x00, 0x00},
  /* fundo: riscos horizontais nao mostram a nona coluna */
  {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00},
  /* bloco meio ocupado: so a metade de baixo */
  {0x00, 0x00, 0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x00, 0x00, 0x00},
  /* nao e RAM: riscos inclinados */
  {0x00, 0x00, 0x00, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x00, 0x00, 0x00},
  /* seta para baixo da coluna ordenada */
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x7C, 0x38, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

static unsigned char salvo[N_GLIFOS][16];
static unsigned short tela[25 * 80];
static unsigned short vista[25 * 80];
static unsigned short copia[25 * 80];

int tela_inicia(void) {
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

void tela_fim(void) {
  int i;
  for (i = 0; i < N_GLIFOS; i++) k_font_set(G_BASE + i, salvo[i]);
  k_blink(1);
  k_clear();
  k_cursor(0, 0);
}

void tela_mostra(void) {
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

void poe(int r, int c, int ch, int cor) {
  if (r < 0 || r >= 25 || c < 0 || c >= 80) return;
  tela[r * 80 + c] = (unsigned short)((ch & 0xFF) | ((cor & 0xFF) << 8));
}

void texto(int r, int c, const char* s, int cor) {
  while (*s) poe(r, c++, (unsigned char)*s++, cor);
}

void textow(int r, int c, const char* s, int w, int cor) {
  int i = 0;
  while (i < w && s[i]) {
    poe(r, c + i, (unsigned char)s[i], cor);
    i++;
  }
  while (i < w) poe(r, c + i++, ' ', cor);
}

void linha(int r, int c, int n, int ch, int cor) {
  while (n-- > 0) poe(r, c++, ch, cor);
}

void pinta(int r, int c, int n, int cor) {
  for (; n > 0; n--, c++) {
    if (r < 0 || r >= 25 || c < 0 || c >= 80) continue;
    tela[r * 80 + c] = (unsigned short)((tela[r * 80 + c] & 0xFF) | ((cor & 0xFF) << 8));
  }
}

int tam(const char* s) { return (int)strlen(s); }

void fundo(void) {
  int r;
  for (r = 1; r < 24; r++) linha(r, 0, 80, G_DESK, C_FUNDO);
}

/* Como o menu do MC: abas na barra ciano, a ativa em preto. */
void menu_topo(int aba) {
  static const char* abas[4] = { " Processos ", " Mapa ", " Video ", " CPU " };
  char buf[32];
  int c = 7, i, t;
  linha(0, 0, 80, ' ', C_MENU);
  texto(0, 1, "TOP", COR(CIANO, BRANCO));
  for (i = 0; i < 4; i++) {
    int cor = i == aba ? C_MENUSEL : C_MENU;
    texto(0, c, abas[i], cor);
    poe(0, c + 1, (unsigned char)abas[i][1], i == aba ? COR(PRETO, AMARELO) : C_MENUHOT);
    c += tam(abas[i]) + 1;
  }
  t = k_time();
  if (t >= 0) {
    sprintf(buf, "%02d:%02d:%02d UTC", t / 10000, t / 100 % 100, t % 100);
    texto(0, 66, buf, C_MENU);
  }
  {
    unsigned s = k_ms() / 1000;
    sprintf(buf, "ligado %u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    texto(0, 46, buf, C_MENU);
  }
}

/* Barra do MC: numero cinza no preto, rotulo preto no ciano. */
void teclas(const char* const* rotulos) {
  int i, c = 0;
  char n[4];
  linha(24, 0, 80, ' ', C_ROTULO);
  for (i = 0; i < 10; i++) {
    int w = i == 9 ? 80 - c : 8;
    sprintf(n, "%d", i + 1);
    texto(24, c, n, C_TECLA);
    textow(24, c + tam(n), rotulos[i], w - tam(n), C_ROTULO);
    c += w;
  }
}

/* Janela com sombra preta, barra de titulo e risco na beira das celulas. */
void janela(int r, int c, int h, int w, const char* titulo) {
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

void separador(int r, int c, int w) {
  poe(r, c, G_SEPL, C_BORDA);
  linha(r, c + 1, w - 2, G_SEPH, C_BORDA);
  poe(r, c + w - 1, G_SEPR, C_BORDA);
}

void botao(int r, int c, const char* s, int sel) {
  int f = sel ? BRANCO : CINZA;
  poe(r, c, G_BTNL, COR(AZUL_CLARO, f));
  linha(r, c + 1, tam(s) + 2, ' ', COR(f, sel ? AZUL : PRETO));
  texto(r, c + 2, s, COR(f, sel ? AZUL : PRETO));
  poe(r, c + tam(s) + 3, G_BTNR, COR(AZUL_CLARO, f));
}

/* Barra de w celulas, cada uma com 4 passos. */
void barra(int r, int c, int w, unsigned feito, unsigned total) {
  int n = total ? (int)((unsigned long long)feito * (unsigned)w * 4u / total) : 0;
  int i;
  for (i = 0; i < w; i++) {
    int v = n - i * 4;
    if (v < 0) v = 0;
    if (v > 4) v = 4;
    poe(r, c + i, G_PB0 + v, COR(AZUL_CLARO, BRANCO));
  }
}

int tecla(int ms) {
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

void aviso(const char* titulo, const char* msg) {
  int n = 1, w = tam(titulo) + 8, l = 0, i, r, c, h;
  const char* p;
  for (p = msg; *p; p++) {
    if (*p == '\n') {
      n++;
      l = 0;
    } else if (++l + 8 > w) {
      w = l + 8;
    }
  }
  if (w > 76) w = 76;
  h = n + 6;
  r = (24 - h) / 2;
  c = (80 - w) / 2;
  memcpy(copia, tela, sizeof tela);
  janela(r, c, h, w, titulo);
  for (i = 0, p = msg; i < n; i++) {
    int k = 0;
    while (p[k] && p[k] != '\n') {
      poe(r + 2 + i, c + 4 + k, (unsigned char)p[k], i == 0 ? C_FORTE : C_JANELA);
      k++;
    }
    p += k;
    if (*p) p++;
  }
  separador(r + h - 3, c, w);
  botao(r + h - 2, c + (w - 6) / 2, "OK", 1);
  for (;;) {
    int k = tecla(0);
    if (k == K_ENTER || k == K_ESC || k == K_EOF || k == ' ') break;
  }
  memcpy(tela, copia, sizeof tela);
}
