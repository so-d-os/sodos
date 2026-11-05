/* ai_panel.c - painel de IA do SODOS (AIPANEL.EXE).
 *
 * Chat convencional docado na lateral direita:
 *   - usa o mesmo AGENT.CFG do AGENT.EXE (key/url/model);
 *   - bolhas de conversa (voce a direita, SODOS a esquerda);
 *   - Enter envia, Esc fecha;
 *   - a resposta aparece letra por letra (stream SSE da API);
 *   - "novo" limpa a conversa.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui.h>
#include <widgets.h>
#include <so.h>
#include <ai_client.h>

#define LINES_MAX 64
#define MSG_MAX 60
#define MSG_LEN 1400
#define ROLE_ME 1
#define ROLE_AI 0
#define ROLE_SYS 2

struct msg {
  int role;
  char text[MSG_LEN];
};

static struct msg g_msgs[MSG_MAX];
static int g_n;
static ai_cfg_t g_cfg;
static int g_waiting;
static char g_stream[MSG_LEN];
static int g_stream_len;
static gui_edit_t g_edit;
static char g_status[120];
static int g_mx, g_my, g_down, g_ready;
static const char id_send, id_edit, id_new;

static const char* SYS_PROMPT =
    "Voce e o assistente do SODOS, um sistema operacional x86_64 escrito do "
    "zero. Responda em portugues do Brasil, de forma curta e direta.";

static void set_status(const char* s) {
  int i = 0;
  while (s[i] && i < (int)sizeof(g_status) - 1) {
    g_status[i] = s[i];
    i++;
  }
  g_status[i] = 0;
}

static void add_msg(int role, const char* text) {
  int i = 0;
  if (g_n >= MSG_MAX) {
    for (i = 1; i < MSG_MAX; i++) g_msgs[i - 1] = g_msgs[i];
    g_n = MSG_MAX - 1;
  }
  g_msgs[g_n].role = role;
  i = 0;
  while (text[i] && i < MSG_LEN - 1) {
    g_msgs[g_n].text[i] = text[i];
    i++;
  }
  g_msgs[g_n].text[i] = 0;
  g_n++;
}

static int chars_per_line(int w) {
  int cw = gui_char_w();
  if (cw < 1) cw = 8;
  return (w - 20) / cw;
}


static int wrap_lines(const char* text, int cpl, int* start, int* len);

static int msg_height(const char* text, int w) {
  int cpl = chars_per_line(w);
  int nl, i, s[LINES_MAX], l[LINES_MAX];
  (void)i;
  nl = wrap_lines(text, cpl, s, l);
  return nl * (gui_char_h() + 2) + 16 + gui_char_h();
}

/* Corta o texto em linhas de no maximo 'cpl' caracteres, sem partir palavras.
 * fills[i] recebe inicio e tamanho de cada linha; devolve quantas linhas. */
static int wrap_lines(const char* text, int cpl, int* start, int* len) {
  int n = 0, i = 0, total = (int)strlen(text);
  int cur_start = 0, cur_len = 0;
  if (cpl < 8) cpl = 8;
  while (i < total && n < LINES_MAX) {
    int word_start = i;
    int word_len;
    while (i < total && text[i] == ' ') i++;
    word_start = i;
    while (i < total && text[i] != ' ') i++;
    word_len = i - word_start;
    if (cur_len == 0) {
      cur_start = word_start;
      cur_len = word_len;
    } else if (cur_len + 1 + word_len <= cpl) {
      cur_len += 1 + word_len;
    } else {
      start[n] = cur_start;
      len[n] = cur_len;
      n++;
      cur_start = word_start;
      cur_len = word_len;
    }
  }
  if (cur_len > 0 && n < LINES_MAX) {
    start[n] = cur_start;
    len[n] = cur_len;
    n++;
  }
  if (n == 0) {
    start[0] = 0;
    len[0] = 0;
    n = 1;
  }
  return n;
}

