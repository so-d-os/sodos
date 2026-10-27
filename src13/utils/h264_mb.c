#ifdef H264_DEBUG
#include <stdio.h>
#endif

static int cnz_at(h264_t* h, int plane, int bx, int by) {
  int bw = h->mb_w * 2;
  int bh = h->mb_h * 2;
  if (bx < 0 || by < 0 || bx >= bw || by >= bh) return 0;
  return h->cnnz[(plane * bh + by) * bw + bx];
}

static void cnz_set(h264_t* h, int plane, int bx, int by, int v) {
  int bw = h->mb_w * 2;
  int bh = h->mb_h * 2;
  if (bx < 0 || by < 0 || bx >= bw || by >= bh) return;
  h->cnnz[(plane * bh + by) * bw + bx] = (uint8_t)v;
}

static int mode_of(h264_t* h, int bx, int by, int mx, int my, const unsigned char* mdone) {
  int omx, omy, mb, bi, k, mbt;
  if (bx < 0 || by < 0 || bx >= h->bw4 || by >= h->bh4) return -1;
  omx = bx >> 2;
  omy = by >> 2;
  mb = omy * h->mb_w + omx;
  if (omx == mx && omy == my) {
    bi = -1;
    for (k = 0; k < 16; k++)
      if (blk_x[k] == (bx & 3) && blk_y[k] == (by & 3)) bi = k;
    if (bi < 0 || !mdone || !mdone[bi]) return -1;
  } else if (!h->mbs[mb]) {
    return -1;
  }
  mbt = h->mbt[mb];
  if (mbt == 1) return 2;
  if (mbt != 0 && mbt != 2) return -1;
  return h->mode4[idx4(h, bx, by)];
}

static int intra_mb_type(h264_t* h, int mb, int intra_slice) {
  int base = intra_slice ? 3 : 17;
  int ctx = 0;
  int s;
  int mx = mb % h->mb_w;
  int my = mb / h->mb_w;
  int mb_type;
  if (intra_slice) {
    if (mx > 0 && (h->mbt[mb - 1] == 1 || h->mbt[mb - 1] == 25)) ctx++;
    if (my > 0 && (h->mbt[mb - h->mb_w] == 1 || h->mbt[mb - h->mb_w] == 25)) ctx++;
    if (!cab_dec(h, base + ctx)) return 0;
    s = 2;
  } else {
    if (!cab_dec(h, base)) return 0;
    s = 0;
  }
  if (cab_term(h)) return 25;
  mb_type = 1;
  mb_type += 12 * cab_dec(h, base + s + 1);
  if (cab_dec(h, base + s + 2)) mb_type += 4 + 4 * cab_dec(h, base + s + 2 + intra_slice);
  mb_type += 2 * cab_dec(h, base + s + 3 + intra_slice);
  mb_type += cab_dec(h, base + s + 3 + 2 * intra_slice);
  return mb_type;
}

static int dec_ipred(h264_t* h, int bx, int by, int mx, int my, const unsigned char* mdone) {
  int la = -1;
  int ta = -1;
  int pred = 2;
  int rem;
  if (bx > 0) la = mode_of(h, bx - 1, by, mx, my, mdone);
  if (by > 0) ta = mode_of(h, bx, by - 1, mx, my, mdone);
  if (la < 0) la = 2;
  if (ta < 0) ta = 2;
  pred = la < ta ? la : ta;
  if (cab_dec(h, 68)) return pred;
  rem = cab_dec(h, 69);
  rem |= cab_dec(h, 69) << 1;
  rem |= cab_dec(h, 69) << 2;
  if (rem >= pred) rem++;
  return rem;
}

static int dec_cmode(h264_t* h, int mb) {
  int mx = mb % h->mb_w;
  int my = mb / h->mb_w;
  int ctx = 0;
  if (mx > 0 && h->cmode[mb - 1] > 0) ctx++;
  if (my > 0 && h->cmode[mb - h->mb_w] > 0) ctx++;
  if (!cab_dec(h, 64 + ctx)) return 0;
  if (!cab_dec(h, 67)) return 1;
  if (!cab_dec(h, 67)) return 2;
  return 3;
}

static int dec_cbp_luma(h264_t* h, int mb) {
  int cbp = 0;
  int i;
  int mx = mb % h->mb_w;
  int my = mb / h->mb_w;
  int left = mx > 0 ? (h->cbpb[mb - 1] & 15) : 15;
  int top = my > 0 ? (h->cbpb[mb - h->mb_w] & 15) : 15;
  for (i = 0; i < 4; i++) {
    int ctx = 0;
    int lb, tb;
    if (i & 1)
      lb = (cbp >> (i - 1)) & 1;
    else if (mx > 0)
      lb = (left >> (i + 1)) & 1;
    else
      lb = 1;
    if (i & 2)
      tb = (cbp >> (i - 2)) & 1;
    else if (my > 0)
      tb = (top >> (i + 2)) & 1;
    else
      tb = 1;
    if (!lb) ctx++;
    if (!tb) ctx += 2;
    if (cab_dec(h, 73 + ctx)) cbp |= 1 << i;
  }
  return cbp;
}

static int dec_cbp_chroma(h264_t* h, int mb) {
  int mx = mb % h->mb_w;
  int my = mb / h->mb_w;
  int lc = mx > 0 ? ((h->cbpb[mb - 1] >> 4) & 3) : 0;
  int tc = my > 0 ? ((h->cbpb[mb - h->mb_w] >> 4) & 3) : 0;
  int ctx = 0;
  if (mx > 0 && lc > 0) ctx++;
  if (my > 0 && tc > 0) ctx += 2;
  if (!cab_dec(h, 77 + ctx)) return 0;
  ctx = 4;
  if (mx > 0 && lc == 2) ctx++;
  if (my > 0 && tc == 2) ctx += 2;
  return 1 + cab_dec(h, 77 + ctx);
}

