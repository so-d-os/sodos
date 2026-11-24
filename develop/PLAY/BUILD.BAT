@echo off
rem Compila o PLAY do SODOS com o CC do proprio sistema.
rem Rode dentro da pasta dos fontes:  cd \develop\play  e depois  build
rem Headers e DLLs vem de D:\; sem D: (notebook), de U:\INCLUDE e U:\.
set inc=D:\
set lib=D:\
if not exist D:\STDIO.H set inc=U:\INCLUDE\
if not exist D:\LIBC.DLL set lib=U:\
echo Compilando PLAY.EXE (headers em %inc%, DLLs em %lib%)
cc -I %inc% -L %lib% -o PLAY.EXE play.c %lib%gui.dll %lib%img.dll
if errorlevel 1 goto erro
echo PLAY.EXE pronto. No WIN, abra pelo Explorer ou rode no Command:  play
goto fim
:erro
echo Falhou a compilacao do PLAY
:fim
