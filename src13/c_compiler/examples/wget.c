/* wget.c - baixa um arquivo por HTTP ou HTTPS.
 *   CC WGET.C TLS.DLL
 *   WGET http://exemplo.com/leia.txt
 *   WGET https://exemplo.com/a.bin D:A.BIN
 *   WGET -O SAIDA.TXT http://exemplo.com/a
 * Grava no maximo 4 MB. Segue ate 5 redirecionamentos. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include <tls.h>

#define URL_MAX 512
#define BODY_MAX (4 * 1024 * 1024)

static int ci_start(const char* s, const char* p) {
  while (*p) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) return 0;
    s++;
    p++;
  }
  return 1;
}

static int ci_eq_n(const char* a, const char* b, int n) {
  int i;
  for (i = 0; i < n; i++) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
  }
  return 1;
}

static void copy_n(char* dst, int max, const char* src, int n) {
  if (n > max - 1) n = max - 1;
  if (n < 0) n = 0;
  memcpy(dst, src, (size_t)n);
  dst[n] = 0;
}

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
      copy_n(out, max, hdr + s, t - s);
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

/* 1 = corpo chunked completo, 0 = ainda falta, -1 = grande demais. */
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
    if (total + size > BODY_MAX) return -1;
    if (dst) memcpy(dst + total, b + p, (size_t)size);
    total += size;
    p += size;
    if (b[p] == '\r') p++;
    if (p < n && b[p] == '\n') p++;
  }
}

struct url {
  char scheme[8];
  char host[128];
  char path[URL_MAX];
  int port;
  int tls;
};

static int url_parse(const char* s, struct url* u) {
  const char* p;
  const char* h;
  int n;
  memset(u, 0, sizeof(*u));
  while (*s == ' ') s++;
  n = (int)strlen(s);
  while (n > 0 && s[n - 1] == ' ') n--;
  if (ci_start(s, "https://")) {
    strcpy(u->scheme, "https");
    u->port = 443;
    u->tls = 1;
    p = s + 8;
  } else if (ci_start(s, "http://")) {
    strcpy(u->scheme, "http");
    u->port = 80;
    p = s + 7;
  } else {
    return -1;
  }
  h = p;
  while (p < s + n && *p != '/' && *p != ':' && *p != '?' && *p != '#') p++;
  copy_n(u->host, (int)sizeof(u->host), h, (int)(p - h));
  for (h = u->host; *h; h++) {
    if (!isalnum((unsigned char)*h) && *h != '.' && *h != '-' && *h != '_') return -1;
  }
  {
    char* q;
    for (q = u->host; *q; q++) *q = (char)tolower((unsigned char)*q);
  }
  if (!u->host[0]) return -1;
  if (p < s + n && *p == ':') {
    p++;
    u->port = 0;
    while (p < s + n && isdigit((unsigned char)*p)) u->port = u->port * 10 + (*p++ - '0');
    if (u->port <= 0 || u->port > 65535) return -1;
  }
  if (p < s + n && *p != '#') {
    const char* e = p;
    while (e < s + n && *e != '#') e++;
    if (*p == '?') {
      u->path[0] = '/';
      copy_n(u->path + 1, URL_MAX - 1, p, (int)(e - p));
    } else {
      copy_n(u->path, URL_MAX, p, (int)(e - p));
    }
  } else {
    strcpy(u->path, "/");
  }
  if (!u->path[0]) strcpy(u->path, "/");
  return 0;
}

static void url_text(const struct url* u, char* out, int max) {
  if ((u->tls && u->port == 443) || (!u->tls && u->port == 80))
    snprintf(out, (size_t)max, "%s://%s%s", u->scheme, u->host, u->path);
  else
    snprintf(out, (size_t)max, "%s://%s:%d%s", u->scheme, u->host, u->port, u->path);
}

/* location absoluto ou relativo ao pedido atual. */
static int url_join(const struct url* base, const char* loc, struct url* out) {
  char abs[URL_MAX];
  if (ci_start(loc, "http://") || ci_start(loc, "https://"))
    return url_parse(loc, out);
  if (loc[0] == '/') {
    *out = *base;
    copy_n(out->path, URL_MAX, loc, (int)strlen(loc));
    return 0;
  }
  {
    const char* slash = strrchr(base->path, '/');
    int dir = slash ? (int)(slash - base->path + 1) : 1;
    if (dir + (int)strlen(loc) >= URL_MAX) return -1;
    memcpy(abs, base->path, (size_t)dir);
    strcpy(abs + dir, loc);
    *out = *base;
    copy_n(out->path, URL_MAX, abs, (int)strlen(abs));
  }
  return 0;
}

