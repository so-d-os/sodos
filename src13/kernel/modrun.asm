; modrun.asm - chama um programa 64 bits (SysV) com pilha propria.
; int mod_call_entry(void* fn, int argc, char** argv, void* stack_top)
; void mod_exit(int code)
; void ctx_switch(uint64_t* save_rsp, uint64_t next_rsp)
; Programas SOX de 32 bits nao executam neste kernel.
;
; Cada chamada guarda o proprio rsp em cur_ctx (moldura ligada), nao numa
; pilha pelo nivel. Um programa pode terminar antes de outro que comecou
; depois: o cur_ctx de quem esta parado fica salvo na tarefa.
bits 64

global mod_call_entry
global mod_exit
global mod_depth
global cur_ctx
global ctx_switch

section .bss
mod_depth: resd 1
cur_ctx: resq 1

section .text
mod_call_entry:
  push rbx
  push rbp
  push r12
  push qword [rel cur_ctx]
  mov [rel cur_ctx], rsp
  mov eax, [rel mod_depth]
  cmp eax, 16
  jae mod_full
  inc dword [rel mod_depth]
  mov rbx, rdi
  mov r10, rcx
  mov rdi, rsi
  mov rsi, rdx
  mov rsp, r10
  and rsp, -16
  call rbx
mod_ret:
  dec dword [rel mod_depth]
  mov rsp, [rel cur_ctx]
  pop r12
  mov [rel cur_ctx], r12
  pop r12
  pop rbp
  pop rbx
  ret
mod_full:
  mov rsp, [rel cur_ctx]
  pop r12
  mov [rel cur_ctx], r12
  pop r12
  pop rbp
  pop rbx
  mov eax, -1
  ret

mod_exit:
  mov eax, edi
  jmp mod_ret

; Troca de pilha cooperativa. Salva rbx, rbp e r12-r15.
ctx_switch:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  mov [rdi], rsp
  mov rsp, rsi
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret
