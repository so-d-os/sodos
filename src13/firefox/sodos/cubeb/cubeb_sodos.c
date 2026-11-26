/*
 * cubeb_sodos.c - saida de audio do SODOS (k_audio_*, docs/AUDIO.md).
 *
 * Uma thread por contexto (o "misturador") pede ao Gecko (data_callback) as
 * amostras de cada stream que esta tocando, converte para float estereo,
 * muda a taxa para 48 kHz (interpolacao linear), aplica o volume do stream,
 * soma tudo e manda 16 bits estereo a 48 kHz para o k_audio_write. A fila
 * na placa fica em ~SODOS_TARGET_MS: o misturador olha o k_audio_pending a
 * cada SODOS_TICK_MS e completa.
 *
 * Posicao: quadros de 48 kHz mandados menos os que ainda estao na fila
 * (k_audio_pending) dizem quanto a placa ja tocou; cada stream guarda onde
 * os quadros dele entraram na mistura (um segmento por start) e converte de
 * volta para a taxa dele. O MediaDecoderStateMachine sincroniza o video por
 * essa posicao, contando que tudo o que ele entregou vai tocar: por isso a
 * pausa nao joga nada fora (o que esta na fila toca, a entrada que sobrou
 * fica para o proximo start).
 *
 * Sem placa o backend faz o papel de relogio, como o antigo backend nulo:
 * as amostras sao pedidas no ritmo do tempo real e jogadas fora. Isso vale
 * quando o kernel nao tem k_audio_* (anterior ao build 61: os simbolos sao
 * procurados com k_dl_sym, entao o FIREFOX.EXE abre em qualquer kernel), com
 * SET AUDIO=0 (driver nulo: k_audio_volume(-1) < 0), quando outro programa
 * esta com o som (k_audio_open -2; tenta de novo a cada segundo) ou se o
 * k_audio_write falhar.
 *
 * Um stream destruido (fim, seek, aba fechada) sem outro tocando corta a fila
 * da placa na hora (k_audio_close(0)); a placa fica livre para outros
 * programas depois de SODOS_IDLE_MS sem nada tocando.
 *
 * So usa pthread_create/join, mutex, clock_gettime e nanosleep, como o
 * backend nulo (sem pthread_cond). Teste no Linux com uma placa falsa:
 * sodos/tests/t_cubeb_sodos.c.
 *
 * Entra no cubeb.c pelo patch 0013 (USE_SODOS); build-browser64.sh copia este
 * arquivo para media/libcubeb/src.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cubeb-internal.h"
#include "cubeb/cubeb.h"

#define SODOS_RATE 48000
#define SODOS_CHANNELS 2
#define SODOS_TICK_MS 10
#define SODOS_TARGET_MS 80     /* fila na placa */
#define SODOS_CHUNK_MS 20      /* a mistura anda em pedacos de 20 ms */
#define SODOS_PULL_MS 20       /* cada data_callback pede ~20 ms */
#define SODOS_IDLE_MS 2000     /* sem som: solta a placa */
#define SODOS_RETRY_MS 1000    /* placa ocupada: tenta de novo */
#define SODOS_CHUNK_FRAMES (SODOS_RATE * SODOS_CHUNK_MS / 1000)
#define SODOS_TARGET_FRAMES ((uint64_t)SODOS_RATE * SODOS_TARGET_MS / 1000)

/* ---------- k_audio_* do kernel, procurados em tempo de execucao ---------- */
void * k_dl_sym(int handle, char const * name, void const * caller);

static struct {
  int done;
  int (*open)(int rate, int channels, int bits);
  int (*write)(void const * buf, int bytes);
  int (*close)(int wait);
  int (*pending)(void);
  int (*volume)(int pct);
} kau;

