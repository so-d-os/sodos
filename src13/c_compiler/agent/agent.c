/* agent.c - AGENT.EXE: agente de IA nativo do SODOS.
 *   cc -o agent.exe agent.c tls.dll
 *   AGENT [-y] [-m modelo] [tarefa...]
 * Conversa com uma API no formato OpenAI (DeepSeek, OpenAI, Ollama,
 * llama.cpp...) por HTTPS ou HTTP. O modelo responde com um bloco de acao
 * (```cmd ou ```write CAMINHO); o agente roda o comando no prompt, captura a
 * saida (k_shell_capture) e devolve ao modelo, ate ele responder sem acao.
 *
 * AGENT.CFG (pasta atual, D:\, U:\ ou C:\), uma chave=valor por linha:
 *   key=sk-...         (deepseek=sk-... e o mesmo, ja com a url do DeepSeek)
 *   url=https://api.deepseek.com/chat/completions
 *   model=deepseek-chat
 *
 * A resposta vem em streaming (SSE): o kernel desiste de um recv parado ha
 * 8 s, e o stream manda bytes enquanto o modelo escreve.
 * O texto vai e volta da API em UTF-8; na tela e nos arquivos vira CP850.
 * So usa k_malloc e k_write: o comando rodado pode ser um .EXE com a mesma
 * LIBC.DLL, e o heap e a stdio dela nao podem estar no meio de nada. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include <tls.h>

#define LINE_MAX 240        /* linha do prompt (k_shell copia 256) */
#define OUT_MAX (64 * 1024) /* saida capturada de um comando */
#define OBS_MAX 6000        /* bytes da saida que voltam ao modelo */
#define BODY_MAX (512 * 1024)
#define HDR_MAX 8192
#define STEPS_MAX 25        /* acoes por pedido do usuario */

/* ---------- texto que cresce ---------- */
typedef struct {
  char* p;
  int len, cap;
} str_t;

static int s_grow(str_t* s, int need) {
  int cap;
  char* np;
  if (s->len + need + 1 <= s->cap) return 0;
  cap = s->cap ? s->cap : 256;
  while (cap < s->len + need + 1) cap *= 2;
  np = (char*)k_realloc(s->p, (unsigned)cap);
  if (!np) return -1;
  s->p = np;
  s->cap = cap;
  return 0;
}

static void s_addn(str_t* s, const char* d, int n) {
  if (n <= 0 || s_grow(s, n) < 0) return;
  memcpy(s->p + s->len, d, (size_t)n);
  s->len += n;
  s->p[s->len] = 0;
}

static void s_add(str_t* s, const char* d) { s_addn(s, d, (int)strlen(d)); }

static void s_addc(str_t* s, int c) {
  char ch = (char)c;
  s_addn(s, &ch, 1);
}

static void s_clear(str_t* s) {
  s->len = 0;
  if (s->p) s->p[0] = 0;
}

static void s_free(str_t* s) {
  if (s->p) k_free(s->p);
  s->p = 0;
  s->len = s->cap = 0;
}

static char* s_dup(const char* d, int n) {
  char* p = (char*)k_malloc((unsigned)n + 1);
  if (!p) return 0;
  memcpy(p, d, (size_t)n);
  p[n] = 0;
  return p;
}

/* ---------- console ---------- */
static void out(const char* s) { k_write(s, (int)strlen(s)); }

/* ---------- CP850 <-> UTF-8 ---------- */
/* U+00A0..U+00FF -> CP850 (igual ao mc/view.c) */
static const unsigned char latin1_cp850[96] = {
  0xFF, 0xAD, 0xBD, 0x9C, 0xCF, 0xBE, 0xDD, 0xF5, 0xF9, 0xB8, 0xA6, 0xAE,
  0xAA, 0xF0, 0xA9, 0xEE, 0xF8, 0xF1, 0xFD, 0xFC, 0xEF, 0xE6, 0xF4, 0xFA,
  0xF7, 0xFB, 0xA7, 0xAF, 0xAC, 0xAB, 0xF3, 0xA8, 0xB7, 0xB5, 0xB6, 0xC7,
  0x8E, 0x8F, 0x92, 0x80, 0xD4, 0x90, 0xD2, 0xD3, 0xDE, 0xD6, 0xD7, 0xD8,
  0xD1, 0xA5, 0xE3, 0xE0, 0xE2, 0xE5, 0x99, 0x9E, 0x9D, 0xEB, 0xE9, 0xEA,
  0x9A, 0xED, 0xE8, 0xE1, 0x85, 0xA0, 0x83, 0xC6, 0x84, 0x86, 0x91, 0x87,
  0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B, 0xD0, 0xA4, 0x95, 0xA2,
  0x93, 0xE4, 0x94, 0xF6, 0x9B, 0x97, 0xA3, 0x96, 0x81, 0xEC, 0xE7, 0x98,
};

