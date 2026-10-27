/* http.c - HTTP/1.1 GET por TCP (http) ou TLS.DLL (https): redirecionamentos,
 * Content-Length e Transfer-Encoding: chunked. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <so.h>
#include <tls.h>
#include "browser.h"
#include "cache.h"

#define BODY_MAX (4 * 1024 * 1024)

static int c_send(int tls, const char* d, int n) { return tls ? tls_send(d, n) : k_tcp_send(d, n); }
static int c_recv(int tls, char* d, int cap) { return tls ? tls_recv(d, cap) : k_tcp_recv(d, cap); }
static void c_close(int tls) {
  if (tls) tls_close();
  else k_tcp_close();
}

static int ci_eq_n(const char* a, const char* b, int n) {
  int i;
  for (i = 0; i < n; i++) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    if (!a[i]) return 1;
  }
  return 1;
}

/* valor do cabecalho 'name' (sem ':'); copia em out */
static int header(const char* hdr, int hlen, const char* name, char* out, int max) {
  int nl = (int)strlen(name);
  int i = 0;
  while (i < hlen) {
    int e = i;
    while (e < hlen && hdr[e] != '\n') e++;
    if (e - i > nl && hdr[i + nl] == ':' && ci_eq_n(hdr + i, name, nl)) {
      int s = i + nl + 1;
      int t = e;
      while (s < t && (hdr[s] == ' ' || hdr[s] == '\t')) s++;
      while (t > s && (hdr[t - 1] == '\r' || hdr[t - 1] == ' ')) t--;
      if (t - s > max - 1) t = s + max - 1;
      memcpy(out, hdr + s, (size_t)(t - s));
      out[t - s] = 0;
      return 1;
    }
    i = e + 1;
  }
  out[0] = 0;
  return 0;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = tolower(c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

/* 1 se o corpo chunked em b[0..n) esta completo; *out_len = tamanho decodificado.
 * Se dst != 0 copia os dados. */
static int dechunk(const char* b, int n, char* dst, int* out_len) {
  int p = 0, total = 0;
  for (;;) {
    int size = 0, digits = 0, v;
    while (p < n && (v = hexval((unsigned char)b[p])) >= 0) {
      size = size * 16 + v;
      p++;
      digits++;
      if (size > BODY_MAX) return -1;
    }
    while (p < n && b[p] != '\n') p++;
    if (p >= n || !digits) return 0;
    p++;
    if (size == 0) {
      *out_len = total;
      return 1;
    }
    if (p + size + 2 > n) return 0;
    if (dst) memcpy(dst + total, b + p, (size_t)size);
    total += size;
    p += size;
    if (b[p] == '\r') p++;
    if (p < n && b[p] == '\n') p++;
  }
}

/* 1 se o usuario interrompeu: fecha a conexao e marca o pedido. */
static int user_stop(progress_fn progress, http_resp_t* r, int tls, const char* msg) {
  if (progress && progress(msg)) {
    c_close(tls);
    r->aborted = 1;
    return 1;
  }
  return 0;
}

static int stopped(http_resp_t* r, int tls) {
  if (tls && strstr(tls_error(), "interrompido")) {
    c_close(tls);
    r->aborted = 1;
    return 1;
  }
  return 0;
}

/* Conexao guardada para o proximo pedido ao mesmo servidor. Abrir outra custa
 * a troca de chaves do TLS, segundos no WIN, e a grade de imagens pede dezenas
 * de miniaturas ao mesmo host. O nucleo tem uma conexao TCP so, dividida por
 * todos os programas: por isso ela so fica aberta enquanto a pagina carrega
 * (main.c chama http_idle_close no fim), e o pedido que falha nela tenta de
 * novo numa conexao nova. */
static int ka_open, ka_tls, ka_port;
static char ka_host[256];

void http_idle_close(void) {
  if (ka_open) c_close(ka_tls);
  ka_open = 0;
}

/* 1 se a resposta deixa a conexao aberta para o proximo pedido */
static int keeps_alive(const char* buf, int hdr_end) {
  char v[64];
  int i, has = header(buf, hdr_end, "Connection", v, (int)sizeof(v));
  for (i = 0; v[i]; i++) v[i] = (char)tolower((unsigned char)v[i]);
  if (has && strstr(v, "close")) return 0;
  if (strncmp(buf, "HTTP/1.0", 8) == 0) return has && strstr(v, "keep-alive") != 0;
  return 1;
}

static int fetch_once(const url_t* u, http_resp_t* r, char* location, progress_fn progress) {
  char req[URL_MAX + 512];
  char msg[160];
  char* buf = 0;
  int len = 0, cap = 0, hdr_end = -1, clen = -1, chunked = 0, done = 0;
  int tls = strcmp(u->scheme, "https") == 0;
  int last_report = 0;
  int reuse = ka_open && ka_tls == tls && ka_port == u->port && strcmp(ka_host, u->host) == 0;
  int stale = 1;
  unsigned t0 = k_ms();

  if (!reuse) http_idle_close();
  ka_open = 0; /* em uso; volta a ficar guardada se a resposta deixar */
  snprintf(req, sizeof(req),
           "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SO-Navegador/1.0\r\n"
           "Accept: text/html,text/plain;q=0.9,*/*;q=0.5\r\nAccept-Encoding: identity\r\n\r\n",
           u->path, u->host);
  /* A segunda volta so acontece se a conexao guardada ja tinha caido. */
  while (stale) {
    stale = 0;
    if (!reuse) {
      snprintf(msg, sizeof(msg), "5%% conectando em %s...", u->host);
      if (user_stop(progress, r, tls, msg)) return -1;
      if (tls) {
        if (tls_connect(u->host, u->port) != 0) {
          if (stopped(r, tls)) return -1;
          snprintf(r->err, sizeof(r->err), "TLS com %s falhou: %s", u->host, tls_error());
          return -1;
        }
      } else {
        int c = k_tcp_connect(u->host, u->port);
        if (c == -3) {
          k_tcp_close();
          r->aborted = 1;
          return -1;
        }
        if (c != 0) {
          snprintf(r->err, sizeof(r->err), "nao conectou em %s:%d (DNS/TCP)", u->host, u->port);
          return -1;
        }
      }
    }
    r->tls = tls;
    {
      int sn = c_send(tls, req, (int)strlen(req));
      if (sn == -3 || (sn < 0 && stopped(r, tls))) {
        if (!r->aborted) {
          c_close(tls);
          r->aborted = 1;
        }
        return -1;
      }
      if (sn < 0 && reuse) {
        c_close(tls);
        reuse = 0;
        stale = 1;
        continue;
      }
      if (sn < 0) {
        snprintf(r->err, sizeof(r->err), "falha ao enviar o pedido");
        c_close(tls);
        return -1;
      }
    }
    if (user_stop(progress, r, tls, "15% esperando resposta...")) return -1;
    while (!done) {
      int n;
      if (cap - len < 8192) {
        int nc = cap ? cap * 2 : 65536;
        char* nb;
        if (nc > BODY_MAX + 65536) nc = BODY_MAX + 65536;
        if (nc - len < 2) break;
        nb = (char*)realloc(buf, (size_t)nc);
        if (!nb) break;
        buf = nb;
        cap = nc;
      }
      n = c_recv(tls, buf + len, cap - len - 1);
      if (n == -3 || (n < 0 && stopped(r, tls))) {
        c_close(tls);
        free(buf);
        r->aborted = 1;
        return -1;
      }
      if (n <= 0) {
        if (reuse && len == 0) {
          /* o servidor ja tinha fechado a conexao guardada */
          c_close(tls);
          free(buf);
          buf = 0;
          cap = 0;
          reuse = 0;
          stale = 1;
        }
        break;
      }
      len += n;
      buf[len] = 0;
      if (hdr_end < 0) {
        char* e = strstr(buf, "\r\n\r\n");
        if (e) {
          char v[64];
          const char* sp = strchr(buf, ' ');
          int st = sp ? atoi(sp + 1) : 0;
          hdr_end = (int)(e - buf) + 4;
          if (header(buf, hdr_end, "Content-Length", v, (int)sizeof(v))) clen = atoi(v);
          if (header(buf, hdr_end, "Transfer-Encoding", v, (int)sizeof(v)) && strstr(v, "chunked"))
            chunked = 1;
          /* sem corpo: com a conexao aberta, esperar o fim seria esperar o prazo */
          if (st == 204 || st == 304) done = 1;
        }
      }
      if (hdr_end >= 0) {
        int body = len - hdr_end;
        int dl;
        if (clen >= 0 && body >= clen) done = 1;
        if (chunked && dechunk(buf + hdr_end, body, 0, &dl) != 0) done = 1;
        if (body > last_report && (last_report == 0 || body - last_report >= 8192)) {
          int pct;
          last_report = body;
          pct = 15;
          if (clen > 0) pct = 15 + body * 75 / clen;
          else pct = 15 + body / 8192;
          if (pct < 15) pct = 15;
          if (pct > 95) pct = 95;
          snprintf(msg, sizeof(msg), "%d%% recebendo %s", pct, u->host);
          if (user_stop(progress, r, tls, msg)) {
            free(buf);
            return -1;
          }
        }
      }
    }
  }
  if (done && keeps_alive(buf, hdr_end)) {
    ka_open = 1;
    ka_tls = tls;
    ka_port = u->port;
    strncpy(ka_host, u->host, sizeof(ka_host) - 1);
    ka_host[sizeof(ka_host) - 1] = 0;
  } else {
    c_close(tls);
  }
  if (tls) {
    r->tls_ok = tls_cert_ok();
    strncpy(r->secure, tls_security(), sizeof(r->secure) - 1);
    r->secure[sizeof(r->secure) - 1] = 0;
  }
  if (!buf || hdr_end < 0) {
    free(buf);
    snprintf(r->err, sizeof(r->err), "resposta HTTP incompleta de %s", u->host);
    return -1;
  }
  if (strncmp(buf, "HTTP/", 5) != 0) {
    free(buf);
    snprintf(r->err, sizeof(r->err), "%s nao respondeu HTTP", u->host);
    return -1;
  }
  {
    const char* sp = strchr(buf, ' ');
    r->status = sp ? atoi(sp + 1) : 0;
  }
  header(buf, hdr_end, "Content-Type", r->content_type, (int)sizeof(r->content_type));
  header(buf, hdr_end, "Location", location, URL_MAX);
  {
    int body = len - hdr_end;
    int out_len = body;
    char* data;
    if (chunked) {
      if (dechunk(buf + hdr_end, body, 0, &out_len) != 1) out_len = -1;
    } else if (clen >= 0 && clen < body) {
      out_len = clen;
    }
    if (out_len < 0) {
      /* chunked cortado: usa o que veio, sem os tamanhos */
      out_len = body;
      chunked = 0;
    }
    data = (char*)malloc((size_t)out_len + 1);
    if (!data) {
      free(buf);
      snprintf(r->err, sizeof(r->err), "sem memoria para %d bytes", out_len);
      return -1;
    }
    if (chunked) dechunk(buf + hdr_end, body, data, &out_len);
    else memcpy(data, buf + hdr_end, (size_t)out_len);
    data[out_len] = 0;
    r->body = data;
    r->body_len = out_len;
  }
  free(buf);
  snprintf(msg, sizeof(msg), "100%% %d bytes em %u ms", r->body_len, k_ms() - t0);
  progress(msg);
  return 0;
}

int http_get(const char* url, http_resp_t* r, progress_fn progress, int flags) {
  char cur[URL_MAX];
  char location[URL_MAX];
  int hops;
  memset(r, 0, sizeof(*r));
  if (!(flags & HTTP_FRESH) &&
      cache_load(url, &r->status, &r->body, &r->body_len, r->content_type, (int)sizeof(r->content_type),
                 r->url, URL_MAX, &r->tls, &r->tls_ok) == 0) {
    char msg[80];
    r->from_cache = 1;
    if (!r->url[0]) {
      strncpy(r->url, url, URL_MAX - 1);
      r->url[URL_MAX - 1] = 0;
    }
    if (r->tls) strncpy(r->secure, "HTTPS", sizeof(r->secure) - 1);
    snprintf(msg, sizeof(msg), "100%% cache D:\\temp\\net");
    progress(msg);
    return 0;
  }
  memset(r, 0, sizeof(*r));
  strncpy(cur, url, URL_MAX - 1);
  cur[URL_MAX - 1] = 0;
  for (hops = 0; hops < 8; hops++) {
    url_t u;
    if (url_parse(cur, &u) != 0 || (strcmp(u.scheme, "http") != 0 && strcmp(u.scheme, "https") != 0)) {
      snprintf(r->err, sizeof(r->err), "endereco invalido: %s", cur);
      return -1;
    }
    url_format(&u, r->url, URL_MAX);
    location[0] = 0;
    if (fetch_once(&u, r, location, progress) != 0) return -1;
    if (r->aborted) return -1;
    if (r->status >= 300 && r->status < 400 && r->status != 304 && location[0]) {
      char next[URL_MAX];
      if (url_resolve(r->url, location, next, URL_MAX) != 0) return 0;
      http_free(r);
      strcpy(cur, next);
      continue;
    }
    /* So 200: o 202 do desafio anti-robo do DuckDuckGo nao pode ficar no cache.
     * Figuras tambem nao: no WIN o FAT cede a CPU a cada 4 setores, e gravar
     * uma miniatura levava segundos, contra 0,1 s para baixar de novo. */
    if (r->status == 200 && r->body && !(flags & HTTP_NOSTORE) &&
        strncmp(r->content_type, "image/", 6) != 0) {
      if (cache_store(url, r->url[0] ? r->url : url, r->status, r->content_type, r->body, r->body_len,
                      r->tls, r->tls_ok) == 0)
        r->cache_saved = 1;
      if (r->url[0] && strcmp(r->url, url) != 0)
        cache_store(r->url, r->url, r->status, r->content_type, r->body, r->body_len, r->tls, r->tls_ok);
    }
    return 0;
  }
  snprintf(r->err, sizeof(r->err), "redirecionamentos demais");
  return -1;
}

void http_free(http_resp_t* r) {
  free(r->body);
  r->body = 0;
  r->body_len = 0;
}
