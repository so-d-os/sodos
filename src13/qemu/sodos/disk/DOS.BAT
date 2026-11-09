@echo off
rem FreeDOS 1.3 (disquete de 1,44 MB) na tela de texto do SODOS (VGA, -display
rem curses = ui/sodos.c). F11 = Ctrl e F12 = Alt na proxima tecla. Ctrl+C sai.
set Q=D:\UTILS\QEMU
%Q%\QEMU.EXE -L %Q% -M isapc -m 16 -display curses -serial none -monitor none -nic none -boot a -drive if=floppy,format=raw,file.driver=file,file.locking=off,file.filename=%Q%\FREEDOS.IMG
