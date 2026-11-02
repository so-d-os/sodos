@echo off
rem Chat direto com o Qwen2.5-0.5B (pede o kernel com 1 GB de heap).
rem Arquivos: U:\LLAMA.EXE, U:\LIBCX.DLL e U:\SRC\LLM\QWEN05.GGU.
if not exist U:\LLAMA.EXE goto semexe
if not exist U:\SRC\LLM\QWEN05.GGU goto semmodelo
U:\LLAMA.EXE U:\SRC\LLM\QWEN05.GGU
goto fim
:semexe
echo Falta U:\LLAMA.EXE
goto fim
:semmodelo
echo Falta U:\SRC\LLM\QWEN05.GGU
:fim
