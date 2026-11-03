@echo off
rem Compila o jogo da cobrinha do SODOS.
rem Rode dentro da pasta dos fontes:  cd \src\cobra  e depois  build
set inc=D:\
set lib=D:\
if not exist D:\STDIO.H set inc=U:\INCLUDE\
if not exist D:\LIBC.DLL set lib=U:\
echo Compilando COBRA.EXE (headers em %inc%, DLLs em %lib%)
cc -I %inc% -L %lib% -o COBRA.EXE cobra.cpp
if errorlevel 1 goto erro
echo COBRA.EXE pronto. Rode:  cobra
goto fim
:erro
echo Falhou a compilacao do COBRA
:fim
