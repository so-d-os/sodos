@echo off
rem Compila o MAKE do SODOS com o CC do proprio sistema (sem precisar do make).
rem Rode dentro da pasta dos fontes:  cd \develop\make  e depois  build
rem Headers, LIBC.DLL e POSIX.DLL vem de D:\; sem D: (notebook), de U:\INCLUDE e U:\.
rem Com um MAKE.EXE ja pronto, basta:  make   (so recompila o que mudou)
set inc=D:\
set lib=D:\
if not exist D:\STDIO.H set inc=U:\INCLUDE\
if not exist D:\LIBC.DLL set lib=U:\
echo Compilando MAKE.EXE (headers em %inc%, DLLs em %lib%)
cc -I %inc% -L %lib% -o MAKE.EXE main.c input.c macro.c make.c modtime.c rules.c target.c utils.c check.c sodos.c %lib%POSIX.DLL
if errorlevel 1 goto erro
echo MAKE.EXE pronto. Rode:  make
goto fim
:erro
echo Falhou a compilacao do MAKE
:fim
