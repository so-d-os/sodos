@echo off
rem Teste do QEMU passo a passo, para achar onde o SODOS cai (EXCEPTION).
rem   qteste         passos 1 a 3, sem interacao (versao, maquinas, HELLO.ELF)
rem   qteste dos     passo 4: FreeDOS na tela de texto (F12 F12 sai)
rem   qteste linux   passo 5: Tiny Core com X (abra pelo WIN)
rem   qteste crash   derruba o SODOS de proposito (CRASHT pf) e testa o log
rem Cada passo anota inicio e fim em QTESTE.LOG, nesta pasta. Se o SODOS
rem cair, o ultimo "inicio" sem "fim" e o passo culpado, e o kernel grava
rem CRASH.LOG na raiz desta unidade: registradores, RIP como QEMU.EXE+0x...,
rem modulos carregados e a pilha. Traga os dois arquivos.
set Q=D:\UTILS\QEMU
set L=%Q%\QTESTE.LOG
if %1==dos goto dos
if %1==linux goto linux
if %1==crash goto crash

echo === QTESTE: memoria e discos antes do QEMU > %L%
free >> %L%
df >> %L%

echo passo 1 inicio: QEMU.EXE --version >> %L%
echo passo 1: QEMU.EXE --version
%Q%\QEMU.EXE --version >> %L%
if errorlevel 1 echo passo 1 errorlevel 1 ou mais >> %L%
echo passo 1 fim >> %L%

echo passo 2 inicio: -machine help >> %L%
echo passo 2: -machine help
%Q%\QEMU.EXE -machine help >> %L%
echo passo 2 fim >> %L%

echo passo 3 inicio: HELLO.ELF pela serial (sai com 33) >> %L%
echo passo 3: HELLO.ELF
%Q%\QEMU.EXE -L %Q% -M isapc -m 16 -kernel %Q%\HELLO.ELF -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=4 -display none -vga none -monitor none -nic none >> %L%
if errorlevel 1 echo passo 3 errorlevel 1 ou mais (33 e o normal) >> %L%
echo passo 3 fim >> %L%
free >> %L%

echo Passos 1 a 3 ok. Veja %L%.
echo Depois: qteste dos (F12 F12 sai) e, no WIN, qteste linux.
goto fim

:dos
echo passo 4 inicio: FreeDOS (DOS.BAT) >> %L%
%Q%\QEMU.EXE -L %Q% -M isapc -m 16 -display curses -serial none -monitor none -nic none -boot a -drive if=floppy,format=raw,file.driver=file,file.locking=off,file.filename=%Q%\FREEDOS.IMG
echo passo 4 fim >> %L%
free >> %L%
goto fim

:linux
echo passo 5 inicio: Tiny Core (LINUX.BAT) >> %L%
free >> %L%
cd %Q%
%Q%\QEMU.EXE -L %Q% -M isapc -cpu pentium3 -m 128 -accel tcg,tb-size=64 -nodefaults -vga std -display curses -kernel VMLINUZ -initrd CORE.GZ -append "loglevel=3 cde noapic" -cdrom TINYCORE.ISO
echo passo 5 fim >> %L%
goto fim

:crash
echo crash inicio: CRASHT pf (o SODOS deve parar e gravar CRASH.LOG) >> %L%
%Q%\CRASHT.EXE pf
echo crash: CRASHT voltou sem excecao >> %L%

:fim
