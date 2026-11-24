@echo off
rem Compila o AGENT2 do SODOS com o CC do proprio sistema.
rem Rode dentro da pasta dos fontes:  cd \develop\agent2  e depois  build
rem Headers e LIBC.DLL vem de D:\; sem D: (notebook), de U:\INCLUDE e U:\.
set inc=D:\
set lib=D:\
if not exist D:\STDIO.H set inc=U:\INCLUDE\
if not exist D:\LIBC.DLL set lib=U:\
echo Compilando AGENT2.EXE (headers em %inc%, DLLs em %lib%)
cc -I %inc% -L %lib% -o AGENT2.EXE agent2.c
if errorlevel 1 goto erro
echo AGENT2.EXE pronto. Rode:  agent2
goto fim
:erro
echo Falhou a compilacao do AGENT2
:fim
