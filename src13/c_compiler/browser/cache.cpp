/* cache.cpp - cache HTTP do navegador em Z:\temp\browser\<host>\<nome>.
 * Z: e o disco em RAM (formatado a cada boot): ler e gravar e rapido, e as
 * figuras tambem entram. Sem Z: o cache vai para D:\temp\browser.
 * O nome e longo (LFN): HHHHHHHH_caminho_da_url.web. O hash na frente deixa
 * o apelido 8.3 que o FAT cria unico ja no ~1, e o arquivo guarda a URL
 * inteira: se dois enderecos derem o mesmo nome, a leitura compara a URL.
 * Classe, construtor, destrutor e new/delete: o C++ que o cc aceita. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include "cache.h"

#define CACHE_BODY_MAX (4 * 1024 * 1024)
#define NAME_PART 120 /* caracteres do caminho no nome do arquivo */
#define HOST_PART 64

static unsigned hash_url(const char* s) {
  unsigned h = 2166136261u;
  int i;
  for (i = 0; s && s[i]; i++) {
    h = h ^ (unsigned)(unsigned char)s[i];
    h = h * 16777619u;
  }
  return h;
}

static void put_i32(char* p, int v) {
  unsigned u = (unsigned)v;
  p[0] = (char)(u & 255u);
  p[1] = (char)((u >> 8) & 255u);
  p[2] = (char)((u >> 16) & 255u);
  p[3] = (char)((u >> 24) & 255u);
}

static int get_i32(const char* p) {
  const unsigned char* u = (const unsigned char*)p;
  unsigned v = (unsigned)u[0] | ((unsigned)u[1] << 8) | ((unsigned)u[2] << 16) |
               ((unsigned)u[3] << 24);
  return (int)v;
}

static int slen(const char* s) {
  int n = 0;
  if (!s) return 0;
  while (s[n]) n++;
  return n;
}

/* 1 se c nao pode ir num nome longo do FAT (fs/fat.c long_bad) ou atrapalha */
static int name_bad(int c) {
  return c < 0x21 || c >= 0x7F || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
         c == '>' || c == '?' || c == '\\' || c == '|' || c == '%' || c == '#';
}

/* Bytes do arquivo. O destrutor devolve a memoria. */
class ByteBuf {
public:
  char* data;
  int len;
  ByteBuf(void);
  ~ByteBuf(void);
  int read_all(const char* path);
  int write_all(const char* path);
};

ByteBuf::ByteBuf(void) {
  data = nullptr;
  len = 0;
}

ByteBuf::~ByteBuf(void) {
  if (data) delete[] data;
  data = nullptr;
  len = 0;
}

int ByteBuf::read_all(const char* path) {
  FILE* fp;
  int sz;
  int n;
  char* neu;
  fp = fopen(path, "rb");
  if (!fp) return -1;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  sz = (int)ftell(fp);
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }
  if (sz < 36 || sz > CACHE_BODY_MAX + 4096) {
    fclose(fp);
    return -1;
  }
  neu = new char[sz];
  if (!neu) {
    fclose(fp);
    return -1;
  }
  n = (int)fread(neu, 1, (size_t)sz, fp);
  fclose(fp);
  if (n != sz) {
    delete[] neu;
    return -1;
  }
  if (data) delete[] data;
  data = neu;
  len = n;
  return 0;
}

int ByteBuf::write_all(const char* path) {
  FILE* fp;
  int n;
  if (!data || len < 0) return -1;
  fp = fopen(path, "wb");
  if (!fp) return -1;
  n = (int)fwrite(data, 1, (size_t)len, fp);
  if (fclose(fp) != 0) return -1;
  return n == len ? 0 : -1;
}

/* Pasta raiz escolhida (Z: ou D:) e a ultima pasta de host ja criada. */
static char g_root[32];
static int g_root_ok;
static char g_made[HOST_PART + 40];

class DiskCache {
public:
  DiskCache(void);
  int root(void);
  int host_dir(const char* url, char* out, int max);
  int path_for(const char* url, char* out, int max, int make);
  int load(const char* url, int* status, char** body, int* body_len, char* ctype,
           int ctype_max, char* final_url, int url_max, int* tls, int* tls_ok);
  int store(const char* url, const char* final_url, int status, const char* ctype,
            const char* body, int body_len, int tls, int tls_ok);
};

