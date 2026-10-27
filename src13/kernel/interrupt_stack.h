/* interrupt_stack.h - Stacks separadas para IRQ e exceções */
#pragma once
#include <stdint.h>

extern uint8_t irq_stack[4096];
extern uint8_t fault_stack[4096];

/* Retornam o endereço do topo (primeiro byte acima da stack). Chamados pelo assembly. */
uint32_t irq_stack_top(void);
uint32_t fault_stack_top(void);
