/* main.c - BROWSER.EXE: navegador minimo do WIN.
 *   cc -o browser.exe browser.c url.c http.c html.c js.c search.c cache.cpp
 *      tls.dll gui.dll img.dll
 * No WIN abre uma janela com barra de endereco, Voltar, Recarregar, rolagem
 * e links clicaveis. Fora do WIN (shell do DOS) imprime a pagina como texto.
 * Palavras na barra viram busca (search.c): DuckDuckGo, Google, Bing ou imagens. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <so.h>
#include <gui.h>
#include <img.h>
#include "browser.h"

#define HIST_MAX 32
#define SCROLL_W 14
#define WHEEL_LINES 3 /* linhas por passo da roda do rato */
#define JS_MAX (64 * 1024) /* bytes por <script src> baixado */

static const char start_page[] =
  "<html><head><title>Navegador do SO</title>"
  "<style>h1{color:#003399}.nota{color:#804000;font-weight:bold}</style>"
  "</head><body><h1>Navegador do SO</h1>"
  "<form action=\"about:busca\"><b>Buscar:</b> <input name=\"q\" size=\"36\"> "
  "<input type=\"submit\" name=\"em\" value=\"DuckDuckGo\"> "
  "<input type=\"submit\" name=\"em\" value=\"Google\"> "
  "<input type=\"submit\" name=\"em\" value=\"Bing\"> "
  "<input type=\"submit\" name=\"em\" value=\"Imagens\"></form>"
  "<p>Na barra de endereco tambem: <code>gato preto</code> busca no DuckDuckGo, "
  "<code>g gato preto</code> no Google, <code>b gato preto</code> no Bing e "
  "<code>i gato preto</code> procura imagens. O Google so responde a navegadores "
  "com JavaScript, e o DuckDuckGo as vezes pede um desafio anti-robo; quando um "
  "recusa, a busca passa para o seguinte (Google, DuckDuckGo, Bing) com um aviso.</p>"
  "<p>Feito em C, compilado pelo <b>cc</b> do proprio sistema. HTTP/1.1 e HTTPS "
  "(TLS 1.3 da <code>TLS.DLL</code>); a janela vem da <code>GUI.DLL</code>.</p>"
  "<p><b>Atencao:</b> o certificado HTTPS e conferido (nome, cadeia e assinatura) "
  "quando a raiz e conhecida. Se nao for, o rodape avisa.</p>"
  "<p>Figuras <b>PNG</b>, <b>JPEG</b> (inclusive progressivo), <b>GIF</b> e <b>ICO</b> "
  "aparecem na pagina (ate 8, pela <code>IMG.DLL</code>). Fotos grandes sao reduzidas.</p>"
  "<p class=\"nota\">CSS ativo: este paragrafo fica marrom e em negrito.</p>"
  "<p id=\"saida\">JS ainda nao rodou.</p>"
  "<script>var el=document.getElementById(\"saida\");"
  "el.style.color=\"#006600\";el.innerHTML=\"JavaScript ativo.\";"
  "alert(\"CSS e JavaScript ligados\");</script>"
  "<h2>Experimente</h2><ul>"
  "<li><a href=\"https://example.com/\">https://example.com/</a></li>"
  "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">info.cern.ch</a> - o primeiro site (http)</li>"
  "<li><a href=\"https://text.npr.org/\">text.npr.org</a> - noticias em texto</li>"
  "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a></li>"
  "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
  "<li><a href=\"https://en.wikipedia.org/wiki/Special:Random\">Wikipedia (artigo aleatorio)</a></li>"
  "</ul><h2>Teclas</h2><ul>"
  "<li>Clique na barra de endereco, ou num campo da pagina, digite e tecle Enter</li>"
  "<li>Roda do mouse, setas, PgUp/PgDn, Home/End e Espaco rolam a pagina</li>"
  "<li>Backspace volta; Tab alterna entre endereco e pagina; Esc fecha</li>"
  "</ul></body></html>";

static doc_t doc;
static img_t pics[DOC_IMGS];
static char cur_url[URL_MAX];
static char status[200];
static int is_tls, tls_warn;
static int scroll;
static int gui_mode;
static int cols = 78, rows = 24;
static char hist[HIST_MAX][URL_MAX];
static int hist_n, hist_pos = -1;
static gui_edit_t addr;
static int focus_addr;
static int focus_field = -1;
static int W, H, CW, CH, bar_h, status_h, content_y, content_h, btn_back_x, btn_back_w;
static int btn_rel_x, btn_rel_w, addr_x, addr_w, btn_go_x, btn_go_w;
static int load_pct = -1;
static int async_phase, async_i, async_js;
static char page_status[200];
static gui_event_t pending_ev;
static int pending_ev_have;
static char search_note[400]; /* aviso no topo da proxima pagina de busca */

static void draw(void);
static void clamp_scroll(void);
static void navigate(const char* input, int push, int fresh);

static void set_status(const char* s) {
  strncpy(status, s, sizeof(status) - 1);
  status[sizeof(status) - 1] = 0;
}

static void take_pct(const char* s) {
  int n = 0;
  if (!s || s[0] < '0' || s[0] > '9') return;
  while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
  if (*s != '%') return;
  if (n > 100) n = 100;
  load_pct = n;
}

/* 1 se o usuario teclou, clicou ou fechou. O soltar do clique que abriu
 * a pagina nao conta: senao todo carregamento seria cancelado. */
