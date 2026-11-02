/* llama.c - LLAMA.EXE: roda um LLM local no formato GGUF (Hugging Face).
 *
 * Arquiteturas llama (SmolLM, TinyLlama...) e qwen2. Pesos F32, F16, Q8_0 e
 * Q4_0. Tokenizador BPE do GPT-2 (tokenizer.ggml.model = gpt2) com os
 * pre-tokenizadores do GPT-2/SmolLM e do Qwen2/Llama 3. Um nucleo, SSE2.
 *
 *   llama MODELO.GGU                 chat (ChatML)
 *   llama MODELO.GGU -p "pergunta"   uma resposta e sai
 * opcoes: -n N   maximo de tokens por resposta (padrao 256)
 *         -t T   temperatura (padrao 0.3; 0 = sempre o mais provavel)
 *         -c N   contexto em tokens (padrao: o que couber, ate 2048)
 *         -s N   semente
 *         -r R   penalidade de repeticao (padrao 1.1; 1 = desligada)
 *         -sys "texto"  mensagem de sistema
 *         -raw   -p sem template de chat (continua o texto)
 *         -tok "texto"  so mostra os ids dos tokens
 *         -utf8  terminal em UTF-8 (soxrun no Linux); sem ela, CP850
 *
 * O arquivo inteiro fica na memoria e o heap do SODOS vai ate 256 MB: cabem
 * modelos de ~200 MB, como o SmolLM2-135M-Instruct Q8_0 (145 MB).
 * Compilado com gcc -mx32 -msse2: o cc do SODOS nao tem ponto flutuante e o
 * kernel liga o SSE em kernel_entry.asm.
 */
#include <so.h>
#include <string.h>

typedef unsigned char u8;
typedef signed char i8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef char v16qi __attribute__((vector_size(16)));
typedef short v8hi __attribute__((vector_size(16)));
typedef unsigned short v8hu __attribute__((vector_size(16)));
typedef int v4si __attribute__((vector_size(16)));
typedef float v4sf __attribute__((vector_size(16)));

#define CPY __builtin_memcpy

/* ---------------- saida ---------------- */
static void out(const char* s) { k_write(s, (int)strlen(s)); }
static void outn(const char* s, int n) { k_write(s, n); }

