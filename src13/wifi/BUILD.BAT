@echo off
rem Compila o WIFI (configurador das redes) do SODOS com o CC do proprio sistema.
rem Rode dentro da pasta dos fontes:  cd \src\wifi  e depois  build
rem Headers e LIBC.DLL vem de D:\; sem D: (notebook), de U:\INCLUDE e U:\.
set inc=D:\
set lib=D:\
if not exist D:\STDIO.H set inc=U:\INCLUDE\
if not exist D:\LIBC.DLL set lib=U:\
echo Compilando WIFI.EXE (headers em %inc%, DLLs em %lib%)
cc -I %inc% -L %lib% -o WIFI.EXE wifi.cpp
if errorlevel 1 goto erro
echo WIFI.EXE pronto. Rode:  wifi
goto fim
:erro
echo Falhou a compilacao do WIFI
:fim
