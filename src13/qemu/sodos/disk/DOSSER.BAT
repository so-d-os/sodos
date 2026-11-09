@echo off
rem FreeDOS 1.3 so pela serial (sem tela): o
rem SeaBIOS (sercon) manda a tela de texto do BIOS para a COM1. Ctrl+C sai.
set Q=D:\UTILS\QEMU
%Q%\QEMU.EXE -L %Q% -M isapc -m 16 -nographic -vga none -nic none -boot a -fw_cfg name=etc/sercon-port,file=%Q%\SERCON.BIN -drive if=floppy,format=raw,file.driver=file,file.locking=off,file.filename=%Q%\FREEDOS.IMG