static void dec_dqp(h264_t* h) {
  int ctx = 60 + (h->last_dqp != 0);
  int val = 1;
  int dqp;
  if (!cab_dec(h, ctx)) {
    h->last_dqp = 0;
    return;
  }
  while (val < 52 && cab_dec(h, 62 + (val > 1))) val++;
  if (val & 1)
    dqp = (val + 1) >> 1;
  else
    dqp = -((val + 1) >> 1);
  h->last_dqp = dqp;
  h->cur_qp = (h->cur_qp + dqp + 104) % 52;
  if (h->cur_qp < 0) h->cur_qp = 0;
  if (h->cur_qp > 51) h->cur_qp = 51;
}

static int pix_ok(h264_t* h, int px, int py, int mx, int my, const unsigned char* done4) {
  int omx, omy, k, bi;
  if (px < 0 || py < 0 || px >= h->width || py >= h->height) return 0;
  omx = px >> 4;
  omy = py >> 4;
  if (omx == mx && omy == my) {
    if (!done4) return 0;
    bi = -1;
    for (k = 0; k < 16; k++)
      if (blk_x[k] == ((px >> 2) & 3) && blk_y[k] == ((py >> 2) & 3)) bi = k;
    if (bi < 0) return 0;
    return done4[bi];
  }
  if (omx >= h->mb_w || omy >= h->mb_h) return 0;
  return h->mbs[omy * h->mb_w + omx] != 0;
}

static void paint4(h264_t* h, int x, int y, int mode, int mx, int my, const unsigned char* done4) {
  int top[8], left[4], tl, i, j, ht, hl, htl;
  uint8_t b[16];
  ht = pix_ok(h, x, y - 1, mx, my, done4);
  hl = pix_ok(h, x - 1, y, mx, my, done4);
  htl = pix_ok(h, x - 1, y - 1, mx, my, done4);
  tl = htl ? h->y[(y - 1) * h->stride_y + (x - 1)] : 128;
  for (i = 0; i < 4; i++) {
    top[i] = ht && pix_ok(h, x + i, y - 1, mx, my, done4) ? h->y[(y - 1) * h->stride_y + (x + i)] : 128;
    left[i] = hl && pix_ok(h, x - 1, y + i, mx, my, done4) ? h->y[(y + i) * h->stride_y + (x - 1)] : 128;
  }
  for (i = 4; i < 8; i++) {
    if (pix_ok(h, x + i, y - 1, mx, my, done4))
      top[i] = h->y[(y - 1) * h->stride_y + (x + i)];
    else
      top[i] = top[3];
  }
  if (!ht && hl) {
    for (i = 0; i < 8; i++) top[i] = left[0];
    tl = left[0];
    ht = 1;
  }
  if (!hl && ht) {
    for (i = 0; i < 4; i++) left[i] = top[0];
    tl = top[0];
    hl = 1;
  }
  if ((!ht && (mode == 0 || mode == 3 || mode == 7)) || (!hl && (mode == 1 || mode == 8))) mode = 2;
  if (mode == 2) {
    int s = 0, n = 0, dc;
    if (ht) {
      for (i = 0; i < 4; i++) s += top[i];
      n += 4;
    }
    if (hl) {
      for (i = 0; i < 4; i++) s += left[i];
      n += 4;
    }
    dc = n ? (s + (n >> 1)) / n : 128;
    for (i = 0; i < 16; i++) b[i] = (uint8_t)dc;
  } else {
    for (j = 0; j < 4; j++) {
      for (i = 0; i < 4; i++) {
        int v = 128;
        if (mode == 0) v = top[i];
        else if (mode == 1) v = left[j];
        else if (mode == 3) v = (top[i + j] + 2 * top[i + j + 1] + top[i + j + 2] + 2) >> 2;
        else if (mode == 4) {
          if (i > j) v = (top[i - j - 2 < 0 ? 0 : i - j - 2] + 2 * top[i - j - 1 < 0 ? 0 : i - j - 1] + top[i - j] + 2) >> 2;
          else if (i == j) v = (tl + 2 * top[0] + top[1] + 2) >> 2;
          else if (j == i + 1) v = (left[0] + 2 * tl + top[0] + 2) >> 2;
          else v = (left[j - i - 2] + 2 * left[j - i - 1] + left[j - i] + 2) >> 2;
        } else if (mode == 5) {
          int z = 2 * i - j;
          if (z == 0 || z == 2 || z == 4 || z == 6) v = (top[i - (j >> 1)] + top[i - (j >> 1) + 1] + 1) >> 1;
          else if (z == 1 || z == 3 || z == 5) v = (top[i - (j >> 1) - 1 < 0 ? 0 : i - (j >> 1) - 1] + 2 * top[i - (j >> 1)] + top[i - (j >> 1) + 1] + 2) >> 2;
          else if (z == -1) v = (left[0] + 2 * tl + top[0] + 2) >> 2;
          else v = left[j > 0 ? j - 1 : 0];
        } else if (mode == 6) {
          if (j == i) v = (tl + 2 * left[j] + left[j + 1 < 4 ? j + 1 : j] + 2) >> 2;
          else if (j > i) v = (left[j - i] + left[j - i - 1] + 1) >> 1;
          else v = (top[i - j] + 2 * top[i - j - 1 < 0 ? 0 : i - j - 1] + (i - j >= 2 ? top[i - j - 2] : tl) + 2) >> 2;
        } else if (mode == 7) {
          v = (top[i + j] + 2 * top[i + j + 1] + top[i + j + 2] + 2) >> 2;
          if ((j & 1) == 0) v = (top[i + (j >> 1)] + top[i + (j >> 1) + 1] + 1) >> 1;
        } else if (mode == 8) {
          int z = i + 2 * j;
          if (z < 6) v = (left[j + (i >> 1)] + left[j + (i >> 1) + 1 < 4 ? j + (i >> 1) + 1 : 3] + 1) >> 1;
          else v = left[3];
        }
        b[j * 4 + i] = (uint8_t)v;
      }
    }
  }
  fill_pred(h, x, y, b, 4);
}

