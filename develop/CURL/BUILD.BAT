@echo off
rem Compila o curl. Precisa do -I D: para achar so.h/tls.h.
cc -I D: -c D:\DEVELOP\CURL\CURL.C
D:\LINK.EXE /out:D:\DEVELOP\CURL\CURL.EXE D:\CRT0.OBJ D:\DEVELOP\CURL\CURL.OBJ D:\LIBC.DLL D:\KERNEL.IMP D:\TLS.DLL
echo curl compilado: D:\DEVELOP\CURL\CURL.EXE
