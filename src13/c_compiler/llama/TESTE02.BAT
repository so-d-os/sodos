@echo off
rem Chat com o DeepSeek-R1-Distill-Qwen-1.5B Q4_0 (1,07 GB; pede o kernel
rem com o mapa de 4 GB). Arquivos: U:\LLAMA.EXE, U:\LIBCX.DLL e
rem U:\SRC\LLM\DEEPSEEK.GGU. O modelo pensa em <think>...</think> antes de responder.
if not exist U:\LLAMA.EXE goto semexe
if not exist U:\SRC\LLM\DEEPSEEK.GGU goto semmodelo
U:\LLAMA.EXE U:\SRC\LLM\DEEPSEEK.GGU -n 1024
goto fim
:semexe
echo Falta U:\LLAMA.EXE
goto fim
:semmodelo
echo Falta U:\SRC\LLM\DEEPSEEK.GGU
:fim
