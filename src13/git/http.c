/* http.c - pedidos HTTP/HTTPS do git (GET de info/refs, POST de upload-pack).
 * Le a resposta inteira na memoria; segue ate 5 redirecionamentos. */
#include "git.h"

#define BODY_MAX (96 * 1024 * 1024)

struct url {
  char host[128];
  char path[512];
  int port;
  int tls;
};

static int low(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ci_start(const char* s, const char* p) {
  while (*p)
    if (low((unsigned char)*s++) != low((unsigned char)*p++)) return 0;
  return 1;
}

static int url_parse(const char* s, struct url* u) {
  const char* p;
  int n = 0;
  memset(u, 0, sizeof(*u));
  if (ci_start(s, "https://")) {
    u->tls = 1;
    u->port = 443;
    p = s + 8;
  } else if (ci_start(s, "http://")) {
    u->port = 80;
    p = s + 7;
  } else {
    return -1;
  }
  while (*p && *p != '/' && *p != ':' && n < (int)sizeof(u->host) - 1) u->host[n++] = (char)low((unsigned char)*p++);
  if (!n) return -1;
  if (*p == ':') {
    u->port = atoi(p + 1);
    while (*p && *p != '/') p++;
  }
  strncpy(u->path, *p ? p : "/", sizeof(u->path) - 1);
  return 0;
}

static int header(const char* h, int hlen, const char* name, char* out, int max) {
  int nl = (int)strlen(name), i = 0;
  while (i < hlen) {
    int e = i, k;
    while (e < hlen && h[e] != '\n') e++;
    if (e - i > nl && h[i + nl] == ':') {
      for (k = 0; k < nl && low((unsigned char)h[i + k]) == low((unsigned char)name[k]); k++) {}
      if (k == nl) {
        int s = i + nl + 1, t = e;
        while (s < t && h[s] == ' ') s++;
        while (t > s && (h[t - 1] == '\r' || h[t - 1] == ' ')) t--;
        if (t - s > max - 1) t = s + max - 1;
        memcpy(out, h + s, (size_t)(t - s));
        out[t - s] = 0;
        return 1;
      }
    }
    i = e + 1;
  }
  out[0] = 0;
  return 0;
}

static int hexv(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = low(c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

/* Tira os pedacos chunked no proprio buffer. Tamanho final ou -1. */
static int dechunk(char* b, int n) {
  int p = 0, o = 0;
  for (;;) {
    int size = 0, d = 0, v;
    while (p < n && (v = hexv((unsigned char)b[p])) >= 0) {
      size = size * 16 + v;
      p++;
      d++;
    }
    while (p < n && b[p] != '\n') p++;
    if (p >= n || !d) return -1;
    p++;
    if (size == 0) return o;
    if (p + size > n) return -1;
    memmove(b + o, b + p, (size_t)size);
    o += size;
    p += size;
    if (p < n && b[p] == '\r') p++;
    if (p < n && b[p] == '\n') p++;
  }
}

static int c_send(int tls, const char* d, int n) { return tls ? tls_send(d, n) : k_tcp_send(d, n); }
static int c_recv(int tls, char* d, int cap) { return tls ? tls_recv(d, cap) : k_tcp_recv(d, cap); }
static void c_close(int tls) {
  if (tls) tls_close();
  else k_tcp_close();
}

static int once(const struct url* u, const char* ctype, const char* body, int blen, char** out, int* olen,
                char* location, char* err) {
  char req[1024];
  char* buf = 0;
  int len = 0, cap = 0, hdr_end = -1, clen = -1, chunked = 0, status = 0, shown = 0;
  int rn;
  if (body)
    rn = sprintf(req,
                 "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: git/2.43.0 (SODOS)\r\n"
                 "Accept: */*\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                 u->path, u->host, ctype, blen);
  else
    rn = sprintf(req,
                 "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: git/2.43.0 (SODOS)\r\n"
                 "Accept: */*\r\nConnection: close\r\n\r\n",
                 u->path, u->host);
  if (u->tls) {
    if (tls_connect(u->host, u->port) != 0) {
      sprintf(err, "TLS falhou: %s", tls_error());
      return -1;
    }
  } else if (k_tcp_connect(u->host, u->port) != 0) {
    sprintf(err, "nao conectou em %s", u->host);
    return -1;
  }
  if (c_send(u->tls, req, rn) < 0 || (body && blen > 0 && c_send(u->tls, body, blen) < 0)) {
    sprintf(err, "falha ao enviar o pedido");
    c_close(u->tls);
    return -1;
  }
  for (;;) {
    int n;
    if (cap - len < 16384) {
      int nc = cap ? cap * 2 : 65536;
      char* nb;
      if (nc > BODY_MAX) nc = BODY_MAX;
      if (nc - len < 2) {
        sprintf(err, "resposta grande demais");
        c_close(u->tls);
        free(buf);
        return -1;
      }
      nb = (char*)realloc(buf, (size_t)nc);
      if (!nb) {
        sprintf(err, "sem memoria (%d KB)", nc / 1024);
        c_close(u->tls);
        free(buf);
        return -1;
      }
      buf = nb;
      cap = nc;
    }
    n = c_recv(u->tls, buf + len, cap - len - 1);
    if (n < 0) {
      sprintf(err, "falha ao receber");
      c_close(u->tls);
      free(buf);
      return -1;
    }
    if (n == 0) break;
    len += n;
    buf[len] = 0;
    if (hdr_end < 0) {
      char* e = strstr(buf, "\r\n\r\n");
      if (e) {
        char v[64];
        char* sp = strchr(buf, ' ');
        hdr_end = (int)(e - buf) + 4;
        status = sp ? atoi(sp + 1) : 0;
        if (header(buf, hdr_end, "Content-Length", v, sizeof(v))) clen = atoi(v);
        if (header(buf, hdr_end, "Transfer-Encoding", v, sizeof(v)) && strstr(v, "chunked")) chunked = 1;
        header(buf, hdr_end, "Location", location, 512);
      }
    }
    if (hdr_end >= 0) {
      int got = len - hdr_end;
      if (clen >= 0 && got >= clen) break;
      if (chunked && got >= 5 && memcmp(buf + len - 5, "0\r\n\r\n", 5) == 0) break;
      if (got - shown >= 256 * 1024) {
        shown = got;
        printf("\r  recebidos %d KB", got / 1024);
      }
    }
  }
  c_close(u->tls);
  if (shown) printf("\r  recebidos %d KB\n", (len - hdr_end) / 1024);
  if (hdr_end < 0) {
    free(buf);
    sprintf(err, "resposta HTTP incompleta");
    return -1;
  }
  len -= hdr_end;
  memmove(buf, buf + hdr_end, (size_t)len);
  if (chunked) {
    len = dechunk(buf, len);
    if (len < 0) {
      free(buf);
      sprintf(err, "resposta cortada");
      return -1;
    }
  } else if (clen >= 0 && len > clen) {
    len = clen;
  }
  buf[len] = 0;
  *out = buf;
  *olen = len;
  return status;
}

int http_req(const char* url, const char* ctype, const char* body, int blen, char** out, int* olen,
             char* err) {
  struct url u;
  char location[512];
  int hop;
  *out = 0;
  *olen = 0;
  if (url_parse(url, &u) != 0) {
    sprintf(err, "url invalida: %s", url);
    return -1;
  }
  for (hop = 0; hop < 5; hop++) {
    int st;
    location[0] = 0;
    st = once(&u, ctype, body, blen, out, olen, location, err);
    if (st < 0) return -1;
    if (st >= 300 && st < 400 && location[0]) {
      free(*out);
      *out = 0;
      if (location[0] == '/') {
        strncpy(u.path, location, sizeof(u.path) - 1);
      } else if (url_parse(location, &u) != 0) {
        sprintf(err, "redirecionamento invalido");
        return -1;
      }
      continue;
    }
    return st;
  }
  sprintf(err, "redirecionamentos demais");
  return -1;
}
