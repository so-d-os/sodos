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
/* O linker do SODOS corrompe o heap quando um EXE tem ai_client.obj *e*
 * TLS.DLL: compilamos o ai_client.c dentro deste arquivo (fonte unica). */
#include "../c_compiler/agent/ai_client.c"

#define LINES_MAX 64
#define MSG_MAX 60
#define MSG_LEN 2000
#define ROLE_ME 1
#define ROLE_AI 0
#define ROLE_SYS 2
#define ROLE_OBS 3

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
    "You are AGENT, the assistant that runs natively inside SODOS (a small "
    "DOS-like OS: 64-bit kernel, FAT16 on C:/D:/U:, the WIN desktop, its own "
    "C compiler CC.EXE). You are not a text-only chat: you act on the system, "
    "and you never say you cannot do it. Answer in Brazilian Portuguese, "
    "short.\n"
    "To act, end the reply with exactly ONE fenced block and stop:\n"
    "```cmd\n<one DOS command line>\n```\n"
    "```write D:\\PATH\\FILE.EXT\n<file content>\n```\n"
    "```open D:\\PATH\\PROGRAM.EXE\n```\n"
    "open shows a window: D:\\EXPLORER.EXE, D:\\VIDEOCFG.EXE, "
    "D:\\MENUCFG.EXE, D:\\AIPANEL.EXE, D:\\WIDGETS.EXE, D:\\SODOS\\*.EXE.\n"
    "After the block you get its result and may act again; when done, or if "
    "you need the user, reply with NO block. Use DOS commands (dir, type, "
    "copy, del, mkdir), 8.3 names; compile with: cc -o PROG.EXE PROG.C. Never "
    "run edit, mc, win or other interactive programs.";

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
  static char disp[MSG_LEN * 2];
  int cpl = chars_per_line(w);
  int nl, s[LINES_MAX], l[LINES_MAX];
  ai_utf8_to_cp850(disp, (int)sizeof(disp), text, (int)strlen(text));
  nl = wrap_lines(disp, cpl, s, l);
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
  static char disp[MSG_LEN * 2];
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
  ai_utf8_to_cp850(disp, (int)sizeof(disp), m->text, (int)strlen(m->text));
  len = (int)strlen(disp);
  nlines = wrap_lines(disp, cpl, lstart, llen);
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
    fg = (m->role == ROLE_SYS || m->role == ROLE_OBS) ? wg_theme()->text_dim
                                                      : wg_theme()->text;
    bg = (m->role == ROLE_SYS || m->role == ROLE_OBS) ? wg_theme()->bg
                                                      : wg_theme()->panel;
    lab = wg_theme()->text_dim;
    bx = x;
  }
  if (m->role != ROLE_SYS && m->role != ROLE_OBS)
    gui_text(bx + 2, y, m->role == ROLE_ME ? "voce" : "SODOS", -1, lab,
             wg_theme()->bg, GUI_TEXT_NOBG);
  gui_fill(bx, y + ch + 2, bw, bh - ch - 2, bg);
  (void)len;
  for (li = 0; li < nlines; li++) {
    if (llen[li] <= 0) break;
    gui_text(bx + pad, y + ch + 2 + pad + li * (ch + 2), disp + lstart[li],
             llen[li], fg, bg, GUI_TEXT_NOBG);
  }
}

/* ---------------- ferramentas (as mesmas do AGENT.EXE) ---------------- */
struct tool {
  int kind;   /* 0 nenhuma, 'c' comando, 'w' gravar, 'o' abrir programa */
  char arg[128];
  char body[1600];
};

/* valor de <|DSML| parameter name="NOME" ...>VALOR</|DSML| parameter> */
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

/* Procura uma chamada de ferramenta na resposta (DSML ou ```bloco). */
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
        if (!strncmp(nm, "write", 5) || !strncmp(nm, "create", 6) || !strncmp(nm, "save", 4)) {
          if (dsml_param(inv, end, "path", t->arg, sizeof(t->arg)) ||
              dsml_param(inv, end, "file", t->arg, sizeof(t->arg)) ||
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
                   !strncmp(nm, "exec", 4) || !strncmp(nm, "bash", 4)) {
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
  /* ```cmd / ```write CAMINHO */
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
        if (next) *next = strchr(p + 3, '\n') ? strstr(p + 3, "```") + 3 : p + strlen(p);
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

/* Executa; enche obs (CP850) com o resultado que volta ao modelo. */
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

/* Tira os blocos de chamada (DSML e ```cmd/write/open) do texto do chat. */
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

static void on_token(void* ud, const char* s, int n) {
  (void)ud;
  if (n <= 0) return;
  if (g_stream_len + n < MSG_LEN - 1) {
    memcpy(g_stream + g_stream_len, s, (size_t)n);
    g_stream_len += n;
    g_stream[g_stream_len] = 0;
  }
}

/* Chamada da API com o historico atual. 0 = ok, -1 = erro (texto em g_stream
 * ou em ai_last_error). */
static int call_api(void) {
  ai_msg_t* list;
  int i, k = 0, rc;

  g_stream_len = 0;
  g_stream[0] = 0;
  list = (ai_msg_t*)malloc((size_t)(g_n + 1) * sizeof(ai_msg_t));
  if (!list) {
    set_status("sem memoria");
    return -1;
  }
  list[k].role = "system";
  list[k].text = SYS_PROMPT;
  k++;
  for (i = 0; i < g_n; i++) {
    if (g_msgs[i].role == ROLE_SYS) continue;
    list[k].role = (g_msgs[i].role == ROLE_ME || g_msgs[i].role == ROLE_OBS)
                       ? "user"
                       : "assistant";
    list[k].text = g_msgs[i].text;
    k++;
  }
  rc = ai_chat(&g_cfg, list, k, on_token, 0);
  free(list);
  return rc;
}

#define STEPS_MAX 8

static void send_now(void) {
  char q[MSG_LEN];
  char obs[1200];
  char label[160];
  char shown[MSG_LEN * 2];
  struct tool tl;
  const char* cur;
  int step;

  ai_cp850_to_utf8(q, (int)sizeof(q), g_edit.text, (int)strlen(g_edit.text));
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

  for (step = 0; step < STEPS_MAX; step++) {
    g_waiting = 1;
    set_status(step ? "executando e perguntando de novo..." : "esperando a API...");
    gui_present();
    if (call_api() != 0) {
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
      return;
    }
    strip_tool_markup(g_stream, shown, (int)sizeof(shown));
    if (shown[0]) add_msg(ROLE_AI, shown);

    cur = g_stream;
    {
      int did = 0;
      while (parse_tool(cur, &cur, &tl)) {
        obs[0] = 0;
        label[0] = 0;
        run_tool(&tl, obs, (int)sizeof(obs), label, (int)sizeof(label));
        if (label[0]) add_msg(ROLE_SYS, label);
        add_msg(ROLE_OBS, obs);
        did = 1;
        if (!cur || !*cur) break;
      }
      if (!did) break;
    }
  }
  if (step == STEPS_MAX) add_msg(ROLE_SYS, "[limite de acoes neste pedido]");
  g_waiting = 0;
  g_stream_len = 0;
  g_stream[0] = 0;
  set_status("ok");
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
      int h;
      if (g_msgs[i].role == ROLE_OBS) continue;
      h = msg_height(g_msgs[i].text, sw - 2 * pad - 4);
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