/* Aspas, tracos, setas e molduras; o resto vira '?'. */
static const unsigned short uni_from[] = {
  0x2018, 0x2019, 0x201C, 0x201D, 0x2013, 0x2014, 0x2022, 0x2026, 0x20AC,
  0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C,
  0x2534, 0x253C, 0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D, 0x2588,
  0x2591, 0x2592, 0x2593, 0x2192, 0x2190, 0x2191, 0x2193,
};
static const unsigned char uni_to[] = {
  0x27, 0x27, 0x22, 0x22, 0x2D, 0x2D, 0x07, 0x2E, 0x45,
  0xC4, 0xB3, 0xDA, 0xBF, 0xC0, 0xD9, 0xC3, 0xB4, 0xC2,
  0xC1, 0xC5, 0xCD, 0xBA, 0xC9, 0xBB, 0xC8, 0xBC, 0xDB,
  0xB0, 0xB1, 0xB2, 0x1A, 0x1B, 0x18, 0x19,
};

static void utf8_put(str_t* s, unsigned u) {
  if (u < 0x80) {
    s_addc(s, (int)u);
  } else if (u < 0x800) {
    s_addc(s, (int)(0xC0 | (u >> 6)));
    s_addc(s, (int)(0x80 | (u & 63)));
  } else if (u < 0x10000) {
    s_addc(s, (int)(0xE0 | (u >> 12)));
    s_addc(s, (int)(0x80 | ((u >> 6) & 63)));
    s_addc(s, (int)(0x80 | (u & 63)));
  } else {
    s_addc(s, (int)(0xF0 | (u >> 18)));
    s_addc(s, (int)(0x80 | ((u >> 12) & 63)));
    s_addc(s, (int)(0x80 | ((u >> 6) & 63)));
    s_addc(s, (int)(0x80 | (u & 63)));
  }
}

/* Um byte CP850 em Unicode; 0 se nao tem par (vira '?'). */
static unsigned cp850_uni(int c) {
  unsigned i;
  for (i = 0; i < 96; i++)
    if (latin1_cp850[i] == c) return 0xA0 + i;
  for (i = 0; i < sizeof(uni_to); i++)
    if (uni_to[i] == c && c >= 0x80) return uni_from[i];
  return 0;
}

/* Texto do teclado ou da saida de um comando (CP850) para a API (UTF-8).
 * Controles somem, menos \n e \t. */
static void cp850_to_utf8(str_t* dst, const char* src, int n) {
  int i;
  for (i = 0; i < n; i++) {
    int c = (unsigned char)src[i];
    if (c < 0x80) {
      if (c >= 32 || c == '\n' || c == '\t') s_addc(dst, c);
    } else {
      unsigned u = cp850_uni(c);
      utf8_put(dst, u ? u : '?');
    }
  }
}

/* UTF-8 da API para a tela e para os arquivos do SO. */
static void utf8_to_cp850(str_t* dst, const char* src, int n) {
  int i = 0;
  while (i < n) {
    int c = (unsigned char)src[i];
    unsigned u;
    unsigned k;
    if (c < 0x80) {
      s_addc(dst, c);
      i++;
      continue;
    }
    if ((c & 0xE0) == 0xC0 && i + 1 < n) {
      u = ((unsigned)(c & 0x1F) << 6) | (src[i + 1] & 0x3Fu);
      i += 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
      u = ((unsigned)(c & 0x0F) << 12) | ((src[i + 1] & 0x3Fu) << 6) | (src[i + 2] & 0x3Fu);
      i += 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
      s_addc(dst, '?');
      i += 4;
      continue;
    } else {
      s_addc(dst, '?');
      i++;
      continue;
    }
    if (u >= 0xA0 && u <= 0xFF) {
      s_addc(dst, latin1_cp850[u - 0xA0]);
      continue;
    }
    if (u == 0xFEFF) continue;
    for (k = 0; k < sizeof(uni_to); k++)
      if (uni_from[k] == u) break;
    s_addc(dst, k < sizeof(uni_to) ? uni_to[k] : '?');
  }
}

