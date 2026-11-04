/* sodos_time.c - relogio monotonico de 64 bits e hora de parede.
 *
 * k_ms() e um uint32 que volta a zero a cada ~49,7 dias e so anda quando o
 * PIT e lido. Aqui mantemos a parte alta para o Gecko nunca ver o tempo
 * andar para tras (TimeStamp, timers do NSPR, cache de DNS...).
 */
#include <so.h>
#include <time.h>

#include "sodos.h"
#include "sodos_internal.h"

uint64_t sodos__ms(void) {
  static uint32_t last_raw;
  static uint64_t high;
  uint32_t raw = (uint32_t)k_ms();
  if (raw < last_raw) high += 0x100000000ULL;
  last_raw = raw;
  return high + raw;
}

uint64_t sodos_now_ms(void) { return sodos__ms(); }

uint64_t sodos_wall_ms(void) {
  time_t sec = time(NULL);
  uint32_t raw = (uint32_t)k_ms();
  if (sec <= 0) return sodos__ms();
  return (uint64_t)sec * 1000ULL + (raw % 1000u);
}

void sodos_yield(void) { sodos__yield(); }

void sodos_sleep_ms(unsigned ms) { sodos__sleep_ms(ms); }
