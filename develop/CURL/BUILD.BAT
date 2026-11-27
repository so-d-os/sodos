@echo off
rem Compila o curl. Precisa do -I D: para achar so.h/tls.h.
cc -I D:\ -c CURL.C
D:\LINK.EXE /out:CURL.EXE D:\CRT0.OBJ CURL.OBJ D:\LIBC.DLL D:\KERNEL.IMP D:\TLS.DLL
echo curl compilado: CURL.EXE
