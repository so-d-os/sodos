/* videoplay.c - player de video do WIN. Nome 8.3: VIDPLAY.EXE
 *   cc -o vidplay.exe videoplay.c gui.dll img.dll
 * Abre o arquivo da linha de comando.
 * Reproduz MJPEG e RGB dentro de AVI, MOV e MP4, e uma sequencia de JPEG.
 * Espaco pausa, setas trocam o quadro, Esc fecha. Sem som. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <img.h>
#ifndef VID_TEST
#include <gui.h>
#include <so.h>
#endif

#define VID_MAX    8000
#define VID_SAMPLE (4 * 1024 * 1024)

typedef struct {
  FILE* fp;
  int nframes;
  int cap;
  int frame_ms;
  int raw;
  int bpp;
  int width;
  int height;
  int bottom_up;
  int stride;
  int playable;
  char codec[8];
  char err[160];
  long* off;
  int* sz;
} vid_t;

static unsigned rd_le(const unsigned char* p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned rd_be(const unsigned char* p) {
  return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static int tag4(const unsigned char* p, const char* s) {
  return p[0] == (unsigned char)s[0] && p[1] == (unsigned char)s[1] &&
         p[2] == (unsigned char)s[2] && p[3] == (unsigned char)s[3];
}

static void set_codec(vid_t* v, const unsigned char* p) {
  int i;
  for (i = 0; i < 4; i++) {
    unsigned char c = p[i];
    if (c < 32 || c > 126) c = '?';
    v->codec[i] = (char)c;
  }
  v->codec[4] = 0;
}

static int read_full(FILE* fp, void* b, int n) {
  if (n < 0) return 0;
  return (int)fread(b, 1, (size_t)n, fp) == n;
}

static long file_end(FILE* fp) {
  long n;
  if (fseek(fp, 0, SEEK_END) != 0) return -1;
  n = ftell(fp);
  if (fseek(fp, 0, SEEK_SET) != 0) return -1;
  return n;
}

static int push_frame(vid_t* v, long off, int sz) {
  int nc;
  long* no;
  int* ns;
  if (sz <= 0 || off < 0) return 0;
  if (v->nframes >= VID_MAX) return -1;
  if (v->nframes >= v->cap) {
    nc = v->cap ? v->cap * 2 : 32;
    if (nc > VID_MAX) nc = VID_MAX;
    no = (long*)malloc((size_t)nc * sizeof(long));
    ns = (int*)malloc((size_t)nc * sizeof(int));
    if (!no || !ns) {
      free(no);
      free(ns);
      return -1;
    }
    if (v->nframes && v->off && v->sz) {
      memcpy(no, v->off, (size_t)v->nframes * sizeof(long));
      memcpy(ns, v->sz, (size_t)v->nframes * sizeof(int));
    }
    free(v->off);
    free(v->sz);
    v->off = no;
    v->sz = ns;
    v->cap = nc;
  }
  v->off[v->nframes] = off;
  v->sz[v->nframes] = sz;
  v->nframes++;
  return 0;
}

static void vid_free(vid_t* v) {
  if (!v) return;
  if (v->fp) fclose(v->fp);
  free(v->off);
  free(v->sz);
  v->fp = 0;
  v->off = 0;
  v->sz = 0;
}

static int codec_is_jpeg(const char* c) {
  return strcmp(c, "MJPG") == 0 || strcmp(c, "mjpg") == 0 || strcmp(c, "JPEG") == 0 ||
         strcmp(c, "jpeg") == 0 || strcmp(c, "mjpa") == 0 || strcmp(c, "mjpb") == 0 ||
         strcmp(c, "MJPA") == 0 || strcmp(c, "MJPB") == 0;
}

static int looks_jpeg(FILE* fp, long off, int sz) {
  unsigned char b[64];
  int n, i;
  if (sz < 2) return 0;
  n = sz < 64 ? sz : 64;
  if (fseek(fp, off, SEEK_SET) != 0) return 0;
  if ((int)fread(b, 1, (size_t)n, fp) != n) return 0;
  for (i = 0; i + 1 < n; i++)
    if (b[i] == 0xFF && b[i + 1] == 0xD8) return 1;
  return 0;
}

static void note_codec(vid_t* v) {
  if (v->playable) {
    v->err[0] = 0;
    return;
  }
  if (!v->codec[0]) strcpy(v->codec, "?");
  if (strcmp(v->codec, "avc1") == 0 || strcmp(v->codec, "avc3") == 0)
    snprintf(v->err, sizeof(v->err), "H.264 (%s) ainda nao e reproduzido", v->codec);
  else if (strcmp(v->codec, "hvc1") == 0 || strcmp(v->codec, "hev1") == 0)
    snprintf(v->err, sizeof(v->err), "H.265 (%s) ainda nao e reproduzido", v->codec);
  else
    snprintf(v->err, sizeof(v->err), "Codec %s ainda nao e reproduzido", v->codec);
}

/* ---------- AVI ---------- */

