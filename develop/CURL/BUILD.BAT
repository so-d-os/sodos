@echo off
rem Compila o CURL do SODOS com o CC do proprio sistema (sem precisar do make).
rem Rode dentro da pasta dos fontes:  cd \develop\curl  e depois  build
rem Headers (SO.H, TLS.H), LIBC.DLL e TLS.DLL vem de D:\; sem D: (notebook),
rem de U:\INCLUDE e U:\. Com o MAKE.EXE, basta:  make
set inc=D:\
set lib=D:\
if not exist D:\STDIO.H set inc=U:\INCLUDE\
if not exist D:\LIBC.DLL set lib=U:\
echo Compilando CURL.EXE (headers em %inc%, DLLs em %lib%)
cc -I %inc% -L %lib% -o CURL.EXE curl.c %lib%TLS.DLL
if errorlevel 1 goto erro
echo CURL.EXE pronto. Rode:  curl http://example.com/
goto fim
:erro
echo Falhou a compilacao do CURL
:fim
