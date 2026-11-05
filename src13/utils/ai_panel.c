/* ai_panel.c - painel de IA do SODOS (AIPANEL.EXE).
 *
 * Abre docado na lateral direita (k_gui_dock) e desenha um chat no estilo do
 * mockup: historico em bolhas, sugestoes e campo de mensagem.
 *
 * Ainda NAO existe modelo de linguagem ligado: as respostas sao locais e
 * dizem isso. O objetivo desta versao e o layout e o encaixe na tela.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui.h>
#include <widgets.h>
#include <so.h>

#define MSG_MAX 40
#define MSG_LEN 160

struct msg {
  int mine;   /* 1 = usuario */
  char text[MSG_LEN];
};

static struct msg g_msgs[MSG_MAX];
static int g_n;
static gui_edit_t g_edit;
static char g_status[96];
static int g_mx, g_my, g_down;
static int g_ready;
static const char id_send, id_edit, id_chip0, id_chip1, id_chip2;

static void set_status(const char* s) {
  int i = 0;
  while (s[i] && i < (int)sizeof(g_status) - 1) {
    g_status[i] = s[i];
    i++;
  }
  g_status[i] = 0;
}

static void push(int mine, const char* text) {
  int i = 0;
  if (g_n >= MSG_MAX) {
    /* descarta a mais antiga */
    for (i = 1; i < MSG_MAX; i++) g_msgs[i - 1] = g_msgs[i];
    g_n = MSG_MAX - 1;
  }
  g_msgs[g_n].mine = mine;
  i = 0;
  while (text[i] && i < MSG_LEN - 1) {
    g_msgs[g_n].text[i] = text[i];
    i++;
  }
  g_msgs[g_n].text[i] = 0;
  g_n++;
}

/* Resposta local (sem modelo): repete o pedido e explica o estado. */
static void reply_to(const char* q) {
  char buf[MSG_LEN];
  int n = 0;
  const char* pre = "Ainda nao tenho modelo de linguagem ligado. Voce pediu: ";
  int i = 0;
  while (pre[i] && n < MSG_LEN - 1) buf[n++] = pre[i++];
  i = 0;
  while (q[i] && n < MSG_LEN - 1) buf[n++] = q[i++];
  buf[n] = 0;
  push(0, buf);
}

static void send_message(void) {
  char q[MSG_LEN];
  int i = 0;
  while (g_edit.text[i] && i < MSG_LEN - 1) {
    q[i] = g_edit.text[i];
    i++;
  }
  q[i] = 0;
  if (!q[0]) {
    set_status("escreva uma mensagem");
    return;
  }
  push(1, q);
  reply_to(q);
  gui_edit_set(&g_edit, "");
  set_status("resposta local (sem modelo)");
}

/* bolha de mensagem; devolve a altura usada */
static int bubble(int x, int w, int y, struct msg* m) {
  int pad = 8;
  int cw = gui_char_w();
  int ch = gui_char_h();
  int len = (int)strlen(m->text);
  int maxc = (w * 3 / 4) / (cw > 0 ? cw : 8);
  int bw, bh, li, nlines;
  unsigned fg, bg;
  int bx;

  if (maxc < 8) maxc = 8;
  nlines = (len + maxc - 1) / maxc;
  if (nlines < 1) nlines = 1;
  bw = len * cw + pad * 2;
  if (bw > w * 3 / 4) bw = w * 3 / 4;
  if (bw < 120) bw = 120;
  bh = nlines * (ch + 2) + pad * 2;
  fg = m->mine ? wg_theme()->sel_text : wg_theme()->text;
  bg = m->mine ? wg_theme()->accent : wg_theme()->panel;
  bx = m->mine ? x + w - bw : x;
  gui_fill(bx, y, bw, bh, bg);
  for (li = 0; li < nlines; li++) {
    int from = li * maxc;
    int n = len - from;
    if (n > maxc) n = maxc;
    if (n <= 0) break;
    gui_text(bx + pad, y + pad + li * (ch + 2), m->text + from, n, fg, bg,
             GUI_TEXT_NOBG);
  }
  return bh;
}

