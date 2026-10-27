/* trace.h - lightweight kernel trace buffer (freestanding) */
#ifndef TRACE_H
#define TRACE_H

#include <stdint.h>

void trace_event(const char* tag, uint32_t a, uint32_t b, uint32_t c);
void trace_dump_vga(int row);

static inline uint32_t trace_get_esp(void) {
  uint64_t v;
  __asm__ volatile ("mov %%rsp, %0" : "=r"(v));
  return (uint32_t)v;
}

static inline uint32_t trace_get_ebp(void) {
  uint64_t v;
  __asm__ volatile ("mov %%rbp, %0" : "=r"(v));
  return (uint32_t)v;
}

#define TRACE_FUNC(tag) \
  trace_event((tag), (uint32_t)__builtin_return_address(0), trace_get_esp(), trace_get_ebp())

#endif
