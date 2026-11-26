/* t_cubeb_sodos.c - testa o backend de audio do Firefox do SODOS
 * (sodos/cubeb/cubeb_sodos.c) no Linux, com uma placa falsa no lugar dos
 * k_audio_* do kernel. Roda na VM do WSL:
 *
 *   cd src13/firefox/sodos/tests
 *   gcc -O2 -Wall -Wextra -Wno-unused-parameter -I cubeb_stub -pthread \
 *       t_cubeb_sodos.c -o /tmp/t_cubeb -lm && /tmp/t_cubeb
 *   (mais -fsanitize=thread ou -fsanitize=address,undefined)
 *
 * A placa falsa toca 48 kHz estereo 16 bits em tempo real a partir de
 * quando tem amostras (falta de som recomeca o relogio dela, como o
 * driver), segura no maximo 200 ms na fila (k_audio_write espera) e grava
 * o que recebeu para conferir o conteudo. */
#include <math.h>
#include <stdio.h>

#include "../cubeb/cubeb_sodos.c"

/* ---------- placa falsa ---------- */
enum { D_REAL, D_BUSY, D_NULL, D_NOSYM };
#define D_CAP (48000 * 200 / 1000)
#define REC_MAX (48000 * 12)

static pthread_mutex_t dlk = PTHREAD_MUTEX_INITIALIZER;
static volatile int d_mode;
static int d_open, d_opens, d_closes, d_underruns;
static uint64_t d_written, d_pbase, d_tbase;
static int16_t rec[REC_MAX * 2];
static long rec_n;

static uint64_t
d_played_l(void)
{
  uint64_t k = d_pbase + (now_ns() - d_tbase) / 1000ull * 48000ull / 1000000ull;
  return k < d_written ? k : d_written;
}

static int
f_open(int rate, int ch, int bits)
{
  int r = 0;
  pthread_mutex_lock(&dlk);
  if (rate != 48000 || ch != 2 || bits != 16) {
    r = -1;
  } else if (d_mode == D_BUSY) {
    r = -2;
  } else {
    d_written = d_played_l();  /* reabrir corta a fila */
    d_open = 1;
    d_opens++;
  }
  pthread_mutex_unlock(&dlk);
  return r;
}

static int
f_write(void const * buf, int bytes)
{
  int n = bytes / 4;
  pthread_mutex_lock(&dlk);
  if (!d_open) {
    pthread_mutex_unlock(&dlk);
    return -1;
  }
  if (d_mode == D_NULL) {  /* driver nulo: joga fora */
    pthread_mutex_unlock(&dlk);
    return bytes;
  }
  if (d_played_l() >= d_written) {
    if (d_written) {
      d_underruns++;
    }
    d_pbase = d_written;
    d_tbase = now_ns();
  }
  while (d_written - d_played_l() + (uint64_t)n > D_CAP) {
    pthread_mutex_unlock(&dlk);
    sleep_ms(2);
    pthread_mutex_lock(&dlk);
  }
  if (rec_n + n <= REC_MAX) {
    memcpy(rec + 2 * rec_n, buf, (size_t)n * 4);
    rec_n += n;
  }
  d_written += (uint64_t)n;
  pthread_mutex_unlock(&dlk);
  return bytes;
}

static int
f_close(int wait)
{
  (void)wait;
  pthread_mutex_lock(&dlk);
  d_written = d_played_l();
  d_open = 0;
  d_closes++;
  pthread_mutex_unlock(&dlk);
  return 0;
}

static int
f_pending(void)
{
  int p;
  pthread_mutex_lock(&dlk);
  p = d_open && d_mode != D_NULL ? (int)(d_written - d_played_l()) * 4 : 0;
  pthread_mutex_unlock(&dlk);
  return p;
}

static int
f_volume(int pct)
{
  int r;
  (void)pct;
  pthread_mutex_lock(&dlk);
  r = d_mode == D_NULL ? -1 : 70;
  pthread_mutex_unlock(&dlk);
  return r;
}

void *
k_dl_sym(int handle, char const * name, void const * caller)
{
  (void)handle;
  (void)caller;
  if (d_mode == D_NOSYM) {
    return NULL;
  }
  if (!strcmp(name, "k_audio_open")) return (void *)f_open;
  if (!strcmp(name, "k_audio_write")) return (void *)f_write;
  if (!strcmp(name, "k_audio_close")) return (void *)f_close;
  if (!strcmp(name, "k_audio_pending")) return (void *)f_pending;
  if (!strcmp(name, "k_audio_volume")) return (void *)f_volume;
  return NULL;
}