static void out_int(long long v) {
  char b[24];
  int i = 23;
  int neg = v < 0;
  unsigned long long u = neg ? (unsigned long long)-v : (unsigned long long)v;
  b[i] = 0;
  do {
    b[--i] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  if (neg) b[--i] = '-';
  out(b + i);
}

/* uma casa decimal */
static void out_f1(double v) {
  long long t;
  if (v < 0) {
    k_putc('-');
    v = -v;
  }
  t = (long long)(v * 10.0 + 0.5);
  out_int(t / 10);
  k_putc('.');
  k_putc((int)('0' + t % 10));
}

static void die(const char* msg) {
  out("llama: ");
  out(msg);
  out("\n");
  k_exit(1);
}

static void* xmalloc(u32 n) {
  void* p = k_malloc(n ? n : 1);
  if (!p) die("sem memoria");
  return p;
}

/* Relogio: k_ms so avanca se for consultado a cada ~1 ms (PIT por sondagem),
 * entao medimos com o TSC, calibrado uma vez contra o k_ms. 0 = sem relogio. */
static double tsc_per_ms;

static u64 tsc(void) {
  unsigned hi = 0, lo = k_rdtsc(&hi);
  return ((u64)hi << 32) | lo;
}

static void clock_init(void) {
  unsigned m0 = k_ms(), m = m0, spins = 0;
  u64 t0;
  while (m == m0 && ++spins < 20000000u) m = k_ms();
  if (m == m0) return;
  t0 = tsc();
  while (k_ms() - m < 50) {
  }
  tsc_per_ms = (double)(tsc() - t0) / 50.0;
}

static unsigned now_ms(void) { return tsc_per_ms > 0 ? (unsigned)((double)tsc() / tsc_per_ms) : 0; }

/* ---------------- matematica (sem libm) ---------------- */
static double m_floor(double x) {
  double d = (double)(long long)x;
  return d > x ? d - 1.0 : d;
}

static double m_exp(double x) {
  union { double d; u64 u; } s;
  double k, r, p;
  if (x > 700.0) x = 700.0;
  if (x < -700.0) return 0.0;
  k = m_floor(x * 1.4426950408889634 + 0.5);
  r = x - k * 6.93147180369123816490e-01 - k * 1.90821492927058770002e-10;
  p = 1.0 + r * (1.0 + r * (1.0 / 2 + r * (1.0 / 6 + r * (1.0 / 24 + r * (1.0 / 120 +
      r * (1.0 / 720 + r * (1.0 / 5040 + r * (1.0 / 40320 + r * (1.0 / 362880 +
      r * (1.0 / 3628800 + r * (1.0 / 39916800)))))))))));
  s.u = (u64)((long long)k + 1023) << 52;
  return p * s.d;
}

static double m_log(double x) {
  union { double d; u64 u; } v;
  double m, s, s2, t;
  int e, k;
  if (x <= 0) return -1e300;
  v.d = x;
  e = (int)((v.u >> 52) & 0x7FF) - 1023;
  v.u = (v.u & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;
  m = v.d;
  if (m > 1.4142135623730951) {
    m *= 0.5;
    e++;
  }
  s = (m - 1.0) / (m + 1.0);
  s2 = s * s;
  t = 0;
  for (k = 21; k >= 1; k -= 2) t = t * s2 + 1.0 / k;
  return 2.0 * s * t + e * 0.6931471805599453;
}

static void m_sincos(double x, double* sn, double* cs) {
  double n = m_floor(x * 0.6366197723675814 + 0.5);
  double r = x - n * 1.57079632673412561417e+00 - n * 6.07710050650619224932e-11;
  double r2 = r * r, s, c;
  int q = (int)((long long)n & 3);
  s = r * (1 - r2 / 6 * (1 - r2 / 20 * (1 - r2 / 42 * (1 - r2 / 72 * (1 - r2 / 110 *
      (1 - r2 / 156 * (1 - r2 / 210)))))));
  c = 1 - r2 / 2 * (1 - r2 / 12 * (1 - r2 / 30 * (1 - r2 / 56 * (1 - r2 / 90 *
      (1 - r2 / 132 * (1 - r2 / 182 * (1 - r2 / 240)))))));
  if (q == 0) { *sn = s; *cs = c; }
  else if (q == 1) { *sn = c; *cs = -s; }
  else if (q == 2) { *sn = -s; *cs = -c; }
  else { *sn = -c; *cs = s; }
}

static float f16tab[65536];

static float h2f(u16 h) {
  union { u32 u; float f; } v;
  u32 sign = (u32)(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 0x3FF;
  if (e == 0) {
    if (!m) v.u = sign;
    else {
      e = 113;
      while (!(m & 0x400)) {
        m <<= 1;
        e--;
      }
      v.u = sign | (e << 23) | ((m & 0x3FF) << 13);
    }
  } else if (e == 31) v.u = sign | 0x7F800000u | (m << 13);
  else v.u = sign | ((e + 112) << 23) | (m << 13);
  return v.f;
}

/* ---------------- GGUF ---------------- */
enum { T_F32 = 0, T_F16 = 1, T_Q4_0 = 2, T_Q8_0 = 8 };

struct str { const char* s; u32 n; };

struct rd { const u8* p; const u8* end; };

static void need(struct rd* r, u64 n) {
  if (n > (u64)(r->end - r->p)) die("GGUF truncado ou invalido");
}
static u32 rd_u32(struct rd* r) { u32 v; need(r, 4); CPY(&v, r->p, 4); r->p += 4; return v; }
static u64 rd_u64(struct rd* r) { u64 v; need(r, 8); CPY(&v, r->p, 8); r->p += 8; return v; }
static struct str rd_str(struct rd* r) {
  struct str s;
  u64 n = rd_u64(r);
  need(r, n);
  s.s = (const char*)r->p;
  s.n = (u32)n;
  r->p += n;
  return s;
}

static long long rd_ival(struct rd* r, u32 t) {
  u8 b[8];
  int sz = (t == 0 || t == 1 || t == 7) ? 1 : (t == 2 || t == 3) ? 2 : (t == 4 || t == 5) ? 4 : 8;
  if (t == 6 || t == 8 || t == 9 || t == 12 || t > 12) die("GGUF: esperava inteiro");
  need(r, sz);
  CPY(b, r->p, sz);
  r->p += sz;
  switch (t) {
  case 0: case 7: return b[0];
  case 1: return (i8)b[0];
  case 2: return *(u16*)b;
  case 3: return *(short*)b;
  case 4: return *(u32*)b;
  case 5: return *(int*)b;
  case 10: return (long long)*(u64*)b;
  default: return *(long long*)b;
  }
}

static double rd_fval(struct rd* r, u32 t) {
  if (t == 6) { float f; need(r, 4); CPY(&f, r->p, 4); r->p += 4; return f; }
  if (t == 12) { double d; need(r, 8); CPY(&d, r->p, 8); r->p += 8; return d; }
  return (double)rd_ival(r, t);
}

static void skip_val(struct rd* r, u32 t) {
  static const u8 fixed[13] = { 1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8 };
  if (t > 12) die("GGUF: tipo de valor desconhecido");
  if (t == 8) rd_str(r);
  else if (t == 9) {
    u32 at = rd_u32(r);
    u64 n = rd_u64(r), i;
    if (at > 12) die("GGUF: tipo de array desconhecido");
    if (at != 8 && at != 9) {
      need(r, n * fixed[at]);
      r->p += n * fixed[at];
    } else
      for (i = 0; i < n; i++) skip_val(r, at);
  } else {
    need(r, fixed[t]);
    r->p += fixed[t];
  }
}

static int seq(struct str s, const char* c) {
  u32 n = (u32)strlen(c);
  return s.n == n && memcmp(s.s, c, n) == 0;
}

struct tinfo { struct str name; u32 type, nd; u64 ne[4]; u64 off; };

struct w { const u8* d; int type, cols, rows; };

struct layer {
  struct w attn_norm, wq, wk, wv, wo, bq, bk, bv, ffn_norm, gate, up, down;
};

/* modelo */
static u8* g_file;
static u32 g_fsize;
static struct str g_arch, g_name, g_tokmodel, g_pre, g_tmpl;
static int n_layer, n_embd, n_ff, n_head, n_kv, head_dim, rot_dim, n_ctx_train, n_vocab;
static int is_qwen2;
static double rope_base = 10000.0, rms_eps = 1e-5;
static u32 g_align = 32;
static struct tinfo* g_tens;
static u32 g_ntens;
static const u8* g_data;
static struct layer* L;
static struct w w_embd, w_onorm, w_out;

/* tokenizador */
static const char** tok_s;
static u32* tok_n;
static u8* tok_t;
static int bos_id = -1, eos_id = -1, eot_id = -1, add_bos;
static const u8* merges_p;
static u32 n_merges;
static const u8* types_p;
static u32 types_at;

static int tsize(int type, int cols, u32* bytes) {
  if (type == T_F32) *bytes = (u32)cols * 4;
  else if (type == T_F16) *bytes = (u32)cols * 2;
  else if (type == T_Q8_0 && cols % 32 == 0) *bytes = (u32)cols / 32 * 34;
  else if (type == T_Q4_0 && cols % 32 == 0) *bytes = (u32)cols / 32 * 18;
  else return -1;
  return 0;
}

static void lname(char* b, int l, const char* suf) {
  char t[12];
  int i = 0, n = 0;
  CPY(b, "blk.", 4);
  n = 4;
  do {
    t[i++] = (char)('0' + l % 10);
    l /= 10;
  } while (l);
  while (i) b[n++] = t[--i];
  b[n++] = '.';
  strcpy(b + n, suf);
}

static int get_w(struct w* w, const char* name, int cols, int rows, int required) {
  u32 i, rb = 0;
  for (i = 0; i < g_ntens; i++) {
    struct tinfo* t = &g_tens[i];
    if (!seq(t->name, name)) continue;
    if (t->ne[0] != (u64)cols || (t->nd > 1 ? t->ne[1] : 1) != (u64)rows) {
      out("llama: tensor ");
      out(name);
      die(" com dimensoes inesperadas");
    }
    if (tsize((int)t->type, cols, &rb) < 0) {
      out("llama: tensor ");
      out(name);
      out(" tipo ");
      out_int(t->type);
      die(" nao suportado (use F32, F16, Q8_0 ou Q4_0)");
    }
    if (t->off + (u64)rb * rows > (u64)(g_file + g_fsize - g_data)) die("tensor fora do arquivo");
    w->d = g_data + t->off;
    w->type = (int)t->type;
    w->cols = cols;
    w->rows = rows;
    return 1;
  }
  if (required) {
    out("llama: falta o tensor ");
    die(name);
  }
  w->d = 0;
  return 0;
}

static void load_file(const char* path) {
  int sz = k_fsize(path);
  u32 got = 0, step = 1u << 20, mark = 0;
  unsigned t0 = now_ms();
  if (sz <= 0) {
    out("llama: nao achei ");
    die(path);
  }
  g_fsize = (u32)sz;
  g_file = (u8*)k_malloc(g_fsize);
  if (!g_file) {
    unsigned tot = 0, fr = 0;
    k_meminfo(&tot, &fr);
    out("llama: o modelo tem ");
    out_int(g_fsize >> 20);
    out(" MB e so ha ");
    out_int(fr >> 20);
    die(" MB livres");
  }
  out("carregando ");
  out(path);
  out(" (");
  out_int(g_fsize >> 20);
  out(" MB) ");
  while (got < g_fsize) {
    int n = (int)(g_fsize - got < step ? g_fsize - got : step);
    int r = k_fread(path, got, (char*)g_file + got, n);
    if (r <= 0) die("erro de leitura");
    got += (u32)r;
    if (got - mark >= (16u << 20)) {
      k_putc('.');
      mark = got;
    }
  }
  out(" ");
  out_f1((now_ms() - t0) / 1000.0);
  out(" s\n");
}

static void parse_gguf(void) {
  struct rd r;
  u32 ver, i;
  u64 nt, nkv;
  r.p = g_file;
  r.end = g_file + g_fsize;
  if (rd_u32(&r) != 0x46554747u) die("nao e um arquivo GGUF");
  ver = rd_u32(&r);
  if (ver < 2 || ver > 3) die("versao de GGUF nao suportada");
  nt = rd_u64(&r);
  nkv = rd_u64(&r);
  for (i = 0; i < nkv; i++) {
    struct str k = rd_str(&r);
    u32 t = rd_u32(&r);
    struct str sfx;
    if (seq(k, "general.architecture")) g_arch = rd_str(&r);
    else if (seq(k, "general.name") && t == 8) g_name = rd_str(&r);
    else if (seq(k, "general.alignment")) g_align = (u32)rd_ival(&r, t);
    else if (seq(k, "tokenizer.ggml.model")) g_tokmodel = rd_str(&r);
    else if (seq(k, "tokenizer.ggml.pre")) g_pre = rd_str(&r);
    else if (seq(k, "tokenizer.chat_template")) g_tmpl = rd_str(&r);
    else if (seq(k, "tokenizer.ggml.bos_token_id")) bos_id = (int)rd_ival(&r, t);
    else if (seq(k, "tokenizer.ggml.eos_token_id")) eos_id = (int)rd_ival(&r, t);
    else if (seq(k, "tokenizer.ggml.eot_token_id")) eot_id = (int)rd_ival(&r, t);
    else if (seq(k, "tokenizer.ggml.add_bos_token")) add_bos = (int)rd_ival(&r, t);
    else if (seq(k, "tokenizer.ggml.tokens") || seq(k, "tokenizer.ggml.merges") ||
             seq(k, "tokenizer.ggml.token_type")) {
      const u8* p;
      u32 at;
      u64 n;
      if (t != 9) die("GGUF: tokenizador invalido");
      p = r.p;
      at = rd_u32(&r);
      n = rd_u64(&r);
      if (seq(k, "tokenizer.ggml.tokens")) {
        if (at != 8) die("GGUF: tokens invalidos");
        n_vocab = (int)n;
        tok_s = (const char**)xmalloc((u32)n * sizeof(char*));
        tok_n = (u32*)xmalloc((u32)n * 4);
        tok_t = (u8*)xmalloc((u32)n);
        {
          u32 j;
          for (j = 0; j < (u32)n; j++) {
            struct str s = rd_str(&r);
            tok_s[j] = s.s;
            tok_n[j] = s.n;
          }
        }
        continue;
      }
      if (seq(k, "tokenizer.ggml.merges")) {
        merges_p = r.p;
        n_merges = (u32)n;
      } else {
        types_p = r.p;
        types_at = at;
      }
      r.p = p;
      skip_val(&r, t);
    } else {
      /* "<arch>.chave" */
      u32 dot = 0;
      while (dot < k.n && k.s[dot] != '.') dot++;
      sfx.s = k.s + dot + 1;
      sfx.n = dot < k.n ? k.n - dot - 1 : 0;
      if (dot >= k.n || memcmp(k.s, "general", 7) == 0 || memcmp(k.s, "tokenizer", 9) == 0)
        skip_val(&r, t);
      else if (seq(sfx, "block_count")) n_layer = (int)rd_ival(&r, t);
      else if (seq(sfx, "context_length")) n_ctx_train = (int)rd_ival(&r, t);
      else if (seq(sfx, "embedding_length")) n_embd = (int)rd_ival(&r, t);
      else if (seq(sfx, "feed_forward_length") && t != 9) n_ff = (int)rd_ival(&r, t);
      else if (seq(sfx, "attention.head_count") && t != 9) n_head = (int)rd_ival(&r, t);
      else if (seq(sfx, "attention.head_count_kv") && t != 9) n_kv = (int)rd_ival(&r, t);
      else if (seq(sfx, "rope.dimension_count")) rot_dim = (int)rd_ival(&r, t);
      else if (seq(sfx, "rope.freq_base")) rope_base = rd_fval(&r, t);
      else if (seq(sfx, "attention.layer_norm_rms_epsilon")) rms_eps = rd_fval(&r, t);
      else skip_val(&r, t);
    }
  }
  if (nt > 100000) die("GGUF: tensores demais");
  g_ntens = (u32)nt;
  g_tens = (struct tinfo*)xmalloc(g_ntens * sizeof(struct tinfo));
  for (i = 0; i < g_ntens; i++) {
    struct tinfo* t = &g_tens[i];
    u32 d;
    t->name = rd_str(&r);
    t->nd = rd_u32(&r);
    if (t->nd < 1 || t->nd > 4) die("GGUF: tensor invalido");
    for (d = 0; d < 4; d++) t->ne[d] = d < t->nd ? rd_u64(&r) : 1;
    t->type = rd_u32(&r);
    t->off = rd_u64(&r);
  }
  if (!g_align || (g_align & (g_align - 1))) g_align = 32;
  {
    u32 pos = (u32)(r.p - g_file);
    pos = (pos + g_align - 1) & ~(g_align - 1);
    if (pos > g_fsize) die("GGUF truncado");
    g_data = g_file + pos;
  }
}

static void setup_model(void) {
  int l;
  char nm[64];
  if (seq(g_arch, "qwen2")) is_qwen2 = 1;
  else if (!seq(g_arch, "llama")) {
    out("llama: arquitetura '");
    outn(g_arch.s, (int)g_arch.n);
    die("' nao suportada (so llama e qwen2)");
  }
  if (!n_layer || !n_embd || !n_ff || !n_head || !n_vocab) die("GGUF sem os hiperparametros");
  if (!n_kv) n_kv = n_head;
  if (n_embd % n_head || n_head % n_kv) die("cabecas de atencao invalidas");
  head_dim = n_embd / n_head;
  if (!rot_dim || rot_dim > head_dim) rot_dim = head_dim;
  if (!seq(g_tokmodel, "gpt2")) {
    out("llama: tokenizador '");
    outn(g_tokmodel.s, (int)g_tokmodel.n);
    die("' nao suportado (so gpt2/BPE)");
  }
  get_w(&w_embd, "token_embd.weight", n_embd, n_vocab, 1);
  get_w(&w_onorm, "output_norm.weight", n_embd, 1, 1);
  if (!get_w(&w_out, "output.weight", n_embd, n_vocab, 0)) w_out = w_embd;
  if (w_onorm.type != T_F32) die("normas precisam ser F32");
  L = (struct layer*)xmalloc((u32)n_layer * sizeof(struct layer));
  for (l = 0; l < n_layer; l++) {
    struct layer* y = &L[l];
    int kvd = n_kv * head_dim;
    lname(nm, l, "attn_norm.weight"); get_w(&y->attn_norm, nm, n_embd, 1, 1);
    lname(nm, l, "attn_q.weight"); get_w(&y->wq, nm, n_embd, n_embd, 1);
    lname(nm, l, "attn_k.weight"); get_w(&y->wk, nm, n_embd, kvd, 1);
    lname(nm, l, "attn_v.weight"); get_w(&y->wv, nm, n_embd, kvd, 1);
    lname(nm, l, "attn_output.weight"); get_w(&y->wo, nm, n_embd, n_embd, 1);
    lname(nm, l, "attn_q.bias"); get_w(&y->bq, nm, n_embd, 1, 0);
    lname(nm, l, "attn_k.bias"); get_w(&y->bk, nm, kvd, 1, 0);
    lname(nm, l, "attn_v.bias"); get_w(&y->bv, nm, kvd, 1, 0);
    lname(nm, l, "ffn_norm.weight"); get_w(&y->ffn_norm, nm, n_embd, 1, 1);
    lname(nm, l, "ffn_gate.weight"); get_w(&y->gate, nm, n_embd, n_ff, 1);
    lname(nm, l, "ffn_up.weight"); get_w(&y->up, nm, n_embd, n_ff, 1);
    lname(nm, l, "ffn_down.weight"); get_w(&y->down, nm, n_ff, n_embd, 1);
    if (y->attn_norm.type != T_F32 || y->ffn_norm.type != T_F32 ||
        (y->bq.d && y->bq.type != T_F32) || (y->bk.d && y->bk.type != T_F32) ||
        (y->bv.d && y->bv.type != T_F32))
      die("normas e bias precisam ser F32");
  }
}

/* ---------------- matvec ---------------- */
static i8* g_xq;     /* ativacao quantizada Q8_0 */
static float* g_xd;  /* escala por bloco de 32 */

static void quantize_x(const float* x, int n) {
  int b, j;
  for (b = 0; b < n / 32; b++) {
    const float* xb = x + b * 32;
    float amax = 0, d, id;
    for (j = 0; j < 32; j++) {
      float a = xb[j] < 0 ? -xb[j] : xb[j];
      if (a > amax) amax = a;
    }
    d = amax / 127.0f;
    id = d > 0 ? 1.0f / d : 0.0f;
    g_xd[b] = d;
    for (j = 0; j < 32; j++) {
      float v = xb[j] * id;
      g_xq[b * 32 + j] = (i8)(int)(v + (v >= 0 ? 0.5f : -0.5f));
    }
  }
}

static inline v16qi ld16(const void* p) { return (v16qi)__builtin_ia32_loaddqu((const char*)p); }

/* soma dos 16 produtos int8*int8, em 4 int32 */
static inline v4si dot16(v16qi a, v16qi b) {
  v8hi al = __builtin_ia32_psrawi128((v8hi)__builtin_ia32_punpcklbw128(a, a), 8);
  v8hi ah = __builtin_ia32_psrawi128((v8hi)__builtin_ia32_punpckhbw128(a, a), 8);
  v8hi bl = __builtin_ia32_psrawi128((v8hi)__builtin_ia32_punpcklbw128(b, b), 8);
  v8hi bh = __builtin_ia32_psrawi128((v8hi)__builtin_ia32_punpckhbw128(b, b), 8);
  return __builtin_ia32_pmaddwd128(al, bl) + __builtin_ia32_pmaddwd128(ah, bh);
}

static float hsum(v4sf v) { return (v[0] + v[1]) + (v[2] + v[3]); }

static float dot_q8_0(const u8* row, int nb) {
  v4sf acc = { 0, 0, 0, 0 };
  const i8* xq = g_xq;
  int b;
  for (b = 0; b < nb; b++, row += 34, xq += 32) {
    u16 dh;
    v4si p;
    CPY(&dh, row, 2);
    p = dot16(ld16(row + 2), ld16(xq)) + dot16(ld16(row + 18), ld16(xq + 16));
    acc += __builtin_convertvector(p, v4sf) * (f16tab[dh] * g_xd[b]);
  }
  return hsum(acc);
}

static float dot_q4_0(const u8* row, int nb) {
  const v16qi m4 = { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15 };
  const v16qi e8 = { 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8 };
  v4sf acc = { 0, 0, 0, 0 };
  const i8* xq = g_xq;
  int b;
  for (b = 0; b < nb; b++, row += 18, xq += 32) {
    u16 dh;
    v16qi q = ld16(row + 2), lo, hi;
    v4si p;
    CPY(&dh, row, 2);
    lo = (q & m4) - e8;
    hi = ((v16qi)((v8hu)q >> 4) & m4) - e8;
    p = dot16(lo, ld16(xq)) + dot16(hi, ld16(xq + 16));
    acc += __builtin_convertvector(p, v4sf) * (f16tab[dh] * g_xd[b]);
  }
  return hsum(acc);
}

static float dot_f32(const u8* row, const float* x, int n) {
  float s = 0, v;
  int i;
  for (i = 0; i < n; i++) {
    CPY(&v, row + i * 4, 4);
    s += v * x[i];
  }
  return s;
}

static float dot_f16(const u8* row, const float* x, int n) {
  float s = 0;
  int i;
  for (i = 0; i < n; i++) {
    u16 h;
    CPY(&h, row + i * 2, 2);
    s += f16tab[h] * x[i];
  }
  return s;
}

/* out[r] = W[r] . x */
static void matvec(float* o, const struct w* w, const float* x) {
  int r, nb = w->cols / 32;
  u32 rb = 0;
  const u8* row = w->d;
  tsize(w->type, w->cols, &rb);
  if (w->type == T_Q8_0 || w->type == T_Q4_0) quantize_x(x, w->cols);
  for (r = 0; r < w->rows; r++, row += rb) {
    if (w->type == T_Q8_0) o[r] = dot_q8_0(row, nb);
    else if (w->type == T_Q4_0) o[r] = dot_q4_0(row, nb);
    else if (w->type == T_F16) o[r] = dot_f16(row, x, w->cols);
    else o[r] = dot_f32(row, x, w->cols);
  }
}

static void get_row(float* o, const struct w* w, int r) {
  u32 rb = 0;
  const u8* row;
  int i, j;
  tsize(w->type, w->cols, &rb);
  row = w->d + (u32)r * rb;
  if (w->type == T_F32) CPY(o, row, (u32)w->cols * 4);
  else if (w->type == T_F16) {
    for (i = 0; i < w->cols; i++) {
      u16 h;
      CPY(&h, row + i * 2, 2);
      o[i] = f16tab[h];
    }
  } else {
    int bs = w->type == T_Q8_0 ? 34 : 18;
    for (i = 0; i < w->cols / 32; i++, row += bs) {
      u16 dh;
      float d;
      CPY(&dh, row, 2);
      d = f16tab[dh];
      if (w->type == T_Q8_0)
        for (j = 0; j < 32; j++) o[i * 32 + j] = d * (float)((const i8*)row)[2 + j];
      else
        for (j = 0; j < 16; j++) {
          o[i * 32 + j] = d * (float)((row[2 + j] & 15) - 8);
          o[i * 32 + j + 16] = d * (float)((row[2 + j] >> 4) - 8);
        }
    }
  }
}

/* ---------------- transformer ---------------- */
static int n_ctx;
static float *x, *xb, *xb2, *hb, *hb2, *q, *kk, *vv, *att, *logits;
static float *kcache, *vcache;
static double* inv_freq;

static void rmsnorm(float* o, const float* in, const struct w* w, int n) {
  float ss = 0, sc, g;
  int i;
  for (i = 0; i < n; i++) ss += in[i] * in[i];
  sc = 1.0f / __builtin_sqrtf(ss / n + (float)rms_eps);
  for (i = 0; i < n; i++) {
    CPY(&g, w->d + i * 4, 4);
    o[i] = in[i] * sc * g;
  }
}

static void add_bias(float* v, const struct w* b) {
  int i;
  float f;
  if (!b->d) return;
  for (i = 0; i < b->cols; i++) {
    CPY(&f, b->d + i * 4, 4);
    v[i] += f;
  }
}

/* llama: pares vizinhos (2i, 2i+1); qwen2 (NEOX): (i, i + dim/2) */
static void rope(float* v, int nh, int pos) {
  int h, i, half = rot_dim / 2;
  static float cs[512], sn[512];
  for (i = 0; i < half; i++) {
    double s, c;
    m_sincos(pos * inv_freq[i], &s, &c);
    cs[i] = (float)c;
    sn[i] = (float)s;
  }
  for (h = 0; h < nh; h++) {
    float* p = v + h * head_dim;
    for (i = 0; i < half; i++) {
      int a = is_qwen2 ? i : 2 * i, b = is_qwen2 ? i + half : 2 * i + 1;
      float x0 = p[a], x1 = p[b];
      p[a] = x0 * cs[i] - x1 * sn[i];
      p[b] = x0 * sn[i] + x1 * cs[i];
    }
  }
}

static float* forward(int tok, int pos) {
  int l, h, t, i, kvd = n_kv * head_dim, grp = n_head / n_kv;
  float scale = 1.0f / __builtin_sqrtf((float)head_dim);
  get_row(x, &w_embd, tok);
  for (l = 0; l < n_layer; l++) {
    struct layer* y = &L[l];
    float* kc = kcache + ((u32)l * n_ctx) * kvd;
    float* vc = vcache + ((u32)l * n_ctx) * kvd;
    rmsnorm(xb, x, &y->attn_norm, n_embd);
    matvec(q, &y->wq, xb);
    matvec(kk, &y->wk, xb);
    matvec(vv, &y->wv, xb);
    add_bias(q, &y->bq);
    add_bias(kk, &y->bk);
    add_bias(vv, &y->bv);
    rope(q, n_head, pos);
    rope(kk, n_kv, pos);
    CPY(kc + (u32)pos * kvd, kk, (u32)kvd * 4);
    CPY(vc + (u32)pos * kvd, vv, (u32)kvd * 4);
    for (h = 0; h < n_head; h++) {
      const float* qh = q + h * head_dim;
      int off = (h / grp) * head_dim;
      float mx = -1e30f, sum = 0;
      float* o = xb2 + h * head_dim;
      for (t = 0; t <= pos; t++) {
        const float* kt = kc + (u32)t * kvd + off;
        float s = 0;
        for (i = 0; i < head_dim; i++) s += qh[i] * kt[i];
        s *= scale;
        att[t] = s;
        if (s > mx) mx = s;
      }
      for (t = 0; t <= pos; t++) {
        att[t] = (float)m_exp(att[t] - mx);
        sum += att[t];
      }
      for (i = 0; i < head_dim; i++) o[i] = 0;
      for (t = 0; t <= pos; t++) {
        const float* vt = vc + (u32)t * kvd + off;
        float a = att[t] / sum;
        for (i = 0; i < head_dim; i++) o[i] += a * vt[i];
      }
    }
    matvec(xb, &y->wo, xb2);
    for (i = 0; i < n_embd; i++) x[i] += xb[i];
    rmsnorm(xb, x, &y->ffn_norm, n_embd);
    matvec(hb, &y->gate, xb);
    matvec(hb2, &y->up, xb);
    for (i = 0; i < n_ff; i++) {
      float g = hb[i];
      hb[i] = g / (1.0f + (float)m_exp(-g)) * hb2[i];
    }
    matvec(xb, &y->down, hb);
    for (i = 0; i < n_embd; i++) x[i] += xb[i];
  }
  rmsnorm(x, x, &w_onorm, n_embd);
  matvec(logits, &w_out, x);
  return logits;
}

static void alloc_state(int want_ctx) {
  unsigned tot = 0, fr = 0;
  u32 kvd = (u32)(n_kv * head_dim), per, mx = (u32)(n_embd > n_ff ? n_embd : n_ff);
  int i, cap;
  x = (float*)xmalloc((u32)n_embd * 4);
  xb = (float*)xmalloc((u32)n_embd * 4);
  xb2 = (float*)xmalloc((u32)n_embd * 4);
  q = (float*)xmalloc((u32)n_embd * 4);
  kk = (float*)xmalloc(kvd * 4);
  vv = (float*)xmalloc(kvd * 4);
  hb = (float*)xmalloc((u32)n_ff * 4);
  hb2 = (float*)xmalloc((u32)n_ff * 4);
  logits = (float*)xmalloc((u32)n_vocab * 4);
  g_xq = (i8*)xmalloc(mx);
  g_xd = (float*)xmalloc(mx / 32 * 4 + 4);
  inv_freq = (double*)xmalloc((u32)rot_dim / 2 * 8 + 8);
  for (i = 0; i < rot_dim / 2; i++) inv_freq[i] = m_exp(-(2.0 * i / rot_dim) * m_log(rope_base));
  /* contexto: o pedido, ou o que couber na memoria livre (reserva 4 MB) */
  per = (u32)n_layer * kvd * 4 * 2 + 4;
  k_meminfo(&tot, &fr);
  cap = fr > (8u << 20) ? (int)((fr - (8u << 20)) / per) : 0;
  n_ctx = want_ctx > 0 ? want_ctx : 2048;
  if (n_ctx_train > 0 && n_ctx > n_ctx_train) n_ctx = n_ctx_train;
  if (want_ctx <= 0 && n_ctx > cap) n_ctx = cap;
  if (n_ctx < 16) die("sem memoria para o contexto");
  kcache = (float*)k_malloc((u32)n_ctx * n_layer * kvd * 4);
  vcache = (float*)k_malloc((u32)n_ctx * n_layer * kvd * 4);
  att = (float*)k_malloc((u32)n_ctx * 4);
  if (!kcache || !vcache || !att) die("sem memoria para o contexto (use -c menor)");
}

/* ---------------- tokenizador BPE (byte-level, GPT-2) ---------------- */
static int* vt_slot;
static u32 vt_mask;
static u64* mg_key;
static int *mg_rank, *mg_res;
static u32 mg_mask;
static int byte_tok[256];
static short u2b[512];
static int b2u[256];
static int pre_kind; /* 0 gpt2, 1 smollm (digitos sozinhos), 2 qwen2 (1 digito), 3 llama3 (3) */

static u32 hstr(const char* s, u32 n) {
  u32 h = 2166136261u;
  while (n--) h = (h ^ (u8)*s++) * 16777619u;
  return h;
}

static int vocab_find(const char* s, u32 n) {
  u32 h = hstr(s, n) & vt_mask;
  while (vt_slot[h] >= 0) {
    int id = vt_slot[h];
    if (tok_n[id] == n && memcmp(tok_s[id], s, n) == 0) return id;
    h = (h + 1) & vt_mask;
  }
  return -1;
}

static u32 hpair(u64 k) { return (u32)((k * 0x9E3779B97F4A7C15ull) >> 32); }

static int merge_find(int a, int b, int* res) {
  u64 k = ((u64)(u32)a << 32) | (u32)b;
  u32 h = hpair(k) & mg_mask;
  while (mg_rank[h] >= 0) {
    if (mg_key[h] == k) {
      *res = mg_res[h];
      return mg_rank[h];
    }
    h = (h + 1) & mg_mask;
  }
  return -1;
}

static int utf8_put(u32 c, char* o) {
  if (c < 0x80) { o[0] = (char)c; return 1; }
  if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
  if (c < 0x10000) {
    o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63));
    o[2] = (char)(0x80 | (c & 63));
    return 3;
  }
  o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63));
  o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63));
  return 4;
}

