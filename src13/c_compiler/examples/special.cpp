/* special.cpp - simulacao de um doutor de disco dos anos 90 no modo texto.
 * As bordas, botoes, caixas, barras e o mapa de blocos sao desenhos trocados
 * com k_font_set; a fonte volta ao padrao na saida. Nenhum disco e lido nem
 * alterado: unidades, erros e blocos sao inventados.
 * Dentro do SO: CC SPECIAL.CPP e SPECIAL. Nao funciona no WIN.
 *
 * Os desenhos usam os codigos 0xC0-0xDF porque so neles a VGA repete a ultima
 * coluna na nona: fora dali os riscos horizontais teriam furos. Enquanto o
 * programa roda, os acentos dessa faixa (ã, Ã, Ê, Í...) nao aparecem. */
#include <stdio.h>
#include <string.h>
#include <so.h>

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
#define G_PB1 0xD1
#define G_PB2 0xD2
#define G_PB3 0xD3
#define G_PB4 0xD4
#define G_MAPF 0xD5
#define G_MAPU 0xD6
#define G_MAPB 0xD7
#define G_ARROW 0xD8
#define G_DESK 0xD9

#define N_GLIFOS 26
#define G_BASE 0xC0

/* Um byte por linha, bit 7 a esquerda. */
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
  /* bloco usado */
  {0x00, 0x00, 0x00, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x00, 0x00, 0x00},
  /* bloco defeituoso */
  {0x00, 0x00, 0x00, 0x82, 0xC6, 0x6C, 0x38, 0x38, 0x38, 0x6C, 0xC6, 0x82, 0x00, 0x00, 0x00, 0x00},
  /* seta do menu */
  {0x00, 0x00, 0x00, 0x00, 0x20, 0x30, 0x38, 0x3C, 0x3C, 0x38, 0x30, 0x20, 0x00, 0x00, 0x00, 0x00},
  /* fundo: riscos horizontais nao mostram a nona coluna */
  {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00},
};

/* Liga o fundo claro e os desenhos; no fim do bloco devolve os dois. */
class FonteBorda {
  unsigned char salvo[N_GLIFOS][16];
  int ok;
public:
  FonteBorda() {
    int i;
    ok = k_blink(0) == 0;
    for (i = 0; i < N_GLIFOS; i++)
      if (k_font_get(G_BASE + i, salvo[i]) != 0) ok = 0;
    for (i = 0; ok && i < N_GLIFOS; i++)
      if (k_font_set(G_BASE + i, glifos[i]) != 0) ok = 0;
  }
  ~FonteBorda() {
    int i;
    for (i = 0; i < N_GLIFOS; i++) k_font_set(G_BASE + i, salvo[i]);
    k_blink(1);
  }
  int pronta() { return ok; }
};

/* ---------- tela ---------- */

#define COR(fundo, frente) (((fundo) << 4) | (frente))
#define PRETO 0
#define AZUL 1
#define CIANO 3
#define CINZA 7
#define ESCURO 8
#define AZUL_CLARO 9
#define CIANO_CLARO 11
#define BRANCO 15

/* Fundo de 8 a 15: k_blink(0) esta ligado. */
#define C_JANELA COR(AZUL_CLARO, BRANCO)
#define C_BORDA COR(AZUL_CLARO, BRANCO)
#define C_TITULO COR(BRANCO, AZUL)
#define C_FORTE COR(AZUL_CLARO, BRANCO)
#define C_FRACO COR(AZUL_CLARO, AZUL)
#define C_SELECAO COR(BRANCO, AZUL)
#define C_BARRAS COR(CIANO, PRETO)
#define C_FUNDO COR(PRETO, AZUL)

#define K_UP 0x100
#define K_DOWN 0x101
#define K_LEFT 0x102
#define K_RIGHT 0x103
#define K_ESC 27
#define K_ENTER '\n'

static unsigned short tela[25 * 80];
static unsigned short copia[25 * 80];

static void mostra(void) { k_vput(0, 0, tela, 25 * 80); }

static void poe(int r, int c, int ch, int cor) {
  if (r < 0 || r >= 25 || c < 0 || c >= 80) return;
  tela[r * 80 + c] = (unsigned short)((ch & 0xFF) | (cor << 8));
}

static void texto(int r, int c, const char* s, int cor) {
  while (*s) poe(r, c++, (unsigned char)*s++, cor);
}

static void linha(int r, int c, int n, int ch, int cor) {
  while (n-- > 0) poe(r, c++, ch, cor);
}

static int tam(const char* s) { return (int)strlen(s); }

/* Espera tecla. Fim da entrada conta como ESC. */
static int tecla(void) {
  int k;
  mostra();
  k = k_getc();
  if (k < 0) return K_ESC;
  if (k == '\r') return K_ENTER;
  return k;
}

