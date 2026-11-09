@echo off
rem Guest de teste: escreve pela serial do QEMU e sai (codigo 33).
D:\UTILS\QEMU\QEMU.EXE -L D:\UTILS\QEMU -M isapc -m 16 -kernel D:\UTILS\QEMU\HELLO.ELF -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=4 -display none -vga none -monitor none -nic none