int main(void) {
  gui_event_t ev;
  int run = 1, sw, sh;
  int pad = 8;

  if (gui_dock_open("SODOS IA", 380) != 0) {
    printf("AIPANEL: abra pelo WIN\n");
    return 1;
  }
  wg_theme_use(1);   /* tema escuro, como o resto do desktop */
  push(0, "Ola! Sou o assistente do SODOS.");
  push(0, "Ainda nao tenho modelo de linguagem ligado nesta versao.");
  gui_edit_set(&g_edit, "");
  set_status("pronto");
  wg_focus_set(&id_edit);

  while (run) {
    int ch = gui_char_h();
    int y;
    int i;
    int input_h = 32;
    int status_h = 22;

    sw = gui_width();
    sh = gui_height();
    if (!g_ready && k_ms() > 500) g_ready = 1;
    wg_input(g_mx, g_my, g_down);

    gui_fill(0, 0, sw, sh, wg_theme()->bg);
    /* cabecalho */
    gui_fill(pad, pad, sw - pad * 2, 46, wg_theme()->panel);
    wg_dot(pad + 18, pad + 23, 11, wg_theme()->accent);
    gui_text(pad + 14, pad + 15, "S", 1, wg_theme()->sel_text, wg_theme()->accent,
             GUI_TEXT_NOBG);
    gui_text(pad + 38, pad + 8, "Assistente SODOS", -1, wg_theme()->text,
             wg_theme()->panel, GUI_TEXT_NOBG);
    gui_text(pad + 38, pad + 26, "local - sem modelo ligado", -1,
             wg_theme()->text_dim, wg_theme()->panel, GUI_TEXT_NOBG);

    /* historico */
    y = pad + 46 + 8;
    for (i = 0; i < g_n; i++) {
      int h;
      if (y > sh - status_h - input_h - 8 - 28 - 12) break;
      h = bubble(pad + 2, sw - pad * 2 - 4, y, &g_msgs[i]);
      y += h + 6;
    }

    /* sugestoes */
    {
      int cy = sh - status_h - 8 - input_h - 8 - 28;
      int bw = (sw - pad * 2 - 12) / 3;
      int bi = 0;
      static const char* chips[3] = {"Buscar", "Abrir app", "Explicar"};
      const char* ids[3];
      (void)ids;
      for (bi = 0; bi < 3; bi++) {
        const void* id = bi == 0 ? (const void*)&id_chip0
                       : bi == 1 ? (const void*)&id_chip1
                                 : (const void*)&id_chip2;
        if (wg_button(id, pad + bi * (bw + 6), cy, bw, 28, chips[bi], g_ready)) {
          if (bi == 0) push(1, "buscar arquivos");
          else if (bi == 1) push(1, "abrir aplicativo");
          else push(1, "explicar codigo");
          reply_to(bi == 0 ? "buscar arquivos"
                   : bi == 1 ? "abrir aplicativo"
                             : "explicar codigo");
        }
      }
    }

    /* entrada */
    {
      int by = sh - status_h - 8 - input_h;
      int bw = sw - pad * 2 - 92;
      wg_edit_box(&id_edit, &g_edit, pad, by, bw, 32);
      if (wg_button(&id_send, pad + bw + 8, by, 76, 32, "Enviar", g_ready))
        send_message();
    }
    wg_status(0, sh - status_h, sw, g_status, "IA");

    gui_present();

    if (gui_wait(&ev, 120) != GUI_EV_NONE) {
      if (ev.type == GUI_EV_CLOSE) run = 0;
      else if (ev.type == GUI_EV_DOWN) { g_mx = ev.x; g_my = ev.y; g_down = 1; }
      else if (ev.type == GUI_EV_UP) { g_mx = ev.x; g_my = ev.y; g_down = 0; }
      else if (ev.type == GUI_EV_DRAG) { g_mx = ev.x; g_my = ev.y; g_down = 1; }
      else if (ev.type == GUI_EV_KEY) {
        if (ev.key == 27) run = 0;
        else if (ev.key == '\r' || ev.key == '\n') send_message();
        else gui_edit_key(&g_edit, ev.key);
      }
    }
  }
  gui_close();
  return 0;
}
