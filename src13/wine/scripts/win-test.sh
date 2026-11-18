#!/bin/sh
# win-test.sh start [EXE]   sobe o SODOS com o desktop (WIN), liga o WINE e
#                           abre o EXE do Windows numa janela (padrao D:\CLAUDE.EXE)
# win-test.sh shot [nome]   captura a tela em wine/build/NOME.png e mostra o
#                           fim da serial e do ClaudeSetup.log
# win-test.sh cmd 'LINHA'   roda uma linha no shell do SODOS
# win-test.sh stop          derruba o QEMU
# Variaveis no start: NOVERIFY=1 -> SET WINE_NOVERIFY=1.
S=$(cd "$(dirname "$0")/../.." && pwd)
cd "$S" || exit 1
PORT=${PORT:-8799}
ST=/tmp/wine-win-test.state
case "$1" in
  start)
    EXE=${2:-'D:\CLAUDE.EXE'}
    make -C wine build/WINE.DLL > /tmp/wine-build.log 2>&1 || { grep -E "error|Error" /tmp/wine-build.log; exit 1; }
    [ -f "$ST" ] && sh "$0" stop
    sh wine/scripts/qemu-test.sh -w -k -c "SET WINE=1" -c "SET WINEDEBUG=1" \
      ${NOVERIFY:+-c "SET WINE_NOVERIFY=1"} ${SIGTEST:+-c "SET WINE_SIGTEST=1"} > /tmp/wine-win-start.out 2>&1
    grep "qemu-test: QEMU pid" /tmp/wine-win-start.out > "$ST" || { cat /tmp/wine-win-start.out; exit 1; }
    cat "$ST"
    sleep 2
    curl -s -m 10 -G --data-urlencode "path=$EXE" "http://127.0.0.1:$PORT/win/launch"
    echo
    ;;
  shot)
    N=${2:-shot}
    D=$(sed -n 's/.*disco em \(.*\)\/disk.img/\1/p' "$ST")
    curl -s -m 20 -o "wine/build/$N.png" "http://127.0.0.1:$PORT/win/printscreen.png" && echo "wine/build/$N.png"
    echo "=== serial"
    grep -a "wine\]\|EXCEPTION\|\[exc\]" "$D/serial.log" | tail -${LINES_SER:-15}
    echo "=== ClaudeSetup.log"
    timeout 10 mcopy -n -o -i "$D/disk.img@@512" ::/WINE/TEMP/ClaudeSetup.log /tmp/ClaudeSetup.log </dev/null 2>/dev/null; tail -${LINES_LOG:-12} /tmp/ClaudeSetup.log
    ;;
  cmd)
    curl -s -m ${TIMEOUT:-30} --data-binary "$2" "http://127.0.0.1:$PORT/exec"
    ;;
  stop)
    P=$(sed -n 's/.*QEMU pid \([0-9]*\).*/\1/p' "$ST")
    D=$(sed -n 's/.*disco em \(.*\)\/disk.img/\1/p' "$ST")
    [ -n "$P" ] && kill "$P" 2>/dev/null
    [ -n "$D" ] && cp "$D/serial.log" wine/build/qemu-serial.log 2>/dev/null && rm -rf "$D"
    rm -f "$ST"
    ;;
  *) echo "uso: win-test.sh start|shot|cmd|stop"; exit 1 ;;
esac