static int progress(const char* msg) {
  gui_event_t ev;
  take_pct(msg);
  set_status(msg);
  if (!gui_mode) {
    fprintf(stderr, "[%s]\n", msg);
    return 0;
  }
  if (pending_ev_have) return 1;
  draw();
  gui_present();
  gui_wait(&ev, 0);
  if (ev.type == GUI_EV_WHEEL) {
    /* a pagina de antes continua rolando enquanto a proxima carrega */
    scroll += ev.key * WHEEL_LINES;
    clamp_scroll();
    return 0;
  }
  if (ev.type == GUI_EV_KEY || ev.type == GUI_EV_DOWN || ev.type == GUI_EV_CLOSE) {
    pending_ev = ev;
    pending_ev_have = 1;
    return 1;
  }
  return 0;
}

static int ci_has(const char* s, const char* sub) {
  int n = (int)strlen(sub);
  for (; *s; s++) {
    int k;
    for (k = 0; k < n; k++)
      if (tolower((unsigned char)s[k]) != sub[k]) break;
    if (k == n) return 1;
  }
  return 0;
}

static void pics_free(void) {
  int i;
  for (i = 0; i < DOC_IMGS; i++) img_free(&pics[i]);
}

static int read_url(const char* url, char** data, int* len) {
  url_t u;
  *data = 0;
  *len = 0;
  if (url_parse(url, &u) != 0) return -1;
  if (strcmp(u.scheme, "http") == 0 || strcmp(u.scheme, "https") == 0) {
    http_resp_t r;
    if (http_get(url, &r, progress, 0) != 0) {
      http_free(&r);
      return -1;
    }
    *data = r.body;
    *len = r.body_len;
    r.body = 0;
    http_free(&r);
    return *data ? 0 : -1;
  }
  if (strcmp(u.scheme, "file") == 0) {
    FILE* f = fopen(u.path, "rb");
    long n;
    char* buf;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 4 * 1024 * 1024) {
      fclose(f);
      return -1;
    }
    buf = (char*)malloc((size_t)n + 1);
    if (!buf) {
      fclose(f);
      return -1;
    }
    n = (long)fread(buf, 1, (size_t)n, f);
    fclose(f);
    *data = buf;
    *len = (int)n;
    return 0;
  }
  return -1;
}

static int b64_val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static int b64_decode(const char* s, int n, unsigned char* out, int cap) {
  int i, o = 0, val = 0, bits = 0;
  for (i = 0; i < n; i++) {
    int d;
    if (s[i] == '=' || isspace((unsigned char)s[i])) continue;
    d = b64_val((unsigned char)s[i]);
    if (d < 0) break;
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return -1;
      out[o++] = (unsigned char)((val >> bits) & 0xFF);
    }
  }
  return o;
}

static int decode_data_url(const char* url, img_t* im) {
  const char* comma = strchr(url, ',');
  unsigned char* buf;
  int n, cap, b64;
  if (!comma) return -1;
  b64 = ci_has(url, "base64");
  comma++;
  cap = (int)strlen(comma);
  buf = (unsigned char*)malloc((size_t)cap + 1);
  if (!buf) return -1;
  if (b64) n = b64_decode(comma, cap, buf, cap);
  else {
    memcpy(buf, comma, (size_t)cap);
    n = cap;
  }
  if (n < 8 || img_decode(buf, n, im) != 0) {
    free(buf);
    return -1;
  }
  free(buf);
  return 0;
}

static void load_one_image(int i) {
  char* data = 0;
  int n = 0;
  doc.imgs[i].state = -1;
  if (strncmp(doc.imgs[i].url, "data:", 5) == 0) {
    if (decode_data_url(doc.imgs[i].url, &pics[i]) == 0) doc.imgs[i].state = 1;
  } else if (read_url(doc.imgs[i].url, &data, &n) == 0 &&
             img_decode(data, n, &pics[i]) == 0)
    doc.imgs[i].state = 1;
  free(data);
}

static void load_images(void) {
  int i;
  for (i = 0; i < doc.nimgs && i < DOC_IMGS; i++) load_one_image(i);
}

static void load_one_script(int i) {
  char* data = 0;
  int n = 0;
  if (doc.scripts[i].src || !doc.scripts[i].url[0]) return;
  if (read_url(doc.scripts[i].url, &data, &n) == 0 && data) {
    if (n > JS_MAX) n = JS_MAX;
    data[n] = 0;
    doc.scripts[i].src = data;
    doc.scripts[i].len = n;
  } else free(data);
}

static void load_scripts(void) {
  int i;
  for (i = 0; i < doc.nscripts; i++) load_one_script(i);
}

/* Um script ou uma imagem por volta do laco. A pagina aparece antes do resto. */
static int async_step(void) {
  int total, done;
  if (pending_ev_have) {
    async_phase = 0;
    load_pct = -1;
    http_idle_close();
    return 0;
  }
  if (async_phase == 1) {
    while (async_i < doc.nscripts &&
           (doc.scripts[async_i].src || !doc.scripts[async_i].url[0]))
      async_i++;
    if (async_i < doc.nscripts) {
      load_one_script(async_i);
      async_i++;
    } else {
      if (!async_js) {
        js_run(&doc);
        async_js = 1;
        if (doc.js_alert[0]) {
          char b[200];
          snprintf(b, sizeof b, "%s | %s", doc.js_alert,
                   page_status[0] ? page_status : status);
          strncpy(page_status, b, sizeof(page_status) - 1);
          page_status[sizeof(page_status) - 1] = 0;
          set_status(page_status);
        }
      }
      async_phase = 2;
      async_i = 0;
    }
  } else if (async_phase == 2) {
    if (async_i < doc.nimgs && async_i < DOC_IMGS) {
      load_one_image(async_i);
      async_i++;
    } else async_phase = 0;
  }
  if (!async_phase) {
    load_pct = -1;
    if (page_status[0]) set_status(page_status);
    http_idle_close();
    return 0;
  }
  total = doc.nscripts + doc.nimgs;
  if (total < 1) total = 1;
  done = (async_phase == 2) ? doc.nscripts + async_i : async_i;
  load_pct = 90 + done * 10 / total;
  if (load_pct > 99) load_pct = 99;
  return 1;
}

