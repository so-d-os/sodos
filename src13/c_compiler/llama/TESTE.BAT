@echo off
rem Teste do LLAMA.EXE: uma pergunta fixa (guloso, sempre a mesma resposta)
rem e depois o chat. Uso: TESTE [MODELO.GGU]   (padrao: SMOL135.GGU)
rem Arquivos: U:\LLAMA.EXE, U:\LIBCX.DLL e o modelo em U:\SRC\LLM.
set modelo=U:\SRC\LLM\SMOL135.GGU
if not %1== set modelo=%1
if not exist U:\LLAMA.EXE goto semexe
if not exist %modelo% goto semmodelo
echo == pergunta fixa (esperado: Paris)
U:\LLAMA.EXE %modelo% -t 0 -n 40 -p "What is the capital of France?"
if errorlevel 1 goto erro
echo == chat (/sair para terminar)
U:\LLAMA.EXE %modelo%
goto fim
:semexe
echo Falta U:\LLAMA.EXE
goto fim
:semmodelo
echo Falta o modelo %modelo%
goto fim
:erro
echo LLAMA falhou (kernel novo com SSE? rode make grub)
:fim