static void draw_bubble(int x, int w, int y, struct msg* m) {
  int pad = 8;
  int cw = gui_char_w();
  int ch = gui_char_h();
  int cpl, len, nlines, bw, bh, li, bx;
  int lstart[LINES_MAX], llen[LINES_MAX];
  int maxlen = 0;
  unsigned fg, bg, lab;

  if (cw < 1) cw = 8;
  if (ch < 1) ch = 12;
  cpl = chars_per_line(w);
  if (cpl < 8) cpl = 8;
  len = (int)strlen(m->text);
  nlines = wrap_lines(m->text, cpl, lstart, llen);
  for (li = 0; li < nlines; li++)
    if (llen[li] > maxlen) maxlen = llen[li];
  bw = (maxlen > 0 ? maxlen : 1) * cw + pad * 2;
  if (bw > w) bw = w;
  if (bw < 60) bw = 60;
  bh = nlines * (ch + 2) + pad * 2 + ch;

  if (m->role == ROLE_ME) {
    fg = wg_theme()->sel_text;
    bg = wg_theme()->accent;
    lab = wg_theme()->text_dim;
    bx = x + w - bw;
  } else {
    fg = m->role == ROLE_SYS ? wg_theme()->text_dim : wg_theme()->text;
    bg = m->role == ROLE_SYS ? wg_theme()->bg : wg_theme()->panel;
    lab = wg_theme()->text_dim;
    bx = x;
  }
  if (m->role != ROLE_SYS)
    gui_text(bx + 2, y, m->role == ROLE_ME ? "voce" : "SODOS", -1, lab,
             wg_theme()->bg, GUI_TEXT_NOBG);
  gui_fill(bx, y + ch + 2, bw, bh - ch - 2, bg);
  (void)len;
  for (li = 0; li < nlines; li++) {
    if (llen[li] <= 0) break;
    gui_text(bx + pad, y + ch + 2 + pad + li * (ch + 2), m->text + lstart[li],
             llen[li], fg, bg, GUI_TEXT_NOBG);
  }
}

static void on_token(void* ud, const char* s, int n) {
  char buf[MSG_LEN];
  int add;
  (void)ud;
  ai_utf8_to_cp850(buf, (int)sizeof(buf), s, n);
  add = (int)strlen(buf);
  if (g_stream_len + add < MSG_LEN - 1) {
    memcpy(g_stream + g_stream_len, buf, (size_t)add);
    g_stream_len += add;
    g_stream[g_stream_len] = 0;
  }
}

static void send_now(void) {
  ai_msg_t* list;
  int i, k = 0, rc;
  char q[MSG_LEN];

  i = 0;
  while (g_edit.text[i] && i < MSG_LEN - 1) {
    q[i] = g_edit.text[i];
    i++;
  }
  q[i] = 0;
  if (!q[0]) {
    set_status("escreva uma mensagem");
    return;
  }
  if (!g_cfg.key[0]) {
    set_status("falta key no AGENT.CFG");
    return;
  }
  add_msg(ROLE_ME, q);
  gui_edit_set(&g_edit, "");
  g_stream_len = 0;
  g_stream[0] = 0;
  g_waiting = 1;
  set_status("esperando a API...");
  gui_present();

  list = (ai_msg_t*)malloc((size_t)(g_n + 1) * sizeof(ai_msg_t));
  if (!list) {
    g_waiting = 0;
    set_status("sem memoria");
    return;
  }
  list[k].role = "system";
  list[k].text = SYS_PROMPT;
  k++;
  for (i = 0; i < g_n; i++) {
    if (g_msgs[i].role == ROLE_SYS) continue;
    list[k].role = g_msgs[i].role == ROLE_ME ? "user" : "assistant";
    list[k].text = g_msgs[i].text;
    k++;
  }
  rc = ai_chat(&g_cfg, list, k, on_token, 0);
  free(list);
  g_waiting = 0;
  if (rc == 0 && g_stream_len > 0) {
    add_msg(ROLE_AI, g_stream);
    set_status("ok");
  } else {
    char st[200];
    if (g_stream_len > 0) add_msg(ROLE_AI, g_stream);
    snprintf(st, sizeof(st), "[%s]", ai_last_error()[0] ? ai_last_error() : "sem resposta");
    add_msg(ROLE_SYS, st);
    set_status(ai_last_error());
  }
  g_stream_len = 0;
  g_stream[0] = 0;
}