static void
dev_reset(int mode)
{
  pthread_mutex_lock(&dlk);
  d_mode = mode;
  d_open = d_opens = d_closes = d_underruns = 0;
  d_written = d_pbase = 0;
  d_tbase = now_ns();
  rec_n = 0;
  pthread_mutex_unlock(&dlk);
  memset(&kau, 0, sizeof kau);  /* o backend procura os simbolos de novo */
}

/* ---------- "Gecko" ---------- */
struct gen {
  cubeb_stream_params p;
  int kind;          /* 0 seno, 1 constante */
  double hz, amp;
  long limit;        /* quadros no total (<0: sem fim) */
  long gave;
  volatile int st[4];
};

static long
gen_cb(cubeb_stream * s, void * u, void const * in, void * out, long n)
{
  struct gen * g = u;
  long k;
  unsigned c;
  (void)s;
  (void)in;
  if (g->limit >= 0 && g->gave + n > g->limit) {
    n = g->limit - g->gave;
  }
  for (k = 0; k < n; k++) {
    double v = g->kind ? g->amp : g->amp * sin(2 * M_PI * g->hz * (double)(g->gave + k) / g->p.rate);
    for (c = 0; c < g->p.channels; c++) {
      if (g->p.format == CUBEB_SAMPLE_FLOAT32LE) {
        ((float *)out)[k * g->p.channels + c] = (float)v;
      } else {
        ((int16_t *)out)[k * g->p.channels + c] = (int16_t)(v * 32767.0);
      }
    }
  }
  __atomic_store_n(&g->gave, g->gave + n, __ATOMIC_SEQ_CST);
  return n;
}

static void
gen_st(cubeb_stream * s, void * u, cubeb_state st)
{
  (void)s;
  __atomic_add_fetch(&((struct gen *)u)->st[st], 1, __ATOMIC_SEQ_CST);
}

static void
gen_init(struct gen * g, cubeb_sample_format f, uint32_t rate, uint32_t ch, int kind, double hz,
         double amp, long limit)
{
  memset(g, 0, sizeof *g);
  g->p.format = f;
  g->p.rate = rate;
  g->p.channels = ch;
  g->kind = kind;
  g->hz = hz;
  g->amp = amp;
  g->limit = limit;
}

static cubeb_stream *
mk(cubeb * c, struct gen * g)
{
  cubeb_stream * s = NULL;
  int r = sodos_ops.stream_init(c, &s, "t", NULL, NULL, NULL, &g->p, 0, gen_cb, gen_st, g);
  if (r != CUBEB_OK) {
    printf("stream_init falhou %d\n", r);
    exit(1);
  }
  return s;
}

static uint64_t
pos(cubeb_stream * s)
{
  uint64_t p = 0;
  sodos_ops.stream_get_position(s, &p);
  return p;
}

static int fails;

/* Leituras do main: os contadores mudam na thread do misturador. */
#define ST(g, x) __atomic_load_n(&(g).st[x], __ATOMIC_SEQ_CST)
#define GAVE(g) __atomic_load_n(&(g).gave, __ATOMIC_SEQ_CST)

static long
rec_count(void)
{
  long v;
  pthread_mutex_lock(&dlk);
  v = rec_n;
  pthread_mutex_unlock(&dlk);
  return v;
}

static int
closes(void)
{
  int v;
  pthread_mutex_lock(&dlk);
  v = d_closes;
  pthread_mutex_unlock(&dlk);
  return v;
}

static void
set_mode(int m)
{
  pthread_mutex_lock(&dlk);
  d_mode = m;
  pthread_mutex_unlock(&dlk);
}
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      printf("  ok   ");                                                       \
    } else {                                                                   \
      printf("  FALHA ");                                                      \
      fails++;                                                                 \
    }                                                                          \
    printf(__VA_ARGS__);                                                       \
    printf("\n");                                                              \
  } while (0)

static int16_t
expect_sine(struct gen * g, long k)
{
  float f = (float)(g->amp * sin(2 * M_PI * g->hz * (double)k / g->p.rate));
  float x = f * 32768.0f;
  return x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : (int16_t)x;
}

/* quantos quadros do inicio de rec[] batem com o seno */
static long
match_sine(struct gen * g)
{
  long k, n = rec_count();
  for (k = 0; k < n; k++) {
    int16_t e = expect_sine(g, k);
    if (rec[2 * k] != e || rec[2 * k + 1] != e) {
      break;
    }
  }
  return k;
}

