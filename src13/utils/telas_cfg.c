/* telas_cfg.c - configurador das duas telas (TELAS.EXE), 2026-11-18.
 *
 * Escolhe como o WIN usa os dois monitores da placa (docs/GPU.md, etapa M):
 *  - Duplicado: a segunda tela mostra a mesma imagem (espelho);
 *  - Estendido: a area de trabalho fica com a largura das duas telas.
 * Grava telas=espelho|estendida na secao [video] do D:\SODOS.CFG (o mesmo
 * arquivo que diz qual driver a placa usa; as outras linhas ficam como
 * estao). Vale na proxima vez que o WIN liga o driver: sair com Esc e
 * entrar de novo. So funciona com um driver de duas telas (AMDDUAL.DLL).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui.h>
#include <widgets.h>
#include <so.h>

#define CFG_PATH "D:\\SODOS.CFG"
#define CFG_MAX 8192

static int g_modo;          /* 0 duplicado, 1 estendido */
static int g_tem_video;     /* o SODOS.CFG tem [video] */
static char g_status[160];
static int g_mx, g_my, g_down;
static int g_ready;

static const char id_dup, id_ext, id_apply, id_exit;

static void set_status(const char* s) {
  strncpy(g_status, s, sizeof(g_status) - 1);
  g_status[sizeof(g_status) - 1] = 0;
}

static int le_cfg(char* b, int max) {
  FILE* f = fopen(CFG_PATH, "rb");
  int n;
  if (!f) return -1;
  n = (int)fread(b, 1, (size_t)max - 1, f);
  fclose(f);
  if (n < 0) n = 0;
  b[n] = 0;
  return n;
}

/* inicio da linha seguinte */
static int prox_linha(const char* b, int i) {
  while (b[i] && b[i] != '\n') i++;
  if (b[i] == '\n') i++;
  return i;
}

static int comeca(const char* b, int i, const char* k) {
  int j = 0;
  while (k[j]) {
    char c = b[i + j];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != k[j]) return 0;
    j++;
  }
  return 1;
}

/* Acha a secao [video] e a linha telas= dela. *ini = linha logo depois de
 * [video]; *tl = inicio da linha telas= (-1 se nao ha). 0 = achou [video]. */
static int acha(const char* b, int* ini, int* tl) {
  int i = 0, dentro = 0;
  *ini = -1;
  *tl = -1;
  while (b[i]) {
    int j = i;
    while (b[j] == ' ' || b[j] == '\t') j++;
    if (b[j] == '[') {
      dentro = comeca(b, j, "[video]");
      if (dentro && *ini < 0) *ini = prox_linha(b, i);
    } else if (dentro && *tl < 0 && comeca(b, j, "telas=")) {
      *tl = i;
    }
    i = prox_linha(b, i);
  }
  return *ini >= 0 ? 0 : -1;
}

static void carrega(void) {
  static char b[CFG_MAX];
  int ini, tl;
  g_modo = 0;
  g_tem_video = 0;
  if (le_cfg(b, CFG_MAX) < 0) {
    set_status("sem D:\\SODOS.CFG: a placa nao tem driver configurado");
    return;
  }
  if (acha(b, &ini, &tl) != 0) {
    set_status("o SODOS.CFG nao tem a secao [video]");
    return;
  }
  g_tem_video = 1;
  if (tl >= 0) {
    int j = tl;
    while (b[j] == ' ' || b[j] == '\t') j++;
    g_modo = comeca(b, j + 6, "e");
  }
  set_status(g_modo ? "agora: estendido" : "agora: duplicado");
}

static void grava(void) {
  static char b[CFG_MAX], o[CFG_MAX + 64];
  const char* linha = g_modo ? "telas=estendida" : "telas=espelho";
  int n = le_cfg(b, CFG_MAX), ini, tl, k = 0, i;
  FILE* f;
  if (n < 0 || acha(b, &ini, &tl) != 0) {
    set_status("nao achei a secao [video] no D:\\SODOS.CFG");
    return;
  }
  if (tl >= 0) {
    /* troca a linha telas= */
    int fim = prox_linha(b, tl), crlf = fim >= 2 && b[fim - 2] == '\r';
    for (i = 0; i < tl; i++) o[k++] = b[i];
    for (i = 0; linha[i]; i++) o[k++] = linha[i];
    if (crlf) o[k++] = '\r';
    o[k++] = '\n';
    for (i = fim; i < n; i++) o[k++] = b[i];
  } else {
    /* poe logo depois de [video] */
    int crlf = ini >= 2 && b[ini - 2] == '\r';
    for (i = 0; i < ini; i++) o[k++] = b[i];
    if (ini > 0 && b[ini - 1] != '\n') o[k++] = '\n';
    for (i = 0; linha[i]; i++) o[k++] = linha[i];
    if (crlf) o[k++] = '\r';
    o[k++] = '\n';
    for (i = ini; i < n; i++) o[k++] = b[i];
  }
  f = fopen(CFG_PATH, "wb");
  if (!f) {
    set_status("nao gravou o D:\\SODOS.CFG");
    return;
  }
  fwrite(o, 1, (size_t)k, f);
  fclose(f);
  set_status(g_modo ? "gravado: estendido. Saia do WIN (Esc) e entre de novo"
                    : "gravado: duplicado. Saia do WIN (Esc) e entre de novo");
}