static void pred16(h264_t* h, int x, int y, int mode) {
  int top[16], left[16], tl, i, j, ht, hl, s, n, dc;
  ht = y > 0;
  hl = x > 0;
  tl = (ht && hl) ? h->y[(y - 1) * h->stride_y + (x - 1)] : 128;
  for (i = 0; i < 16; i++) {
    top[i] = ht ? h->y[(y - 1) * h->stride_y + (x + i)] : 128;
    left[i] = hl ? h->y[(y + i) * h->stride_y + (x - 1)] : 128;
  }
  if (mode == 0 && ht) {
    for (j = 0; j < 16; j++)
      for (i = 0; i < 16; i++) h->y[(y + j) * h->stride_y + (x + i)] = (uint8_t)top[i];
    return;
  }
  if (mode == 1 && hl) {
    for (j = 0; j < 16; j++)
      for (i = 0; i < 16; i++) h->y[(y + j) * h->stride_y + (x + i)] = (uint8_t)left[j];
    return;
  }
  if (mode == 3 && ht && hl) {
    int H = 0, V = 0, a, b, c;
    for (i = 0; i < 8; i++) {
      int tr = (i == 7) ? top[15] : top[8 + i];
      int tu = (i == 7) ? tl : top[6 - i];
      int lr = left[8 + i];
      int lu = (i == 7) ? tl : left[6 - i];
      H += (i + 1) * (tr - tu);
      V += (i + 1) * (lr - lu);
    }
    a = 16 * (top[15] + left[15]);
    b = (5 * H + 32) >> 6;
    c = (5 * V + 32) >> 6;
    for (j = 0; j < 16; j++)
      for (i = 0; i < 16; i++)
        h->y[(y + j) * h->stride_y + (x + i)] = (uint8_t)clip8((a + b * (i - 7) + c * (j - 7) + 16) >> 5);
    return;
  }
  s = 0;
  n = 0;
  if (ht) {
    for (i = 0; i < 16; i++) s += top[i];
    n += 16;
  }
  if (hl) {
    for (i = 0; i < 16; i++) s += left[i];
    n += 16;
  }
  dc = n ? (s + (n >> 1)) / n : 128;
  for (j = 0; j < 16; j++)
    for (i = 0; i < 16; i++) h->y[(y + j) * h->stride_y + (x + i)] = (uint8_t)dc;
}

static void pred_c(h264_t* h, uint8_t* p, int x, int y, int mode) {
  int st = h->stride_c;
  int sw = h->width / 2;
  int sh = h->height / 2;
  int top[8], left[8], tl, i, j, ht, hl, s, n, dc;
  ht = y > 0;
  hl = x > 0;
  tl = (ht && hl) ? c_at(p, st, sw, sh, x - 1, y - 1) : 128;
  for (i = 0; i < 8; i++) {
    top[i] = ht ? c_at(p, st, sw, sh, x + i, y - 1) : 128;
    left[i] = hl ? c_at(p, st, sw, sh, x - 1, y + i) : 128;
  }
  if (mode == 2 && ht) {
    for (j = 0; j < 8; j++)
      for (i = 0; i < 8; i++) p[(y + j) * st + (x + i)] = (uint8_t)top[i];
    return;
  }
  if (mode == 1 && hl) {
    for (j = 0; j < 8; j++)
      for (i = 0; i < 8; i++) p[(y + j) * st + (x + i)] = (uint8_t)left[j];
    return;
  }
  if (mode == 3 && ht && hl) {
    int H = 0, V = 0, a, b, c;
    for (i = 0; i < 4; i++) {
      H += (i + 1) * (top[4 + i] - (i == 3 ? tl : top[2 - i]));
      V += (i + 1) * (left[4 + i] - (i == 3 ? tl : left[2 - i]));
    }
    a = 16 * (top[7] + left[7]);
    b = (17 * H + 16) >> 5;
    c = (17 * V + 16) >> 5;
    for (j = 0; j < 8; j++)
      for (i = 0; i < 8; i++)
        p[(y + j) * st + (x + i)] = (uint8_t)clip8((a + b * (i - 3) + c * (j - 3) + 16) >> 5);
    return;
  }
  s = 0;
  n = 0;
  if (ht) {
    for (i = 0; i < 8; i++) s += top[i];
    n += 8;
  }
  if (hl) {
    for (i = 0; i < 8; i++) s += left[i];
    n += 8;
  }
  dc = n ? (s + (n >> 1)) / n : 128;
  for (j = 0; j < 8; j++)
    for (i = 0; i < 8; i++) p[(y + j) * st + (x + i)] = (uint8_t)dc;
}

static void hada2(int* s) {
  int a = s[0], b = s[1], c = s[2], d = s[3];
  s[0] = a + b + c + d;
  s[1] = a - b + c - d;
  s[2] = a + b - c - d;
  s[3] = a - b - c + d;
}

static int t8_flag(h264_t* h, int mb) {
  int mx = mb % h->mb_w;
  int my = mb / h->mb_w;
  int ctx = 0;
  if (mx > 0 && h->mb8[mb - 1]) ctx++;
  if (my > 0 && h->mb8[mb - h->mb_w]) ctx++;
  return cab_dec(h, 399 + ctx);
}

static void part_mv(h264_t* h, int bx, int by, int bw, int bh, int ref) {
  int ox = 0, oy = 0, mdx, mdy;
  pred_mv(h, bx, by, bw, bh, ref, &ox, &oy);
  mdx = dec_mvd_comp(h, 40, neigh_mvd(h, bx, by, 0));
  mdy = dec_mvd_comp(h, 47, neigh_mvd(h, bx, by, 1));
  put_mv(h, bx, by, bw, bh, ox + mdx, oy + mdy, mdx, mdy, ref);
}

