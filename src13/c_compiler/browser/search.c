/* search.c - busca pela barra de endereco: DuckDuckGo, Google, Bing e imagens.
 * Palavras soltas vao para o DuckDuckGo (html.duckduckgo.com, sem JavaScript).
 * O Google so responde a navegadores com JavaScript, e o DuckDuckGo as vezes
 * pede um desafio anti-robo (captcha); main.c passa a busca recusada para o
 * proximo buscador com um aviso. As imagens vem da pagina de imagens do Bing:
 * cada resultado traz um atributo m= com JSON (murl = original, turl =
 * miniatura, purl = pagina, t = titulo), que vira uma grade. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "browser.h"

#define IMG_RESULTS 24 /* cabe em DOC_IMGS */
#define THUMB_W 160    /* 20 colunas da grade */
#define THUMB_H 128    /* 8 linhas */
#define THUMB_GAP 1

static const struct {
  const char* pre;
  int kind;
} prefixes[] = {
  { "g ", SEARCH_GOOGLE }, { "google ", SEARCH_GOOGLE },
  { "d ", SEARCH_DDG }, { "ddg ", SEARCH_DDG }, { "b ", SEARCH_BING }, { "bing ", SEARCH_BING },
  { "i ", SEARCH_IMAGES }, { "img ", SEARCH_IMAGES }, { "imagem ", SEARCH_IMAGES },
  { "imagens ", SEARCH_IMAGES }, { 0, 0 }
};

/* resultados da ultima busca de imagens: original, copia do Bing e pagina */
static struct {
  char orig[URL_MAX];
  char copy[URL_MAX];
  char page[URL_MAX];
} res[IMG_RESULTS];
static int nres;