/* Mostra e espera ms; 1 se apertaram ESC. */
static int pausa(int ms) {
  unsigned t;
  mostra();
  t = k_ms();
  while (k_ms() - t < (unsigned)ms) {
    if (k_trygetc() == K_ESC) return 1;
    k_yield();
  }
  return 0;
}

static void fundo(const char* ajuda) {
  int r;
  for (r = 1; r < 24; r++) linha(r, 0, 80, G_DESK, C_FUNDO);
  linha(0, 0, 80, ' ', C_BARRAS);
  texto(0, 2, "Doutor de Disco", COR(CIANO, BRANCO));
  texto(0, 58, "SIMULACAO - sem disco", C_BARRAS);
  linha(24, 0, 80, ' ', C_BARRAS);
  texto(24, 2, ajuda, C_BARRAS);
}

/* Janela com sombra preta, barra de titulo e risco na beira das celulas. */
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

/* Barra de w celulas, cada uma com 4 passos. */
static void barra(int r, int c, int w, int feito, int total) {
  int n = total ? (int)((long long)feito * w * 4 / total) : 0;
  int i;
  for (i = 0; i < w; i++) {
    int v = n - i * 4;
    if (v < 0) v = 0;
    if (v > 4) v = 4;
    poe(r, c + i, G_PB0 + v, COR(AZUL_CLARO, BRANCO));
  }
}

/* Caixa de pergunta sobre a tela atual. 1 = primeiro botao; 0 = segundo ou ESC. */
static int pergunta(const char* titulo, const char* l1, const char* l2,
                    const char* b1, const char* b2) {
  int r = 8, h = 8, w = 52, c = 14, sel = 0, res;
  memcpy(copia, tela, sizeof tela);
  janela(r, c, h, w, titulo);
  texto(r + 2, c + 3, l1, C_FORTE);
  texto(r + 3, c + 3, l2, C_JANELA);
  separador(r + 4, c, w);
  for (;;) {
    int k;
    int x1 = c + 8;
    int x2 = c + w - 8 - tam(b2) - 4;
    botao(r + 5, x1, b1, sel == 0);
    if (b2[0]) botao(r + 5, x2, b2, sel == 1);
    k = tecla();
    if (k == K_ENTER) { res = sel == 0; break; }
    if (k == K_ESC) { res = 0; break; }
    if (b2[0] && (k == K_LEFT || k == K_RIGHT || k == '\t')) sel = !sel;
  }
  memcpy(tela, copia, sizeof tela);
  return res;
}

/* ---------- menu e unidade ---------- */

static const char* itens[5] = {
  "Diagnosticar disco", "Testar a superficie", "Desfazer reparo", "Opcoes", "Sair"
};
static const char* ajudas[5] = {
  "Confere particao, boot, FAT, pastas e arquivos",
  "Le todos os blocos e marca os defeituosos",
  "Volta o disco ao estado anterior ao reparo",
  "Escolhe como o teste se comporta",
  "Volta ao prompt e restaura a fonte"
};

static int menu(void) {
  static int sel = 0;
  for (;;) {
    int i, k;
    int r = 6, c = 22, w = 36;
    fundo(ajudas[sel]);
    janela(r, c, 12, w, "Doutor de Disco");
    texto(r + 2, c + 5, "Escolha o que examinar:", C_JANELA);
    separador(r + 3, c, w);
    for (i = 0; i < 5; i++) {
      int y = r + 5 + i;
      if (i == sel) {
        linha(y, c + 4, w - 8, ' ', C_SELECAO);
        poe(y, c + 5, G_ARROW, C_SELECAO);
        texto(y, c + 7, itens[i], C_SELECAO);
      } else {
        texto(y, c + 7, itens[i], C_FORTE);
      }
    }
    k = tecla();
    if (k == K_UP) sel = (sel + 4) % 5;
    else if (k == K_DOWN) sel = (sel + 1) % 5;
    else if (k == K_ENTER) return sel;
    else if (k == K_ESC) return 4;
  }
}

static const char* unidades[3] = {
  "A:  disquete 3 1/2   1,44 MB",
  "C:  disco fixo        340 MB",
  "D:  disco fixo        1,2 GB"
};

/* 'A', 'C', 'D', ou 0 se cancelou. */
static int escolhe_unidade(void) {
  static int sel = 1;
  for (;;) {
    int i, k;
    int r = 7, c = 20, w = 40;
    fundo("Setas escolhem   Enter segue   ESC volta");
    janela(r, c, 11, w, "Unidade");
    texto(r + 2, c + 4, "Qual unidade examinar?", C_JANELA);
    for (i = 0; i < 3; i++) {
      poe(r + 4 + i, c + 4, i == sel ? G_RADON : G_RAD, C_BORDA);
      texto(r + 4 + i, c + 6, unidades[i], i == sel ? C_FORTE : C_JANELA);
    }
    separador(r + 8, c, w);
    botao(r + 9, c + 15, "Seguir", 1);
    k = tecla();
    if (k == K_UP) sel = (sel + 2) % 3;
    else if (k == K_DOWN) sel = (sel + 1) % 3;
    else if (k == K_ENTER) return unidades[sel][0];
    else if (k == K_ESC) return 0;
  }
}

