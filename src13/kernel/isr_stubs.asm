; isr_stubs.asm - excecoes 0..31 em long mode.
; Frame (endereco baixo primeiro): rdi, rsi, rbp, rbx, rdx, rcx, rax,
;   int_no, err_code, rip, cs, rflags, rsp, ss.
; Excecoes COM error code: 8, 10, 11, 12, 13, 14, 17.

bits 64
section .text
extern isr_dispatch

%macro ISR_NOERR 1
global isr%1_stub
isr%1_stub:
  push qword 0
  push qword %1
  jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1_stub
isr%1_stub:
  push qword %1
  jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

global isr_common
isr_common:
  push rax
  push rcx
  push rdx
  push rbx
  push rbp
  push rsi
  push rdi
  mov rdi, rsp
  mov rbx, rsp
  and rsp, -16
  call isr_dispatch
  mov rsp, rbx
  pop rdi
  pop rsi
  pop rbp
  pop rbx
  pop rdx
  pop rcx
  pop rax
  add rsp, 16
  iretq