static int starts_ci(const char* s, const char* p) {
  while (*p) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) return 0;
    s++;
    p++;
  }
  return 1;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = tolower(c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static int hexdig(int v) { return v < 10 ? '0' + v : 'A' + (v - 10); }

int search_enc(char* dst, int n, int max, const char* s) {
  if (!s) s = "";
  while (*s) {
    unsigned char c = (unsigned char)*s++;
    if (c == ' ') {
      if (n + 1 >= max) return -1;
      dst[n++] = '+';
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '*') {
      if (n + 1 >= max) return -1;
      dst[n++] = (char)c;
    } else {
      if (n + 3 >= max) return -1;
      dst[n++] = '%';
      dst[n++] = (char)hexdig(c >> 4);
      dst[n++] = (char)hexdig(c & 15);
    }
  }
  dst[n] = 0;
  return n;
}

/* valor de name= na query (sem o '?'), com %XX e '+' decodificados */
static int param(const char* query, const char* name, char* out, int max) {
  int nl = (int)strlen(name);
  const char* p = query;
  while (p && *p) {
    if (strncmp(p, name, (size_t)nl) == 0 && p[nl] == '=') {
      int o = 0;
      p += nl + 1;
      while (*p && *p != '&' && *p != '#' && o < max - 1) {
        int a, b;
        if (*p == '+') {
          out[o++] = ' ';
          p++;
        } else if (*p == '%' && (a = hexval((unsigned char)p[1])) >= 0 &&
                   (b = hexval((unsigned char)p[2])) >= 0) {
          out[o++] = (char)(a * 16 + b);
          p += 3;
        } else {
          out[o++] = *p++;
        }
      }
      out[o] = 0;
      return 1;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  out[0] = 0;
  return 0;
}

/* URL vinda de JSON ou de um parametro: escapa espaco, aspas e bytes altos */
static void url_clean(const char* in, char* out, int max) {
  int o = 0;
  for (; *in && o < max - 4; in++) {
    unsigned char c = (unsigned char)*in;
    if (c <= ' ' || c >= 0x7F || c == '"' || c == '<' || c == '>') {
      out[o++] = '%';
      out[o++] = (char)hexdig(c >> 4);
      out[o++] = (char)hexdig(c & 15);
    } else {
      out[o++] = (char)c;
    }
  }
  out[o] = 0;
}

/* palavras soltas viram busca; endereco, unidade (D:) e about: nao */
static int looks_like_query(const char* s) {
  int n = (int)strlen(s);
  while (n > 0 && s[n - 1] == ' ') n--;
  if (n == 0) return 0;
  if (memchr(s, ' ', (size_t)n)) return 1;
  if (memchr(s, '.', (size_t)n) || memchr(s, ':', (size_t)n) || memchr(s, '/', (size_t)n) ||
      memchr(s, '\\', (size_t)n))
    return 0;
  return !starts_ci(s, "localhost");
}

static int build(int kind, const char* q, char* out, int max) {
  const char* base = "https://html.duckduckgo.com/html/?q=";
  const char* tail = "";
  int n;
  if (kind == SEARCH_GOOGLE) {
    base = "https://www.google.com/search?q=";
    tail = "&hl=pt-BR";
  } else if (kind == SEARCH_IMAGES) {
    base = "https://www.bing.com/images/search?q=";
  } else if (kind == SEARCH_BING) {
    base = "https://www.bing.com/search?q=";
  }
  n = (int)strlen(base);
  if (n + (int)strlen(tail) >= max) return 0;
  strcpy(out, base);
  n = search_enc(out, n, max - (int)strlen(tail), q);
  if (n < 0) return 0;
  strcpy(out + n, tail);
  return 1;
}

int search_url(const char* input, char* out, int max) {
  char q[256];
  int kind = SEARCH_DDG, i, n;
  const char* s = input;
  while (*s == ' ') s++;
  if (starts_ci(s, "about:busca")) {
    const char* p = strchr(s, '?');
    char em[32];
    if (!p || !param(p + 1, "q", q, (int)sizeof(q))) return 0;
    if (param(p + 1, "em", em, (int)sizeof(em))) {
      if (starts_ci(em, "google")) kind = SEARCH_GOOGLE;
      else if (starts_ci(em, "imag")) kind = SEARCH_IMAGES;
      else if (starts_ci(em, "bing")) kind = SEARCH_BING;
    }
  } else {
    for (i = 0; prefixes[i].pre; i++)
      if (starts_ci(s, prefixes[i].pre)) break;
    if (prefixes[i].pre) {
      kind = prefixes[i].kind;
      s += strlen(prefixes[i].pre);
      while (*s == ' ') s++;
    } else if (!looks_like_query(s)) {
      return 0;
    }
    strncpy(q, s, sizeof(q) - 1);
    q[sizeof(q) - 1] = 0;
  }
  n = (int)strlen(q);
  while (n > 0 && q[n - 1] == ' ') q[--n] = 0;
  s = q;
  while (*s == ' ') s++;
  if (!*s) return 0;
  return build(kind, s, out, max);
}

static int b64url_val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-' || c == '+') return 62;
  if (c == '_' || c == '/') return 63;
  return -1;
}

/* base64url sem '=' -> texto; 0 se nao era base64 */
static int b64url_decode(const char* s, char* out, int max) {
  int o = 0, val = 0, bits = 0, d;
  for (; *s && *s != '='; s++) {
    d = b64url_val((unsigned char)*s);
    if (d < 0) return 0;
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= max - 1) return 0;
      out[o++] = (char)((val >> bits) & 0xFF);
    }
  }
  out[o] = 0;
  return o > 0;
}