/* ---------- CSS externo: <link rel=stylesheet> e @import ---------- */
#define CSS_SHEETS 8                 /* arquivos por pagina */
#define CSS_SHEET_MAX (96 * 1024)    /* bytes por arquivo */
#define CSS_TOTAL (256 * 1024)       /* bytes somados */

typedef struct {
  char* b;
  int n, cap, sheets;
  char seen[CSS_SHEETS][URL_MAX];
} css_acc_t;

/* valor do atributo name= numa tag (texto depois do '<'); 0 se nao tem */
static int tag_attr(const char* tag, const char* name, char* out, int max) {
  int nl = (int)strlen(name), i;
  for (i = 1; tag[i]; i++) {
    const char* p;
    char q;
    int k = 0;
    if (!isspace((unsigned char)tag[i - 1]) || strncasecmp(tag + i, name, (size_t)nl) != 0) continue;
    p = tag + i + nl;
    while (*p == ' ') p++;
    if (*p != '=') continue;
    p++;
    while (*p == ' ') p++;
    q = (*p == '"' || *p == '\'') ? *p++ : 0;
    while (*p && k < max - 1 && (q ? *p != q : !isspace((unsigned char)*p) && *p != '>'))
      out[k++] = *p++;
    out[k] = 0;
    return 1;
  }
  out[0] = 0;
  return 0;
}

static void css_put(css_acc_t* a, const char* s, int n) {
  if (n > CSS_TOTAL - a->n) n = CSS_TOTAL - a->n;
  if (n <= 0) return;
  if (a->n + n + 2 > a->cap) {
    int nc = a->cap ? a->cap : 65536;
    char* nb;
    while (nc < a->n + n + 2) nc *= 2;
    nb = (char*)realloc(a->b, (size_t)nc);
    if (!nb) return;
    a->b = nb;
    a->cap = nc;
  }
  memcpy(a->b + a->n, s, (size_t)n);
  a->n += n;
  a->b[a->n++] = '\n';
  a->b[a->n] = 0;
}

static void css_fetch(css_acc_t* a, const char* base, const char* href, int depth);

/* @import "x.css"; @import url(x.css) screen; -> baixa antes do resto da folha */
static void css_imports(css_acc_t* a, const char* css, int n, const char* base, int depth) {
  int i;
  for (i = 0; i + 7 < n; i++) {
    char href[URL_MAX];
    const char* p;
    int k = 0;
    if (css[i] != '@' || strncasecmp(css + i, "@import", 7) != 0) continue;
    p = css + i + 7;
    while (p < css + n && isspace((unsigned char)*p)) p++;
    if (p + 4 <= css + n && strncasecmp(p, "url(", 4) == 0) p += 4;
    while (p < css + n && (*p == ' ' || *p == '"' || *p == '\'')) p++;
    while (p < css + n && k < URL_MAX - 1 && *p != '"' && *p != '\'' && *p != ')' && *p != ';' &&
           !isspace((unsigned char)*p))
      href[k++] = *p++;
    href[k] = 0;
    if (href[0]) css_fetch(a, base, href, depth + 1);
  }
}

static void css_fetch(css_acc_t* a, const char* base, const char* href, int depth) {
  char absu[URL_MAX];
  char* data = 0;
  int dn = 0, i;
  if (depth > 1 || a->sheets >= CSS_SHEETS || a->n >= CSS_TOTAL) return;
  if (url_resolve(base ? base : "", href, absu, URL_MAX) != 0) return;
  for (i = 0; i < a->sheets; i++)
    if (strcmp(a->seen[i], absu) == 0) return;
  strcpy(a->seen[a->sheets++], absu);
  if (read_url(absu, &data, &dn) != 0 || !data) {
    free(data);
    return;
  }
  if (dn > CSS_SHEET_MAX) dn = CSS_SHEET_MAX;
  css_imports(a, data, dn, absu, depth); /* os importados valem antes desta folha */
  css_put(a, data, dn);
  free(data);
}

/* Junta o CSS externo da pagina, na ordem: folhas de <link rel=stylesheet>
 * (menos media=print) e os @import dos blocos <style>. */
static char* gather_css(const char* html, int len, const char* base, int* outn) {
  css_acc_t a;
  int i;
  memset(&a, 0, sizeof a);
  *outn = 0;
  for (i = 1; i + 6 < len && a.sheets < CSS_SHEETS && a.n < CSS_TOTAL; i++) {
    char tag[1024], v[URL_MAX];
    int j = i, k = 0;
    if (html[i - 1] != '<') continue;
    if (strncasecmp(html + i, "link", 4) != 0 && strncasecmp(html + i, "style", 5) != 0) continue;
    while (j < len && html[j] != '>' && k < (int)sizeof tag - 1) tag[k++] = html[j++];
    tag[k] = 0;
    if (strncasecmp(tag, "style", 5) == 0) {
      /* <style>...</style>: so os @import; o resto o html.c le */
      int e = j;
      while (e + 8 <= len && strncasecmp(html + e, "</style", 7) != 0) e++;
      if (j < e) css_imports(&a, html + j + 1, e - j - 1, base, 0);
      i = e;
      continue;
    }
    if (!tag_attr(tag, "rel", v, (int)sizeof v) || !ci_has(v, "stylesheet") || ci_has(v, "alternate"))
      continue;
    if (tag_attr(tag, "media", v, (int)sizeof v) && ci_has(v, "print") && !ci_has(v, "screen") &&
        !ci_has(v, "all"))
      continue;
    if (tag_attr(tag, "href", v, (int)sizeof v) && v[0]) css_fetch(&a, base, v, 0);
  }
  *outn = a.n;
  return a.b;
}

