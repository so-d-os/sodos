/* main.c - BROWSER.EXE: navegador minimo do WIN.
 *   cc -o browser.exe browser.c url.c http.c html.c js.c search.c cache.cpp
 *      inflate.c youtube.c tls.dll gui.dll img.dll
 * No WIN abre uma janela com barra de endereco, Voltar, Recarregar, JS,
 * rolagem e links clicaveis. Fora do WIN (shell do DOS) imprime a pagina como
 * texto. Palavras na barra viram busca (search.c): DuckDuckGo, Google, Bing ou
 * imagens. O YouTube vira pagina simples (youtube.c).
 *   BROWSER [-t] [-j] [endereco ou busca]
 *   -t  sempre como texto, com o tempo total (testes pela PONTE ou pela API)
 *   -j  liga o JS.EXE (o mesmo que o botao JS ou SET BROWSERJS=1)
 *   SET BROWSERDUMP=arquivo  grava o corpo de cada pagina recebida */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <so.h>
#include <gui.h>
#include <img.h>
#include "browser.h"
#include "cache.h"

#define HIST_MAX 32
#define SCROLL_W 14
#define WHEEL_LINES 3 /* linhas por passo da roda do rato */
#define JS_MAX (64 * 1024) /* bytes por <script src> baixado */
#define JS_EXT_MAX 3       /* mais <script src> que isso: pagina de framework */

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
  "<li>Botao <b>JS</b>: roda os scripts da pagina no <code>JS.EXE</code> (o SpiderMonkey "
  "do Firefox) antes de mostrar. A faixa verde diz que esta ligado.</li>"
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
static int btn_rel_x, btn_rel_w, addr_x, addr_w, btn_go_x, btn_go_w, btn_js_x, btn_js_w;
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

