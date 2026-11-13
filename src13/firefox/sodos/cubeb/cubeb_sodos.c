/*
 * cubeb_sodos.c - saida de audio nula do SODOS.
 *
 * Ainda nao ha placa de som: este backend so faz o papel do relogio. Uma
 * thread por stream pede ao Gecko (data_callback) as amostras no ritmo do
 * tempo real e as descarta. Assim o MediaDecoderStateMachine ve o audio
 * "tocando", a posicao anda e o video sincroniza com ela (YouTube, <audio>).
 *
 * Entra no cubeb.c pelo patch 0013 (USE_SODOS); build-browser64.sh copia este
 * arquivo para media/libcubeb/src. Para ter som de verdade, troque o descarte
 * em sodos_run por uma escrita no driver e mantenha o resto.
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
#define SODOS_LATENCY_MS 20
/* No maximo 100 ms por chamada: se a thread atrasar (CPU ocupada) ela pede o
 * atraso em pedacos, sem um buffer enorme. */
#define SODOS_CHUNK_MS 100

static struct cubeb_ops const sodos_ops;

struct cubeb {
  struct cubeb_ops const * ops;
};

struct cubeb_stream {
  cubeb * context;
  void * user_ptr;
  cubeb_data_callback data_cb;
  cubeb_state_callback state_cb;
  cubeb_stream_params params;
  unsigned frame_bytes;
  uint8_t * buf;
  long buf_frames;

  pthread_mutex_t lock;
  pthread_t thread;
  int thread_live;
  volatile int running;

  /* Quadros entregues pelo Gecko e quadros "tocados" (pelo relogio). */
  uint64_t written;
  uint64_t played;
  uint64_t base_ns;   /* instante em que played seria 0 */
  int drained;
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

static uint64_t
clock_frames(cubeb_stream * s)
{
  uint64_t el = now_ns() - s->base_ns;
  return el / 1000000ull * s->params.rate / 1000ull;
}

static void *
sodos_run(void * arg)
{
  cubeb_stream * s = arg;
  while (s->running) {
    uint64_t target;
    long want, got;

    pthread_mutex_lock(&s->lock);
    target = clock_frames(s);
    /* O que ja foi entregue e o relogio passou conta como tocado. */
    s->played = target < s->written ? target : s->written;
    /* Mantem SODOS_LATENCY_MS entregues a frente do relogio. */
    target += (uint64_t)s->params.rate * SODOS_LATENCY_MS / 1000;
    want = target > s->written ? (long)(target - s->written) : 0;
    if (want > s->buf_frames) {
      want = s->buf_frames;
    }
    pthread_mutex_unlock(&s->lock);

    if (want > 0 && !s->drained) {
      got = s->data_cb(s, s->user_ptr, NULL, s->buf, want);
      if (got < 0) {
        s->running = 0;
        s->state_cb(s, s->user_ptr, CUBEB_STATE_ERROR);
        break;
      }
      pthread_mutex_lock(&s->lock);
      s->written += (uint64_t)got;
      pthread_mutex_unlock(&s->lock);
      if (got < want) {
        /* Fim do fluxo: espera o relogio chegar ao ultimo quadro. */
        s->drained = 1;
      }
    }

    if (s->drained) {
      int done;
      pthread_mutex_lock(&s->lock);
      done = clock_frames(s) >= s->written;
      if (done) {
        s->played = s->written;
      }
      pthread_mutex_unlock(&s->lock);
      if (done) {
        s->running = 0;
        s->state_cb(s, s->user_ptr, CUBEB_STATE_DRAINED);
        break;
      }
    }
    sleep_ms(SODOS_TICK_MS);
  }
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
  *context = ctx;
  return CUBEB_OK;
}

static char const *
sodos_get_backend_id(cubeb * context)
{
  (void)context;
  return "sodos-null";
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
  *latency_frames = params.rate * SODOS_LATENCY_MS / 1000;
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
  free(context);
}

static int
sodos_stream_stop(cubeb_stream * s);

static void
sodos_stream_destroy(cubeb_stream * s)
{
  if (!s) {
    return;
  }
  sodos_stream_stop(s);
  pthread_mutex_destroy(&s->lock);
  free(s->buf);
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
  s->frame_bytes = sample * output_stream_params->channels;
  s->buf_frames = (long)output_stream_params->rate * SODOS_CHUNK_MS / 1000;
  s->buf = calloc((size_t)s->buf_frames, s->frame_bytes);
  if (!s->buf) {
    free(s);
    return CUBEB_ERROR;
  }
  pthread_mutex_init(&s->lock, NULL);
  *stream = s;
  return CUBEB_OK;
}

static int
sodos_stream_start(cubeb_stream * s)
{
  if (s->thread_live) {
    return CUBEB_OK;
  }
  /* Recomeca de onde parou: o relogio volta a contar a partir de played. */
  pthread_mutex_lock(&s->lock);
  s->written = s->played;
  s->base_ns = now_ns() - s->played * 1000000000ull / s->params.rate;
  s->drained = 0;
  pthread_mutex_unlock(&s->lock);
  s->running = 1;
  if (pthread_create(&s->thread, NULL, sodos_run, s) != 0) {
    s->running = 0;
    return CUBEB_ERROR;
  }
  s->thread_live = 1;
  s->state_cb(s, s->user_ptr, CUBEB_STATE_STARTED);
  return CUBEB_OK;
}

static int
sodos_stream_stop(cubeb_stream * s)
{
  int was = s->running;
  if (!s->thread_live) {
    return CUBEB_OK;
  }
  s->running = 0;
  pthread_join(s->thread, NULL);
  s->thread_live = 0;
  if (was) {
    s->state_cb(s, s->user_ptr, CUBEB_STATE_STOPPED);
  }
  return CUBEB_OK;
}

static int
sodos_stream_get_position(cubeb_stream * s, uint64_t * position)
{
  pthread_mutex_lock(&s->lock);
  if (s->running) {
    uint64_t c = clock_frames(s);
    s->played = c < s->written ? c : s->written;
  }
  *position = s->played;
  pthread_mutex_unlock(&s->lock);
  return CUBEB_OK;
}

static int
sodos_stream_get_latency(cubeb_stream * s, uint32_t * latency)
{
  *latency = s->params.rate * SODOS_LATENCY_MS / 1000;
  return CUBEB_OK;
}

static int
sodos_stream_set_volume(cubeb_stream * s, float volume)
{
  (void)s;
  (void)volume;
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