int
main(void)
{
  cubeb * c;
  cubeb_stream *s, *s2;
  struct gen g, g2;
  uint64_t p1, p2, p3, t0;
  long m;
  int i;

  printf("1. 48 kHz float estereo, 1 s, pausa e volta sem perder amostra\n");
  dev_reset(D_REAL);
  sodos_init(&c, "t");
  gen_init(&g, CUBEB_SAMPLE_FLOAT32LE, 48000, 2, 0, 1000, 0.5, -1);
  s = mk(c, &g);
  sodos_ops.stream_start(s);
  sleep_ms(1000);
  p1 = pos(s);
  CHECK(p1 > 44000 && p1 < 49000, "posicao depois de 1 s: %llu (~48000)", (unsigned long long)p1);
  CHECK(ST(g, CUBEB_STATE_STARTED) == 1, "STARTED");
  sodos_ops.stream_stop(s);
  CHECK(ST(g, CUBEB_STATE_STOPPED) == 1, "STOPPED");
  sleep_ms(300);
  p2 = pos(s);
  CHECK(p2 >= p1 && p2 - p1 < 48 * 130, "parado: a fila tocou %llu quadros e parou",
        (unsigned long long)(p2 - p1));
  sleep_ms(200);
  CHECK(pos(s) == p2, "parado: a posicao nao anda (%llu)", (unsigned long long)pos(s));
  sodos_ops.stream_start(s);
  sleep_ms(500);
  p3 = pos(s);
  CHECK(p3 > p2 + 20000 && p3 < p2 + 25000, "de volta: +%llu em 500 ms (~24000)",
        (unsigned long long)(p3 - p2));
  m = match_sine(&g);
  CHECK(m == rec_count() && rec_count() > 70000, "a placa recebeu o seno inteiro, sem buraco: %ld de %ld quadros", m,
        rec_count());
  CHECK(d_underruns == 1, "falta de som so na pausa (%d)", d_underruns);
  sodos_ops.stream_destroy(s);
  sleep_ms(50);
  CHECK(closes() == 1, "destroy corta a placa (%d)", closes());
  sodos_destroy(c);

  printf("2. fim do fluxo: 0,5 s e DRAINED\n");
  dev_reset(D_REAL);
  sodos_init(&c, "t");
  gen_init(&g, CUBEB_SAMPLE_FLOAT32LE, 48000, 2, 0, 440, 0.3, 24000);
  s = mk(c, &g);
  t0 = now_ns();
  sodos_ops.stream_start(s);
  for (i = 0; i < 300 && !ST(g, CUBEB_STATE_DRAINED); i++) {
    sleep_ms(10);
  }
  p1 = (now_ns() - t0) / 1000000ull;
  CHECK(ST(g, CUBEB_STATE_DRAINED) == 1 && p1 >= 480 && p1 < 700, "DRAINED em %llu ms (~500)",
        (unsigned long long)p1);
  CHECK(pos(s) == 24000, "posicao final %llu (24000)", (unsigned long long)pos(s));
  for (m = 24000; m < rec_count() && rec[2 * m] == 0 && rec[2 * m + 1] == 0; m++) {
  }
  CHECK(match_sine(&g) >= 24000 && m == rec_count(),
        "a placa recebeu os 24000 quadros e depois silencio ate o DRAINED (%ld no total)", rec_count());
  sodos_ops.stream_destroy(s);
  sodos_destroy(c);

  printf("3. 44,1 kHz s16 mono\n");
  dev_reset(D_REAL);
  sodos_init(&c, "t");
  gen_init(&g, CUBEB_SAMPLE_S16LE, 44100, 1, 0, 500, 0.5, -1);
  s = mk(c, &g);
  sodos_ops.stream_start(s);
  sleep_ms(1000);
  p1 = pos(s);
  CHECK(p1 > 40500 && p1 < 45000, "posicao depois de 1 s: %llu (~44100)", (unsigned long long)p1);
  CHECK(rec_count() > 48000 && rec_count() < 53500, "a placa recebeu %ld quadros de 48 kHz", rec_count());
  for (m = 0; m < rec_count() && rec[2 * m] == rec[2 * m + 1]; m++) {
  }
  CHECK(m == rec_count(), "mono: esquerdo = direito");
  {
    int mx = 0;
    for (m = 0; m < rec_count(); m++) {
      mx = rec[2 * m] > mx ? rec[2 * m] : mx;
    }
    CHECK(mx > 16000 && mx <= 16384, "amplitude %d (~16383)", mx);
  }
  sodos_ops.stream_destroy(s);
  sodos_destroy(c);

  printf("4. dois streams somados, volume\n");
  dev_reset(D_REAL);
  sodos_init(&c, "t");
  gen_init(&g, CUBEB_SAMPLE_FLOAT32LE, 48000, 2, 1, 0, 0.25, -1);
  gen_init(&g2, CUBEB_SAMPLE_FLOAT32LE, 48000, 2, 1, 0, 0.25, -1);
  s = mk(c, &g);
  s2 = mk(c, &g2);
  sodos_ops.stream_set_volume(s2, 0.5f);
  sodos_ops.stream_start(s);
  sodos_ops.stream_start(s2);
  sleep_ms(300);
  CHECK(rec_count() > 0 && rec[2 * (rec_count() - 1)] == 12288, "ultima amostra %d (0,25 + 0,125 = 12288)",
        rec_count() ? rec[2 * (rec_count() - 1)] : 0);
  sodos_ops.stream_stop(s);
  sodos_ops.stream_stop(s2);
  sodos_ops.stream_destroy(s);
  sodos_ops.stream_destroy(s2);
  sodos_destroy(c);

  {
    static char const * const nome[] = {"", "placa ocupada (-2)", "driver nulo (AUDIO=0)",
                                        "kernel sem k_audio_*"};
    int mode;
    for (mode = D_BUSY; mode <= D_NOSYM; mode++) {
      printf("%d. %s: relogio\n", 4 + mode, nome[mode]);
      dev_reset(mode);
      sodos_init(&c, "t");
      gen_init(&g, CUBEB_SAMPLE_FLOAT32LE, 48000, 2, 0, 440, 0.3, -1);
      s = mk(c, &g);
      sodos_ops.stream_start(s);
      sleep_ms(1000);
      p1 = pos(s);
      CHECK(p1 > 44000 && p1 < 49000, "posicao depois de 1 s: %llu (~48000)", (unsigned long long)p1);
      CHECK(GAVE(g) < 48000 + 10000, "pediu %ld quadros ao Gecko (~48000 + 80 ms)", GAVE(g));
      CHECK(rec_count() == 0, "nada na placa (%ld)", rec_count());
      sodos_ops.stream_destroy(s);
      sodos_destroy(c);
    }
  }

  printf("8. SET AUDIO=0 no meio, depois SET AUDIO=1\n");
  dev_reset(D_REAL);
  sodos_init(&c, "t");
  gen_init(&g, CUBEB_SAMPLE_FLOAT32LE, 48000, 2, 0, 440, 0.3, -1);
  s = mk(c, &g);
  sodos_ops.stream_start(s);
  sleep_ms(300);
  set_mode(D_NULL);
  sleep_ms(700);
  CHECK(GAVE(g) < 48000 + 10000, "sem disparar: pediu %ld quadros em 1 s", GAVE(g));
  /* o driver nulo jogou fora a fila (<= 80 ms): a posicao pula uma vez */
  p1 = pos(s);
  CHECK(p1 > 40000 && p1 < 48000 + 3840 + 1500, "posicao %llu (~48000, mais a fila perdida)",
        (unsigned long long)p1);
  set_mode(D_REAL);
  m = rec_count();
  sleep_ms(1000);
  /* a volta so e notada na conferencia de 500 ms */
  CHECK(rec_count() - m > 20000, "voltou para a placa: %ld quadros no ultimo segundo", rec_count() - m);
  CHECK(GAVE(g) < 96000 + 12000, "pediu %ld quadros em 2 s", GAVE(g));
  sodos_ops.stream_destroy(s);
  sodos_destroy(c);

  printf("9. formato invalido e microfone\n");
  dev_reset(D_REAL);
  sodos_init(&c, "t");
  gen_init(&g, CUBEB_SAMPLE_FLOAT32LE, 48000, 2, 0, 440, 0.3, -1);
  {
    cubeb_stream * x = NULL;
    cubeb_stream_params in = g.p;
    CHECK(sodos_ops.stream_init(c, &x, "t", NULL, &in, NULL, &g.p, 0, gen_cb, gen_st, &g) ==
              CUBEB_ERROR_NOT_SUPPORTED,
          "microfone: NOT_SUPPORTED");
    g.p.rate = 500;
    CHECK(sodos_ops.stream_init(c, &x, "t", NULL, NULL, NULL, &g.p, 0, gen_cb, gen_st, &g) ==
              CUBEB_ERROR_INVALID_FORMAT,
          "500 Hz: INVALID_FORMAT");
  }
  sodos_destroy(c);

  printf(fails ? "FALHAS: %d\n" : "TUDO OK\n", fails);
  return fails != 0;
}
