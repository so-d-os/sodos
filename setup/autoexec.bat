@echo off
rem AUTOEXEC.BAT - roda no boot do SODOS (gravado pelo SETUP so se nao havia um)
set PATHSODOS=D:\SODOS
set PATH=D:\SODOS\BIN;D:\;D:\SODOS
set LINKEXT=1
set DISKSYNC=1
rem PRODUCTION=1: programas do WIN sem janela de saida, tudo no log (Ctrl+F12)
set PRODUCTION=1
rem SYSTEMLOG=1: log do Ctrl+F12 salvo em D:\LOG\CONSOLE.LOG a cada 5 min
set SYSTEMLOG=1
rem WSL=1: Linux (D:\WSL) em segundo plano, console no Ctrl+F10
set WSL=0
set FIREFOX_WORKFOLDER=D:\USER\DEFAULT\FIREFOX
rem NET WIFI START
D:
WIN
