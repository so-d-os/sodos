/* p384.c - ECDSA sobre secp384r1. Aritmetica em 12 membros de 32 bits
 * (o de indice 0 e o menos significativo). A reducao usa
 * 2^384 = 2^128 + 2^96 - 2^32 + 1 (mod p). Coordenadas jacobianas,
 * com a = -3. */
#include "p384.h"
#include <string.h>

typedef unsigned int u32;
typedef unsigned long long u64;

#define N 12

static const u32 P[N] = {
  0xffffffffu, 0x00000000u, 0x00000000u, 0xffffffffu,
  0xfffffffeu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
  0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu
};

/* ordem do grupo */
static const u32 OR[N] = {
  0xccc52973u, 0xecec196au, 0x48b0a77au, 0x581a0db2u,
  0xf4372ddfu, 0xc7634d81u, 0xffffffffu, 0xffffffffu,
  0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu
};

static const u32 B[N] = {
  0xd3ec2aefu, 0x2a85c8edu, 0x8a2ed19du, 0xc656398du,
  0x5013875au, 0x0314088fu, 0xfe814112u, 0x181d9c6eu,
  0xe3f82d19u, 0x988e056bu, 0xe23ee7e4u, 0xb3312fa7u
};

static const u32 GX[N] = {
  0x72760ab7u, 0x3a545e38u, 0xbf55296cu, 0x5502f25du,
  0x82542a38u, 0x59f741e0u, 0x8ba79b98u, 0x6e1d3b62u,
  0xf320ad74u, 0x8eb1c71eu, 0xbe8b0537u, 0xaa87ca22u
};

static const u32 GY[N] = {
  0x90ea0e5fu, 0x7a431d7cu, 0x1d7e819du, 0x0a60b1ceu,
  0xb5f0b8c0u, 0xe9da3113u, 0x289a147cu, 0xf8f41dbdu,
  0x9292dc29u, 0x5d9e98bfu, 0x96262c6fu, 0x3617de4au
};

static int cmp_n(const u32* a, const u32* b) {
  int i;
  for (i = N - 1; i >= 0; i--) {
    if (a[i] > b[i]) return 1;
    if (a[i] < b[i]) return -1;
  }
  return 0;
}

static int is0(const u32* a) {
  int i;
  for (i = 0; i < N; i++)
    if (a[i]) return 0;
  return 1;
}

static void set0(u32* a) {
  int i;
  for (i = 0; i < N; i++) a[i] = 0;
}

static void cpy(u32* d, const u32* s) {
  int i;
  for (i = 0; i < N; i++) d[i] = s[i];
}

static void from_be(u32* o, const uint8_t* p, int nb) {
  int i;
  set0(o);
  for (i = 0; i < nb && i < N * 4; i++) {
    int off = nb - 1 - i;
    o[off / 4] |= (u32)p[i] << ((off % 4) * 8);
  }
}

static void to_be(const u32* a, uint8_t* p) {
  int i;
  for (i = 0; i < N * 4; i++) {
    int off = N * 4 - 1 - i;
    p[i] = (uint8_t)(a[off / 4] >> ((off % 4) * 8));
  }
}

/* soma com vai-um; devolve o vai-um final */
static u32 add_raw(u32* o, const u32* a, const u32* b) {
  u64 c = 0;
  int i;
  for (i = 0; i < N; i++) {
    c += (u64)a[i] + b[i];
    o[i] = (u32)c;
    c >>= 32;
  }
  return (u32)c;
}

static u32 sub_raw(u32* o, const u32* a, const u32* b) {
  u64 br = 0;
  int i;
  for (i = 0; i < N; i++) {
    u64 d = (u64)a[i] - b[i] - br;
    o[i] = (u32)d;
    br = (d >> 63) ? 1 : 0;
  }
  return (u32)br;
}

/* 2^384 - p, em 12 membros */
static const u32 CRED[N] = {
  0x00000001u, 0xffffffffu, 0xffffffffu, 0x00000000u,
  0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u,
  0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u
};

static void fe_norm(u32* o, u32 carry) {
  int guard = 0;
  if (carry) carry = add_raw(o, o, CRED);
  while ((carry || cmp_n(o, P) >= 0) && guard < 4) {
    if (carry) carry = add_raw(o, o, CRED);
    else sub_raw(o, o, P);
    guard++;
  }
}

static void fe_add(u32* o, const u32* a, const u32* b) {
  u32 t[N];
  u32 c = add_raw(t, a, b);
  fe_norm(t, c);
  cpy(o, t);
}

static void fe_sub(u32* o, const u32* a, const u32* b) {
  u32 t[N];
  if (sub_raw(t, a, b)) add_raw(t, t, P);
  cpy(o, t);
}

