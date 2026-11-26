/* cubeb-internal.h minimo (so o struct cubeb_ops) para o teste do
 * cubeb_sodos.c no Linux. Os campos que o backend deixa NULL sao void*. */
#ifndef CUBEB_INTERNAL_STUB_H
#define CUBEB_INTERNAL_STUB_H
#include "cubeb/cubeb.h"

struct cubeb_ops {
  int (*init)(cubeb ** context, char const * context_name);
  char const * (*get_backend_id)(cubeb * context);
  int (*get_max_channel_count)(cubeb * context, uint32_t * max_channels);
  int (*get_min_latency)(cubeb * context, cubeb_stream_params params,
                         uint32_t * latency_ms);
  int (*get_preferred_sample_rate)(cubeb * context, uint32_t * rate);
  void * get_supported_input_processing_params;
  void * enumerate_devices;
  void * device_collection_destroy;
  void (*destroy)(cubeb * context);
  int (*stream_init)(cubeb * context, cubeb_stream ** stream,
                     char const * stream_name, cubeb_devid input_device,
                     cubeb_stream_params * input_stream_params,
                     cubeb_devid output_device,
                     cubeb_stream_params * output_stream_params,
                     unsigned int latency, cubeb_data_callback data_callback,
                     cubeb_state_callback state_callback, void * user_ptr);
  void (*stream_destroy)(cubeb_stream * stream);
  int (*stream_start)(cubeb_stream * stream);
  int (*stream_stop)(cubeb_stream * stream);
  int (*stream_get_position)(cubeb_stream * stream, uint64_t * position);
  int (*stream_get_latency)(cubeb_stream * stream, uint32_t * latency);
  void * stream_get_input_latency;
  int (*stream_set_volume)(cubeb_stream * stream, float volumes);
  void * stream_set_name;
  void * stream_get_current_device;
  void * stream_set_input_mute;
  void * stream_set_input_processing_params;
  void * stream_device_destroy;
  void * stream_register_device_changed_callback;
  void * register_device_collection_changed;
};
#endif