/* Mostra texto UTF-8 na tela. */
static void show_utf8(const char* s, int n) {
  str_t t = {0, 0, 0};
  utf8_to_cp850(&t, s, n);
  if (t.len) k_write(t.p, t.len);
  s_free(&t);
}

/* ---------- JSON ---------- */
static void json_str(str_t* s, const char* v) {
  s_addc(s, '"');
  for (; *v; v++) {
    int c = (unsigned char)*v;
    if (c == '"') s_add(s, "\\\"");
    else if (c == '\\') s_add(s, "\\\\");
    else if (c == '\n') s_add(s, "\\n");
    else if (c == '\r') s_add(s, "\\r");
    else if (c == '\t') s_add(s, "\\t");
    else if (c < 32) {
      char u[8];
      snprintf(u, sizeof(u), "\\u%04x", c);
      s_add(s, u);
    } else s_addc(s, c);
  }
  s_addc(s, '"');
}

static int hexv(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static unsigned hex4(const char* p) {
  unsigned u = 0;
  int i;
  for (i = 0; i < 4; i++) {
    int v = hexv(p[i]);
    if (v < 0) return 0xFFFD;
    u = (u << 4) | (unsigned)v;
  }
  return u;
}

/* p aponta para a aspa de abertura. Decodifica em out (UTF-8).
 * Devolve o ponteiro depois da aspa final, ou 0. */
static const char* json_read_str(const char* p, const char* end, str_t* o) {
  if (p >= end || *p != '"') return 0;
  p++;
  while (p < end && *p != '"') {
    if (*p != '\\') {
      s_addc(o, *p++);
      continue;
    }
    if (++p >= end) return 0;
    switch (*p) {
    case 'n': s_addc(o, '\n'); break;
    case 't': s_addc(o, '\t'); break;
    case 'r': s_addc(o, '\r'); break;
    case 'b': s_addc(o, 8); break;
    case 'f': s_addc(o, 12); break;
    case 'u': {
      unsigned u;
      if (end - p < 5) return 0;
      u = hex4(p + 1);
      p += 4;
      if (u >= 0xD800 && u < 0xDC00 && end - p >= 7 && p[1] == '\\' && p[2] == 'u') {
        unsigned lo = hex4(p + 3);
        if (lo >= 0xDC00 && lo < 0xE000) {
          u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
          p += 6;
        }
      }
      utf8_put(o, u);
      break;
    }
    default: s_addc(o, *p); break; /* \" \\ \/ */
    }
    p++;
  }
  return p < end ? p + 1 : 0;
}

/* Acha "chave": e le o valor string. 1 = achou string, 0 = nao achou ou
 * nao e string (null). A busca comeca em from. */
static int json_get(const char* from, const char* end, const char* key, str_t* o) {
  int kl = (int)strlen(key);
  const char* p = from;
  while (p + kl + 2 < end) {
    if (*p == '"' && memcmp(p + 1, key, (size_t)kl) == 0 && p[kl + 1] == '"') {
      const char* q = p + kl + 2;
      while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
      if (q < end && *q == ':') {
        q++;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q < end && *q == '"') return json_read_str(q, end, o) != 0;
        return 0;
      }
    }
    p++;
  }
  return 0;
}

/* ---------- configuracao ---------- */
static char cfg_key[256];
static char cfg_url[256] = "https://api.deepseek.com/chat/completions";
static char cfg_model[64] = "deepseek-chat";
static char cfg_path[64];

static void cfg_set(char* line) {
  char* eq = strchr(line, '=');
  char* v;
  int n;
  if (!eq || line[0] == '#' || line[0] == ';') return;
  *eq = 0;
  v = eq + 1;
  while (*v == ' ') v++;
  n = (int)strlen(v);
  while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\r')) v[--n] = 0;
  n = (int)strlen(line);
  while (n > 0 && line[n - 1] == ' ') line[--n] = 0;
  if (!strcmp(line, "key")) snprintf(cfg_key, sizeof(cfg_key), "%s", v);
  else if (!strcmp(line, "deepseek")) {
    /* chave do DeepSeek: vale como key e aponta a url para ele */
    snprintf(cfg_key, sizeof(cfg_key), "%s", v);
    snprintf(cfg_url, sizeof(cfg_url), "%s", "https://api.deepseek.com/chat/completions");
  }
  else if (!strcmp(line, "url")) snprintf(cfg_url, sizeof(cfg_url), "%s", v);
  else if (!strcmp(line, "model")) snprintf(cfg_model, sizeof(cfg_model), "%s", v);
}

