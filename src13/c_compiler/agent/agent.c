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
 * Claude (API da Anthropic, /v1/messages com ferramentas nativas):
 *   anthropic=sk-ant-...   (ja aponta a url e o modelo claude-opus-5-5)
 *   model=claude-sonnet-5-5   effort=high   max_tokens=32000   steps=100
 *   fallbacks=0            (desliga o fallback do servidor em recusas)
 * Com Claude o modelo usa ferramentas: run_command, read_file, write_file e
 * edit_file. AGENT.MD na pasta atual entra no prompt (notas do projeto).
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
static int obs_max = OBS_MAX; /* 30000 com Claude */
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

/* Esconde os blocos de chamada de ferramenta (<|DSML|...>) da tela: eles
 * viram acao, nao conversa. O texto normal continua aparecendo. */
static int hide_dsml;

static void show_clean(const char* s, int n) {
  int i = 0;
  while (i < n) {
    if (hide_dsml) {
      if (s[i] == '>' && i >= 5 && !memcmp(s + i - 5, "calls", 5)) hide_dsml = 0;
      i++;
      continue;
    }
    if (s[i] == '<' && i + 1 < n && (s[i + 1] == '|' || s[i + 1] == '?')) {
      hide_dsml = 1;
      i += 2;
      continue;
    }
    {
      int j = i;
      while (j < n && !(s[j] == '<' && j + 1 < n && (s[j + 1] == '|' || s[j + 1] == '?'))) j++;
      show_utf8(s + i, j - i);
      i = j;
    }
  }
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

/* ---------- JSON: navegar sem montar arvore ---------- */
static const char* jws(const char* p, const char* e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  return p;
}

/* Pula um valor JSON; devolve o ponteiro depois dele, ou 0 se incompleto. */
static const char* json_skip(const char* p, const char* e) {
  p = jws(p, e);
  if (p >= e) return 0;
  if (*p == '"') {
    p++;
    while (p < e && *p != '"') {
      if (*p == '\\') p++;
      p++;
    }
    return p < e ? p + 1 : 0;
  }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (p < e) {
      int c = *p;
      if (c == '"') {
        p = json_skip(p, e);
        if (!p) return 0;
        continue;
      }
      if (c == '{' || c == '[') depth++;
      else if (c == '}' || c == ']') {
        if (--depth == 0) return p + 1;
      }
      p++;
    }
    return 0;
  }
  {
    const char* s = p;
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' &&
           *p != '\r' && *p != '\t')
      p++;
    return p > s ? p : 0;
  }
}

/* Campo de primeiro nivel de um objeto. 1 = achou ([*vs,*ve) e o valor). */
static int jfield(const char* p, const char* e, const char* key, const char** vs,
                  const char** ve) {
  int kl = (int)strlen(key);
  p = jws(p, e);
  if (p >= e || *p != '{') return 0;
  p++;
  for (;;) {
    const char* ks;
    const char* kend;
    const char* v;
    p = jws(p, e);
    if (p >= e || *p != '"') return 0;
    ks = p + 1;
    p = json_skip(p, e);
    if (!p) return 0;
    kend = p - 1;
    p = jws(p, e);
    if (p >= e || *p != ':') return 0;
    v = jws(p + 1, e);
    p = json_skip(v, e);
    if (!p) return 0;
    if (kend - ks == kl && !memcmp(ks, key, (size_t)kl)) {
      *vs = v;
      *ve = p;
      return 1;
    }
    p = jws(p, e);
    if (p < e && *p == ',') p++;
    else return 0;
  }
}

static int jstr(const char* p, const char* e, const char* key, str_t* o) {
  const char* vs;
  const char* ve;
  if (!jfield(p, e, key, &vs, &ve) || *vs != '"') return 0;
  return json_read_str(vs, ve, o) != 0;
}

static long jnum(const char* p, const char* e, const char* key, long def) {
  const char* vs;
  const char* ve;
  if (!jfield(p, e, key, &vs, &ve) || !((*vs >= '0' && *vs <= '9') || *vs == '-')) return def;
  return atol(vs);
}

static int jtrue(const char* p, const char* e, const char* key) {
  const char* vs;
  const char* ve;
  return jfield(p, e, key, &vs, &ve) && ve - vs == 4 && !memcmp(vs, "true", 4);
}

/* So espacos depois de um valor completo? */
static int json_valid(const char* p, const char* e) {
  const char* q = json_skip(p, e);
  return q && jws(q, e) == e;
}

/* ---------- configuracao ---------- */
static char cfg_key[256];
static char cfg_url[256] = "https://api.deepseek.com/chat/completions";
static char cfg_model[64] = "deepseek-chat";
static char cfg_path[64];
static int cfg_anth;              /* 1 = API da Anthropic (Claude) */
static int cfg_model_set;         /* model= apareceu no AGENT.CFG */
static char cfg_effort[16] = "high";
static int cfg_maxtok = 32000;
static int cfg_steps = 0;         /* 0 = padrao (25; 100 com Claude) */
static int cfg_fallbacks = 1;
static int cfg_url_set;           /* url= apareceu no AGENT.CFG */

