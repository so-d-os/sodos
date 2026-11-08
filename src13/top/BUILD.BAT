@echo off
rem Compila o TOP do SODOS com o CC do proprio sistema.
rem Rode dentro da pasta dos fontes:  cd \src\top  e depois  build
rem Headers e LIBC.DLL vem de D:\; sem D: (notebook), de U:\INCLUDE e U:\.
set inc=D:\
set lib=D:\
if not exist D:\STDIO.H set inc=U:\INCLUDE\
if not exist D:\LIBC.DLL set lib=U:\
echo Compilando TOP.EXE (headers em %inc%, DLLs em %lib%)
cc -I %inc% -L %lib% -o TOP.EXE top.c tela.c dados.c procs.c mapa.c video.c
if errorlevel 1 goto erro
echo TOP.EXE pronto. Rode:  top
goto fim
:erro
echo Falhou a compilacao do TOP
:fim
