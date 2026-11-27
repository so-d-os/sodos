/* instchrm.c - INSTCHRM.EXE: baixa o Google Chrome (x64) do Google e instala
 * em D:\APPS\CHROME para o WINE.DLL, com o D:\RODACHROME.BAT e o atalho
 * CHROME.LNK no desktop.
 *
 *   INSTCHRM          versao estavel atual: pergunta, baixa, confere, instala
 *   INSTCHRM -r       instala de novo mesmo se a versao ja esta la
 *   INSTCHRM -n       so mostra a versao que o Google oferece
 *   INSTCHRM -f ARQ   instala de um instalador ja baixado (sem rede); ARQ e o
 *                     NNN_chrome_installer_uncompressed.exe
 *   INSTCHRM -b       so grava o RODACHROME.BAT e o atalho
 *
 * De onde vem: o servidor de atualizacao do Chrome (o do Google Update,
 * protocolo Omaha 3.0, https://tools.google.com/service/update2) responde com
 * a versao, o endereco em dl.google.com, o tamanho e o SHA-256 do instalador
 * (o mini_installer "uncompressed", ~536 MB). Com Accept-Encoding: gzip o
 * dl.google.com o manda com ~265 MB.
 *
 * O setup.exe do Google nao roda aqui (servico do Google Update, registro,
 * tarefas agendadas) e nem precisa: o mini_installer guarda o Chrome num 7z
 * (recurso BN/CHROME.7Z) com os arquivos sem compressao (metodo Copy; so o
 * cabecalho do 7z e os dois .sig usam LZMA e BCJ2). O INSTCHRM le antes, por
 * Range, o comeco do arquivo (cabecalho PE e recursos) e o fim do 7z (a lista
 * de arquivos); depois baixa o pacote em gzip para a memoria (ver
 * download_ram) e so entao descompacta, gravando cada arquivo do Chrome-bin
 * enquanto ele passa: o pacote nao vai para o disco, que so precisa do
 * espaco do Chrome instalado (~505 MB).
 *
 * Confere o CRC32 de cada arquivo (do 7z) e o SHA-256 do pacote inteiro (do
 * Google). Tudo vai para a pasta da versao (D:\APPS\CHROME\158.0.8127.89),
 * inclusive o chrome.exe (ver write_launchers); o RODACHROME.BAT so passa a
 * abrir a versao nova depois do SHA-256, e entao as pastas das versoes
 * antigas sao apagadas.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include <tls.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define DEST "D:\\APPS\\CHROME"
#define BAT_PATH "D:\\RODACHROME.BAT"
#define LNK_PATH "D:\\USER\\DEFAULT\\DESKTOP\\CHROME.LNK"
#define OMAHA_URL "https://tools.google.com/service/update2"
#define APPID "{8A69D345-D564-463C-AFF1-A69D9E530F96}"
#define HEAD_LEN (128u << 10) /* comeco do instalador: PE e diretorio de recursos */
#define TAIL_LEN (256u << 10) /* fim do 7z: cabecalho (lista de arquivos) */
#define FOLD_MAX (64u << 20)  /* pastas do 7z com LZMA/BCJ2, decodificadas na memoria */
#define PATH_N 300

static char g_err[300];
static int g_fail;
static int g_dbg; /* SET INSTDBG=1 */

static int fail(const char* fmt, ...) {
  va_list ap;
  if (g_fail) return -1;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof(g_err), fmt, ap);
  va_end(ap);
  g_fail = 1;
  return -1;
}

static u32 rd16(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 rd64(const u8* p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }

static int lowc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ci_start(const char* s, const char* p) {
  while (*p) {
    if (lowc((u8)*s) != lowc((u8)*p)) return 0;
    s++;
    p++;
  }
  return 1;
}

/* ---------- CRC32 e SHA-256 ---------- */

static u32 crc_tab[256];
static void crc_init(void) {
  u32 i, k, c;
  for (i = 0; i < 256; i++) {
    c = i;
    for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_tab[i] = c;
  }
}
static u32 crc_upd(u32 c, const u8* p, u32 n) {
  c = ~c;
  while (n--) c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
  return ~c;
}

typedef struct {
  u32 h[8];
  u8 buf[64];
  u32 nbuf;
  u64 len;
} sha_t;

static const u32 sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(sha_t* s, const u8* p) {
  u32 w[64], a, b, c, d, e, f, g, h, t1, t2;
  int i;
  for (i = 0; i < 16; i++) w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) | ((u32)p[4 * i + 2] << 8) | p[4 * i + 3];
  for (i = 16; i < 64; i++) {
    u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (i = 0; i < 64; i++) {
    t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
    t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d;
  s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}
static void sha_init(sha_t* s) {
  static const u32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, iv, sizeof(iv));
  s->nbuf = 0;
  s->len = 0;
}
static void sha_upd(sha_t* s, const u8* p, u32 n) {
  s->len += n;
  if (s->nbuf) {
    while (n && s->nbuf < 64) s->buf[s->nbuf++] = *p++, n--;
    if (s->nbuf < 64) return;
    sha_block(s, s->buf);
    s->nbuf = 0;
  }
  while (n >= 64) {
    sha_block(s, p);
    p += 64;
    n -= 64;
  }
  while (n--) s->buf[s->nbuf++] = *p++;
}
static void sha_hex(sha_t* s, char out[65]) {
  static const char hx[] = "0123456789abcdef";
  u64 bits = s->len * 8;
  int i;
  s->buf[s->nbuf++] = 0x80;
  if (s->nbuf > 56) {
    while (s->nbuf < 64) s->buf[s->nbuf++] = 0;
    sha_block(s, s->buf);
    s->nbuf = 0;
  }
  while (s->nbuf < 56) s->buf[s->nbuf++] = 0;
  for (i = 0; i < 8; i++) s->buf[56 + i] = (u8)(bits >> (56 - 8 * i));
  sha_block(s, s->buf);
  for (i = 0; i < 32; i++) {
    u8 b = (u8)(s->h[i / 4] >> (24 - 8 * (i % 4)));
    out[2 * i] = hx[b >> 4];
    out[2 * i + 1] = hx[b & 15];
  }
  out[64] = 0;
}

/* ---------- HTTP(S): uma conexao por vez, corpo lido aos pedacos ---------- */

struct url {
  char host[128];
  char path[1024];
  int port;
  int tls;
};

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
  while (*p && *p != '/' && *p != ':' && *p != '?') {
    int c = lowc((u8)*p++);
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return -1;
    if (n >= (int)sizeof(u->host) - 1) return -1;
    u->host[n++] = (char)c;
  }
  u->host[n] = 0;
  if (!n) return -1;
  if (*p == ':') {
    u->port = atoi(p + 1);
    while (*p && *p != '/') p++;
    if (u->port <= 0 || u->port > 65535) return -1;
  }
  if (!*p) p = "/";
  if (strlen(p) >= sizeof(u->path)) return -1;
  strcpy(u->path, *p == '?' ? "/" : p);
  if (*p == '?') strcat(u->path, p);
  return 0;
}

static int hc_tls;
static u8 hc_buf[65536];
static int hc_pos, hc_len;

struct resp {
  int status;
  long long clen; /* -1: sem Content-Length */
  long long left; /* do corpo (ou do pedaco, no chunked) */
  int chunked;
  int gzip;
  int done;
  char loc[1024];
};
static struct resp R;

static int c_recv(u8* d, int cap) { return hc_tls ? tls_recv(d, cap) : k_tcp_recv(d, cap); }
static int c_send(const char* d, int n) {
  int off = 0;
  while (off < n) {
    int k = hc_tls ? tls_send(d + off, n - off) : k_tcp_send(d + off, n - off);
    if (k <= 0) return -1;
    off += k;
  }
  return 0;
}
static void http_close(void) {
  if (hc_tls) tls_close();
  else k_tcp_close();
  hc_pos = hc_len = 0;
}
static int hc_fill(void) {
  int n = c_recv(hc_buf, (int)sizeof(hc_buf));
  if (n <= 0) return n < 0 ? -1 : 0;
  hc_pos = 0;
  hc_len = n;
  return n;
}
static int hc_byte(void) {
  if (hc_pos >= hc_len && hc_fill() <= 0) return -1;
  return hc_buf[hc_pos++];
}

static int hdr_value(const char* h, const char* name, char* out, int max) {
  int nl = (int)strlen(name);
  const char* p = h;
  while (*p) {
    const char* e = strchr(p, '\n');
    if (!e) e = p + strlen(p);
    if (e - p > nl && p[nl] == ':' && ci_start(p, name)) {
      const char* s = p + nl + 1;
      int n = 0;
      while (*s == ' ' || *s == '\t') s++;
      while (s < e && *s != '\r' && n < max - 1) out[n++] = *s++;
      out[n] = 0;
      return 1;
    }
    p = *e ? e + 1 : e;
  }
  out[0] = 0;
  return 0;
}

/* Manda o pedido e le o cabecalho da resposta (fica em R). */
static int http_open(const struct url* u, const char* method, const char* extra, const char* body, int blen) {
  static char req[2048], hdr[16384];
  char v[128];
  int n = 0, c;
  hc_tls = u->tls;
  hc_pos = hc_len = 0;
  memset(&R, 0, sizeof(R));
  if (u->tls) {
    if (tls_connect(u->host, u->port) != 0) return fail("TLS com %s falhou: %s", u->host, tls_error());
    if (!tls_cert_ok()) {
      tls_close();
      return fail("%s: %s", u->host, tls_security());
    }
  } else if (k_tcp_connect(u->host, u->port) != 0) {
    return fail("sem conexao com %s:%d", u->host, u->port);
  }
  snprintf(req, sizeof(req),
           "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SODOS-INSTCHRM/1.0\r\nAccept: */*\r\n"
           "Connection: close\r\n%s",
           method, u->path, u->host, extra ? extra : "");
  if (body) snprintf(req + strlen(req), sizeof(req) - strlen(req), "Content-Length: %d\r\n", blen);
  strcat(req, "\r\n");
  if (c_send(req, (int)strlen(req)) != 0 || (body && blen && c_send(body, blen) != 0)) {
    http_close();
    return fail("%s: falha ao enviar o pedido", u->host);
  }
  for (;;) {
    c = hc_byte();
    if (c < 0) {
      http_close();
      return fail("%s: resposta HTTP incompleta", u->host);
    }
    if (n >= (int)sizeof(hdr) - 1) {
      http_close();
      return fail("%s: cabecalho HTTP grande demais", u->host);
    }
    hdr[n++] = (char)c;
    if (n >= 4 && memcmp(hdr + n - 4, "\r\n\r\n", 4) == 0) break;
  }
  hdr[n] = 0;
  if (strncmp(hdr, "HTTP/", 5) != 0 || !strchr(hdr, ' ')) {
    http_close();
    return fail("%s: resposta nao e HTTP", u->host);
  }
  R.status = atoi(strchr(hdr, ' ') + 1);
  R.clen = -1;
  if (hdr_value(hdr, "Content-Length", v, (int)sizeof(v))) R.clen = atoll(v);
  if (hdr_value(hdr, "Transfer-Encoding", v, (int)sizeof(v)) && strstr(v, "chunked")) R.chunked = 1;
  if (hdr_value(hdr, "Content-Encoding", v, (int)sizeof(v)) && strstr(v, "gzip")) R.gzip = 1;
  hdr_value(hdr, "Location", R.loc, (int)sizeof(R.loc));
  R.left = R.chunked ? 0 : R.clen;
  if (!strcmp(method, "HEAD") || R.status == 204 || R.status == 304) R.done = 1;
  return 0;
}

