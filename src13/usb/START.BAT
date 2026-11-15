@echo off
rem START.BAT - caminhos do SODOS no pendrive (raiz de U:).
rem Os programas ficam em U:\SODOS e as configuracoes em U:\SODOS\NET.
rem Rode:  U:  e depois  start
set PATHSODOS=U:\SODOS
set PATH=U:\SODOS;U:\SODOS\BIN
if not exist U:\SODOS\NET\WIFI.CFG echo Falta U:\SODOS\NET\WIFI.CFG (ssid= e psk=)
if not exist U:\SODOS\NET\RTW8852B.BIN echo Falta U:\SODOS\NET\RTW8852B.BIN (firmware do WiFi)
echo PATHSODOS=%PATHSODOS%
echo PATH=%PATH%
