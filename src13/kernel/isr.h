#pragma once
#include <stdint.h>

/* Frame montado por isr_common (long mode). Endereco baixo = primeiro campo. */
typedef struct isr_frame {
  uint64_t r11;
  uint64_t r10;
  uint64_t r9;
  uint64_t r8;
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

/* entry 0: volta normal. Senao, a entrada de falhas do programa
 * (k_fault_entry): o isr_common pula para ela com rdi = frame, rsi = cr2.
 * 16 bytes: volta em rax:rdx (SysV). */
typedef struct {
  uint64_t entry;
  uint64_t cr2;
} isr_ret_t;
isr_ret_t isr_dispatch(isr_frame_t* f);
/* k_fault_entry: o programa corrente recebe as suas excecoes (#DE, #UD, #GP,
 * #PF, #MF, #AC, #XM) em entry em vez da tela de EXCEPTION. 0 desliga. */
int isr_fault_set(uint64_t entry);
/* Fim do processo: esquece a entrada de falhas dele. */
void isr_fault_release(uint32_t pid);
/* Double Fault: escreve em VGA e trava; nao retorna */
void isr_df_panic(isr_frame_t* f);