/* Desenho das duas telas, para a escolha ficar clara */
static void desenha_telas(int x, int y, int estendido) {
  int w = 120, h = 70, g = 8;
  unsigned borda = 0x303030, tela = 0x2060A0, janela = 0xE0E0E0;
  gui_fill(x, y, w, h, borda);
  gui_fill(x + 4, y + 4, w - 8, h - 8, tela);
  gui_fill(x + w + g, y, w, h, borda);
  gui_fill(x + w + g + 4, y + 4, w - 8, h - 8, tela);
  if (estendido) {
    /* uma janela atravessando para a segunda tela */
    gui_fill(x + 70, y + 20, 70, 34, janela);
    gui_fill(x + w + g + 4, y + 20, 22, 34, janela);
    gui_text(x + 14, y + h + 4, "1", -1, wg_theme()->text, wg_theme()->bg, GUI_TEXT_NOBG);
    gui_text(x + w + g + 14, y + h + 4, "2", -1, wg_theme()->text, wg_theme()->bg, GUI_TEXT_NOBG);
  } else {
    gui_fill(x + 30, y + 20, 60, 34, janela);
    gui_fill(x + w + g + 30, y + 20, 60, 34, janela);
    gui_text(x + 14, y + h + 4, "1", -1, wg_theme()->text, wg_theme()->bg, GUI_TEXT_NOBG);
    gui_text(x + w + g + 14, y + h + 4, "1", -1, wg_theme()->text, wg_theme()->bg, GUI_TEXT_NOBG);
  }
}

int main(void) {
  gui_event_t ev;
  int run = 1;
  unsigned t_open;

  if (gui_open("Telas") != 0) {
    printf("TELAS: abra pelo WIN\n");
    return 1;
  }
  carrega();
  wg_focus_set(g_modo ? &id_ext : &id_dup);
  t_open = k_ms();

  while (run) {
    int sw = gui_width(), sh = gui_height(), ch = gui_char_h();
    int bh = wg_theme()->btn_h, pad = 10, lw = 340;
    if (!g_ready && k_ms() - t_open > 400) g_ready = 1;
    wg_input(g_mx, g_my, g_down);
    gui_fill(0, 0, sw, sh, wg_theme()->bg);

    wg_group(pad, pad, lw, 96, "Dois monitores");
    if (wg_radio(&id_dup, pad + 14, pad + 26, "Duplicado (a mesma imagem)", &g_modo, 0))
      set_status("duplicado: as duas telas mostram o mesmo");
    if (wg_radio(&id_ext, pad + 14, pad + 52, "Estendido (area de trabalho maior)", &g_modo, 1))
      set_status("estendido: a segunda tela fica a direita da primeira");

    wg_group(pad + lw + 10, pad, 268, 116, "Como fica");
    desenha_telas(pad + lw + 22, pad + 24, g_modo);

    wg_label_dim(pad, pad + 126, "Vale ao entrar de novo no WIN (Esc e WIN).");
    wg_label_dim(pad, pad + 126 + ch + 4, "Precisa do driver AMDDUAL.DLL no SODOS.CFG.");

    {
      int by = sh - (ch + 26) - bh - 10;
      if (wg_button(&id_apply, pad, by, 130, bh, "Aplicar", g_ready && g_tem_video)) grava();
      if (wg_button(&id_exit, sw - pad - 110, by, 110, bh, "Sair", 1)) run = 0;
    }
    wg_status(0, sh - (ch + 26), sw, g_status, g_modo ? "estendido" : "duplicado");
    gui_present();

    if (gui_wait(&ev, 150) != GUI_EV_NONE) {
      if (ev.type == GUI_EV_CLOSE) run = 0;
      else if (ev.type == GUI_EV_DOWN) { g_mx = ev.x; g_my = ev.y; g_down = 1; }
      else if (ev.type == GUI_EV_UP) { g_mx = ev.x; g_my = ev.y; g_down = 0; }
      else if (ev.type == GUI_EV_DRAG) { g_mx = ev.x; g_my = ev.y; g_down = 1; }
      else if (ev.type == GUI_EV_KEY && ev.key == 27) run = 0;
    }
  }
  gui_close();
  return 0;
}