/* Tecla ou clique no meio do pedido: a pagina de antes fica, e a barra diz
 * que parou (antes ficava "5% conectando..." como se ainda carregasse). */
static void stopped_status(const char* what) {
  char b[200];
  snprintf(b, sizeof b, "interrompido: %s", what);
  set_status(b);
  load_pct = -1;
}

static void status_done(const char* msg) {
  if (doc.js_alert[0]) {
    char b[200];
    snprintf(b, sizeof b, "%s | %s", doc.js_alert, msg);
    set_status(b);
  } else set_status(msg);
  if (async_phase) {
    strncpy(page_status, status, sizeof(page_status) - 1);
    page_status[sizeof(page_status) - 1] = 0;
  } else load_pct = -1;
}

static void show_html(const char* html, int len, const char* base, int latin1, int fetch_img) {
  char* css = 0;
  int cssn = 0;
  async_phase = 0;
  pics_free();
  doc_free(&doc);
  if (fetch_img) css = gather_css(html, len, base, &cssn);
  doc_layout(&doc, html, len, cols, base, latin1, gui_mode, css, cssn);
  free(css);
  scroll = 0;
  if (fetch_img && gui_mode) {
    /* CSS ja entrou. Scripts e figuras seguem no laco, um por vez. */
    async_phase = 1;
    async_i = 0;
    async_js = 0;
    load_pct = 90;
  } else {
    if (fetch_img) load_scripts();
    js_run(&doc);
    if (gui_mode && fetch_img) load_images();
    load_pct = -1;
  }
}

static void show_error(const char* title, const char* msg) {
  char page[700];
  snprintf(page, sizeof(page),
           "<html><head><title>%s</title></head><body><h1>%s</h1><p>%s</p>"
           "<p><a href=\"about:inicio\">Pagina inicial</a></p></body></html>",
           title, title, msg);
  show_html(page, (int)strlen(page), "about:erro", 0, 0);
}