static int c_send(int tls, const char* d, int n) { return tls ? tls_send(d, n) : k_tcp_send(d, n); }
static int c_recv(int tls, char* d, int cap) { return tls ? tls_recv(d, cap) : k_tcp_recv(d, cap); }
static void c_close(int tls) {
  if (tls) tls_close();
  else k_tcp_close();
}

/* 0 = ok. *body e malloc (pode ser vazio). location preenchido em 3xx. */
static int fetch(const struct url* u, int* status, char** body, int* blen, char* location, char* err) {
  char req[URL_MAX + 256];
  char* buf = 0;
  int len = 0, cap = 0, hdr_end = -1, clen = -1, chunked = 0, done = 0;
  int shown = 0;
  const char* hostport = u->host;
  char hp[160];

  location[0] = 0;
  *body = 0;
  *blen = 0;
  *status = 0;
  if ((u->tls && u->port != 443) || (!u->tls && u->port != 80)) {
    snprintf(hp, sizeof(hp), "%s:%d", u->host, u->port);
    hostport = hp;
  }
  snprintf(req, sizeof(req),
           "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: SO-Wget/1.0\r\n"
           "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
           u->path, hostport);
  printf("conectando em %s:%d ...\n", u->host, u->port);
  if (u->tls) {
    if (tls_connect(u->host, u->port) != 0) {
      snprintf(err, 160, "TLS falhou: %s", tls_error());
      return -1;
    }
    if (!tls_cert_ok()) printf("aviso: %s\n", tls_security());
  } else {
    int c = k_tcp_connect(u->host, u->port);
    if (c != 0) {
      snprintf(err, 160, "nao conectou em %s:%d", u->host, u->port);
      return -1;
    }
  }
  if (c_send(u->tls, req, (int)strlen(req)) < 0) {
    snprintf(err, 160, "falha ao enviar o pedido");
    c_close(u->tls);
    return -1;
  }
  while (!done) {
    int n;
    if (cap - len < 4096) {
      int nc = cap ? cap * 2 : 8192;
      char* nb;
      if (nc > BODY_MAX + 65536) nc = BODY_MAX + 65536;
      if (nc - len < 2) {
        snprintf(err, 160, "resposta grande demais (max %d bytes)", BODY_MAX);
        c_close(u->tls);
        free(buf);
        return -1;
      }
      nb = (char*)realloc(buf, (size_t)nc);
      if (!nb) {
        snprintf(err, 160, "sem memoria");
        c_close(u->tls);
        free(buf);
        return -1;
      }
      buf = nb;
      cap = nc;
    }
    n = c_recv(u->tls, buf + len, cap - len - 1);
    if (n < 0) {
      snprintf(err, 160, "falha ao receber");
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
        const char* sp;
        hdr_end = (int)(e - buf) + 4;
        sp = strchr(buf, ' ');
        *status = sp ? atoi(sp + 1) : 0;
        if (header(buf, hdr_end, "Content-Length", v, (int)sizeof(v))) clen = atoi(v);
        if (header(buf, hdr_end, "Transfer-Encoding", v, (int)sizeof(v)) && strstr(v, "chunked"))
          chunked = 1;
        header(buf, hdr_end, "Location", location, URL_MAX);
        if (clen > BODY_MAX) {
          snprintf(err, 160, "arquivo de %d bytes passa de %d", clen, BODY_MAX);
          c_close(u->tls);
          free(buf);
          return -1;
        }
        if (*status == 204 || *status == 304) done = 1;
        if (*status >= 300 && *status < 400 && location[0]) done = 1;
      }
    }
    if (hdr_end >= 0 && !done) {
      int body_n = len - hdr_end;
      int dl = 0;
      if (clen >= 0 && body_n >= clen) done = 1;
      if (chunked && dechunk(buf + hdr_end, body_n, 0, &dl) != 0) done = 1;
      if (body_n - shown >= 16384) {
        shown = body_n;
        printf("  %d bytes\n", body_n);
      }
    }
  }
  c_close(u->tls);
  if (!buf || hdr_end < 0 || strncmp(buf, "HTTP/", 5) != 0) {
    free(buf);
    snprintf(err, 160, "resposta HTTP incompleta");
    return -1;
  }
  if (*status >= 300 && *status < 400 && location[0]) {
    free(buf);
    return 0;
  }
  {
    int body_n = len - hdr_end;
    int out_len = body_n;
    char* data;
    if (body_n < 0) body_n = 0;
    if (chunked) {
      int dc = dechunk(buf + hdr_end, body_n, 0, &out_len);
      if (dc < 0) {
        free(buf);
        snprintf(err, 160, "corpo grande demais");
        return -1;
      }
      if (dc != 1) {
        free(buf);
        snprintf(err, 160, "download cortado (chunked incompleto)");
        return -1;
      }
    } else if (clen >= 0) {
      if (body_n < clen) {
        free(buf);
        snprintf(err, 160, "download cortado (%d de %d)", body_n, clen);
        return -1;
      }
      out_len = clen;
    }
    data = (char*)malloc((size_t)out_len + 1);
    if (!data) {
      free(buf);
      snprintf(err, 160, "sem memoria para %d bytes", out_len);
      return -1;
    }
    if (chunked) dechunk(buf + hdr_end, body_n, data, &out_len);
    else if (out_len > 0) memcpy(data, buf + hdr_end, (size_t)out_len);
    data[out_len] = 0;
    *body = data;
    *blen = out_len;
  }
  free(buf);
  return 0;
}

