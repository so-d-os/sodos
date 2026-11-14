; modrun.asm - chama um programa 64 bits (SysV) com pilha propria.
; int mod_call_entry(void* fn, int argc, char** argv, void* stack_top)
; void mod_exit(int code)
; void ctx_switch(uint64_t* save_rsp, uint64_t next_rsp)
; Programas SOX de 32 bits nao executam neste kernel.
;
; Cada chamada guarda o proprio rsp em cur_ctx (moldura ligada), nao numa
; pilha pelo nivel. Um programa pode terminar antes de outro que comecou
; depois: o cur_ctx de quem esta parado fica salvo na tarefa.
;
; mod_exit pode vir do meio de uma chamada ao kernel (exit, Ctrl+C), sem
; passar pelos epilogos do programa: por isso r13-r15 tambem sao salvos aqui.
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
  push r13
  push r14
  push r15
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
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret
mod_full:
  mov rsp, [rel cur_ctx]
  pop r12
  mov [rel cur_ctx], r12
  pop r15
  pop r14
  pop r13
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

; ---------------------------------------------------------------------------
; bkl_thunk_common: toda funcao k_* chamada por um programa passa por aqui
; (kexports.c gera um thunk "mov r11, alvo; jmp bkl_thunk_common" por
; export). Pega o BKL (kt_bkl_enter), chama o alvo e solta (kt_bkl_leave).
;
; O endereco de retorno fica na pilha de quem chamou (nada de pilha lateral):
; as tarefas do WIN trocam de pilha no meio de um k_* e o mod_exit pode
; abandonar o quadro. Os argumentos de pilha (7o em diante) sao copiados
; (ate 10) para o quadro novo, no mesmo lugar relativo ao rsp.
; rax (al = numero de registros vetoriais em varargs) e preservado; rax:rdx
; da volta tambem.
; ---------------------------------------------------------------------------
global bkl_thunk_common
extern kt_bkl_enter
extern kt_bkl_leave

bkl_thunk_common:
  push rbp
  mov rbp, rsp            ; [rbp+8] = retorno, [rbp+16..] = args de pilha
  push rdi
  push rsi
  push rdx
  push rcx
  push r8
  push r9
  push rax
  push r11                ; rsp = rbp - 64 (rbp % 16 == 0)
  call kt_bkl_enter
  pop r11
  pop rax
  pop r9
  pop r8
  pop rcx
  pop rdx
  pop rsi
  pop rdi
  sub rsp, 80
  mov r10, [rbp + 16]
  mov [rsp], r10
  mov r10, [rbp + 24]
  mov [rsp + 8], r10
  mov r10, [rbp + 32]
  mov [rsp + 16], r10
  mov r10, [rbp + 40]
  mov [rsp + 24], r10
  mov r10, [rbp + 48]
  mov [rsp + 32], r10
  mov r10, [rbp + 56]
  mov [rsp + 40], r10
  mov r10, [rbp + 64]
  mov [rsp + 48], r10
  mov r10, [rbp + 72]
  mov [rsp + 56], r10
  mov r10, [rbp + 80]
  mov [rsp + 64], r10
  mov r10, [rbp + 88]
  mov [rsp + 72], r10
  call r11
  mov rsp, rbp
  push rax
  push rdx                ; rsp = rbp - 16
  call kt_bkl_leave
  pop rdx
  pop rax
  pop rbp
  ret