static int cfg_load(void) {
  static const char* const places[] = {"AGENT.CFG", "D:\\AGENT.CFG", "U:\\AGENT.CFG",
                                       "C:\\AGENT.CFG"};
  char buf[2048];
  unsigned i;
  for (i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
    int n = k_fload(places[i], buf, (int)sizeof(buf) - 1);
    char* p;
    if (n <= 0) continue;
    buf[n] = 0;
    p = buf;
    while (*p) {
      char* e = p;
      while (*e && *e != '\n') e++;
      if (*e) *e++ = 0;
      cfg_set(p);
      p = e;
    }
    snprintf(cfg_path, sizeof(cfg_path), "%s", places[i]);
    return 0;
  }
  return -1;
}

/* ---------- HTTP(S) ---------- */
typedef struct {
  int tls;
  char host[128];
  int port;
  char path[256];
} url_t;

static int url_parse(const char* s, url_t* u) {
  const char* h;
  int n = 0;
  memset(u, 0, sizeof(*u));
  if (!strncmp(s, "https://", 8)) {
    u->tls = 1;
    u->port = 443;
    h = s + 8;
  } else if (!strncmp(s, "http://", 7)) {
    u->port = 80;
    h = s + 7;
  } else {
    return -1;
  }
  while (*h && *h != '/' && *h != ':' && n < (int)sizeof(u->host) - 1) u->host[n++] = *h++;
  if (*h == ':') {
    u->port = 0;
    h++;
    while (*h >= '0' && *h <= '9') u->port = u->port * 10 + (*h++ - '0');
  }
  snprintf(u->path, sizeof(u->path), "%s", *h ? h : "/");
  return n ? 0 : -1;
}

static int c_tls;
static int c_send(const char* d, int n) { return c_tls ? tls_send(d, n) : k_tcp_send(d, n); }
static int c_recv(char* d, int cap) { return c_tls ? tls_recv(d, cap) : k_tcp_recv(d, cap); }
static void c_close(void) {
  if (c_tls) tls_close();
  else k_tcp_close();
}

/* Estado de uma resposta: cabecalho, chunked, linhas do SSE. */
typedef struct {
  str_t hdr;
  int hdr_done;
  int status;
  int chunked, sse;
  int ck_state;      /* 0 tamanho, 1 dados, 2 CRLF depois dos dados */
  unsigned ck_left;
  str_t ck_line;
  str_t line;        /* linha SSE em montagem */
  str_t body;        /* corpo inteiro (erro ou resposta sem stream) */
  str_t text;        /* resposta do modelo (UTF-8) */
  int done;
  int thinking;      /* ja mostrou "(pensando...)" */
} resp_t;

static int ci_has(const char* hay, const char* needle) {
  int nl = (int)strlen(needle);
  for (; *hay; hay++) {
    int i = 0;
    while (i < nl) {
      int a = hay[i], b = needle[i];
      if (a >= 'A' && a <= 'Z') a += 32;
      if (b >= 'A' && b <= 'Z') b += 32;
      if (a != b) break;
      i++;
    }
    if (i == nl) return 1;
  }
  return 0;
}

static void sse_line(resp_t* r, const char* l, int n) {
  str_t v = {0, 0, 0};
  const char* end = l + n;
  if (n < 5 || memcmp(l, "data:", 5) != 0) return;
  l += 5;
  while (l < end && *l == ' ') l++;
  if (end - l >= 6 && !memcmp(l, "[DONE]", 6)) {
    r->done = 1;
    return;
  }
  if (json_get(l, end, "content", &v) && v.len) {
    if (r->thinking == 1) {
      out("\n");
      r->thinking = 2;
    }
    s_addn(&r->text, v.p, v.len);
    show_utf8(v.p, v.len);
  } else {
    s_clear(&v);
    if (r->thinking == 0 && json_get(l, end, "reasoning_content", &v) && v.len) {
      out("(pensando...)");
      r->thinking = 1;
    }
  }
  s_free(&v);
}