static int avi_walk(vid_t* v, long end, int in_movi, int* expect_strf, int* have_fmt) {
  while (ftell(v->fp) + 8 <= end) {
    unsigned char id[4];
    unsigned char szb[4];
    unsigned sz;
    long data, after;
    if (!read_full(v->fp, id, 4) || !read_full(v->fp, szb, 4)) return -1;
    sz = rd_le(szb);
    data = ftell(v->fp);
    after = data + (long)sz;
    if (sz & 1) after++;
    if (after < data) return -1;
    if (tag4(id, "LIST") || tag4(id, "RIFF")) {
      unsigned char ty[4];
      long inner_end;
      if (sz < 4 || !read_full(v->fp, ty, 4)) return -1;
      inner_end = data + (long)sz;
      if (inner_end > end) inner_end = end;
      if (tag4(ty, "movi")) {
        if (avi_walk(v, inner_end, 1, expect_strf, have_fmt) != 0) return -1;
      } else if (tag4(ty, "hdrl") || tag4(ty, "strl") || tag4(ty, "rec ")) {
        if (avi_walk(v, inner_end, in_movi || tag4(ty, "rec "), expect_strf, have_fmt) != 0)
          return -1;
      }
    } else if (!in_movi && tag4(id, "avih") && sz >= 4) {
      unsigned char b[4];
      unsigned us;
      if (!read_full(v->fp, b, 4)) return -1;
      us = rd_le(b);
      if (v->frame_ms <= 0 && us > 0) {
        v->frame_ms = (int)(us / 1000u);
        if (v->frame_ms < 1) v->frame_ms = 1;
      }
    } else if (!in_movi && tag4(id, "strh") && sz >= 28) {
      unsigned char b[32];
      int n = sz > 32 ? 32 : (int)sz;
      if (!read_full(v->fp, b, n)) return -1;
      if (!*have_fmt && tag4(b, "vids")) {
        unsigned scale = rd_le(b + 20);
        unsigned rate = rd_le(b + 24);
        set_codec(v, b + 4);
        *have_fmt = 1;
        *expect_strf = 1;
        if (rate > 0 && scale > 0) {
          v->frame_ms = (int)((unsigned long)scale * 1000ul / rate);
          if (v->frame_ms < 1) v->frame_ms = 1;
        }
      } else {
        *expect_strf = 0;
      }
    } else if (!in_movi && tag4(id, "strf") && *expect_strf && sz >= 20) {
      unsigned char b[40];
      int n = sz > 40 ? 40 : (int)sz;
      int h;
      memset(b, 0, sizeof b);
      if (!read_full(v->fp, b, n)) return -1;
      v->width = (int)rd_le(b + 4);
      h = (int)rd_le(b + 8);
      v->bottom_up = h > 0;
      if (h < 0) h = -h;
      v->height = h;
      v->bpp = b[14] | (b[15] << 8);
      set_codec(v, b + 16);
      if (rd_le(b + 16) == 0 || tag4(b + 16, "DIB ")) {
        if (v->bpp == 24 || v->bpp == 32) v->raw = 1;
        strcpy(v->codec, v->bpp == 32 ? "RGB32" : "RGB24");
      }
      if (v->width > 0 && v->bpp > 0)
        v->stride = ((v->width * v->bpp + 31) / 32) * 4;
      *expect_strf = 0;
    } else if (in_movi && id[2] == 'd' && (id[3] == 'c' || id[3] == 'b')) {
      if (push_frame(v, data, (int)sz) != 0) {
        if (fseek(v->fp, after, SEEK_SET) != 0) return -1;
        return 0;
      }
    }
    if (fseek(v->fp, after, SEEK_SET) != 0) return -1;
  }
  return 0;
}

