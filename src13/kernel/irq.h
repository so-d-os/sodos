/* irq.h - Tratamento de IRQ (32..47), EOI */
#pragma once
#include <stdint.h>
#include "isr.h"

void irq_handle(isr_frame_t* f);
/* Ticks do IRQ0 (PIT), contados desde o boot. O IRQ0 e desmascarado no kmain. */
uint64_t irq_ticks(void);