int search_unwrap(const char* url, char* out, int max) {
  url_t u;
  char dest[URL_MAX];
  const char* q;
  if (url_parse(url, &u) != 0 || (strcmp(u.scheme, "https") != 0 && strcmp(u.scheme, "http") != 0))
    return 0;
  q = strchr(u.path, '?');
  if (!q) return 0;
  q++;
  if (strstr(u.host, "duckduckgo.com") && strncmp(u.path, "/l/", 3) == 0) {
    if (!param(q, "uddg", dest, (int)sizeof(dest))) return 0;
  } else if (strstr(u.host, "google.") && strncmp(u.path, "/url?", 5) == 0) {
    if (!param(q, "q", dest, (int)sizeof(dest)) && !param(q, "url", dest, (int)sizeof(dest))) return 0;
  } else if (strcmp(u.host, "www.bing.com") == 0 && strncmp(u.path, "/ck/a?", 6) == 0) {
    char enc[URL_MAX];
    if (!param(q, "u", enc, (int)sizeof(enc)) || strncmp(enc, "a1", 2) != 0 ||
        !b64url_decode(enc + 2, dest, (int)sizeof(dest)))
      return 0;
  } else {
    return 0;
  }
  if (!starts_ci(dest, "http://") && !starts_ci(dest, "https://")) return 0;
  url_clean(dest, out, max);
  return 1;
}

int search_kind(const char* url, char* q, int qmax) {
  url_t u;
  const char* p;
  int kind = 0;
  q[0] = 0;
  if (url_parse(url, &u) != 0) return 0;
  if ((strcmp(u.host, "html.duckduckgo.com") == 0 && strncmp(u.path, "/html", 5) == 0) ||
      (strcmp(u.host, "lite.duckduckgo.com") == 0 && strncmp(u.path, "/lite", 5) == 0) ||
      (strcmp(u.host, "duckduckgo.com") == 0 && strncmp(u.path, "/?", 2) == 0))
    kind = SEARCH_DDG;
  else if ((strcmp(u.host, "www.google.com") == 0 || strcmp(u.host, "google.com") == 0) &&
           strncmp(u.path, "/search", 7) == 0)
    kind = SEARCH_GOOGLE;
  else if (strcmp(u.host, "www.bing.com") == 0 && strncmp(u.path, "/images/search", 14) == 0)
    kind = SEARCH_IMAGES;
  else if (strcmp(u.host, "www.bing.com") == 0 && strncmp(u.path, "/search", 7) == 0)
    kind = SEARCH_BING;
  p = strchr(u.path, '?');
  if (!kind || !p || !param(p + 1, "q", q, qmax) || !q[0]) return 0;
  return kind;
}

static int has(const char* b, int n, const char* needle) {
  int k = (int)strlen(needle), i;
  for (i = 0; i + k <= n; i++)
    if (b[i] == needle[0] && memcmp(b + i, needle, (size_t)k) == 0) return 1;
  return 0;
}

int search_refused(int kind, int status, const char* body, int len) {
  if (kind == SEARCH_GOOGLE)
    return !body || len <= 0 || has(body, len, "enablejs") || has(body, len, "mais aceito") ||
           has(body, len, "no longer supported") || has(body, len, "/sorry/") ||
           has(body, len, "consent.google");
  if (kind == SEARCH_DDG)
    return status == 202 || (body && has(body, len, "anomaly-modal"));
  return 0;
}

/* ---------- grade de imagens ---------- */
typedef struct {
  char* b;
  int n, cap;
} sbuf_t;

