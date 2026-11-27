/* curl.c - cliente HTTP/HTTPS no estilo curl para o SODOS.
 *   CC CURL.C TLS.DLL
 *   CURL http://exemplo.com/
 *   CURL -o D:SAIDA.HTM https://exemplo.com/
 *   CURL -I http://exemplo.com/
 *   CURL -d "a=1&b=2" http://exemplo.com/post
 *   CURL -H "X-Teste: 1" http://exemplo.com/
 *   CURL -I http://exemplo.com/   (-I so cabecalhos; -i cabecalhos e corpo)
 *   CURL -L http://exemplo.com/   (segue redirecionamentos, ate 5)
 *   CURL -s ...   (silencioso)
 * HTTP e HTTPS (TLS 1.3/1.2 via TLS.DLL). Corpo ate 4 MB. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include <tls.h>

#define URL_MAX 512
#define BODY_MAX (4 * 1024 * 1024)
#define HDR_MAX 8192

struct url {
  char scheme[8];
  char host[128];
  char path[URL_MAX];
  int port;
  int tls;
};

static int ci_start(const char* s, const char* p) {
  while (*p) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) return 0;
    s++;
    p++;
  }
  return 1;
}

static void copy_n(char* dst, int max, const char* src, int n) {
  if (n > max - 1) n = max - 1;
  if (n < 0) n = 0;
  memcpy(dst, src, (size_t)n);
  dst[n] = 0;
}

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
  if (!u->host[0]) return -1;
  {
    char* q;
    for (q = u->host; *q; q++) *q = (char)tolower((unsigned char)*q);
  }
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
  return 0;
}

static void url_text(const struct url* u, char* out, int max) {
  if ((u->tls && u->port == 443) || (!u->tls && u->port == 80))
    snprintf(out, (size_t)max, "%s://%s%s", u->scheme, u->host, u->path);
  else
    snprintf(out, (size_t)max, "%s://%s:%d%s", u->scheme, u->host, u->port, u->path);
}

static int url_join(const struct url* base, const char* loc, struct url* out) {
  char full[URL_MAX];
  if (ci_start(loc, "http://") || ci_start(loc, "https://")) {
    return url_parse(loc, out);
  }
  if (loc[0] == '/') {
    snprintf(full, sizeof(full), "%s://%s%s", base->scheme, base->host, loc);
  } else {
    char dir[URL_MAX];
    char* slash;
    copy_n(dir, URL_MAX, base->path, (int)strlen(base->path));
    slash = strrchr(dir, '/');
    if (slash) slash[1] = 0;
    else strcpy(dir, "/");
    snprintf(full, sizeof(full), "%s://%s%s%s", base->scheme, base->host, dir, loc);
  }
  return url_parse(full, out);
}

static int c_send(int tls, const char* d, int n) { return tls ? tls_send(d, n) : k_tcp_send(d, n); }
static int c_recv(int tls, char* d, int cap) { return tls ? tls_recv(d, cap) : k_tcp_recv(d, cap); }

static int header(const char* hdr, int hlen, const char* name, char* out, int max) {
  int nl = (int)strlen(name);
  int i = 0;
  while (i < hlen) {
    int e = i;
    while (e < hlen && hdr[e] != '\n') e++;
    if (e - i > nl && hdr[i + nl] == ':' && ci_start(hdr + i, name)) {
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
    if (size == 0) { *out_len = total; return 1; }
    if (p + size + 2 > n) return 0;
    if (total + size > BODY_MAX) return -1;
    if (dst) memcpy(dst + total, b + p, (size_t)size);
    total += size;
    p += size;
    if (b[p] == '\r') p++;
    if (p < n && b[p] == '\n') p++;
  }
}

static int fetch(const struct url* u, const char* method, const char* data,
                 const char* extra_hdr, int head_only, char* hdr,
                 int* status, char** body, int* blen, char* location, char* err) {
  char req[2048];
  char xh[512];
  char* raw;
  int hlen = 0, rlen = 0, n, hdr_end = -1, i;
  int chunked = 0, clen = -1;

  *body = 0;
  *blen = 0;
  location[0] = 0;
  hdr[0] = 0;
  *status = 0;

  if (u->tls) {
    if (tls_connect(u->host, u->port) != 0) {
      snprintf(err, 160, "TLS: %s", tls_error());
      return -1;
    }
  } else {
    if (k_tcp_connect(u->host, u->port) != 0) {
      snprintf(err, 160, "nao conectou em %s:%d", u->host, u->port);
      return -1;
    }
  }

  /* O -H e uma linha propria: sem o \r\n dele o pedido nao terminava com a
   * linha em branco e o servidor ficava esperando o resto dos cabecalhos. */
  if (extra_hdr[0]) snprintf(xh, sizeof(xh), "%s\r\n", extra_hdr);
  else xh[0] = 0;
  if (data && *data) {
    snprintf(req, sizeof(req),
             "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: curl-sodos/1.0\r\n"
             "Accept: */*\r\nContent-Type: application/x-www-form-urlencoded\r\n"
             "Content-Length: %d\r\n%s\r\n",
             method, u->path, u->host, (int)strlen(data), xh);
  } else {
    snprintf(req, sizeof(req),
             "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: curl-sodos/1.0\r\n"
             "Accept: */*\r\n%s\r\n",
             method, u->path, u->host, xh);
  }
  if (c_send(u->tls, req, (int)strlen(req)) < 0) {
    snprintf(err, 160, "falha ao enviar");
    goto fail;
  }
  /* O corpo do POST vai a parte: dentro do req ele era cortado no tamanho
   * do buffer e o Content-Length ficava maior que o enviado. */
  if (data && *data && c_send(u->tls, data, (int)strlen(data)) < 0) {
    snprintf(err, 160, "falha ao enviar os dados");
    goto fail;
  }

  raw = (char*)malloc(BODY_MAX);
  if (!raw) { snprintf(err, 160, "sem memoria"); goto fail; }

  for (;;) {
    if (rlen >= BODY_MAX) break;
    n = c_recv(u->tls, raw + rlen, BODY_MAX - rlen);
    if (n < 0) { snprintf(err, 160, "falha ao receber"); free(raw); goto fail; }
    if (n == 0) break;
    rlen += n;
    if (hdr_end < 0) {
      for (i = 0; i + 3 < rlen; i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
          hdr_end = i + 4;
          break;
        }
      }
    }
  }

  if (hdr_end < 0) hdr_end = rlen;
  hlen = hdr_end;
  if (hlen > HDR_MAX) hlen = HDR_MAX;
  memcpy(hdr, raw, (size_t)hlen);
  hdr[hlen < HDR_MAX ? hlen : HDR_MAX - 1] = 0;

  {
    char* sp = strchr(hdr, ' ');
    if (sp) *status = atoi(sp + 1);
  }
  {
    char tmp[64];
    header(hdr, hlen, "Location", location, URL_MAX);
    if (header(hdr, hlen, "Transfer-Encoding", tmp, sizeof(tmp))) {
      if (ci_start(tmp, "chunked")) chunked = 1;
    }
    if (header(hdr, hlen, "Content-Length", tmp, sizeof(tmp))) clen = atoi(tmp);
  }

  {
    int body_off = hdr_end;
    int body_len = rlen - body_off;
    if (body_len < 0) body_len = 0;
    if (head_only) {
      *body = 0;
      *blen = 0;
    } else if (chunked) {
      char* out = (char*)malloc(BODY_MAX);
      int olen = 0;
      if (!out) { snprintf(err, 160, "sem memoria"); free(raw); goto fail; }
      if (dechunk(raw + body_off, body_len, out, &olen) < 0) {
        snprintf(err, 160, "chunked invalido");
        free(out); free(raw); goto fail;
      }
      *body = out;
      *blen = olen;
    } else {
      if (clen >= 0 && clen < body_len) body_len = clen;
      *body = (char*)malloc(body_len + 1);
      if (!*body) { snprintf(err, 160, "sem memoria"); free(raw); goto fail; }
      memcpy(*body, raw + body_off, (size_t)body_len);
      (*body)[body_len] = 0;
      *blen = body_len;
    }
  }
  free(raw);
  if (u->tls) tls_close(); else k_tcp_close();
  return 0;