static void body_byte(resp_t* r, int c) {
  if (!r->sse || r->status != 200) {
    if (r->body.len < BODY_MAX) s_addc(&r->body, c);
    return;
  }
  if (c == '\n') {
    int n = r->line.len;
    if (n && r->line.p[n - 1] == '\r') n--;
    if (n) sse_line(r, r->line.p, n);
    s_clear(&r->line);
  } else if (r->line.len < BODY_MAX) {
    s_addc(&r->line, c);
  }
}

static void parse_header(resp_t* r) {
  const char* h = r->hdr.p;
  const char* sp = strchr(h, ' ');
  r->status = sp ? atoi(sp + 1) : 0;
  r->chunked = ci_has(h, "transfer-encoding: chunked");
  r->sse = ci_has(h, "content-type: text/event-stream");
}

static void feed(resp_t* r, const char* d, int n) {
  int i;
  for (i = 0; i < n; i++) {
    int c = (unsigned char)d[i];
    if (!r->hdr_done) {
      s_addc(&r->hdr, c);
      if (r->hdr.len >= 4 && !memcmp(r->hdr.p + r->hdr.len - 4, "\r\n\r\n", 4)) {
        r->hdr_done = 1;
        parse_header(r);
      } else if (r->hdr.len > HDR_MAX) {
        r->hdr_done = 1;
      }
      continue;
    }
    if (!r->chunked) {
      body_byte(r, c);
      continue;
    }
    if (r->ck_state == 0) {
      if (c == '\n') {
        unsigned v = 0;
        int k;
        for (k = 0; k < r->ck_line.len; k++) {
          int h = hexv(r->ck_line.p[k]);
          if (h < 0) break;
          v = v * 16 + (unsigned)h;
        }
        s_clear(&r->ck_line);
        r->ck_left = v;
        r->ck_state = v ? 1 : 3;
      } else {
        s_addc(&r->ck_line, c);
      }
    } else if (r->ck_state == 1) {
      body_byte(r, c);
      if (--r->ck_left == 0) r->ck_state = 2;
    } else if (r->ck_state == 2) {
      if (c == '\n') r->ck_state = 0;
    }
  }
}

static void resp_free(resp_t* r) {
  s_free(&r->hdr);
  s_free(&r->ck_line);
  s_free(&r->line);
  s_free(&r->body);
  s_free(&r->text);
}

/* POST do corpo JSON. Preenche r->text com a resposta do modelo.
 * 0 = ok; -1 = erro (ja mostrado). */
static int api_post(const str_t* json, resp_t* r) {
  url_t u;
  char req[1024];
  char buf[4096];
  int n, off;
  unsigned t0 = k_ms();
  memset(r, 0, sizeof(*r));
  if (url_parse(cfg_url, &u) < 0) {
    out("url invalida em AGENT.CFG\n");
    return -1;
  }
  c_tls = u.tls;
  if (u.tls) {
    if (tls_connect(u.host, u.port) != 0) {
      snprintf(buf, sizeof(buf), "[TLS falhou: %s]\n", tls_error());
      out(buf);
      return -1;
    }
  } else if (k_tcp_connect(u.host, u.port) < 0) {
    snprintf(buf, sizeof(buf), "[sem conexao com %s:%d]\n", u.host, u.port);
    out(buf);
    return -1;
  }
  n = snprintf(req, sizeof(req),
               "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SODOS-agent/0.1\r\n"
               "Content-Type: application/json\r\nAccept: text/event-stream\r\n"
               "%s%s%s"
               "Content-Length: %d\r\nConnection: close\r\n\r\n",
               u.path, u.host, cfg_key[0] ? "Authorization: Bearer " : "", cfg_key,
               cfg_key[0] ? "\r\n" : "", json->len);
  if (c_send(req, n) < 0) goto send_err;
  for (off = 0; off < json->len; off += 4096) {
    int part = json->len - off;
    if (part > 4096) part = 4096;
    if (c_send(json->p + off, part) < 0) goto send_err;
  }
  while (!r->done && (n = c_recv(buf, (int)sizeof(buf) - 1)) > 0) feed(r, buf, n);
  c_close();
  if (r->body.len && r->status == 200 && !r->sse) {
    /* servidor sem stream: resposta inteira em JSON */
    json_get(r->body.p, r->body.p + r->body.len, "content", &r->text);
    show_utf8(r->text.p, r->text.len);
  }
  if (r->status != 200) {
    str_t m = {0, 0, 0};
    if (!r->status) snprintf(buf, sizeof(buf), "\n[sem resposta da API (%u ms)]\n", k_ms() - t0);
    else snprintf(buf, sizeof(buf), "\n[API respondeu %d] ", r->status);
    out(buf);
    if (r->body.len && json_get(r->body.p, r->body.p + r->body.len, "message", &m))
      show_utf8(m.p, m.len);
    else if (r->body.len)
      show_utf8(r->body.p, r->body.len > 300 ? 300 : r->body.len);
    out("\n");
    s_free(&m);
    return -1;
  }
  if (n < 0 && !r->done && !r->text.len) {
    out("\n[conexao caiu no meio da resposta]\n");
    return -1;
  }
  out("\n");
  return 0;
send_err:
  out("[falha ao enviar o pedido]\n");
  c_close();
  return -1;
}