DiskCache::DiskCache(void) {}

/* k_mkdir: 0 criou, 1 ja existia */
static int mk(const char* p) {
  int r = k_mkdir(p);
  return r == 0 || r == 1 ? 0 : -1;
}

int DiskCache::root(void) {
  if (g_root_ok) return g_root_ok > 0 ? 0 : -1;
  if (mk("Z:\\temp") == 0 && mk("Z:\\temp\\browser") == 0) {
    strcpy(g_root, "Z:\\temp\\browser");
    g_root_ok = 1;
    return 0;
  }
  if (mk("D:\\temp") == 0 && mk("D:\\temp\\browser") == 0) {
    strcpy(g_root, "D:\\temp\\browser");
    g_root_ok = 1;
    return 0;
  }
  g_root_ok = -1;
  return -1;
}

/* "https://www.exemplo.com:8080/x" -> "<raiz>\www.exemplo.com_8080" */
int DiskCache::host_dir(const char* url, char* out, int max) {
  const char* p = strstr(url, "://");
  int n, k = 0;
  if (root() != 0) return -1;
  n = snprintf(out, (size_t)max, "%s\\", g_root);
  if (n < 0 || n + HOST_PART + 2 > max) return -1;
  p = p ? p + 3 : url;
  while (*p && *p != '/' && *p != '?' && *p != '#' && k < HOST_PART) {
    int c = (unsigned char)*p++;
    if (c == '@') {
      /* usuario@host: so o host */
      n -= k;
      k = 0;
      continue;
    }
    if (c >= 'A' && c <= 'Z') c += 32;
    out[n++] = (char)(name_bad(c) ? '_' : c);
    k++;
  }
  if (k == 0) out[n++] = '_';
  while (n > 0 && out[n - 1] == '.') n--;
  out[n] = 0;
  return n;
}

int DiskCache::path_for(const char* url, char* out, int max, int make) {
  const char* p;
  unsigned h;
  int n, k, start;
  if (!out || max < 64) return -1;
  n = host_dir(url, out, max);
  if (n < 0) return -1;
  if (make && strcmp(g_made, out) != 0) {
    if (mk(out) != 0) return -1;
    strncpy(g_made, out, sizeof(g_made) - 1);
    g_made[sizeof(g_made) - 1] = 0;
  }
  h = hash_url(url);
  k = snprintf(out + n, (size_t)(max - n), "\\%08X_", h);
  if (k < 0 || n + k + NAME_PART + 8 > max) return -1;
  n += k;
  /* caminho e consulta da URL, legiveis: /wiki/Gato?x=1 -> wiki_Gato_x=1 */
  p = strstr(url, "://");
  p = p ? p + 3 : url;
  while (*p && *p != '/' && *p != '?') p++;
  while (*p == '/') p++;
  start = n;
  while (*p && *p != '#' && n - start < NAME_PART) {
    int c = (unsigned char)*p++;
    if (name_bad(c)) {
      if (n > start && out[n - 1] == '_') continue;
      c = '_';
    }
    out[n++] = (char)c;
  }
  while (n > start && (out[n - 1] == '_' || out[n - 1] == '.' || out[n - 1] == ' ')) n--;
  if (n == start) {
    memcpy(out + n, "index", 5);
    n += 5;
  }
  memcpy(out + n, ".web", 5);
  return 0;
}