/* decodifica um caractere; invalido vira o proprio byte */
static u32 utf8_get(const u8* s, int n, int* len) {
  u32 c = s[0];
  int k = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1, i;
  if (k > n || c >= 0xF8 || (c >= 0x80 && c < 0xC0)) { *len = 1; return c; }
  if (k == 1) { *len = 1; return c; }
  c &= 0x7F >> k;
  for (i = 1; i < k; i++) {
    if ((s[i] & 0xC0) != 0x80) { *len = 1; return s[0]; }
    c = (c << 6) | (s[i] & 63);
  }
  *len = k;
  return c;
}

static void tok_init(void) {
  u32 i, sz = 1;
  int n = 0, b;
  char* buf;
  if (!n_vocab) die("GGUF sem vocabulario");
  while (sz < (u32)n_vocab * 2) sz <<= 1;
  vt_mask = sz - 1;
  vt_slot = (int*)xmalloc(sz * 4);
  for (i = 0; i < sz; i++) vt_slot[i] = -1;
  for (i = 0; i < (u32)n_vocab; i++) {
    u32 h = hstr(tok_s[i], tok_n[i]) & vt_mask;
    int dup = 0;
    while (vt_slot[h] >= 0) {
      int id = vt_slot[h];
      if (tok_n[id] == tok_n[i] && memcmp(tok_s[id], tok_s[i], tok_n[i]) == 0) { dup = 1; break; }
      h = (h + 1) & vt_mask;
    }
    if (!dup) vt_slot[h] = (int)i;
    tok_t[i] = 1;
    if (types_p && (types_at == 4 || types_at == 5)) {
      int tt;
      CPY(&tt, types_p + i * 4, 4);
      tok_t[i] = (u8)tt;
    }
  }
  /* bytes <-> caracteres do GPT-2 */
  for (i = 0; i < 512; i++) u2b[i] = -1;
  for (b = 0; b < 256; b++) {
    int cp;
    char o[4];
    if ((b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF)) cp = b;
    else cp = 256 + n++;
    b2u[b] = cp;
    u2b[cp] = (short)b;
    byte_tok[b] = vocab_find(o, (u32)utf8_put((u32)cp, o));
  }
  /* merges "A B" -> (rank, id de AB) */
  sz = 1;
  while (sz < n_merges * 2 + 2) sz <<= 1;
  mg_mask = sz - 1;
  mg_key = (u64*)xmalloc(sz * 8);
  mg_rank = (int*)xmalloc(sz * 4);
  mg_res = (int*)xmalloc(sz * 4);
  for (i = 0; i < sz; i++) mg_rank[i] = -1;
  buf = (char*)xmalloc(1024);
  if (merges_p) {
    struct rd r;
    r.p = merges_p;
    r.end = g_file + g_fsize;
    for (i = 0; i < n_merges; i++) {
      struct str s = rd_str(&r);
      u32 sp = 0;
      int a, c, ab;
      while (sp < s.n && s.s[sp] != ' ') sp++;
      if (sp == 0 || sp >= s.n || s.n > 1024) continue;
      a = vocab_find(s.s, sp);
      c = vocab_find(s.s + sp + 1, s.n - sp - 1);
      CPY(buf, s.s, sp);
      memcpy(buf + sp, s.s + sp + 1, s.n - sp - 1);
      ab = vocab_find(buf, s.n - 1);
      if (a < 0 || c < 0 || ab < 0) continue;
      {
        u64 k = ((u64)(u32)a << 32) | (u32)c;
        u32 h = hpair(k) & mg_mask;
        while (mg_rank[h] >= 0 && mg_key[h] != k) h = (h + 1) & mg_mask;
        if (mg_rank[h] >= 0) continue;
        mg_key[h] = k;
        mg_rank[h] = (int)i;
        mg_res[h] = ab;
      }
    }
  }
  k_free(buf);
  if (seq(g_pre, "smollm")) pre_kind = 1;
  else if (seq(g_pre, "qwen2")) pre_kind = 2;
  else if (seq(g_pre, "llama3") || seq(g_pre, "llama-bpe")) pre_kind = 3;
}