/* ---------- conversa ---------- */
typedef struct {
  const char* role;
  char* text; /* UTF-8, k_malloc */
} msg_t;

static msg_t* msgs;
static int nmsgs, cap_msgs;
static int auto_yes;

static void msg_add(const char* role, const char* text, int n) {
  if (nmsgs == cap_msgs) {
    int nc = cap_msgs ? cap_msgs * 2 : 16;
    msg_t* nm = (msg_t*)k_realloc(msgs, (unsigned)nc * sizeof(msg_t));
    if (!nm) return;
    msgs = nm;
    cap_msgs = nc;
  }
  msgs[nmsgs].role = role;
  msgs[nmsgs].text = s_dup(text, n);
  if (msgs[nmsgs].text) nmsgs++;
}

static void msgs_reset(void) {
  int i;
  for (i = 1; i < nmsgs; i++) k_free(msgs[i].text);
  if (nmsgs > 1) nmsgs = 1;
}

static void build_json(str_t* j) {
  int i;
  s_clear(j);
  s_add(j, "{\"model\":");
  json_str(j, cfg_model);
  s_add(j, ",\"stream\":true,\"temperature\":0.2,\"messages\":[");
  for (i = 0; i < nmsgs; i++) {
    if (i) s_addc(j, ',');
    s_add(j, "{\"role\":\"");
    s_add(j, msgs[i].role);
    s_add(j, "\",\"content\":");
    json_str(j, msgs[i].text);
    s_addc(j, '}');
  }
  s_add(j, "]}");
}

static const char sys_head[] =
  "You are AGENT, an AI assistant that runs natively inside SODOS, a small "
  "DOS-like operating system (64-bit kernel, FAT16 drives such as C:, D: and the "
  "USB stick U:, a graphical desktop called WIN, its own C/C++ compiler CC.EXE, "
  "TLS 1.3 networking). You work by running commands at the SODOS prompt.\n"
  "Always answer in the user's language.\n\n"
  "To act, end your reply with exactly ONE action block, then stop and wait:\n"
  "```cmd\n<one prompt line, at most 200 characters>\n```\n"
  "or, to create or overwrite a file with the full content:\n"
  "```write D:\\PATH\\FILE.EXT\n<content>\n```\n"
  "After each action you receive its output and exit code. "
  "When the task is finished, or you need the user, reply with NO action block.\n\n"
  "Rules:\n"
  "- These are DOS-style commands, not Unix: dir, type, cd, copy, del, mkdir. "
  "Paths use backslashes. File names are 8.3 (NAME.EXT).\n"
  "- Never run interactive or full-screen programs (edit, mc, win, more, "
  "AGENT itself, or anything waiting for keys): there is no one to type.\n"
  "- Compile C with: cc -o PROG.EXE PROG.C   (then run PROG).\n"
  "- Keep commands short; inspect before changing; do not delete files the "
  "user did not ask you to delete.\n\n";

static void build_system(void) {
  str_t s = {0, 0, 0};
  char* cap = (char*)k_malloc(OUT_MAX);
  char line[64];
  char cwd[128];
  int len = 0;
  s_add(&s, sys_head);
  k_pwd(cwd, (int)sizeof(cwd));
  s_add(&s, "Current directory: ");
  cp850_to_utf8(&s, cwd, (int)strlen(cwd));
  s_add(&s, "\n\nOutput of the `help` command:\n");
  if (cap) {
    snprintf(line, sizeof(line), "help");
    k_shell_capture(line, cap, 8192, &len);
    if (len < 0) len = -len;
    cp850_to_utf8(&s, cap, len);
    k_free(cap);
  }
  msg_add("system", s.p, s.len);
  s_free(&s);
}

