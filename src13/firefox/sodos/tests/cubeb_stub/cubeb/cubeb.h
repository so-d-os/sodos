/* cubeb.h minimo para testar sodos/cubeb/cubeb_sodos.c no Linux
 * (sodos/tests/t_cubeb_sodos.c). Mesmos nomes do cubeb do Firefox 153. */
#ifndef CUBEB_STUB_H
#define CUBEB_STUB_H
#include <stdint.h>

typedef struct cubeb cubeb;
typedef struct cubeb_stream cubeb_stream;
typedef void const * cubeb_devid;
typedef uint32_t cubeb_channel_layout;

typedef enum {
  CUBEB_SAMPLE_S16LE,
  CUBEB_SAMPLE_S16BE,
  CUBEB_SAMPLE_FLOAT32LE,
  CUBEB_SAMPLE_FLOAT32BE
} cubeb_sample_format;

typedef enum { CUBEB_STREAM_PREF_NONE = 0 } cubeb_stream_prefs;

typedef struct {
  cubeb_sample_format format;
  uint32_t rate;
  uint32_t channels;
  cubeb_channel_layout layout;
  cubeb_stream_prefs prefs;
} cubeb_stream_params;

typedef enum {
  CUBEB_STATE_STARTED,
  CUBEB_STATE_STOPPED,
  CUBEB_STATE_DRAINED,
  CUBEB_STATE_ERROR
} cubeb_state;

enum {
  CUBEB_OK = 0,
  CUBEB_ERROR = -1,
  CUBEB_ERROR_INVALID_FORMAT = -2,
  CUBEB_ERROR_INVALID_PARAMETER = -3,
  CUBEB_ERROR_NOT_SUPPORTED = -4,
  CUBEB_ERROR_DEVICE_UNAVAILABLE = -5
};

typedef long (*cubeb_data_callback)(cubeb_stream * stream, void * user_ptr,
                                    void const * input_buffer,
                                    void * output_buffer, long nframes);
typedef void (*cubeb_state_callback)(cubeb_stream * stream, void * user_ptr,
                                     cubeb_state state);
#endif
