#!/bin/sh
# try.sh [EXE [args]] - compila o WINE.DLL e roda um EXE do Windows no QEMU
# com WINEDEBUG=1, mostrando a saida, as linhas [wine] da serial e o log do
# instalador do Claude (D:\WINE\TEMP\ClaudeSetup.log), se houver.
# EXE padrao: D:\CLAUDE.EXE (copie o instalador para wine/build/tests/CLAUDE.EXE).
S=$(cd "$(dirname "$0")/../.." && pwd)
cd "$S" || exit 1
make -C wine build/WINE.DLL > /tmp/wine-build.log 2>&1 || { grep -E "error|Error" /tmp/wine-build.log; exit 1; }
grep -E "error|link64:|Error" /tmp/wine-build.log | grep -v "^undef"

CMD=${*:-'D:\CLAUDE.EXE'}
rm -f wine/build/qemu-out/ClaudeSetup.log
TIMEOUT=${TIMEOUT:-40} timeout 300 sh wine/scripts/qemu-test.sh ${WIN:+-w} -g WINE/TEMP/ClaudeSetup.log \
  -c "SET WINE=1" -c "SET WINEDEBUG=1" ${NOVERIFY:+-c "SET WINE_NOVERIFY=1"} -c "$CMD" > /tmp/wine-run.out 2>&1
echo "=== saida"
grep -v "^[[:space:]]\|^\$\|^## (" /tmp/wine-run.out | tail -${LINES_OUT:-25}
echo "=== serial"
grep -a "wine\]\|EXCEPTION\|\[exc\]" wine/build/qemu-serial.log | tail -${LINES_SER:-25}
echo "=== ClaudeSetup.log"
tail -${LINES_LOG:-12} wine/build/qemu-out/ClaudeSetup.log 2>/dev/null
