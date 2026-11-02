@echo off
rem Compila o git do SODOS com o CC do proprio sistema.
rem Rode dentro da pasta dos fontes:  cd \src\git  e depois  build
rem Headers, LIBC.DLL e TLS.DLL vem de D:\; sem D: (notebook), de U:\INCLUDE e U:\.
set inc=D:\
set lib=D:\
if not exist D:\STDIO.H set inc=U:\INCLUDE\
if not exist D:\LIBC.DLL set lib=U:\
echo Compilando GIT.EXE (headers em %inc%, DLLs em %lib%)
cc -I %inc% -L %lib% -o GIT.EXE git.c http.c pack.c sha1.c zinf.c %lib%TLS.DLL
if errorlevel 1 goto erro
echo GIT.EXE pronto. Rode:  git
goto fim
:erro
echo Falhou a compilacao do git
:fim