static void put_n(sbuf_t* s, const char* t, int k) {
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

static void put(sbuf_t* s, const char* t) { put_n(s, t, (int)strlen(t)); }

/* texto ou atributo HTML */
static void put_esc(sbuf_t* s, const char* t) {
  for (; *t; t++) {
    if (*t == '&') put(s, "&amp;");
    else if (*t == '"') put(s, "&quot;");
    else if (*t == '<') put(s, "&lt;");
    else if (*t == '>') put(s, "&gt;");
    else put_n(s, t, 1);
  }
}

/* &quot; &amp; &lt; &gt; &#39; do atributo m= */
static int unescape_attr(const char* s, int n, char* out, int max) {
  int i = 0, o = 0;
  while (i < n && o < max - 1) {
    if (s[i] == '&') {
      static const struct { const char* e; char c; } ents[] = {
        { "&quot;", '"' }, { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
        { "&#39;", '\'' }, { "&#x27;", '\'' }, { 0, 0 }
      };
      int k;
      for (k = 0; ents[k].e; k++) {
        int el = (int)strlen(ents[k].e);
        if (i + el <= n && strncmp(s + i, ents[k].e, (size_t)el) == 0) {
          out[o++] = ents[k].c;
          i += el;
          break;
        }
      }
      if (ents[k].e) continue;
    }
    out[o++] = s[i++];
  }
  out[o] = 0;
  return o;
}

static int put_utf8(char* out, int o, int max, unsigned cp) {
  if (cp < 0x80) {
    if (o + 1 < max) out[o++] = (char)cp;
  } else if (cp < 0x800) {
    if (o + 2 < max) {
      out[o++] = (char)(0xC0 | (cp >> 6));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    }
  } else if (o + 3 < max) {
    out[o++] = (char)(0xE0 | (cp >> 12));
    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[o++] = (char)(0x80 | (cp & 0x3F));
  }
  return o;
}

/* "key":"valor" de um objeto JSON simples; \uXXXX vira UTF-8 */
static int json_str(const char* obj, const char* key, char* out, int max) {
  char pat[32];
  const char* p;
  int o = 0;
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  p = strstr(obj, pat);
  out[0] = 0;
  if (!p) return 0;
  p += strlen(pat);
  while (*p && *p != '"' && o < max - 4) {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'u') {
        unsigned cp = 0;
        int k;
        for (k = 1; k <= 4 && hexval((unsigned char)p[k]) >= 0; k++) cp = cp * 16 + (unsigned)hexval((unsigned char)p[k]);
        o = put_utf8(out, o, max, cp);
        p += k;
        continue;
      }
      if (*p == 'n' || *p == 't' || *p == 'r') out[o++] = ' ';
      else out[o++] = *p;
      p++;
      continue;
    }
    out[o++] = *p++;
  }
  out[o] = 0;
  return 1;
}

/* "https://www.site.com.br/x" -> "site.com.br", cortado em max-1 */
static void domain_of(const char* url, char* out, int max) {
  url_t u;
  const char* h = "pagina";
  if (url_parse(url, &u) == 0 && u.host[0]) {
    h = u.host;
    if (strncmp(h, "www.", 4) == 0) h += 4;
  }
  strncpy(out, h, (size_t)max - 1);
  out[max - 1] = 0;
}

/* As miniaturas vem de ts1..ts4.mm.bing.net. Num host so (th.bing.com) a
 * grade inteira passa por uma conexao: http.c guarda a conexao entre pedidos. */
static void thumb_host(char* url, int max) {
  url_t u;
  if (url_parse(url, &u) != 0 || !strstr(u.host, ".mm.bing.net")) return;
  snprintf(url, (size_t)max, "https://th.bing.com%s", u.path);
}

/* linha de nomes de site, cada um embaixo da sua miniatura */
static void put_names(sbuf_t* s, int first, int n, int tc) {
  int k;
  put(s, "<br>");
  for (k = first; k < first + n; k++) {
    char dom[32];
    int pad;
    domain_of(res[k].page, dom, tc < (int)sizeof(dom) ? tc : (int)sizeof(dom));
    put(s, "<a href=\"");
    put_esc(s, res[k].page);
    put(s, "\">");
    put_esc(s, dom);
    put(s, "</a>");
    if (k + 1 < first + n)
      for (pad = (int)strlen(dom); pad < tc + THUMB_GAP; pad++) put(s, "&nbsp;");
  }
  put(s, "<br><br>\n");
}

/* Le o proximo atributo m= de <a class="iusc"> a partir de *pos; 0 no fim */
static int next_result(const char* body, int len, int* pos, char* m, int mmax) {
  int j;
  for (j = *pos; j + 12 <= len; j++) {
    const char* tag_end;
    const char* mv = 0;
    const char* me;
    int k;
    if (body[j] != 'c' || memcmp(body + j, "class=\"iusc\"", 12) != 0) continue;
    *pos = j + 12;
    tag_end = (const char*)memchr(body + j, '>', (size_t)(len - j));
    if (!tag_end) return 0;
    for (k = j; k + 4 <= (int)(tag_end - body); k++)
      if (memcmp(body + k, " m=\"", 4) == 0) {
        mv = body + k + 4;
        break;
      }
    if (!mv) continue;
    me = (const char*)memchr(mv, '"', (size_t)(tag_end - mv));
    if (!me) continue;
    unescape_attr(mv, (int)(me - mv), m, mmax);
    return 1;
  }
  *pos = len;
  return 0;
}