static int
kau_resolve(void)
{
  if (!kau.done) {
    kau.open = (int (*)(int, int, int))k_dl_sym(0, "k_audio_open", 0);
    kau.write = (int (*)(void const *, int))k_dl_sym(0, "k_audio_write", 0);
    kau.close = (int (*)(int))k_dl_sym(0, "k_audio_close", 0);
    kau.pending = (int (*)(void))k_dl_sym(0, "k_audio_pending", 0);
    kau.volume = (int (*)(int))k_dl_sym(0, "k_audio_volume", 0);
    if (!kau.open || !kau.write || !kau.close || !kau.pending || !kau.volume) {
      kau.open = 0;
    }
    kau.done = 1;
  }
  return kau.open != 0;
}

static struct cubeb_ops const sodos_ops;

struct cubeb {
  struct cubeb_ops const * ops;
  pthread_mutex_t lock;
  pthread_t thread;
  int thread_live;
  volatile int quit;
  cubeb_stream * streams;  /* todos os streams criados */

  int dev_open;            /* k_audio_open deu certo */
  int dev_real;            /* e ha placa (nao e o driver nulo) */
  int cut;                 /* pausa: cortar a fila da placa */
  uint64_t dev_written;    /* quadros de 48 kHz misturados desde o inicio */
  uint64_t clock_frames;   /* sem placa: o relogio estava aqui ... */
  uint64_t clock_ns;       /* ... neste instante */
  uint64_t idle_ns;        /* desde quando nada toca */
  uint64_t retry_ns;       /* proxima tentativa de abrir a placa */
  uint64_t check_ns;       /* proxima conferencia de k_audio_volume */

  float mix[SODOS_CHUNK_FRAMES * 2];
  int16_t out[SODOS_CHUNK_FRAMES * 2];
};

struct cubeb_stream {
  cubeb * context;
  cubeb_stream * next;
  void * user_ptr;
  cubeb_data_callback data_cb;
  cubeb_state_callback state_cb;
  cubeb_stream_params params;
  unsigned sample;         /* bytes por amostra: 2 ou 4 */
  unsigned frame_bytes;
  uint8_t * raw;           /* o que o Gecko escreve, no formato do stream */
  long raw_frames;
  float * in;              /* entrada em float estereo, ainda na taxa do stream */
  long in_cap;
  long in_len;
  double pos;              /* posicao fracionaria em in[] */
  double step;             /* rate / 48000 */
  float volume;

  int running;             /* entre start e stop (ou o fim) */
  int busy;                /* o misturador esta num callback dele */
  int ended;               /* o data_callback deu menos que o pedido */
  int error;
  uint64_t frames_in;      /* quadros entregues pelo Gecko, desde o init */
  /* Quadros de 48 kHz deste stream na mistura: segmentos [inicio, +tam),
   * um por start. O anterior pode ainda estar na fila da placa. */
  uint64_t out_done;       /* segmentos mais antigos, ja contados */
  uint64_t old_start, old_len;
  int cur_started;
  uint64_t cur_start, cur_len;
  uint64_t last_pos;       /* a posicao nunca volta */
};

