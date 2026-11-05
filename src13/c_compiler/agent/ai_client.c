/* ai_client.c - conversa com uma API no formato OpenAI (ver ai_client.h).
 *
 * A logica de HTTP/TLS/SSE veio do agent.c (AGENT.EXE), que ja roda isso no
 * SODOS: mesma configuracao AGENT.CFG, mesmo tratamento de chunked e de
 * "data:" do stream.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include <ai_client.h>

#define HDR_MAX 8192
#define BODY_MAX (512 * 1024)

static char err[200];

const char* ai_last_error(void) { return err; }

static void set_err(const char* s) {
  int i = 0;
  while (s[i] && i < (int)sizeof(err) - 1) {
    err[i] = s[i];
    i++;
  }
  err[i] = 0;
}

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

/* ---------- UTF-8 -> CP850 (a fonte do console) ---------- */
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

static int cp850_of(unsigned u) {
  unsigned i;
  if (u < 0x80) return (int)u;
  if (u >= 0xA0 && u <= 0xFF) {
    /* Latin-1: procura o byte CP850 correspondente */
    for (i = 0; i < 96; i++)
      if ((unsigned)(0xA0 + i) == u) return latin1_cp850[i];
  }
  switch (u) {
    case 0x2018: case 0x2019: return 0x27;
    case 0x201C: case 0x201D: return 0x22;
    case 0x2013: case 0x2014: return 0x2D;
    case 0x2026: return 0x2E;
    case 0x2022: return 0x07;
    case 0x2192: return 0x1A;
    default: return 0x3F;
  }
}

/* CP850 (teclado/historico antigo) -> UTF-8. */
int ai_cp850_to_utf8(char* dst, int cap, const char* src, int n) {
  int i, o = 0;
  for (i = 0; i < n; i++) {
    int c = (unsigned char)src[i];
    if (c < 0x80) {
      if (o < cap - 1) dst[o++] = (char)c;
    } else {
      unsigned u = 0;
      unsigned k;
      for (k = 0; k < 96; k++)
        if ((unsigned)latin1_cp850[k] == (unsigned)c) {
          u = 0xA0 + k;
          break;
        }
      if (!u) u = 0x3F;
      if (o + 3 >= cap) break;
      if (u < 0x800) {
        dst[o++] = (char)(0xC0 | (u >> 6));
        dst[o++] = (char)(0x80 | (u & 0x3F));
      }
    }
  }
  dst[o] = 0;
  return o;
}

/* Converte UTF-8 para CP850 em dst (cap-1 bytes, com NUL). Devolve o tamanho. */
int ai_utf8_to_cp850(char* dst, int cap, const char* src, int n) {
  int i = 0, o = 0;
  while (i < n && o < cap - 1) {
    int c = (unsigned char)src[i];
    unsigned u;
    if (c < 0x80) {
      if (c >= 32 || c == '\n' || c == '\t') dst[o++] = (char)c;
      i++;
      continue;
    }
    if ((c & 0xE0) == 0xC0 && i + 1 < n) {
      u = (unsigned)(c & 0x1F) << 6 | (unsigned)((unsigned char)src[i + 1] & 0x3F);
      i += 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
      u = (unsigned)(c & 0x0F) << 12 |
          (unsigned)((unsigned char)src[i + 1] & 0x3F) << 6 |
          (unsigned)((unsigned char)src[i + 2] & 0x3F);
      i += 3;
    } else {
      i++;
      continue;
    }
    dst[o++] = (char)cp850_of(u);
  }
  dst[o] = 0;
  return o;
}

