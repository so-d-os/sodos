/* PNG RGB para o screenshot da API.
 *
 * O deflate de bloco fixo e a escrita dos chunks foram adaptados do
 * stb_image_write v1.17 (Sean Barrett), dominio publico / licenca MIT.
 * Diferenca desta versao: sem stdio/libc e sem realocacao. O compressor
 * usa tabelas fixas e um buffer de saida unico, o que evita fragmentar o
 * heap alto do kernel durante o screenshot.
 */
#include "png.h"

#ifdef PNG_HOST
#include <stdlib.h>
#define PNG_ALLOC(n) malloc((size_t)(n))
#define PNG_FREE(p) free(p)
#else
#include "mm.h"
#define PNG_ALLOC(n) umalloc_owned((uint32_t)(n), 0)
#define PNG_FREE(p) ufree(p)
#endif

#define PNG_ZHASH 16384
#define PNG_CHAIN 24

static void png_copy(void* dst, const void* src, int n) {
  uint8_t* d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
  while (n-- > 0) *d++ = *s++;
}

static int zlib_bitrev(int code, int codebits) {
  int res = 0;
  while (codebits--) {
    res = (res << 1) | (code & 1);
    code >>= 1;
  }
  return res;
}

static unsigned int zlib_countm(const unsigned char* a, const unsigned char* b,
                                int limit) {
  int i;
  for (i = 0; i < limit && i < 258; ++i)
    if (a[i] != b[i]) break;
  return (unsigned int)i;
}

static unsigned int zhash(const unsigned char* data) {
  uint32_t hash = (uint32_t)data[0] + ((uint32_t)data[1] << 8) +
                  ((uint32_t)data[2] << 16);
  hash ^= hash << 3;
  hash += hash >> 5;
  hash ^= hash << 4;
  hash += hash >> 17;
  hash ^= hash << 25;
  hash += hash >> 6;
  return hash;
}

struct zout {
  uint8_t* p;
  int len;
  int cap;
};

static void zput(struct zout* b, unsigned v) {
  if (b->len < 0) return;
  if (b->len >= b->cap) {
    b->len = -1;
    return;
  }
  b->p[b->len++] = (uint8_t)(v & 0xff);
}

static void zflush(struct zout* b, unsigned int* bits, int* nbits) {
  while (*nbits >= 8) {
    zput(b, *bits);
    if (b->len < 0) return;
    *bits >>= 8;
    *nbits -= 8;
  }
}

static void zadd(struct zout* b, unsigned int* bits, int* nbits, unsigned code,
                 int nb) {
  if (b->len < 0) return;
  *bits |= code << *nbits;
  *nbits += nb;
  zflush(b, bits, nbits);
}

static void zhuffa(struct zout* b, unsigned int* bits, int* nbits, unsigned code,
                   int nb) {
  zadd(b, bits, nbits, (unsigned)zlib_bitrev((int)code, nb), nb);
}

static void zhuff(struct zout* b, unsigned int* bits, int* nbits, int n) {
  if (n <= 143) {
    zhuffa(b, bits, nbits, (unsigned)(0x30 + n), 8);
  } else if (n <= 255) {
    zhuffa(b, bits, nbits, (unsigned)(0x190 + n - 144), 9);
  } else if (n <= 279) {
    zhuffa(b, bits, nbits, (unsigned)(n - 256), 7);
  } else {
    zhuffa(b, bits, nbits, (unsigned)(0xc0 + n - 280), 8);
  }
}

static void zhuff_lit(struct zout* b, unsigned int* bits, int* nbits, int n) {
  if (n <= 143) zhuffa(b, bits, nbits, (unsigned)(0x30 + n), 8);
  else zhuffa(b, bits, nbits, (unsigned)(0x190 + n - 144), 9);
}

