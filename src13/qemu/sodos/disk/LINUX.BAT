@echo off
rem Tiny Core Linux 15 com X (Xvesa) numa janela do WIN. Abra pelo WIN.
rem Fechar a janela desliga o QEMU. Sem KVM no QEMU de fora, demora minutos.
cd D:\UTILS\QEMU
QEMU.EXE -L D:\UTILS\QEMU -M isapc -cpu pentium3 -m 128 -accel tcg,tb-size=64 -nodefaults -vga std -display curses -kernel VMLINUZ -initrd CORE.GZ -append "loglevel=3 cde noapic" -cdrom TINYCORE.ISO