/* ---------- JSON ---------- */
static void json_str(str_t* s, const char* v) {
  s_addc(s, '"');
  while (*v) {
    unsigned char c = (unsigned char)*v++;
    if (c == '"' || c == '\\') {
      s_addc(s, '\\');
      s_addc(s, c);
    } else if (c == '\n') {
      s_add(s, "\\n");
    } else if (c == '\r') {
      s_add(s, "\\r");
    } else if (c == '\t') {
      s_add(s, "\\t");
    } else if (c < 0x20) {
      char tmp[8];
      snprintf(tmp, sizeof(tmp), "\\u%04x", c);
      s_add(s, tmp);
    } else {
      s_addc(s, c);
    }
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
  return ((unsigned)hexv(p[0]) << 12) | ((unsigned)hexv(p[1]) << 8) |
         ((unsigned)hexv(p[2]) << 4) | (unsigned)hexv(p[3]);
}

static void utf8_put(str_t* s, unsigned u) {
  if (u < 0x80) {
    s_addc(s, (int)u);
  } else if (u < 0x800) {
    s_addc(s, (int)(0xC0 | (u >> 6)));
    s_addc(s, (int)(0x80 | (u & 0x3F)));
  } else {
    s_addc(s, (int)(0xE0 | (u >> 12)));
    s_addc(s, (int)(0x80 | ((u >> 6) & 0x3F)));
    s_addc(s, (int)(0x80 | (u & 0x3F)));
  }
}

/* Le uma string JSON (com escapes) para o. */
static const char* json_read_str(const char* p, const char* end, str_t* o) {
  if (p >= end || *p != '"') return 0;
  p++;
  while (p < end && *p != '"') {
    if (*p == '\\' && p + 1 < end) {
      p++;
      switch (*p) {
        case 'n': s_addc(o, '\n'); break;
        case 'r': s_addc(o, '\r'); break;
        case 't': s_addc(o, '\t'); break;
        case 'b': s_addc(o, '\b'); break;
        case 'f': s_addc(o, '\f'); break;
        case 'u':
          if (p + 4 < end) {
            unsigned u = hex4(p + 1);
            p += 4;
            if (u >= 0xD800 && u <= 0xDBFF && p + 6 < end && p[1] == '\\' &&
                p[2] == 'u') {
              unsigned lo = hex4(p + 3);
              u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
              p += 6;
            }
            utf8_put(o, u);
          }
          break;
        default: s_addc(o, (unsigned char)*p); break;
      }
      p++;
    } else {
      s_addc(o, (unsigned char)*p);
      p++;
    }
  }
  return p < end ? p + 1 : p;
}

/* Procura "chave": "valor" e devolve o valor na string o. */
static int json_get(const char* from, const char* end, const char* key, str_t* o) {
  char pat[64];
  int n = 0;
  const char* p;
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  n = (int)strlen(pat);
  for (p = from; p + n < end; p++) {
    if (memcmp(p, pat, (size_t)n)) continue;
    p += n;
    while (p < end && (*p == ' ' || *p == ':')) p++;
    if (p < end && *p == '"') {
      if (json_read_str(p, end, o)) return 1;
    }
    return 0;
  }
  return 0;
}

/* ---------- configuracao (AGENT.CFG) ---------- */
void ai_cfg_defaults(ai_cfg_t* c) {
  memset(c, 0, sizeof(*c));
  snprintf(c->url, sizeof(c->url), "%s", "https://api.deepseek.com/chat/completions");
  snprintf(c->model, sizeof(c->model), "%s", "deepseek-chat");
}

static void cfg_set(ai_cfg_t* c, char* line) {
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
  if (!strcmp(line, "key")) {
    snprintf(c->key, sizeof(c->key), "%s", v);
  } else if (!strcmp(line, "deepseek")) {
    snprintf(c->key, sizeof(c->key), "%s", v);
    snprintf(c->url, sizeof(c->url), "%s", "https://api.deepseek.com/chat/completions");
  } else if (!strcmp(line, "url")) {
    snprintf(c->url, sizeof(c->url), "%s", v);
  } else if (!strcmp(line, "model")) {
    snprintf(c->model, sizeof(c->model), "%s", v);
  }
}

int ai_cfg_load(ai_cfg_t* c) {
  static const char* const places[] = {"AGENT.CFG", "D:\\AGENT.CFG", "U:\\AGENT.CFG",
                                       "C:\\AGENT.CFG"};
  char buf[2048];
  unsigned i;
  ai_cfg_defaults(c);
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
      cfg_set(c, p);
      p = e;
    }
    snprintf(c->path, sizeof(c->path), "%s", places[i]);
    return 0;
  }
  return -1;
}

int ai_cfg_ready(const ai_cfg_t* c) { return c && c->url[0]; }

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

/* TLS carregado em tempo de execucao (k_dll_sym): o linker do SODOS corrompe
 * o heap quando um EXE importa GUI.DLL e TLS.DLL juntos. */
typedef struct {
  int (*connect)(const char*, int);
  int (*send)(const char*, int);
  int (*recv)(char*, int);
  void (*close)(void);
  const char* (*error)(void);
  int ok;
} tls_api_t;

static tls_api_t tlsapi;