static int body_is_image(const char* ct, const unsigned char* b, int n) {
  if (ct[0] && (ci_has(ct, "png") || ci_has(ct, "jpeg") || ci_has(ct, "jpg") ||
                ci_has(ct, "gif") || ci_has(ct, "icon")))
    return 1;
  if (!b || n < 4) return 0;
  if (n >= 8 && b[0] == 137 && b[1] == 80 && b[2] == 78 && b[3] == 71) return 1;
  if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return 1;
  if (n >= 6 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F') return 1;
  if (n >= 6 && b[0] == 0 && b[1] == 0 && (b[2] == 1 || b[2] == 2)) return 1;
  return 0;
}

/* A caixa segue a proporcao da figura, sem passar da janela.
 * Devolve -1 se a figura nao decodificou. */
static int show_picture(const char* url, const char* body, int n) {
  char page[URL_MAX + 128];
  img_t im;
  int w = 480, h = 360, maxw = (cols - 2) * 8, maxh = 30 * 16;
  int ok = 0;
  memset(&im, 0, sizeof(im));
  if (gui_mode && img_decode(body, n, &im) == 0 && im.w > 0 && im.h > 0) {
    ok = 1;
    w = im.w;
    h = im.h;
    if (w > maxw) {
      h = h * maxw / w;
      w = maxw;
    }
    if (h > maxh) {
      w = w * maxh / h;
      h = maxh;
    }
    if (w < 8) w = 8;
    if (h < 16) h = 16;
  }
  snprintf(page, sizeof(page),
           "<html><head><title>Imagem</title></head><body>"
           "<img src=\"%s\" width=\"%d\" height=\"%d\"></body></html>",
           url, w, h);
  show_html(page, (int)strlen(page), url, 0, 0);
  if (doc.nimgs > 0 && ok) {
    pics[0] = im;
    doc.imgs[0].state = 1;
    return 0;
  }
  img_free(&im);
  if (doc.nimgs > 0) doc.imgs[0].state = -1;
  return gui_mode ? -1 : 0;
}

/* Linha no topo das buscas de texto: as mesmas palavras nos outros buscadores. */
static void search_bar(int kind, const char* q, char* out, int max) {
  static const struct {
    int kind;
    const char* name;
  } other[] = { { SEARCH_IMAGES, "Imagens" }, { SEARCH_DDG, "DuckDuckGo" },
                { SEARCH_GOOGLE, "Google" }, { SEARCH_BING, "Bing" } };
  char enc[400];
  int i, n, sep = 0;
  if (search_enc(enc, 0, (int)sizeof(enc), q) < 0) enc[0] = 0;
  n = snprintf(out, (size_t)max, "<p>%s%s<b>Buscar tambem:</b>", search_note,
               search_note[0] ? "<br>" : "");
  for (i = 0; i < 4 && n < max; i++) {
    if (other[i].kind == kind) continue;
    n += snprintf(out + n, (size_t)(max - n), "%s <a href=\"about:busca?q=%s&amp;em=%s\">%s</a>",
                  sep ? " |" : "", enc, other[i].name, other[i].name);
    sep = 1;
  }
  if (n < max) snprintf(out + n, (size_t)(max - n), "</p>");
  search_note[0] = 0;
}

/* Busca recusada: tenta o proximo buscador e explica no topo. */
static int search_next(int kind, const char* q, int push, int fresh) {
  char t[300];
  const char* why;
  const char* next;
  int n;
  if (kind == SEARCH_GOOGLE) {
    why = "<b>O Google recusou a busca:</b> ele so responde a navegadores com JavaScript, "
          "que este nao roda.";
    next = "d";
  } else if (kind == SEARCH_DDG) {
    why = "<b>O DuckDuckGo pediu um desafio anti-robo</b> (captcha com caixas de marcar e "
          "JavaScript), que este navegador nao mostra. Tente de novo mais tarde.";
    next = "b";
  } else {
    return 0;
  }
  n = (int)strlen(search_note);
  snprintf(search_note + n, sizeof(search_note) - (size_t)n, "%s%s Resultados do %s:",
           n ? "<br>" : "", why, next[0] == 'd' ? "DuckDuckGo" : "Bing");
  snprintf(t, sizeof(t), "%s %s", next, q);
  navigate(t, push, fresh);
  return 1;
}

/* Imagem da busca que nao abriu: mostra a copia do Bing no lugar. */
static int show_copy(const char* url, int push, int fresh) {
  char copy[URL_MAX];
  if (!search_image_copy(url, copy, URL_MAX)) return 0;
  navigate(copy, push, fresh);
  set_status("A imagem original nao abriu (formato, tamanho ou TLS); esta e a copia do Bing.");
  return 1;
}

static void load_file(const char* path) {
  FILE* f = fopen(path, "rb");
  char* buf;
  long n;
  if (!f) {
    show_error("Arquivo nao encontrado", path);
    return;
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = (char*)malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    show_error("Sem memoria", path);
    return;
  }
  n = (long)fread(buf, 1, (size_t)n, f);
  buf[n] = 0;
  fclose(f);
  if (ci_has(path, ".htm")) show_html(buf, (int)n, path, 0, 1);
  else if (ci_has(path, ".png") || ci_has(path, ".jpg") || ci_has(path, ".jpeg") ||
           ci_has(path, ".gif") || ci_has(path, ".ico"))
    show_picture(path, buf, (int)n);
  else {
    pics_free();
    doc_free(&doc);
    doc_layout_text(&doc, buf, (int)n, cols, gui_mode);
    strncpy(doc.title, path, sizeof(doc.title) - 1);
    scroll = 0;
  }
  free(buf);
  status_done(path);
}

/* carrega url; push = guarda no historico */
static void navigate(const char* input, int push, int fresh) {
  url_t u;
  char url[URL_MAX];
  char via[URL_MAX];
  char q[256];
  int typed_scheme, kind = 0;
  if (search_unwrap(input, via, URL_MAX) || search_url(input, via, URL_MAX)) input = via;
  typed_scheme = ci_has(input, "://");
  focus_field = -1;
  if (url_parse(input, &u) != 0) {
    show_error("Endereco invalido", input);
    set_status("endereco invalido");
    return;
  }
  url_format(&u, url, URL_MAX);
  is_tls = 0;
  if (strcmp(u.scheme, "about") == 0) {
    show_html(start_page, (int)strlen(start_page), url, 0, 1);
    status_done("pagina inicial");
  } else if (strcmp(u.scheme, "file") == 0) {
    load_file(u.path);
  } else {
    http_resp_t r;
    int rc, flags;
    char msg[200];
    /* a URL pedida, antes dos redirecionamentos. Resultado de busca muda a
     * cada vez: nem le nem grava o cache. */
    kind = search_kind(url, q, (int)sizeof(q));
    flags = kind ? (HTTP_FRESH | HTTP_NOSTORE) : (fresh ? HTTP_FRESH : 0);
    set_status("carregando...");
    rc = http_get(url, &r, progress, flags);
    if (rc != 0 && r.aborted) {
      http_free(&r);
      stopped_status(input);
      return;
    }
    if (rc != 0 && !typed_scheme && strcmp(u.scheme, "https") == 0) {
      /* digitado sem esquema: tenta http:// */
      char alt[URL_MAX];
      u.port = 80;
      strcpy(u.scheme, "http");
      url_format(&u, alt, URL_MAX);
      rc = http_get(alt, &r, progress, flags);
      if (rc != 0 && r.aborted) {
        http_free(&r);
        stopped_status(input);
        return;
      }
    }
    if (rc != 0) {
      if (show_copy(url, push, fresh)) {
        http_free(&r);
        return;
      }
      tls_warn = 0;
      show_error("Falha ao carregar", r.err);
      set_status(r.err);
    } else if (kind && search_refused(kind, r.status, r.body, r.body_len) &&
               search_next(kind, q, push, fresh)) {
      http_free(&r);
      return;
    } else {
      strcpy(url, r.url);
      is_tls = r.tls;
      tls_warn = r.tls && !r.tls_ok;
      if (!ci_has(r.content_type, "html") && !ci_has(r.content_type, "xml") &&
          !ci_has(r.content_type, "text") &&
          body_is_image(r.content_type, (const unsigned char*)r.body, r.body_len)) {
        if (show_picture(url, r.body, r.body_len) != 0 && show_copy(url, push, fresh)) {
          http_free(&r);
          return;
        }
      } else if (r.content_type[0] && !ci_has(r.content_type, "html") &&
                 !ci_has(r.content_type, "text") && !ci_has(r.content_type, "xml")) {
        if (show_copy(url, push, fresh)) {
          http_free(&r);
          return;
        }
        snprintf(msg, sizeof(msg), "O servidor mandou %s (%d bytes), que este navegador nao mostra.",
                 r.content_type, r.body_len);
        show_error("Tipo nao suportado", msg);
      } else if (ci_has(r.content_type, "text/plain")) {
        async_phase = 0;
        load_pct = -1;
        pics_free();
        doc_free(&doc);
        doc_layout_text(&doc, r.body, r.body_len, cols, gui_mode);
        scroll = 0;
      } else if (kind == SEARCH_IMAGES) {
        int n = 0;
        char* grid = search_images_html(r.body, r.body_len, q, cols, &n);
        if (grid) show_html(grid, n, url, 0, 1);
        else show_error("Sem memoria", "para a pagina de imagens");
        free(grid);
      } else if (kind) {
        char bar[1600];
        int bn;
        char* page;
        search_bar(kind, q, bar, (int)sizeof(bar));
        bn = (int)strlen(bar);
        page = (char*)malloc((size_t)(bn + r.body_len + 1));
        if (page) {
          memcpy(page, bar, (size_t)bn);
          memcpy(page + bn, r.body, (size_t)r.body_len);
          page[bn + r.body_len] = 0;
          show_html(page, bn + r.body_len, url, 0, 1);
          free(page);
        } else {
          show_html(r.body, r.body_len, url, 0, 1);
        }
      } else {
        show_html(r.body, r.body_len, url, ci_has(r.content_type, "8859") ||
                                           ci_has(r.content_type, "1252"), 1);
      }
      if (r.from_cache)
        snprintf(msg, sizeof(msg), "%d - %d KB - cache D:\\temp\\net", r.status, r.body_len / 1024);
      else if (r.cache_saved)
        snprintf(msg, sizeof(msg), "%d - %d KB - gravou D:\\temp\\net - %s", r.status,
                 r.body_len / 1024, r.tls ? (r.secure[0] ? r.secure : "HTTPS") : "HTTP");
      else
        snprintf(msg, sizeof(msg), "%d - %d KB - %s", r.status, r.body_len / 1024,
                 r.tls ? (r.secure[0] ? r.secure : "HTTPS") : "HTTP");
      status_done(msg);
      http_free(&r);
    }
  }
  search_note[0] = 0;
  /* sem figuras nem scripts por carregar, a conexao guardada nao serve mais */
  if (!async_phase) http_idle_close();
  strcpy(cur_url, url);
  gui_edit_set(&addr, url);
  if (push) {
    if (hist_pos < HIST_MAX - 1) hist_pos++;
    else memmove(hist[0], hist[1], sizeof(hist[0]) * (HIST_MAX - 1));
    strcpy(hist[hist_pos], url);
    hist_n = hist_pos + 1;
  }
  if (gui_mode) {
    char t[160];
    int i, pick = -1;
    snprintf(t, sizeof(t), "%s - Navegador", doc.title[0] ? doc.title : url);
    gui_title(t);
    for (i = 0; i < doc.nfields; i++) {
      if (doc.fields[i].hidden || doc.fields[i].submit || doc.fields[i].line < 0) continue;
      if (strcmp(doc.fields[i].name, "q") == 0) { pick = i; break; }
      if (pick < 0) pick = i;
    }
    focus_field = pick;
    if (pick >= 0) focus_addr = 0;
  }
}

static void go_back(void) {
  if (hist_pos <= 0) return;
  hist_pos--;
  navigate(hist[hist_pos], 0, 0);
}

/* ---------- desenho ---------- */
static void layout_geometry(void) {
  int small = 0;
  W = gui_width();
  H = gui_height();
  CW = gui_char_w();
  CH = gui_char_h();
  small = W < 500;
  bar_h = CH + 12;
  status_h = CH + 12;
  content_y = bar_h;
  content_h = H - bar_h - status_h;
  btn_back_x = 4;
  btn_back_w = (small ? 1 : 6) * CW + 12;
  btn_rel_x = btn_back_x + btn_back_w + 4;
  btn_rel_w = (small ? 1 : 10) * CW + 12;
  btn_go_w = 2 * CW + 16;
  btn_go_x = W - btn_go_w - 4;
  addr_x = btn_rel_x + btn_rel_w + 6;
  addr_w = btn_go_x - addr_x - 6;
  cols = (W - SCROLL_W - 16) / CW;
  rows = content_h / CH;
  if (rows < 1) rows = 1;
}

static int max_scroll(void) {
  int m = doc.nlines - rows;
  return m > 0 ? m : 0;
}

static void clamp_scroll(void) {
  if (scroll > max_scroll()) scroll = max_scroll();
  if (scroll < 0) scroll = 0;
}

static void thumb_geom(int* ty, int* th) {
  int track = content_h;
  int total = doc.nlines > rows ? doc.nlines : rows;
  *th = track * rows / total;
  if (*th < 20) *th = 20;
  if (*th > track) *th = track;
  *ty = content_y + (max_scroll() ? (track - *th) * scroll / max_scroll() : 0);
}

static void draw(void) {
  int small = W < 500;
  int i, ty, th;
  int sx = W - SCROLL_W;
  gui_fill(0, 0, W, bar_h, 0xD4D0C8);
  gui_fill(0, bar_h - 1, W, 1, 0x808080);
  gui_button(btn_back_x, 4, btn_back_w, CH + 4, small ? "<" : "Voltar", hist_pos > 0);
  gui_button(btn_rel_x, 4, btn_rel_w, CH + 4, small ? "R" : "Recarregar", 1);
  gui_edit_draw(&addr, addr_x, 4, addr_w, CH + 4, focus_addr);
  gui_button(btn_go_x, 4, btn_go_w, CH + 4, "Ir", 1);

  gui_fill(0, content_y, sx, content_h, 0xFFFFFF);
  for (i = doc_first_run(&doc, scroll); i < doc.nruns; i++) {
    const run_t* r = &doc.runs[i];
    int y, x;
    if (r->line >= scroll + rows) break;
    y = content_y + (r->line - scroll) * CH;
    x = 8 + r->col * CW;
    if (r->flags & RUN_HR) {
      gui_fill(x, y + CH / 2, r->len * CW, 1, 0x808080);
      continue;
    }
    if (r->flags & RUN_IMG) {
      /* uma vez por figura: na linha de cima ou na primeira visivel */
      int first = r->img_row == 0 || r->line == scroll;
      if (first && r->img >= 0 && r->img < doc.nimgs) {
        int top = r->line - r->img_row;
        int iy = content_y + (top - scroll) * CH;
        int iw = r->len * CW;
        int ih = doc.imgs[r->img].lines * CH;
        if (doc.imgs[r->img].state > 0 && pics[r->img].rgba) {
          gui_image(x, iy, iw, ih, pics[r->img].rgba, pics[r->img].w, pics[r->img].h,
                    0, content_y, sx, content_h);
        } else {
          int y0 = iy < content_y ? content_y : iy;
          int y1 = iy + ih;
          if (y1 > content_y + content_h) y1 = content_y + content_h;
          if (y1 > y0) gui_fill(x, y0, iw, y1 - y0, 0xC0C0C0);
        }
      }
      continue;
    }
    {
      unsigned bg = 0xFFFFFF;
      int tf = GUI_TEXT_NOBG;
      if (r->flags & RUN_BG) {
        bg = r->bg;
        tf = 0;
      }
      if (r->flags & RUN_BOLD) tf |= GUI_TEXT_BOLD;
      if (r->flags & RUN_ULINE) tf |= GUI_TEXT_UNDERLINE;
      if (r->field >= 0 && r->field < doc.nfields && !doc.fields[r->field].submit &&
          !doc.fields[r->field].hidden) {
        doc_field_t* f = &doc.fields[r->field];
        int box = f->box > 0 ? f->box : (r->len > 2 ? r->len - 2 : 1);
        int n = (int)strlen(f->value);
        int show, from, on, k;
        char cell[48];
        int c;
        if (box > 40) box = 40;
        if (box < 1) box = 1;
        show = n > box ? box : n;
        from = n - show;
        on = r->field == focus_field;
        cell[0] = '[';
        for (c = 0; c < box; c++) cell[1 + c] = (c < show) ? f->value[from + c] : '_';
        cell[box + 1] = ']';
        gui_text(x, y, cell, box + 2, r->color, on ? 0xFFFF99 : bg, tf & ~GUI_TEXT_NOBG);
        if (on) {
          k = 1 + show;
          if (k > box) k = box;
          gui_fill(x + k * CW, y + CH - 2, CW > 2 ? CW - 1 : 1, 2, 0x000080);
        }
        continue;
      }
      gui_text(x, y, doc.text + r->off, r->len, r->color, bg, tf);
    }
  }

  gui_fill(sx, content_y, SCROLL_W, content_h, 0xE0E0E0);
  thumb_geom(&ty, &th);
  gui_fill(sx + 2, ty, SCROLL_W - 4, th, 0x909090);

  gui_fill(0, H - status_h, W, status_h, 0xD4D0C8);
  gui_fill(0, H - status_h, W, 1, 0x808080);
  gui_text_fit(6, H - status_h + 2, status, W - 12, tls_warn ? 0x804000 : 0x000000, 0xD4D0C8,
               GUI_TEXT_NOBG);
  if (load_pct >= 0) {
    int bw = W - 12;
    int fill = bw * load_pct / 100;
    if (bw < 1) bw = 1;
    if (fill < 0) fill = 0;
    if (fill > bw) fill = bw;
    gui_fill(6, H - 6, bw, 4, 0xFFFFFF);
    if (fill > 0) gui_fill(6, H - 6, fill, 4, 0x000080);
  }
}

/* Monta action?nome=valor e abre. button < 0 omite o botao (Enter no campo). */
static void submit_form(int field, int button) {
  char url[URL_MAX];
  const char* base;
  int form, n, i, first, q;
  if (field < 0 || field >= doc.nfields) return;
  form = doc.fields[field].form;
  base = (form >= 0 && form < doc.nforms && doc.form_action[form][0]) ? doc.form_action[form] : cur_url;
  n = 0;
  while (base[n] && n < URL_MAX - 1) {
    url[n] = base[n];
    n++;
  }
  url[n] = 0;
  q = 0;
  while (url[q] && url[q] != '?' && url[q] != '#') q++;
  url[q] = 0;
  n = q;
  first = 1;
  for (i = 0; i < doc.nfields; i++) {
    doc_field_t* e = &doc.fields[i];
    int m;
    if (e->form != form) continue;
    if (e->submit) {
      if (i != button || !e->name[0]) continue;
    } else if (!e->name[0]) continue;
    if (n + 2 >= URL_MAX) break;
    url[n++] = first ? '?' : '&';
    first = 0;
    m = search_enc(url, n, URL_MAX, e->name);
    if (m < 0) break;
    n = m;
    if (n + 1 >= URL_MAX) break;
    url[n++] = '=';
    m = search_enc(url, n, URL_MAX, e->value);
    if (m < 0) break;
    n = m;
  }
  url[n] = 0;
  navigate(url, 1, 0);
}

static void field_key(int k) {
  doc_field_t* f;
  int n;
  if (focus_field < 0 || focus_field >= doc.nfields) return;
  f = &doc.fields[focus_field];
  if (f->submit || f->hidden) { focus_field = -1; return; }
  n = (int)strlen(f->value);
  if (k == '\r' || k == '\n') {
    submit_form(focus_field, -1);
    return;
  }
  if (k == 27) {
    focus_field = -1;
    return;
  }
  if (k == '\b') {
    if (n > 0) f->value[n - 1] = 0;
    return;
  }
  if (k >= 32 && k < 127 && n < (int)sizeof(f->value) - 1) {
    f->value[n] = (char)k;
    f->value[n + 1] = 0;
  }
}

static void click_content(int x, int y) {
  int line = scroll + (y - content_y) / CH;
  int col = (x - 8) / CW;
  int field, link;
  if (x < 8) return;
  field = doc_field_at(&doc, line, col);
  if (field >= 0 && field < doc.nfields) {
    if (doc.fields[field].submit) submit_form(field, field);
    else if (!doc.fields[field].hidden) focus_field = field;
    return;
  }
  focus_field = -1;
  link = doc_link_at(&doc, line, col);
  if (link >= 0 && link < doc.nlinks) {
    char target[URL_MAX];
    strcpy(target, doc.links[link]);
    navigate(target, 1, 0);
  }
}

static int run_gui(const char* first) {
  gui_event_t ev;
  int dragging = 0, drag_dy = 0;
  if (gui_open("Navegador") != 0) return -1;
  gui_mode = 1;
  layout_geometry();
  navigate(first, 1, 0);
  for (;;) {
    int sx = W - SCROLL_W;
    int busy;
    clamp_scroll();
    if (pending_ev_have) {
      ev = pending_ev;
      pending_ev_have = 0;
    } else {
      draw();
      gui_present();
      busy = async_phase != 0;
      gui_wait(&ev, busy ? 0 : -1);
    }
    if (ev.type == GUI_EV_CLOSE) break;
    if (ev.type == GUI_EV_DOWN) {
      if (gui_hit(addr_x, 4, addr_w, CH + 4, ev.x, ev.y)) {
        if (focus_addr) gui_edit_click(&addr, addr_x, ev.x);
        else gui_edit_select_all(&addr);
        focus_addr = 1;
        focus_field = -1;
      } else if (gui_hit(btn_back_x, 4, btn_back_w, CH + 4, ev.x, ev.y)) {
        go_back();
      } else if (gui_hit(btn_rel_x, 4, btn_rel_w, CH + 4, ev.x, ev.y)) {
        char u[URL_MAX];
        strcpy(u, cur_url);
        navigate(u, 0, 1);
      } else if (gui_hit(btn_go_x, 4, btn_go_w, CH + 4, ev.x, ev.y)) {
        char u[URL_MAX];
        strcpy(u, addr.text);
        focus_addr = 0;
        navigate(u, 1, 0);
      } else if (gui_hit(sx, content_y, SCROLL_W, content_h, ev.x, ev.y)) {
        int ty, th;
        focus_addr = 0;
        thumb_geom(&ty, &th);
        if (ev.y < ty) scroll -= rows - 1;
        else if (ev.y >= ty + th) scroll += rows - 1;
        else {
          dragging = 1;
          drag_dy = ev.y - ty;
        }
      } else if (gui_hit(0, content_y, sx, content_h, ev.x, ev.y)) {
        focus_addr = 0;
        click_content(ev.x, ev.y);
      }
    } else if (ev.type == GUI_EV_DRAG && dragging) {
      int ty, th, track;
      thumb_geom(&ty, &th);
      track = content_h - th;
      if (track > 0) scroll = (ev.y - drag_dy - content_y) * max_scroll() / track;
    } else if (ev.type == GUI_EV_UP) {
      dragging = 0;
    } else if (ev.type == GUI_EV_WHEEL) {
      scroll += ev.key * WHEEL_LINES;
    } else if (ev.type == GUI_EV_KEY) {
      int k = ev.key;
      if (k == '\t') {
        if (focus_field >= 0) {
          focus_field = -1;
          focus_addr = 1;
          gui_edit_select_all(&addr);
        } else {
          focus_addr = !focus_addr;
          if (focus_addr) gui_edit_select_all(&addr);
        }
      } else if (focus_addr) {
        if (k == 27) focus_addr = 0;
        else if (gui_edit_key(&addr, k) == 2) {
          char u[URL_MAX];
          strcpy(u, addr.text);
          focus_addr = 0;
          navigate(u, 1, 0);
        }
      } else if (focus_field >= 0) {
        field_key(k);
      } else if (k == 27) {
        break;
      } else if (k == GUI_KEY_DOWN) scroll += 1;
      else if (k == GUI_KEY_UP) scroll -= 1;
      else if (k == GUI_KEY_PGDN || k == ' ') scroll += rows - 1;
      else if (k == GUI_KEY_PGUP) scroll -= rows - 1;
      else if (k == GUI_KEY_HOME) scroll = 0;
      else if (k == GUI_KEY_END) scroll = max_scroll();
      else if (k == '\b') go_back();
    }
    if (async_phase && !pending_ev_have) async_step();
  }
  http_idle_close();
  gui_close();
  return 0;
}

/* sem WIN: imprime a pagina como texto e a lista de links */
static void dump_doc(void) {
  char* line = (char*)malloc((size_t)cols + 2);
  int i = 0, l;
  if (!line) return;
  printf("== %s ==\n", doc.title[0] ? doc.title : cur_url);
  for (l = 0; l < doc.nlines; l++) {
    int end = 0, k;
    memset(line, ' ', (size_t)cols);
    for (; i < doc.nruns && doc.runs[i].line == l; i++) {
      const run_t* r = &doc.runs[i];
      for (k = 0; k < r->len && r->col + k < cols; k++) {
        char c = (r->flags & RUN_HR) ? '-' : doc.text[r->off + k];
        line[r->col + k] = c;
      }
      if (r->col + r->len > end) end = r->col + r->len;
    }
    if (end > cols) end = cols;
    line[end] = 0;
    printf("%s\n", line);
  }
  if (doc.nlinks) {
    printf("-- %d links --\n", doc.nlinks);
    for (i = 0; i < doc.nlinks && i < 40; i++) printf("[%d] %s\n", i + 1, doc.links[i]);
  }
  printf("[%s]\n", status);
  free(line);
}

int main(int argc, char** argv) {
  static char first[URL_MAX];
  int i, n = 0;
  strcpy(first, "about:inicio");
  /* BROWSER g gato preto: os argumentos voltam a ser uma linha */
  for (i = 1; i < argc; i++) {
    int k = (int)strlen(argv[i]);
    if (n + k + 2 > URL_MAX) break;
    if (n) first[n++] = ' ';
    memcpy(first + n, argv[i], (size_t)k + 1);
    n += k;
  }
  if (run_gui(first) == 0) return 0;
  gui_mode = 0;
  cols = 78;
  navigate(first, 1, 0);
  http_idle_close();
  dump_doc();
  return 0;
}
