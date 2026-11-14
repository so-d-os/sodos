; irq_stubs.asm - IRQ 0..15 (vetores 32..47) em long mode.
; Mesmo frame que isr_stubs.asm. EOI fica no C.

bits 64
section .text
extern isr_common

%macro IRQ_STUB 1
global irq%1_stub
irq%1_stub:
  push qword 0
  push qword (32 + %1)
  jmp isr_common
%endmacro

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

; IPI do SMP: o BSP acorda um AP (ou o AP avisa que a tarefa terminou).
; Vetor 0xF1; o EOI do LAPIC fica no C (smp_ipi_ack).
global smp_ipi_stub
smp_ipi_stub:
  push qword 0
  push qword 0xF1
  jmp isr_common

; Escalonador SMP (kthread.c): timer local, reschedule e TLB. EOI no C.
global lapic_timer_stub
lapic_timer_stub:
  push qword 0
  push qword 0xF2
  jmp isr_common

global resched_ipi_stub
resched_ipi_stub:
  push qword 0
  push qword 0xF3
  jmp isr_common

global tlb_ipi_stub
tlb_ipi_stub:
  push qword 0
  push qword 0xF4
  jmp isr_common

; Interrupcao espuria do LAPIC: nao leva EOI.
global spurious_stub
spurious_stub:
  iretq
