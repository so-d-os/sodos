@echo off
rem Compila o MC do SODOS com o CC do proprio sistema.
rem Rode dentro da pasta dos fontes:  cd \src\mc  e depois  build
rem Headers e LIBC.DLL vem de D:\; sem D: (notebook), de U:\INCLUDE e U:\.
set inc=D:\
set lib=D:\
if not exist D:\STDIO.H set inc=U:\INCLUDE\
if not exist D:\LIBC.DLL set lib=U:\
echo Compilando MC.EXE (headers em %inc%, DLLs em %lib%)
cc -I %inc% -L %lib% -o MC.EXE mc.c scr.c panel.c ops.c view.c
if errorlevel 1 goto erro
echo MC.EXE pronto. Rode:  mc
goto fim
:erro
echo Falhou a compilacao do MC
:fim
