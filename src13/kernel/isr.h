#pragma once
#include <stdint.h>

/* Frame montado por isr_common (long mode). Endereco baixo = primeiro campo. */
typedef struct isr_frame {
  uint64_t rdi;
  uint64_t rsi;
  uint64_t ebp;
  uint64_t rbx;
  uint64_t rdx;
  uint64_t rcx;
  uint64_t rax;
  uint64_t int_no;
  uint64_t err_code;
  uint64_t eip;
  uint64_t cs;
  uint64_t eflags;
  uint64_t esp_dummy;
  uint64_t ss;
} __attribute__((packed)) isr_frame_t;

void isr_dispatch(isr_frame_t* f);
/* Double Fault: escreve em VGA e trava; nao retorna */
void isr_df_panic(isr_frame_t* f);