fail:
  if (u->tls) tls_close(); else k_tcp_close();
  return -1;
}

static void usage(void) {
  printf("curl - transfere dados de uma URL (HTTP/HTTPS)\n");
  printf("  curl [opcoes] URL\n");
  printf("  -o ARQ     grava o corpo em ARQ\n");
  printf("  -O         grava com o nome do arquivo da URL\n");
  printf("  -I         so os cabecalhos (HEAD)\n");
  printf("  -i         mostra cabecalhos e corpo\n");
  printf("  -L         segue redirecionamentos (ate 5)\n");
  printf("  -s         silencioso (sem mensagens de progresso)\n");
  printf("  -d DADOS   POST com os dados\n");
  printf("  -H \"C: v\"  cabecalho extra\n");
  printf("Exemplos:\n");
  printf("  curl http://exemplo.com/\n");
  printf("  curl -o D:SAIDA.HTM https://exemplo.com/\n");
  printf("  curl -d \"a=1\" http://exemplo.com/post\n");
}

static void name_from_path(const char* path, char* out, int max) {
  const char* base = path;
  const char* p;
  const char* dot = 0;
  char name[9];
  char ext[4];
  int ni = 0, ei = 0;
  for (p = path; *p; p++)
    if (*p == '/') base = p + 1;
  if (!*base || *base == '?') { snprintf(out, (size_t)max, "INDEX.HTM"); return; }
  for (p = base; *p && *p != '?' && *p != '#'; p++)
    if (*p == '.') dot = p;
  p = base;
  if (dot) {
    while (p < dot && ni < 8) {
      char c = *p++;
      if (isalnum((unsigned char)c) || c == '-' || c == '_') name[ni++] = (char)toupper((unsigned char)c);
    }
    p = dot + 1;
    while (*p && *p != '?' && *p != '#' && ei < 3) {
      char c = *p++;
      if (isalnum((unsigned char)c)) ext[ei++] = (char)toupper((unsigned char)c);
    }
  } else {
    while (*p && *p != '?' && *p != '#' && ni < 8) {
      char c = *p++;
      if (isalnum((unsigned char)c) || c == '-' || c == '_') name[ni++] = (char)toupper((unsigned char)c);
    }
  }
  name[ni] = 0;
  ext[ei] = 0;
  if (!name[0]) strcpy(name, "INDEX");
  if (ext[0]) snprintf(out, (size_t)max, "%s.%s", name, ext);
  else snprintf(out, (size_t)max, "%s", name);
}