static void w_rect(h264_t* h, int x, int y, int w, int ht, int ref, int chroma, int plane) {
  int den, wv, off, j, i;
  uint8_t* p;
  int st;
  if (!h->wpred || ref < 0 || ref > 15) return;
  if (!chroma) {
    if (!h->w_flag[ref]) return;
    den = h->w_den;
    wv = h->w_luma[ref];
    off = h->w_off[ref];
    p = h->y;
    st = h->stride_y;
  } else {
    if (!h->wc_flag[ref]) return;
    den = h->wc_den;
    wv = plane ? h->wc_r[ref] : h->wc_b[ref];
    off = plane ? h->wc_ro[ref] : h->wc_bo[ref];
    p = plane ? h->v : h->u;
    st = h->stride_c;
  }
  for (j = 0; j < ht; j++) {
    for (i = 0; i < w; i++) {
      int s = p[(y + j) * st + (x + i)];
      int o;
      if (den >= 1) o = ((s * wv + (1 << (den - 1))) >> den) + off;
      else o = s * wv + off;
      p[(y + j) * st + (x + i)] = (uint8_t)clip8(o);
    }
  }
}

static void mc_part(h264_t* h, int x, int y, int w, int ht, int ref) {
  int slot = 0;
  if (ref < 0) ref = 0;
  if (ref < h->list_n) slot = h->list_slot[ref];
  if (slot < 0 || slot >= h->ref_n) slot = 0;
  if (h->ref_n <= 0 || !h->refy[slot]) return;
  mc_luma(h, h->refy[slot], x, y, w, ht, h->mvx[idx4(h, x >> 2, y >> 2)], h->mvy[idx4(h, x >> 2, y >> 2)]);
  w_rect(h, x, y, w, ht, ref, 0, 0);
  mc_chroma(h, h->refu[slot], x >> 1, y >> 1, w >> 1, ht >> 1, h->mvx[idx4(h, x >> 2, y >> 2)],
            h->mvy[idx4(h, x >> 2, y >> 2)]);
  mc_chroma(h, h->refv[slot], x >> 1, y >> 1, w >> 1, ht >> 1, h->mvx[idx4(h, x >> 2, y >> 2)],
            h->mvy[idx4(h, x >> 2, y >> 2)]);
  w_rect(h, x >> 1, y >> 1, w >> 1, ht >> 1, ref, 1, 0);
  w_rect(h, x >> 1, y >> 1, w >> 1, ht >> 1, ref, 1, 1);
}

static int cbp_covers(int cbp, int bx, int by) {
  int bit = (bx >> 1) + (by >> 1) * 2;
  return (cbp >> bit) & 1;
}

static void add_dc4(h264_t* h, int x, int y, const int* coef, int qp, int dc) {
  int tmp[16];
  int i;
  for (i = 0; i < 16; i++) tmp[i] = coef[i];
  tmp[0] = 0;
  recon4(h, x, y, tmp, qp, dc);
}

