; ap_blob.asm - embute o trampolim (binario cru) no kernel.
; O BSP copia ap_tramp_blob_start..end para 0xA000 antes do INIT-SIPI.
bits 64
section .rodata
global ap_tramp_blob_start
global ap_tramp_blob_end
ap_tramp_blob_start:
incbin "kernel/ap_tramp.bin"
ap_tramp_blob_end:
