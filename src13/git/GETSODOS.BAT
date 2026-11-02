@echo off
rem Baixa o codigo do SODOS do GitHub para U:\REPO (so o ultimo commit).
rem Se ja foi baixado, atualiza com git pull.
if not exist U:\GIT.EXE goto semgit
if exist U:\REPO\_GIT\STATE.TXT goto atualiza
echo Baixando o SODOS em U:\REPO ...
U:\GIT.EXE clone https://github.com/so-d-os/sodos U:\REPO
if errorlevel 1 goto erro
echo Pronto: U:\REPO
goto fim
:atualiza
echo U:\REPO ja existe; atualizando ...
U:\GIT.EXE pull U:\REPO
if errorlevel 1 goto erro
goto fim
:semgit
echo Falta U:\GIT.EXE
goto fim
:erro
echo Falhou (a rede esta pronta? veja: net)
:fim