static int decode_mb(h264_t* h, int mb, int mx, int my, int st) {
  int intra = (st == 2);
  int i16 = 0;
  int is8 = 0;
  int can8 = 0;
  int pmb = 0;
  int predm = 2;
  int cbp_y = 0;
  int cbp_c = 0;
  int ctype = 0;
  int sub[4];
  int refs[4];
  int i, k, bi;
  unsigned char mdone[16];
  unsigned char done4[16];
  int ac[16][16];
  int a8[4][64];
  int dc16[16];
  int cdc[2][4];
  int cac[2][4][16];
  int dc_nz = 0;
  if (h->cab_byte > h->cab_n) {
    set_err(h, "Quadro cortado");
    return -1;
  }
  for (i = 0; i < 16; i++) {
    mdone[i] = 0;
    done4[i] = 0;
    refs[i < 4 ? i : 0] = 0;
    for (k = 0; k < 16; k++) ac[i][k] = 0;
  }
  for (i = 0; i < 4; i++) {
    sub[i] = 0;
    refs[i] = 0;
    for (k = 0; k < 64; k++) a8[i][k] = 0;
  }
  for (i = 0; i < 16; i++) dc16[i] = 0;
  for (i = 0; i < 2; i++) {
    for (k = 0; k < 4; k++) {
      int t;
      cdc[i][k] = 0;
      for (t = 0; t < 16; t++) cac[i][k][t] = 0;
    }
  }
  if (!intra) {
    if (cab_dec(h, 14) == 0) {
      if (cab_dec(h, 15) == 0)
        pmb = cab_dec(h, 16) ? 3 : 0;
      else
        pmb = cab_dec(h, 17) ? 1 : 2;
      h->mbt[mb] = 3;
    } else {
      intra = 1;
      pmb = intra_mb_type(h, mb, 0);
    }
  } else {
    pmb = intra_mb_type(h, mb, 1);
  }
  if (pmb == 25) {
    set_err(h, "PCM nao entra");
    return -1;
  }
  if (intra) {
    if (pmb >= 1) {
      int v = pmb - 1;
      int tmp;
      i16 = 1;
      predm = v % 4;
      tmp = v / 4;
      cbp_c = tmp % 3;
      cbp_y = tmp >= 3 ? 15 : 0;
      h->mbt[mb] = 1;
      h->mb8[mb] = 0;
      ctype = dec_cmode(h, mb);
      h->cmode[mb] = (int8_t)ctype;
    } else {
      h->mbt[mb] = 0;
      if (h->t8x8) is8 = t8_flag(h, mb);
      h->mb8[mb] = (uint8_t)is8;
      if (is8) h->mbt[mb] = 2;
      if (is8) {
        for (i = 0; i < 4; i++) {
          int sbx = (i & 1) * 2;
          int sby = (i >> 1) * 2;
          int mode = dec_ipred(h, mx * 4 + sbx, my * 4 + sby, mx, my, mdone);
          int a, b;
          for (b = 0; b < 2; b++)
            for (a = 0; a < 2; a++) {
              int lx = sbx + a;
              int ly = sby + b;
              int id = 0;
              h->mode4[idx4(h, mx * 4 + lx, my * 4 + ly)] = (int8_t)mode;
              for (k = 0; k < 16; k++)
                if (blk_x[k] == lx && blk_y[k] == ly) id = k;
              mdone[id] = 1;
            }
        }
      } else {
        for (bi = 0; bi < 16; bi++) {
          int mode = dec_ipred(h, mx * 4 + blk_x[bi], my * 4 + blk_y[bi], mx, my, mdone);
          h->mode4[idx4(h, mx * 4 + blk_x[bi], my * 4 + blk_y[bi])] = (int8_t)mode;
          mdone[bi] = 1;
        }
      }
      ctype = dec_cmode(h, mb);
      h->cmode[mb] = (int8_t)ctype;
      cbp_y = dec_cbp_luma(h, mb);
      cbp_c = dec_cbp_chroma(h, mb);
    }
    put_mv(h, mx * 4, my * 4, 4, 4, 0, 0, 0, 0, -1);
  } else {
    int npart = (pmb == 0) ? 1 : 2;
    int all8 = 1;
    h->cmode[mb] = -1;
    h->mb8[mb] = 0;
    if (pmb == 3) {
      for (i = 0; i < 4; i++) {
        if (cab_dec(h, 21))
          sub[i] = 0;
        else if (!cab_dec(h, 22))
          sub[i] = 1;
        else if (cab_dec(h, 23))
          sub[i] = 2;
        else
          sub[i] = 3;
        if (sub[i] != 0) all8 = 0;
      }
      for (i = 0; i < 4; i++) {
        refs[i] = h->nref > 1 ? dec_ref(h, mx * 4 + (i & 1) * 2, my * 4 + (i >> 1) * 2) : 0;
        if (refs[i] >= h->nref) refs[i] = h->nref - 1;
        if (refs[i] < 0) refs[i] = 0;
      }
      for (i = 0; i < 4; i++) {
        int sbx = (i & 1) * 2;
        int sby = (i >> 1) * 2;
        int t = sub[i];
        if (t == 0) part_mv(h, mx * 4 + sbx, my * 4 + sby, 2, 2, refs[i]);
        else if (t == 1) {
          part_mv(h, mx * 4 + sbx, my * 4 + sby, 2, 1, refs[i]);
          part_mv(h, mx * 4 + sbx, my * 4 + sby + 1, 2, 1, refs[i]);
        } else if (t == 2) {
          part_mv(h, mx * 4 + sbx, my * 4 + sby, 1, 2, refs[i]);
          part_mv(h, mx * 4 + sbx + 1, my * 4 + sby, 1, 2, refs[i]);
        } else {
          part_mv(h, mx * 4 + sbx, my * 4 + sby, 1, 1, refs[i]);
          part_mv(h, mx * 4 + sbx + 1, my * 4 + sby, 1, 1, refs[i]);
          part_mv(h, mx * 4 + sbx, my * 4 + sby + 1, 1, 1, refs[i]);
          part_mv(h, mx * 4 + sbx + 1, my * 4 + sby + 1, 1, 1, refs[i]);
        }
      }
      if (all8) can8 = 1;
    } else if (pmb == 0) {
      refs[0] = h->nref > 1 ? dec_ref(h, mx * 4, my * 4) : 0;
      part_mv(h, mx * 4, my * 4, 4, 4, refs[0]);
    } else if (pmb == 1) {
      for (i = 0; i < npart; i++) {
        refs[i] = h->nref > 1 ? dec_ref(h, mx * 4, my * 4 + i * 2) : 0;
      }
      for (i = 0; i < npart; i++) part_mv(h, mx * 4, my * 4 + i * 2, 4, 2, refs[i]);
    } else {
      for (i = 0; i < npart; i++) {
        refs[i] = h->nref > 1 ? dec_ref(h, mx * 4 + i * 2, my * 4) : 0;
      }
      for (i = 0; i < npart; i++) part_mv(h, mx * 4 + i * 2, my * 4, 2, 4, refs[i]);
    }
    cbp_y = dec_cbp_luma(h, mb);
    cbp_c = dec_cbp_chroma(h, mb);
    if (can8 && h->t8x8 && cbp_y) {
      is8 = t8_flag(h, mb);
      h->mb8[mb] = (uint8_t)is8;
    }
  }
  if (cbp_y || cbp_c || i16)
    dec_dqp(h);
  else
    h->last_dqp = 0;
  h->qp[mb] = (int8_t)h->cur_qp;
  if (i16) {
    int nz = 0;
    int left = mx > 0 ? (h->mbt[mb - 1] == 1 && (h->cbpb[mb - 1] & 0x80)) : intra;
    int top = my > 0 ? (h->mbt[mb - h->mb_w] == 1 && (h->cbpb[mb - h->mb_w] & 0x80)) : intra;
    if (cbf(h, 0, left, top)) nz = decode_coefs(h, dc16, 16, 0, 0);
    dc_nz = nz > 0;
  }
  if (is8) {
    for (i = 0; i < 4; i++) {
      int sbx = (i & 1) * 2;
      int sby = (i >> 1) * 2;
      int nz = 0;
      if (cbp_covers(cbp_y, sbx, sby)) nz = decode_coefs(h, a8[i], 64, 5, 1);
      for (k = 0; k < 4; k++) h->nnz[idx4(h, mx * 4 + sbx + (k & 1), my * 4 + sby + (k >> 1))] = (uint8_t)(nz > 0);
    }
  } else {
    for (bi = 0; bi < 16; bi++) {
      int bx = blk_x[bi];
      int by = blk_y[bi];
      int nz = 0;
      int cat = i16 ? 1 : 2;
      int ncf = i16 ? 15 : 16;
      if (!i16 && !cbp_covers(cbp_y, bx, by)) {
        h->nnz[idx4(h, mx * 4 + bx, my * 4 + by)] = 0;
        continue;
      }
      if (i16 && !cbp_covers(cbp_y, bx, by)) {
        h->nnz[idx4(h, mx * 4 + bx, my * 4 + by)] = 0;
        continue;
      }
      {
        int left = (mx * 4 + bx > 0) ? h->nnz[idx4(h, mx * 4 + bx - 1, my * 4 + by)] : (intra ? 1 : 0);
        int top = (my * 4 + by > 0) ? h->nnz[idx4(h, mx * 4 + bx, my * 4 + by - 1)] : (intra ? 1 : 0);
        if (cbf(h, cat, left, top)) nz = decode_coefs(h, ac[bi], ncf, cat, 0);
      }
#ifdef H264_DEBUG
      if (h->idr && mb < 2)
        printf("blk mb %d i %d xy %d %d left %d top %d nz %d\n", mb, bi, bx, by, 
               (mx * 4 + bx > 0) ? h->nnz[idx4(h, mx * 4 + bx - 1, my * 4 + by)] : -1,
               (my * 4 + by > 0) ? h->nnz[idx4(h, mx * 4 + bx, my * 4 + by - 1)] : -1, nz);
#endif
      h->nnz[idx4(h, mx * 4 + bx, my * 4 + by)] = (uint8_t)nz;
    }
  }
  for (i = 0; i < 2; i++) {
    int left, top, nz = 0;
    if (!cbp_c) {
      h->cdcz[mb * 2 + i] = 0;
      continue;
    }
    left = mx > 0 ? h->cdcz[(mb - 1) * 2 + i] : (intra ? 1 : 0);
    top = my > 0 ? h->cdcz[(mb - h->mb_w) * 2 + i] : (intra ? 1 : 0);
    if (cbf(h, 3, left, top)) nz = decode_coefs(h, cdc[i], 4, 3, 0);
    h->cdcz[mb * 2 + i] = (uint8_t)(nz > 0);
  }
  if (cbp_c == 2) {
    for (i = 0; i < 2; i++) {
      for (k = 0; k < 4; k++) {
        int bx = mx * 2 + (k & 1);
        int by = my * 2 + (k >> 1);
        int left = bx > 0 ? cnz_at(h, i, bx - 1, by) : (intra ? 1 : 0);
        int top = by > 0 ? cnz_at(h, i, bx, by - 1) : (intra ? 1 : 0);
        int nz = 0;
        if (cbf(h, 4, left, top)) nz = decode_coefs(h, cac[i][k], 15, 4, 0);
        cnz_set(h, i, bx, by, nz);
      }
    }
  } else {
    for (i = 0; i < 2; i++)
      for (k = 0; k < 4; k++) cnz_set(h, i, mx * 2 + (k & 1), my * 2 + (k >> 1), 0);
  }
  h->cbpb[mb] = (uint8_t)((cbp_y & 15) | ((cbp_c & 3) << 4) | (dc_nz ? 0x80 : 0));
  if (!intra) {
    if (pmb == 0) mc_part(h, mx * 16, my * 16, 16, 16, refs[0]);
    else if (pmb == 1) {
      mc_part(h, mx * 16, my * 16, 16, 8, refs[0]);
      mc_part(h, mx * 16, my * 16 + 8, 16, 8, refs[1]);
    } else if (pmb == 2) {
      mc_part(h, mx * 16, my * 16, 8, 16, refs[0]);
      mc_part(h, mx * 16 + 8, my * 16, 8, 16, refs[1]);
    } else {
      for (i = 0; i < 4; i++) {
        int sbx = (i & 1) * 8;
        int sby = (i >> 1) * 8;
        int t = sub[i];
        if (t == 0) mc_part(h, mx * 16 + sbx, my * 16 + sby, 8, 8, refs[i]);
        else if (t == 1) {
          mc_part(h, mx * 16 + sbx, my * 16 + sby, 8, 4, refs[i]);
          mc_part(h, mx * 16 + sbx, my * 16 + sby + 4, 8, 4, refs[i]);
        } else if (t == 2) {
          mc_part(h, mx * 16 + sbx, my * 16 + sby, 4, 8, refs[i]);
          mc_part(h, mx * 16 + sbx + 4, my * 16 + sby, 4, 8, refs[i]);
        } else {
          mc_part(h, mx * 16 + sbx, my * 16 + sby, 4, 4, refs[i]);
          mc_part(h, mx * 16 + sbx + 4, my * 16 + sby, 4, 4, refs[i]);
          mc_part(h, mx * 16 + sbx, my * 16 + sby + 4, 4, 4, refs[i]);
          mc_part(h, mx * 16 + sbx + 4, my * 16 + sby + 4, 4, 4, refs[i]);
        }
      }
    }
  } else if (i16) {
    pred16(h, mx * 16, my * 16, predm);
  }
  if (is8) {
    for (i = 0; i < 4; i++) {
      int x = mx * 16 + (i & 1) * 8;
      int y = my * 16 + (i >> 1) * 8;
      if (intra) {
        int mode = h->mode4[idx4(h, mx * 4 + (i & 1) * 2, my * 4 + (i >> 1) * 2)];
        int j, t;
        uint8_t blk[64];
        for (j = 0; j < 8; j++)
          for (t = 0; t < 8; t++) {
            int yy = y + j;
            int xx = x + t;
            int v = 128;
            if (mode == 0 && yy > 0) v = h->y[(yy - 1) * h->stride_y + xx];
            else if (mode == 1 && xx > 0) v = h->y[yy * h->stride_y + xx - 1];
            else if (yy > 0 && xx > 0) {
              int s2 = 0, n2 = 0, u;
              if (j == 0 && t == 0) {
                for (u = 0; u < 8; u++) {
                  if (y > 0) {
                    s2 += h->y[(y - 1) * h->stride_y + x + u];
                    n2++;
                  }
                  if (x > 0) {
                    s2 += h->y[(y + u) * h->stride_y + x - 1];
                    n2++;
                  }
                }
                v = n2 ? (s2 + n2 / 2) / n2 : 128;
              } else if (mode == 0)
                v = h->y[(y - 1) * h->stride_y + xx];
              else
                v = x > 0 ? h->y[yy * h->stride_y + x - 1] : 128;
            }
            blk[j * 8 + t] = (uint8_t)v;
          }
        if (mode == 2 || (mode != 0 && mode != 1)) {
          int s2 = 0, n2 = 0, u, dc;
          for (u = 0; u < 8; u++) {
            if (y > 0) {
              s2 += h->y[(y - 1) * h->stride_y + x + u];
              n2++;
            }
            if (x > 0) {
              s2 += h->y[(y + u) * h->stride_y + x - 1];
              n2++;
            }
          }
          dc = n2 ? (s2 + n2 / 2) / n2 : 128;
          if (mode == 2) {
            for (j = 0; j < 64; j++) blk[j] = (uint8_t)dc;
          }
        }
        for (j = 0; j < 8; j++)
          for (t = 0; t < 8; t++) h->y[(y + j) * h->stride_y + (x + t)] = blk[j * 8 + t];
      }
      recon8(h, x, y, a8[i], h->cur_qp, intra);
    }
  } else if (i16) {
    int f[16];
    int sc[16];
    ihada4(dc16, f);
    for (i = 0; i < 16; i++) sc[i] = scale_dc16(f[i], h->cur_qp);
    for (bi = 0; bi < 16; bi++) {
      int x = mx * 16 + blk_x[bi] * 4;
      int y = my * 16 + blk_y[bi] * 4;
      add_dc4(h, x, y, ac[bi], h->cur_qp, sc[blk_y[bi] * 4 + blk_x[bi]]);
      done4[bi] = 1;
    }
  } else {
    for (bi = 0; bi < 16; bi++) {
      int x = mx * 16 + blk_x[bi] * 4;
      int y = my * 16 + blk_y[bi] * 4;
      int mode = 2;
      if (intra) {
        mode = h->mode4[idx4(h, mx * 4 + blk_x[bi], my * 4 + blk_y[bi])];
        paint4(h, x, y, mode, mx, my, done4);
      }
      recon4(h, x, y, ac[bi], h->cur_qp, 0);
      done4[bi] = 1;
    }
  }
  if (intra) {
    int cx = mx * 8;
    int cy = my * 8;
    int plane;
    pred_c(h, h->u, cx, cy, ctype);
    pred_c(h, h->v, cx, cy, ctype);
    for (plane = 0; plane < 2; plane++) {
      uint8_t* p = plane ? h->v : h->u;
      int qp = chroma_qp(h, h->cur_qp, plane);
      int sc[4];
      hada2(cdc[plane]);
      for (k = 0; k < 4; k++) sc[k] = cbp_c ? scale_dc2(cdc[plane][k], qp) : 0;
      for (k = 0; k < 4; k++) {
        int x = cx + (k & 1) * 4;
        int y = cy + (k >> 1) * 4;
        int coef[16];
        int t;
        int d[16], r[16];
        for (t = 0; t < 16; t++) coef[t] = cac[plane][k][t];
        for (t = 0; t < 16; t++) {
          int xx = t & 3;
          int yy = t >> 2;
          if (t == 0) d[0] = sc[k];
          else d[t] = dequant4(coef[t], qp, xx, yy);
        }
        idct4(d, r);
        add_c(p, h->stride_c, x, y, r, 4);
      }
    }
  } else {
    int cx = mx * 8;
    int cy = my * 8;
    int plane;
    for (plane = 0; plane < 2; plane++) {
      uint8_t* p = plane ? h->v : h->u;
      int qp = chroma_qp(h, h->cur_qp, plane);
      int sc[4];
      hada2(cdc[plane]);
      for (k = 0; k < 4; k++) sc[k] = cbp_c ? scale_dc2(cdc[plane][k], qp) : 0;
      for (k = 0; k < 4; k++) {
        int x = cx + (k & 1) * 4;
        int y = cy + (k >> 1) * 4;
        int coef[16];
        int t;
        int d[16], r[16];
        for (t = 0; t < 16; t++) coef[t] = cac[plane][k][t];
        for (t = 0; t < 16; t++) {
          int xx = t & 3;
          int yy = t >> 2;
          if (t == 0) d[0] = sc[k];
          else d[t] = dequant4(coef[t], qp, xx, yy);
        }
        idct4(d, r);
        add_c(p, h->stride_c, x, y, r, 4);
      }
    }
  }
#ifdef H264_DEBUG
  if (h->idr && mb < 8) {
    int s = 0, t;
    for (t = 0; t < 16; t++) {
      int u;
      for (u = 0; u < 16; u++) s += ac[t][u] < 0 ? -ac[t][u] : ac[t][u];
    }
    for (t = 0; t < 4; t++) {
      int u;
      for (u = 0; u < 64; u++) s += a8[t][u] < 0 ? -a8[t][u] : a8[t][u];
    }
    printf("mb %d t %d 8 %d coefsum %d cbp %d/%d qp %d cab %d\n", mb, h->mbt[mb], is8, s, cbp_y, cbp_c,
           h->cur_qp, h->cab_byte);
  }
#endif
  return 0;
}