static uint64_t
now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
sleep_ms(unsigned ms)
{
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

/* Relogio de 48 kHz (sem placa). */
static uint64_t
clock_now(cubeb * c, uint64_t t)
{
  return c->clock_frames + (t - c->clock_ns) / 1000ull * SODOS_RATE / 1000000ull;
}

static void
clock_rebase(cubeb * c, uint64_t frames)
{
  c->clock_frames = frames;
  c->clock_ns = now_ns();
}

/* Quadros de 48 kHz que ja sairam (com o lock). */
static uint64_t
dev_played(cubeb * c)
{
  if (c->dev_open && c->dev_real) {
    int p = kau.pending();
    uint64_t q = p > 0 ? (uint64_t)(p / 4) : 0;
    return c->dev_written > q ? c->dev_written - q : 0;
  } else {
    uint64_t k = clock_now(c, now_ns());
    return k < c->dev_written ? k : c->dev_written;
  }
}

static void
dev_close(cubeb * c)
{
  if (c->dev_open) {
    kau.close(0);
    c->dev_open = 0;
    c->dev_real = 0;
    /* o que ainda estava na fila foi cortado: conta como tocado */
    clock_rebase(c, c->dev_written);
  }
}

static uint64_t
seg_played(uint64_t dp, uint64_t start, uint64_t len)
{
  uint64_t d = dp > start ? dp - start : 0;
  return d < len ? d : len;
}

/* Posicao na taxa do stream (com o lock). */
static uint64_t
stream_pos(cubeb_stream * s)
{
  uint64_t dp = dev_played(s->context);
  uint64_t o = s->out_done + seg_played(dp, s->old_start, s->old_len);
  uint64_t p;
  if (s->cur_started) {
    o += seg_played(dp, s->cur_start, s->cur_len);
  }
  /* cada quadro de saida anda step quadros na entrada */
  p = (uint64_t)((double)o * s->step);
  if (p > s->frames_in) {
    p = s->frames_in;
  }
  if (p < s->last_pos) {
    p = s->last_pos;
  }
  s->last_pos = p;
  return p;
}

/* Abre a placa ou confere se ela continua de verdade (com o lock). */
static void
dev_check(cubeb * c, uint64_t t)
{
  if (!c->dev_open) {
    if (t < c->retry_ns || !kau_resolve()) {
      return;
    }
    if (kau.open(SODOS_RATE, SODOS_CHANNELS, 16) != 0) {
      c->retry_ns = t + SODOS_RETRY_MS * 1000000ull;  /* outro programa */
      return;
    }
    c->dev_open = 1;
    c->dev_real = kau.volume(-1) >= 0;
    c->check_ns = t + 500000000ull;
    clock_rebase(c, c->dev_written);
    return;
  }
  if (t >= c->check_ns) {
    int real = kau.volume(-1) >= 0;
    c->check_ns = t + 500000000ull;
    if (real != c->dev_real) {
      /* SET AUDIO=0 ou =1 no meio: abre de novo (o mesmo dono pode) */
      uint64_t p = dev_played(c);
      c->dev_written = c->dev_written > p ? c->dev_written : p;
      clock_rebase(c, c->dev_written);
      c->dev_real = 0;
      if (kau.open(SODOS_RATE, SODOS_CHANNELS, 16) == 0) {
        c->dev_real = kau.volume(-1) >= 0;
      } else {
        c->dev_open = 0;
        c->retry_ns = t + SODOS_RETRY_MS * 1000000ull;
      }
    }
  }
}

static float
rd_f32(uint8_t const * p, int be)
{
  union {
    uint32_t u;
    float f;
  } v;
  if (be) {
    v.u = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
  } else {
    memcpy(&v.u, p, 4);
  }
  return v.f;
}

static float
rd_s16(uint8_t const * p, int be)
{
  uint16_t u = be ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[0] | p[1] << 8);
  return (float)(int16_t)u * (1.0f / 32768.0f);
}

/* Converte got quadros de raw para o fim de in[] (float estereo). */
static void
raw_to_in(cubeb_stream * s, long got)
{
  int fmt = s->params.format;
  int be = fmt == CUBEB_SAMPLE_S16BE || fmt == CUBEB_SAMPLE_FLOAT32BE;
  int flt = fmt == CUBEB_SAMPLE_FLOAT32LE || fmt == CUBEB_SAMPLE_FLOAT32BE;
  unsigned ch = s->params.channels;
  long k;
  for (k = 0; k < got; k++) {
    uint8_t const * f = s->raw + (size_t)k * s->frame_bytes;
    float l = flt ? rd_f32(f, be) : rd_s16(f, be);
    float r = l;
    if (ch >= 2) {
      r = flt ? rd_f32(f + 4, be) : rd_s16(f + 2, be);
    }
    s->in[2 * (s->in_len + k)] = l;
    s->in[2 * (s->in_len + k) + 1] = r;
  }
  s->in_len += got;
}

/* Pede mais quadros ao Gecko. O lock sai durante o callback; o stop e o
 * destroy esperam busy voltar a 0. Devolve 0, ou -1 se o stream parou. */