static void fe_mul(u32* o, const u32* a, const u32* b) {
  u32 acc[24];
  int i, j, pass;
  memset(acc, 0, sizeof acc);
  for (i = 0; i < N; i++) {
    u64 c = 0;
    if (!a[i]) continue;
    for (j = 0; j < N; j++) {
      c += (u64)a[i] * b[j] + acc[i + j];
      acc[i + j] = (u32)c;
      c >>= 32;
    }
    acc[i + N] = (u32)c;
  }
  /* z_hi * (2^128 + 2^96 - 2^32 + 1) somado a z_lo, duas vezes */
  for (pass = 0; pass < 3; pass++) {
    u32 hi[N];
    u32 wide[28];
    u64 c;
    int k;
    for (i = 0; i < N; i++) hi[i] = acc[N + i];
    if (is0(hi) && acc[N + 0] == 0) {
      int z = 1;
      for (i = N; i < 24; i++)
        if (acc[i]) z = 0;
      if (z) break;
    }
    memset(wide, 0, sizeof wide);
    for (i = 0; i < N; i++) wide[i] = acc[i];
    /* + hi, + hi<<96 (3), + hi<<128 (4), - hi<<32 (1) */
    {
      static const int sh[3] = { 0, 3, 4 };
      for (k = 0; k < 3; k++) {
        c = 0;
        for (i = 0; i < N; i++) {
          c += (u64)wide[i + sh[k]] + hi[i];
          wide[i + sh[k]] = (u32)c;
          c >>= 32;
        }
        for (i = N + sh[k]; c && i < 28; i++) {
          c += wide[i];
          wide[i] = (u32)c;
          c >>= 32;
        }
      }
      c = 0;
      for (i = 0; i < N; i++) {
        u64 d = (u64)wide[i + 1] - hi[i] - c;
        wide[i + 1] = (u32)d;
        c = (d >> 63) ? 1 : 0;
      }
      for (i = N + 1; c && i < 28; i++) {
        u64 d = (u64)wide[i] - c;
        wide[i] = (u32)d;
        c = (d >> 63) ? 1 : 0;
      }
    }
    memcpy(acc, wide, 24 * 4);
  }
  fe_norm(acc, 0);
  cpy(o, acc);
}

static void fe_sqr(u32* o, const u32* a) { fe_mul(o, a, a); }

/* inverso por Fermat: a^(p-2) */
static void fe_inv(u32* o, const u32* a) {
  u32 b[N], r[N];
  int i, top;
  cpy(b, a);
  set0(r);
  r[0] = 1;
  /* p - 2: p termina em ...ffffffff, menos 2 = ...fffffffd no membro 0 */
  top = 383;
  for (i = top; i >= 0; i--) {
    int on;
    fe_sqr(r, r);
    if (i >= 32) on = (P[i / 32] >> (i % 32)) & 1;
    else if (i == 0) on = 1;          /* (p-2) bit 0 = 1 */
    else if (i == 1) on = 0;          /* (p-2) bit 1 = 0 */
    else on = (P[0] >> i) & 1;
    if (on) fe_mul(r, r, b);
  }
  cpy(o, r);
}

/* ---------- modulo a ordem ---------- */
static void n_add(u32* o, const u32* a, const u32* b) {
  u32 t[N];
  if (add_raw(t, a, b) || cmp_n(t, OR) >= 0) sub_raw(t, t, OR);
  cpy(o, t);
}

static void n_sub(u32* o, const u32* a, const u32* b) {
  u32 t[N];
  if (sub_raw(t, a, b)) add_raw(t, t, OR);
  cpy(o, t);
}

static void n_mod_wide(u32* o, const u32 w[24]) {
  u32 acc[N + 1];
  int i, k;
  memset(acc, 0, sizeof acc);
  for (i = 24 * 32 - 1; i >= 0; i--) {
    u32 carry = 0;
    int bit = (w[i / 32] >> (i % 32)) & 1;
    for (k = 0; k < N + 1; k++) {
      u32 n = (acc[k] << 1) | carry;
      carry = acc[k] >> 31;
      acc[k] = n;
    }
    acc[0] |= (u32)bit;
    if (acc[N] || cmp_n(acc, OR) >= 0) {
      u32 br = sub_raw(acc, acc, OR);
      acc[N] -= br;
    }
  }
  cpy(o, acc);
}

static void n_mul(u32* o, const u32* a, const u32* b) {
  u32 w[24];
  int i, j;
  memset(w, 0, sizeof w);
  for (i = 0; i < N; i++) {
    u64 c = 0;
    if (!a[i]) continue;
    for (j = 0; j < N; j++) {
      c += (u64)a[i] * b[j] + w[i + j];
      w[i + j] = (u32)c;
      c >>= 32;
    }
    w[i + N] = (u32)c;
  }
  n_mod_wide(o, w);
}