static int avi_open(vid_t* v) {
  long end = file_end(v->fp);
  int expect = 0, have = 0;
  unsigned char mag[12];
  if (end < 12) return -1;
  if (fseek(v->fp, 0, SEEK_SET) != 0) return -1;
  if (!read_full(v->fp, mag, 12)) return -1;
  if (!tag4(mag, "RIFF") || !tag4(mag + 8, "AVI ")) {
    strcpy(v->err, "AVI invalido");
    return -1;
  }
  if (fseek(v->fp, 12, SEEK_SET) != 0) return -1;
  if (avi_walk(v, end, 0, &expect, &have) != 0) {
    strcpy(v->err, "AVI truncado");
    return -1;
  }
  if (v->frame_ms <= 0) v->frame_ms = 40;
  if (v->frame_ms > 2000) v->frame_ms = 2000;
  if (v->nframes < 1) {
    strcpy(v->err, "AVI sem quadros de video");
    return -1;
  }
  if (!v->codec[0]) strcpy(v->codec, v->raw ? "RGB" : "AVI");
  v->playable = v->raw || codec_is_jpeg(v->codec) || looks_jpeg(v->fp, v->off[0], v->sz[0]);
  note_codec(v);
  return 0;
}

/* ---------- MP4 / MOV ---------- */

typedef struct {
  vid_t* v;
  int video;
  int committed;
  int timescale;
  int stsc_n;
  int* stsc_first;
  int* stsc_spc;
  int stsz_fixed;
  int stsz_n;
  int* stsz;
  int stco_n;
  long* stco;
  int delta;
} mp4_t;

static void mp4_reset_track(mp4_t* m) {
  m->video = 0;
  m->timescale = 0;
  m->delta = 0;
  m->stsc_n = 0;
  free(m->stsc_first);
  free(m->stsc_spc);
  m->stsc_first = 0;
  m->stsc_spc = 0;
  m->stsz_fixed = 0;
  m->stsz_n = 0;
  free(m->stsz);
  m->stsz = 0;
  m->stco_n = 0;
  free(m->stco);
  m->stco = 0;
}

static void mp4_commit(mp4_t* m) {
  int chunk, produced;
  if (m->committed || !m->video || m->stco_n < 1) return;
  produced = 0;
  for (chunk = 1; chunk <= m->stco_n && m->v->nframes < VID_MAX; chunk++) {
    int spc = 1;
    int e, s;
    long pos;
    for (e = 0; e < m->stsc_n; e++)
      if (m->stsc_first[e] <= chunk) spc = m->stsc_spc[e];
    if (spc < 1) spc = 1;
    pos = m->stco[chunk - 1];
    for (s = 0; s < spc && m->v->nframes < VID_MAX; s++) {
      int sz = m->stsz_fixed;
      if (!sz) sz = (produced < m->stsz_n) ? m->stsz[produced] : 0;
      if (sz > 0) push_frame(m->v, pos, sz);
      if (sz > 0) pos += sz;
      produced++;
    }
  }
  if (m->timescale > 0 && m->delta > 0) {
    m->v->frame_ms = (int)((long)m->delta * 1000L / m->timescale);
    if (m->v->frame_ms < 1) m->v->frame_ms = 1;
  }
  m->committed = 1;
}

static int mp4_payload(mp4_t* m, const char* type, long payload, long box_end);

