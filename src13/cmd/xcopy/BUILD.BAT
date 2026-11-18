@echo off
rem Compila o XCOPY (copia arquivos e arvores de pastas) do SODOS.
rem Rode dentro da pasta dos fontes:  cd \src\cmd\xcopy  e depois  build
rem Headers e LIBC.DLL vem de D:\; sem D: (notebook), de U:\INCLUDE e U:\.
set inc=D:\
set lib=D:\
if not exist D:\STDIO.H set inc=U:\INCLUDE\
if not exist D:\LIBC.DLL set lib=U:\
echo Compilando XCOPY.EXE (headers em %inc%, DLLs em %lib%)
cc -I %inc% -L %lib% -o XCOPY.EXE xcopy.cpp
if errorlevel 1 goto erro
echo XCOPY.EXE pronto. Rode:  xcopy /?
goto fim
:erro
echo Falhou a compilacao do XCOPY
:fim
