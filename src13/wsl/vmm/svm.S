/* svm.S - Entra na VM (VMRUN) e volta. Ver struct vcpu_regs em vmm.h.
 *
 * Com GIF=0 (CLGI) do comeco ao fim: nenhuma interrupcao do host entra no
 * meio da troca de FPU/XCR0 nem entre o VMLOAD e o VMRUN. A interrupcao que
 * fez a VM sair (INTR) e atendida pelo SODOS logo depois do STGI.
 *
 * VMRUN guarda/restaura so parte do estado do host (RSP, RAX, RFLAGS, CRs,
 * EFER, segmentos basicos). FS/GS/TR/LDTR, KernelGSBase, STAR/LSTAR e
 * SYSENTER vao pelo VMSAVE/VMLOAD numa pagina do host; os registradores
 * gerais da VM ficam em regs->gpr; a FPU/SSE/AVX de cada lado em areas
 * XSAVE proprias (a VM pode ter outro XCR0). */
	.text
	.globl svm_enter
	.type svm_enter, @function
svm_enter:
	push %rbx
	push %rbp
	push %r12
	push %r13
	push %r14
	push %r15
	push %rdi
	clgi

	cmpq $0, 176(%rdi)
	je 1f
	mov 152(%rdi), %r8
	mov $-1, %eax
	mov $-1, %edx
	xsave64 (%r8)
	mov 160(%rdi), %rax
	mov %rax, %rdx
	shr $32, %rdx
	xor %ecx, %ecx
	xsetbv
	mov 144(%rdi), %r8
	mov $-1, %eax
	mov $-1, %edx
	xrstor64 (%r8)
	jmp 2f
1:	mov 152(%rdi), %r8
	fxsave64 (%r8)
	mov 144(%rdi), %r8
	fxrstor64 (%r8)
2:
	mov 136(%rdi), %rax
	vmsave %rax
	mov 128(%rdi), %rax
	vmload %rax
	mov 8(%rdi), %rcx
	mov 16(%rdi), %rdx
	mov 24(%rdi), %rbx
	mov 40(%rdi), %rbp
	mov 48(%rdi), %rsi
	mov 64(%rdi), %r8
	mov 72(%rdi), %r9
	mov 80(%rdi), %r10
	mov 88(%rdi), %r11
	mov 96(%rdi), %r12
	mov 104(%rdi), %r13
	mov 112(%rdi), %r14
	mov 120(%rdi), %r15
	mov 56(%rdi), %rdi
	vmrun %rax

	/* #VMEXIT: rsp/rax/rflags do host de volta; o resto e da VM */
	push %rdi
	mov 8(%rsp), %rdi
	mov %rcx, 8(%rdi)
	mov %rdx, 16(%rdi)
	mov %rbx, 24(%rdi)
	mov %rbp, 40(%rdi)
	mov %rsi, 48(%rdi)
	mov %r8, 64(%rdi)
	mov %r9, 72(%rdi)
	mov %r10, 80(%rdi)
	mov %r11, 88(%rdi)
	mov %r12, 96(%rdi)
	mov %r13, 104(%rdi)
	mov %r14, 112(%rdi)
	mov %r15, 120(%rdi)
	pop %rcx
	mov %rcx, 56(%rdi)
	mov 128(%rdi), %rax
	vmsave %rax
	mov 136(%rdi), %rax
	vmload %rax

	cmpq $0, 176(%rdi)
	je 3f
	mov 144(%rdi), %r8
	mov $-1, %eax
	mov $-1, %edx
	xsave64 (%r8)
	mov 168(%rdi), %rax
	mov %rax, %rdx
	shr $32, %rdx
	xor %ecx, %ecx
	xsetbv
	mov 152(%rdi), %r8
	mov $-1, %eax
	mov $-1, %edx
	xrstor64 (%r8)
	jmp 4f
3:	mov 144(%rdi), %r8
	fxsave64 (%r8)
	mov 152(%rdi), %r8
	fxrstor64 (%r8)
4:
	stgi
	pop %rdi
	pop %r15
	pop %r14
	pop %r13
	pop %r12
	pop %rbp
	pop %rbx
	ret
	.size svm_enter, .-svm_enter
	.section .note.GNU-stack,"",@progbits
