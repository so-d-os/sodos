/* cache.cpp - cache HTTP em D:\temp\net\HHHHHHHH.NET.
 * Classe, construtor, destrutor e new/delete: o C++ que o cc aceita.
 * O arquivo guarda a URL pedida; se o hash colidir, a leitura compara a URL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cache.h"

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
  if (sz < 36 || sz > 4 * 1024 * 1024 + 4096) {
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

class DiskCache {
public:
  int ready;
  DiskCache(void);
  int path_for(const char* url, char* out, int max);
  int load(const char* url, int* status, char** body, int* body_len, char* ctype,
           int ctype_max, char* final_url, int url_max, int* tls, int* tls_ok);
  int store(const char* url, const char* final_url, int status, const char* ctype,
            const char* body, int body_len, int tls, int tls_ok);
};

DiskCache::DiskCache(void) { ready = 1; }

int DiskCache::path_for(const char* url, char* out, int max) {
  unsigned h;
  int n;
  if (!out || max < 28) return -1;
  h = hash_url(url);
  n = snprintf(out, (size_t)max, "D:\\temp\\net\\%08X.NET", h);
  if (n < 0 || n >= max) return -1;
  return 0;
}

int DiskCache::load(const char* url, int* status, char** body, int* body_len, char* ctype,
                    int ctype_max, char* final_url, int url_max, int* tls, int* tls_ok) {
  ByteBuf file;
  char path[64];
  int type_len, key_len, final_len, blen, off, ulen, i;
  const char* p;
  char* b;
  if (!ready || !url || !status || !body || !body_len) return -1;
  if (path_for(url, path, (int)sizeof(path)) != 0) return -1;
  if (file.read_all(path) != 0) return -1;
  p = file.data;
  if (p[0] != 'S' || p[1] != 'O' || p[2] != 'C' || p[3] != 'A' || p[4] != 'C' || p[5] != 'H' ||
      p[6] != '1' || p[7] != 0)
    return -1;
  *status = get_i32(p + 8);
  blen = get_i32(p + 12);
  type_len = get_i32(p + 16);
  key_len = get_i32(p + 20);
  final_len = get_i32(p + 24);
  if (type_len < 0 || type_len > 127 || key_len < 1 || key_len > 1023 || final_len < 0 ||
      final_len > 1023 || blen < 0 || blen > 4 * 1024 * 1024)
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
  b = new char[blen + 1];
  if (!b) return -1;
  for (i = 0; i < blen; i++) b[i] = p[off + i];
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
  char path[64];
  int type_len, key_len, final_len, total, off, i;
  if (!ready || !url || !url[0] || body_len < 0) return -1;
  if (body_len > 0 && !body) return -1;
  if (!final_url) final_url = url;
  if (!ctype) ctype = "";
  type_len = slen(ctype);
  key_len = slen(url);
  final_len = slen(final_url);
  if (type_len > 127) type_len = 127;
  if (key_len < 1 || key_len > 1023 || final_len > 1023 || body_len > 4 * 1024 * 1024) return -1;
  if (path_for(url, path, (int)sizeof(path)) != 0) return -1;
  total = 36 + type_len + key_len + final_len + body_len;
  file.data = new char[total];
  if (!file.data) return -1;
  file.len = total;
  for (i = 0; i < total; i++) file.data[i] = 0;
  file.data[0] = 'S';
  file.data[1] = 'O';
  file.data[2] = 'C';
  file.data[3] = 'A';
  file.data[4] = 'C';
  file.data[5] = 'H';
  file.data[6] = '1';
  put_i32(file.data + 8, status);
  put_i32(file.data + 12, body_len);
  put_i32(file.data + 16, type_len);
  put_i32(file.data + 20, key_len);
  put_i32(file.data + 24, final_len);
  put_i32(file.data + 28, tls);
  put_i32(file.data + 32, tls_ok);
  off = 36;
  for (i = 0; i < type_len; i++) file.data[off++] = ctype[i];
  for (i = 0; i < key_len; i++) file.data[off++] = url[i];
  for (i = 0; i < final_len; i++) file.data[off++] = final_url[i];
  for (i = 0; i < body_len; i++) file.data[off++] = body[i];
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
