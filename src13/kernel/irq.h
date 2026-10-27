/* irq.h - Tratamento de IRQ (32..47), EOI */
#pragma once
#include <stdint.h>
#include "isr.h"

void irq_handle(isr_frame_t* f);
