/* youtube.c - o YouTube sem o JavaScript dele.
 *
 * A pagina do YouTube vem quase vazia: os dados estao num JSON dentro de um
 * <script> (var ytInitialData = {...}; e, no video, ytInitialPlayerResponse).
 * Aqui esse JSON e lido por busca de texto (sem montar a arvore: tem ~1 MB) e
 * vira uma pagina simples:
 *  - inicio (youtube.com): a caixa de busca;
 *  - busca (/results?search_query=): videos com miniatura, canal, duracao e
 *    visualizacoes (videoRenderer);
 *  - video (/watch?v=): miniatura, titulo, canal, visualizacoes, data,
 *    descricao e os relacionados (lockupViewModel ou compactVideoRenderer);
 *  - canal e playlist: os videos que vierem na pagina.
 * O video nao toca: o endereco do fluxo so sai com o JavaScript do YouTube.
 * Formato conferido em 11/2026. Se o YouTube mudar, a pagina diz que nao
 * achou os videos, em vez de mostrar 1 MB de script. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "browser.h"

#define YT_ITEMS 40
#define THUMB_W 160 /* 20 colunas */
#define THUMB_H 96  /* 6 linhas: perto de 16:9 na grade de 8x16 */
#define BIG_W 320
#define BIG_H 176
#define YT "https://www.youtube.com"

typedef struct {
  char* b;
  int n, cap;
} yb_t;

static void put_n(yb_t* s, const char* t, int k) {
  if (!s->b || k <= 0) return;
  if (s->n + k + 1 > s->cap) {
    int nc = s->cap * 2;
    char* nb;
    while (nc < s->n + k + 1) nc *= 2;
    nb = (char*)realloc(s->b, (size_t)nc);
    if (!nb) {
      free(s->b);
      s->b = 0;
      return;
    }
    s->b = nb;
    s->cap = nc;
  }
  memcpy(s->b + s->n, t, (size_t)k);
  s->n += k;
  s->b[s->n] = 0;
}

static void put(yb_t* s, const char* t) { put_n(s, t, (int)strlen(t)); }

/* texto ou atributo HTML; br = 1 troca quebra de linha por <br> */
static void put_esc_br(yb_t* s, const char* t, int br) {
  for (; *t; t++) {
    if (*t == '&') put(s, "&amp;");
    else if (*t == '"') put(s, "&quot;");
    else if (*t == '<') put(s, "&lt;");
    else if (*t == '>') put(s, "&gt;");
    else if (*t == '\n') put(s, br ? "<br>\n" : " ");
    else put_n(s, t, 1);
  }
}

static void put_esc(yb_t* s, const char* t) { put_esc_br(s, t, 0); }