/* ---------- opcoes ---------- */

static const char* nomes_opcao[3] = {
  "Testar so a area usada", "Reparar sem perguntar", "Teste lento"
};
static int opcoes[3] = { 0, 0, 0 };

static void tela_opcoes(void) {
  int sel = 0;
  for (;;) {
    int i, k;
    int r = 7, c = 20, w = 40;
    fundo("Setas escolhem   Espaco marca   ESC volta");
    janela(r, c, 9, w, "Opcoes");
    for (i = 0; i < 3; i++) {
      int cor = i == sel ? C_SELECAO : C_FORTE;
      if (i == sel) linha(r + 2 + i, c + 3, w - 6, ' ', C_SELECAO);
      poe(r + 2 + i, c + 4, opcoes[i] ? G_CHKON : G_CHK, i == sel ? C_SELECAO : C_BORDA);
      texto(r + 2 + i, c + 6, nomes_opcao[i], cor);
    }
    separador(r + 6, c, w);
    botao(r + 7, c + 17, "OK", 1);
    k = tecla();
    if (k == K_UP) sel = (sel + 2) % 3;
    else if (k == K_DOWN) sel = (sel + 1) % 3;
    else if (k == ' ') opcoes[sel] = !opcoes[sel];
    else if (k == K_ENTER || k == K_ESC) return;
  }
}

/* ---------- diagnostico ---------- */

static int rel_feito, rel_perdidos, rel_blocos, rel_ruins, rel_unidade;

static const char* fases[6] = {
  "Tabela de particoes", "Registro de boot", "Tabela de alocacao (FAT)",
  "Estrutura de pastas", "Estrutura de arquivos", "Clusters perdidos"
};

/* 0 se cancelou. */
static int diagnostico(int unidade) {
  int r = 4, c = 14, w = 52, h = 16, f, i;
  char buf[48];
  fundo("ESC interrompe");
  sprintf(buf, "Diagnostico de %c:", unidade);
  janela(r, c, h, w, buf);
  for (i = 0; i < 6; i++) texto(r + 2 + i, c + 7, fases[i], C_FRACO);
  separador(r + 9, c, w);
  for (f = 0; f < 6; f++) {
    int passo;
    poe(r + 2 + f, c + 4, G_ARROW, C_BORDA);
    texto(r + 2 + f, c + 7, fases[f], C_FORTE);
    for (passo = 0; passo < 12; passo++) {
      int feito = f * 12 + passo + 1;
      sprintf(buf, "Examinando: %-28s", fases[f]);
      texto(r + 10, c + 4, buf, C_JANELA);
      barra(r + 12, c + 4, w - 13, feito, 72);
      sprintf(buf, "%3d%%", feito * 100 / 72);
      texto(r + 12, c + w - 8, buf, C_FORTE);
      if (pausa(50)) return 0;
    }
    poe(r + 2 + f, c + 4, G_TICK, C_BORDA);
    if (f == 5) {
      texto(r + 2 + f, c + 36, "3 cadeias", COR(AZUL_CLARO, PRETO));
      if (opcoes[1] || pergunta("Problema encontrado",
                                "Ha 3 cadeias de clusters perdidos.",
                                "Salvar como FILE0000.CHK e liberar o espaco?",
                                "Reparar", "Ignorar")) {
        rel_perdidos = 3;
        texto(r + 2 + f, c + 36, "reparado ", C_BORDA);
      }
    } else {
      texto(r + 2 + f, c + 36, "ok", C_BORDA);
    }
  }
  texto(r + 10, c + 4, "Diagnostico concluido.                  ", C_FORTE);
  botao(r + 14, c + 20, "Continuar", 1);
  tecla();
  rel_feito = 1;
  return 1;
}

/* ---------- superficie ---------- */

#define MAPA_L 8
#define MAPA_C 60

/* Sequencia fixa: o mapa sai sempre igual. */
static unsigned sorteio;
static unsigned proximo(void) {
  sorteio = sorteio * 1103515245u + 12345u;
  return (sorteio >> 16) & 0x7FFF;
}