/* Acha o bloco de acao na resposta. kind 'c' = cmd, 'w' = write.
 * arg = linha do comando ou caminho; body = conteudo do write. */
static int find_action(const char* t, int* kind, str_t* arg, str_t* body) {
  const char* p = t;
  while ((p = strstr(p, "```")) != 0) {
    const char* tag = p + 3;
    const char* nl = strchr(tag, '\n');
    const char* close;
    if (!nl) return 0;
    if (!strncmp(tag, "cmd", 3) && (tag[3] == '\n' || tag[3] == ' ' || tag[3] == '\r')) {
      const char* a = nl + 1;
      const char* e;
      close = strstr(a, "```");
      if (!close) close = a + strlen(a);
      while (a < close && (*a == ' ' || *a == '\n' || *a == '\r')) a++;
      e = a;
      while (e < close && *e != '\n' && *e != '\r') e++;
      while (e > a && e[-1] == ' ') e--;
      utf8_to_cp850(arg, a, (int)(e - a));
      *kind = 'c';
      return 1;
    }
    if (!strncmp(tag, "write ", 6)) {
      const char* a = tag + 6;
      const char* e = nl;
      while (*a == ' ') a++;
      while (e > a && (e[-1] == ' ' || e[-1] == '\r')) e--;
      utf8_to_cp850(arg, a, (int)(e - a));
      close = strstr(nl + 1, "\n```");
      if (!close) close = nl + 1 + strlen(nl + 1);
      else close++;
      utf8_to_cp850(body, nl + 1, (int)(close - (nl + 1)));
      *kind = 'w';
      return 1;
    }
    p = nl + 1;
    /* pula o bloco que nao e acao (```c, ```text...) */
    close = strstr(p, "```");
    if (!close) return 0;
    p = close + 3;
  }
  return 0;
}

/* Pergunta antes de agir. 1 = pode, 0 = recusou (motivo em why). */
static int confirm(str_t* why) {
  char ans[LINE_MAX];
  if (auto_yes) return 1;
  out("Executar? [S]im, [n]ao, [t]odos > ");
  if (k_readline(ans, (int)sizeof(ans)) < 0) return 0;
  if (ans[0] == 't' || ans[0] == 'T') {
    auto_yes = 1;
    return 1;
  }
  if (ans[0] == 0 || ans[0] == 's' || ans[0] == 'S' || ans[0] == 'y' || ans[0] == 'Y') return 1;
  out("O que fazer em vez disso? > ");
  if (k_readline(ans, (int)sizeof(ans)) < 0) ans[0] = 0;
  s_add(why, "The user refused this action.");
  if (ans[0]) {
    s_add(why, " User says: ");
    cp850_to_utf8(why, ans, (int)strlen(ans));
  }
  return 0;
}

/* Roda a acao e monta a observacao para o modelo (UTF-8). */
static void run_action(int kind, str_t* arg, str_t* body, str_t* obs) {
  char msg[128];
  if (kind == 'w') {
    int rc = k_fsave(arg->p, body->p ? body->p : "", body->len);
    snprintf(msg, sizeof(msg), "[gravou %s: %d bytes, codigo %d]\n", arg->p, body->len, rc);
    out(msg);
    snprintf(msg, sizeof(msg), "write: %d bytes, result %d%s", body->len, rc,
             rc < 0 ? " (failed)" : " (ok)");
    s_add(obs, msg);
    return;
  }
  if (arg->len == 0 || arg->len > LINE_MAX - 1) {
    s_add(obs, arg->len ? "Command too long (max 200 characters)." : "Empty command.");
    out("[comando vazio ou longo demais]\n");
    return;
  }
  {
    char line[LINE_MAX];
    char* cap = (char*)k_malloc(OUT_MAX);
    int len = 0, rc, cut;
    if (!cap) {
      s_add(obs, "Out of memory.");
      return;
    }
    memcpy(line, arg->p, (size_t)arg->len + 1);
    rc = k_shell_capture(line, cap, OUT_MAX, &len);
    cut = len < 0;
    if (cut) len = -len;
    k_write(cap, len);
    if (len && cap[len - 1] != '\n') out("\n");
    snprintf(msg, sizeof(msg), "Exit code %d. Output (%d bytes%s):\n", rc, len,
             cut ? ", truncated" : "");
    s_add(obs, msg);
    if (len > OBS_MAX) {
      cp850_to_utf8(obs, cap, OBS_MAX / 2);
      snprintf(msg, sizeof(msg), "\n... [%d bytes omitted] ...\n", len - OBS_MAX);
      s_add(obs, msg);
      cp850_to_utf8(obs, cap + len - OBS_MAX / 2, OBS_MAX / 2);
    } else {
      cp850_to_utf8(obs, cap, len);
    }
    k_free(cap);
  }
}