static int
pull(cubeb * c, cubeb_stream * s)
{
  long drop = (long)s->pos, want, got;
  if (drop > s->in_len) {
    drop = s->in_len;
  }
  if (drop > 0) {
    memmove(s->in, s->in + 2 * drop, (size_t)(s->in_len - drop) * 2 * sizeof(float));
    s->in_len -= drop;
    s->pos -= (double)drop;
  }
  want = s->in_cap - s->in_len;
  if (want > s->raw_frames) {
    want = s->raw_frames;
  }
  if (want <= 0) {
    return 0;
  }
  s->busy = 1;
  pthread_mutex_unlock(&c->lock);
  got = s->data_cb(s, s->user_ptr, NULL, s->raw, want);
  pthread_mutex_lock(&c->lock);
  s->busy = 0;
  if (got < 0) {
    s->error = 1;
    s->running = 0;
    return -1;
  }
  if (got > want) {
    got = want;
  }
  raw_to_in(s, got);
  s->frames_in += (uint64_t)got;
  if (got < want) {
    s->ended = 1;  /* fim do fluxo */
  }
  return s->running ? 0 : -1;
}

/* Soma ate n quadros de 48 kHz do stream em c->mix. Devolve quantos. */
static long
stream_mix(cubeb * c, cubeb_stream * s, long n)
{
  long k = 0;
  float v = s->volume;
  while (k < n && s->running) {
    long i = (long)s->pos;
    float fr, al, ar, bl, br;
    if (i + 1 >= s->in_len && !s->ended) {
      if (pull(c, s) < 0) {
        break;
      }
      continue;
    }
    if (i >= s->in_len) {
      break;  /* acabou de vez */
    }
    fr = (float)(s->pos - (double)i);
    al = s->in[2 * i];
    ar = s->in[2 * i + 1];
    bl = i + 1 < s->in_len ? s->in[2 * i + 2] : al;
    br = i + 1 < s->in_len ? s->in[2 * i + 3] : ar;
    c->mix[2 * k] += (al + (bl - al) * fr) * v;
    c->mix[2 * k + 1] += (ar + (br - ar) * fr) * v;
    s->pos += s->step;
    k++;
  }
  return k;
}

/* Avisa o Gecko (DRAINED/ERROR) sem o lock; busy segura o destroy. */
static void
notify(cubeb * c, cubeb_stream * s, cubeb_state st)
{
  s->busy = 1;
  pthread_mutex_unlock(&c->lock);
  s->state_cb(s, s->user_ptr, st);
  pthread_mutex_lock(&c->lock);
  s->busy = 0;
}