static int edge_bs(h264_t* h, int pbx, int pby, int qbx, int qby, int mb_edge) {
  int pmx, pmy, qmx, qmy, pmb, qmb, pt, qt;
  if (pbx < 0 || pby < 0 || qbx < 0 || qby < 0) return 0;
  if (pbx >= h->bw4 || qbx >= h->bw4 || pby >= h->bh4 || qby >= h->bh4) return 0;
  pmx = pbx >> 2;
  pmy = pby >> 2;
  qmx = qbx >> 2;
  qmy = qby >> 2;
  pmb = pmy * h->mb_w + pmx;
  qmb = qmy * h->mb_w + qmx;
  pt = h->mbt[pmb];
  qt = h->mbt[qmb];
  if (pt == 0 || pt == 1 || pt == 2 || qt == 0 || qt == 1 || qt == 2) return mb_edge ? 4 : 3;
  if (h->nnz[idx4(h, pbx, pby)] || h->nnz[idx4(h, qbx, qby)]) return 2;
  if (h->refi[idx4(h, pbx, pby)] != h->refi[idx4(h, qbx, qby)]) return 1;
  if (iabs(h->mvx[idx4(h, pbx, pby)] - h->mvx[idx4(h, qbx, qby)]) >= 4) return 1;
  if (iabs(h->mvy[idx4(h, pbx, pby)] - h->mvy[idx4(h, qbx, qby)]) >= 4) return 1;
  return 0;
}