int main(int argc, char** argv) {
  const char* url_s = 0;
  const char* outfile = 0;
  const char* data = 0;
  const char* extra_hdr = "";
  int head_only = 0, show_hdr = 0, follow = 0, silent = 0, auto_name = 0;
  int i, hop, status = 0, blen = 0;
  struct url u;
  char err[160];
  char location[URL_MAX];
  static char hdr[HDR_MAX];
  char* body = 0;
  unsigned t0;

  for (i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "-o") == 0 && i + 1 < argc) { outfile = argv[++i]; }
    else if (strcmp(a, "-O") == 0) { auto_name = 1; }
    else if (strcmp(a, "-I") == 0) { head_only = 1; show_hdr = 1; }
    else if (strcmp(a, "-i") == 0) { show_hdr = 1; }
    else if (strcmp(a, "-L") == 0) { follow = 1; }
    else if (strcmp(a, "-s") == 0) { silent = 1; }
    else if (strcmp(a, "-d") == 0 && i + 1 < argc) { data = argv[++i]; }
    else if (strcmp(a, "-H") == 0 && i + 1 < argc) { extra_hdr = argv[++i]; }
    else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(); return 0; }
    else if (a[0] == '-') { printf("opcao desconhecida: %s\n", a); usage(); return 1; }
    else { url_s = a; }
  }
  if (!url_s) { usage(); return 1; }
  if (url_parse(url_s, &u) != 0) {
    printf("curl: URL invalida (use http:// ou https://)\n");
    return 1;
  }
  if (!k_net_ready()) {
    printf("curl: rede nao pronta\n");
    return 1;
  }

  t0 = k_ms();
  for (hop = 0; hop < 5; hop++) {
    char shown[URL_MAX];
    struct url next;
    url_text(&u, shown, URL_MAX);
    if (!silent) printf("> %s %s\n", head_only ? "HEAD" : (data ? "POST" : "GET"), shown);
    err[0] = 0;
    if (fetch(&u, head_only ? "HEAD" : (data ? "POST" : "GET"), data, extra_hdr,
              head_only, hdr, &status, &body, &blen, location, err) != 0) {
      printf("curl: %s\n", err[0] ? err : "falhou");
      return 1;
    }
    if (follow && status >= 300 && status < 400 && location[0]) {
      if (!silent) printf("< HTTP %d -> %s\n", status, location);
      if (url_join(&u, location, &next) != 0) {
        printf("curl: redirecionamento invalido\n");
        free(body);
        return 1;
      }
      u = next;
      free(body);
      body = 0;
      continue;
    }
    break;
  }
  if (hop == 5) {
    printf("curl: redirecionamentos demais\n");
    free(body);
    return 1;
  }

  /* -I e -i: os cabecalhos da resposta (da ultima, com -L), como no curl,
   * inclusive com -s. Antes saia so "< HTTP 200". */
  if (show_hdr && hdr[0]) k_write(hdr, (int)strlen(hdr));

  if (outfile || auto_name) {
    char name[16];
    const char* dest;
    if (!outfile) { name_from_path(u.path, name, (int)sizeof(name)); dest = name; }
    else dest = outfile;
    if (k_fsave(dest, body ? body : "", blen) < 0) {
      printf("curl: nao gravou %s\n", dest);
      free(body);
      return 1;
    }
    if (!silent) printf("gravado: %s (%d bytes em %u ms)\n", dest, blen, k_ms() - t0);
  } else {
    if (body && blen > 0) k_write(body, blen);
    if (!silent) printf("\n[%d bytes em %u ms, HTTP %d]\n", blen, k_ms() - t0, status);
  }
  free(body);
  return (status >= 200 && status < 400) ? 0 : 1;
}
