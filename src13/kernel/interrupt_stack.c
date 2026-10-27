/* interrupt_stack.c - Definição das stacks de interrupção */
#include "interrupt_stack.h"
#include <stdint.h>

uint8_t irq_stack[4096] __attribute__((aligned(16)));
uint8_t fault_stack[4096] __attribute__((aligned(16)));

uint32_t irq_stack_top(void) {
  return (uint32_t)(irq_stack + sizeof(irq_stack));
}
uint32_t fault_stack_top(void) {
  return (uint32_t)(fault_stack + sizeof(fault_stack));
}