static void *
sodos_run(void * arg)
{
  cubeb * c = arg;
  pthread_mutex_lock(&c->lock);
  while (!c->quit) {
    cubeb_stream * s;
    uint64_t t = now_ns(), need;
    int any = 0, more = 0;
    long k, n;

    if (c->cut) {
      dev_close(c);  /* um stream destruido sem outro tocando */
      c->cut = 0;
    }
    for (s = c->streams; s; s = s->next) {
      any |= s->running;
    }
    if (!any) {
      if (c->dev_open && t - c->idle_ns > SODOS_IDLE_MS * 1000000ull) {
        dev_close(c);
      }
      pthread_mutex_unlock(&c->lock);
      sleep_ms(SODOS_TICK_MS);
      pthread_mutex_lock(&c->lock);
      continue;
    }
    c->idle_ns = t;
    dev_check(c, t);

    /* Quanto falta para a fila ficar em SODOS_TARGET_MS. */
    if (c->dev_open && c->dev_real) {
      int p = kau.pending();
      uint64_t q = p > 0 ? (uint64_t)(p / 4) : 0, cap;
      if (q > 0) {
        clock_rebase(c, c->dev_written - q);  /* a placa anda: o relogio segue */
      }
      need = q < SODOS_TARGET_FRAMES ? SODOS_TARGET_FRAMES - q : 0;
      /* Teto pelo relogio: se a fila nunca anda (SET AUDIO=0 no meio, antes
       * da conferencia de 500 ms) nao dispara pedindo o video inteiro. */
      cap = clock_now(c, t) + SODOS_TARGET_FRAMES;
      cap = cap > c->dev_written ? cap - c->dev_written : 0;
      if (need > cap) {
        need = cap;
      }
    } else {
      uint64_t k2 = clock_now(c, t);
      if (k2 > c->dev_written) {
        /* atrasou (ou recomecou): o relogio volta para o que foi misturado */
        clock_rebase(c, c->dev_written);
        k2 = c->dev_written;
      }
      need = k2 + SODOS_TARGET_FRAMES - c->dev_written;
    }

    if (need >= SODOS_CHUNK_FRAMES / 4) {
      n = need > SODOS_CHUNK_FRAMES ? SODOS_CHUNK_FRAMES : (long)need;
      more = need > SODOS_CHUNK_FRAMES;
      memset(c->mix, 0, sizeof(float) * 2 * (size_t)n);
      for (s = c->streams; s; s = s->next) {
        long got;
        uint64_t at = c->dev_written;
        if (!s->running) {
          continue;
        }
        got = stream_mix(c, s, n);
        if (got > 0) {
          if (!s->cur_started) {
            s->cur_started = 1;
            s->cur_start = at;
          }
          s->cur_len += (uint64_t)got;
        }
      }
      for (k = 0; k < 2 * n; k++) {
        float x = c->mix[k] * 32768.0f;
        c->out[k] = x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : (int16_t)x;
      }
      if (c->dev_open && c->dev_real) {
        int w;
        pthread_mutex_unlock(&c->lock);
        w = kau.write(c->out, (int)n * 4);
        pthread_mutex_lock(&c->lock);
        if (w < 0) {
          dev_close(c);
          c->retry_ns = now_ns() + SODOS_RETRY_MS * 1000000ull;
        }
      }
      c->dev_written += (uint64_t)n;
    }

    /* Fim do fluxo: DRAINED quando a placa tocou o ultimo quadro. Erro do
     * data_callback: ERROR. */
    for (s = c->streams; s; s = s->next) {
      if (s->error) {
        s->error = 0;
        notify(c, s, CUBEB_STATE_ERROR);
        break;  /* a lista pode ter mudado */
      }
      if (s->running && s->ended && (long)s->pos >= s->in_len &&
          (!s->cur_started || dev_played(c) >= s->cur_start + s->cur_len)) {
        s->running = 0;
        notify(c, s, CUBEB_STATE_DRAINED);
        break;
      }
    }

    if (!more) {
      pthread_mutex_unlock(&c->lock);
      sleep_ms(SODOS_TICK_MS);
      pthread_mutex_lock(&c->lock);
    }
  }
  pthread_mutex_unlock(&c->lock);
  return NULL;
}

/*static*/ int
sodos_init(cubeb ** context, char const * context_name)
{
  cubeb * ctx;
  (void)context_name;
  ctx = calloc(1, sizeof(*ctx));
  if (!ctx) {
    return CUBEB_ERROR;
  }
  ctx->ops = &sodos_ops;
  pthread_mutex_init(&ctx->lock, NULL);
  clock_rebase(ctx, 0);
  *context = ctx;
  return CUBEB_OK;
}

static char const *
sodos_get_backend_id(cubeb * context)
{
  (void)context;
  return "sodos";
}

static int
sodos_get_max_channel_count(cubeb * context, uint32_t * max_channels)
{
  (void)context;
  *max_channels = SODOS_CHANNELS;
  return CUBEB_OK;
}

static int
sodos_get_min_latency(cubeb * context, cubeb_stream_params params,
                      uint32_t * latency_frames)
{
  (void)context;
  *latency_frames = params.rate * SODOS_TARGET_MS / 1000;
  return CUBEB_OK;
}

static int
sodos_get_preferred_sample_rate(cubeb * context, uint32_t * rate)
{
  (void)context;
  *rate = SODOS_RATE;
  return CUBEB_OK;
}