/* buffers do pre-tokenizador */
static u32* t_cp;
static int* t_off;
static u8* t_fl;
static int* t_sym;
static const u8* t_txt;
static int* t_out;
static int t_nout, t_cap;
enum { F_IN = 1, F_WS = 2, F_L = 4, F_N = 8 };

static int cp_flags(u32 c) {
  if (c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
      (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
      c == 0x205F || c == 0x3000)
    return F_IN | F_WS;
  if (c >= '0' && c <= '9') return F_IN | F_N;
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return F_IN | F_L;
  if (c < 0x80) return F_IN;
  /* fora do ASCII: letra, menos simbolos, pontuacao e marcas comuns */
  if ((c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7 || (c >= 0x300 && c <= 0x36F) ||
      (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) ||
      (c >= 0x20A0 && c <= 0x20CF) || (c >= 0x2190 && c <= 0x2BFF) ||
      (c >= 0x2E00 && c <= 0x2E7F) || (c >= 0x3001 && c <= 0x3003) ||
      (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFF01 && c <= 0xFF0F) ||
      (c >= 0x1F000 && c <= 0x1FAFF))
    return F_IN;
  return F_IN | F_L;
}

static void emit_tok(int id) {
  if (id >= 0 && t_nout < t_cap) t_out[t_nout++] = id;
}

/* BPE de um pedaco [a, b) em caracteres */
static void bpe(int a, int b) {
  int i, n = 0, bo = t_off[a], eo = t_off[b];
  if (eo <= bo) return;
  for (i = bo; i < eo; i++) t_sym[n++] = byte_tok[t_txt[i]];
  for (;;) {
    int best = 0x7FFFFFFF, bi = -1, res = 0;
    for (i = 0; i + 1 < n; i++) {
      int rr, rk = merge_find(t_sym[i], t_sym[i + 1], &rr);
      if (rk >= 0 && rk < best) {
        best = rk;
        bi = i;
        res = rr;
      }
    }
    if (bi < 0) break;
    t_sym[bi] = res;
    for (i = bi + 1; i + 1 < n; i++) t_sym[i] = t_sym[i + 1];
    n--;
  }
  for (i = 0; i < n; i++) emit_tok(t_sym[i]);
}

static int fl(int i, int end) { return i < end ? t_fl[i] : 0; }
static u32 cpt(int i, int end) { return i < end ? t_cp[i] : 0xFFFFFFFFu; }
static u32 lower(u32 c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* regex do GPT-2: 's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+ */
static void split_gpt2(int pos, int end) {
  while (pos < end) {
    u32 c = t_cp[pos];
    int f = t_fl[pos], f2, prev = pos, nws;
    if (c == '\'' && pos + 1 < end) {
      u32 c1 = t_cp[pos + 1];
      if (c1 == 's' || c1 == 't' || c1 == 'm' || c1 == 'd') { bpe(pos, pos + 2); pos += 2; continue; }
      if (pos + 2 < end) {
        u32 c2 = t_cp[pos + 2];
        if ((c1 == 'r' && c2 == 'e') || (c1 == 'v' && c2 == 'e') || (c1 == 'l' && c2 == 'l')) {
          bpe(pos, pos + 3);
          pos += 3;
          continue;
        }
      }
    }
    f2 = c == ' ' ? fl(pos + 1, end) : f;
    if (f2 & F_L) {
      pos += c == ' ';
      while (fl(pos, end) & F_L) pos++;
      bpe(prev, pos);
      continue;
    }
    if (f2 & F_N) {
      pos += c == ' ';
      while (fl(pos, end) & F_N) pos++;
      bpe(prev, pos);
      continue;
    }
    if (f2 == F_IN) {
      pos += c == ' ';
      while (fl(pos, end) == F_IN) pos++;
      bpe(prev, pos);
      continue;
    }
    nws = 0;
    while (fl(pos + nws, end) & F_WS) nws++;
    if (nws > 1 && pos + nws < end) pos += nws - 1;
    else if (nws > 0) pos += nws;
    else pos++;
    bpe(prev, pos);
  }
}

/* regex do Llama 3 / Qwen2 (\p{N}{1,maxd}) */
static void split_llama3(int pos, int end, int maxd) {
  while (pos < end) {
    u32 c = t_cp[pos];
    int f = t_fl[pos], f2, prev = pos, nws, lastnl;
    if (c == '\'' && pos + 1 < end) {
      u32 c1 = lower(t_cp[pos + 1]);
      if (c1 == 's' || c1 == 't' || c1 == 'm' || c1 == 'd') { bpe(pos, pos + 2); pos += 2; continue; }
      if (pos + 2 < end) {
        u32 c2 = lower(t_cp[pos + 2]);
        if ((c1 == 'r' && c2 == 'e') || (c1 == 'v' && c2 == 'e') || (c1 == 'l' && c2 == 'l')) {
          bpe(pos, pos + 3);
          pos += 3;
          continue;
        }
      }
    }
    if (!(c == '\r' || c == '\n' || (f & F_N)) && ((f & F_L) || (fl(pos + 1, end) & F_L))) {
      pos++;
      while (fl(pos, end) & F_L) pos++;
      bpe(prev, pos);
      continue;
    }
    if (f & F_N) {
      while ((fl(pos, end) & F_N) && pos - prev < maxd) pos++;
      bpe(prev, pos);
      continue;
    }
    f2 = c == ' ' ? fl(pos + 1, end) : f;
    if (f2 == F_IN) {
      pos += c == ' ';
      while (fl(pos, end) == F_IN) pos++;
      while (cpt(pos, end) == '\r' || cpt(pos, end) == '\n') pos++;
      bpe(prev, pos);
      continue;
    }
    nws = 0;
    lastnl = -1;
    while (fl(pos + nws, end) & F_WS) {
      u32 c2 = t_cp[pos + nws];
      if (c2 == '\r' || c2 == '\n') lastnl = pos + nws + 1;
      nws++;
    }
    if (lastnl > 0) pos = lastnl;
    else if (nws > 1 && pos + nws < end) pos += nws - 1;
    else if (nws > 0) pos += nws;
    else pos++;
    bpe(prev, pos);
  }
}

/* texto -> tokens, acrescentados em out[*n..cap) */
static void tokenize(const char* s, int len, int* o, int* n, int cap) {
  int i, ncp = 0;
  t_cp = (u32*)xmalloc((u32)len * 4 + 4);
  t_off = (int*)xmalloc((u32)len * 4 + 8);
  t_fl = (u8*)xmalloc((u32)len + 4);
  t_sym = (int*)xmalloc((u32)len * 4 + 4);
  t_txt = (const u8*)s;
  t_out = o;
  t_nout = *n;
  t_cap = cap;
  for (i = 0; i < len;) {
    int k;
    u32 c = utf8_get((const u8*)s + i, len - i, &k);
    t_cp[ncp] = c;
    t_fl[ncp] = (u8)cp_flags(c);
    t_off[ncp++] = i;
    i += k;
  }
  t_off[ncp] = len;
  if (pre_kind == 2 || pre_kind == 3) split_llama3(0, ncp, pre_kind == 2 ? 1 : 3);
  else if (pre_kind == 1) {
    int a = 0;
    for (i = 0; i <= ncp; i++)
      if (i == ncp || (t_fl[i] & F_N)) {
        split_gpt2(a, i);
        if (i < ncp) bpe(i, i + 1);
        a = i + 1;
      }
  } else
    split_gpt2(0, ncp);
  *n = t_nout;
  k_free(t_cp);
  k_free(t_off);
  k_free(t_fl);
  k_free(t_sym);
}

/* bytes de um token; controle (tipo 3) nao aparece */
static int detok(int id, char* o, int max) {
  const u8* s;
  int n = 0, i;
  if (id < 0 || id >= n_vocab || tok_t[id] == 3) return 0;
  s = (const u8*)tok_s[id];
  for (i = 0; i < (int)tok_n[id] && n < max - 4;) {
    int k;
    u32 c = utf8_get(s + i, (int)tok_n[id] - i, &k);
    if (tok_t[id] != 4 && c < 512 && u2b[c] >= 0) o[n++] = (char)u2b[c];
    else {
      CPY(o + n, s + i, (u32)k);
      n += k;
    }
    i += k;
  }
  return n;
}

/* ---------------- CP850 <-> UTF-8 ---------------- */
/* O modelo fala UTF-8; o console e o teclado do SODOS usam CP850 (como o
 * AGENT.EXE). -utf8 desliga a conversao (terminal do Linux no soxrun). */
static int g_utf8;

/* U+00A0..U+00FF -> CP850 (igual ao mc/view.c e ao agent.c) */
static const u8 latin1_cp850[96] = {
  0xFF, 0xAD, 0xBD, 0x9C, 0xCF, 0xBE, 0xDD, 0xF5, 0xF9, 0xB8, 0xA6, 0xAE,
  0xAA, 0xF0, 0xA9, 0xEE, 0xF8, 0xF1, 0xFD, 0xFC, 0xEF, 0xE6, 0xF4, 0xFA,
  0xF7, 0xFB, 0xA7, 0xAF, 0xAC, 0xAB, 0xF3, 0xA8, 0xB7, 0xB5, 0xB6, 0xC7,
  0x8E, 0x8F, 0x92, 0x80, 0xD4, 0x90, 0xD2, 0xD3, 0xDE, 0xD6, 0xD7, 0xD8,
  0xD1, 0xA5, 0xE3, 0xE0, 0xE2, 0xE5, 0x99, 0x9E, 0x9D, 0xEB, 0xE9, 0xEA,
  0x9A, 0xED, 0xE8, 0xE1, 0x85, 0xA0, 0x83, 0xC6, 0x84, 0x86, 0x91, 0x87,
  0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B, 0xD0, 0xA4, 0x95, 0xA2,
  0x93, 0xE4, 0x94, 0xF6, 0x9B, 0x97, 0xA3, 0x96, 0x81, 0xEC, 0xE7, 0x98,
};

/* Aspas, tracos, setas e molduras; o resto vira '?'. */
static const u16 uni_from[] = {
  0x2018, 0x2019, 0x201C, 0x201D, 0x2013, 0x2014, 0x2022, 0x2026, 0x20AC,
  0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C,
  0x2534, 0x253C, 0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D, 0x2588,
  0x2591, 0x2592, 0x2593, 0x2192, 0x2190, 0x2191, 0x2193,
};
static const u8 uni_to[] = {
  0x27, 0x27, 0x22, 0x22, 0x2D, 0x2D, 0x07, 0x2E, 0x45,
  0xC4, 0xB3, 0xDA, 0xBF, 0xC0, 0xD9, 0xC3, 0xB4, 0xC2,
  0xC1, 0xC5, 0xCD, 0xBA, 0xC9, 0xBB, 0xC8, 0xBC, 0xDB,
  0xB0, 0xB1, 0xB2, 0x1A, 0x1B, 0x18, 0x19,
};

static u8 uni_cp850(u32 c) {
  u32 i;
  if (c < 0x80) return (u8)c;
  if (c >= 0xA0 && c <= 0xFF) return latin1_cp850[c - 0xA0];
  for (i = 0; i < sizeof(uni_to); i++)
    if (uni_from[i] == c) return uni_to[i];
  return '?';
}

static u32 cp850_uni(u8 c) {
  u32 i;
  if (c < 0x80) return c;
  for (i = 0; i < 96; i++)
    if (latin1_cp850[i] == c) return 0xA0 + i;
  for (i = 0; i < sizeof(uni_to); i++)
    if (uni_to[i] == c) return uni_from[i];
  return '?';
}

/* Saida de tokens: um caractere UTF-8 pode vir partido entre dois tokens. */
static u32 o_cp;
static int o_need;

static void out_text(const char* s, int n) {
  char b[256];
  int i, k = 0;
  if (g_utf8) {
    outn(s, n);
    return;
  }
  for (i = 0; i < n; i++) {
    u8 c = (u8)s[i];
    if (k > (int)sizeof(b) - 2) {
      outn(b, k);
      k = 0;
    }
    if (o_need) {
      if ((c & 0xC0) == 0x80) {
        o_cp = (o_cp << 6) | (c & 63);
        if (--o_need == 0) b[k++] = (char)uni_cp850(o_cp);
        continue;
      }
      o_need = 0;
      b[k++] = '?';
    }
    if (c < 0x80) b[k++] = (char)c;
    else if ((c & 0xE0) == 0xC0) { o_cp = c & 31; o_need = 1; }
    else if ((c & 0xF0) == 0xE0) { o_cp = c & 15; o_need = 2; }
    else if ((c & 0xF8) == 0xF0) { o_cp = c & 7; o_need = 3; }
    else b[k++] = '?';
  }
  if (k) outn(b, k);
}

/* Texto do usuario (teclado/argv em CP850) para UTF-8, em memoria nova. */
static char* in_text(const char* s) {
  u32 n = (u32)strlen(s), i;
  char* o = (char*)xmalloc(n * 3 + 1);
  int k = 0;
  if (g_utf8) {
    CPY(o, s, n + 1);
    return o;
  }
  for (i = 0; i < n; i++) k += utf8_put(cp850_uni((u8)s[i]), o + k);
  o[k] = 0;
  return o;
}

/* ---------------- amostragem ---------------- */
static u64 rng = 0x2545F4914F6CDD1Dull;
static float temp = 0.3f, topp = 0.9f, rep_pen = 1.1f;
static int recent[64], n_recent;

static float frand(void) {
  rng ^= rng >> 12;
  rng ^= rng << 25;
  rng ^= rng >> 27;
  return (float)((rng * 0x2545F4914F6CDD1Dull) >> 40) / 16777216.0f;
}

static int* cand;

static int sample(float* lg) {
  int i, best = 0, nc = 0;
  float mx, sum = 0, cut, cum = 0, r;
  /* penalidade de repeticao: uma vez por token distinto (como o llama.cpp) */
  for (i = 0; i < n_recent; i++) {
    int id = recent[i], j;
    for (j = 0; j < i && recent[j] != id; j++) {
    }
    if (j < i) continue;
    lg[id] = lg[id] > 0 ? lg[id] / rep_pen : lg[id] * rep_pen;
  }
  for (i = 1; i < n_vocab; i++)
    if (lg[i] > lg[best]) best = i;
  if (temp <= 0) return best;
  mx = lg[best];
  for (i = 0; i < n_vocab; i++) {
    lg[i] = (float)m_exp((lg[i] - mx) / temp);
    sum += lg[i];
  }
  /* top-p: so os candidatos com prob >= (1-p)/(n-1), em ordem */
  cut = (1.0f - topp) / (n_vocab - 1) * sum;
  for (i = 0; i < n_vocab; i++)
    if (lg[i] >= cut) cand[nc++] = i;
  {
    int gap, j;
    for (gap = nc / 2; gap > 0; gap /= 2)
      for (i = gap; i < nc; i++) {
        int t = cand[i];
        for (j = i; j >= gap && lg[cand[j - gap]] < lg[t]; j -= gap) cand[j] = cand[j - gap];
        cand[j] = t;
      }
  }
  for (i = 0; i < nc; i++) {
    cum += lg[cand[i]];
    if (cum > topp * sum) {
      nc = i + 1;
      break;
    }
  }
  r = frand() * cum;
  for (i = 0; i < nc; i++) {
    r -= lg[cand[i]];
    if (r <= 0) return cand[i];
  }
  return cand[nc - 1];
}

static void remember(int id) {
  if (n_recent < 64) recent[n_recent++] = id;
  else {
    memmove(recent, recent + 1, 63 * sizeof(int));
    recent[63] = id;
  }
}

/* ---------------- chat ---------------- */
static int g_pos;
static int* ptok;
static int n_ptok;
static int im_start = -1, im_end = -1, endoftext = -1;
static int chatml;
static char* g_sys;

static void p_text(const char* s) { tokenize(s, (int)strlen(s), ptok, &n_ptok, n_ctx); }
static void p_id(int id) {
  if (id >= 0 && n_ptok < n_ctx) ptok[n_ptok++] = id;
}

static int is_stop(int id) {
  return id == eos_id || id == eot_id || id == im_end || id == endoftext;
}

/* processa ptok; devolve 0, ou -1 se o contexto encheu */
static int feed(void) {
  int i;
  for (i = 0; i < n_ptok; i++) {
    if (g_pos >= n_ctx) return -1;
    forward(ptok[i], g_pos++);
    remember(ptok[i]);
  }
  n_ptok = 0;
  return 0;
}

/* gera ate maxn tokens; devolve quantos */
static int generate(int maxn, unsigned* ms) {
  int n = 0;
  char buf[64];
  unsigned t0 = now_ms();
  float* lg = logits;
  while (n < maxn) {
    int id = sample(lg), len;
    if (is_stop(id)) break;
    len = detok(id, buf, sizeof(buf));
    if (len) out_text(buf, len);
    remember(id);
    n++;
    if (g_pos >= n_ctx) {
      out("\n[contexto cheio]");
      break;
    }
    lg = forward(id, g_pos++);
  }
  *ms = now_ms() - t0;
  return n;
}

static void stats(int n, unsigned ms) {
  out("\n[");
  out_int(n);
  out(" tokens, ");
  out_f1(ms ? n * 1000.0 / ms : 0);
  out(" tok/s, contexto ");
  out_int(g_pos);
  out("/");
  out_int(n_ctx);
  out("]\n");
}

/* mensagem de sistema padrao do template ("<|im_start|>system\n...<|im_end|>") */
static void default_sys(void) {
  const char* t = g_tmpl.s;
  u32 i, n = g_tmpl.n;
  static const char key[] = "<|im_start|>system";
  if (g_sys) return;
  for (i = 0; t && i + sizeof(key) + 2 < n; i++) {
    u32 j, k = 0;
    if (memcmp(t + i, key, sizeof(key) - 1)) continue;
    j = i + sizeof(key) - 1;
    if (t[j] == '\n') j++;
    else if (t[j] == '\\' && t[j + 1] == 'n') j += 2;
    else continue;
    if (t[j] == '\'' || t[j] == '{') continue;
    g_sys = (char*)xmalloc(n - j + 1);
    while (j < n && memcmp(t + j, "<|im_end|>", 10)) {
      if (t[j] == '\\' && j + 1 < n && t[j + 1] == 'n') {
        g_sys[k++] = '\n';
        j += 2;
      } else
        g_sys[k++] = t[j++];
    }
    g_sys[k] = 0;
    return;
  }
}

static void turn(const char* user, int first) {
  if (chatml) {
    if (first && g_sys && *g_sys) {
      p_id(im_start);
      p_text("system\n");
      p_text(g_sys);
      p_id(im_end);
      p_text("\n");
    }
    p_id(im_start);
    p_text("user\n");
    p_text(user);
    p_id(im_end);
    p_text("\n");
    p_id(im_start);
    p_text("assistant\n");
  } else {
    if (first && add_bos && bos_id >= 0) p_id(bos_id);
    p_text(first ? "" : "\n");
    p_text("User: ");
    p_text(user);
    p_text("\nAssistant:");
  }
}

static int s_atoi(const char* s) {
  int v = 0, neg = 0;
  if (*s == '-') { neg = 1; s++; }
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return neg ? -v : v;
}

static float s_atof(const char* s) {
  float v = 0, f = 0.1f;
  int neg = 0;
  if (*s == '-') { neg = 1; s++; }
  while (*s >= '0' && *s <= '9') v = v * 10 + (float)(*s++ - '0');
  if (*s == '.' || *s == ',')
    for (s++; *s >= '0' && *s <= '9'; s++, f *= 0.1f) v += f * (float)(*s - '0');
  return neg ? -v : v;
}

static void usage(void) {
  out("uso: llama MODELO.GGU [-p \"pergunta\"] [-n N] [-t temp] [-c ctx] [-s semente]\n"
      "                      [-r pen] [-sys \"texto\"] [-raw] [-tok \"texto\"] [-utf8]\n"
      "  Roda um LLM GGUF do Hugging Face (llama/qwen2; F16, Q8_0, Q4_0).\n"
      "  Ex.: SmolLM2-135M-Instruct-Q8_0.gguf copiado como SMOL135.GGU,\n"
      "       Qwen2.5-0.5B-Instruct-Q8_0.gguf como QWEN05.GGU (pede 1 GB de RAM)\n");
  k_exit(1);
}

/* O shell do SODOS separa so por espaco: junta "varias palavras" num argv. */
static int join_quotes(int argc, char** argv) {
  int i, n = 0;
  for (i = 0; i < argc; i++) {
    char* a = argv[i];
    int len = (int)strlen(a);
    if (a[0] == '"') {
      int tot = 0, j, k = 0;
      char* s;
      for (j = i; j < argc; j++) {
        int l = (int)strlen(argv[j]);
        tot += l + 1;
        if ((j > i || l > 1) && argv[j][l - 1] == '"') break;
      }
      s = (char*)xmalloc((u32)tot + 1);
      for (; i <= j && i < argc; i++) {
        int l = (int)strlen(argv[i]);
        if (k) s[k++] = ' ';
        CPY(s + k, argv[i], (u32)l);
        k += l;
      }
      i--;
      s[k] = 0;
      if (k && s[k - 1] == '"' && k > 1) s[--k] = 0;
      argv[n++] = s + 1;
    } else if (len)
      argv[n++] = a;
  }
  return n;
}

int main(int argc, char** argv) {
  const char *model = 0, *prompt = 0, *tokstr = 0;
  int i, maxn = 256, want_ctx = 0, raw = 0, seeded = 0;
  unsigned t0;
  argc = join_quotes(argc, argv);
  for (i = 1; i < argc; i++) {
    const char* a = argv[i];
    int more = i + 1 < argc;
    if (!strcmp(a, "-p") && more) prompt = argv[++i];
    else if (!strcmp(a, "-n") && more) maxn = s_atoi(argv[++i]);
    else if (!strcmp(a, "-t") && more) temp = s_atof(argv[++i]);
    else if (!strcmp(a, "-c") && more) want_ctx = s_atoi(argv[++i]);
    else if (!strcmp(a, "-s") && more) { rng = (u64)s_atoi(argv[++i]) * 2654435761u + 1; seeded = 1; }
    else if (!strcmp(a, "-sys") && more) g_sys = argv[++i];
    else if (!strcmp(a, "-tok") && more) tokstr = argv[++i];
    else if (!strcmp(a, "-r") && more) rep_pen = s_atof(argv[++i]);
    else if (!strcmp(a, "-raw")) raw = 1;
    else if (!strcmp(a, "-utf8")) g_utf8 = 1;
    else if (a[0] == '-' || model) usage();
    else model = a;
  }
  if (!model) usage();
  if (prompt) prompt = in_text(prompt);
  if (g_sys) g_sys = in_text(g_sys);
  if (tokstr) tokstr = in_text(tokstr);
  if (!seeded) {
    unsigned hi = 0, lo = k_rdtsc(&hi);
    rng ^= ((u64)hi << 32) | lo;
    if (!rng) rng = 1;
  }
  clock_init();
  for (i = 0; i < 65536; i++) f16tab[i] = h2f((u16)i);
  load_file(model);
  t0 = now_ms();
  parse_gguf();
  setup_model();
  tok_init();
  im_start = vocab_find("<|im_start|>", 12);
  im_end = vocab_find("<|im_end|>", 10);
  endoftext = vocab_find("<|endoftext|>", 13);
  chatml = im_start >= 0 && im_end >= 0;
  if (tokstr) {
    int n = 0, cap = (int)strlen(tokstr) + 8;
    int* o = (int*)xmalloc((u32)cap * 4);
    tokenize(tokstr, (int)strlen(tokstr), o, &n, cap);
    for (i = 0; i < n; i++) {
      out_int(o[i]);
      out(i + 1 < n ? " " : "\n");
    }
    return 0;
  }
  alloc_state(want_ctx);
  ptok = (int*)xmalloc((u32)n_ctx * 4);
  cand = (int*)xmalloc((u32)n_vocab * 4);
  default_sys();
  out("modelo: ");
  if (g_name.n) outn(g_name.s, (int)g_name.n);
  out(" (");
  outn(g_arch.s, (int)g_arch.n);
  out(", ");
  out_int(n_layer);
  out(" camadas, dim ");
  out_int(n_embd);
  out(", vocab ");
  out_int(n_vocab);
  out(", contexto ");
  out_int(n_ctx);
  out(") pronto em ");
  out_f1((now_ms() - t0) / 1000.0);
  out(" s\n");

  if (prompt) {
    unsigned ms;
    int n;
    if (raw) {
      if (add_bos && bos_id >= 0) p_id(bos_id);
      p_text(prompt);
      out(prompt);
    } else
      turn(prompt, 1);
    if (feed() < 0) die("pergunta maior que o contexto");
    n = generate(maxn, &ms);
    stats(n, ms);
    return 0;
  }

  out("Chat: escreva e Enter. /reset limpa a conversa, /sair termina.\n");
  {
    static char line[2048];
    char* uline;
    int first = 1;
    for (;;) {
      int n, len;
      unsigned ms;
      out("\n> ");
      len = k_readline(line, sizeof(line));
      if (len < 0 || (len > 0 && line[0] == 26)) break;
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
      if (!len) continue;
      if (!strcmp(line, "/sair") || !strcmp(line, "/exit") || !strcmp(line, "/bye")) break;
      if (!strcmp(line, "/reset")) {
        g_pos = 0;
        n_recent = 0;
        first = 1;
        out("(conversa limpa)\n");
        continue;
      }
      uline = in_text(line);
      turn(uline, first);
      if (g_pos + n_ptok + 8 > n_ctx) {
        out("(contexto cheio: conversa reiniciada)\n");
        g_pos = 0;
        n_recent = 0;
        n_ptok = 0;
        turn(uline, 1);
      }
      first = 0;
      k_free(uline);
      if (feed() < 0) {
        out("(mensagem grande demais)\n");
        g_pos = 0;
        n_ptok = 0;
        first = 1;
        continue;
      }
      n = generate(maxn, &ms);
      stats(n, ms);
      /* fecha a vez do assistente */
      if (chatml) {
        p_id(im_end);
        p_text("\n");
        if (g_pos + n_ptok <= n_ctx) feed();
        else n_ptok = 0;
      }
    }
  }
  return 0;
}