/* Bytes do corpo (sem o chunked): >0 lidos, 0 fim, -1 erro. */
static int http_body(u8* out, int cap) {
  int take;
  if (R.done || g_fail) return g_fail ? -1 : 0;
  if (R.chunked && R.left == 0) {
    long long sz = 0;
    int c, digits = 0;
    while ((c = hc_byte()) >= 0) {
      int v = (c >= '0' && c <= '9') ? c - '0' : (lowc(c) >= 'a' && lowc(c) <= 'f') ? lowc(c) - 'a' + 10 : -1;
      if (v < 0) break;
      sz = sz * 16 + v;
      digits++;
    }
    while (c >= 0 && c != '\n') c = hc_byte();
    if (c < 0 || !digits) return fail("download cortado (chunked)");
    if (sz == 0) {
      int len = 0; /* trailers ate a linha vazia */
      while ((c = hc_byte()) >= 0) {
        if (c == '\n') {
          if (len == 0) break;
          len = 0;
        } else if (c != '\r') {
          len++;
        }
      }
      R.done = 1;
      return 0;
    }
    R.left = sz;
  }
  if (!R.chunked && R.left == 0) {
    R.done = 1;
    return 0;
  }
  if (hc_pos >= hc_len) {
    int n = hc_fill();
    if (n <= 0) {
      if (!R.chunked && R.left < 0 && n == 0) {
        R.done = 1;
        return 0;
      }
      if (n == 0) return fail("download cortado (o servidor fechou a conexao)");
      return fail("download cortado (%s)", hc_tls ? tls_error() : "conexao caiu");
    }
  }
  take = hc_len - hc_pos;
  if (take > cap) take = cap;
  if (R.left >= 0 && take > R.left) take = (int)R.left;
  memcpy(out, hc_buf + hc_pos, (size_t)take);
  hc_pos += take;
  if (R.left >= 0) R.left -= take;
  if (R.chunked && R.left == 0) {
    int c = hc_byte();
    if (c == '\r') c = hc_byte();
    if (c != '\n') return fail("download cortado (chunked)");
  }
  return take;
}

/* GET (ou POST) seguindo ate 5 redirecionamentos. */
static int http_req(const char* url_s, const char* method, const char* extra, const char* body, int blen) {
  struct url u;
  static char cur[1100];
  int hop;
  if (strlen(url_s) >= sizeof(cur)) return fail("url grande demais");
  strcpy(cur, url_s);
  for (hop = 0; hop < 6; hop++) {
    if (url_parse(cur, &u) != 0) return fail("url invalida: %s", cur);
    if (http_open(&u, method, extra, body, blen) != 0) return -1;
    if (R.status >= 300 && R.status < 400 && R.loc[0]) {
      http_close();
      if (R.loc[0] == '/') {
        snprintf(cur, sizeof(cur), "%s://%s%s", u.tls ? "https" : "http", u.host, R.loc);
      } else {
        strncpy(cur, R.loc, sizeof(cur) - 1);
        cur[sizeof(cur) - 1] = 0;
      }
      continue;
    }
    return 0;
  }
  return fail("redirecionamentos demais");
}

/* ---------- origem do instalador: rede (Range) ou arquivo ---------- */

static const char* src_file;  /* -f */
static char src_url[1200];
static u64 src_size;          /* tamanho do instalador (o do Omaha ou o do arquivo) */
static u64 file_off;          /* leitura em sequencia do -f */

static int body_file(u8* out, int cap) {
  int n;
  if (file_off >= (u64)k_fsize(src_file)) return 0;
  n = k_fread(src_file, (unsigned)file_off, (char*)out, cap);
  if (n < 0) return fail("nao consegui ler %s", src_file);
  file_off += (u64)n;
  return n;
}

/* Trecho [off, off+len) do instalador. */
static int src_range(u64 off, u32 len, u8* out) {
  u32 got = 0;
  if (src_file) {
    int n = k_fread(src_file, (unsigned)off, (char*)out, (int)len);
    if (n != (int)len) return fail("nao consegui ler %s", src_file);
    return 0;
  }
  {
    char extra[96];
    snprintf(extra, sizeof(extra), "Range: bytes=%llu-%llu\r\nAccept-Encoding: identity\r\n", off,
             off + len - 1);
    if (http_req(src_url, "GET", extra, 0, 0) != 0) return -1;
    if (R.status != 206 && !(R.status == 200 && off == 0)) {
      http_close();
      return fail("dl.google.com respondeu HTTP %d ao pedido parcial", R.status);
    }
    while (got < len) {
      int n = http_body(out + got, (int)(len - got));
      if (n <= 0) break;
      got += (u32)n;
    }
    http_close();
    if (got != len) return fail("pedido parcial incompleto (%u de %u bytes)", got, len);
  }
  return 0;
}

/* ---------- inflate (gzip) em fluxo: entrada puxada, saida em blocos de 32 KB ---------- */

static int (*z_body)(u8*, int);
static int (*z_sink)(const u8*, u32);
static u8 zin[65536];
static int zin_pos, zin_len, zpad;
static u32 zb;
static int zn;
static u8 zwin[65536];
static u32 zw; /* bytes de saida (modulo 2^32: a janela so usa os 16 de baixo) */

static int z_byte(void) {
  if (zin_pos >= zin_len) {
    int n = z_body(zin, (int)sizeof(zin));
    if (n <= 0) return -1;
    zin_pos = 0;
    zin_len = n;
  }
  return zin[zin_pos++];
}
static void z_need(int n) {
  while (zn < n) {
    int c = z_byte();
    if (c < 0) {
      /* fim da entrada: zeros, para o decodificador nao ler fora; a corrente
       * cortada vira erro (senao zeros viram literais para sempre) */
      c = 0;
      if (++zpad > 8) fail("download cortado (gzip)");
    }
    zb |= (u32)c << zn;
    zn += 8;
  }
}
static u32 z_bits(int n) {
  u32 v;
  if (!n) return 0;
  z_need(n);
  v = zb & ((1u << n) - 1);
  zb >>= n;
  zn -= n;
  return v;
}
static void z_out(int b) {
  zwin[zw & 0xFFFF] = (u8)b;
  zw++;
  if ((zw & 0x7FFF) == 0 && z_sink(zwin + ((zw - 0x8000) & 0xFFFF), 0x8000) != 0) g_fail = 1;
}

#define FASTB 10
typedef struct {
  u16 fast[1 << FASTB]; /* (simbolo << 4) | bits; 0 = codigo mais longo */
  short count[16];
  short sym[320];
} huf_t;