static unsigned char* zlib_compress(const unsigned char* data, int data_len,
                                    int* out_len, int quality) {
  static const unsigned short lengthc[] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 259
  };
  static const unsigned char lengtheb[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
  };
  static const unsigned short distc[] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
    12289, 16385, 24577, 32768
  };
  static const unsigned char disteb[] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
  };
  int cap;
  int work_size;
  uint8_t* work;
  uint8_t* out;
  int* bucket;
  uint8_t* bcount;
  uint8_t* bpos;
  struct zout zo;
  unsigned int bitbuf = 0;
  int bitcount = 0;
  int chain;
  int i;
  int j;
  if (data_len <= 0) return 0;
  if (quality < 1) quality = 1;
  if (quality > 9) quality = 9;
  chain = quality * 2 + 2;
  if (chain > PNG_CHAIN) chain = PNG_CHAIN;
  cap = data_len + data_len / 4 + 65536;
  if (cap <= data_len) return 0;
  work_size = PNG_ZHASH * PNG_CHAIN * (int)sizeof(int) + PNG_ZHASH * 2;
  work = (uint8_t*)PNG_ALLOC(work_size);
  out = (uint8_t*)PNG_ALLOC(cap);
  if (!work || !out) {
    if (work) PNG_FREE(work);
    if (out) PNG_FREE(out);
    return 0;
  }
  bucket = (int*)work;
  bcount = (uint8_t*)(bucket + PNG_ZHASH * PNG_CHAIN);
  bpos = bcount + PNG_ZHASH;
  for (i = 0; i < PNG_ZHASH; ++i) {
    bcount[i] = 0;
    bpos[i] = 0;
  }
  zo.p = out;
  zo.len = 0;
  zo.cap = cap;

  zput(&zo, 0x78);  /* zlib, janela de 32K */
  zput(&zo, 0x5e);  /* FLEVEL */
  zadd(&zo, &bitbuf, &bitcount, 1, 1);  /* BFINAL */
  zadd(&zo, &bitbuf, &bitcount, 1, 2);  /* BTYPE = 1, bloco fixo */

  i = 0;
  while (i < data_len - 3 && zo.len >= 0) {
    int h = (int)(zhash(data + i) & (PNG_ZHASH - 1));
    int best = 3;
    int bestloc = -1;
    int cnt = bcount[h];
    for (j = 0; j < cnt; ++j) {
      int pos = bucket[h * PNG_CHAIN + j];
      if (pos > i - 32768) {
        int d = (int)zlib_countm(data + pos, data + i, data_len - i);
        if (d >= best) {
          best = d;
          bestloc = pos;
        }
      }
    }
    if (cnt < chain) {
      bucket[h * PNG_CHAIN + cnt] = i;
      bcount[h] = (uint8_t)(cnt + 1);
    } else {
      bucket[h * PNG_CHAIN + bpos[h]] = i;
      bpos[h] = (uint8_t)((bpos[h] + 1) % chain);
    }

    if (bestloc >= 0) {
      int h2 = (int)(zhash(data + i + 1) & (PNG_ZHASH - 1));
      int cnt2 = bcount[h2];
      for (j = 0; j < cnt2; ++j) {
        int pos = bucket[h2 * PNG_CHAIN + j];
        if (pos > i - 32767) {
          int e = (int)zlib_countm(data + pos, data + i + 1, data_len - i - 1);
          if (e > best) {
            bestloc = -1;
            break;
          }
        }
      }
    }

    if (bestloc >= 0) {
      int d = i - bestloc;
      for (j = 0; best > lengthc[j + 1] - 1; ++j) {}
      zhuff(&zo, &bitbuf, &bitcount, j + 257);
      if (lengtheb[j])
        zadd(&zo, &bitbuf, &bitcount, (unsigned)(best - lengthc[j]), lengtheb[j]);
      for (j = 0; d > distc[j + 1] - 1; ++j) {}
      zadd(&zo, &bitbuf, &bitcount, (unsigned)zlib_bitrev(j, 5), 5);
      if (disteb[j])
        zadd(&zo, &bitbuf, &bitcount, (unsigned)(d - distc[j]), disteb[j]);
      i += best;
    } else {
      zhuff_lit(&zo, &bitbuf, &bitcount, data[i]);
      ++i;
    }
  }

  while (i < data_len && zo.len >= 0) {
    zhuff_lit(&zo, &bitbuf, &bitcount, data[i]);
    ++i;
  }
  zhuff(&zo, &bitbuf, &bitcount, 256);
  while (bitcount > 0 && zo.len >= 0)
    zadd(&zo, &bitbuf, &bitcount, 0, 1);

  if (zo.len >= 0) {
    unsigned int s1 = 1;
    unsigned int s2 = 0;
    int blocklen = data_len % 5552;
    j = 0;
    while (j < data_len) {
      for (i = 0; i < blocklen; ++i) {
        s1 += data[j + i];
        s2 += s1;
      }
      s1 %= 65521;
      s2 %= 65521;
      j += blocklen;
      blocklen = 5552;
    }
    zput(&zo, s2 >> 8);
    zput(&zo, s2);
    zput(&zo, s1 >> 8);
    zput(&zo, s1);
  }

  PNG_FREE(work);
  if (zo.len < 0) {
    PNG_FREE(out);
    return 0;
  }
  *out_len = zo.len;
  return out;
}