int main(void) {
  gui_event_t ev;
  int run = 1;

  if (gui_dock_open("SODOS IA", 380) != 0) {
    printf("AIPANEL: abra pelo WIN\n");
    return 1;
  }
  wg_theme_use(1);
  ai_cfg_load(&g_cfg);
  if (g_cfg.key[0]) set_status(g_cfg.model);
  else set_status("falta key no AGENT.CFG");
  add_msg(ROLE_SYS, "Converse com o SODOS. Enter envia, Esc fecha.");
  gui_edit_set(&g_edit, "");
  wg_focus_set(&id_edit);

  while (run) {
    int pad = 8;
    int head_h = 52;
    int input_h = 34;
    int status_h = 22;
    int sw = gui_width();
    int sh = gui_height();
    int ch = gui_char_h();
    int ytop, ybot, i, y;

    if (!g_ready && k_ms() > 500) g_ready = 1;
    wg_input(g_mx, g_my, g_down);
    gui_fill(0, 0, sw, sh, wg_theme()->bg);

    gui_fill(0, 0, sw, head_h, wg_theme()->panel);
    wg_dot(18, 26, 13, wg_theme()->accent);
    gui_text(14, 18, "S", 1, wg_theme()->sel_text, wg_theme()->accent, GUI_TEXT_NOBG);
    gui_text(38, 8, "Assistente SODOS", -1, wg_theme()->text, wg_theme()->panel,
             GUI_TEXT_NOBG);
    gui_text(38, 28, g_cfg.model, 20, wg_theme()->text_dim, wg_theme()->panel,
             GUI_TEXT_NOBG);
    if (wg_button(&id_new, sw - 78, 10, 68, 32, "novo", g_ready)) {
      g_n = 0;
      add_msg(ROLE_SYS, "Conversa nova. Enter envia, Esc fecha.");
      set_status("conversa limpa");
    }

    ytop = head_h + 6;
    ybot = sh - status_h - input_h - 12;
    y = ybot;
    for (i = g_n - 1; i >= 0; i--) {
      int h = msg_height(g_msgs[i].text, sw - 2 * pad - 4);
      if (y - h < ytop) break;
      y -= h;
      draw_bubble(pad, sw - 2 * pad, y, &g_msgs[i]);
      y -= 6;
    }
    if (g_waiting) {
      struct msg tmp;
      int h;
      tmp.role = ROLE_AI;
      snprintf(tmp.text, sizeof(tmp.text), "%s", g_stream_len ? g_stream : "...");
      h = msg_height(tmp.text, sw - 2 * pad - 4);
      if (y - h >= ytop) {
        y -= h;
        draw_bubble(pad, sw - 2 * pad, y, &tmp);
      }
    }

    {
      int by = sh - status_h - input_h;
      int bw = sw - 2 * pad - 90;
      wg_edit_box(&id_edit, &g_edit, pad, by, bw, input_h);
      if (wg_button(&id_send, pad + bw + 6, by, 84, input_h, "Enviar",
                    g_ready && !g_waiting))
        send_now();
    }
    wg_status(0, sh - status_h, sw, g_status, g_cfg.key[0] ? "online" : "sem key");

    gui_present();

    if (gui_wait(&ev, 120) != GUI_EV_NONE) {
      if (ev.type == GUI_EV_CLOSE) run = 0;
      else if (ev.type == GUI_EV_DOWN) { g_mx = ev.x; g_my = ev.y; g_down = 1; }
      else if (ev.type == GUI_EV_UP) { g_mx = ev.x; g_my = ev.y; g_down = 0; }
      else if (ev.type == GUI_EV_DRAG) { g_mx = ev.x; g_my = ev.y; g_down = 1; }
      else if (ev.type == GUI_EV_KEY) {
        if (ev.key == 27) run = 0;
        else if (ev.key == '\r' || ev.key == '\n') {
          if (!g_waiting) send_now();
        } else {
          gui_edit_key(&g_edit, ev.key);
        }
      }
    }
  }
  gui_close();
  return 0;
}