static int mp4_boxes(mp4_t* m, long start, long end, int depth) {
  if (depth > 8) return 0;
  if (fseek(m->v->fp, start, SEEK_SET) != 0) return -1;
  while (ftell(m->v->fp) + 8 <= end) {
    unsigned char b[8];
    unsigned sz;
    long at, payload, box_end;
    char type[5];
    int i;
    at = ftell(m->v->fp);
    if (!read_full(m->v->fp, b, 8)) return -1;
    sz = rd_be(b);
    for (i = 0; i < 4; i++) type[i] = (char)b[4 + i];
    type[4] = 0;
    if (sz == 1) {
      unsigned char e[8];
      unsigned hi, lo;
      if (!read_full(m->v->fp, e, 8)) return -1;
      hi = rd_be(e);
      lo = rd_be(e + 4);
      if (hi) return -1;
      box_end = at + (long)lo;
    } else if (sz == 0) {
      box_end = end;
    } else {
      if (sz < 8) return -1;
      box_end = at + (long)sz;
    }
    if (box_end < at || box_end > end) box_end = end;
    payload = ftell(m->v->fp);
    if (strcmp(type, "trak") == 0) {
      mp4_reset_track(m);
      if (mp4_boxes(m, payload, box_end, depth + 1) != 0) return -1;
      mp4_commit(m);
      mp4_reset_track(m);
    } else if (strcmp(type, "moov") == 0 || strcmp(type, "mdia") == 0 ||
               strcmp(type, "minf") == 0 || strcmp(type, "stbl") == 0) {
      if (mp4_boxes(m, payload, box_end, depth + 1) != 0) return -1;
    } else if (mp4_payload(m, type, payload, box_end) != 0) {
      return -1;
    }
    if (fseek(m->v->fp, box_end, SEEK_SET) != 0) return -1;
  }
  return 0;
}

static int read_be_pairs(FILE* fp, int n, int* a, int* b, int rec) {
  int i;
  for (i = 0; i < n; i++) {
    unsigned char p[12];
    if (!read_full(fp, p, rec)) return -1;
    a[i] = (int)rd_be(p);
    if (b) b[i] = (int)rd_be(p + 4);
  }
  return 0;
}

