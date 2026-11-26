@echo off
rem Compila o GESTURE (analisador de imagem e gestos) com o CC do SODOS.
rem Rode dentro da pasta dos fontes:  cd \source\sodos\develop\gesture  e depois  build
rem Headers e DLLs vem de D:\; sem D: (notebook), de U:\INCLUDE e U:\.
set inc=D:\
set lib=D:\
if not exist D:\STDIO.H set inc=U:\INCLUDE\
if not exist D:\LIBC.DLL set lib=U:\
echo Compilando GESTURE.EXE (headers em %inc%, DLLs em %lib%)
cc -I %inc% -I . -L %lib% -o GESTURE.EXE gesture.c analyze.c capture.c image.c skin.c blobs.c face.c eyes.c head.c hands.c track.c events.c draw.c haar.c %lib%GUI.DLL %lib%IMGBIG.DLL
if errorlevel 1 goto erro
echo GESTURE.EXE pronto. Rode:  gesture   (ou gesture /? )
goto fim
:erro
echo Falhou a compilacao do GESTURE
:fim