int DiskCache::load(const char* url, int* status, char** body, int* body_len, char* ctype,
                    int ctype_max, char* final_url, int url_max, int* tls, int* tls_ok) {
  ByteBuf file;
  char path[300];
  int type_len, key_len, final_len, blen, off, ulen, i;
  const char* p;
  char* b;
  if (!url || !status || !body || !body_len) return -1;
  if (path_for(url, path, (int)sizeof(path), 0) != 0) return -1;
  if (file.read_all(path) != 0) return -1;
  p = file.data;
  if (p[0] != 'S' || p[1] != 'O' || p[2] != 'C' || p[3] != 'A' || p[4] != 'C' || p[5] != 'H' ||
      p[6] != '2' || p[7] != 0)
    return -1;
  *status = get_i32(p + 8);
  blen = get_i32(p + 12);
  type_len = get_i32(p + 16);
  key_len = get_i32(p + 20);
  final_len = get_i32(p + 24);
  if (type_len < 0 || type_len > 127 || key_len < 1 || key_len > 1023 || final_len < 0 ||
      final_len > 1023 || blen < 0 || blen > CACHE_BODY_MAX)
    return -1;
  if (36 + type_len + key_len + final_len + blen != file.len) return -1;
  ulen = slen(url);
  if (ulen != key_len) return -1;
  off = 36;
  if (memcmp(p + off + type_len, url, (size_t)key_len) != 0) return -1;
  if (ctype && ctype_max > 0) {
    int n = type_len;
    if (n > ctype_max - 1) n = ctype_max - 1;
    for (i = 0; i < n; i++) ctype[i] = p[off + i];
    ctype[n] = 0;
  }
  off += type_len + key_len;
  if (final_url && url_max > 0) {
    int n = final_len;
    if (n > url_max - 1) n = url_max - 1;
    for (i = 0; i < n; i++) final_url[i] = p[off + i];
    final_url[n] = 0;
  }
  off += final_len;
  /* malloc: quem libera e o http_free */
  b = (char*)malloc((size_t)blen + 1);
  if (!b) return -1;
  memcpy(b, p + off, (size_t)blen);
  b[blen] = 0;
  *body = b;
  *body_len = blen;
  if (tls) *tls = get_i32(p + 28);
  if (tls_ok) *tls_ok = get_i32(p + 32);
  return 0;
}

int DiskCache::store(const char* url, const char* final_url, int status, const char* ctype,
                     const char* body, int body_len, int tls, int tls_ok) {
  ByteBuf file;
  char path[300];
  int type_len, key_len, final_len, total, off;
  if (!url || !url[0] || body_len < 0) return -1;
  if (body_len > 0 && !body) return -1;
  if (!final_url) final_url = url;
  if (!ctype) ctype = "";
  type_len = slen(ctype);
  key_len = slen(url);
  final_len = slen(final_url);
  if (type_len > 127) type_len = 127;
  if (key_len < 1 || key_len > 1023 || final_len > 1023 || body_len > CACHE_BODY_MAX) return -1;
  if (path_for(url, path, (int)sizeof(path), 1) != 0) return -1;
  total = 36 + type_len + key_len + final_len + body_len;
  file.data = new char[total];
  if (!file.data) return -1;
  file.len = total;
  memset(file.data, 0, 36);
  memcpy(file.data, "SOCACH2", 7);
  put_i32(file.data + 8, status);
  put_i32(file.data + 12, body_len);
  put_i32(file.data + 16, type_len);
  put_i32(file.data + 20, key_len);
  put_i32(file.data + 24, final_len);
  put_i32(file.data + 28, tls);
  put_i32(file.data + 32, tls_ok);
  off = 36;
  memcpy(file.data + off, ctype, (size_t)type_len);
  off += type_len;
  memcpy(file.data + off, url, (size_t)key_len);
  off += key_len;
  memcpy(file.data + off, final_url, (size_t)final_len);
  off += final_len;
  if (body_len > 0) memcpy(file.data + off, body, (size_t)body_len);
  return file.write_all(path);
}

int cache_load(const char* url, int* status, char** body, int* body_len, char* ctype,
               int ctype_max, char* final_url, int url_max, int* tls, int* tls_ok) {
  DiskCache c;
  return c.load(url, status, body, body_len, ctype, ctype_max, final_url, url_max, tls, tls_ok);
}

int cache_store(const char* url, const char* final_url, int status, const char* ctype,
                const char* body, int body_len, int tls, int tls_ok) {
  DiskCache c;
  return c.store(url, final_url, status, ctype, body, body_len, tls, tls_ok);
}

const char* cache_where(void) {
  DiskCache c;
  if (c.root() != 0) return "sem cache";
  return g_root;
}
