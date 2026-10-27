/* Host: gcc -O2 -DH264_DEBUG -o h264test h264test.c
 * Decodifica os dois primeiros quadros de um MP4 avc1 e grava PPM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int vid_stop = 0;
void vid_h264_tick(int pct) { (void)pct; }

#include "h264.c"

static unsigned be32(const unsigned char* p) {
  return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

static int find_box(const unsigned char* b, int n, const char* tag, int* off, int* sz) {
  int i;
  for (i = 0; i + 8 <= n; i++) {
    if (b[i + 4] == tag[0] && b[i + 5] == tag[1] && b[i + 6] == tag[2] && b[i + 7] == tag[3]) {
      unsigned s = be32(b + i);
      if (s >= 8 && i + (int)s <= n) {
        *off = i + 8;
        *sz = (int)s - 8;
        return 1;
      }
    }
  }
  return 0;
}

static int write_ppm(const char* path, const uint8_t* rgba, int w, int h) {
  FILE* f = fopen(path, "wb");
  int y, x;
  if (!f) return -1;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      const uint8_t* p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
      fputc(p[0], f);
      fputc(p[1], f);
      fputc(p[2], f);
    }
  }
  fclose(f);
  return 0;
}

int main(int argc, char** argv) {
  FILE* f;
  unsigned char* buf;
  long sz;
  int avcc_off = 0, avcc_sz = 0;
  int i, o, stsz = -1, stco = -1, count = 0;
  unsigned fixed = 0;
  h264_t* h;
  const char* path = argc > 1 ? argv[1] : "content/VIDEO2.mp4";
  f = fopen(path, "rb");
  if (!f) {
    printf("nao abriu %s\n", path);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 32 || sz > 200 * 1024 * 1024) {
    printf("tamanho %ld\n", sz);
    fclose(f);
    return 1;
  }
  buf = (unsigned char*)malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    printf("leitura\n");
    fclose(f);
    return 1;
  }
  fclose(f);
  if (!find_box(buf, (int)sz, "avcC", &avcc_off, &avcc_sz)) {
    printf("sem avcC\n");
    return 1;
  }
  printf("avcC off %d sz %d\n", avcc_off, avcc_sz);
  h = h264_new();
  if (h264_avcc(h, buf + avcc_off, avcc_sz) != 0) {
    printf("avcc falhou %s\n", h264_error(h));
    return 1;
  }
  printf("pic %dx%d disp %dx%d cabac %d t8x8 %d qp %d log2 %d maxref %d wpred %d crop %d %d %d %d\n",
         h->width, h->height, h->disp_w, h->disp_h, h->cabac, h->t8x8, h->init_qp, h->log2_fn, h->max_ref,
         h->wpred, h->crop_l, h->crop_r, h->crop_t, h->crop_b);
  for (i = 0; i + 8 < (int)sz; i++) {
    if (buf[i + 4] == 's' && buf[i + 5] == 't' && buf[i + 6] == 's' && buf[i + 7] == 'z' && stsz < 0) stsz = i + 8;
    if (buf[i + 4] == 's' && buf[i + 5] == 't' && buf[i + 6] == 'c' && buf[i + 7] == 'o' && stco < 0) stco = i + 8;
  }
  if (stsz < 0 || stco < 0) {
    printf("sem tabelas\n");
    return 1;
  }
  fixed = be32(buf + stsz + 4);
  count = (int)be32(buf + stsz + 8);
  printf("stsz %d fixed %u count %d stco %d\n", stsz, fixed, count, stco);
  o = stco + 4;
  {
    int nco = (int)be32(buf + o);
    int fi;
    o += 4;
    printf("chunks %d\n", nco);
    for (fi = 0; fi < 2 && fi < count && fi < nco; fi++) {
      unsigned off = be32(buf + o + fi * 4);
      unsigned ssz;
      uint8_t* rgba = 0;
      int w = 0, ht = 0;
      int rc;
      char name[64];
      if (fixed) ssz = fixed;
      else ssz = be32(buf + stsz + 12 + fi * 4);
      printf("frame %d off %u sz %u\n", fi, off, ssz);
      if (off + ssz > (unsigned)sz) {
        printf("amostra fora\n");
        return 1;
      }
      rc = h264_au(h, buf + off, (int)ssz);
      printf("au %d err %s y0 %d\n", rc, h264_error(h), h->y ? h->y[h->height / 2 * h->stride_y + h->width / 2] : -1);
      {
        int k, nset = 0, c4 = 0, c8 = 0, c16 = 0;
        for (k = 0; k < h->mb_w * h->mb_h; k++) {
          if (!h->mbs[k]) continue;
          nset++;
          if (h->mbt[k] == 0) c4++;
          else if (h->mbt[k] == 2) c8++;
          else if (h->mbt[k] == 1) c16++;
        }
        printf("mbs %d i4 %d i8 %d i16 %d\n", nset, c4, c8, c16);
      }
      if (fi == 0 && h->y && h->u && h->v) {
        FILE* fy = fopen("/mnt/c/Users/Valmor/AppData/Local/Temp/ff264/h264top.ppm", "wb");
        int rows = 64;
        int step = 2;
        int ow, yy, xx;
        if (rows > h->height) rows = h->height;
        ow = h->width / step;
        if (fy) {
          fprintf(fy, "P6\n%d %d\n255\n", ow, rows / step);
          for (yy = 0; yy < rows; yy += step) {
            for (xx = 0; xx < h->width; xx += step) {
              int Y = h->y[yy * h->stride_y + xx];
              int U = h->u[(yy / 2) * h->stride_c + (xx / 2)];
              int V = h->v[(yy / 2) * h->stride_c + (xx / 2)];
              int C = Y - 16;
              int D = U - 128;
              int E = V - 128;
              int R, G, B;
              unsigned char rgb[3];
              if (C < 0) C = 0;
              R = (298 * C + 409 * E + 128) >> 8;
              G = (298 * C - 100 * D - 208 * E + 128) >> 8;
              B = (298 * C + 516 * D + 128) >> 8;
              if (R < 0) R = 0;
              if (G < 0) G = 0;
              if (B < 0) B = 0;
              if (R > 255) R = 255;
              if (G > 255) G = 255;
              if (B > 255) B = 255;
              rgb[0] = (unsigned char)R;
              rgb[1] = (unsigned char)G;
              rgb[2] = (unsigned char)B;
              fwrite(rgb, 1, 3, fy);
            }
          }
          fclose(fy);
          printf("top ppm %dx%d\n", ow, rows / step);
        }
        fy = fopen("/mnt/c/Users/Valmor/AppData/Local/Temp/ff264/h264y.raw", "wb");
        if (fy) {
          int yy;
          for (yy = 0; yy < 32 && yy < h->height; yy++)
            fwrite(h->y + yy * h->stride_y, 1, (size_t)h->width, fy);
          fclose(fy);
        }
      }
      if (rc != 0) return 1;
      if (h264_rgba(h, &rgba, &w, &ht, 320) != 0) return 1;
      snprintf(name, sizeof name, "/mnt/c/Users/Valmor/AppData/Local/Temp/ff264/h264f%d.ppm", fi);
      write_ppm(name, rgba, w, ht);
      printf("ppm %s %dx%d\n", name, w, ht);
      free(rgba);
    }
  }
  h264_free(h);
  free(buf);
  return 0;
}