static int fat_char(int c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
  if (c == '-' || c == '_') return c;
  return '_';
}

/* Ultimo pedaco do caminho, em 8.3. Sem nome: INDEX.HTM. */
static void name_from_path(const char* path, char* out, int max) {
  const char* base = path;
  const char* p;
  const char* dot = 0;
  char name[9];
  char ext[4];
  int ni = 0, ei = 0;
  for (p = path; *p && *p != '?' && *p != '#'; p++)
    if (*p == '/') base = p + 1;
  if (!*base || *base == '?' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
    snprintf(out, (size_t)max, "INDEX.HTM");
    return;
  }
  for (p = base; *p && *p != '?' && *p != '#'; p++)
    if (*p == '.') dot = p;
  p = base;
  if (dot) {
    while (p < dot && ni < 8) name[ni++] = (char)fat_char((unsigned char)*p++);
    p = dot + 1;
    while (*p && *p != '?' && *p != '#' && ei < 3) ext[ei++] = (char)fat_char((unsigned char)*p++);
  } else {
    while (*p && *p != '?' && *p != '#' && ni < 8) name[ni++] = (char)fat_char((unsigned char)*p++);
  }
  name[ni] = 0;
  ext[ei] = 0;
  if (!name[0]) strcpy(name, "INDEX");
  if (ext[0]) snprintf(out, (size_t)max, "%s.%s", name, ext);
  else snprintf(out, (size_t)max, "%s", name);
}

static void usage(void) {
  printf("WGET url [arquivo]\n");
  printf("  WGET http://exemplo.com/leia.txt\n");
  printf("  WGET https://exemplo.com/a.bin D:A.BIN\n");
  printf("  WGET -O SAIDA.TXT http://exemplo.com/a\n");
  printf("HTTP e HTTPS. Ate 4 MB. Ate 5 redirecionamentos.\n");
}

int main(int argc, char** argv) {
  const char* url_s = 0;
  const char* outfile = 0;
  struct url u;
  char final_name[16];
  char err[160];
  char location[URL_MAX];
  char* body = 0;
  int blen = 0, status = 0, hop;
  unsigned t0;

  if (argc < 2) {
    usage();
    return 1;
  }
  if ((argv[1][0] == '-' && (argv[1][1] == 'O' || argv[1][1] == 'o') && argv[1][2] == 0)) {
    if (argc < 4) {
      usage();
      return 1;
    }
    outfile = argv[2];
    url_s = argv[3];
  } else {
    url_s = argv[1];
    if (argc >= 3) outfile = argv[2];
  }
  if (url_parse(url_s, &u) != 0) {
    printf("url invalida (use http:// ou https://)\n");
    return 1;
  }
  if (!k_net_ready()) {
    printf("rede nao pronta\n");
    return 1;
  }
  t0 = k_ms();
  for (hop = 0; hop < 5; hop++) {
    char shown[URL_MAX];
    struct url next;
    url_text(&u, shown, URL_MAX);
    printf("GET %s\n", shown);
    err[0] = 0;
    if (fetch(&u, &status, &body, &blen, location, err) != 0) {
      printf("%s\n", err[0] ? err : "falhou");
      return 1;
    }
    if (status >= 300 && status < 400 && location[0]) {
      printf("HTTP %d -> %s\n", status, location);
      if (url_join(&u, location, &next) != 0) {
        printf("redirecionamento invalido\n");
        return 1;
      }
      u = next;
      continue;
    }
    break;
  }
  if (hop == 5) {
    printf("redirecionamentos demais\n");
    free(body);
    return 1;
  }
  if (status != 200) {
    printf("HTTP %d, nao gravado\n", status);
    free(body);
    return 1;
  }
  if (!outfile) name_from_path(u.path, final_name, (int)sizeof(final_name));
  {
    const char* dest = outfile ? outfile : final_name;
    if (k_fsave(dest, body ? body : "", blen) < 0) {
      printf("nao gravou %s\n", dest);
      free(body);
      return 1;
    }
    printf("HTTP 200, %d bytes em %u ms\n", blen, k_ms() - t0);
    printf("gravado: %s\n", dest);
  }
  free(body);
  return 0;
}
