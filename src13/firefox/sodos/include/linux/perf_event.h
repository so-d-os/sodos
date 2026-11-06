/* linux/perf_event.h (SODOS) - o kernel nao tem contadores de desempenho. */
#ifndef _LINUX_PERF_EVENT_H
#define _LINUX_PERF_EVENT_H
#include <stdint.h>
#include <linux/unistd.h>
#define PERF_FORMAT_TOTAL_TIME_ENABLED 1
#define PERF_FORMAT_TOTAL_TIME_RUNNING 2
#define PERF_EVENT_IOC_ENABLE 0x2400
#define PERF_EVENT_IOC_DISABLE 0x2401
struct perf_event_attr {
  uint32_t type, size;
  uint64_t config, sample_period, sample_type, read_format;
  uint64_t disabled : 1, inherit : 1, pad : 62;
};
/* lote3 */
#define PERF_SAMPLE_IDENTIFIER (1U << 16)
#define PERF_FLAG_FD_CLOEXEC 8

#endif
