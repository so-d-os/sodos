/* setup.c - SETUP.EXE: instala o SODOS nas pastas certas com um botao.
 *
 * Roda no WIN (GUI.DLL + widgets). Le os arquivos de <pasta do SETUP.EXE>\
 * ARQUIVOS e chama install() (install.c), que tem a lista do que vai para
 * onde. A barra mostra os bytes copiados.
 *
 *   SETUP                    instala no D:
 *   SETUP D:\TEMP\TESTE      teste: grava tudo debaixo dessa pasta
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui.h>
#include <widgets.h>
#include <so.h>
#include "install.h"

#define ESPERA  0  /* pronto para instalar */
#define COPIA   1  /* copiando */
#define PRONTO  2  /* terminou (bem ou com erros) */

static int g_estado = ESPERA;
static char g_origem[300];
static char g_raiz[300];     /* vazio = D: de verdade */
static long g_total;
static int g_tem_origem;
static int g_pct;
static char g_arquivo[300];
static char g_contagem[64];
static char g_status[200];
static inst_resultado_t g_res;
static int g_mx, g_my, g_down;
static int g_pronto_clique;  /* ignora o clique que abriu a janela */
static unsigned g_t_abriu;
static int g_fechar;
/* Redesenhar a janela inteira a cada movimento do mouse faz ela piscar:
 * so redesenha quando algo muda (g_sujo). */
static int g_sujo = 1;
static int g_bx, g_by, g_bw, g_bh;  /* o botao, para saber se o mouse entrou */
static int g_sobre;                 /* mouse em cima do botao */

static const char id_botao;

static void status(const char* s) {
  strncpy(g_status, s, sizeof(g_status) - 1);
  g_status[sizeof(g_status) - 1] = 0;
}

/* "D:\SETUP\SETUP.EXE" -> "D:\SETUP\ARQUIVOS\" */
static void acha_origem(const char* exe) {
  int i, barra = -1;
  for (i = 0; exe && exe[i]; i++)
    if (exe[i] == '\\' || exe[i] == '/') barra = i;
  if (barra < 0) {
    strcpy(g_origem, "ARQUIVOS\\");
    return;
  }
  if (barra + 11 >= (int)sizeof(g_origem)) barra = (int)sizeof(g_origem) - 12;
  memcpy(g_origem, exe, (size_t)barra + 1);
  strcpy(g_origem + barra + 1, "ARQUIVOS\\");
}

static void mb(char* out, int max, long b) {
  if (b >= 1024L * 1024L) snprintf(out, (size_t)max, "%ld,%ld MB", b / (1024L * 1024L), (b % (1024L * 1024L)) * 10 / (1024L * 1024L));
  else snprintf(out, (size_t)max, "%ld KB", (b + 1023) / 1024);
}