static int tls_load(void) {
  int h;
  if (tlsapi.ok) return 0;
  h = k_dll_load("TLS.DLL");
  if (!h) return -1;
  tlsapi.connect = (int (*)(const char*, int))k_dll_sym(h, "tls_connect");
  tlsapi.send = (int (*)(const char*, int))k_dll_sym(h, "tls_send");
  tlsapi.recv = (int (*)(char*, int))k_dll_sym(h, "tls_recv");
  tlsapi.close = (void (*)(void))k_dll_sym(h, "tls_close");
  tlsapi.error = (const char* (*)(void))k_dll_sym(h, "tls_error");
  if (!tlsapi.connect || !tlsapi.send || !tlsapi.recv || !tlsapi.close) {
    set_err("TLS.DLL sem as funcoes esperadas");
    return -1;
  }
  tlsapi.ok = 1;
  return 0;
}

static int c_tls;
static int c_send(const char* d, int n) {
  return c_tls ? tlsapi.send(d, n) : k_tcp_send(d, n);
}
static int c_recv(char* d, int cap) {
  return c_tls ? tlsapi.recv(d, cap) : k_tcp_recv(d, cap);
}
static void c_close(void) {
  if (c_tls) tlsapi.close();
  else k_tcp_close();
}

typedef struct {
  str_t hdr;
  int hdr_done;
  int status;
  int chunked, sse;
  int ck_state;
  unsigned ck_left;
  str_t ck_line;
  str_t line;
  str_t body;
  str_t text;
  int done;
  void (*on_token)(void* ud, const char* s, int n);
  void* ud;
  /* chamadas de ferramenta (formato OpenAI, em deltas) */
  ai_tool_t* tools;
  int max_tools;
  int n_tools;
  str_t tc_args[4];
  char tc_name[4][24];
  char tc_id[4][80];
} resp_t;

/* strstr limitado a [a,b). */
static int has_word(const char* a, const char* b, const char* w) {
  int n = (int)strlen(w);
  const char* p;
  for (p = a; p + n <= b; p++)
    if (!memcmp(p, w, (size_t)n)) return 1;
  return 0;
}

/* Guarda um pedaco de tool_call: index, id, nome e argumentos (JSON). */
static void tool_delta(resp_t* r, const char* line, const char* end) {
  const char* p = line;
  while ((p = strstr(p, "\"index\":")) != 0 && p < end) {
    int idx = 0;
    const char* q = p + 8;
    const char* idp;
    const char* namep;
    while (q < end && *q == ' ') q++;
    while (q < end && *q >= '0' && *q <= '9') idx = idx * 10 + (*q++ - '0');
    if (idx < 0 || idx > 3) { p = q; continue; }
    if (idx + 1 > r->n_tools) r->n_tools = idx + 1;
    idp = strstr(q, "\"id\":");
    if (idp && idp < end) {
      const char* v = idp + 5;
      str_t tmp;
      memset(&tmp, 0, sizeof(tmp));
      v = json_read_str(v, end, &tmp);
      if (tmp.p && !r->tc_id[idx][0]) {
        int k = 0;
        while (tmp.p[k] && k < 79) { r->tc_id[idx][k] = tmp.p[k]; k++; }
        r->tc_id[idx][k] = 0;
      }
      s_free(&tmp);
    }
    namep = strstr(q, "\"name\":");
    if (namep && namep < end && !r->tc_name[idx][0]) {
      const char* v = namep + 7;
      str_t tmp;
      memset(&tmp, 0, sizeof(tmp));
      v = json_read_str(v, end, &tmp);
      if (tmp.p) {
        int k = 0;
        while (tmp.p[k] && k < 23) { r->tc_name[idx][k] = tmp.p[k]; k++; }
        r->tc_name[idx][k] = 0;
      }
      s_free(&tmp);
    }
    {
      const char* ap = strstr(q, "\"arguments\":");
      if (ap && ap < end) {
        str_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (json_read_str(ap + 12, end, &tmp) && tmp.p)
          s_addn(&r->tc_args[idx], tmp.p, tmp.len);
        s_free(&tmp);
      }
    }
    p = q;
  }
}

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

static void emit(resp_t* r, const char* s, int n) {
  if (n <= 0) return;
  s_addn(&r->text, s, n);
  if (r->on_token) r->on_token(r->ud, s, n);
}

static void sse_line(resp_t* r, const char* l, int n) {
  str_t v;
  const char* end = l + n;
  memset(&v, 0, sizeof(v));
  if (n < 5 || memcmp(l, "data:", 5) != 0) return;
  l += 5;
  while (l < end && *l == ' ') l++;
  if (end - l >= 6 && !memcmp(l, "[DONE]", 6)) {
    r->done = 1;
    return;
  }
  if (json_get(l, end, "content", &v) && v.len) emit(r, v.p, v.len);
  else if (has_word(l, end, "tool_calls"))
    tool_delta(r, l, end);
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

/* Transforma os deltas acumulados em chamadas prontas. */
static int collect_tools(resp_t* r) {
  int i, k = 0;
  for (i = 0; i < r->n_tools && k < r->max_tools; i++) {
    ai_tool_t* o = &r->tools[k];
    const char* name = r->tc_name[i];
    const char* args = r->tc_args[i].p;
    int n = r->tc_args[i].len;
    int j;
    memset(o, 0, sizeof(*o));
    for (j = 0; j < 79 && r->tc_id[i][j]; j++) o->id[j] = r->tc_id[i][j];
    o->id[j] = 0;
    if (!name[0] || !args || n <= 0) continue;
    if (!strcmp(name, "write")) {
      str_t v;
      memset(&v, 0, sizeof(v));
      if (json_get(args, args + n, "path", &v)) {
        int m = v.len > 127 ? 127 : v.len;
        memcpy(o->arg, v.p, (size_t)m);
        o->arg[m] = 0;
      }
      s_free(&v);
      memset(&v, 0, sizeof(v));
      if (json_get(args, args + n, "content", &v)) {
        int m = v.len > 1399 ? 1399 : v.len;
        memcpy(o->body, v.p, (size_t)m);
        o->body[m] = 0;
      }
      s_free(&v);
      if (o->arg[0]) {
        o->kind = 'w';
        k++;
      }
    } else if (!strcmp(name, "run") || !strcmp(name, "cmd") ||
               !strcmp(name, "shell")) {
      str_t v;
      memset(&v, 0, sizeof(v));
      if (json_get(args, args + n, "command", &v)) {
        int m = v.len > 127 ? 127 : v.len;
        memcpy(o->arg, v.p, (size_t)m);
        o->arg[m] = 0;
      }
      s_free(&v);
      if (o->arg[0]) {
        o->kind = 'c';
        k++;
      }
    } else if (!strcmp(name, "open") || !strcmp(name, "launch")) {
      str_t v;
      memset(&v, 0, sizeof(v));
      if (json_get(args, args + n, "program", &v) ||
          json_get(args, args + n, "path", &v)) {
        int m = v.len > 127 ? 127 : v.len;
        memcpy(o->arg, v.p, (size_t)m);
        o->arg[m] = 0;
      }
      s_free(&v);
      if (o->arg[0]) {
        o->kind = 'o';
        k++;
      }
    }
  }
  return k;
}

static void resp_free(resp_t* r) {
  int i;
  for (i = 0; i < 4; i++) s_free(&r->tc_args[i]);
  s_free(&r->hdr);
  s_free(&r->ck_line);
  s_free(&r->line);
  s_free(&r->body);
  s_free(&r->text);
}

/* Definicao das ferramentas no formato OpenAI. */
static const char* TOOLS_JSON =
    ",\"tools\":["
    "{\"type\":\"function\",\"function\":{\"name\":\"write\",\"description\":"
    "\"Cria ou sobrescreve um arquivo no SODOS\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"content\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"run\",\"description\":"
    "\"Roda uma linha no prompt do SODOS (dir, type, copy, del, mkdir, cc)\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":"
    "{\"type\":\"string\"}},\"required\":[\"command\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"open\",\"description\":"
    "\"Abre a janela de um programa .EXE (D:\\\\EXPLORER.EXE etc)\",\"parameters\":"
    "{\"type\":\"object\",\"properties\":{\"program\":{\"type\":\"string\"}},"
    "\"required\":[\"program\"]}}}"
    "],\"tool_choice\":\"auto\"";

static int build_json_t(const ai_cfg_t* c, const ai_msg_t* msgs, int n, str_t* j,
                        int with_tools) {
  int i;
  s_clear(j);
  s_add(j, "{\"model\":");
  json_str(j, c->model);
  s_add(j, ",\"stream\":true,\"temperature\":0.2,\"messages\":[");
  for (i = 0; i < n; i++) {
    if (i) s_addc(j, ',');
    s_add(j, "{\"role\":\"");
    s_add(j, msgs[i].role);
    s_add(j, "\",\"content\":");
    json_str(j, msgs[i].text);
    s_addc(j, '}');
  }
  s_addc(j, ']');
  if (with_tools) s_add(j, TOOLS_JSON);
  s_addc(j, '}');
  return j->len;
}

static int build_json(const ai_cfg_t* c, const ai_msg_t* msgs, int n, str_t* j) {
  return build_json_t(c, msgs, n, j, 0);
}

/* ---------- conversa ---------- */
static int chat_core(const ai_cfg_t* c, const ai_msg_t* msgs, int n,
                     void (*on_token)(void* ud, const char* s, int n), void* ud,
                     int with_tools, ai_tool_t* tools, int max_tools) {
  url_t u;
  char req[1024];
  char buf[4096];
  int len, off, r;
  unsigned t0;
  str_t json;
  resp_t resp;

  set_err("");
  if (!ai_cfg_ready(c)) {
    set_err("sem url em AGENT.CFG");
    return -1;
  }
  memset(&json, 0, sizeof(json));
  memset(&resp, 0, sizeof(resp));
  resp.on_token = on_token;
  resp.ud = ud;
  resp.tools = tools;
  resp.max_tools = max_tools;
  resp_tools_init:;
  if (build_json_t(c, msgs, n, &json, with_tools) <= 0) {
    set_err("sem memoria para o pedido");
    return -1;
  }
  if (url_parse(c->url, &u) < 0) {
    s_free(&json);
    set_err("url invalida em AGENT.CFG");
    return -1;
  }
  c_tls = u.tls;
  t0 = k_ms();
  if (u.tls) {
    if (tls_load() != 0 || tlsapi.connect(u.host, u.port) != 0) {
      snprintf(err, sizeof(err), "TLS: %s",
               tlsapi.error ? tlsapi.error() : "TLS.DLL indisponivel");
      s_free(&json);
      return -1;
    }
  } else if (k_tcp_connect(u.host, u.port) < 0) {
    snprintf(err, sizeof(err), "sem conexao com %s:%d", u.host, u.port);
    s_free(&json);
    return -1;
  }
  len = snprintf(req, sizeof(req),
                 "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SODOS-ai/0.1\r\n"
                 "Content-Type: application/json\r\nAccept: text/event-stream\r\n"
                 "%s%s%s"
                 "Content-Length: %d\r\nConnection: close\r\n\r\n",
                 u.path, u.host, c->key[0] ? "Authorization: Bearer " : "", c->key,
                 c->key[0] ? "\r\n" : "", json.len);
  if (c_send(req, len) < 0) goto send_err;
  for (off = 0; off < json.len; off += 4096) {
    int part = json.len - off;
    if (part > 4096) part = 4096;
    if (c_send(json.p + off, part) < 0) goto send_err;
  }
  while (!resp.done && (r = c_recv(buf, (int)sizeof(buf) - 1)) > 0) feed(&resp, buf, r);
  c_close();
  if (resp.body.len && resp.status == 200 && !resp.sse) {
    str_t t;
    memset(&t, 0, sizeof(t));
    if (json_get(resp.body.p, resp.body.p + resp.body.len, "content", &t))
      emit(&resp, t.p, t.len);
    s_free(&t);
  }
  s_free(&json);
  if (resp.status != 200) {
    str_t m;
    memset(&m, 0, sizeof(m));
    if (!resp.status)
      snprintf(err, sizeof(err), "sem resposta da API (%u ms)", k_ms() - t0);
    else
      snprintf(err, sizeof(err), "API respondeu %d", resp.status);
    if (resp.body.len && json_get(resp.body.p, resp.body.p + resp.body.len, "message", &m))
      snprintf(err, sizeof(err), "API %d: %s", resp.status, m.p ? m.p : "");
    s_free(&m);
    resp_free(&resp);
    return -1;
  }
  if (r < 0 && !resp.done && !resp.text.len) {
    snprintf(err, sizeof(err), "conexao caiu no meio da resposta");
    resp_free(&resp);
    return -1;
  }
  {
    int got = resp.text.len;
    int nt = with_tools ? collect_tools(&resp) : 0;
    resp_free(&resp);
    if (nt > 0) return nt;
    return got > 0 ? 0 : -1;
  }
send_err:
  set_err("falha ao enviar o pedido");
  c_close();
  s_free(&json);
  resp_free(&resp);
  return -1;
}

int ai_chat(const ai_cfg_t* c, const ai_msg_t* msgs, int n,
            void (*on_token)(void* ud, const char* s, int n), void* ud) {
  return chat_core(c, msgs, n, on_token, ud, 0, 0, 0);
}

int ai_chat_tools(const ai_cfg_t* c, const ai_msg_t* msgs, int n,
                  void (*on_token)(void* ud, const char* s, int n), void* ud,
                  ai_tool_t* tools, int max_tools) {
  return chat_core(c, msgs, n, on_token, ud, 1, tools, max_tools);
}
