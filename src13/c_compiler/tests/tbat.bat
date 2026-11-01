@echo off
set nome=SODOS
echo ola %nome%
if %nome%==SODOS echo variavel ok
if not %nome%==outro echo diferente ok
if exist NAO.TEM echo exist-falhou
if not exist NAO.TEM echo ausente ok
goto pula
echo goto-falhou
:pula
echo chegou
call tsub.bat um
if errorlevel 1 echo call-falhou
naoexiste
if not errorlevel 1 echo erro-falhou
if errorlevel 1 echo erro ok
echo arg=%1
echo fim
