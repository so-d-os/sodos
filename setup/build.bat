@echo off
rem Compila o SETUP.EXE com o cc do SODOS. Rode nesta pasta:  build
cc -I D:\ -L D:\ -o SETUP.EXE setup.c install.c D:\GUI.DLL
if errorlevel 1 goto erro
echo SETUP.EXE pronto. Monte o pacote (empacota.sh) e abra pelo WIN.
goto fim
:erro
echo Falhou a compilacao do SETUP
:fim