/* inverso modulo a ordem, pelo algoritmo binario */
static void n_inv(u32* o, const u32* a) {
  u32 u[N], v[N], x1[N], x2[N];
  cpy(u, a);
  cpy(v, OR);
  set0(x1);
  x1[0] = 1;
  set0(x2);
  while (!is0(u)) {
    while ((u[0] & 1) == 0) {
      int i;
      u32 c = 0;
      for (i = 0; i < N - 1; i++) u[i] = (u[i] >> 1) | (u[i + 1] << 31);
      u[N - 1] >>= 1;
      if (x1[0] & 1) c = add_raw(x1, x1, OR);
      for (i = 0; i < N - 1; i++) x1[i] = (x1[i] >> 1) | (x1[i + 1] << 31);
      x1[N - 1] = (x1[N - 1] >> 1) | (c << 31);
    }
    while ((v[0] & 1) == 0) {
      int i;
      u32 c = 0;
      for (i = 0; i < N - 1; i++) v[i] = (v[i] >> 1) | (v[i + 1] << 31);
      v[N - 1] >>= 1;
      if (x2[0] & 1) c = add_raw(x2, x2, OR);
      for (i = 0; i < N - 1; i++) x2[i] = (x2[i] >> 1) | (x2[i + 1] << 31);
      x2[N - 1] = (x2[N - 1] >> 1) | (c << 31);
    }
    if (cmp_n(u, v) >= 0) {
      sub_raw(u, u, v);
      n_sub(x1, x1, x2);
    } else {
      sub_raw(v, v, u);
      n_sub(x2, x2, x1);
    }
  }
  cpy(o, x2);
}

/* ---------- pontos jacobianos ---------- */
typedef struct {
  u32 x[N], y[N], z[N];
  int inf;
} pt;

static void pt_inf(pt* p) {
  memset(p, 0, sizeof *p);
  p->inf = 1;
}

static void pt_affine(pt* p, const u32 x[N], const u32 y[N]) {
  memset(p, 0, sizeof *p);
  cpy(p->x, x);
  cpy(p->y, y);
  p->z[0] = 1;
}

static void pt_dbl(pt* o, const pt* a) {
  u32 delta[N], gamma[N], beta[N], alpha[N], t1[N], t2[N], x3[N], y3[N], z3[N];
  pt r;
  if (a->inf || is0(a->y)) {
    pt_inf(o);
    return;
  }
  fe_sqr(delta, a->z);
  fe_sqr(gamma, a->y);
  fe_mul(beta, a->x, gamma);
  fe_add(t1, a->x, delta);
  fe_sub(t2, a->x, delta);
  fe_mul(alpha, t1, t2);
  fe_add(t1, alpha, alpha);
  fe_add(alpha, t1, alpha);          /* 3*(X-delta)*(X+delta) */
  fe_sqr(x3, alpha);
  fe_add(t1, beta, beta);
  fe_add(t1, t1, t1);
  fe_add(t1, t1, t1);                /* 8*beta */
  fe_sub(x3, x3, t1);
  fe_add(t1, a->y, a->z);
  fe_sqr(z3, t1);
  fe_sub(z3, z3, gamma);
  fe_sub(z3, z3, delta);
  fe_add(t1, beta, beta);
  fe_add(t1, t1, t1);                /* 4*beta */
  fe_sub(t1, t1, x3);
  fe_mul(y3, alpha, t1);
  fe_sqr(t2, gamma);
  fe_add(t2, t2, t2);
  fe_add(t2, t2, t2);
  fe_add(t2, t2, t2);                /* 8*gamma^2 */
  fe_sub(y3, y3, t2);
  memset(&r, 0, sizeof r);
  cpy(r.x, x3);
  cpy(r.y, y3);
  cpy(r.z, z3);
  *o = r;
}