/* Um pedido do usuario: conversa com o modelo ate ele parar de agir. */
static void run_task(void) {
  str_t json = {0, 0, 0};
  int step;
  for (step = 0; step < STEPS_MAX; step++) {
    resp_t r;
    str_t arg = {0, 0, 0}, body = {0, 0, 0}, obs = {0, 0, 0};
    int kind = 0;
    build_json(&json);
    out("\nagente: ");
    if (api_post(&json, &r) < 0) {
      resp_free(&r);
      break;
    }
    msg_add("assistant", r.text.p ? r.text.p : "", r.text.len);
    if (!r.text.p || !find_action(r.text.p, &kind, &arg, &body)) {
      resp_free(&r);
      break;
    }
    resp_free(&r);
    if (kind == 'c') {
      out("\n> ");
      k_write(arg.p, arg.len);
      out("\n");
    } else {
      char m[128];
      snprintf(m, sizeof(m), "\n> gravar %s (%d bytes)\n", arg.p ? arg.p : "", body.len);
      out(m);
    }
    if (confirm(&obs)) run_action(kind, &arg, &body, &obs);
    msg_add("user", obs.p ? obs.p : "", obs.len);
    s_free(&arg);
    s_free(&body);
    s_free(&obs);
  }
  if (step == STEPS_MAX) out("[limite de acoes neste pedido; continue com outra mensagem]\n");
  s_free(&json);
}

static void ask(const char* text) {
  str_t u = {0, 0, 0};
  cp850_to_utf8(&u, text, (int)strlen(text));
  msg_add("user", u.p ? u.p : "", u.len);
  s_free(&u);
  run_task();
}

int main(int argc, char** argv) {
  char line[LINE_MAX];
  str_t task = {0, 0, 0};
  const char* model = 0;
  int i;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-y") || !strcmp(argv[i], "-Y")) {
      auto_yes = 1;
    } else if ((!strcmp(argv[i], "-m") || !strcmp(argv[i], "-M")) && i + 1 < argc) {
      model = argv[++i];
    } else {
      if (task.len) s_addc(&task, ' ');
      s_add(&task, argv[i]);
    }
  }
  if (cfg_load() < 0) {
    out("AGENT.CFG nao encontrado (pasta atual, D:\\, U:\\ ou C:\\). Exemplo:\n"
        "  key=sk-...   (ou deepseek=sk-...)\n"
        "  url=https://api.deepseek.com/chat/completions\n"
        "  model=deepseek-chat\n");
    return 1;
  }
  if (model) snprintf(cfg_model, sizeof(cfg_model), "%s", model); /* -m ganha do AGENT.CFG */
  if (!k_net_ready()) out("[aviso: rede nao esta pronta; use net antes]\n");
  build_system();
  snprintf(line, sizeof(line), "AGENT 0.1 - %s em %s (%s)\n", cfg_model, cfg_url, cfg_path);
  out(line);
  out("Escreva o pedido. /novo limpa a conversa, /sair termina.\n");
  if (task.len) {
    out("\nvoce> ");
    out(task.p);
    out("\n");
    ask(task.p);
  }
  s_free(&task);
  for (;;) {
    out("\nvoce> ");
    if (k_readline(line, (int)sizeof(line)) < 0) break;
    if (!line[0]) continue;
    if (!strcmp(line, "/sair") || !strcmp(line, "/exit")) break;
    if (!strcmp(line, "/novo") || !strcmp(line, "/new")) {
      msgs_reset();
      out("[conversa limpa]\n");
      continue;
    }
    ask(line);
  }
  return 0;
}