static int mp4_payload(mp4_t* m, const char* type, long payload, long box_end) {
  unsigned char b[16];
  if (strcmp(type, "hdlr") == 0) {
    if (box_end - payload < 12) return 0;
    if (fseek(m->v->fp, payload + 8, SEEK_SET) != 0) return -1;
    if (!read_full(m->v->fp, b, 4)) return -1;
    m->video = tag4(b, "vide");
    return 0;
  }
  if (strcmp(type, "mdhd") == 0) {
    int ver;
    if (!read_full(m->v->fp, b, 4)) return -1;
    ver = b[0];
    if (fseek(m->v->fp, payload + (ver ? 20 : 12), SEEK_SET) != 0) return -1;
    if (!read_full(m->v->fp, b, 4)) return -1;
    m->timescale = (int)rd_be(b);
    return 0;
  }
  if (strcmp(type, "tkhd") == 0) {
    int ver;
    if (!read_full(m->v->fp, b, 4)) return -1;
    ver = b[0];
    if (fseek(m->v->fp, payload + (ver ? 88 : 76), SEEK_SET) != 0) return 0;
    if (read_full(m->v->fp, b, 8)) {
      m->v->width = (int)(rd_be(b) >> 16);
      m->v->height = (int)(rd_be(b + 4) >> 16);
    }
    return 0;
  }
  if (!m->video || m->committed) return 0;
  if (strcmp(type, "stsd") == 0) {
    if (box_end - payload < 16) return 0;
    if (fseek(m->v->fp, payload + 8, SEEK_SET) != 0) return -1;
    if (!read_full(m->v->fp, b, 8)) return -1;
    set_codec(m->v, b + 4);
    return 0;
  }
  if (strcmp(type, "stts") == 0) {
    unsigned count;
    if (box_end - payload < 16) return 0;
    if (fseek(m->v->fp, payload + 4, SEEK_SET) != 0) return -1;
    if (!read_full(m->v->fp, b, 4)) return -1;
    count = rd_be(b);
    if (count > 0 && read_full(m->v->fp, b, 8)) m->delta = (int)rd_be(b + 4);
    return 0;
  }
  if (strcmp(type, "stsc") == 0) {
    unsigned count;
    int n;
    if (fseek(m->v->fp, payload + 4, SEEK_SET) != 0) return -1;
    if (!read_full(m->v->fp, b, 4)) return -1;
    count = rd_be(b);
    n = count > 1024u ? 1024 : (int)count;
    m->stsc_first = (int*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    m->stsc_spc = (int*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!m->stsc_first || !m->stsc_spc) return -1;
    m->stsc_n = n;
    if (n > 0 && read_be_pairs(m->v->fp, n, m->stsc_first, m->stsc_spc, 12) != 0) return -1;
    return 0;
  }
  if (strcmp(type, "stsz") == 0) {
    unsigned fixed, count;
    int n, i;
    if (fseek(m->v->fp, payload + 4, SEEK_SET) != 0) return -1;
    if (!read_full(m->v->fp, b, 8)) return -1;
    fixed = rd_be(b);
    count = rd_be(b + 4);
    n = count > (unsigned)VID_MAX ? VID_MAX : (int)count;
    m->stsz_n = n;
    if (fixed) {
      m->stsz_fixed = (int)fixed;
      return 0;
    }
    m->stsz = (int*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!m->stsz) return -1;
    for (i = 0; i < n; i++) {
      if (!read_full(m->v->fp, b, 4)) return -1;
      m->stsz[i] = (int)rd_be(b);
    }
    return 0;
  }
  if (strcmp(type, "stco") == 0 || strcmp(type, "co64") == 0) {
    unsigned count;
    int n, i, wide;
    if (fseek(m->v->fp, payload + 4, SEEK_SET) != 0) return -1;
    if (!read_full(m->v->fp, b, 4)) return -1;
    count = rd_be(b);
    n = count > (unsigned)VID_MAX ? VID_MAX : (int)count;
    wide = strcmp(type, "co64") == 0;
    m->stco = (long*)malloc((size_t)(n > 0 ? n : 1) * sizeof(long));
    if (!m->stco) return -1;
    m->stco_n = 0;
    for (i = 0; i < n; i++) {
      unsigned hi = 0, lo;
      if (wide) {
        if (!read_full(m->v->fp, b, 8)) return -1;
        hi = rd_be(b);
        lo = rd_be(b + 4);
      } else {
        if (!read_full(m->v->fp, b, 4)) return -1;
        lo = rd_be(b);
      }
      if (hi) continue;
      m->stco[m->stco_n++] = (long)lo;
    }
    return 0;
  }
  return 0;
}

static int mp4_open(vid_t* v) {
  mp4_t m;
  long end = file_end(v->fp);
  memset(&m, 0, sizeof m);
  m.v = v;
  if (end < 16) return -1;
  if (mp4_boxes(&m, 0, end, 0) != 0) {
    mp4_reset_track(&m);
    strcpy(v->err, "MP4 truncado");
    return -1;
  }
  mp4_reset_track(&m);
  if (v->frame_ms <= 0) v->frame_ms = 40;
  if (v->frame_ms > 2000) v->frame_ms = 2000;
  if (v->nframes < 1) {
    strcpy(v->err, "MP4 sem quadros de video");
    return -1;
  }
  if (!v->codec[0]) strcpy(v->codec, "MP4");
  v->playable = codec_is_jpeg(v->codec) || looks_jpeg(v->fp, v->off[0], v->sz[0]);
  note_codec(v);
  return 0;
}

/* ---------- sequencia de JPEG ---------- */

static int jpg_open(vid_t* v) {
  unsigned char buf[4096];
  long file = file_end(v->fp);
  long pos = 0;
  int prev = 0;
  long start = -1;
  if (file < 4) return -1;
  if (file > 32 * 1024 * 1024) file = 32 * 1024 * 1024;
  if (fseek(v->fp, 0, SEEK_SET) != 0) return -1;
  while (pos < file && v->nframes < VID_MAX) {
    int n = (int)((file - pos) > 4096 ? 4096 : (file - pos));
    int i;
    if ((int)fread(buf, 1, (size_t)n, v->fp) != n) break;
    for (i = 0; i < n; i++) {
      int c = buf[i];
      if (prev == 0xFF && c == 0xD8 && start < 0) start = pos + i - 1;
      else if (prev == 0xFF && c == 0xD9 && start >= 0) {
        long e = pos + i;
        push_frame(v, start, (int)(e - start + 1));
        start = -1;
      }
      prev = c;
    }
    pos += n;
  }
  strcpy(v->codec, "JPEG");
  v->frame_ms = 80;
  v->playable = v->nframes > 0;
  if (!v->playable) {
    strcpy(v->err, "JPEG sem quadros");
    return -1;
  }
  return 0;
}

#ifndef VID_TEST
/* O fopen carrega o arquivo inteiro. Um MP4 de dezenas de MB so para
 * descobrir avc1 deixa a tela parada. Os 64 KB iniciais bastam. */
static unsigned char head_buf[65536];

static int head_has(const unsigned char* b, int n, const char* s) {
  int i;
  for (i = 0; i + 4 <= n; i++)
    if (tag4(b + i, s)) return 1;
  return 0;
}

static int refuse_known(vid_t* v, const char* path) {
  int sz = k_fsize(path);
  int n;
  if (sz < 16) return 0;
  n = sz > (int)sizeof(head_buf) ? (int)sizeof(head_buf) : sz;
  if (k_fload(path, (char*)head_buf, n) < 16) return 0;
  if (head_has(head_buf, n, "avc1") || head_has(head_buf, n, "avc3")) {
    strcpy(v->codec, "avc1");
    strcpy(v->err, "H.264 (avc1) ainda nao e reproduzido");
    return 1;
  }
  if (head_has(head_buf, n, "hvc1") || head_has(head_buf, n, "hev1")) {
    strcpy(v->codec, "hvc1");
    strcpy(v->err, "H.265 ainda nao e reproduzido");
    return 1;
  }
  return 0;
}
#endif

static int vid_open(vid_t* v, const char* path) {
  unsigned char mag[12];
  memset(v, 0, sizeof(*v));
  v->frame_ms = 40;
  if (!path || !path[0]) {
    strcpy(v->err, "Falta o arquivo");
    return -1;
  }
#ifndef VID_TEST
  if (refuse_known(v, path)) return -1;
#endif
  v->fp = fopen(path, "rb");
  if (!v->fp) {
    snprintf(v->err, sizeof(v->err), "Nao abriu %s", path);
    return -1;
  }
  if (!read_full(v->fp, mag, 12)) {
    strcpy(v->err, "Arquivo curto demais");
    return -1;
  }
  if (tag4(mag, "RIFF") && tag4(mag + 8, "AVI ")) return avi_open(v);
  if (tag4(mag + 4, "ftyp") || tag4(mag + 4, "moov") || tag4(mag + 4, "mdat") ||
      tag4(mag + 4, "wide") || tag4(mag + 4, "free") || tag4(mag + 4, "skip"))
    return mp4_open(v);
  if (mag[0] == 0xFF && mag[1] == 0xD8) return jpg_open(v);
  if (mag[0] == 0x1A && mag[1] == 0x45 && mag[2] == 0xDF && mag[3] == 0xA3) {
    strcpy(v->err, "MKV/WebM ainda nao e reproduzido");
    return -1;
  }
  if (mag[0] == 0x30 && mag[1] == 0x26 && mag[2] == 0xB2 && mag[3] == 0x75) {
    strcpy(v->err, "WMV/ASF ainda nao e reproduzido");
    return -1;
  }
  if (mag[0] == 'F' && mag[1] == 'L' && mag[2] == 'V') {
    strcpy(v->err, "FLV ainda nao e reproduzido");
    return -1;
  }
  if (mag[0] == 0 && mag[1] == 0 && mag[2] == 1) {
    strcpy(v->err, "MPEG-1/2 ainda nao e reproduzido. Use AVI MJPEG ou MP4 JPEG.");
    return -1;
  }
  strcpy(v->err, "Formato de video desconhecido");
  return -1;
}

static int find_jpeg(const unsigned char* b, int n) {
  int i;
  for (i = 0; i + 1 < n; i++)
    if (b[i] == 0xFF && b[i + 1] == 0xD8) return i;
  return -1;
}

static int raw_frame(vid_t* v, const unsigned char* b, int n, img_t* im) {
  int w = v->width;
  int h = v->height;
  int step = 1;
  int ow, oh, y, x, bpp;
  if (w < 1 || h < 1 || (v->bpp != 24 && v->bpp != 32) || v->stride < 1) return -1;
  if ((long)h * v->stride > n) return -1;
  while ((w + step - 1) / step > 400 || (h + step - 1) / step > 400) step++;
  ow = (w + step - 1) / step;
  oh = (h + step - 1) / step;
  if (ow < 1) ow = 1;
  if (oh < 1) oh = 1;
  im->w = ow;
  im->h = oh;
  im->rgba = (unsigned char*)malloc((size_t)ow * (size_t)oh * 4);
  if (!im->rgba) return -1;
  bpp = v->bpp / 8;
  for (y = 0; y < oh; y++) {
    int sy = y * step;
    const unsigned char* row;
    if (sy >= h) sy = h - 1;
    if (v->bottom_up) sy = h - 1 - sy;
    row = b + (long)sy * v->stride;
    for (x = 0; x < ow; x++) {
      int sx = x * step;
      const unsigned char* p;
      unsigned char* d = im->rgba + ((size_t)y * (size_t)ow + (size_t)x) * 4;
      if (sx >= w) sx = w - 1;
      p = row + sx * bpp;
      d[2] = p[0];
      d[1] = p[1];
      d[0] = p[2];
      d[3] = 255;
    }
  }
  return 0;
}

/* 0 = desenhou, -1 = este quadro nao da imagem */
static int vid_frame(vid_t* v, int index, img_t* im) {
  unsigned char* buf;
  int sz, j;
  im->w = im->h = 0;
  im->rgba = 0;
  if (index < 0 || index >= v->nframes) return -1;
  sz = v->sz[index];
  if (sz < 1 || sz > VID_SAMPLE) return -1;
  buf = (unsigned char*)malloc((size_t)sz);
  if (!buf) return -1;
  if (fseek(v->fp, v->off[index], SEEK_SET) != 0 || !read_full(v->fp, buf, sz)) {
    free(buf);
    return -1;
  }
  j = find_jpeg(buf, sz < 64 ? sz : 64);
  if (j >= 0) {
    int rc = img_decode(buf + j, sz - j, im);
    free(buf);
    return rc;
  }
  if (v->raw) {
    int rc = raw_frame(v, buf, sz, im);
    free(buf);
    return rc;
  }
  free(buf);
  return -1;
}

#ifdef VID_TEST
int main(int argc, char** argv) {
  vid_t v;
  img_t im;
  int i, rc;
  if (argc < 2) {
    printf("uso: vidtest arquivo\n");
    return 2;
  }
  rc = vid_open(&v, argv[1]);
  printf("open %d frames %d ms %d codec %s play %d raw %d %dx%d err %s\n", rc, v.nframes,
         v.frame_ms, v.codec, v.playable, v.raw, v.width, v.height, v.err);
  if (rc != 0) {
    vid_free(&v);
    return 1;
  }
  for (i = 0; i < v.nframes && i < 8; i++) {
    int fr = vid_frame(&v, i, &im);
    printf("frame %d rc %d %dx%d pix %u %u %u\n", i, fr, im.w, im.h,
           im.rgba ? im.rgba[0] : 0, im.rgba ? im.rgba[1] : 0, im.rgba ? im.rgba[2] : 0);
    if (fr != 0 && v.playable) {
      img_free(&im);
      vid_free(&v);
      return 1;
    }
    img_free(&im);
  }
  vid_free(&v);
  return v.playable ? 0 : 0;
}
#else

static const char* base_name(const char* path) {
  const char* s = path;
  const char* b = path;
  if (!path || !path[0]) return "Video";
  for (; *s; s++)
    if (*s == '\\' || *s == '/' || *s == ':') b = s + 1;
  return b[0] ? b : path;
}

static void fit_rect(int sw, int sh, int iw, int ih, int* x, int* y, int* w, int* h) {
  int aw = sw - 16;
  int ah = sh - 36;
  if (aw < 1) aw = 1;
  if (ah < 1) ah = 1;
  if (iw < 1) iw = 1;
  if (ih < 1) ih = 1;
  if (iw * ah > ih * aw) {
    *w = aw;
    *h = ih * aw / iw;
  } else {
    *h = ah;
    *w = iw * ah / ih;
  }
  if (*w < 1) *w = 1;
  if (*h < 1) *h = 1;
  *x = (sw - *w) / 2;
  *y = (ah - *h) / 2;
  if (*y < 0) *y = 0;
}

static int key_frame(int key, int* frame, int n, int* paused, int* alive) {
  if (key == 27 || key == 'q') {
    *alive = 0;
    return 1;
  }
  if (key == ' ') {
    *paused = !*paused;
    return 1;
  }
  if (key == GUI_KEY_LEFT) {
    if (*frame > 0) (*frame)--;
    *paused = 1;
    return 1;
  }
  if (key == GUI_KEY_RIGHT) {
    if (*frame + 1 < n) (*frame)++;
    *paused = 1;
    return 1;
  }
  if (key == GUI_KEY_HOME) {
    *frame = 0;
    *paused = 1;
    return 1;
  }
  return 0;
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : 0;
  vid_t vid;
  int frame = 0;
  int paused = 0;
  int alive = 1;
  int opened;
  opened = vid_open(&vid, path);
  if (gui_open(base_name(path)) != 0) {
    if (opened == 0)
      printf("%s  %d quadros  %s\n", vid.codec, vid.nframes, vid.playable ? "ok" : vid.err);
    else
      printf("%s\n", vid.err);
    vid_free(&vid);
    return opened == 0 ? 0 : 1;
  }
  while (alive) {
    gui_event_t ev;
    img_t im;
    char msg[180];
    int sw, sh, ch, x, y, w, h;
    im.rgba = 0;
    im.w = im.h = 0;
    sw = gui_width();
    sh = gui_height();
    ch = gui_char_h();
    gui_fill(0, 0, sw, sh, 0x101010);
    if (opened != 0 || !vid.playable) {
      const char* m = opened != 0 ? vid.err : vid.err;
      gui_text(16, 24, m[0] ? m : "Nao foi possivel mostrar o video", -1, 0xFFFFFF, 0x101010,
               GUI_TEXT_NOBG);
      gui_text(16, 24 + ch + 4, "Mostra MJPEG, RGB e JPEG.", -1, 0xC0C0C0, 0x101010, GUI_TEXT_NOBG);
      gui_text(16, 24 + 2 * (ch + 4), "Esc fecha", -1, 0xC0C0C0, 0x101010, GUI_TEXT_NOBG);
      gui_present();
      if (gui_wait(&ev, -1) == GUI_EV_CLOSE) break;
      if (ev.type == GUI_EV_CLOSE || (ev.type == GUI_EV_KEY && (ev.key == 27 || ev.key == 'q')))
        break;
      continue;
    }
    if (frame < 0) frame = 0;
    if (frame >= vid.nframes) frame = 0;
    vid_frame(&vid, frame, &im);
    if (im.rgba && im.w > 0)
      fit_rect(sw, sh, im.w, im.h, &x, &y, &w, &h);
    else {
      x = y = 16;
      w = h = 0;
      gui_text(16, 24, "Quadro sem imagem", -1, 0xFFFFFF, 0x101010, GUI_TEXT_NOBG);
    }
    if (im.rgba && w > 0)
      gui_image(x, y, w, h, im.rgba, im.w, im.h, 0, 0, sw, sh - ch - 8);
    snprintf(msg, sizeof msg, "%s  %d/%d  %s  %d ms  %s  sem som   espaco pausa  setas  Esc",
             vid.codec, frame + 1, vid.nframes, paused ? "pausa" : "play", vid.frame_ms,
             base_name(path));
    gui_fill(0, sh - ch - 8, sw, ch + 8, 0x000000);
    gui_text(8, sh - ch - 4, msg, -1, 0xE0E0E0, 0x000000, GUI_TEXT_NOBG);
    gui_present();
    img_free(&im);
    if (paused) {
      if (gui_wait(&ev, -1) == GUI_EV_CLOSE) break;
      if (ev.type == GUI_EV_CLOSE) break;
      if (ev.type == GUI_EV_KEY) key_frame(ev.key, &frame, vid.nframes, &paused, &alive);
    } else {
      unsigned t0 = k_ms();
      int spins = 0;
      while (alive && !paused && (int)(k_ms() - t0) < vid.frame_ms && spins < 20000) {
        int t = gui_wait(&ev, 0);
        spins++;
        if (t == GUI_EV_CLOSE || ev.type == GUI_EV_CLOSE) {
          alive = 0;
          break;
        }
        if (ev.type == GUI_EV_KEY)
          key_frame(ev.key, &frame, vid.nframes, &paused, &alive);
      }
      if (alive && !paused) {
        frame++;
        if (frame >= vid.nframes) frame = 0;
      }
    }
  }
  gui_close();
  vid_free(&vid);
  return 0;
}
#endif