static void filt_pix(int* p, int bs, int alpha, int beta, int tc0, int chroma) {
  int p0 = p[3], p1 = p[2], p2 = p[1], p3 = p[0];
  int q0 = p[4], q1 = p[5], q2 = p[6], q3 = p[7];
  int ap, aq;
  if (bs <= 0 || alpha <= 0) return;
  if (iabs(p0 - q0) >= alpha || iabs(p1 - p0) >= beta || iabs(q1 - q0) >= beta) return;
  ap = iabs(p2 - p0) < beta;
  aq = iabs(q2 - q0) < beta;
  if (bs < 4) {
    int tc = tc0;
    int d;
    if (ap) tc++;
    if (aq) tc++;
    d = ((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
    if (d > tc) d = tc;
    if (d < -tc) d = -tc;
    p[3] = clip8(p0 + d);
    p[4] = clip8(q0 - d);
    if (!chroma && ap) {
      int d2 = (p2 + ((p[3] + q0 + 1) >> 1) - (p1 << 1)) >> 1;
      if (d2 > tc0) d2 = tc0;
      if (d2 < -tc0) d2 = -tc0;
      p[2] = clip8(p1 + d2);
    }
    if (!chroma && aq) {
      int d2 = (q2 + ((p[4] + p0 + 1) >> 1) - (q1 << 1)) >> 1;
      if (d2 > tc0) d2 = tc0;
      if (d2 < -tc0) d2 = -tc0;
      p[5] = clip8(q1 + d2);
    }
  } else {
    int strong = iabs(p0 - q0) < ((alpha >> 2) + 2);
    if (!chroma && ap && strong)
      p[3] = (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3;
    else
      p[3] = (2 * p1 + p0 + q1 + 2) >> 2;
    if (!chroma && ap && strong) {
      p[2] = (p2 + p1 + p0 + q0 + 2) >> 2;
      p[1] = (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3;
    }
    if (!chroma && aq && strong)
      p[4] = (q2 + 2 * q1 + 2 * q0 + 2 * p0 + p1 + 4) >> 3;
    else
      p[4] = (2 * q1 + q0 + p1 + 2) >> 2;
    if (!chroma && aq && strong) {
      p[5] = (q2 + q1 + q0 + p0 + 2) >> 2;
      p[6] = (2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3;
    }
    if (chroma) {
      p[3] = (2 * p1 + p0 + q1 + 2) >> 2;
      p[4] = (2 * q1 + q0 + p1 + 2) >> 2;
    }
  }
}

static void deblock_pic(h264_t* h) {
  int mx, my, e, row, vert;
  if (!h->y || h->mb_w < 1) return;
  for (vert = 1; vert >= 0; vert--) {
    for (my = 0; my < h->mb_h; my++) {
      for (mx = 0; mx < h->mb_w; mx++) {
        for (e = 0; e < 4; e++) {
          int mb_edge = (e == 0);
          if (vert && mx == 0 && e == 0) continue;
          if (!vert && my == 0 && e == 0) continue;
          for (row = 0; row < 4; row++) {
            int pbx, pby, qbx, qby, bs, x, y, k;
            int samp[8];
            int qp, ia, ib, alpha, beta, tc0;
            int pmb, qmb;
            if (vert) {
              qbx = mx * 4 + e;
              qby = my * 4 + row;
              pbx = qbx - 1;
              pby = qby;
            } else {
              qbx = mx * 4 + row;
              qby = my * 4 + e;
              pbx = qbx;
              pby = qby - 1;
            }
            bs = edge_bs(h, pbx, pby, qbx, qby, mb_edge);
            if (bs <= 0) continue;
            pmb = (pby >> 2) * h->mb_w + (pbx >> 2);
            qmb = (qby >> 2) * h->mb_w + (qbx >> 2);
            qp = (h->qp[pmb] + h->qp[qmb] + 1) >> 1;
            ia = iclip(qp + h->aoff * 2, 0, 51);
            ib = iclip(qp + h->boff * 2, 0, 51);
            alpha = alpha_t[ia];
            beta = beta_t[ib];
            tc0 = bs < 4 ? tc0_t[ia][bs - 1] : 0;
            for (k = 0; k < 4; k++) {
              int n;
              if (vert) {
                x = qbx * 4;
                y = qby * 4 + k;
                if (x < 4 || x >= h->width) continue;
                for (n = 0; n < 8; n++) samp[n] = h->y[y * h->stride_y + (x - 4 + n)];
                filt_pix(samp, bs, alpha, beta, tc0, 0);
                for (n = 0; n < 8; n++) h->y[y * h->stride_y + (x - 4 + n)] = (uint8_t)samp[n];
              } else {
                x = qbx * 4 + k;
                y = qby * 4;
                if (y < 4 || y >= h->height) continue;
                for (n = 0; n < 8; n++) samp[n] = h->y[(y - 4 + n) * h->stride_y + x];
                filt_pix(samp, bs, alpha, beta, tc0, 0);
                for (n = 0; n < 8; n++) h->y[(y - 4 + n) * h->stride_y + x] = (uint8_t)samp[n];
              }
            }
          }
        }
      }
    }
  }
}