static int huf_build(huf_t* h, const u8* lens, int n) {
  short offs[16];
  u32 next[16], code;
  int len, s, left = 1;
  memset(h->count, 0, sizeof(h->count));
  memset(h->fast, 0, sizeof(h->fast));
  for (s = 0; s < n; s++) h->count[lens[s]]++;
  h->count[0] = 0;
  for (len = 1; len < 16; len++) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) return -1;
  }
  offs[1] = 0;
  for (len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
  for (s = 0; s < n; s++)
    if (lens[s]) h->sym[offs[lens[s]]++] = (short)s;
  code = 0;
  next[0] = 0;
  for (len = 1; len < 16; len++) {
    code = (code + (u32)h->count[len - 1]) << 1;
    next[len] = code;
  }
  for (s = 0; s < n; s++) {
    u32 c, r = 0, k;
    int i;
    len = lens[s];
    if (!len) continue;
    c = next[len]++;
    if (len > FASTB) continue;
    for (i = 0; i < len; i++) r |= ((c >> i) & 1u) << (len - 1 - i);
    for (k = r; k < (1u << FASTB); k += 1u << len) h->fast[k] = (u16)((s << 4) | len);
  }
  return 0;
}
static int huf_dec(const huf_t* h) {
  u32 e;
  int code = 0, first = 0, index = 0, len;
  z_need(15);
  e = h->fast[zb & ((1u << FASTB) - 1)];
  if (e) {
    len = (int)(e & 15);
    zb >>= len;
    zn -= len;
    return (int)(e >> 4);
  }
  for (len = 1; len < 16; len++) {
    int count;
    code |= (int)((zb >> (len - 1)) & 1);
    count = h->count[len];
    if (code - count < first) {
      zb >>= len;
      zn -= len;
      return h->sym[index + (code - first)];
    }
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int z_codes(const huf_t* lit, const huf_t* dist) {
  for (;;) {
    int sym = huf_dec(lit);
    if (g_fail) return -1;
    if (sym < 0) return fail("gzip: codigo invalido");
    if (sym < 256) {
      z_out(sym);
    } else if (sym == 256) {
      return 0;
    } else {
      int len, ds;
      u32 d;
      sym -= 257;
      if (sym >= 29) return fail("gzip: comprimento invalido");
      len = lbase[sym] + (int)z_bits(lext[sym]);
      ds = huf_dec(dist);
      if (ds < 0 || ds >= 30) return fail("gzip: distancia invalida");
      d = (u32)dbase[ds] + z_bits(dext[ds]);
      if (d > zw) return fail("gzip: distancia antes do comeco");
      while (len--) z_out(zwin[(zw - d) & 0xFFFF]);
    }
  }
}

static int z_stored(void) {
  u32 len, nlen;
  zb >>= (zn & 7);
  zn -= zn & 7;
  len = z_bits(16);
  nlen = z_bits(16);
  if ((len ^ 0xFFFF) != nlen) return fail("gzip: bloco guardado invalido");
  while (len && zn >= 8) {
    z_out((int)z_bits(8));
    len--;
  }
  while (len && !g_fail) {
    int c = z_byte();
    if (c < 0) return fail("download cortado (gzip)");
    z_out(c);
    len--;
  }
  return g_fail ? -1 : 0;
}

static huf_t hf_lit, hf_dist, hf_flit, hf_fdist;
static int hf_fixed_ok;

static int z_dynamic(void) {
  static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  u8 lens[320];
  huf_t* lc = &hf_dist; /* emprestado para o codigo dos comprimentos */
  int nlen, ndist, ncode, i, idx = 0;
  nlen = (int)z_bits(5) + 257; /* um por instrucao: a ordem importa */
  ndist = (int)z_bits(5) + 1;
  ncode = (int)z_bits(4) + 4;
  if (nlen > 286 || ndist > 30) return fail("gzip: cabecalho de bloco invalido");
  memset(lens, 0, sizeof(lens));
  for (i = 0; i < ncode; i++) lens[order[i]] = (u8)z_bits(3);
  if (huf_build(lc, lens, 19) != 0) return fail("gzip: codigo de comprimentos invalido");
  while (idx < nlen + ndist) {
    int sym = huf_dec(lc), rep, val = 0;
    if (sym < 0) return fail("gzip: comprimento invalido");
    if (sym < 16) {
      lens[idx++] = (u8)sym;
      continue;
    }
    if (sym == 16) {
      if (!idx) return fail("gzip: repeticao sem anterior");
      val = lens[idx - 1];
      rep = 3 + (int)z_bits(2);
    } else if (sym == 17) {
      rep = 3 + (int)z_bits(3);
    } else {
      rep = 11 + (int)z_bits(7);
    }
    if (idx + rep > nlen + ndist) return fail("gzip: comprimentos demais");
    while (rep--) lens[idx++] = (u8)val;
  }
  if (!lens[256]) return fail("gzip: bloco sem fim");
  if (huf_build(&hf_lit, lens, nlen) != 0 || huf_build(&hf_dist, lens + nlen, ndist) != 0)
    return fail("gzip: codigo invalido");
  return z_codes(&hf_lit, &hf_dist);
}

/* Descompacta um gzip de body() para sink(). */
static int gunzip(int (*body)(u8*, int), int (*sink)(const u8*, u32)) {
  int flg, i, last;
  z_body = body;
  z_sink = sink;
  zin_pos = zin_len = zpad = 0;
  zb = 0;
  zn = 0;
  zw = 0;
  if (z_byte() != 0x1f || z_byte() != 0x8b || z_byte() != 8) return fail("resposta gzip invalida");
  flg = z_byte();
  for (i = 0; i < 6; i++) z_byte();
  if (flg & 4) {
    int xl = z_byte();
    xl |= z_byte() << 8;
    while (xl-- > 0) z_byte();
  }
  if (flg & 8)
    while (z_byte() > 0) {
    }
  if (flg & 16)
    while (z_byte() > 0) {
    }
  if (flg & 2) z_byte(), z_byte();
  if (!hf_fixed_ok) {
    u8 l[320];
    for (i = 0; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    huf_build(&hf_flit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    huf_build(&hf_fdist, l, 30);
    hf_fixed_ok = 1;
  }
  do {
    int type;
    last = (int)z_bits(1);
    type = (int)z_bits(2);
    if (type == 0) {
      if (z_stored() != 0) return -1;
    } else if (type == 1) {
      if (z_codes(&hf_flit, &hf_fdist) != 0) return -1;
    } else if (type == 2) {
      if (z_dynamic() != 0) return -1;
    } else {
      return fail("gzip: tipo de bloco invalido");
    }
  } while (!last && !g_fail);
  if (g_fail) return -1;
  if (zw & 0x7FFF) {
    u32 n = zw & 0x7FFF;
    if (z_sink(zwin + ((zw - n) & 0xFFFF), n) != 0) return -1;
  }
  return 0;
}

/* ---------- LZMA e BCJ2 (cabecalho do 7z e os .sig), tudo na memoria ---------- */

struct lzrc {
  const u8* p;
  u32 n, i;
  u32 range, code;
  int bad;
};
static u32 lz_in(struct lzrc* r) {
  if (r->i >= r->n) {
    r->bad = 1;
    return 0;
  }
  return r->p[r->i++];
}
static int lz_bit(struct lzrc* r, u16* prob) {
  u32 bound = (r->range >> 11) * *prob;
  int bit;
  if (r->code < bound) {
    *prob = (u16)(*prob + ((2048 - *prob) >> 5));
    r->range = bound;
    bit = 0;
  } else {
    *prob = (u16)(*prob - (*prob >> 5));
    r->code -= bound;
    r->range -= bound;
    bit = 1;
  }
  if (r->range < (1u << 24)) {
    r->range <<= 8;
    r->code = (r->code << 8) | lz_in(r);
  }
  return bit;
}
static u32 lz_direct(struct lzrc* r, int n) {
  u32 res = 0;
  while (n--) {
    u32 t;
    r->range >>= 1;
    r->code -= r->range;
    t = 0u - (r->code >> 31);
    r->code += r->range & t;
    if (r->range < (1u << 24)) {
      r->range <<= 8;
      r->code = (r->code << 8) | lz_in(r);
    }
    res = (res << 1) + (t + 1);
  }
  return res;
}
static u32 lz_tree(struct lzrc* r, u16* p, int bits) {
  u32 m = 1;
  int i;
  for (i = 0; i < bits; i++) m = (m << 1) + (u32)lz_bit(r, &p[m]);
  return m - (1u << bits);
}
static u32 lz_rtree(struct lzrc* r, u16* p, int bits) {
  u32 m = 1, sym = 0;
  int i;
  for (i = 0; i < bits; i++) {
    u32 b = (u32)lz_bit(r, &p[m]);
    m = (m << 1) + b;
    sym |= b << i;
  }
  return sym;
}

struct lzlen {
  u16 choice, choice2, low[16][8], mid[16][8], high[256];
};
static u32 lz_len(struct lzrc* r, struct lzlen* l, int ps) {
  if (!lz_bit(r, &l->choice)) return lz_tree(r, l->low[ps], 3);
  if (!lz_bit(r, &l->choice2)) return 8 + lz_tree(r, l->mid[ps], 3);
  return 16 + lz_tree(r, l->high, 8);
}

struct lzst {
  u16 is_match[12 * 16], is_rep[12], is_rep_g0[12], is_rep_g1[12], is_rep_g2[12], is_rep0_long[12 * 16];
  u16 pos_slot[4][64], pos[115], align[16];
  struct lzlen len, rep_len;
};

static void lz_init_probs(u16* p, u32 n) {
  while (n--) *p++ = 1024;
}

static int lzma_dec(const u8* props, int np, const u8* in, u32 inlen, u8* out, u32 outlen) {
  struct lzrc rc;
  struct lzst* st;
  u16* lit;
  u32 lc, lp, pb, d, rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0, op = 0, i;
  int state = 0;
  if (np < 5) return fail("7z: propriedades do LZMA invalidas");
  d = props[0];
  if (d >= 9 * 5 * 5) return fail("7z: propriedades do LZMA invalidas");
  lc = d % 9;
  d /= 9;
  lp = d % 5;
  pb = d / 5;
  st = (struct lzst*)malloc(sizeof(*st));
  lit = (u16*)malloc(sizeof(u16) * (0x300u << (lc + lp)));
  if (!st || !lit) {
    free(st);
    free(lit);
    return fail("sem memoria (LZMA)");
  }
  lz_init_probs((u16*)st, (u32)(sizeof(*st) / 2));
  lz_init_probs(lit, 0x300u << (lc + lp));
  memset(&rc, 0, sizeof(rc));
  rc.p = in;
  rc.n = inlen;
  rc.range = 0xFFFFFFFFu;
  lz_in(&rc);
  for (i = 0; i < 4; i++) rc.code = (rc.code << 8) | lz_in(&rc);
  while (op < outlen && !rc.bad) {
    u32 ps = op & ((1u << pb) - 1), len;
    if (!lz_bit(&rc, &st->is_match[(state << 4) + ps])) {
      u32 prev = op ? out[op - 1] : 0, sym = 1;
      u16* pr = lit + 0x300u * (((op & ((1u << lp) - 1)) << lc) + (prev >> (8 - lc)));
      if (state >= 7) {
        u32 mb = out[op - rep0 - 1];
        do {
          u32 mbit = (mb >> 7) & 1, bit;
          mb <<= 1;
          bit = (u32)lz_bit(&rc, &pr[((1 + mbit) << 8) + sym]);
          sym = (sym << 1) | bit;
          if (mbit != bit) break;
        } while (sym < 0x100);
      }
      while (sym < 0x100) sym = (sym << 1) | (u32)lz_bit(&rc, &pr[sym]);
      if (g_dbg > 1 && op < 1700) printf("L %u %u %d\n", op, sym & 255, state);
      out[op++] = (u8)sym;
      state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
      continue;
    }
    if (lz_bit(&rc, &st->is_rep[state])) {
      if (!op) break;
      if (!lz_bit(&rc, &st->is_rep_g0[state])) {
        if (!lz_bit(&rc, &st->is_rep0_long[(state << 4) + ps])) {
          state = state < 7 ? 9 : 11;
          out[op] = out[op - rep0 - 1];
          op++;
          continue;
        }
      } else {
        u32 dist;
        if (!lz_bit(&rc, &st->is_rep_g1[state])) {
          dist = rep1;
        } else {
          if (!lz_bit(&rc, &st->is_rep_g2[state])) {
            dist = rep2;
          } else {
            dist = rep3;
            rep3 = rep2;
          }
          rep2 = rep1;
        }
        rep1 = rep0;
        rep0 = dist;
      }
      len = lz_len(&rc, &st->rep_len, (int)ps);
      state = state < 7 ? 8 : 11;
    } else {
      u32 slot, lstate;
      rep3 = rep2;
      rep2 = rep1;
      rep1 = rep0;
      len = lz_len(&rc, &st->len, (int)ps);
      state = state < 7 ? 7 : 10;
      lstate = len < 3 ? len : 3;
      slot = lz_tree(&rc, st->pos_slot[lstate], 6);
      if (slot < 4) {
        rep0 = slot;
      } else {
        int nd = (int)(slot >> 1) - 1;
        rep0 = (2 | (slot & 1)) << nd;
        if (slot < 14) {
          rep0 += lz_rtree(&rc, st->pos + rep0 - slot, nd);
        } else {
          /* duas instrucoes: a ordem dos operandos de + nao e definida (o cc
           * do SODOS chamava o da direita primeiro) e as duas leem o fluxo */
          rep0 += lz_direct(&rc, nd - 4) << 4;
          rep0 += lz_rtree(&rc, st->align, 4);
        }
      }
      if (rep0 == 0xFFFFFFFFu) break; /* marca de fim */
      if (rep0 >= op) break;
    }
    if (g_dbg > 1 && op < 1700) printf("M %u %d %u %u r=%x c=%x\n", op, state, len, rep0, rc.range, rc.code);
    len += 2;
    while (len-- && op < outlen) {
      out[op] = out[op - rep0 - 1];
      op++;
    }
  }
  free(st);
  free(lit);
  if (g_dbg)
    printf("[dbg] lzma lc=%u lp=%u pb=%u in=%u usado=%u bad=%d saida=%u de %u state=%d rep0=%u\n", lc, lp, pb,
           inlen, rc.i, rc.bad, op, outlen, state, rep0);
  if (op != outlen) return fail("7z: dados LZMA invalidos");
  return 0;
}

/* BCJ2 do 7-Zip (x86, 4 entradas): main, call, jump e o fluxo do range coder. */
static int bcj2_dec(const u8* b0, u32 n0, const u8* b1, u32 n1, const u8* b2, u32 n2, const u8* b3, u32 n3,
                    u8* out, u32 outlen) {
  u16 p[258];
  struct lzrc rc;
  u32 ip = 0, op = 0, i;
  u8 prev = 0;
  for (i = 0; i < 258; i++) p[i] = 1024;
  memset(&rc, 0, sizeof(rc));
  rc.p = b3;
  rc.n = n3;
  rc.range = 0xFFFFFFFFu;
  for (i = 0; i < 5; i++) rc.code = (rc.code << 8) | lz_in(&rc);
  while (op < outlen) {
    u8 b;
    u16* pr;
    while (ip < n0 && op < outlen) {
      b = b0[ip];
      out[op++] = b;
      if ((b & 0xFE) == 0xE8 || (prev == 0x0F && (b & 0xF0) == 0x80)) break;
      ip++;
      prev = b;
    }
    if (ip >= n0 || op >= outlen) break;
    b = b0[ip++];
    pr = b == 0xE8 ? p + prev : b == 0xE9 ? p + 256 : p + 257;
    if (!lz_bit(&rc, pr)) {
      prev = b;
    } else {
      const u8* v;
      u32 dest;
      if (b == 0xE8) {
        if (n1 < 4) return fail("7z: BCJ2 invalido");
        v = b1;
        b1 += 4;
        n1 -= 4;
      } else {
        if (n2 < 4) return fail("7z: BCJ2 invalido");
        v = b2;
        b2 += 4;
        n2 -= 4;
      }
      dest = (((u32)v[0] << 24) | ((u32)v[1] << 16) | ((u32)v[2] << 8) | v[3]) - (op + 4);
      for (i = 0; i < 4 && op < outlen; i++) out[op++] = (u8)(dest >> (8 * i));
      prev = (u8)(dest >> 24);
    }
  }
  if (rc.bad || op != outlen) return fail("7z: BCJ2 invalido");
  return 0;
}

/* ---------- 7z ---------- */

struct coder {
  u8 id[8];
  int idlen, nin, nout, nprops;
  u8 props[16];
};
struct folder {
  int nc, nbp, np, nin, nout;
  struct coder c[4];
  int bp_in[8], bp_out[8], packed[8];
  u64 unpack[8];
  int has_crc;
  u32 crc;
  int first_pack, nsub, first_sub, main_out;
};
struct sinfo {
  u64 packpos;
  int npack, nfold, nsubs;
  u64* psize;
  struct folder* fo;
  u64* ssize;   /* tamanho de cada subfluxo (arquivo com dados) */
  u8* sdef;     /* tem CRC */
  u32* scrc;
};
struct sfile {
  char* name;   /* relativo, com '\', sem o "Chrome-bin\" */
  int dir, stream;
  u64 size;
};

static struct sinfo A;
static struct sfile* F;
static int nF;

struct rdr {
  const u8* p;
  u32 n, i;
  int bad;
};
static u32 r_byte(struct rdr* r) {
  if (r->i >= r->n) {
    r->bad = 1;
    return 0;
  }
  return r->p[r->i++];
}
static u64 r_num(struct rdr* r) {
  u32 first = r_byte(r), mask = 0x80;
  u64 v = 0;
  int i;
  for (i = 0; i < 8; i++) {
    if (!(first & mask)) return v | ((u64)(first & (mask - 1)) << (8 * i));
    v |= (u64)r_byte(r) << (8 * i);
    mask >>= 1;
  }
  return v;
}
static u32 r_u32(struct rdr* r) {
  u32 v = r_byte(r);
  v |= r_byte(r) << 8;
  v |= r_byte(r) << 16;
  v |= r_byte(r) << 24;
  return v;
}
static void r_bits(struct rdr* r, int n, u8* out) {
  int i;
  u32 m = 0, v = 0;
  for (i = 0; i < n; i++) {
    if (!m) {
      v = r_byte(r);
      m = 0x80;
    }
    out[i] = (v & m) ? 1 : 0;
    m >>= 1;
  }
}
static void r_digests(struct rdr* r, int n, u8* def, u32* crc) {
  int i;
  if (r_byte(r)) memset(def, 1, (size_t)n);
  else r_bits(r, n, def);
  for (i = 0; i < n; i++) crc[i] = def[i] ? r_u32(r) : 0;
}

static int read_folder(struct rdr* r, struct folder* f) {
  int i, k;
  memset(f, 0, sizeof(*f));
  f->nc = (int)r_num(r);
  if (f->nc < 1 || f->nc > 4) return fail("7z: pasta com %d metodos", f->nc);
  for (i = 0; i < f->nc; i++) {
    struct coder* c = &f->c[i];
    u32 fl = r_byte(r);
    c->idlen = (int)(fl & 15);
    if (c->idlen > 8 || (fl & 0x80)) return fail("7z: metodo invalido");
    for (k = 0; k < c->idlen; k++) c->id[k] = (u8)r_byte(r);
    c->nin = c->nout = 1;
    if (fl & 0x10) {
      c->nin = (int)r_num(r);
      c->nout = (int)r_num(r);
    }
    if (fl & 0x20) {
      u64 np = r_num(r);
      if (np > sizeof(c->props)) return fail("7z: propriedades grandes demais");
      c->nprops = (int)np;
      for (k = 0; k < c->nprops; k++) c->props[k] = (u8)r_byte(r);
    }
    f->nin += c->nin;
    f->nout += c->nout;
  }
  if (f->nin > 8 || f->nout > 8 || f->nout < 1) return fail("7z: pasta grande demais");
  f->nbp = f->nout - 1;
  for (i = 0; i < f->nbp; i++) {
    f->bp_in[i] = (int)r_num(r);
    f->bp_out[i] = (int)r_num(r);
  }
  f->np = f->nin - f->nbp;
  if (f->np < 1) return fail("7z: pasta sem dados");
  if (f->np == 1) {
    for (i = 0; i < f->nin; i++) {
      for (k = 0; k < f->nbp && f->bp_in[k] != i; k++) {
      }
      if (k == f->nbp) break;
    }
    f->packed[0] = i;
  } else {
    for (i = 0; i < f->np; i++) f->packed[i] = (int)r_num(r);
  }
  for (i = 0; i < f->nout; i++) {
    for (k = 0; k < f->nbp && f->bp_out[k] != i; k++) {
    }
    if (k == f->nbp) break;
  }
  f->main_out = i;
  return r->bad ? fail("7z: cabecalho cortado") : 0;
}

static int read_streams(struct rdr* r, struct sinfo* s) {
  int i, k, have_sub = 0;
  memset(s, 0, sizeof(*s));
  for (;;) {
    u64 t = r_num(r);
    if (r->bad) return fail("7z: cabecalho cortado");
    if (t == 0) break;
    if (t == 6) {
      s->packpos = r_num(r);
      s->npack = (int)r_num(r);
      if (s->npack < 0 || s->npack > 100000) return fail("7z: fluxos demais");
      s->psize = (u64*)calloc((size_t)s->npack + 1, sizeof(u64));
      if (!s->psize) return fail("sem memoria");
      for (;;) {
        u64 t2 = r_num(r);
        if (r->bad) return fail("7z: cabecalho cortado");
        if (t2 == 0) break;
        if (t2 == 9) {
          for (i = 0; i < s->npack; i++) s->psize[i] = r_num(r);
        } else if (t2 == 10) {
          u8* d = (u8*)malloc((size_t)s->npack + 1);
          u32* c = (u32*)malloc(sizeof(u32) * ((size_t)s->npack + 1));
          if (!d || !c) return fail("sem memoria");
          r_digests(r, s->npack, d, c);
          free(d);
          free(c);
        } else {
          return fail("7z: PackInfo desconhecido (%d)", (int)t2);
        }
      }
    } else if (t == 7) {
      if (r_num(r) != 0x0b) return fail("7z: esperava kFolder");
      s->nfold = (int)r_num(r);
      if (s->nfold < 0 || s->nfold > 100000) return fail("7z: pastas demais");
      if (r_byte(r) != 0) return fail("7z: pastas externas nao suportadas");
      s->fo = (struct folder*)calloc((size_t)s->nfold + 1, sizeof(struct folder));
      if (!s->fo) return fail("sem memoria");
      for (i = 0; i < s->nfold; i++)
        if (read_folder(r, &s->fo[i]) != 0) return -1;
      if (r_num(r) != 0x0c) return fail("7z: esperava kCodersUnPackSize");
      for (i = 0; i < s->nfold; i++)
        for (k = 0; k < s->fo[i].nout; k++) s->fo[i].unpack[k] = r_num(r);
      for (;;) {
        u64 t2 = r_num(r);
        if (r->bad) return fail("7z: cabecalho cortado");
        if (t2 == 0) break;
        if (t2 == 10) {
          u8* d = (u8*)malloc((size_t)s->nfold + 1);
          u32* c = (u32*)malloc(sizeof(u32) * ((size_t)s->nfold + 1));
          if (!d || !c) return fail("sem memoria");
          r_digests(r, s->nfold, d, c);
          for (i = 0; i < s->nfold; i++) {
            s->fo[i].has_crc = d[i];
            s->fo[i].crc = c[i];
          }
          free(d);
          free(c);
        } else {
          return fail("7z: UnPackInfo desconhecido (%d)", (int)t2);
        }
      }
    } else if (t == 8) {
      int got_size = 0;
      have_sub = 1;
      for (i = 0; i < s->nfold; i++) s->fo[i].nsub = 1;
      for (;;) {
        u64 t2 = r_num(r);
        if (r->bad) return fail("7z: cabecalho cortado");
        if (t2 == 0) break;
        if (t2 == 0x0d) {
          for (i = 0; i < s->nfold; i++) s->fo[i].nsub = (int)r_num(r);
        } else if (t2 == 9 || t2 == 10) {
          if (!s->ssize) {
            for (i = 0, s->nsubs = 0; i < s->nfold; i++) {
              if (s->fo[i].nsub < 0 || s->fo[i].nsub > 100000) return fail("7z: subfluxos demais");
              s->fo[i].first_sub = s->nsubs;
              s->nsubs += s->fo[i].nsub;
            }
            s->ssize = (u64*)calloc((size_t)s->nsubs + 1, sizeof(u64));
            s->sdef = (u8*)calloc((size_t)s->nsubs + 1, 1);
            s->scrc = (u32*)calloc((size_t)s->nsubs + 1, sizeof(u32));
            if (!s->ssize || !s->sdef || !s->scrc) return fail("sem memoria");
          }
          if (t2 == 9) {
            got_size = 1;
            for (i = 0; i < s->nfold; i++) {
              struct folder* f = &s->fo[i];
              u64 sum = 0;
              if (!f->nsub) continue;
              for (k = 0; k < f->nsub - 1; k++) {
                s->ssize[f->first_sub + k] = r_num(r);
                sum += s->ssize[f->first_sub + k];
              }
              if (sum > f->unpack[f->main_out]) return fail("7z: tamanhos invalidos");
              s->ssize[f->first_sub + f->nsub - 1] = f->unpack[f->main_out] - sum;
            }
          } else {
            int nd = 0, j = 0;
            u8* d;
            u32* c;
            for (i = 0; i < s->nfold; i++)
              if (!(s->fo[i].nsub == 1 && s->fo[i].has_crc)) nd += s->fo[i].nsub;
            d = (u8*)malloc((size_t)nd + 1);
            c = (u32*)malloc(sizeof(u32) * ((size_t)nd + 1));
            if (!d || !c) return fail("sem memoria");
            r_digests(r, nd, d, c);
            for (i = 0; i < s->nfold; i++) {
              struct folder* f = &s->fo[i];
              if (f->nsub == 1 && f->has_crc) {
                s->sdef[f->first_sub] = 1;
                s->scrc[f->first_sub] = f->crc;
              } else {
                for (k = 0; k < f->nsub; k++, j++) {
                  s->sdef[f->first_sub + k] = d[j];
                  s->scrc[f->first_sub + k] = c[j];
                }
              }
            }
            free(d);
            free(c);
          }
        } else {
          return fail("7z: SubStreamsInfo desconhecido (%d)", (int)t2);
        }
      }
      if (!s->ssize) {
        have_sub = 0; /* nem tamanho nem CRC: monta abaixo */
        for (i = 0; i < s->nfold; i++)
          if (s->fo[i].nsub != 1) return fail("7z: subfluxos sem tamanho");
      } else if (!got_size) {
        for (i = 0; i < s->nfold; i++) {
          if (s->fo[i].nsub > 1) return fail("7z: subfluxos sem tamanho");
          if (s->fo[i].nsub == 1) s->ssize[s->fo[i].first_sub] = s->fo[i].unpack[s->fo[i].main_out];
        }
      }
    } else {
      return fail("7z: StreamsInfo desconhecido (%d)", (int)t);
    }
  }
  if (!have_sub && s->nfold) {
    s->nsubs = s->nfold;
    s->ssize = (u64*)calloc((size_t)s->nsubs + 1, sizeof(u64));
    s->sdef = (u8*)calloc((size_t)s->nsubs + 1, 1);
    s->scrc = (u32*)calloc((size_t)s->nsubs + 1, sizeof(u32));
    if (!s->ssize || !s->sdef || !s->scrc) return fail("sem memoria");
    for (i = 0; i < s->nfold; i++) {
      s->fo[i].nsub = 1;
      s->fo[i].first_sub = i;
      s->ssize[i] = s->fo[i].unpack[s->fo[i].main_out];
      s->sdef[i] = (u8)s->fo[i].has_crc;
      s->scrc[i] = s->fo[i].crc;
    }
  }
  for (i = 0, k = 0; i < s->nfold; i++) {
    s->fo[i].first_pack = k;
    k += s->fo[i].np;
  }
  if (k > s->npack) return fail("7z: pastas pedem mais fluxos do que ha");
  return 0;
}

static int is_copy(const struct folder* f) { return f->nc == 1 && f->c[0].idlen == 1 && f->c[0].id[0] == 0; }

/* Decodifica na memoria a saida out de uma pasta (LZMA, BCJ2, Copy). */
static u8* fold_out(const struct folder* f, u8** pk, const u64* pklen, int out, u64* olen, int depth) {
  int ci, ostart = 0, istart = 0, k, j;
  const struct coder* c = 0;
  u8* in[4];
  u64 inlen[4];
  u8* res;
  u64 n;
  if (depth > 8) {
    fail("7z: pasta em laco");
    return 0;
  }
  for (ci = 0; ci < f->nc; ci++) {
    if (out >= ostart && out < ostart + f->c[ci].nout) {
      c = &f->c[ci];
      break;
    }
    ostart += f->c[ci].nout;
    istart += f->c[ci].nin;
  }
  if (!c || c->nout != 1 || c->nin > 4) {
    fail("7z: metodo com varias saidas nao suportado");
    return 0;
  }
  n = f->unpack[out];
  if (n > FOLD_MAX) {
    fail("7z: pasta comprimida grande demais (%llu bytes)", n);
    return 0;
  }
  for (j = 0; j < c->nin; j++) {
    int ii = istart + j;
    in[j] = 0;
    for (k = 0; k < f->nbp; k++)
      if (f->bp_in[k] == ii) break;
    if (k < f->nbp) {
      in[j] = fold_out(f, pk, pklen, f->bp_out[k], &inlen[j], depth + 1);
      if (!in[j]) {
        while (j--) free(in[j]);
        return 0;
      }
      continue;
    }
    for (k = 0; k < f->np; k++)
      if (f->packed[k] == ii) break;
    if (k == f->np) {
      fail("7z: entrada sem dados");
      while (j--) free(in[j]);
      return 0;
    }
    inlen[j] = pklen[k];
    in[j] = (u8*)malloc((size_t)pklen[k] + 1);
    if (!in[j]) {
      fail("sem memoria");
      while (j--) free(in[j]);
      return 0;
    }
    memcpy(in[j], pk[k], (size_t)pklen[k]);
  }
  res = (u8*)malloc((size_t)n + 1);
  if (!res) {
    fail("sem memoria");
  } else if (c->idlen == 1 && c->id[0] == 0) {
    if (inlen[0] < n) fail("7z: dados curtos");
    else memcpy(res, in[0], (size_t)n);
  } else if (c->idlen == 3 && c->id[0] == 3 && c->id[1] == 1 && c->id[2] == 1) {
    lzma_dec(c->props, c->nprops, in[0], (u32)inlen[0], res, (u32)n);
  } else if (c->idlen == 4 && c->id[0] == 3 && c->id[1] == 3 && c->id[2] == 1 && c->id[3] == 0x1b && c->nin == 4) {
    bcj2_dec(in[0], (u32)inlen[0], in[1], (u32)inlen[1], in[2], (u32)inlen[2], in[3], (u32)inlen[3], res, (u32)n);
  } else {
    fail("7z: metodo de compressao nao suportado (atualize o INSTCHRM)");
  }
  for (j = 0; j < c->nin; j++) free(in[j]);
  if (g_fail) {
    free(res);
    return 0;
  }
  *olen = n;
  return res;
}

/* UTF-16LE (com 0 no fim) para UTF-8. */
static char* utf16_dup(const u8* p, u32 maxbytes, u32* used) {
  u32 i = 0, n = 0;
  char* s;
  while (i + 1 < maxbytes && (p[i] | p[i + 1])) i += 2;
  *used = i + 2;
  s = (char*)malloc(i * 2 + 4);
  if (!s) return 0;
  for (n = 0, i = 0; i + 1 < maxbytes && (p[i] | p[i + 1]); i += 2) {
    u32 c = rd16(p + i);
    if (c >= 0xD800 && c < 0xDC00 && i + 3 < maxbytes) {
      u32 c2 = rd16(p + i + 2);
      if (c2 >= 0xDC00 && c2 < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
        i += 2;
      }
    }
    if (c < 0x80) {
      s[n++] = (char)c;
    } else if (c < 0x800) {
      s[n++] = (char)(0xC0 | (c >> 6));
      s[n++] = (char)(0x80 | (c & 63));
    } else if (c < 0x10000) {
      s[n++] = (char)(0xE0 | (c >> 12));
      s[n++] = (char)(0x80 | ((c >> 6) & 63));
      s[n++] = (char)(0x80 | (c & 63));
    } else {
      s[n++] = (char)(0xF0 | (c >> 18));
      s[n++] = (char)(0x80 | ((c >> 12) & 63));
      s[n++] = (char)(0x80 | ((c >> 6) & 63));
      s[n++] = (char)(0x80 | (c & 63));
    }
  }
  s[n] = 0;
  return s;
}

/* "Chrome-bin/158.0.8127.89/x.dll" -> "158.0.8127.89\x.dll"; recusa .. e : */
static int fix_name(char* s) {
  char* p;
  int n;
  for (p = s; *p; p++)
    if (*p == '/') *p = '\\';
  if (ci_start(s, "Chrome-bin\\")) memmove(s, s + 11, strlen(s + 11) + 1);
  else if (!strcmp(s, "Chrome-bin")) s[0] = 0;
  if (strchr(s, ':') || s[0] == '\\') return -1;
  for (p = s; *p;) {
    char* e = strchr(p, '\\');
    n = e ? (int)(e - p) : (int)strlen(p);
    if (n == 0 || (n == 2 && p[0] == '.' && p[1] == '.') || (n == 1 && p[0] == '.')) return -1;
    p += n;
    if (*p) p++;
  }
  return 0;
}

static int read_files(struct rdr* r) {
  u8 *es = 0, *ef = 0;
  int i, nempty = 0, ei, si;
  nF = (int)r_num(r);
  if (nF < 1 || nF > 100000) return fail("7z: %d arquivos", nF);
  F = (struct sfile*)calloc((size_t)nF, sizeof(struct sfile));
  es = (u8*)calloc((size_t)nF, 1);
  ef = (u8*)calloc((size_t)nF, 1);
  if (!F || !es || !ef) return fail("sem memoria");
  for (;;) {
    u64 t = r_num(r), sz;
    u32 start;
    struct rdr sub;
    if (r->bad) return fail("7z: cabecalho cortado");
    if (t == 0) break;
    sz = r_num(r);
    if (sz > r->n - r->i) return fail("7z: propriedade cortada");
    start = r->i;
    sub.p = r->p + start;
    sub.n = (u32)sz;
    sub.i = 0;
    sub.bad = 0;
    if (t == 0x0e) {
      r_bits(&sub, nF, es);
      for (i = 0, nempty = 0; i < nF; i++) nempty += es[i];
    } else if (t == 0x0f) {
      r_bits(&sub, nempty, ef);
    } else if (t == 0x11) {
      u32 off = 1, used;
      if (sz < 1 || sub.p[0] != 0) return fail("7z: nomes externos nao suportados");
      for (i = 0; i < nF; i++) {
        if (off >= sz) return fail("7z: nomes cortados");
        F[i].name = utf16_dup(sub.p + off, (u32)sz - off, &used);
        if (!F[i].name) return fail("sem memoria");
        off += used;
      }
    }
    if (sub.bad) return fail("7z: propriedade %d cortada", (int)t);
    r->i = start + (u32)sz;
  }
  for (i = 0, ei = 0, si = 0; i < nF; i++) {
    if (!F[i].name) return fail("7z: arquivo sem nome");
    if (fix_name(F[i].name) != 0) return fail("7z: nome recusado: %s", F[i].name);
    F[i].stream = -1;
    if (es[i]) {
      F[i].dir = !ef[ei++];
    } else {
      if (si >= A.nsubs) return fail("7z: mais arquivos do que dados");
      F[i].stream = si;
      F[i].size = A.ssize[si];
      si++;
    }
  }
  free(es);
  free(ef);
  if (si != A.nsubs) return fail("7z: dados sem arquivo");
  return 0;
}

/* ---------- leitura previa: PE, recurso CHROME.7Z e cabecalho do 7z ---------- */

static u8* g_head;
static u32 g_head_len;
static u8* g_tail;
static u64 g_tail_off;
static u32 g_tail_len;
static u64 g_7z;      /* offset do 7z no instalador */
static u64 g_7z_size;

static int pre_range(u64 off, u32 len, u8* out) {
  if (off + len <= g_head_len) {
    memcpy(out, g_head + off, len);
    return 0;
  }
  if (g_tail && off >= g_tail_off && off + len <= g_tail_off + g_tail_len) {
    memcpy(out, g_tail + (off - g_tail_off), len);
    return 0;
  }
  return src_range(off, len, out);
}

static int pe_find_7z(void) {
  const u8* b = g_head;
  u32 n = g_head_len, pe, nsec, optsz, opt, dd, rva, base = 0, i;
  u32 sva[16], ssz[16], sraw[16];
  int ns, found = 0;
  if (n < 0x200 || b[0] != 'M' || b[1] != 'Z') return fail("o instalador nao e um EXE");
  pe = rd32(b + 0x3c);
  if (pe + 24 + 240 > n || memcmp(b + pe, "PE\0\0", 4) != 0) return fail("o instalador nao e PE");
  nsec = rd16(b + pe + 6);
  optsz = rd16(b + pe + 20);
  opt = pe + 24;
  dd = opt + (rd16(b + opt) == 0x20b ? 112 : 96);
  rva = rd32(b + dd + 16);
  if (!rva || nsec > 16 || opt + optsz + nsec * 40 > n) return fail("instalador sem recursos");
  ns = (int)nsec;
  for (i = 0; i < nsec; i++) {
    const u8* s = b + opt + optsz + 40 * i;
    sva[i] = rd32(s + 12);
    ssz[i] = rd32(s + 8) > rd32(s + 16) ? rd32(s + 8) : rd32(s + 16);
    sraw[i] = rd32(s + 20);
    if (rva >= sva[i] && rva < sva[i] + ssz[i]) base = rva - sva[i] + sraw[i];
  }
  if (!base || base + 16 > n) return fail("recursos fora do comeco do instalador");
  /* tipo -> nome -> idioma -> dado; procura um nome terminado em .7Z */
  {
    u32 t0 = base, nt = rd16(b + t0 + 12) + rd16(b + t0 + 14), it;
    for (it = 0; it < nt && !found; it++) {
      u32 e = t0 + 16 + 8 * it, t1, nn, in_;
      if (e + 8 > n || !(rd32(b + e + 4) & 0x80000000u)) continue;
      t1 = base + (rd32(b + e + 4) & 0x7FFFFFFFu);
      if (t1 + 16 > n) continue;
      nn = rd16(b + t1 + 12) + rd16(b + t1 + 14);
      for (in_ = 0; in_ < nn && !found; in_++) {
        u32 e1 = t1 + 16 + 8 * in_, nm, l, k, t2, ed, drva, dsz, doff = 0;
        char name[64];
        int si;
        if (e1 + 8 > n || !(rd32(b + e1) & 0x80000000u) || !(rd32(b + e1 + 4) & 0x80000000u)) continue;
        nm = base + (rd32(b + e1) & 0x7FFFFFFFu);
        if (nm + 2 > n) continue;
        l = rd16(b + nm);
        if (l >= sizeof(name) || nm + 2 + 2 * l > n) continue;
        for (k = 0; k < l; k++) name[k] = (char)b[nm + 2 + 2 * k];
        name[l] = 0;
        if (l < 3 || !ci_start(name + l - 3, ".7Z")) continue;
        t2 = base + (rd32(b + e1 + 4) & 0x7FFFFFFFu);
        if (t2 + 24 > n) continue;
        ed = base + (rd32(b + t2 + 16 + 4) & 0x7FFFFFFFu);
        if (ed + 8 > n) continue;
        drva = rd32(b + ed);
        dsz = rd32(b + ed + 4);
        for (si = 0; si < ns; si++)
          if (drva >= sva[si] && drva < sva[si] + ssz[si]) doff = drva - sva[si] + sraw[si];
        if (!doff) continue;
        g_7z = doff;
        g_7z_size = dsz;
        found = 1;
      }
    }
  }
  if (!found) return fail("o instalador nao tem o recurso CHROME.7Z");
  if (src_size && g_7z + g_7z_size > src_size) return fail("recurso CHROME.7Z fora do arquivo");
  return 0;
}

static int read_7z(void) {
  u8 sh[32];
  u64 nho, nhs;
  u8* hdr;
  u32 hlen;
  struct rdr r;
  int pass;
  if (pre_range(g_7z, 32, sh) != 0) return -1;
  if (memcmp(sh, "7z\xBC\xAF\x27\x1C", 6) != 0) return fail("CHROME.7Z nao e um 7z");
  nho = rd64(sh + 12);
  nhs = rd64(sh + 20);
  if (nhs > (16u << 20) || 32 + nho + nhs > g_7z_size) return fail("7z: cabecalho invalido");
  {
    u64 end = g_7z + 32 + nho + nhs;
    u64 ts = end > g_7z + TAIL_LEN ? end - TAIL_LEN : g_7z;
    g_tail_len = (u32)(end - ts);
    g_tail = (u8*)malloc(g_tail_len);
    if (!g_tail) return fail("sem memoria");
    if (src_range(ts, g_tail_len, g_tail) != 0) return -1;
    g_tail_off = ts;
  }
  hlen = (u32)nhs;
  hdr = (u8*)malloc(hlen + 1);
  if (!hdr || pre_range(g_7z + 32 + nho, hlen, hdr) != 0) return g_fail ? -1 : fail("sem memoria");
  if (crc_upd(0, hdr, hlen) != rd32(sh + 28)) return fail("7z: CRC do cabecalho nao confere");
  for (pass = 0; pass < 4; pass++) {
    r.p = hdr;
    r.n = hlen;
    r.i = 0;
    r.bad = 0;
    if (hlen && hdr[0] == 0x17) {
      /* cabecalho comprimido: descreve onde esta e como abrir o de verdade */
      struct sinfo hs;
      u64 out_n;
      u8* outb;
      r.i = 1;
      if (read_streams(&r, &hs) != 0) return -1;
      if (hs.nfold != 1 || hs.npack < 1) return fail("7z: cabecalho comprimido estranho");
      {
        struct folder* f = &hs.fo[0];
        u8* pks[8];
        int k;
        u64 off = g_7z + 32 + hs.packpos;
        for (k = 0; k < f->np; k++) {
          if (hs.psize[k] > (16u << 20)) return fail("7z: cabecalho grande demais");
          pks[k] = (u8*)malloc((size_t)hs.psize[k] + 1);
          if (!pks[k] || pre_range(off, (u32)hs.psize[k], pks[k]) != 0) return g_fail ? -1 : fail("sem memoria");
          off += hs.psize[k];
        }
        if (g_dbg)
          printf("[dbg] cab: packpos=%llu np=%d psize=%llu unpack=%llu coder=%d ids=%02x%02x%02x props=%02x%02x%02x%02x%02x "
                 "dados=%02x%02x%02x%02x%02x%02x\n",
                 hs.packpos, f->np, hs.psize[0], f->unpack[0], f->nc, f->c[0].id[0], f->c[0].id[1], f->c[0].id[2],
                 f->c[0].props[0], f->c[0].props[1], f->c[0].props[2], f->c[0].props[3], f->c[0].props[4], pks[0][0],
                 pks[0][1], pks[0][2], pks[0][3], pks[0][4], pks[0][5]);
        outb = fold_out(f, pks, hs.psize, f->main_out, &out_n, 0);
        for (k = 0; k < f->np; k++) free(pks[k]);
        if (!outb) return -1;
        if (f->has_crc && crc_upd(0, outb, (u32)out_n) != f->crc) return fail("7z: CRC do cabecalho nao confere");
      }
      free(hdr);
      hdr = outb;
      hlen = (u32)out_n;
      continue;
    }
    break;
  }
  if (!hlen || hdr[0] != 0x01) return fail("7z: cabecalho desconhecido");
  r.i = 1;
  for (;;) {
    u64 t = r_num(&r);
    if (r.bad) return fail("7z: cabecalho cortado");
    if (t == 0) break;
    if (t == 4) {
      if (read_streams(&r, &A) != 0) return -1;
    } else if (t == 5) {
      if (read_files(&r) != 0) return -1;
    } else {
      return fail("7z: parte do cabecalho nao suportada (%d)", (int)t);
    }
  }
  free(hdr);
  if (!nF) return fail("7z: sem arquivos");
  return 0;
}

/* ---------- gravacao enquanto baixa ---------- */

struct piece {
  u64 start, size;
  int file;  /* >= 0: arquivo; -1: fluxo de uma pasta comprimida */
  int fold, pack;
};
static struct piece* P;
static int nP;

static char g_ver[64];
static char g_dest[PATH_N];

/* estado da passada */
static u64 s_pos;
static sha_t s_sha;
static int s_cur;
static u64 s_done;     /* do pedaco atual */
static int s_open;     /* o pedaco atual ja comecou (piece_begin) */
static FILE* s_out;
static u8* s_mem;      /* arquivo da raiz ou fluxo comprimido, na memoria */
static u32 s_crc;
static u64 s_written, s_total;
static int s_pct;
static unsigned s_t0;
static u8** s_pk;      /* fluxos da pasta comprimida atual */
static u64* s_pklen;

static void path_of(const char* rel, char* out) { snprintf(out, PATH_N, "%s%s%s", g_dest, rel[0] ? "\\" : "", rel); }

static int mkdir_p(const char* path) {
  char tmp[PATH_N];
  int i, n = (int)strlen(path);
  if (n >= PATH_N) return fail("caminho grande demais: %s", path);
  for (i = 3; i <= n; i++) {
    if (path[i] == '\\' || path[i] == 0) {
      memcpy(tmp, path, (size_t)i);
      tmp[i] = 0;
      if (k_mkdir(tmp) < 0) return fail("nao consegui criar a pasta %s", tmp);
    }
  }
  return 0;
}

static int file_done(int fi, u32 crc) {
  const struct sfile* f = &F[fi];
  if (A.sdef[f->stream] && A.scrc[f->stream] != crc) return fail("CRC nao confere: %s", f->name);
  s_written += f->size;
  return 0;
}

/* Grava um arquivo decodificado inteiro (pastas comprimidas). */
static int write_whole(int fi, const u8* d, u64 n) {
  char path[PATH_N];
  u32 crc = crc_upd(0, d, (u32)n);
  path_of(F[fi].name, path);
  if (k_fsave(path, (const char*)d, (int)n) < 0) return fail("nao consegui gravar %s", path);
  return file_done(fi, crc);
}

static int fold_done(int fo) {
  const struct folder* f = &A.fo[fo];
  u64 n, off = 0;
  u8* d = fold_out(f, s_pk, s_pklen, f->main_out, &n, 0);
  int k, i;
  for (k = 0; k < f->np; k++) {
    free(s_pk[k]);
    s_pk[k] = 0;
  }
  if (!d) return -1;
  for (k = 0; k < f->nsub; k++) {
    int si = f->first_sub + k;
    for (i = 0; i < nF && F[i].stream != si; i++) {
    }
    if (i == nF || off + A.ssize[si] > n) {
      free(d);
      return fail("7z: pasta sem o arquivo esperado");
    }
    if (write_whole(i, d + off, A.ssize[si]) != 0) {
      free(d);
      return -1;
    }
    off += A.ssize[si];
  }
  free(d);
  return 0;
}

static int piece_begin(struct piece* pc) {
  char path[PATH_N];
  s_crc = 0;
  s_mem = 0;
  s_out = 0;
  if (pc->file < 0) {
    s_pk[pc->pack] = (u8*)malloc((size_t)pc->size + 1);
    if (!s_pk[pc->pack]) return fail("sem memoria");
    s_pklen[pc->pack] = pc->size;
    s_mem = s_pk[pc->pack];
    return 0;
  }
  path_of(F[pc->file].name, path);
  s_out = fopen(path, "wb");
  if (!s_out) return fail("nao consegui criar %s", path);
  return 0;
}

static int piece_data(struct piece* pc, const u8* p, u32 n) {
  if (pc->file >= 0) s_crc = crc_upd(s_crc, p, n);
  if (s_mem) {
    memcpy(s_mem + s_done, p, n);
  } else if (s_out && fwrite(p, 1, n, s_out) != n) {
    return fail("erro gravando %s (disco cheio?)", F[pc->file].name);
  }
  return 0;
}

static int piece_end(struct piece* pc) {
  if (s_out) {
    if (fclose(s_out) != 0) {
      s_out = 0;
      return fail("erro gravando %s (disco cheio?)", F[pc->file].name);
    }
    s_out = 0;
  }
  s_mem = 0;
  if (pc->file >= 0) return file_done(pc->file, s_crc);
  if (pc->pack == A.fo[pc->fold].np - 1) return fold_done(pc->fold);
  return 0;
}

static void progress(void) {
  int pct = s_total ? (int)(s_pos * 100 / s_total) : 0;
  if (pct >= s_pct + 5 || s_pos == s_total) {
    unsigned ms = k_ms() - s_t0;
    unsigned kbs = ms ? (unsigned)(s_pos / ms) : 0;
    s_pct = pct - pct % 5;
    printf("  gravado %3d%%  %4llu de %llu MB  %u.%u MB/s\n", pct, s_pos >> 20, s_total >> 20, kbs / 1000,
           (kbs % 1000) / 100);
  }
}

/* Recebe o instalador em ordem, do byte 0 ao fim. */
static int sink(const u8* p, u32 n) {
  if (g_fail) return -1;
  sha_upd(&s_sha, p, n);
  /* o que foi lido antes (por Range) tem que ser o que passa agora */
  if (s_pos < g_head_len) {
    u32 k = g_head_len - (u32)s_pos < n ? g_head_len - (u32)s_pos : n;
    if (memcmp(g_head + s_pos, p, k) != 0) return fail("o instalador mudou durante o download");
  }
  if (g_tail && s_pos + n > g_tail_off && s_pos < g_tail_off + g_tail_len) {
    u64 a = s_pos > g_tail_off ? s_pos : g_tail_off;
    u64 b = s_pos + n < g_tail_off + g_tail_len ? s_pos + n : g_tail_off + g_tail_len;
    if (memcmp(g_tail + (a - g_tail_off), p + (a - s_pos), (size_t)(b - a)) != 0)
      return fail("o instalador mudou durante o download");
  }
  while (!g_fail && s_cur < nP) {
    struct piece* pc = &P[s_cur];
    u32 take;
    if (s_pos < pc->start) {
      u64 skip = pc->start - s_pos;
      if (!n) break;
      if (skip > n) skip = n;
      p += skip;
      n -= (u32)skip;
      s_pos += skip;
      continue;
    }
    if (!s_open) {
      if (pc->size && !n) break; /* comeca quando chegar o primeiro byte */
      if (piece_begin(pc) != 0) break;
      s_open = 1;
    }
    take = pc->size - s_done < n ? (u32)(pc->size - s_done) : n;
    if (take && piece_data(pc, p, take) != 0) break;
    p += take;
    n -= take;
    s_pos += take;
    s_done += take;
    if (s_done == pc->size) {
      s_open = 0;
      if (piece_end(pc) != 0) break;
      s_cur++;
      s_done = 0;
      continue;
    }
    if (!n) break;
  }
  if (s_cur >= nP) s_pos += n; /* depois do ultimo arquivo (setup.exe, icones) */
  progress();
  return g_fail ? -1 : 0;
}

static void pass_reset(void) {
  int i;
  if (s_out) fclose(s_out);
  s_out = 0;
  s_mem = 0;
  s_pos = 0;
  s_cur = 0;
  s_done = 0;
  s_open = 0;
  s_written = 0;
  s_pct = -5;
  s_t0 = k_ms();
  sha_init(&s_sha);
  for (i = 0; i < 8; i++) {
    free(s_pk[i]);
    s_pk[i] = 0;
  }
}

static int build_pieces(void) {
  int fo, k, i, max = 0;
  u64 pos = g_7z + 32 + A.packpos;
  u64* pstart = (u64*)calloc((size_t)A.npack + 1, sizeof(u64));
  int* by_stream = (int*)malloc(sizeof(int) * ((size_t)A.nsubs + 1));
  if (!pstart || !by_stream) return fail("sem memoria");
  for (k = 0; k < A.npack; k++) {
    pstart[k] = pos;
    pos += A.psize[k];
  }
  if (pos > g_7z + g_7z_size) return fail("7z: dados alem do fim");
  for (i = 0; i < A.nsubs; i++) by_stream[i] = -1;
  for (i = 0; i < nF; i++)
    if (F[i].stream >= 0) by_stream[F[i].stream] = i;
  for (fo = 0; fo < A.nfold; fo++) max += A.fo[fo].nsub + A.fo[fo].np;
  P = (struct piece*)calloc((size_t)max + 1, sizeof(struct piece));
  if (!P) return fail("sem memoria");
  nP = 0;
  for (fo = 0; fo < A.nfold; fo++) {
    struct folder* f = &A.fo[fo];
    if (f->np > 8) return fail("7z: pasta com fluxos demais");
    if (is_copy(f)) {
      u64 off = pstart[f->first_pack];
      if (A.psize[f->first_pack] != f->unpack[0]) return fail("7z: Copy com tamanhos diferentes");
      for (k = 0; k < f->nsub; k++) {
        int si = f->first_sub + k;
        struct piece* pc = &P[nP++];
        pc->start = off;
        pc->size = A.ssize[si];
        pc->file = by_stream[si];
        pc->fold = fo;
        if (pc->file < 0) return fail("7z: dado sem arquivo");
        off += A.ssize[si];
      }
    } else if (f->nsub > 0) {
      for (k = 0; k < f->np; k++) {
        struct piece* pc = &P[nP++];
        pc->start = pstart[f->first_pack + k];
        pc->size = A.psize[f->first_pack + k];
        pc->file = -1;
        pc->fold = fo;
        pc->pack = k;
        if (pc->size > FOLD_MAX) return fail("7z: pasta comprimida grande demais");
      }
    }
  }
  free(pstart);
  free(by_stream);
  return 0;
}

/* Arquivos vazios (sem dados no 7z). */
static int make_empty(void) {
  char path[PATH_N];
  int i;
  for (i = 0; i < nF; i++) {
    if (F[i].dir || F[i].stream >= 0) continue;
    path_of(F[i].name, path);
    if (k_fsave(path, "", 0) < 0) return fail("nao consegui criar %s", path);
  }
  return 0;
}

/* ---------- Omaha: qual a versao estavel e onde baixar ---------- */

static char o_sha[80];

static int attr(const char* tag, const char* name, char* out, int max) {
  const char* end = strchr(tag, '>');
  int nl = (int)strlen(name), n = 0;
  const char* p = tag;
  while ((p = strstr(p, name)) != 0 && (!end || p < end)) {
    if ((p == tag || p[-1] == ' ') && p[nl] == '=' && p[nl + 1] == '"') {
      p += nl + 2;
      while (*p && *p != '"' && n < max - 1) out[n++] = *p++;
      out[n] = 0;
      return 1;
    }
    p += nl;
  }
  out[0] = 0;
  return 0;
}

static int safe_token(const char* s, const char* extra) {
  if (!*s) return 0;
  for (; *s; s++)
    if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || strchr(extra, *s)))
      return 0;
  return 1;
}

static int omaha(void) {
  static const char req[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<request protocol=\"3.0\" updater=\"Omaha\" ismachine=\"1\" installsource=\"ondemand\">"
      "<os platform=\"win\" version=\"10.0.19045\" arch=\"x64\"/>"
      "<app appid=\"" APPID "\" version=\"\" ap=\"x64-stable-multi-chrome\" lang=\"en\">"
      "<updatecheck/></app></request>";
  static char resp[65536];
  char name[200], size[32], code[700], v[32];
  const char *p, *pkg, *man, *best = 0;
  int n = 0;
  if (http_req(OMAHA_URL, "POST", "Content-Type: application/xml\r\n", req, (int)strlen(req)) != 0) return -1;
  if (R.status != 200) {
    http_close();
    return fail("tools.google.com respondeu HTTP %d", R.status);
  }
  for (;;) {
    int k = http_body((u8*)resp + n, (int)sizeof(resp) - 1 - n);
    if (k <= 0) break;
    n += k;
    if (n >= (int)sizeof(resp) - 1) break;
  }
  http_close();
  if (g_fail) return -1;
  resp[n] = 0;
  p = strstr(resp, "<updatecheck");
  if (!p || !attr(p, "status", v, (int)sizeof(v)) || strcmp(v, "ok") != 0)
    return fail("o Google nao ofereceu o Chrome (updatecheck %s)", p ? v : "ausente");
  man = strstr(resp, "<manifest");
  pkg = strstr(resp, "<package ");
  if (!man || !pkg || !attr(man, "version", g_ver, (int)sizeof(g_ver)) || !attr(pkg, "name", name, (int)sizeof(name)) ||
      !attr(pkg, "size", size, (int)sizeof(size)) || !attr(pkg, "hash_sha256", o_sha, (int)sizeof(o_sha)))
    return fail("resposta do Google sem versao/pacote");
  if (!safe_token(g_ver, ".") || !safe_token(name, "._-") || strlen(o_sha) != 64)
    return fail("resposta do Google estranha (versao %s)", g_ver);
  src_size = (u64)atoll(size);
  if (src_size < (1u << 20)) return fail("pacote pequeno demais (%s bytes)", size);
  for (p = resp; (p = strstr(p, "codebase=\"")) != 0; p += 10) {
    if (ci_start(p + 10, "https://dl.google.com/")) {
      best = p;
      break;
    }
    if (!best && ci_start(p + 10, "https://")) best = p;
  }
  if (!best) return fail("resposta do Google sem endereco https");
  attr(best, "codebase", code, (int)sizeof(code));
  snprintf(src_url, sizeof(src_url), "%s%s%s", code, code[strlen(code) - 1] == '/' ? "" : "/", name);
  return 0;
}

/* ---------- pastas: versoes antigas ---------- */

static int is_version(const char* s) {
  int dots = 0, dig = 0;
  for (; *s; s++) {
    if (*s == '.') {
      if (!dig) return 0;
      dots++;
      dig = 0;
    } else if (*s >= '0' && *s <= '9') {
      dig++;
    } else {
      return 0;
    }
  }
  return dots == 3 && dig;
}

static int rmtree(const char* path, int depth) {
  struct k_direntl* e;
  int n, i;
  if (depth > 12) return -1;
  e = (struct k_direntl*)malloc(sizeof(struct k_direntl) * 2048);
  if (!e) return -1;
  n = k_dir_long(path, e, 2048);
  for (i = 0; i < n; i++) {
    char sub[PATH_N];
    if (!strcmp(e[i].name, ".") || !strcmp(e[i].name, "..")) continue;
    snprintf(sub, sizeof(sub), "%s\\%s", path, e[i].name);
    if (e[i].dir) rmtree(sub, depth + 1);
    else k_fdelete(sub);
  }
  free(e);
  return k_rmdir(path);
}

static void remove_old(void) {
  struct k_direntl* e = (struct k_direntl*)malloc(sizeof(struct k_direntl) * 256);
  char p[PATH_N];
  int n, i;
  if (!e) return;
  n = k_dir_long(g_dest, e, 256);
  for (i = 0; i < n; i++) {
    char sub[PATH_N];
    if (!e[i].dir || !is_version(e[i].name) || !strcmp(e[i].name, g_ver)) continue;
    snprintf(sub, sizeof(sub), "%s\\%s", g_dest, e[i].name);
    printf("apagando a versao antiga %s\n", e[i].name);
    rmtree(sub, 0);
  }
  free(e);
  /* chrome.exe na raiz (como o Chrome-bin) nao abre no WINE: ver write_launchers */
  snprintf(p, sizeof(p), "%s\\chrome.exe", g_dest);
  k_fdelete(p);
  snprintf(p, sizeof(p), "%s\\chrome_proxy.exe", g_dest);
  k_fdelete(p);
}

/* A versao instalada mais nova (a pasta com chrome.exe e chrome.dll), em g_ver. */
static int find_installed(void) {
  struct k_direntl* e = (struct k_direntl*)malloc(sizeof(struct k_direntl) * 256);
  int n, i, best = -1;
  if (!e) return -1;
  n = k_dir_long(g_dest, e, 256);
  for (i = 0; i < n; i++) {
    char p[PATH_N];
    if (!e[i].dir || !is_version(e[i].name)) continue;
    snprintf(p, sizeof(p), "%s\\%s\\chrome.exe", g_dest, e[i].name);
    if (k_fsize(p) <= 0) continue;
    snprintf(p, sizeof(p), "%s\\%s\\chrome.dll", g_dest, e[i].name);
    if (k_fsize(p) <= 0) continue;
    if (best < 0 || atoi(e[i].name) > atoi(e[best].name)) best = i;
  }
  if (best >= 0) strcpy(g_ver, e[best].name);
  free(e);
  return best >= 0 ? 0 : -1;
}

/* ---------- RODACHROME.BAT e atalho ----------
 * O chrome.exe fica na pasta da versao, ao lado do chrome_elf.dll que ele
 * importa: no Windows o manifesto da pasta da versao (assembly privado)
 * indica onde estao as DLLs, e o WINE.DLL so procura na pasta do EXE e no
 * SYSTEM32. Assim o Chromium roda como num build de desenvolvimento: sem
 * <exe>\<versao>\chrome.dll, carrega o chrome.dll ao lado do EXE. */

static const char bat_fmt[] =
    "@ECHO OFF\r\n"
    "REM RODACHROME.BAT - abre o Google Chrome pelo WINE.DLL (SET WINE=1).\r\n"
    "REM Gravado pelo INSTCHRM.EXE com a versao instalada (instalar de novo\r\n"
    "REM regrava). Se a pasta falta, o INSTCHRM baixa do Google e instala.\r\n"
    "IF NOT EXIST " DEST "\\%s\\chrome.exe GOTO INSTALA\r\n"
    "SET WINE=1\r\n"
    "SET HIMEM=80\r\n"
    "REM Perfil no ramdisk Z: (rapido, sem gravar no D:); o login e o perfil\r\n"
    "REM sao salvos em D:\\CHROME.SAV e voltam no proximo boot (profsave.c)\r\n"
    "SET WINEUDSAVE=D:\\CHROME.SAV\r\n"
    "REM Opcoes do Chromium so no chrome.exe de cima (os filhos recebem dele)\r\n"
    "SET WINE_ARGS_EXE=chrome.exe\r\n"
    "SET WINE_ARGS=--no-sandbox --user-data-dir=Z:\\CHROMEUD --no-first-run --no-default-browser-check "
    "--disable-gpu --disable-software-rasterizer --use-gl=disabled\r\n"
    "SET WINE_ARGS2=--disable-3d-apis --disable-webgl --disable-webgpu --disable-accelerated-2d-canvas "
    "--disable-gpu-rasterization --ipc-connection-timeout=300\r\n"
    "SET WINE_ARGS3=\r\n"
    "WIN " DEST "\\%s\\chrome.exe\r\n"
    "GOTO FIM\r\n"
    ":INSTALA\r\n"
    "D:\\INSTCHRM.EXE\r\n"
    "IF ERRORLEVEL 1 GOTO FIM\r\n"
    "REM o INSTCHRM regravou este BAT com a versao nova\r\n"
    "CALL D:\\RODACHROME.BAT\r\n"
    ":FIM\r\n";

static int write_launchers(void) {
  static char bat_text[4096];
  int rc = 0;
  snprintf(bat_text, sizeof(bat_text), bat_fmt, g_ver, g_ver);
  if (k_fsave(BAT_PATH, bat_text, (int)strlen(bat_text)) < 0) {
    printf("nao consegui gravar %s\n", BAT_PATH);
    rc = 1;
  } else {
    printf("gravado %s (Chrome %s)\n", BAT_PATH, g_ver);
  }
  if (k_fsize(LNK_PATH) < 0) {
    static const char lnk[] = "D:\\RODACHROME.BAT\r\n";
    k_mkdir("D:\\USER");
    k_mkdir("D:\\USER\\DEFAULT");
    k_mkdir("D:\\USER\\DEFAULT\\DESKTOP");
    if (k_fsave(LNK_PATH, lnk, (int)strlen(lnk)) >= 0) printf("atalho no desktop: %s\n", LNK_PATH);
  }
  return rc;
}

/* ---------- principal ---------- */

static void usage(void) {
  printf("INSTCHRM - instala o Google Chrome (x64) para o WINE em %s\n", DEST);
  printf("  INSTCHRM         baixa a versao estavel do Google e instala\n");
  printf("  INSTCHRM -r      instala de novo mesmo se ja esta instalada\n");
  printf("  INSTCHRM -n      so mostra a versao oferecida\n");
  printf("  INSTCHRM -f ARQ  instala de um instalador ja baixado (sem rede)\n");
  printf("  INSTCHRM -b      so grava o %s e o atalho\n", BAT_PATH);
  printf("Depois: %s (ou o atalho CHROME no desktop)\n", BAT_PATH);
}

static int installed(const char* ver) {
  char p[PATH_N];
  snprintf(p, sizeof(p), "%s\\%s\\chrome.dll", g_dest, ver);
  if (k_fsize(p) <= 0) return 0;
  snprintf(p, sizeof(p), "%s\\%s\\chrome.exe", g_dest, ver);
  return k_fsize(p) > 0;
}

/* ---------- download para a memoria ----------
 * Baixar e gravar ao mesmo tempo perde a conexao num disco lento: a rede do
 * SODOS so le a placa dentro do recv (nic_poll), e enquanto o programa grava
 * os pacotes se perdem (o anel da ne2000 enche em ~20 ms); o servidor
 * retransmite cada vez mais devagar e o recv desiste com 8 s sem dados. Na VM
 * (disco IDE por PIO, ~600 KB/s) a ne2000 ainda parava de receber de vez.
 * Entao o pacote (~253 MB em gzip) vem inteiro para a memoria, em blocos, e
 * so depois e descompactado e gravado. Sem memoria: grava enquanto baixa. */

#define DL_BLK (4u << 20)
static u8** dl_blk;
static int dl_nblk, dl_cap;
static u64 dl_len, dl_rd;
static int dl_gzip, dl_ok, dl_nomem;

static void dl_free(void) {
  int i;
  for (i = 0; i < dl_nblk; i++) free(dl_blk[i]);
  free(dl_blk);
  dl_blk = 0;
  dl_nblk = dl_cap = 0;
  dl_len = dl_rd = 0;
  dl_ok = 0;
}

static int dl_append(const u8* p, u32 n) {
  while (n) {
    u32 off = (u32)(dl_len % DL_BLK), take;
    if (off == 0 && dl_len / DL_BLK == (u64)dl_nblk) {
      if (dl_nblk == dl_cap) {
        int nc = dl_cap ? dl_cap * 2 : 64;
        u8** nb = (u8**)realloc(dl_blk, sizeof(u8*) * (size_t)nc);
        if (!nb) return -1;
        dl_blk = nb;
        dl_cap = nc;
      }
      dl_blk[dl_nblk] = (u8*)malloc(DL_BLK);
      if (!dl_blk[dl_nblk]) return -1;
      dl_nblk++;
    }
    take = DL_BLK - off;
    if (take > n) take = n;
    memcpy(dl_blk[dl_len / DL_BLK] + off, p, take);
    dl_len += take;
    p += take;
    n -= take;
  }
  return 0;
}

static int body_ram(u8* out, int cap) {
  u32 off, take;
  if (dl_rd >= dl_len) return 0;
  off = (u32)(dl_rd % DL_BLK);
  take = DL_BLK - off;
  if (take > (u32)cap) take = (u32)cap;
  if (dl_rd + take > dl_len) take = (u32)(dl_len - dl_rd);
  memcpy(out, dl_blk[dl_rd / DL_BLK] + off, take);
  dl_rd += take;
  return (int)take;
}

/* 0 = o pacote inteiro esta na memoria; -1 erro de rede; -2 sem memoria */
static int download_ram(void) {
  static u8 buf[65536];
  unsigned t0 = k_ms();
  u64 total;
  int pct = -5;
  dl_free();
  if (http_req(src_url, "GET", "Accept-Encoding: gzip\r\n", 0, 0) != 0) return -1;
  if (R.status != 200) {
    http_close();
    return fail("dl.google.com respondeu HTTP %d", R.status);
  }
  dl_gzip = R.gzip;
  total = R.clen > 0 ? (u64)R.clen : 0;
  printf("baixando %llu MB%s para a memoria\n", (total ? total : src_size) >> 20, dl_gzip ? " (gzip)" : "");
  for (;;) {
    int n = http_body(buf, (int)sizeof(buf));
    if (n < 0) {
      http_close();
      return -1;
    }
    if (n == 0) break;
    if (dl_append(buf, (u32)n) != 0) {
      http_close();
      dl_free();
      return -2;
    }
    if (total && (int)(dl_len * 100 / total) >= pct + 5) {
      unsigned ms = k_ms() - t0, kbs = ms ? (unsigned)(dl_len / ms) : 0;
      pct = (int)(dl_len * 100 / total);
      pct -= pct % 5;
      printf("  baixado %3d%%  %4llu de %llu MB  %u.%u MB/s\n", pct, dl_len >> 20, total >> 20, kbs / 1000,
             (kbs % 1000) / 100);
    }
  }
  http_close();
  if (g_fail) return -1;
  if (total && dl_len != total) return fail("download com %llu bytes, esperava %llu", dl_len, total);
  printf("baixado em %u s; descompactando e gravando em %s\n", (k_ms() - t0) / 1000, g_dest);
  dl_ok = 1;
  return 0;
}

static int run_pass(void) {
  pass_reset();
  if (src_file) {
    file_off = 0;
    for (;;) {
      static u8 buf[1u << 20];
      int n = body_file(buf, (int)sizeof(buf));
      if (n < 0) return -1;
      if (n == 0) return 0;
      if (sink(buf, (u32)n) != 0) return -1;
    }
  }
  if (dl_ok) {
    dl_rd = 0;
    if (dl_gzip) {
      gunzip(body_ram, sink);
    } else {
      static u8 buf[1u << 20];
      int n;
      while (!g_fail && (n = body_ram(buf, (int)sizeof(buf))) > 0) sink(buf, (u32)n);
    }
    return g_fail ? -1 : 0;
  }
  if (http_req(src_url, "GET", "Accept-Encoding: gzip\r\n", 0, 0) != 0) return -1;
  if (R.status != 200) {
    http_close();
    return fail("dl.google.com respondeu HTTP %d", R.status);
  }
  printf("baixando %llu MB%s\n", src_size >> 20, R.gzip ? " (vem em gzip)" : "");
  if (R.gzip) {
    gunzip(http_body, sink);
  } else {
    static u8 buf[65536];
    for (;;) {
      int n = http_body(buf, (int)sizeof(buf));
      if (n <= 0) break;
      if (sink(buf, (u32)n) != 0) break;
    }
  }
  http_close();
  return g_fail ? -1 : 0;
}

int main(int argc, char** argv) {
  int i, reinstall = 0, only_ver = 0, only_bat = 0, existed = 0, wrote = 0, attempt, ok = 0;
  unsigned t0 = k_ms(), tk, fk;
  u64 need = 0;
  char got[65], verdir[PATH_N];
  struct k_direntl de;
  verdir[0] = 0;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-R")) reinstall = 1;
    else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "-N")) only_ver = 1;
    else if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "-B")) only_bat = 1;
    else if ((!strcmp(argv[i], "-f") || !strcmp(argv[i], "-F")) && i + 1 < argc) src_file = argv[++i];
    else {
      usage();
      return argv[i][0] == '-' && (argv[i][1] == 'h' || argv[i][1] == '?') ? 0 : 1;
    }
  }
  strcpy(g_dest, DEST);
  {
    char d[8];
    if (k_getenv("INSTDBG", d, (int)sizeof(d)) > 0) g_dbg = atoi(d);
  }
  if (only_bat) {
    if (find_installed() != 0) {
      printf("INSTCHRM: nenhum Chrome em %s (rode INSTCHRM para instalar)\n", g_dest);
      return 1;
    }
    return write_launchers();
  }
  crc_init();
  if (src_file) {
    int sz = k_fsize(src_file);
    if (sz <= 0) {
      printf("INSTCHRM: nao achei %s\n", src_file);
      return 1;
    }
    src_size = (u64)(unsigned)sz;
  } else {
    if (!k_net_ready()) {
      printf("INSTCHRM: a rede nao esta pronta (WIFI, ou o cabo)\n");
      return 1;
    }
    printf("perguntando ao Google a versao estavel do Chrome...\n");
    if (omaha() != 0) goto out;
    printf("Chrome %s: %llu MB, %s\n", g_ver, src_size >> 20, src_url);
    if (only_ver) return 0;
    if (installed(g_ver) && !reinstall) {
      printf("O Chrome %s ja esta em %s (INSTCHRM -r instala de novo).\n", g_ver, g_dest);
      return write_launchers();
    }
  }
  /* comeco (PE) e fim do 7z */
  g_head_len = src_size < HEAD_LEN ? (u32)src_size : HEAD_LEN;
  g_head = (u8*)malloc(g_head_len);
  if (!g_head) {
    fail("sem memoria");
    goto out;
  }
  printf("lendo o indice do pacote...\n");
  if (src_range(0, g_head_len, g_head) != 0 || pe_find_7z() != 0 || read_7z() != 0) goto out;
  /* versao: a pasta Chrome-bin\<versao> */
  {
    char v[64] = "";
    for (i = 0; i < nF; i++) {
      if (F[i].dir && is_version(F[i].name) && strlen(F[i].name) < sizeof(v)) strcpy(v, F[i].name);
      if (!F[i].dir) need += F[i].size;
    }
    if (!v[0]) {
      fail("o pacote nao tem a pasta da versao");
      goto out;
    }
    if (g_ver[0] && strcmp(v, g_ver) != 0) {
      fail("o pacote e da versao %s, o Google disse %s", v, g_ver);
      goto out;
    }
    strcpy(g_ver, v);
  }
  /* chrome.exe e chrome_proxy.exe (na raiz do Chrome-bin) vao para a pasta da
   * versao, ao lado das DLLs (ver write_launchers) */
  for (i = 0; i < nF; i++) {
    char* nn;
    if (F[i].dir || !F[i].name[0] || strchr(F[i].name, '\\')) continue;
    nn = (char*)malloc(strlen(g_ver) + strlen(F[i].name) + 2);
    if (!nn) {
      fail("sem memoria");
      goto out;
    }
    sprintf(nn, "%s\\%s", g_ver, F[i].name);
    free(F[i].name);
    F[i].name = nn;
  }
  printf("Chrome %s: %d arquivos, %llu MB instalado\n", g_ver, nF, need >> 20);
  if (src_file && installed(g_ver) && !reinstall) {
    printf("O Chrome %s ja esta em %s (INSTCHRM -r instala de novo).\n", g_ver, g_dest);
    return write_launchers();
  }
  snprintf(verdir, sizeof(verdir), "%s\\%s", g_dest, g_ver);
  existed = k_dir_entry(verdir, &de) == 0;
  if (!existed && k_dfree(g_dest[0], &tk, &fk) == 0 && (u64)fk < (need >> 10) + 16384) {
    fail("falta espaco no %c: (%u MB livres, precisa de %llu MB)", g_dest[0], fk >> 10, (need >> 20) + 16);
    goto out;
  }
  if (mkdir_p(g_dest) != 0) goto out;
  wrote = 1;
  for (i = 0; i < nF; i++) {
    char path[PATH_N];
    if (!F[i].dir || !F[i].name[0]) continue;
    path_of(F[i].name, path);
    if (mkdir_p(path) != 0) goto out;
  }
  for (i = 0; i < nF; i++) {
    char path[PATH_N], *s;
    if (F[i].dir || !strchr(F[i].name, '\\')) continue;
    path_of(F[i].name, path);
    s = strrchr(path, '\\');
    *s = 0;
    if (mkdir_p(path) != 0) goto out;
  }
  if (build_pieces() != 0 || make_empty() != 0) goto out;
  s_pk = (u8**)calloc(8, sizeof(u8*));
  s_pklen = (u64*)calloc(8, sizeof(u64));
  if (!s_pk || !s_pklen) {
    fail("sem memoria");
    goto out;
  }
  s_total = src_size;
  for (attempt = 1; attempt <= (src_file ? 1 : 3); attempt++) {
    g_fail = 0;
    if (attempt > 1) printf("tentando de novo (%d de 3): %s\n", attempt, g_err);
    if (!src_file && !dl_ok && !dl_nomem) {
      int d = download_ram();
      if (d == -2) {
        g_fail = 0;
        dl_nomem = 1;
        printf("pouca memoria para o pacote: gravando enquanto baixa\n");
      } else if (d != 0) {
        continue;
      }
    }
    if (run_pass() != 0) {
      if (dl_ok) break; /* do pacote ja na memoria: baixar de novo nao muda */
      continue;
    }
    if (s_pos != src_size) {
      fail("pacote com %llu bytes, esperava %llu", s_pos, src_size);
      if (dl_ok) break;
      continue;
    }
    if (s_cur != nP) {
      fail("o pacote acabou antes dos arquivos (%d de %d)", s_cur, nP);
      if (dl_ok) break;
      continue;
    }
    sha_hex(&s_sha, got);
    if (!src_file && strcmp(got, o_sha) != 0) {
      fail("SHA-256 nao confere (%s)", got);
      break; /* o TLS entregou inteiro: baixar de novo daria o mesmo */
    }
    ok = 1;
    break;
  }
  if (!ok) goto out;
  if (src_file) printf("SHA-256 do pacote: %s\n", got);
  else printf("SHA-256 confere com o do Google\n");
  /* a pasta da versao nova esta completa: o BAT passa a abrir ela */
  write_launchers();
  remove_old();
  printf("Chrome %s instalado em %s (%llu MB em %u s).\n", g_ver, g_dest, s_written >> 20, (k_ms() - t0) / 1000);
  printf("Abra com %s ou pelo atalho CHROME no desktop.\n", BAT_PATH);
  return 0;
out:
  printf("INSTCHRM: %s\n", g_err[0] ? g_err : "falhou");
  if (s_out) fclose(s_out);
  if (verdir[0] && !existed && wrote) {
    printf("apagando o que foi gravado em %s\n", verdir);
    rmtree(verdir, 0);
  }
  return 1;
}