static void desenha(void) {
  const wg_theme_t* t = wg_theme();
  int sw = gui_width(), sh = gui_height(), ch = gui_char_h();
  int pad = 18, y = pad, cw = sw - 2 * pad;
  int bh = t->btn_h + 10, bw = 200;
  int rodape = ch + 26;
  char tam[32], lin[360];

  wg_input(g_mx, g_my, g_down);
  gui_fill(0, 0, sw, sh, t->bg);

  /* cabecalho */
  gui_fill(0, 0, sw, ch * 2 + 2 * pad, t->accent);
  gui_text(pad, pad, "Instalar o SODOS", -1, 0xFFFFFF, t->accent, GUI_TEXT_NOBG | GUI_TEXT_BOLD);
  gui_text(pad, pad + ch + 4, "Programas, bibliotecas e fontes nas pastas certas", -1, 0xFFFFFF,
           t->accent, GUI_TEXT_NOBG);
  y = ch * 2 + 2 * pad + pad;

  wg_card(pad, y, cw, ch * 7 + 24);
  {
    int x = pad + 14, yy = y + 12;
    wg_label(x, yy, "Para onde vai cada coisa:");
    yy += ch + 6;
    wg_label_dim(x, yy, "D:\\SODOS\\BIN           comandos (git, make, top, wifi...)");
    yy += ch + 2;
    wg_label_dim(x, yy, "D:\\SODOS               programas do sistema");
    yy += ch + 2;
    wg_label_dim(x, yy, "D:\\SODOS\\DEV\\SRC       fontes da libc, do cc e do browser");
    yy += ch + 2;
    wg_label_dim(x, yy, "D:\\SODOS\\DEV\\EXEMPLOS  exemplos do cc");
    yy += ch + 2;
    wg_label_dim(x, yy, "D:\\                    o que o sistema procura na raiz");
  }
  y += ch * 7 + 24 + pad;

  if (g_raiz[0]) {
    snprintf(lin, sizeof(lin), "Teste: grava em %s (o D: nao muda)", g_raiz);
    wg_label(pad, y, lin);
    y += ch + 8;
  }

  /* barra */
  {
    char cap[16];
    snprintf(cap, sizeof(cap), "%d%%", g_pct);
    wg_progress(pad, y, cw, 26, g_pct, cap);
    y += 26 + 8;
  }
  if (g_estado == COPIA) {
    gui_text_fit(pad, y, g_arquivo, cw, t->text_dim, t->bg, GUI_TEXT_NOBG);
  } else if (g_estado == PRONTO) {
    if (g_res.parado)
      snprintf(lin, sizeof(lin), "Parado: %d arquivo(s) copiados.", g_res.copiados);
    else if (g_res.erros)
      snprintf(lin, sizeof(lin), "Terminou com %d erro(s): %s", g_res.erros, g_res.ultimo_erro);
    else
      snprintf(lin, sizeof(lin), "Pronto: %d arquivo(s) instalados%s.", g_res.copiados,
               g_res.path_ok ? ", D:\\SODOS\\BIN no PATH" : "");
    gui_text_fit(pad, y, lin, cw, g_res.erros ? 0xC03030 : t->text, t->bg, GUI_TEXT_NOBG);
    if (g_res.faltando || g_res.mantidos) {
      snprintf(lin, sizeof(lin), "%d nao estavam no pacote; %d configuracao(oes) do usuario mantida(s).",
               g_res.faltando, g_res.mantidos);
      gui_text_fit(pad, y + ch + 4, lin, cw, t->text_dim, t->bg, GUI_TEXT_NOBG);
    }
  } else {
    mb(tam, (int)sizeof(tam), g_total);
    if (g_tem_origem)
      snprintf(lin, sizeof(lin), "%d arquivos, %s, de %s", install_total(), tam, g_origem);
    else
      snprintf(lin, sizeof(lin), "Falta a pasta %s com os arquivos", g_origem);
    gui_text_fit(pad, y, lin, cw, g_tem_origem ? t->text_dim : 0xC03030, t->bg, GUI_TEXT_NOBG);
  }

  /* o botao */
  {
    int bx = (sw - bw) / 2, by = sh - rodape - bh - pad;
    const char* rot = g_estado == ESPERA ? "Instalar" : g_estado == COPIA ? "Instalando..." : "Fechar";
    int ativo = g_pronto_clique && (g_estado == PRONTO || (g_estado == ESPERA && g_tem_origem));
    g_bx = bx; g_by = by; g_bw = bw; g_bh = bh;
    if (wg_button_primary(&id_botao, bx, by, bw, bh, rot, ativo)) {
      if (g_estado == ESPERA) g_estado = COPIA; /* o laco comeca a copia */
      else if (g_estado == PRONTO) g_fechar = 1;
    }
  }
  wg_status(0, sh - rodape, sw, g_status, g_contagem);
  gui_present();
}