/* 0 se cancelou. */
static int superficie(int unidade) {
  int r = 2, c = 8, w = 64, h = 20, i, ruins = 0;
  int total = MAPA_L * MAPA_C;
  unsigned t0 = k_ms();
  char buf[64];
  fundo("ESC interrompe");
  sprintf(buf, "Teste de superficie de %c:", unidade);
  janela(r, c, h, w, buf);
  sorteio = 1990;
  for (i = 0; i < total; i++)
    poe(r + 2 + i / MAPA_C, c + 2 + i % MAPA_C, G_MAPF, C_FRACO);
  separador(r + 10, c, w);
  poe(r + 11, c + 4, G_MAPU, COR(AZUL_CLARO, CIANO_CLARO));
  texto(r + 11, c + 6, "usado", C_JANELA);
  poe(r + 11, c + 15, G_MAPF, C_FRACO);
  texto(r + 11, c + 17, "livre", C_JANELA);
  poe(r + 11, c + 26, G_MAPB, COR(AZUL_CLARO, PRETO));
  texto(r + 11, c + 28, "defeituoso", C_JANELA);
  for (i = 0; i < total; i++) {
    int y = r + 2 + i / MAPA_C, x = c + 2 + i % MAPA_C;
    int ruim = i == 137 || i == 138 || i == 402;
    int usado = (proximo() % 100) < 72;
    if (ruim) {
      ruins++;
      poe(y, x, G_MAPB, COR(AZUL_CLARO, PRETO));
    } else if (usado) {
      poe(y, x, G_MAPU, COR(AZUL_CLARO, CIANO_CLARO));
    }
    if (i % 8 == 7 || i == total - 1) {
      unsigned s = (k_ms() - t0) / 1000;
      barra(r + 13, c + 4, w - 13, i + 1, total);
      sprintf(buf, "%3d%%", (i + 1) * 100 / total);
      texto(r + 13, c + w - 8, buf, C_FORTE);
      sprintf(buf, "Blocos: %4d de %d   Defeituosos: %d   Tempo: %02u:%02u",
              i + 1, total, ruins, s / 60, s % 60);
      texto(r + 15, c + 4, buf, C_JANELA);
      if (pausa((opcoes[2] ? 70 : 35) / (opcoes[0] ? 2 : 1))) return 0;
    }
  }
  rel_blocos = total;
  rel_ruins = ruins;
  botao(r + 17, c + 25, "Continuar", 1);
  tecla();
  return 1;
}

/* ---------- relatorio ---------- */

static void relatorio(void) {
  int r = 6, c = 16, w = 48;
  char buf[48];
  fundo("Enter volta ao menu");
  sprintf(buf, "Relatorio de %c:", rel_unidade);
  janela(r, c, 13, w, buf);
  poe(r + 2, c + 4, G_TICK, C_BORDA);
  texto(r + 2, c + 6, rel_feito ? "Diagnostico concluido" : "Diagnostico nao executado", C_FORTE);
  sprintf(buf, "Clusters perdidos reparados: %d", rel_perdidos);
  texto(r + 3, c + 6, buf, C_JANELA);
  poe(r + 5, c + 4, G_TICK, C_BORDA);
  texto(r + 5, c + 6, rel_blocos ? "Superficie testada" : "Superficie nao testada", C_FORTE);
  sprintf(buf, "Blocos %d, defeituosos %d", rel_blocos, rel_ruins);
  texto(r + 6, c + 6, buf, C_JANELA);
  texto(r + 8, c + 6, "Simulacao: nenhum disco foi tocado.", C_FRACO);
  separador(r + 10, c, w);
  botao(r + 11, c + 21, "OK", 1);
  tecla();
}

static void executa(int testa_diag) {
  int u = escolhe_unidade();
  if (!u) return;
  rel_unidade = u;
  rel_feito = rel_perdidos = rel_blocos = rel_ruins = 0;
  if (testa_diag) {
    if (!diagnostico(u)) return;
    if (pergunta("Superficie", "Diagnostico concluido.",
                 "Testar tambem a superficie do disco?", "Testar", "Pular")) {
      if (!superficie(u)) return;
    }
  } else if (!superficie(u)) {
    return;
  }
  relatorio();
}

int main(void) {
  int fim = 0;
  if (k_vput(0, 0, tela, 0) != 0) {
    printf("SPECIAL so funciona na tela de texto, fora do WIN\n");
    return 1;
  }
  {
    FonteBorda fonte;
    if (!fonte.pronta()) {
      printf("nao consegui trocar a fonte\n");
      return 1;
    }
    k_cursor(-1, 0);
    while (!fim) {
      int op = menu();
      if (op == 0) executa(1);
      else if (op == 1) executa(0);
      else if (op == 2) {
        pergunta("Desfazer reparo", "Nao ha reparo para desfazer.",
                 "Rode um diagnostico primeiro.", "OK", "");
      } else if (op == 3) tela_opcoes();
      else fim = 1;
    }
  }
  k_clear();
  k_cursor(0, 0);
  return 0;
}
