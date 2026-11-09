@echo off
rem Guest interativo: o que voce digita vai pela serial ao guest, que devolve
rem em maiusculas. q sai do guest; Ctrl+C encerra o QEMU.
D:\UTILS\QEMU\QEMU.EXE -L D:\UTILS\QEMU -M isapc -m 16 -kernel D:\UTILS\QEMU\ECO.ELF -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=4 -display none -vga none -monitor none -nic none