char* search_images_html(const char* body, int len, const char* q, int cols, int* outlen) {
  sbuf_t s;
  char* m = (char*)malloc(8192);
  char tmp[URL_MAX], title[256], sz[96];
  int tc = (THUMB_W + 7) / 8;
  int per_row = (cols + THUMB_GAP) / (tc + THUMB_GAP);
  int pos = 0, row0 = 0;
  *outlen = 0;
  nres = 0;
  s.cap = 16384;
  s.n = 0;
  s.b = (char*)malloc((size_t)s.cap);
  if (!s.b || !m) {
    free(s.b);
    free(m);
    return 0;
  }
  s.b[0] = 0;
  if (per_row < 1) per_row = 1;
  if (search_enc(tmp, 0, (int)sizeof(tmp), q) < 0) tmp[0] = 0;
  put(&s, "<html><head><title>");
  put_esc(&s, q);
  put(&s, " - Imagens</title></head><body><p><b>Imagens de \"");
  put_esc(&s, q);
  put(&s, "\"</b> (Bing). A miniatura abre a imagem; o nome embaixo abre a pagina "
          "de onde ela veio. <a href=\"about:busca?q=");
  put(&s, tmp);
  put(&s, "\">Buscar o texto no DuckDuckGo</a></p>\n");
  while (nres < IMG_RESULTS && next_result(body, len, &pos, m, 8192)) {
    url_t u;
    if (!json_str(m, "turl", tmp, (int)sizeof(tmp)) || !tmp[0]) continue;
    url_clean(tmp, res[nres].copy, URL_MAX - 32);
    thumb_host(res[nres].copy, URL_MAX - 32);
    json_str(m, "murl", tmp, (int)sizeof(tmp));
    url_clean(tmp, res[nres].orig, URL_MAX);
    if (url_parse(res[nres].orig, &u) == 0) url_format(&u, res[nres].orig, URL_MAX);
    else strcpy(res[nres].orig, res[nres].copy);
    json_str(m, "purl", tmp, (int)sizeof(tmp));
    url_clean(tmp, res[nres].page, URL_MAX);
    if (!res[nres].page[0]) strcpy(res[nres].page, res[nres].orig);
    json_str(m, "t", title, (int)sizeof(title));
    put(&s, "<a href=\"");
    put_esc(&s, res[nres].orig);
    put(&s, "\"><img src=\"");
    put_esc(&s, res[nres].copy);
    snprintf(sz, sizeof(sz), "&amp;w=%d&amp;h=%d&amp;c=7\" width=\"%d\" height=\"%d\" alt=\"",
             THUMB_W, THUMB_H, THUMB_W, THUMB_H);
    put(&s, sz);
    put_esc(&s, title);
    put(&s, "\"></a>\n");
    nres++;
    if (nres - row0 == per_row) {
      put_names(&s, row0, per_row, tc);
      row0 = nres;
    }
  }
  if (nres > row0) put_names(&s, row0, nres - row0, tc);
  if (nres == 0)
    put(&s, "<p>Nenhuma imagem: a pagina do Bing veio sem resultados ou mudou de formato.</p>\n");
  put(&s, "</body></html>\n");
  free(m);
  if (!s.b) return 0;
  *outlen = s.n;
  return s.b;
}

int search_image_copy(const char* url, char* out, int max) {
  int i;
  for (i = 0; i < nres; i++) {
    if (strcmp(res[i].orig, url) == 0 && strcmp(res[i].copy, url) != 0) {
      strncpy(out, res[i].copy, (size_t)max - 1);
      out[max - 1] = 0;
      return 1;
    }
  }
  return 0;
}