/* trata os eventos que chegaram; 1 = fechou a janela */
static int eventos(int espera_ms) {
  gui_event_t ev;
  while (gui_wait(&ev, espera_ms) != GUI_EV_NONE) {
    espera_ms = 0;
    if (ev.type != GUI_EV_MOVE && ev.type != GUI_EV_DRAG) g_sujo = 1;
    if (ev.type == GUI_EV_CLOSE) {
      printf("SETUP: janela fechada (X)\n");
      return 1;
    }
    /* aperta e solta: um quadro para cada, senao o botao nao ve o clique */
    if (ev.type == GUI_EV_DOWN) { g_mx = ev.x; g_my = ev.y; g_down = 1; break; }
    else if (ev.type == GUI_EV_UP) { g_mx = ev.x; g_my = ev.y; g_down = 0; break; }
    else if (ev.type == GUI_EV_DRAG || ev.type == GUI_EV_MOVE) {
      int sobre = gui_hit(g_bx, g_by, g_bw, g_bh, ev.x, ev.y);
      g_mx = ev.x; g_my = ev.y;
      /* so o realce do botao muda com o mouse */
      if (sobre != g_sobre) { g_sobre = sobre; g_sujo = 1; }
      continue;
    }
    else if (ev.type == GUI_EV_KEY && ev.key == 27 && g_estado != COPIA) {
      printf("SETUP: Esc\n");
      return 1;
    }
    else if (ev.type == GUI_EV_KEY && (ev.key == '\r' || ev.key == ' ') && g_estado == ESPERA &&
             g_tem_origem && g_pronto_clique)
      g_estado = COPIA;
    if (g_estado == COPIA) break; /* sem fila: a copia redesenha sozinha */
  }
  return 0;
}

/* chamado pelo install(): atualiza a barra (no maximo a cada 80 ms) */
static int progresso(long feitos, long total, const char* arq, int n, int n_total, void* ud) {
  static unsigned ultimo;
  unsigned agora = k_ms();
  int pct = total > 0 ? (int)((feitos / 1024) * 100 / (total / 1024 > 0 ? total / 1024 : 1)) : 0;
  (void)ud;
  if (pct > 100) pct = 100;
  g_pct = pct;
  if (arq && arq[0]) snprintf(g_arquivo, sizeof(g_arquivo), "Copiando %s", arq);
  snprintf(g_contagem, sizeof(g_contagem), "%d de %d", n, n_total);
  if (agora - ultimo < 80 && feitos < total) return 0;
  ultimo = agora;
  desenha();
  if (eventos(0)) return 1; /* fechou a janela no meio: para */
  return 0;
}

int main(int argc, char** argv) {
  if (gui_open_pct("Instalar o SODOS", 44, 56) != 0) {
    printf("SETUP: abra pelo WIN (Iniciar > Executar ou o Explorer)\n");
    return 1;
  }
  acha_origem(argc > 0 ? argv[0] : 0);
  if (argc > 1 && argv[1][0]) {
    strncpy(g_raiz, argv[1], sizeof(g_raiz) - 1);
    g_raiz[sizeof(g_raiz) - 1] = 0;
  }
  g_total = install_bytes(g_origem);
  g_tem_origem = g_total > 0;
  status(g_tem_origem ? "Clique em Instalar." : "Pacote vazio: rode o empacota.sh");
  snprintf(g_contagem, sizeof(g_contagem), "%d arquivos", install_total());
  g_t_abriu = k_ms();

  while (!g_fechar) {
    if (!g_pronto_clique && k_ms() - g_t_abriu > 400) { g_pronto_clique = 1; g_sujo = 1; }
    if (g_sujo) {
      g_sujo = 0;
      desenha();
      /* o clique solto e tratado no desenho: mais um quadro com o estado novo */
      if (g_fechar || g_estado == COPIA) g_sujo = 1;
    }
    if (g_estado == COPIA) {
      status("Instalando... nao desligue o computador.");
      g_down = 0;
      install(g_origem, g_raiz[0] ? g_raiz : 0, progresso, 0, &g_res);
      g_estado = PRONTO;
      if (!g_res.parado) g_pct = 100;
      status(g_res.erros ? "Terminou com erros." :
             g_res.parado ? "Instalacao parada." : "Instalado. Reinicie para valer em tudo.");
      if (g_res.parado) break; /* fechou a janela durante a copia */
      g_sujo = 1;
      continue;
    }
    if (eventos(g_pronto_clique ? 200 : 60)) break;
  }
  gui_close();
  return 0;
}