static void pt_add(pt* o, const pt* a, const pt* b) {
  u32 z1z1[N], z2z2[N], u1[N], u2[N], s1[N], s2[N], h[N], hh[N], i[N], j[N];
  u32 rr[N], v[N], t1[N], t2[N], x3[N], y3[N], z3[N];
  pt r;
  if (a->inf) { *o = *b; return; }
  if (b->inf) { *o = *a; return; }
  fe_sqr(z1z1, a->z);
  fe_sqr(z2z2, b->z);
  fe_mul(u1, a->x, z2z2);
  fe_mul(u2, b->x, z1z1);
  fe_mul(s1, b->z, z2z2);
  fe_mul(s1, s1, a->y);              /* Y1*Z2^3 */
  fe_mul(s2, a->z, z1z1);
  fe_mul(s2, s2, b->y);              /* Y2*Z1^3 */
  fe_sub(h, u2, u1);
  fe_sub(rr, s2, s1);
  if (is0(h)) {
    if (is0(rr)) pt_dbl(o, a);
    else pt_inf(o);
    return;
  }
  fe_add(rr, rr, rr);                /* r = 2*(S2-S1) */
  fe_sqr(hh, h);
  fe_add(i, hh, hh);
  fe_add(i, i, i);                   /* 4*HH */
  fe_mul(j, h, i);
  fe_mul(v, u1, i);
  fe_sqr(x3, rr);
  fe_sub(x3, x3, j);
  fe_sub(x3, x3, v);
  fe_sub(x3, x3, v);
  fe_sub(t1, v, x3);
  fe_mul(y3, rr, t1);
  fe_mul(t2, s1, j);
  fe_add(t2, t2, t2);
  fe_sub(y3, y3, t2);
  fe_add(t1, a->z, b->z);
  fe_sqr(z3, t1);
  fe_sub(z3, z3, z1z1);
  fe_sub(z3, z3, z2z2);
  fe_mul(z3, z3, h);
  memset(&r, 0, sizeof r);
  cpy(r.x, x3);
  cpy(r.y, y3);
  cpy(r.z, z3);
  *o = r;
}

static int bit_of(const u32* a, int i) {
  return (a[i / 32] >> (i % 32)) & 1;
}

static void pt_mul(pt* o, const u32 k[N], const pt* p) {
  pt r, base;
  int i, top;
  pt_inf(&r);
  base = *p;
  top = -1;
  for (i = 383; i >= 0; i--)
    if (bit_of(k, i)) { top = i; break; }
  for (i = top; i >= 0; i--) {
    pt_dbl(&r, &r);
    if (bit_of(k, i)) pt_add(&r, &r, &base);
  }
  *o = r;
}

static int pt_eq_affine(const pt* a, const u32 x[N], const u32 y[N]) {
  u32 z2[N], z3[N], ax[N], ay[N];
  if (a->inf) return 0;
  fe_sqr(z2, a->z);
  fe_mul(z3, z2, a->z);
  fe_mul(ax, x, z2);
  fe_mul(ay, y, z3);
  return cmp_n(ax, a->x) == 0 && cmp_n(ay, a->y) == 0;
}

static void pt_x_mod_n(u32* o, const pt* a) {
  u32 z2[N], zinv[N], x[N], w[24];
  int i;
  fe_sqr(z2, a->z);
  fe_inv(zinv, z2);
  fe_mul(x, a->x, zinv);             /* X/Z^2 */
  /* x mod n: x < p < 2n, no maximo uma subtracao, mas p pode passar de n */
  memset(w, 0, sizeof w);
  for (i = 0; i < N; i++) w[i] = x[i];
  n_mod_wide(o, w);
}

static int on_curve(const u32 x[N], const u32 y[N]) {
  u32 y2[N], x2[N], x3[N], t[N];
  fe_sqr(y2, y);
  fe_sqr(x2, x);
  fe_mul(x3, x2, x);
  fe_add(t, x, x);
  fe_add(t, t, x);                   /* 3x */
  fe_sub(x3, x3, t);                 /* x^3 - 3x */
  fe_add(x3, x3, B);
  return cmp_n(y2, x3) == 0;
}

int p384_ecdsa_verify(const uint8_t sig[96], const uint8_t pub[96],
                      const uint8_t* hash, size_t hlen) {
  u32 r[N], s[N], e[N], w[N], u1[N], u2[N], rx[N];
  uint8_t eb[48];
  pt G, Q, R1, R2, R;
  if (!sig || !pub || !hash || hlen < 1 || hlen > 64) return -1;
  from_be(r, sig, 48);
  from_be(s, sig + 48, 48);
  if (is0(r) || cmp_n(r, OR) >= 0 || is0(s) || cmp_n(s, OR) >= 0) return -1;
  from_be(Q.x, pub, 48);
  from_be(Q.y, pub + 48, 48);
  memset(Q.z, 0, sizeof Q.z);
  Q.z[0] = 1;
  Q.inf = 0;
  if (!on_curve(Q.x, Q.y)) return -1;
  memset(eb, 0, sizeof eb);
  if (hlen >= 48) memcpy(eb, hash, 48);
  else memcpy(eb + (48 - hlen), hash, hlen);
  from_be(e, eb, 48);
  if (cmp_n(e, OR) >= 0) sub_raw(e, e, OR);
  n_inv(w, s);
  n_mul(u1, e, w);
  n_mul(u2, r, w);
  pt_affine(&G, GX, GY);
  pt_mul(&R1, u1, &G);
  pt_mul(&R2, u2, &Q);
  pt_add(&R, &R1, &R2);
  if (R.inf) return -1;
  pt_x_mod_n(rx, &R);
  return cmp_n(rx, r) == 0 ? 0 : -1;
}
