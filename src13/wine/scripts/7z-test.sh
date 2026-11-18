#!/bin/sh
# 7z-test.sh - instala o 7-Zip no QEMU (instalador em
# wine/build/tests/7ZIP.EXE, modo silencioso) e roda o 7z.exe instalado.
#   DEMO=1       compacta, lista, testa e extrai com o 7z instalado
#   WINEDEBUG=1  registra na serial o que falta
#   GET=1        traz 7z.exe e 7z.dll para wine/build/qemu-out
S=$(cd "$(dirname "$0")/../.." && pwd)
cd "$S" || exit 1
make -C wine build/WINE.DLL > /tmp/wine-build.log 2>&1 || { grep -E "error|Error" /tmp/wine-build.log; exit 1; }
Z='D:\WINE\7-Zip\7z.exe'
set -- -c 'SET WINE=1' ${WINEDEBUG:+-c 'SET WINEDEBUG=1'} -c 'D:\7ZIP.EXE /S /D=D:\WINE\7-Zip' -c "$Z"
if [ -n "$DEMO" ]; then
  set -- "$@" \
    -c "$Z"' a D:\WINE\T.7z D:\HELLO.EXE D:\HELLOW.EXE D:\TESTE.TXT' \
    -c "$Z"' l D:\WINE\T.7z' \
    -c "$Z"' t D:\WINE\T.7z' \
    -c "$Z"' x D:\WINE\T.7z -oD:\WINE\OUT -y' \
    -c 'DIR D:\WINE\OUT' \
    -c "$Z"' a -tzip D:\WINE\T.ZIP D:\HELLOW.EXE' \
    -c "$Z"' h -scrcSHA256 D:\HELLOW.EXE'
fi
TIMEOUT=${TIMEOUT:-120} timeout 900 sh wine/scripts/qemu-test.sh \
  ${GET:+-g WINE/7-Zip/7z.exe -g WINE/7-Zip/7z.dll} ${DEMO:+-g WINE/T.7z -g WINE/T.ZIP -g WINE/OUT/HELLOW.EXE} \
  "$@" > /tmp/wine-run.out 2>&1
echo "=== saida"
grep -v "^## (" /tmp/wine-run.out | tail -${LINES_OUT:-60}
echo "=== serial"
grep -a "wine\]\|EXCEPTION\|\[exc\]\|\[visual\] tela" wine/build/qemu-serial.log | grep -av "CreateFile (nao existe) D:.WINE.7-Zip\|RegCreateKey" | tail -${LINES_SER:-25}