static int starts(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

/* primeira ocorrencia de pat em [p, end); 0 se nao tem */
static const char* find(const char* p, const char* end, const char* pat) {
  int k = (int)strlen(pat);
  const char* last = end - k;
  char c0 = pat[0];
  for (; p <= last; p++)
    if (*p == c0 && memcmp(p, pat, (size_t)k) == 0) return p;
  return 0;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = tolower(c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static unsigned hex4(const char* p) {
  unsigned v = 0;
  int k;
  for (k = 0; k < 4; k++) {
    int h = hexval((unsigned char)p[k]);
    if (h < 0) return 0xFFFD;
    v = v * 16 + (unsigned)h;
  }
  return v;
}

static int put_utf8(char* out, int o, int max, unsigned cp) {
  if (cp < 0x80) {
    if (o + 1 < max) out[o++] = (char)cp;
  } else if (cp < 0x800) {
    if (o + 2 < max) {
      out[o++] = (char)(0xC0 | (cp >> 6));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    }
  } else if (cp < 0x10000) {
    if (o + 3 < max) {
      out[o++] = (char)(0xE0 | (cp >> 12));
      out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    }
  } else if (o + 4 < max) {
    out[o++] = (char)(0xF0 | (cp >> 18));
    out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[o++] = (char)(0x80 | (cp & 0x3F));
  }
  return o;
}

/* string JSON que comeca em p (logo depois da aspa) -> UTF-8 em out */
static int jstr(const char* p, const char* end, char* out, int max) {
  int o = 0, cut = 0;
  while (p < end && *p != '"') {
    if (o >= max - 5) {
      cut = 1;
      break;
    }
    if (*p == '\\' && p + 1 < end) {
      char c;
      p++;
      if (*p == 'u' && p + 4 < end) {
        unsigned cp = hex4(p + 1);
        p += 5;
        if (cp >= 0xD800 && cp <= 0xDBFF && p + 5 < end && p[0] == '\\' && p[1] == 'u') {
          unsigned lo = hex4(p + 2);
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
          }
        }
        o = put_utf8(out, o, max, cp);
        continue;
      }
      c = *p++;
      if (c == 'n') c = '\n';
      else if (c == 't' || c == 'b' || c == 'f') c = ' ';
      else if (c == 'r') continue;
      out[o++] = c;
      continue;
    }
    out[o++] = *p++;
  }
  if (cut) {
    /* nao deixa um caractere UTF-8 pela metade no fim */
    while (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0x80) o--;
    if (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0xC0) o--;
  }
  out[o] = 0;
  return o;
}

/* pat termina na aspa de abertura de uma string: le a string. 1 se achou. */
static int get(const char* p, const char* end, const char* pat, char* out, int max) {
  const char* q = find(p, end, pat);
  out[0] = 0;
  if (!q) return 0;
  jstr(q + strlen(pat), end, out, max);
  return 1;
}

/* Depois de pat (ate window bytes), o primeiro "simpleText":"..." ou
 * "text":"..." ou "content":"...". Serve para os textos do YouTube, que vem
 * como {"simpleText":"x"}, {"runs":[{"text":"x"}]} ou {"content":"x"}. */
static int text_of(const char* p, const char* end, const char* pat, int window, char* out, int max) {
  static const char* keys[] = { "\"simpleText\":\"", "\"text\":\"", "\"content\":\"", 0 };
  const char* q = find(p, end, pat);
  const char* best = 0;
  const char* lim;
  int k, bl = 0;
  out[0] = 0;
  if (!q) return 0;
  q += strlen(pat);
  lim = q + window < end ? q + window : end;
  for (k = 0; keys[k]; k++) {
    const char* r = find(q, lim, keys[k]);
    if (r && (!best || r < best)) {
      best = r;
      bl = (int)strlen(keys[k]);
    }
  }
  if (!best) return 0;
  jstr(best + bl, end, out, max);
  return out[0] != 0;
}

/* ---------- itens (videos) ---------- */
#define IT_VIDEO   1 /* videoRenderer, compactVideoRenderer, gridVideoRenderer */
#define IT_LOCKUP  2 /* lockupViewModel (relacionados e canais, desde 2025) */

static const struct {
  const char* pat;
  int type;
} markers[] = { { "\"videoRenderer\":{", IT_VIDEO }, { "\"compactVideoRenderer\":{", IT_VIDEO },
                { "\"gridVideoRenderer\":{", IT_VIDEO }, { "\"lockupViewModel\":{", IT_LOCKUP },
                { 0, 0 } };

/* proximo item a partir de p; *type recebe o tipo. Uma passada so. */
static const char* next_item(const char* p, const char* end, int* type) {
  for (; p < end; p++) {
    int k;
    if (*p != '"') continue;
    for (k = 0; markers[k].pat; k++) {
      int n = (int)strlen(markers[k].pat);
      if (p + n <= end && p[1] == markers[k].pat[1] && memcmp(p, markers[k].pat, (size_t)n) == 0) {
        *type = markers[k].type;
        return p;
      }
    }
  }
  return 0;
}

typedef struct {
  char id[16];
  char title[200];
  char channel[80];
  char chan_url[96];
  char dur[16];
  char meta[120]; /* visualizacoes e data, ja juntas */
} yt_item_t;

static int valid_id(const char* id) {
  int i;
  for (i = 0; id[i]; i++)
    if (!isalnum((unsigned char)id[i]) && id[i] != '-' && id[i] != '_') return 0;
  return i == 11;
}

static void add_meta(yt_item_t* it, const char* t) {
  int n = (int)strlen(it->meta);
  if (!t[0]) return;
  snprintf(it->meta + n, sizeof(it->meta) - (size_t)n, "%s%s", n ? " - " : "", t);
}

static int read_video(const char* s, const char* e, yt_item_t* it) {
  char t[120];
  const char* q;
  get(s, e, "\"videoId\":\"", it->id, (int)sizeof(it->id));
  if (!valid_id(it->id)) return 0;
  text_of(s, e, "\"title\":{", 1500, it->title, (int)sizeof(it->title));
  if (!text_of(s, e, "\"ownerText\":{", 300, it->channel, (int)sizeof(it->channel)) &&
      !text_of(s, e, "\"longBylineText\":{", 300, it->channel, (int)sizeof(it->channel)))
    text_of(s, e, "\"shortBylineText\":{", 300, it->channel, (int)sizeof(it->channel));
  q = find(s, e, "\"canonicalBaseUrl\":\"/");
  if (q) {
    jstr(q + 20, e, t, (int)sizeof(t));
    snprintf(it->chan_url, sizeof(it->chan_url), "%s%s", YT, t);
  }
  text_of(s, e, "\"lengthText\":{", 600, it->dur, (int)sizeof(it->dur));
  if (text_of(s, e, "\"viewCountText\":{", 300, t, (int)sizeof(t)) ||
      text_of(s, e, "\"shortViewCountText\":{", 300, t, (int)sizeof(t)))
    add_meta(it, t);
  if (text_of(s, e, "\"publishedTimeText\":{", 200, t, (int)sizeof(t))) add_meta(it, t);
  return it->title[0] != 0;
}

static int read_lockup(const char* s, const char* e, yt_item_t* it) {
  char t[120];
  const char* rows;
  const char* q;
  int n = 0;
  if (!find(s, e, "LOCKUP_CONTENT_TYPE_VIDEO")) return 0; /* playlist, mix, canal */
  get(s, e, "\"contentId\":\"", it->id, (int)sizeof(it->id));
  if (!valid_id(it->id)) return 0;
  get(s, e, "\"lockupMetadataViewModel\":{\"title\":{\"content\":\"", it->title, (int)sizeof(it->title));
  get(s, e, "\"thumbnailBadgeViewModel\":{\"text\":\"", it->dur, (int)sizeof(it->dur));
  q = find(s, e, "\"canonicalBaseUrl\":\"/");
  if (q) {
    jstr(q + 20, e, t, (int)sizeof(t));
    snprintf(it->chan_url, sizeof(it->chan_url), "%s%s", YT, t);
  }
  /* metadataRows: [canal (e "e mais 5" na coautoria)], [visualizacoes, data].
   * As linhas acabam no "delimiter" ou no "menuButton" do lockup. */
  rows = find(s, e, "\"metadataRows\":");
  if (rows) {
    const char* rend = find(rows, e, "\"delimiter\":");
    const char* mb = find(rows, e, "\"menuButton\":");
    if (!rend || (mb && mb < rend)) rend = mb;
    if (!rend) rend = e;
    rows = find(rows, rend, "\"metadataParts\":[");
    while (rows && n < 3) {
      const char* next = find(rows + 1, rend, "\"metadataParts\":[");
      const char* lim = next ? next : rend;
      const char* p = rows;
      int parts = 0;
      while (parts < 4 && (q = find(p, lim, "\"text\":{\"content\":\"")) != 0) {
        jstr(q + 19, lim, t, (int)sizeof(t));
        if (n == 0) {
          int cl = (int)strlen(it->channel);
          snprintf(it->channel + cl, sizeof(it->channel) - (size_t)cl, "%s%s", cl ? " " : "", t);
        } else {
          add_meta(it, t);
        }
        p = q + 19;
        parts++;
      }
      rows = next;
      n++;
    }
  }
  return it->title[0] != 0;
}

/* Le ate max videos de [p, end); pula o id skip (o proprio video). */
static int read_items(const char* p, const char* end, yt_item_t* items, int max, const char* skip) {
  int n = 0, type = 0, ntype = 0;
  const char* s = next_item(p, end, &type);
  while (s && n < max) {
    const char* e = next_item(s + 1, end, &ntype);
    const char* lim = e ? e : end;
    yt_item_t* it = &items[n];
    int ok, k;
    memset(it, 0, sizeof(*it));
    ok = type == IT_LOCKUP ? read_lockup(s, lim, it) : read_video(s, lim, it);
    if (ok && skip && strcmp(it->id, skip) == 0) ok = 0;
    for (k = 0; ok && k < n; k++)
      if (strcmp(items[k].id, it->id) == 0) ok = 0;
    if (ok) n++;
    s = e;
    type = ntype;
  }
  return n;
}

/* ---------- partes da pagina ---------- */
static void put_head(yb_t* s, const char* title, const char* q) {
  put(s, "<html><head><title>");
  put_esc(s, title);
  put(s, "</title><style>"
         "body{background:#ffffff;color:#0f0f0f}"
         ".yt{background:#cc0000;color:#ffffff;font-weight:bold}"
         ".meta{color:#606060}.dur{color:#606060}"
         "</style></head><body>\n"
         "<form action=\"" YT "/results\"><span class=\"yt\"> YouTube </span> "
         "<input name=\"search_query\" size=\"40\" value=\"");
  put_esc(s, q ? q : "");
  put(s, "\"> <input type=\"submit\" value=\"Buscar\"></form>\n");
}

static void put_items(yb_t* s, const yt_item_t* items, int n) {
  int i;
  put(s, "<table>\n");
  for (i = 0; i < n; i++) {
    const yt_item_t* it = &items[i];
    char b[200];
    put(s, "<tr><td valign=\"top\"><a href=\"" YT "/watch?v=");
    put(s, it->id);
    snprintf(b, sizeof b, "\"><img src=\"https://i.ytimg.com/vi/%s/mqdefault.jpg\" width=\"%d\" "
                          "height=\"%d\" alt=\"",
             it->id, THUMB_W, THUMB_H);
    put(s, b);
    put_esc(s, it->title);
    put(s, "\"></a></td><td valign=\"top\"><a href=\"" YT "/watch?v=");
    put(s, it->id);
    put(s, "\"><b>");
    put_esc(s, it->title);
    put(s, "</b></a><br>");
    if (it->channel[0]) {
      if (it->chan_url[0]) {
        put(s, "<a href=\"");
        put_esc(s, it->chan_url);
        put(s, "\">");
        put_esc(s, it->channel);
        put(s, "</a>");
      } else {
        put_esc(s, it->channel);
      }
      put(s, "<br>");
    }
    put(s, "<span class=\"meta\">");
    if (it->dur[0]) {
      put_esc(s, it->dur);
      if (it->meta[0]) put(s, " - ");
    }
    put_esc(s, it->meta);
    put(s, "</span></td></tr>\n");
  }
  put(s, "</table>\n");
}

/* Inicio do JSON de name (ytInitialData...). Se vier como string de JS
 * ('{\x22...'), devolve uma copia sem os escapes em *own (malloc). */
static const char* json_of(const char* body, int len, const char* name, const char** end, char** own) {
  const char* bend = body + len;
  const char* p = body;
  const char* e;
  *own = 0;
  /* a atribuicao: var NAME = {...} ou window["NAME"] = ... (pula as mencoes) */
  for (;;) {
    int eq = 0;
    p = find(p, bend, name);
    if (!p) return 0;
    p += strlen(name);
    while (p < bend && (*p == ' ' || *p == '=' || *p == ']' || *p == '"')) {
      if (*p == '=') eq = 1;
      p++;
    }
    if (eq && p < bend && (*p == '{' || *p == '\'')) break;
  }
  e = find(p, bend, "</script>");
  if (!e) e = bend;
  if (p < bend && *p == '\'') {
    /* JSON.parse('...'): \xNN, \\, \' */
    char* o = (char*)malloc((size_t)(e - p) + 1);
    int k = 0;
    if (!o) return 0;
    for (p++; p < e && *p != '\''; p++) {
      if (*p == '\\' && p + 1 < e) {
        p++;
        if (*p == 'x' && p + 2 < e && hexval((unsigned char)p[1]) >= 0) {
          o[k++] = (char)(hexval((unsigned char)p[1]) * 16 + hexval((unsigned char)p[2]));
          p += 2;
          continue;
        }
        if (*p != '\\' && *p != '\'' && *p != '/') o[k++] = '\\';
      }
      o[k++] = *p;
    }
    o[k] = 0;
    *own = o;
    *end = o + k;
    return o;
  }
  *end = e;
  return p;
}

static void fmt_views(const char* n, char* out, int max) {
  int len = (int)strlen(n), i, o = 0;
  for (i = 0; i < len && o < max - 2; i++) {
    if (!isdigit((unsigned char)n[i])) break;
    if (i > 0 && (len - i) % 3 == 0) out[o++] = '.';
    out[o++] = n[i];
  }
  out[o] = 0;
}

static void fmt_secs(const char* n, char* out, int max) {
  int s = atoi(n), h = s / 3600, m = (s / 60) % 60;
  s %= 60;
  if (h) snprintf(out, (size_t)max, "%d:%02d:%02d", h, m, s);
  else snprintf(out, (size_t)max, "%d:%02d", m, s);
}

static void watch_page(yb_t* s, const char* body, int len, const char* id) {
  const char *pr, *pre, *di, *de;
  char *own1 = 0, *own2 = 0;
  char title[300], author[120], views[32], secs[16], date[80], chan[80], b[300];
  char* desc = (char*)malloc(8192);
  yt_item_t* items = (yt_item_t*)malloc(sizeof(yt_item_t) * YT_ITEMS);
  int n = 0;
  title[0] = author[0] = views[0] = secs[0] = date[0] = chan[0] = 0;
  if (desc) desc[0] = 0;
  pr = json_of(body, len, "ytInitialPlayerResponse", &pre, &own1);
  if (pr) {
    const char* vd = find(pr, pre, "\"videoDetails\":{");
    if (vd) {
      const char* ve = vd + 65536 < pre ? vd + 65536 : pre;
      char tmp[32];
      get(vd, ve, "\"title\":\"", title, (int)sizeof(title));
      get(vd, ve, "\"author\":\"", author, (int)sizeof(author));
      get(vd, ve, "\"channelId\":\"", chan, (int)sizeof(chan));
      if (get(vd, ve, "\"viewCount\":\"", tmp, (int)sizeof(tmp))) fmt_views(tmp, views, (int)sizeof(views));
      if (get(vd, ve, "\"lengthSeconds\":\"", tmp, (int)sizeof(tmp))) fmt_secs(tmp, secs, (int)sizeof(secs));
      if (desc) get(vd, ve, "\"shortDescription\":\"", desc, 8192);
    }
  }
  di = json_of(body, len, "ytInitialData", &de, &own2);
  if (di) {
    text_of(di, de, "\"dateText\":{", 200, date, (int)sizeof(date));
    if (!title[0]) text_of(di, de, "\"videoPrimaryInfoRenderer\":{\"title\":{", 400, title, (int)sizeof(title));
    if (items) n = read_items(di, de, items, YT_ITEMS, id);
  }
  if (!title[0]) get(body, body + len, "<meta name=\"title\" content=\"", title, (int)sizeof(title));
  put_head(s, title[0] ? title : "YouTube", "");
  put(s, "<table><tr><td valign=\"top\">");
  snprintf(b, sizeof b, "<img src=\"https://i.ytimg.com/vi/%s/mqdefault.jpg\" width=\"%d\" height=\"%d\">",
           id, BIG_W, BIG_H);
  put(s, b);
  put(s, "</td><td valign=\"top\"><h2>");
  put_esc(s, title[0] ? title : id);
  put(s, "</h2>");
  if (author[0]) {
    put(s, "<b>");
    if (chan[0]) {
      put(s, "<a href=\"" YT "/channel/");
      put_esc(s, chan);
      put(s, "\">");
      put_esc(s, author);
      put(s, "</a>");
    } else {
      put_esc(s, author);
    }
    put(s, "</b><br>");
  }
  put(s, "<span class=\"meta\">");
  if (views[0]) {
    put(s, views);
    put(s, " visualizacoes");
  }
  if (secs[0]) {
    put(s, views[0] ? " - " : "");
    put(s, secs);
  }
  if (date[0]) {
    put(s, " - ");
    put_esc(s, date);
  }
  put(s, "</span><p><i>O video nao toca neste navegador: o endereco do fluxo so sai com o "
         "JavaScript do YouTube. A miniatura, a descricao e os relacionados vem da propria "
         "pagina.</i></p></td></tr></table>\n");
  if (desc && desc[0]) {
    put(s, "<h3>Descricao</h3><p>");
    put_esc_br(s, desc, 1);
    put(s, "</p>\n");
  }
  if (n > 0) {
    put(s, "<h3>Relacionados</h3>\n");
    put_items(s, items, n);
  } else if (!title[0]) {
    put(s, "<p>Nao achei os dados do video na pagina do YouTube (o formato pode ter mudado, "
           "ou o YouTube pediu confirmacao).</p>\n");
  }
  free(items);
  free(desc);
  free(own1);
  free(own2);
}

static void list_page(yb_t* s, int kind, const char* body, int len, const char* q, const char* url) {
  const char *di, *de;
  char* own = 0;
  char title[200];
  yt_item_t* items = (yt_item_t*)malloc(sizeof(yt_item_t) * YT_ITEMS);
  int n = 0;
  title[0] = 0;
  if (kind == YT_SEARCH) snprintf(title, sizeof title, "%s - YouTube", q);
  else if (!get(body, body + len, "<meta property=\"og:title\" content=\"", title, (int)sizeof(title)))
    snprintf(title, sizeof title, "YouTube");
  di = json_of(body, len, "ytInitialData", &de, &own);
  if (di && items) n = read_items(di, de, items, YT_ITEMS, 0);
  put_head(s, title, kind == YT_SEARCH ? q : "");
  if (kind != YT_SEARCH) {
    put(s, "<h2>");
    put_esc(s, title);
    put(s, "</h2>\n");
  }
  if (n > 0) {
    put_items(s, items, n);
  } else {
    put(s, "<p>Nenhum video nesta pagina do YouTube. ");
    if (!di) put(s, "Ela veio sem os dados (ytInitialData): o formato pode ter mudado. ");
    put(s, "Tente a busca acima.</p>\n");
  }
  (void)url;
  free(items);
  free(own);
}

/* ---------- interface ---------- */
static int yt_host(const char* h) {
  return strcmp(h, "www.youtube.com") == 0 || strcmp(h, "youtube.com") == 0 ||
         strcmp(h, "m.youtube.com") == 0;
}

int yt_rewrite(const char* url, char* out, int max) {
  url_t u;
  char id[16];
  int k = 0;
  const char* p;
  if (url_parse(url, &u) != 0 || (strcmp(u.scheme, "https") != 0 && strcmp(u.scheme, "http") != 0))
    return 0;
  p = u.path;
  if (strcmp(u.host, "youtu.be") == 0) {
    p++;
  } else if (yt_host(u.host)) {
    if (starts(p, "/shorts/")) p += 8;
    else if (starts(p, "/embed/")) p += 7;
    else if (starts(p, "/live/")) p += 6;
    else if (strcmp(u.host, "www.youtube.com") != 0 || strcmp(u.scheme, "https") != 0) {
      snprintf(out, (size_t)max, YT "%s", u.path);
      return 1;
    } else {
      return 0;
    }
  } else {
    return 0;
  }
  while (*p && *p != '?' && *p != '/' && *p != '#' && k < 15) id[k++] = *p++;
  id[k] = 0;
  if (!valid_id(id)) return 0;
  snprintf(out, (size_t)max, YT "/watch?v=%s", id);
  return 1;
}

int yt_kind(const char* url, char* q, int qmax) {
  url_t u;
  const char* p;
  q[0] = 0;
  if (url_parse(url, &u) != 0 || (strcmp(u.scheme, "https") != 0 && strcmp(u.scheme, "http") != 0))
    return 0;
  if (!yt_host(u.host)) return 0;
  p = u.path;
  if (strcmp(p, "/") == 0 || p[0] == 0 || starts(p, "/?") || starts(p, "/feed/")) return YT_HOME;
  if (starts(p, "/results?")) {
    if (!search_param(p + 9, "search_query", q, qmax)) search_param(p + 9, "q", q, qmax);
    return q[0] ? YT_SEARCH : YT_HOME;
  }
  if (starts(p, "/watch?")) {
    search_param(p + 7, "v", q, qmax);
    return valid_id(q) ? YT_WATCH : YT_HOME;
  }
  if (starts(p, "/@") || starts(p, "/channel/") || starts(p, "/c/") || starts(p, "/user/") ||
      starts(p, "/playlist?"))
    return YT_LIST;
  return 0;
}

char* yt_html(int kind, const char* body, int len, const char* q, const char* url, int* outlen) {
  yb_t s;
  *outlen = 0;
  s.cap = 32768;
  s.n = 0;
  s.b = (char*)malloc((size_t)s.cap);
  if (!s.b) return 0;
  s.b[0] = 0;
  if (kind == YT_HOME || !body || len <= 0) {
    put_head(&s, "YouTube", "");
    put(&s, "<p>Busque um video acima. Sem login, o YouTube nao manda sugestoes para a "
            "pagina inicial.</p>\n<p>Os resultados mostram miniatura, canal, duracao e "
            "visualizacoes; a pagina do video traz a descricao e os relacionados. O video "
            "em si nao toca neste navegador.</p>\n");
  } else if (kind == YT_WATCH) {
    watch_page(&s, body, len, q);
  } else {
    list_page(&s, kind, body, len, q, url);
  }
  put(&s, "</body></html>\n");
  if (!s.b) return 0;
  *outlen = s.n;
  return s.b;
}