static void
sodos_destroy(cubeb * context)
{
  if (context->thread_live) {
    pthread_mutex_lock(&context->lock);
    context->quit = 1;
    pthread_mutex_unlock(&context->lock);
    pthread_join(context->thread, NULL);
    context->thread_live = 0;
  }
  pthread_mutex_lock(&context->lock);
  dev_close(context);
  pthread_mutex_unlock(&context->lock);
  pthread_mutex_destroy(&context->lock);
  free(context);
}

static int
sodos_stream_stop(cubeb_stream * s);

static void
sodos_stream_destroy(cubeb_stream * s)
{
  cubeb * c;
  cubeb_stream ** pp;
  if (!s) {
    return;
  }
  c = s->context;
  sodos_stream_stop(s);
  pthread_mutex_lock(&c->lock);
  while (s->busy) {  /* um DRAINED/ERROR ainda saindo */
    pthread_mutex_unlock(&c->lock);
    sleep_ms(1);
    pthread_mutex_lock(&c->lock);
  }
  for (pp = &c->streams; *pp; pp = &(*pp)->next) {
    if (*pp == s) {
      *pp = s->next;
      break;
    }
  }
  {
    /* som velho deste stream na fila (seek, aba fechada): sem outro
     * tocando, o misturador corta */
    cubeb_stream * o;
    int others = 0;
    for (o = c->streams; o; o = o->next) {
      others |= o->running;
    }
    if (!others) {
      c->cut = 1;
    }
  }
  pthread_mutex_unlock(&c->lock);
  free(s->raw);
  free(s->in);
  free(s);
}

