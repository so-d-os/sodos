@echo off
rem Boota um kernel multiboot (ELF ou bzImage) com a serial no console:
rem   serial D:\MEU.ELF
rem Sem tela por enquanto: so aparece o que o guest escreve na COM1.
rem Ctrl+C encerra o QEMU.
D:\UTILS\QEMU\QEMU.EXE -L D:\UTILS\QEMU -M isapc -m 32 -kernel %1 -serial stdio -display none -vga none -monitor none -nic none