static void use_anthropic(void) {
  cfg_anth = 1;
  if (!cfg_url_set)
    snprintf(cfg_url, sizeof(cfg_url), "%s", "https://api.anthropic.com/v1/messages");
  if (!cfg_model_set) snprintf(cfg_model, sizeof(cfg_model), "%s", "claude-opus-5-5");
}

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
  else if (!strcmp(line, "anthropic") || !strcmp(line, "claude")) {
    snprintf(cfg_key, sizeof(cfg_key), "%s", v);
    use_anthropic();
  }
  else if (!strcmp(line, "provider")) {
    if (!strcmp(v, "anthropic") || !strcmp(v, "claude")) use_anthropic();
  }
  else if (!strcmp(line, "url")) {
    snprintf(cfg_url, sizeof(cfg_url), "%s", v);
    cfg_url_set = 1;
    if (strstr(v, "anthropic.com")) use_anthropic();
  }
  else if (!strcmp(line, "model")) {
    snprintf(cfg_model, sizeof(cfg_model), "%s", v);
    cfg_model_set = 1;
  }
  else if (!strcmp(line, "effort")) snprintf(cfg_effort, sizeof(cfg_effort), "%s", v);
  else if (!strcmp(line, "max_tokens")) cfg_maxtok = atoi(v);
  else if (!strcmp(line, "steps")) cfg_steps = atoi(v);
  else if (!strcmp(line, "fallbacks")) cfg_fallbacks = atoi(v) != 0;
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
  /* Anthropic */
  char stop[32];     /* stop_reason */
  int err;           /* evento error no stream */
  long in_tok, cache_rd, cache_wr, out_tok;
} resp_t;

/* Blocos de conteudo da resposta do Claude, na ordem do index. */
#define BLK_MAX 64
typedef struct {
  int kind;    /* 't' texto, 'k' thinking, 'u' tool_use, 'f' fallback, 'r' outro */
  str_t text;  /* texto, thinking ou o JSON de entrada da ferramenta */
  str_t sig;   /* assinatura do thinking */
  str_t id, name;
  str_t raw;   /* bloco inteiro como chegou (para 'f' e 'r') */
} blk_t;
static blk_t blk[BLK_MAX];
static int nblk;

static void blk_reset(void) {
  int i;
  for (i = 0; i < nblk; i++) {
    s_free(&blk[i].text);
    s_free(&blk[i].sig);
    s_free(&blk[i].id);
    s_free(&blk[i].name);
    s_free(&blk[i].raw);
    blk[i].kind = 0;
  }
  nblk = 0;
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
    show_clean(v.p, v.len);
  } else {
    s_clear(&v);
    if (r->thinking == 0 && json_get(l, end, "reasoning_content", &v) && v.len) {
      out("(pensando...)");
      r->thinking = 1;
    }
  }
  s_free(&v);
}

/* Uma linha "data: {...}" do stream da Anthropic. */
static void sse_line_anth(resp_t* r, const char* l, int n) {
  const char* end = l + n;
  str_t ty = {0, 0, 0};
  const char* vs;
  const char* ve;
  if (n < 5 || memcmp(l, "data:", 5) != 0) return;
  l = jws(l + 5, end);
  if (!jstr(l, end, "type", &ty) || !ty.p) {
    s_free(&ty);
    return;
  }
  if (!strcmp(ty.p, "content_block_start") && jfield(l, end, "content_block", &vs, &ve)) {
    int i = (int)jnum(l, end, "index", nblk);
    str_t bt = {0, 0, 0};
    blk_t* b;
    if (i < 0 || i >= BLK_MAX) goto done;
    while (nblk <= i) memset(&blk[nblk++], 0, sizeof(blk_t));
    b = &blk[i];
    jstr(vs, ve, "type", &bt);
    if (bt.p && !strcmp(bt.p, "text")) {
      b->kind = 't';
      jstr(vs, ve, "text", &b->text);
      if (r->thinking == 1) {
        out(")\n");
        r->thinking = 2;
      }
      if (b->text.len) show_utf8(b->text.p, b->text.len);
    } else if (bt.p && !strcmp(bt.p, "thinking")) {
      b->kind = 'k';
      jstr(vs, ve, "thinking", &b->text);
      jstr(vs, ve, "signature", &b->sig);
      if (r->thinking != 1) {
        out("(pensando");
        r->thinking = 1;
      }
    } else if (bt.p && !strcmp(bt.p, "tool_use")) {
      b->kind = 'u';
      jstr(vs, ve, "id", &b->id);
      jstr(vs, ve, "name", &b->name);
      if (r->thinking == 1) {
        out(")\n");
        r->thinking = 2;
      }
    } else {
      b->kind = (bt.p && !strcmp(bt.p, "fallback")) ? 'f' : 'r';
      s_addn(&b->raw, vs, (int)(ve - vs));
      if (b->kind == 'f') out("\n[recusa: o servidor passou para outro modelo]\n");
    }
    s_free(&bt);
  } else if (!strcmp(ty.p, "content_block_delta") && jfield(l, end, "delta", &vs, &ve)) {
    int i = (int)jnum(l, end, "index", -1);
    str_t dt = {0, 0, 0};
    blk_t* b;
    if (i < 0 || i >= nblk) goto done;
    b = &blk[i];
    jstr(vs, ve, "type", &dt);
    if (dt.p && !strcmp(dt.p, "text_delta")) {
      str_t t = {0, 0, 0};
      jstr(vs, ve, "text", &t);
      if (t.len) {
        s_addn(&b->text, t.p, t.len);
        show_utf8(t.p, t.len);
      }
      s_free(&t);
    } else if (dt.p && !strcmp(dt.p, "thinking_delta")) {
      static int ndots;
      jstr(vs, ve, "thinking", &b->text);
      if (r->thinking == 1 && (++ndots & 7) == 0) out(".");
    } else if (dt.p && !strcmp(dt.p, "signature_delta")) {
      jstr(vs, ve, "signature", &b->sig);
    } else if (dt.p && !strcmp(dt.p, "input_json_delta")) {
      jstr(vs, ve, "partial_json", &b->text);
    }
    s_free(&dt);
  } else if (!strcmp(ty.p, "message_start") && jfield(l, end, "message", &vs, &ve)) {
    const char* us;
    const char* ue;
    if (jfield(vs, ve, "usage", &us, &ue)) {
      r->in_tok = jnum(us, ue, "input_tokens", 0);
      r->cache_rd = jnum(us, ue, "cache_read_input_tokens", 0);
      r->cache_wr = jnum(us, ue, "cache_creation_input_tokens", 0);
    }
  } else if (!strcmp(ty.p, "message_delta")) {
    const char* us;
    const char* ue;
    str_t sr = {0, 0, 0};
    if (jfield(l, end, "delta", &vs, &ve) && jstr(vs, ve, "stop_reason", &sr) && sr.p)
      snprintf(r->stop, sizeof(r->stop), "%s", sr.p);
    if (jfield(l, end, "usage", &us, &ue)) r->out_tok = jnum(us, ue, "output_tokens", r->out_tok);
    s_free(&sr);
  } else if (!strcmp(ty.p, "message_stop")) {
    r->done = 1;
  } else if (!strcmp(ty.p, "error")) {
    str_t m = {0, 0, 0};
    if (jfield(l, end, "error", &vs, &ve)) jstr(vs, ve, "message", &m);
    out("\n[erro da API: ");
    if (m.len) show_utf8(m.p, m.len);
    out("]\n");
    s_free(&m);
    r->err = 1;
    r->done = 1;
  }
done:
  s_free(&ty);
}