static int
sodos_stream_init(cubeb * context, cubeb_stream ** stream,
                  char const * stream_name, cubeb_devid input_device,
                  cubeb_stream_params * input_stream_params,
                  cubeb_devid output_device,
                  cubeb_stream_params * output_stream_params,
                  unsigned int latency_frames,
                  cubeb_data_callback data_callback,
                  cubeb_state_callback state_callback, void * user_ptr)
{
  cubeb_stream * s;
  unsigned sample;
  (void)stream_name;
  (void)input_device;
  (void)output_device;
  (void)latency_frames;

  /* Sem microfone. */
  if (input_stream_params || !output_stream_params) {
    return CUBEB_ERROR_NOT_SUPPORTED;
  }
  switch (output_stream_params->format) {
  case CUBEB_SAMPLE_S16LE:
  case CUBEB_SAMPLE_S16BE:
    sample = 2;
    break;
  case CUBEB_SAMPLE_FLOAT32LE:
  case CUBEB_SAMPLE_FLOAT32BE:
    sample = 4;
    break;
  default:
    return CUBEB_ERROR_INVALID_FORMAT;
  }
  if (output_stream_params->rate < 1000 || output_stream_params->rate > 384000 ||
      output_stream_params->channels < 1 || output_stream_params->channels > 32) {
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  s = calloc(1, sizeof(*s));
  if (!s) {
    return CUBEB_ERROR;
  }
  s->context = context;
  s->user_ptr = user_ptr;
  s->data_cb = data_callback;
  s->state_cb = state_callback;
  s->params = *output_stream_params;
  s->sample = sample;
  s->frame_bytes = sample * output_stream_params->channels;
  s->raw_frames = (long)output_stream_params->rate * SODOS_PULL_MS / 1000 + 16;
  s->raw = calloc((size_t)s->raw_frames, s->frame_bytes);
  s->in_cap = 2 * s->raw_frames + 16;
  s->in = calloc((size_t)s->in_cap, 2 * sizeof(float));
  if (!s->raw || !s->in) {
    free(s->raw);
    free(s->in);
    free(s);
    return CUBEB_ERROR;
  }
  s->step = (double)output_stream_params->rate / (double)SODOS_RATE;
  s->volume = 1.0f;
  pthread_mutex_lock(&context->lock);
  s->next = context->streams;
  context->streams = s;
  pthread_mutex_unlock(&context->lock);
  *stream = s;
  return CUBEB_OK;
}

static int
sodos_stream_start(cubeb_stream * s)
{
  cubeb * c = s->context;
  pthread_mutex_lock(&c->lock);
  if (s->running) {
    pthread_mutex_unlock(&c->lock);
    return CUBEB_OK;
  }
  /* Recomeca de onde parou: a entrada que sobrou (in[], pos) continua e um
   * segmento novo comeca na mistura. O anterior pode ainda estar tocando. */
  if (s->cur_started) {
    s->out_done += s->old_len;  /* dois starts em ~80 ms: o mais velho ja conta */
    s->old_start = s->cur_start;
    s->old_len = s->cur_len;
    s->cur_started = 0;
    s->cur_len = 0;
  }
  s->ended = 0;
  s->error = 0;
  s->running = 1;
  if (!c->thread_live) {
    c->quit = 0;
    if (pthread_create(&c->thread, NULL, sodos_run, c) != 0) {
      s->running = 0;
      pthread_mutex_unlock(&c->lock);
      return CUBEB_ERROR;
    }
    c->thread_live = 1;
  }
  pthread_mutex_unlock(&c->lock);
  s->state_cb(s, s->user_ptr, CUBEB_STATE_STARTED);
  return CUBEB_OK;
}

static int
sodos_stream_stop(cubeb_stream * s)
{
  cubeb * c = s->context;
  int was;
  pthread_mutex_lock(&c->lock);
  /* nenhum data_callback depois que o stop volta */
  while (s->busy) {
    pthread_mutex_unlock(&c->lock);
    sleep_ms(1);
    pthread_mutex_lock(&c->lock);
  }
  was = s->running;
  s->running = 0;  /* o que ja esta na fila da placa toca (nada se perde) */
  pthread_mutex_unlock(&c->lock);
  if (was) {
    s->state_cb(s, s->user_ptr, CUBEB_STATE_STOPPED);
  }
  return CUBEB_OK;
}

static int
sodos_stream_get_position(cubeb_stream * s, uint64_t * position)
{
  pthread_mutex_lock(&s->context->lock);
  *position = stream_pos(s);  /* parado, a fila ainda pode estar tocando */
  pthread_mutex_unlock(&s->context->lock);
  return CUBEB_OK;
}

static int
sodos_stream_get_latency(cubeb_stream * s, uint32_t * latency)
{
  *latency = s->params.rate * SODOS_TARGET_MS / 1000;
  return CUBEB_OK;
}

static int
sodos_stream_set_volume(cubeb_stream * s, float volume)
{
  pthread_mutex_lock(&s->context->lock);
  s->volume = volume < 0.0f ? 0.0f : volume > 1.0f ? 1.0f : volume;
  pthread_mutex_unlock(&s->context->lock);
  return CUBEB_OK;
}

static struct cubeb_ops const sodos_ops = {
    .init = sodos_init,
    .get_backend_id = sodos_get_backend_id,
    .get_max_channel_count = sodos_get_max_channel_count,
    .get_min_latency = sodos_get_min_latency,
    .get_preferred_sample_rate = sodos_get_preferred_sample_rate,
    .get_supported_input_processing_params = NULL,
    .enumerate_devices = NULL,
    .device_collection_destroy = NULL,
    .destroy = sodos_destroy,
    .stream_init = sodos_stream_init,
    .stream_destroy = sodos_stream_destroy,
    .stream_start = sodos_stream_start,
    .stream_stop = sodos_stream_stop,
    .stream_get_position = sodos_stream_get_position,
    .stream_get_latency = sodos_stream_get_latency,
    .stream_get_input_latency = NULL,
    .stream_set_volume = sodos_stream_set_volume,
    .stream_set_name = NULL,
    .stream_get_current_device = NULL,
    .stream_set_input_mute = NULL,
    .stream_set_input_processing_params = NULL,
    .stream_device_destroy = NULL,
    .stream_register_device_changed_callback = NULL,
    .register_device_collection_changed = NULL};