static uint32_t png_crc32(const uint8_t* p, int n) {
  uint32_t c = 0xFFFFFFFFu;
  int i;
  int k;
  for (i = 0; i < n; ++i) {
    c ^= p[i];
    for (k = 0; k < 8; ++k)
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

static uint8_t* put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
  return p + 4;
}

uint8_t* png_encode_rgb(const uint8_t* rgb, int w, int h, int stride, int level,
                        int* out_len) {
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  int rowbytes;
  int filt_len;
  int zlen = 0;
  int total;
  int y;
  int i;
  uint8_t* filt;
  uint8_t* z;
  uint8_t* out;
  uint8_t* o;
  uint8_t* chunk;
  if (!out_len) return 0;
  *out_len = 0;
  if (!rgb || w <= 0 || h <= 0 || stride < w * 3) return 0;
  if (w > 100000 || h > 100000) return 0;
  rowbytes = w * 3;
  if (rowbytes + 1 > 0x7FFFFFFF / h) return 0;
  filt_len = (rowbytes + 1) * h;
  if (filt_len <= 0) return 0;
  filt = (uint8_t*)PNG_ALLOC(filt_len);
  if (!filt) return 0;
  for (y = 0; y < h; ++y) {
    const uint8_t* src = rgb + y * stride;
    uint8_t* dst = filt + y * (rowbytes + 1);
    if (y == 0) {
      *dst++ = 0;  /* filtro None */
      png_copy(dst, src, rowbytes);
    } else {
      const uint8_t* prev = rgb + (y - 1) * stride;
      *dst++ = 2;  /* filtro Up */
      for (i = 0; i < rowbytes; ++i)
        dst[i] = (uint8_t)(src[i] - prev[i]);
    }
  }
  z = zlib_compress(filt, filt_len, &zlen, level);
  PNG_FREE(filt);
  if (!z) return 0;

  total = 8 + 25 + 12 + zlen + 12;
  if (total <= 0) {
    PNG_FREE(z);
    return 0;
  }
  out = (uint8_t*)PNG_ALLOC(total);
  if (!out) {
    PNG_FREE(z);
    return 0;
  }
  o = out;
  png_copy(o, sig, 8);
  o += 8;

  chunk = o;
  o = put32(o, 13);
  png_copy(o, "IHDR", 4);
  o += 4;
  o = put32(o, (uint32_t)w);
  o = put32(o, (uint32_t)h);
  *o++ = 8;  /* profundidade */
  *o++ = 2;  /* RGB */
  *o++ = 0;
  *o++ = 0;
  *o++ = 0;
  o = put32(o, png_crc32(chunk + 4, 17));

  chunk = o;
  o = put32(o, (uint32_t)zlen);
  png_copy(o, "IDAT", 4);
  o += 4;
  png_copy(o, z, zlen);
  o += zlen;
  o = put32(o, png_crc32(chunk + 4, 4 + zlen));
  PNG_FREE(z);

  chunk = o;
  o = put32(o, 0);
  png_copy(o, "IEND", 4);
  o += 4;
  o = put32(o, png_crc32(chunk + 4, 4));

  *out_len = (int)(o - out);
  return out;
}
