@echo off
rem Compila a PONTE do SODOS com o CC do proprio sistema.
rem Rode dentro da pasta dos fontes:  cd \develop\ponte  e depois  build
rem Headers e LIBC.DLL vem de D:\; sem D: (notebook), de U:\INCLUDE e U:\.
set inc=D:\
set lib=D:\
if not exist D:\STDIO.H set inc=U:\INCLUDE\
if not exist D:\LIBC.DLL set lib=U:\
echo Compilando PONTE.EXE (headers em %inc%, DLLs em %lib%)
cc -I %inc% -L %lib% -o PONTE.EXE ponte.c
if errorlevel 1 goto erro
echo PONTE.EXE pronto. Rode numa tela livre:  ponte
goto fim
:erro
echo Falhou a compilacao da PONTE
:fim