static void body_byte(resp_t* r, int c) {
  if (!r->sse || r->status != 200) {
    if (r->body.len < BODY_MAX) s_addc(&r->body, c);
    return;
  }
  if (c == '\n') {
    int n = r->line.len;
    if (n && r->line.p[n - 1] == '\r') n--;
    if (n) {
      if (cfg_anth) sse_line_anth(r, r->line.p, n);
      else sse_line(r, r->line.p, n);
    }
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
  if (cfg_anth)
    n = snprintf(req, sizeof(req),
                 "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SODOS-agent/0.2\r\n"
                 "Content-Type: application/json\r\nAccept: text/event-stream\r\n"
                 "x-api-key: %s\r\nanthropic-version: 2026-10-21\r\n%s"
                 "Content-Length: %d\r\nConnection: close\r\n\r\n",
                 u.path, u.host, cfg_key,
                 cfg_fallbacks ? "anthropic-beta: server-side-fallback-2026-07-01\r\n" : "",
                 json->len);
  else
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
  {
    /* O recv do kernel desiste apos 8 s parado, mas a conexao continua (e o
     * TLS.DLL guarda o registro pela metade): o modelo pode pensar mais que
     * isso, entao espera ate 3 min enquanto o par nao fechar. ESC cancela. */
    unsigned idle = k_ms();
    for (;;) {
      if (r->done) break;
      n = c_recv(buf, (int)sizeof(buf) - 1);
      if (n > 0) {
        feed(r, buf, n);
        idle = k_ms();
        continue;
      }
      if (k_trygetc() == 27) {
        out("\n[interrompido com ESC]\n");
        c_close();
        r->err = 1;
        return -1;
      }
      if (n == -1 && k_net_pending() >= 0 && k_ms() - idle < 180000u) continue;
      break;
    }
  }
  c_close();
  if (r->body.len && r->status == 200 && !r->sse && !cfg_anth) {
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
  if (cfg_anth && (r->err || !r->done)) {
    if (!r->err) out("\n[conexao caiu no meio da resposta]\n");
    return -1;
  }
  if (!cfg_anth && n < 0 && !r->done && !r->text.len) {
    out("\n[conexao caiu no meio da resposta]\n");
    return -1;
  }
  if (r->thinking == 1) out(")");
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
  "Your call is also accepted as an XML block: "
  "<|DSML| invoke name=\"write\"> with parameter path and content, or "
  "name=\"cmd\"> with parameter command. The user only sees your prose: the "
  "call markup is hidden and executed. When you need something from the user, "
  "just ask in plain text, with no call block.\n"
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

/* Valor de um <|DSML| parameter name="NAME" ...>VALOR</|DSML| parameter>
 * dentro de [a,b). 1 = achou. */
static int dsml_param(const char* a, const char* b, const char* name, str_t* out) {
  int nl = (int)strlen(name);
  const char* p = a;
  while (p < b) {
    const char* q = strstr(p, "name=\"");
    const char* v, * e;
    if (!q || q >= b) return 0;
    q += 6;
    if (q + nl <= b && !memcmp(q, name, (size_t)nl) && q[nl] == '"') {
      v = q + nl + 1;
      while (v < b && *v != '>') v++;
      if (v >= b) return 0;
      v++;
      e = v;
      while (e < b && !(*e == '<' && e + 1 < b && e[1] == '/')) e++;
      utf8_to_cp850(out, v, (int)(e - v));
      return 1;
    }
    p = q + 1;
  }
  return 0;
}

/* Chamada de ferramenta no formato <|DSML| invoke name="..."> (o modelo as
 * vezes responde assim em vez do bloco ```). kind 'c' = cmd, 'w' = write. */
static int find_dsml(const char* t, int* kind, str_t* arg, str_t* body) {
  const char* inv = strstr(t, "DSML");
  const char* end;
  const char* nm;
  if (!inv) return 0;
  nm = strstr(inv, "invoke name=\"");
  if (!nm) return 0;
  {
    const char* ce = strstr(inv, "calls>");
    end = ce ? ce + 6 : t + strlen(t);
  }
  nm += 13;
  if (!strncmp(nm, "write", 5) || !strncmp(nm, "create", 6) || !strncmp(nm, "save", 4)) {
    if (dsml_param(inv, end, "path", arg) || dsml_param(inv, end, "file", arg) ||
        dsml_param(inv, end, "filename", arg)) {
      if (!dsml_param(inv, end, "content", body) && !dsml_param(inv, end, "text", body))
        dsml_param(inv, end, "body", body);
      *kind = 'w';
      return 1;
    }
  }
  if (!strncmp(nm, "cmd", 3) || !strncmp(nm, "run", 3) || !strncmp(nm, "shell", 5) ||
      !strncmp(nm, "exec", 4) || !strncmp(nm, "bash", 4)) {
    if (dsml_param(inv, end, "command", arg) || dsml_param(inv, end, "cmd", arg) ||
        dsml_param(inv, end, "line", arg)) {
      *kind = 'c';
      return 1;
    }
  }
  return 0;
}

/* Acha o bloco de acao na resposta. kind 'c' = cmd, 'w' = write.
 * arg = linha do comando ou caminho; body = conteudo do write. */
static int find_action(const char* t, int* kind, str_t* arg, str_t* body) {
  const char* p = t;
  if (find_dsml(t, kind, arg, body)) return 1;
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
    if (len > obs_max) {
      cp850_to_utf8(obs, cap, obs_max / 2);
      snprintf(msg, sizeof(msg), "\n... [%d bytes omitted] ...\n", len - obs_max);
      s_add(obs, msg);
      cp850_to_utf8(obs, cap + len - obs_max / 2, obs_max / 2);
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

/* ---------- Claude (API da Anthropic) ---------- */
/* Aqui msgs[i].text e o valor JSON do content (sempre um array) e o prompt
 * de sistema fica em g_sys. O historico so cresce: os blocos de thinking
 * voltam como vieram (com assinatura), como a API exige. */
static str_t g_sys;

static const char sys_anth[] =
  "You are Claude, running natively inside SODOS through AGENT.EXE. SODOS is a "
  "small DOS-like operating system: 64-bit kernel, FAT16/FAT32 drives such as C:, "
  "D:, the RAM disk Z: and the USB stick U:, a graphical desktop called WIN, its "
  "own C/C++ compiler CC.EXE, TLS 1.3 networking. You are a software engineer "
  "working on the user's files on this machine.\n"
  "Always answer in the user's language.\n\n"
  "Tools: run_command runs one line at the SODOS prompt; read_file shows a file "
  "with line numbers; write_file creates or overwrites a whole file; edit_file "
  "replaces an exact piece of a file (read it first; prefer edit_file over "
  "rewriting big files). Work step by step until the task is done: inspect, "
  "change, then compile and run to verify. When you are finished or need the "
  "user, reply without calling tools.\n\n"
  "Rules:\n"
  "- DOS-style commands, not Unix: dir, type, cd, copy, del, mkdir, ren. Paths "
  "use backslashes. Short 8.3 names (NAME.EXT) are the safest.\n"
  "- Never run interactive or full-screen programs (edit, mc, win, more, top, "
  "AGENT itself, or anything that waits for keys): nobody can type there.\n"
  "- Compile C with: cc -o PROG.EXE PROG.C   (then run PROG).\n"
  "- Command lines are at most 200 characters; output is captured.\n"
  "- Text files on disk are CP850; tools convert to and from UTF-8 for you.\n"
  "- Do not delete files the user did not ask you to delete.\n\n";

static const char tools_anth[] =
  "[{\"name\":\"run_command\",\"eager_input_streaming\":true,"
  "\"description\":\"Run one line at the SODOS command prompt and return its exit code "
  "and captured output. At most 200 characters. Never interactive programs.\","
  "\"input_schema\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\","
  "\"description\":\"The command line, for example: dir or cc -o HELLO.EXE HELLO.C\"}},"
  "\"required\":[\"command\"]}},"
  "{\"name\":\"read_file\",\"eager_input_streaming\":true,"
  "\"description\":\"Read a text file. Lines come back numbered (number, tab, text); "
  "the numbers are not part of the file.\","
  "\"input_schema\":{\"type\":\"object\",\"properties\":{"
  "\"path\":{\"type\":\"string\",\"description\":\"File path, for example D:/SRC/MAIN.C (either slash works)\"},"
  "\"offset\":{\"type\":\"integer\",\"description\":\"First line to show (1-based). Optional.\"},"
  "\"limit\":{\"type\":\"integer\",\"description\":\"How many lines. Optional, default 2000.\"}},"
  "\"required\":[\"path\"]}},"
  "{\"name\":\"write_file\",\"eager_input_streaming\":true,"
  "\"description\":\"Create or overwrite a file with the full content.\","
  "\"input_schema\":{\"type\":\"object\",\"properties\":{"
  "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
  "\"required\":[\"path\",\"content\"]}},"
  "{\"name\":\"edit_file\",\"eager_input_streaming\":true,"
  "\"description\":\"Replace old_string with new_string in a file. old_string must match "
  "the file exactly (without the line-number prefix of read_file) and be unique unless "
  "replace_all is true.\","
  "\"input_schema\":{\"type\":\"object\",\"properties\":{"
  "\"path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},"
  "\"new_string\":{\"type\":\"string\"},\"replace_all\":{\"type\":\"boolean\"}},"
  "\"required\":[\"path\",\"old_string\",\"new_string\"]}}]";

static void build_system_anth(void) {
  char cwd[128];
  int n;
  s_clear(&g_sys);
  s_add(&g_sys, sys_anth);
  k_pwd(cwd, (int)sizeof(cwd));
  s_add(&g_sys, "Current directory at start: ");
  cp850_to_utf8(&g_sys, cwd, (int)strlen(cwd));
  s_add(&g_sys, "\n");
  {
    char* cap = (char*)k_malloc(OUT_MAX);
    char line[16];
    int len = 0;
    if (cap) {
      snprintf(line, sizeof(line), "help");
      k_shell_capture(line, cap, 8192, &len);
      if (len < 0) len = -len;
      s_add(&g_sys, "\nOutput of the `help` command:\n");
      cp850_to_utf8(&g_sys, cap, len);
      k_free(cap);
    }
  }
  n = k_fsize("AGENT.MD");
  if (n > 0 && n <= 32768) {
    char* b = (char*)k_malloc((unsigned)n + 1);
    if (b && (n = k_fload("AGENT.MD", b, n + 1)) > 0) {
      s_add(&g_sys, "\n\nProject notes from AGENT.MD in the current directory:\n");
      cp850_to_utf8(&g_sys, b, n);
    }
    if (b) k_free(b);
  }
}

static void anth_reset(void) {
  int i;
  for (i = 0; i < nmsgs; i++) k_free(msgs[i].text);
  nmsgs = 0;
}

static void build_json_anth(str_t* j) {
  char num[96];
  int i;
  s_clear(j);
  s_add(j, "{\"model\":");
  json_str(j, cfg_model);
  snprintf(num, sizeof(num), ",\"max_tokens\":%d,\"stream\":true", cfg_maxtok > 0 ? cfg_maxtok : 32000);
  s_add(j, num);
  s_add(j, ",\"thinking\":{\"type\":\"adaptive\",\"display\":\"summarized\"}");
  if (cfg_effort[0]) {
    s_add(j, ",\"output_config\":{\"effort\":");
    json_str(j, cfg_effort);
    s_addc(j, '}');
  }
  if (cfg_fallbacks) s_add(j, ",\"fallbacks\":\"default\"");
  s_add(j, ",\"cache_control\":{\"type\":\"ephemeral\"},\"system\":");
  json_str(j, g_sys.p ? g_sys.p : "");
  s_add(j, ",\"tools\":");
  s_add(j, tools_anth);
  s_add(j, ",\"messages\":[");
  for (i = 0; i < nmsgs; i++) {
    if (i) s_addc(j, ',');
    s_add(j, "{\"role\":\"");
    s_add(j, msgs[i].role);
    s_add(j, "\",\"content\":");
    s_add(j, msgs[i].text);
    s_addc(j, '}');
  }
  s_add(j, "]}");
}

/* Junta blocos (JSON "{...},{...}") a conversa como uma mensagem do papel
 * role; se a ultima mensagem ja e desse papel, entram no mesmo array. */
static void anth_push(const char* role, const str_t* blocks) {
  str_t c = {0, 0, 0};
  if (!blocks->len) return;
  if (nmsgs && !strcmp(msgs[nmsgs - 1].role, role)) {
    char* old = msgs[nmsgs - 1].text;
    int ol = (int)strlen(old);
    s_addn(&c, old, ol - 1); /* sem o ']' */
    s_addc(&c, ',');
    s_addn(&c, blocks->p, blocks->len);
    s_addc(&c, ']');
    k_free(old);
    msgs[nmsgs - 1].text = s_dup(c.p, c.len);
  } else {
    s_addc(&c, '[');
    s_addn(&c, blocks->p, blocks->len);
    s_addc(&c, ']');
    msg_add(role, c.p, c.len);
  }
  s_free(&c);
}

static void add_text_block(str_t* b, const char* utf8) {
  if (b->len) s_addc(b, ',');
  s_add(b, "{\"type\":\"text\",\"text\":");
  json_str(b, utf8);
  s_addc(b, '}');
}

/* Caminho da ferramenta (UTF-8) para o SO (CP850, barras invertidas). */
static int tool_path(const char* in, const char* e, str_t* path) {
  str_t u = {0, 0, 0};
  int i;
  if (!jstr(in, e, "path", &u) || !u.len) {
    s_free(&u);
    return 0;
  }
  utf8_to_cp850(path, u.p, u.len);
  s_free(&u);
  for (i = 0; i < path->len; i++)
    if (path->p[i] == '/') path->p[i] = '\\';
  return path->len > 0;
}

#define READ_MAX (100 * 1024)

static int t_read(const char* path, long off, long lim, str_t* obs) {
  int size = k_fsize(path);
  char* buf;
  int n, i, line = 1, shown = 0, s;
  char m[96];
  if (size < 0) {
    s_add(obs, "File not found.");
    return -1;
  }
  if (size > 8 * 1024 * 1024) {
    s_add(obs, "File too big (over 8 MB).");
    return -1;
  }
  buf = (char*)k_malloc((unsigned)size + 1);
  if (!buf) {
    s_add(obs, "Out of memory.");
    return -1;
  }
  n = size ? k_fload(path, buf, size + 1) : 0; /* max inclui o NUL */
  if (n < 0) {
    k_free(buf);
    s_add(obs, "Could not read the file.");
    return -1;
  }
  if (off < 1) off = 1;
  if (lim < 1) lim = 2000;
  for (i = 0; i < n; i = s) {
    int e = i;
    while (e < n && buf[e] != '\n') e++;
    s = e < n ? e + 1 : e;
    if (line >= off && line < off + lim) {
      int len = e - i;
      if (obs->len > READ_MAX) {
        snprintf(m, sizeof(m), "[stopped at line %d: output limit; use offset]\n", line);
        s_add(obs, m);
        off = -1;
        break;
      }
      if (len && buf[i + len - 1] == '\r') len--;
      snprintf(m, sizeof(m), "%6d\t", line);
      s_add(obs, m);
      cp850_to_utf8(obs, buf + i, len);
      s_addc(obs, '\n');
      shown++;
    }
    line++;
    if (e >= n) break;
  }
  k_free(buf);
  if (!n) s_add(obs, "(empty file)\n");
  snprintf(m, sizeof(m), "(%d lines shown; the file has %d bytes)", shown, n);
  s_add(obs, m);
  return 0;
}

/* Troca \n por \r\n (arquivo com CRLF). */
static void to_crlf(str_t* s) {
  str_t t = {0, 0, 0};
  int i;
  for (i = 0; i < s->len; i++) {
    if (s->p[i] == '\n' && (i == 0 || s->p[i - 1] != '\r')) s_addc(&t, '\r');
    s_addc(&t, s->p[i]);
  }
  s_free(s);
  *s = t;
}

static int t_edit(const char* path, const char* in, const char* e, str_t* obs) {
  str_t ou = {0, 0, 0}, nu = {0, 0, 0}, o = {0, 0, 0}, nw = {0, 0, 0}, res = {0, 0, 0};
  int all = jtrue(in, e, "replace_all");
  int size = k_fsize(path), n, cnt = 0, i, rc = -1;
  char* buf = 0;
  char m[128];
  if (!jstr(in, e, "old_string", &ou) || !jstr(in, e, "new_string", &nu)) {
    s_add(obs, "old_string and new_string are required.");
    goto out_;
  }
  if (!ou.len) {
    s_add(obs, "old_string is empty; use write_file to create a file.");
    goto out_;
  }
  if (size < 0) {
    s_add(obs, "File not found.");
    goto out_;
  }
  buf = (char*)k_malloc((unsigned)size + 1);
  if (!buf) {
    s_add(obs, "Out of memory.");
    goto out_;
  }
  n = size ? k_fload(path, buf, size + 1) : 0; /* max inclui o NUL */
  if (n < 0) {
    s_add(obs, "Could not read the file.");
    goto out_;
  }
  buf[n] = 0;
  utf8_to_cp850(&o, ou.p, ou.len);
  utf8_to_cp850(&nw, nu.p, nu.len);
  if (!nw.p) s_add(&nw, "");
  if (memchr(buf, '\r', (size_t)n)) {
    to_crlf(&o);
    to_crlf(&nw);
    if (!nw.p) s_add(&nw, "");
  }
  for (i = 0; i + o.len <= n;) {
    if (!memcmp(buf + i, o.p, (size_t)o.len)) {
      cnt++;
      i += o.len;
    } else {
      i++;
    }
  }
  if (!cnt) {
    s_add(obs, "old_string was not found. Read the file again and copy the text exactly "
               "(spaces and tabs count).");
    goto out_;
  }
  if (cnt > 1 && !all) {
    snprintf(m, sizeof(m), "old_string occurs %d times; add more context to make it unique "
                           "or set replace_all.", cnt);
    s_add(obs, m);
    goto out_;
  }
  for (i = 0; i < n;) {
    if (i + o.len <= n && !memcmp(buf + i, o.p, (size_t)o.len) && (all || cnt)) {
      s_addn(&res, nw.p, nw.len);
      i += o.len;
      if (!all) cnt = 0;
    } else {
      s_addc(&res, buf[i++]);
    }
  }
  if (!all) cnt = 1;
  rc = k_fsave(path, res.p ? res.p : "", res.len);
  if (rc < 0) {
    s_add(obs, "Could not write the file.");
  } else {
    snprintf(m, sizeof(m), "Replaced %d occurrence(s); the file now has %d bytes.", cnt, res.len);
    s_add(obs, m);
    rc = 0;
  }
out_:
  if (buf) k_free(buf);
  s_free(&ou);
  s_free(&nu);
  s_free(&o);
  s_free(&nw);
  s_free(&res);
  return rc;
}

/* Executa um tool_use. Devolve 1 se deu erro (is_error). */
static int run_tool(blk_t* b, int truncated, str_t* obs) {
  const char* in = b->text.p ? b->text.p : "{}";
  const char* e = in + (b->text.p ? b->text.len : 2);
  const char* nm = b->name.p ? b->name.p : "";
  str_t path = {0, 0, 0}, why = {0, 0, 0};
  char m[200];
  int err = 0;
  if (truncated || !json_valid(in, e)) {
    s_add(obs, truncated ? "The reply hit max_tokens, so this tool input was cut. Write "
                           "big files in smaller pieces (write_file, then edit_file)."
                         : "The tool input was not valid JSON.");
    out("\n[entrada da ferramenta incompleta]\n");
    return 1;
  }
  if (!strcmp(nm, "run_command")) {
    str_t cu = {0, 0, 0}, arg = {0, 0, 0};
    jstr(in, e, "command", &cu);
    utf8_to_cp850(&arg, cu.p ? cu.p : "", cu.len);
    out("\n> ");
    if (arg.len) k_write(arg.p, arg.len);
    out("\n");
    if (confirm(&why)) run_action('c', &arg, &path, obs);
    else {
      s_addn(obs, why.p, why.len);
      err = 1;
    }
    if (!obs->len) s_add(obs, "(no output)");
    s_free(&cu);
    s_free(&arg);
  } else if (!strcmp(nm, "read_file")) {
    if (!tool_path(in, e, &path)) {
      s_add(obs, "path is required.");
      err = 1;
    } else {
      snprintf(m, sizeof(m), "\n> ler %s\n", path.p);
      out(m);
      err = t_read(path.p, jnum(in, e, "offset", 1), jnum(in, e, "limit", 2000), obs) < 0;
    }
  } else if (!strcmp(nm, "write_file")) {
    str_t cu = {0, 0, 0}, body = {0, 0, 0};
    if (!tool_path(in, e, &path) || !jstr(in, e, "content", &cu)) {
      s_add(obs, "path and content are required.");
      err = 1;
    } else {
      utf8_to_cp850(&body, cu.p ? cu.p : "", cu.len);
      snprintf(m, sizeof(m), "\n> gravar %s (%d bytes)\n", path.p, body.len);
      out(m);
      if (confirm(&why)) {
        int rc = k_fsave(path.p, body.p ? body.p : "", body.len);
        snprintf(m, sizeof(m), "%s: %d bytes written.", rc < 0 ? "Failed" : "OK", body.len);
        s_add(obs, m);
        err = rc < 0;
      } else {
        s_addn(obs, why.p, why.len);
        err = 1;
      }
    }
    s_free(&cu);
    s_free(&body);
  } else if (!strcmp(nm, "edit_file")) {
    if (!tool_path(in, e, &path)) {
      s_add(obs, "path is required.");
      err = 1;
    } else {
      snprintf(m, sizeof(m), "\n> editar %s\n", path.p);
      out(m);
      if (confirm(&why)) {
        err = t_edit(path.p, in, e, obs) < 0;
        out(err ? "[edicao falhou]\n" : "[editado]\n");
      } else {
        s_addn(obs, why.p, why.len);
        err = 1;
      }
    }
  } else {
    s_add(obs, "Unknown tool.");
    err = 1;
  }
  s_free(&path);
  s_free(&why);
  return err;
}

static int wait_ms(unsigned ms) {
  unsigned t = k_ms();
  while (k_ms() - t < ms) {
    if (k_trygetc() == 27) return -1;
    k_yield();
  }
  return 0;
}

static void run_task_anth(void) {
  str_t json = {0, 0, 0};
  int steps = cfg_steps > 0 ? cfg_steps : 100;
  int step;
  for (step = 0; step < steps; step++) {
    resp_t r;
    str_t ab = {0, 0, 0}, res = {0, 0, 0};
    int i, lastf = -1, ntool = 0, tries = 0, rc;
    char m[160];
    build_json_anth(&json);
    for (;;) {
      blk_reset();
      out("\nclaude: ");
      rc = api_post(&json, &r);
      if (rc == 0 || tries >= 3 || !(r.status == 429 || r.status >= 500)) break;
      resp_free(&r);
      snprintf(m, sizeof(m), "[tentando de novo em %d s; ESC cancela]", 10 * (tries + 1));
      out(m);
      if (wait_ms(10000u * (unsigned)(tries + 1)) < 0) break;
      tries++;
    }
    if (rc < 0) {
      resp_free(&r);
      break;
    }
    if (r.in_tok || r.out_tok) {
      snprintf(m, sizeof(m), "[tokens: entrada %ld + cache %ld lido/%ld gravado, saida %ld]\n",
               r.in_tok, r.cache_rd, r.cache_wr, r.out_tok);
      out(m);
    }
    for (i = 0; i < nblk; i++)
      if (blk[i].kind == 'f') lastf = i;
    if (!strcmp(r.stop, "refusal")) {
      /* a parte ja escrita e descartada (a API pede isso) */
      add_text_block(&ab, "(reply withheld)");
      anth_push("assistant", &ab);
      out("[o modelo recusou continuar este pedido]\n");
      s_free(&ab);
      resp_free(&r);
      break;
    }
    for (i = 0; i < nblk; i++) {
      blk_t* b = &blk[i];
      if (i < lastf && b->kind != 't' && b->kind != 'f') continue;
      if (b->kind == 't' && !b->text.len) continue;
      if (!b->kind) continue;
      if (ab.len) s_addc(&ab, ',');
      if (b->kind == 't') {
        s_add(&ab, "{\"type\":\"text\",\"text\":");
        json_str(&ab, b->text.p);
        s_addc(&ab, '}');
      } else if (b->kind == 'k') {
        s_add(&ab, "{\"type\":\"thinking\",\"thinking\":");
        json_str(&ab, b->text.p ? b->text.p : "");
        s_add(&ab, ",\"signature\":");
        json_str(&ab, b->sig.p ? b->sig.p : "");
        s_addc(&ab, '}');
      } else if (b->kind == 'u') {
        s_add(&ab, "{\"type\":\"tool_use\",\"id\":");
        json_str(&ab, b->id.p ? b->id.p : "");
        s_add(&ab, ",\"name\":");
        json_str(&ab, b->name.p ? b->name.p : "");
        s_add(&ab, ",\"input\":");
        if (b->text.p && json_valid(b->text.p, b->text.p + b->text.len))
          s_addn(&ab, b->text.p, b->text.len);
        else
          s_add(&ab, "{}");
        s_addc(&ab, '}');
        ntool++;
      } else {
        s_addn(&ab, b->raw.p, b->raw.len);
      }
    }
    if (!ab.len) add_text_block(&ab, "(empty reply)");
    anth_push("assistant", &ab);
    s_free(&ab);
    if (!ntool) {
      if (!strcmp(r.stop, "max_tokens")) out("[resposta cortada em max_tokens]\n");
      resp_free(&r);
      break;
    }
    for (i = lastf + 1; i < nblk; i++) {
      str_t obs = {0, 0, 0};
      int err;
      if (blk[i].kind != 'u') continue;
      err = run_tool(&blk[i], !strcmp(r.stop, "max_tokens") && i == nblk - 1, &obs);
      if (res.len) s_addc(&res, ',');
      s_add(&res, "{\"type\":\"tool_result\",\"tool_use_id\":");
      json_str(&res, blk[i].id.p ? blk[i].id.p : "");
      s_add(&res, ",\"content\":");
      json_str(&res, obs.p ? obs.p : "(no output)");
      if (err) s_add(&res, ",\"is_error\":true");
      s_addc(&res, '}');
      s_free(&obs);
    }
    resp_free(&r);
    anth_push("user", &res);
    s_free(&res);
  }
  if (step == steps) out("[limite de passos neste pedido (steps= no AGENT.CFG); continue com outra mensagem]\n");
  blk_reset();
  s_free(&json);
}

static void ask_anth(const char* text) {
  str_t u = {0, 0, 0}, b = {0, 0, 0};
  cp850_to_utf8(&u, text, (int)strlen(text));
  add_text_block(&b, u.p ? u.p : "");
  anth_push("user", &b);
  s_free(&u);
  s_free(&b);
  run_task_anth();
}

static void ask(const char* text) {
  str_t u = {0, 0, 0};
  if (cfg_anth) {
    ask_anth(text);
    return;
  }
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
        "  anthropic=sk-ant-...   (Claude; ou model=claude-sonnet-5-5)\n"
        "  key=sk-...   (ou deepseek=sk-...)\n"
        "  url=https://api.deepseek.com/chat/completions\n"
        "  model=deepseek-chat\n");
    return 1;
  }
  if (model) snprintf(cfg_model, sizeof(cfg_model), "%s", model); /* -m ganha do AGENT.CFG */
  if (!k_net_ready()) out("[aviso: rede nao esta pronta; use net antes]\n");
  if (cfg_anth) {
    obs_max = 30000;
    if (!cfg_key[0]) out("[aviso: falta a chave (anthropic=sk-ant-...) no AGENT.CFG]\n");
    build_system_anth();
  } else {
    build_system();
  }
  snprintf(line, sizeof(line), "AGENT 0.2 - %s em %s (%s)\n", cfg_model, cfg_url, cfg_path);
  out(line);
  out(cfg_anth ? "Escreva o pedido. ESC interrompe a resposta, /novo limpa a conversa, /sair termina.\n"
               : "Escreva o pedido. /novo limpa a conversa, /sair termina.\n");
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
      if (cfg_anth) anth_reset();
      else msgs_reset();
      out("[conversa limpa]\n");
      continue;
    }
    ask(line);
  }
  return 0;
}