static int read_url_fn(const char* url, char** data, int* len, progress_fn fn) {
  url_t u;
  *data = 0;
  *len = 0;
  if (url_parse(url, &u) != 0) return -1;
  if (strcmp(u.scheme, "http") == 0 || strcmp(u.scheme, "https") == 0) {
    http_resp_t r;
    if (http_get(url, &r, fn, 0) != 0 || r.status < 200 || r.status >= 300) {
      /* pagina de erro (404...) nao serve como CSS, script ou figura */
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

static int read_url(const char* url, char** data, int* len) {
  return read_url_fn(url, data, len, progress);
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

/* ---------- tamanho das figuras ja vistas ----------
 * html.c pergunta (img_hint) o tamanho de uma <img> sem width/height. Se a
 * figura ja foi decodificada nesta sessao, a caixa sai do tamanho certo; se
 * falhou (SVG, formato que a IMG.DLL nao le), nao reserva caixa. */
#define HINTS 512
static struct {
  unsigned h;
  int w, ht;
} hints[HINTS];
static int hint_next;

static unsigned url_hash(const char* s) {
  unsigned h = 2166136261u;
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= 16777619u;
  }
  return h ? h : 1;
}

static void hint_put(const char* url, int w, int h) {
  unsigned k = url_hash(url);
  int i;
  for (i = 0; i < HINTS; i++)
    if (hints[i].h == k) break;
  if (i == HINTS) {
    i = hint_next;
    hint_next = (hint_next + 1) % HINTS;
  }
  hints[i].h = k;
  hints[i].w = w;
  hints[i].ht = h;
}

int img_hint(const char* url, int* w, int* h) {
  unsigned k = url_hash(url);
  int i;
  for (i = 0; i < HINTS; i++) {
    if (hints[i].h == k) {
      *w = hints[i].w;
      *h = hints[i].ht;
      return 1;
    }
  }
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
  if (doc.imgs[i].state > 0) hint_put(doc.imgs[i].url, pics[i].w, pics[i].h);
  else hint_put(doc.imgs[i].url, 0, 0);
}

/* ---------- refazer o layout ----------
 * A ultima pagina (HTML e CSS) fica guardada: quando as figuras chegam com
 * tamanho diferente do adivinhado, ou falham, a pagina e diagramada de novo
 * com os tamanhos certos. As figuras ja decodificadas sao reaproveitadas. */
static char* last_html;
static int last_len, last_latin1;
static char* last_css;
static int last_cssn;
static char last_base[URL_MAX];

static void keep_page(const char* html, int len, const char* base, int latin1, char* css, int cssn) {
  free(last_html);
  free(last_css);
  last_html = (char*)malloc((size_t)len + 1);
  if (last_html) {
    memcpy(last_html, html, (size_t)len);
    last_html[len] = 0;
  }
  last_len = last_html ? len : 0;
  last_css = css; /* fica com o CSS (antes era liberado logo depois do layout) */
  last_cssn = css ? cssn : 0;
  last_latin1 = latin1;
  strncpy(last_base, base ? base : "", URL_MAX - 1);
  last_base[URL_MAX - 1] = 0;
}

/* 1 se alguma figura saiu de tamanho diferente do que o layout reservou */
static int images_moved(void) {
  int i;
  for (i = 0; i < doc.nimgs && i < DOC_IMGS; i++) {
    const doc_img_t* m = &doc.imgs[i];
    if (!m->fit) continue; /* tamanho veio do HTML/CSS */
    if (m->state < 0) return 1;
    if (m->state > 0 && pics[i].w > 0) {
      int want = pics[i].w * m->lines * CH;     /* largura que a altura pede */
      int have = m->cols * CW * pics[i].h;
      if (want > have + have / 5 || have > want + want / 5) return 1;
    }
  }
  return 0;
}

static void relayout(void) {
  img_t keep[DOC_IMGS];
  char kurl[DOC_IMGS][96];
  int kstate[DOC_IMGS];
  int nkeep = 0, i, j, top = scroll;
  if (!last_html) return;
  for (i = 0; i < doc.nimgs && i < DOC_IMGS; i++) {
    keep[nkeep] = pics[i];
    memset(&pics[i], 0, sizeof(img_t));
    strncpy(kurl[nkeep], doc.imgs[i].url, sizeof kurl[0] - 1);
    kurl[nkeep][sizeof kurl[0] - 1] = 0;
    kstate[nkeep] = doc.imgs[i].state;
    nkeep++;
  }
  doc_free(&doc);
  doc_layout(&doc, last_html, last_len, cols, last_base, last_latin1, gui_mode, last_css, last_cssn);
  for (j = 0; j < doc.nimgs && j < DOC_IMGS; j++) {
    for (i = 0; i < nkeep; i++) {
      if (kstate[i] == 2 || strncmp(kurl[i], doc.imgs[j].url, sizeof kurl[0] - 1) != 0) continue;
      if (kstate[i] > 0) {
        pics[j] = keep[i];
        memset(&keep[i], 0, sizeof(img_t));
      }
      doc.imgs[j].state = kstate[i] > 0 ? 1 : -1;
      kstate[i] = 2; /* usada */
      break;
    }
  }
  for (i = 0; i < nkeep; i++) img_free(&keep[i]);
  scroll = top;
}

static void load_images(void) {
  int i;
  for (i = 0; i < doc.nimgs && i < DOC_IMGS; i++) load_one_image(i);
}

/* Pagina com mais de JS_EXT_MAX scripts externos e de framework (React,
 * pacotes do GitHub e do YouTube, de centenas de KB cada): o js.c nao roda
 * nada disso, e baixar tudo era a maior parte do tempo de carga. */
static int ext_scripts(void) {
  int i, n = 0;
  for (i = 0; i < doc.nscripts; i++)
    if (doc.scripts[i].url[0]) n++;
  return n;
}

static void load_one_script(int i) {
  char* data = 0;
  int n = 0;
  if (doc.scripts[i].src || !doc.scripts[i].url[0]) return;
  if (ext_scripts() > JS_EXT_MAX) return;
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

/* Uma imagem ou um script por volta do laco. A pagina aparece antes do resto;
 * as figuras vem primeiro (sao o que se ve), os scripts depois. */
static int async_step(void) {
  int total, done;
  if (pending_ev_have) {
    async_phase = 0;
    load_pct = -1;
    http_idle_close();
    return 0;
  }
  if (async_phase == 1) {
    if (async_i < doc.nimgs && async_i < DOC_IMGS) {
      load_one_image(async_i);
      async_i++;
    } else {
      /* figuras com tamanho adivinhado que vieram diferentes: diagrama de
       * novo (antes dos scripts, que mexem no doc) */
      if (gui_mode && images_moved()) relayout();
      async_phase = 2;
      async_i = 0;
    }
  } else if (async_phase == 2) {
    while (async_i < doc.nscripts &&
           (doc.scripts[async_i].src || !doc.scripts[async_i].url[0] || ext_scripts() > JS_EXT_MAX))
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
      async_phase = 0;
    }
  }
  if (!async_phase) {
    load_pct = -1;
    if (page_status[0]) set_status(page_status);
    http_idle_close();
    return 0;
  }
  total = doc.nscripts + doc.nimgs;
  if (total < 1) total = 1;
  done = (async_phase == 2) ? doc.nimgs + async_i : async_i;
  load_pct = 90 + done * 10 / total;
  if (load_pct > 99) load_pct = 99;
  return 1;
}

/* ---------- CSS externo: <link rel=stylesheet> e @import ---------- */
#define CSS_SHEETS 128                /* arquivos por pagina (o Bing usa ~100) */
#define CSS_SHEET_MAX (512 * 1024)    /* bytes por arquivo */
#define CSS_TOTAL (2 * 1024 * 1024)   /* bytes somados */

typedef struct {
  char url[URL_MAX];
  char* data;
  int n, done;
} css_sheet_t;

typedef struct {
  char* b;
  int n, cap;
  css_sheet_t* sh;
  int ns;
} css_acc_t;

/* valor do atributo name= numa tag (texto depois do '<'); 0 se nao tem.
 * &amp; vira & (os href da Wikipedia vem assim). */
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
    while (*p && k < max - 1 && (q ? *p != q : !isspace((unsigned char)*p) && *p != '>')) {
      if (*p == '&' && strncmp(p, "&amp;", 5) == 0) {
        out[k++] = '&';
        p += 5;
        continue;
      }
      out[k++] = *p++;
    }
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

/* Barra de estado durante o CSS: "CSS 12/96 - recebendo r.bing.com" */
static int css_cur, css_cnt;
static int css_progress(const char* msg) {
  char b[200];
  while (*msg >= '0' && *msg <= '9') msg++;
  if (*msg == '%') msg++;
  while (*msg == ' ') msg++;
  snprintf(b, sizeof b, "CSS %d/%d - %s", css_cur, css_cnt, msg);
  return progress(b);
}

/* Guarda a folha na lista (na ordem da pagina); baixa depois. */
static void css_add_sheet(css_acc_t* a, const char* base, const char* href) {
  char absu[URL_MAX];
  int i;
  if (a->ns >= CSS_SHEETS || !a->sh) return;
  if (url_resolve(base ? base : "", href, absu, URL_MAX) != 0) return;
  if (strncmp(absu, "http", 4) != 0 && strncmp(absu, "file", 4) != 0) return;
  for (i = 0; i < a->ns; i++)
    if (strcmp(a->sh[i].url, absu) == 0) return;
  memset(&a->sh[a->ns], 0, sizeof(css_sheet_t));
  strcpy(a->sh[a->ns].url, absu);
  a->ns++;
}

/* @import "x.css"; @import url(x.css) screen; -> chama fn para cada endereco */
static void css_imports(css_acc_t* a, const char* css, int n, const char* base, int now) {
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
    if (!href[0]) continue;
    if (!now) {
      css_add_sheet(a, base, href);
    } else {
      /* @import dentro de uma folha ja baixada: busca agora, sem descer mais */
      char absu[URL_MAX];
      char* data = 0;
      int dn = 0;
      if (url_resolve(base, href, absu, URL_MAX) != 0) continue;
      if (read_url_fn(absu, &data, &dn, css_progress) == 0 && data) {
        if (dn > CSS_SHEET_MAX) dn = CSS_SHEET_MAX;
        css_put(a, data, dn);
      }
      free(data);
    }
  }
}

static int same_host(const char* u, const char* v) {
  const char* a = strstr(u, "://");
  const char* b = strstr(v, "://");
  if (!a || !b) return 0;
  a += 3;
  b += 3;
  while (*a && *a != '/' && *a == *b) {
    a++;
    b++;
  }
  return (*a == 0 || *a == '/') && (*b == 0 || *b == '/');
}

/* Junta o CSS externo da pagina, na ordem: folhas de <link rel=stylesheet>
 * (menos media=print) e os @import dos blocos <style>. As folhas sao
 * baixadas agrupadas por servidor: a conexao fica aberta entre elas (trocar
 * de servidor a cada folha custaria uma troca de chaves TLS por folha). */
static char* gather_css(const char* html, int len, const char* base, int* outn) {
  css_acc_t a;
  int i, j;
  memset(&a, 0, sizeof a);
  *outn = 0;
  a.sh = (css_sheet_t*)malloc(CSS_SHEETS * sizeof(css_sheet_t));
  if (!a.sh) return 0;
  for (i = 1; i + 6 < len && a.ns < CSS_SHEETS; i++) {
    char tag[2048], v[URL_MAX];
    int j2 = i, k = 0;
    if (html[i - 1] != '<') continue;
    if (strncasecmp(html + i, "link", 4) != 0 && strncasecmp(html + i, "style", 5) != 0) continue;
    while (j2 < len && html[j2] != '>' && k < (int)sizeof tag - 1) tag[k++] = html[j2++];
    tag[k] = 0;
    if (strncasecmp(tag, "style", 5) == 0) {
      /* <style>...</style>: so os @import; o resto o html.c le */
      int e = j2;
      while (e + 8 <= len && strncasecmp(html + e, "</style", 7) != 0) e++;
      if (j2 < e) css_imports(&a, html + j2 + 1, e - j2 - 1, base, 0);
      i = e;
      continue;
    }
    if (!tag_attr(tag, "rel", v, (int)sizeof v) || !ci_has(v, "stylesheet") || ci_has(v, "alternate"))
      continue;
    if (tag_attr(tag, "media", v, (int)sizeof v) && ci_has(v, "print") && !ci_has(v, "screen") &&
        !ci_has(v, "all"))
      continue;
    if (tag_attr(tag, "href", v, (int)sizeof v) && v[0]) css_add_sheet(&a, base, v);
  }
  css_cnt = a.ns;
  css_cur = 0;
  for (i = 0; i < a.ns && !pending_ev_have; i++) {
    if (a.sh[i].done) continue;
    for (j = i; j < a.ns && !pending_ev_have; j++) {
      if (a.sh[j].done || !same_host(a.sh[i].url, a.sh[j].url)) continue;
      css_cur++;
      load_pct = 5 + 80 * css_cur / (a.ns ? a.ns : 1);
      a.sh[j].done = 1;
      if (read_url_fn(a.sh[j].url, &a.sh[j].data, &a.sh[j].n, css_progress) != 0) {
        free(a.sh[j].data);
        a.sh[j].data = 0;
        a.sh[j].n = 0;
      }
    }
    if (!a.sh[i].done) {
      /* file:// ou endereco sem host */
      css_cur++;
      a.sh[i].done = 1;
      if (read_url_fn(a.sh[i].url, &a.sh[i].data, &a.sh[i].n, css_progress) != 0) {
        free(a.sh[i].data);
        a.sh[i].data = 0;
        a.sh[i].n = 0;
      }
    }
  }
  for (i = 0; i < a.ns; i++) {
    css_sheet_t* sh = &a.sh[i];
    int dn = sh->n;
    if (!sh->data) continue;
    if (dn > CSS_SHEET_MAX) dn = CSS_SHEET_MAX;
    if (!pending_ev_have) css_imports(&a, sh->data, dn, sh->url, 1); /* valem antes desta folha */
    css_put(&a, sh->data, dn);
    free(sh->data);
  }
  free(a.sh);
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
  keep_page(html, len, base, latin1, css, cssn); /* fica com o css, para o relayout */
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

/* ---------- JavaScript de verdade: JS.EXE (SpiderMonkey) + BRDOM.JS ----------
 * Com o botao JS ligado (ou SET BROWSERJS=1, ou -j), a pagina com <script>
 * passa pelo JS.EXE antes do layout. O navegador grava numa pasta de trabalho
 * o DADOS.JS (o HTML numa string e a lista de scripts) e os <script src> ja
 * baixados (S0.JS...), roda "JS.EXE --no-jit-backend BRDOM.JS" ali e desenha o
 * HTML que volta: o DOM depois dos scripts, do load e dos timers. Os dados vao
 * como JavaScript porque o load() do shell le arquivos no SODOS e o readFile
 * nao (ele chama fstat). Scripts que a pagina criou e que faltaram sao
 * baixados e o JS.EXE roda mais uma vez. */
#define JSX_SCRIPTS 48
#define JSX_SCRIPT_MAX (3 * 1024 * 1024)
#define JSX_OUT_MAX (8 * 1024 * 1024)

static int js_full;            /* 1 = paginas com <script> passam pelo JS.EXE */
static char jsx_note[160];     /* resumo para a barra de estado */
static char jsx_goto[URL_MAX]; /* location = ... pedido pela pagina */
static int jsx_hops;           /* redirecionamentos do JS seguidos em sequencia */

typedef struct {
  char raw[URL_MAX]; /* src como esta no HTML */
  char abs[URL_MAX];
  char file[16];     /* S<n>.JS */
  int ok;            /* 0 por baixar, 1 gravado, -1 falhou */
} jsx_script_t;

typedef struct {
  char* b;
  int n, cap; /* cap < 0: faltou memoria */
} ob_t;

static void ob_put(ob_t* o, const char* s, int n) {
  if (o->cap < 0 || n <= 0) return;
  if (o->n + n + 1 > o->cap) {
    int nc = o->cap ? o->cap : 65536;
    char* nb;
    while (nc < o->n + n + 1) nc *= 2;
    nb = (char*)realloc(o->b, (size_t)nc);
    if (!nb) {
      free(o->b);
      o->b = 0;
      o->cap = -1;
      return;
    }
    o->b = nb;
    o->cap = nc;
  }
  memcpy(o->b + o->n, s, (size_t)n);
  o->n += n;
  o->b[o->n] = 0;
}

static void ob_str(ob_t* o, const char* s) { ob_put(o, s, (int)strlen(s)); }

/* tamanho da sequencia UTF-8 em s[0..n), 0 se invalida */
static int utf8_len(const unsigned char* s, int n) {
  int need, k;
  unsigned c = s[0];
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF) need = 1;
  else if (c >= 0xE0 && c <= 0xEF) need = 2;
  else if (c >= 0xF0 && c <= 0xF4) need = 3;
  else return 0;
  if (need >= n) return 0;
  for (k = 1; k <= need; k++)
    if ((s[k] & 0xC0) != 0x80) return 0;
  return need + 1;
}

/* texto como string de JavaScript ("..."). Byte que nao e UTF-8 (ou
 * latin1 = 1) vira o caractere Latin-1 dele. */
static void ob_jsstr(ob_t* o, const char* s, int n, int latin1) {
  const unsigned char* p = (const unsigned char*)s;
  char esc[8];
  int i = 0;
  ob_put(o, "\"", 1);
  while (i < n) {
    unsigned c = p[i];
    int k;
    if (c >= 0x20 && c < 0x80 && c != '"' && c != '\\') {
      int j = i + 1;
      while (j < n && p[j] >= 0x20 && p[j] < 0x80 && p[j] != '"' && p[j] != '\\') j++;
      ob_put(o, s + i, j - i);
      i = j;
      continue;
    }
    if (c == '"' || c == '\\') {
      esc[0] = '\\';
      esc[1] = (char)c;
      ob_put(o, esc, 2);
      i++;
      continue;
    }
    if (c == '\n') {
      ob_put(o, "\\n", 2);
      i++;
      continue;
    }
    k = (c >= 0x80 && !latin1) ? utf8_len(p + i, n - i) : 0;
    if (k > 1) {
      if (k == 3 && c == 0xE2 && p[i + 1] == 0x80 && (p[i + 2] == 0xA8 || p[i + 2] == 0xA9))
        ob_str(o, p[i + 2] == 0xA8 ? "\\u2028" : "\\u2029");
      else
        ob_put(o, s + i, k);
      i += k;
      continue;
    }
    snprintf(esc, sizeof esc, "\\u%04x", c); /* controle ou Latin-1 */
    ob_put(o, esc, 6);
    i++;
  }
  ob_put(o, "\"", 1);
}

/* copia em UTF-8 valido: byte solto vira o caractere Latin-1 dele */
static char* utf8_fix(const char* s, int n, int* outn) {
  ob_t o;
  int i = 0;
  memset(&o, 0, sizeof o);
  while (i < n) {
    int j = i;
    while (j < n) {
      int k = (unsigned char)s[j] < 0x80 ? 1 : utf8_len((const unsigned char*)s + j, n - j);
      if (k == 0) break;
      j += k;
    }
    ob_put(&o, s + i, j - i);
    if (j < n) {
      unsigned c = (unsigned char)s[j];
      char t[2];
      t[0] = (char)(0xC0 | (c >> 6));
      t[1] = (char)(0x80 | (c & 0x3F));
      ob_put(&o, t, 2);
      j++;
    }
    i = j;
  }
  if (!o.b && o.cap >= 0) ob_put(&o, "\n", 1);
  *outn = o.n;
  return o.b;
}

/* X:\NOME na unidade do .EXE, no D: ou no U: */
static int find_on_drives(const char* name, char* out, int max) {
  char dr[3];
  int i;
  dr[0] = (char)k_exe_drive();
  dr[1] = 'D';
  dr[2] = 'U';
  for (i = 0; i < 3; i++) {
    if (!dr[i] || (i > 0 && dr[i] == dr[0])) continue;
    snprintf(out, (size_t)max, "%c:\\%s", dr[i], name);
    if (k_fsize(out) > 0) return 1;
  }
  return 0;
}

/* pasta de trabalho: Z:\TEMP\BRJS (disco em RAM) ou D:\TEMP\BRJS */
static void jsx_dir(char* out, int max) {
  char d = (k_drive_mask() & (1u << ('Z' - 'A'))) ? 'Z' : 'D';
  snprintf(out, (size_t)max, "%c:\\TEMP", d);
  k_mkdir(out);
  snprintf(out, (size_t)max, "%c:\\TEMP\\BRJS", d);
  k_mkdir(out);
}

static int has_script(const char* html, int len) {
  int i;
  for (i = 0; i + 7 < len; i++)
    if (html[i] == '<' && strncasecmp(html + i + 1, "script", 6) == 0) return 1;
  return 0;
}

static int jsx_known(const jsx_script_t* sc, int n, const char* abs) {
  int d;
  for (d = 0; d < n; d++)
    if (strcmp(sc[d].abs, abs) == 0) return 1;
  return 0;
}

/* <script src> da pagina, na ordem; type que nao e JS (module, json) fica de fora */
static int jsx_find_scripts(const char* html, int len, const char* base, jsx_script_t* sc, int max) {
  int i, n = 0;
  for (i = 1; i + 7 < len && n < max; i++) {
    char tag[2048], v[URL_MAX], ty[64], absu[URL_MAX];
    int j = i, k = 0, e;
    if (html[i - 1] != '<' || strncasecmp(html + i, "script", 6) != 0 || isalnum((unsigned char)html[i + 6]))
      continue;
    while (j < len && html[j] != '>' && k < (int)sizeof tag - 1) tag[k++] = html[j++];
    tag[k] = 0;
    e = j;
    while (e + 8 <= len && strncasecmp(html + e, "</script", 8) != 0) e++;
    i = e;
    if (!tag_attr(tag, "src", v, (int)sizeof v) || !v[0]) continue;
    if (tag_attr(tag, "type", ty, (int)sizeof ty) && ty[0] && !ci_has(ty, "javascript") &&
        !ci_has(ty, "ecmascript"))
      continue;
    if (url_resolve(base, v, absu, URL_MAX) != 0 || jsx_known(sc, n, absu)) continue;
    strcpy(sc[n].abs, absu);
    strncpy(sc[n].raw, v, URL_MAX - 1);
    sc[n].raw[URL_MAX - 1] = 0;
    sc[n].file[0] = 0;
    sc[n].ok = 0;
    n++;
  }
  return n;
}

static int jsx_cur, jsx_cnt;
static int jsx_progress(const char* msg) {
  char b[200];
  while (*msg >= '0' && *msg <= '9') msg++;
  if (*msg == '%') msg++;
  while (*msg == ' ') msg++;
  snprintf(b, sizeof b, "JS %d/%d - %s", jsx_cur, jsx_cnt, msg);
  return progress(b);
}

/* baixa os scripts ainda nao baixados e grava S<n>.JS (UTF-8 valido) */
static void jsx_fetch(jsx_script_t* sc, int n, const char* dir) {
  int i;
  jsx_cnt = n;
  for (i = 0; i < n && !pending_ev_have; i++) {
    char* data = 0;
    int dn = 0;
    jsx_cur = i + 1;
    if (sc[i].ok) continue;
    sc[i].ok = -1;
    if (read_url_fn(sc[i].abs, &data, &dn, jsx_progress) == 0 && data) {
      char path[200];
      int fn2 = 0;
      char* fixed;
      if (dn > JSX_SCRIPT_MAX) dn = JSX_SCRIPT_MAX;
      fixed = utf8_fix(data, dn, &fn2);
      snprintf(sc[i].file, sizeof sc[i].file, "S%d.JS", i);
      snprintf(path, sizeof path, "%s\\%s", dir, sc[i].file);
      if (fixed && k_fsave(path, fixed, fn2) == 0) sc[i].ok = 1;
      free(fixed);
    }
    free(data);
  }
}

static int jsx_write_data(const char* dir, const char* url, const char* html, int len, int latin1,
                          const jsx_script_t* sc, int n) {
  ob_t o;
  char path[200], num[80];
  int i, rc, first = 1;
  memset(&o, 0, sizeof o);
  ob_str(&o, "var __BRDADOS = {\nurl: ");
  ob_jsstr(&o, url, (int)strlen(url), 0);
  snprintf(num, sizeof num, ",\nw: %d, h: %d,\nscripts: {", gui_mode ? W - SCROLL_W : cols * 8,
           gui_mode ? content_h : 600);
  ob_str(&o, num);
  for (i = 0; i < n; i++) {
    if (sc[i].ok != 1) continue;
    ob_str(&o, first ? "\n" : ",\n");
    first = 0;
    ob_jsstr(&o, sc[i].raw, (int)strlen(sc[i].raw), 0);
    ob_str(&o, ": ");
    ob_jsstr(&o, sc[i].file, (int)strlen(sc[i].file), 0);
    ob_str(&o, ",\n");
    ob_jsstr(&o, sc[i].abs, (int)strlen(sc[i].abs), 0);
    ob_str(&o, ": ");
    ob_jsstr(&o, sc[i].file, (int)strlen(sc[i].file), 0);
  }
  ob_str(&o, "},\nhtml: ");
  ob_jsstr(&o, html, len, latin1);
  ob_str(&o, "\n};\n");
  if (!o.b) return -1;
  snprintf(path, sizeof path, "%s\\DADOS.JS", dir);
  rc = k_fsave(path, o.b, o.n);
  free(o.b);
  return rc;
}

/* Mensagens do BRDOM.JS ate a marca <<<BRDOM>>>; devolve o HTML (dentro de
 * out) ou 0. Scripts que faltaram entram em sc com ok = 0. */
static char* jsx_parse(char* out, int len, int* htmln, jsx_script_t* sc, int* nsc, int* errs,
                       char* last, int lmax) {
  char* mark = strstr(out, "<<<BRDOM>>>\n");
  char* end = mark ? mark : out + len;
  char* p = out;
  while (p < end) {
    char* nl = (char*)memchr(p, '\n', (size_t)(end - p));
    if (!nl) nl = end;
    *nl = 0;
    if (strncmp(p, "[js-ir] ", 8) == 0) {
      if (!jsx_goto[0]) {
        strncpy(jsx_goto, p + 8, URL_MAX - 1);
        jsx_goto[URL_MAX - 1] = 0;
      }
    } else if (strncmp(p, "[js-falta] ", 11) == 0) {
      if (*nsc < JSX_SCRIPTS && !jsx_known(sc, *nsc, p + 11)) {
        jsx_script_t* s = &sc[*nsc];
        strncpy(s->abs, p + 11, URL_MAX - 1);
        s->abs[URL_MAX - 1] = 0;
        strcpy(s->raw, s->abs);
        s->file[0] = 0;
        s->ok = 0;
        (*nsc)++;
      }
    } else if (strncmp(p, "[js-alert] ", 11) == 0) {
      snprintf(last, (size_t)lmax, "alert: %s", p + 11);
    } else if (strncmp(p, "[js] erro", 9) == 0) {
      (*errs)++;
      if (strncmp(last, "alert", 5) != 0) snprintf(last, (size_t)lmax, "%s", p + 5);
    } else if (!mark && p[0] && !last[0]) {
      snprintf(last, (size_t)lmax, "%s", p); /* JS.EXE nao iniciou: a mensagem dele */
    }
    p = nl + 1;
  }
  if (!mark) return 0;
  p = mark + 12;
  *htmln = len - (int)(p - out);
  if (*htmln < 0) *htmln = 0;
  return p;
}

/* 1 = mostrou a pagina depois do JS.EXE; 0 = siga sem ele (desligado, sem
 * <script>, sem JS.EXE ou BRDOM.JS, ou nada voltou) */
static int js_page(const char* html, int len, const char* url, int latin1) {
  char jsexe[64], brdom[64], dir[64], line[200], save[160], last[120];
  jsx_script_t* sc;
  char* out;
  char* page = 0;
  int nsc, round, olen = 0, htmln = 0, errs = 0;
  unsigned tjs = 0;
  jsx_note[0] = 0;
  jsx_goto[0] = 0;
  last[0] = 0;
  if (!js_full || !has_script(html, len)) return 0;
  if (!find_on_drives("JS.EXE", jsexe, (int)sizeof jsexe) ||
      !find_on_drives("BRDOM.JS", brdom, (int)sizeof brdom)) {
    snprintf(jsx_note, sizeof jsx_note, "JS: falta JS.EXE ou BRDOM.JS no D:");
    return 0;
  }
  sc = (jsx_script_t*)calloc(JSX_SCRIPTS, sizeof(jsx_script_t));
  out = (char*)malloc(JSX_OUT_MAX);
  if (!sc || !out) {
    free(sc);
    free(out);
    return 0;
  }
  jsx_dir(dir, (int)sizeof dir);
  nsc = jsx_find_scripts(html, len, url, sc, JSX_SCRIPTS);
  for (round = 0; round < 2 && !pending_ev_have; round++) {
    int before;
    unsigned t1;
    jsx_fetch(sc, nsc, dir);
    if (pending_ev_have) break;
    if (jsx_write_data(dir, url, html, len, latin1, sc, nsc) != 0) {
      snprintf(last, sizeof last, "nao gravei %s\\DADOS.JS", dir);
      break;
    }
    http_idle_close();
    progress("95% JS.EXE: rodando os scripts da pagina...");
    /* --no-threads: o GC do JS.EXE de 11/2026 esperava threads auxiliares
     * que nao rodavam e travava */
    snprintf(line, sizeof line, "%s --no-jit-backend --no-threads %s", jsexe, brdom);
    save[0] = 0;
    k_pwd(save, (int)sizeof save);
    k_chdir(dir);
    t1 = k_ms();
    olen = 0;
    k_shell_capture(line, out, JSX_OUT_MAX, &olen);
    tjs += k_ms() - t1;
    if (save[0]) k_chdir(save);
    if (olen < 0) olen = -olen;
    if (olen > JSX_OUT_MAX - 1) olen = JSX_OUT_MAX - 1;
    out[olen] = 0;
    before = nsc;
    errs = 0;
    page = jsx_parse(out, olen, &htmln, sc, &nsc, &errs, last, (int)sizeof last);
    if (!page || nsc == before) break; /* nada novo para baixar */
  }
  if (page) {
    show_html(page, htmln, url, 0, 1);
    if (errs)
      snprintf(jsx_note, sizeof jsx_note, "JS.EXE %u ms, %d erro%s: %s", tjs, errs, errs > 1 ? "s" : "", last);
    else if (last[0])
      snprintf(jsx_note, sizeof jsx_note, "JS.EXE %u ms - %s", tjs, last);
    else
      snprintf(jsx_note, sizeof jsx_note, "JS.EXE %u ms", tjs);
  } else if (!pending_ev_have) {
    snprintf(jsx_note, sizeof jsx_note, "JS.EXE nao devolveu a pagina: %s", last[0] ? last : "sem saida");
  }
  free(sc);
  free(out);
  return page != 0;
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
  if (ci_has(path, ".htm")) {
    if (!js_page(buf, (int)n, path, 0)) show_html(buf, (int)n, path, 0, 1);
  } else if (ci_has(path, ".png") || ci_has(path, ".jpg") || ci_has(path, ".jpeg") ||
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
  if (jsx_note[0]) {
    char b[200];
    snprintf(b, sizeof b, "%s | %s", path, jsx_note);
    jsx_note[0] = 0;
    status_done(b);
  } else {
    status_done(path);
  }
}

/* carrega url; push = guarda no historico */
static void navigate(const char* input, int push, int fresh) {
  url_t u;
  char url[URL_MAX];
  char via[URL_MAX];
  char q[256], ytq[256];
  int typed_scheme, kind = 0, ytk;
  if (search_unwrap(input, via, URL_MAX) || search_url(input, via, URL_MAX)) input = via;
  typed_scheme = ci_has(input, "://");
  focus_field = -1;
  if (url_parse(input, &u) != 0) {
    show_error("Endereco invalido", input);
    set_status("endereco invalido");
    return;
  }
  url_format(&u, url, URL_MAX);
  /* sites que tem versao sem JavaScript, ou que viram pagina simples aqui */
  if ((yt_rewrite(url, via, URL_MAX) || search_rewrite(url, via, URL_MAX)) && url_parse(via, &u) == 0) {
    url_format(&u, url, URL_MAX);
    typed_scheme = 1;
  }
  ytk = yt_kind(url, ytq, (int)sizeof(ytq));
  is_tls = 0;
  if (strcmp(u.scheme, "about") == 0) {
    show_html(start_page, (int)strlen(start_page), url, 0, 1);
    status_done("pagina inicial");
  } else if (ytk == YT_HOME) {
    int n = 0;
    char* page = yt_html(YT_HOME, 0, 0, "", url, &n);
    if (page) show_html(page, n, url, 0, 1);
    free(page);
    status_done("YouTube: busque um video");
  } else if (strcmp(u.scheme, "file") == 0) {
    load_file(u.path);
  } else {
    http_resp_t r;
    int rc, flags;
    char msg[200];
    /* a URL pedida, antes dos redirecionamentos. Resultado de busca muda a
     * cada vez: nem le nem grava o cache. */
    kind = search_kind(url, q, (int)sizeof(q));
    flags = (kind || ytk == YT_SEARCH) ? (HTTP_FRESH | HTTP_NOSTORE) : (fresh ? HTTP_FRESH : 0);
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
    if (rc == 0 && r.body) {
      /* SET BROWSERDUMP=D:\TEMP\PAGINA.TXT: grava o corpo recebido (depuracao
       * de adaptadores como o do YouTube) */
      char dp[160];
      if (k_getenv("BROWSERDUMP", dp, (int)sizeof dp) > 0 && dp[0]) k_fsave(dp, r.body, r.body_len);
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
      } else if (ytk) {
        int n = 0;
        char* page = yt_html(ytk, r.body, r.body_len, ytq, url, &n);
        if (page) show_html(page, n, url, 0, 1);
        else show_error("Sem memoria", "para a pagina do YouTube");
        free(page);
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
        int l1 = ci_has(r.content_type, "8859") || ci_has(r.content_type, "1252");
        if (!js_page(r.body, r.body_len, url, l1)) show_html(r.body, r.body_len, url, l1, 1);
      }
      if (r.from_cache)
        snprintf(msg, sizeof(msg), "%d - %d KB - cache %s", r.status, r.body_len / 1024,
                 cache_where());
      else if (r.cache_saved)
        snprintf(msg, sizeof(msg), "%d - %d KB - gravou %s - %s", r.status, r.body_len / 1024,
                 cache_where(), r.tls ? (r.secure[0] ? r.secure : "HTTPS") : "HTTP");
      else
        snprintf(msg, sizeof(msg), "%d - %d KB - %s", r.status, r.body_len / 1024,
                 r.tls ? (r.secure[0] ? r.secure : "HTTPS") : "HTTP");
      if (jsx_note[0]) {
        size_t ml = strlen(msg);
        snprintf(msg + ml, sizeof(msg) - ml, " | %s", jsx_note);
        jsx_note[0] = 0;
      }
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
    snprintf(t, sizeof(t), "%s - Navegador", doc.title_ascii[0] ? doc.title_ascii : url);
    gui_title(t);
    for (i = 0; i < doc.nfields; i++) {
      if (doc.fields[i].hidden || doc.fields[i].submit || doc.fields[i].line < 0) continue;
      if (strcmp(doc.fields[i].name, "q") == 0) { pick = i; break; }
      if (pick < 0) pick = i;
    }
    focus_field = pick;
    if (pick >= 0) focus_addr = 0;
  }
  /* a pagina mandou ir para outro endereco (location = ...): segue, no lugar
   * desta no historico, ate 3 vezes seguidas */
  if (jsx_goto[0] && jsx_hops < 3 && strcmp(jsx_goto, url) != 0) {
    char g[URL_MAX];
    strcpy(g, jsx_goto);
    jsx_goto[0] = 0;
    jsx_hops++;
    if (push && hist_pos > 0) hist_pos--;
    navigate(g, push, 0);
    return;
  }
  jsx_goto[0] = 0;
  jsx_hops = 0;
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
  btn_js_x = btn_rel_x + btn_rel_w + 4;
  btn_js_w = 2 * CW + 12;
  addr_x = btn_js_x + btn_js_w + 6;
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
  /* JS: ligado = as paginas passam pelo JS.EXE (faixa verde embaixo) */
  gui_button(btn_js_x, 4, btn_js_w, CH + 4, "JS", 1);
  if (js_full) gui_fill(btn_js_x + 3, CH + 4, btn_js_w - 6, 3, 0x00A000);
  gui_edit_draw(&addr, addr_x, 4, addr_w, CH + 4, focus_addr);
  gui_button(btn_go_x, 4, btn_go_w, CH + 4, "Ir", 1);

  gui_fill(0, content_y, sx, content_h, doc.page_bg);
  /* fundos e bordas de blocos e celulas, antes do texto */
  for (i = 0; i < doc.nrects; i++) {
    const doc_rect_t* q = &doc.rects[i];
    int y0, y1, x0, x1, cy0, cy1, cx0, cx1;
    if (!q->flags || q->line1 < q->line0) continue;
    if (q->line1 < scroll || q->line0 > scroll + rows) continue;
    y0 = content_y + (q->line0 - scroll) * CH;
    y1 = content_y + (q->line1 + 1 - scroll) * CH;
    x0 = 8 + q->col0 * CW - CW / 2;
    x1 = 8 + q->col1 * CW + CW / 2;
    cy0 = y0 < content_y ? content_y : y0;
    cy1 = y1 > content_y + content_h ? content_y + content_h : y1;
    cx0 = x0 < 0 ? 0 : x0;
    cx1 = x1 > sx ? sx : x1;
    if (cx1 <= cx0 || cy1 <= cy0) continue;
    if (q->flags & RECT_FILL) gui_fill(cx0, cy0, cx1 - cx0, cy1 - cy0, q->bg);
    if ((q->flags & RECT_TOP) && y0 >= cy0) gui_fill(cx0, y0, cx1 - cx0, 1, q->border);
    if ((q->flags & RECT_BOTTOM) && y1 - 1 < cy1) gui_fill(cx0, y1 - 1, cx1 - cx0, 1, q->border);
    if ((q->flags & RECT_LEFT) && x0 >= cx0) gui_fill(x0, cy0, 1, cy1 - cy0, q->border);
    if ((q->flags & RECT_RIGHT) && x1 - 1 < cx1) gui_fill(x1 - 1, cy0, 1, cy1 - cy0, q->border);
  }
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
      } else if (gui_hit(btn_js_x, 4, btn_js_w, CH + 4, ev.x, ev.y)) {
        /* liga ou desliga o JS.EXE e mostra a pagina de novo (do cache) */
        char u[URL_MAX];
        js_full = !js_full;
        strcpy(u, cur_url);
        navigate(u, 0, 0);
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
    } else if (ev.type == GUI_EV_RESIZE) {
      /* maximizar/restaurar: a grade muda de largura, a pagina se refaz */
      int old_cols = cols;
      layout_geometry();
      if (cols != old_cols && !async_phase) relayout();
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
  int i, n = 0, text = 0;
  unsigned t0;
  strcpy(first, "about:inicio");
  doc.page_bg = 0xFFFFFF;
  {
    char v[8];
    if (k_getenv("BROWSERJS", v, (int)sizeof v) > 0 && v[0] == '1') js_full = 1;
  }
  /* -t: sempre como texto, com o tempo (testes pela PONTE ou pela API, que
   * rodam fora do WIN mas abririam a janela do navegador). -j: liga o JS.EXE. */
  while (argc > 1 && argv[1][0] == '-' && (argv[1][1] == 't' || argv[1][1] == 'j') && !argv[1][2]) {
    if (argv[1][1] == 't') text = 1;
    else js_full = 1;
    argc--;
    argv++;
  }
  /* BROWSER g gato preto: os argumentos voltam a ser uma linha */
  for (i = 1; i < argc; i++) {
    int k = (int)strlen(argv[i]);
    if (n + k + 2 > URL_MAX) break;
    if (n) first[n++] = ' ';
    memcpy(first + n, argv[i], (size_t)k + 1);
    n += k;
  }
  if (!text && run_gui(first) == 0) return 0;
  gui_mode = 0;
  cols = 78;
  t0 = k_ms();
  navigate(first, 1, 0);
  http_idle_close();
  dump_doc();
  if (text) printf("[tempo total: %u ms]\n", k_ms() - t0);
  return 0;
}
