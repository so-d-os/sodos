#!/bin/sh
# vm.sh - VM de teste do WINE com monitor do QEMU (para ver onde as CPUs estao).
#   vm.sh start [win]     copia do disk.img com o WINE.DLL e os EXEs de
#                         wine/build/tests; "win" sobe o desktop. Sem janela.
#   vm.sh cmd 'LINHA'     roda uma linha no shell (POST /exec, espera acabar)
#   vm.sh launch 'D:\X'   abre um programa no desktop (/win/launch)
#   vm.sh keys 'texto'    digita no teclado da VM (sendkey); \n = Enter
#   vm.sh shot [nome]     captura da tela em wine/build/NOME.png
#   vm.sh rips            RIP de cada CPU, com o modulo do WINE/kernel
#   vm.sh serial [n]      fim da serial
#   vm.sh stop
# PORT (padrao 8803) e VMDIR (padrao /tmp/sodos-wine-vm) escolhem a VM.
# GUI=1 no start abre a janela do QEMU (para o usuario ver).
# KEEP=1 no start reaproveita os discos da VM anterior (programas instalados).
S=$(cd "$(dirname "$0")/../.." && pwd)
PORT=${PORT:-8803}
V=${VMDIR:-/tmp/sodos-wine-vm}
MON=$V/mon
mon() { printf '%s\n' "$1" | nc -U -q1 "$MON" 2>/dev/null | tr -d '\r' | sed 's/\x1b\[[0-9;]*[A-Za-z]//g'; }
case "$1" in
  start)
    sh "$0" stop >/dev/null 2>&1
    mkdir -p "$V"
    cd "$S" || exit 1
    make -C wine build/WINE.DLL > "$V/build.log" 2>&1 || { grep -E "error|Error" "$V/build.log"; exit 1; }
    if [ -z "$KEEP" ] || [ ! -f "$V/disk.img" ]; then
      cp --sparse=always disk.img "$V/disk.img"
      cp os.img "$V/os.img"
    fi
    I="$V/disk.img@@512"
    if [ "$2" = win ]; then printf 'api start\r\nwin\r\n' > "$V/AUTOEXEC.BAT"; else printf 'api start\r\n' > "$V/AUTOEXEC.BAT"; fi
    mcopy -o -i "$I" "$V/AUTOEXEC.BAT" ::/AUTOEXEC.BAT </dev/null
    mdir -i "$I" ::/SODOS/DRV >/dev/null 2>&1 </dev/null || mmd -i "$I" ::/SODOS/DRV </dev/null
    mcopy -o -i "$I" wine/build/WINE.DLL ::/SODOS/DRV/WINE.DLL </dev/null
    for f in wine/build/tests/*.EXE wine/build/tests/*.BAT; do
      [ -f "$f" ] && mcopy -o -i "$I" "$f" "::/$(basename "$f" | tr a-z A-Z)" </dev/null
    done
    DISP="-display none"
    [ -n "$GUI" ] && DISP=""
    # QEMU_EXTRA="-s": gdb do QEMU na porta 1234 (watchpoints de hardware)
    setsid qemu-system-x86_64 -accel kvm -cpu host -m 8192 -smp 4 -usb -device usb-tablet -vga std $DISP $QEMU_EXTRA \
      -monitor "unix:$MON,server,nowait" -serial "file:$V/serial.log" \
      -netdev user,id=n0,hostfwd=tcp::$PORT-:8797 -device ne2k_pci,netdev=n0 -boot order=c \
      -drive format=raw,file="$V/os.img",if=ide,index=0 \
      -drive format=raw,file="$V/disk.img",if=ide,index=1 \
      -drive format=raw,file="$S/disk2.img",if=ide,index=2,snapshot=on > "$V/qemu.log" 2>&1 </dev/null &
    echo $! > "$V/pid"
    i=0
    until curl -s -m 2 "http://127.0.0.1:$PORT/win" >/dev/null 2>&1; do
      i=$((i + 1)); [ $i -gt 60 ] && { echo "vm: a API nao respondeu"; tail -5 "$V/qemu.log"; exit 1; }
      sleep 1
    done
    [ "$2" = win ] && sleep 4
    echo "vm: pronta na porta $PORT ($V)"
    ;;
  cmd) curl -s -m ${TIMEOUT:-120} --data-binary "$2" "http://127.0.0.1:$PORT/exec" ;;
  launch) curl -s -m 10 --data-urlencode "path=$2" "http://127.0.0.1:$PORT/win/launch"; echo ;;
  keys)
    printf '%s' "$2" | python3 -c '
import sys
m = {" ": "spc", "\\": "backslash", ":": "shift-semicolon", ".": "dot", "-": "minus", "/": "slash",
     "=": "equal", "_": "shift-minus", "\n": "ret", ",": "comma", ";": "semicolon"}
for c in sys.stdin.read().replace("\\n", "\n"):
    if c in m: print("sendkey " + m[c])
    elif c.isupper(): print("sendkey shift-" + c.lower())
    else: print("sendkey " + c)
' | while read -r l; do printf '%s\n' "$l"; sleep 0.08; done | nc -U -q1 "$MON" >/dev/null 2>&1
    ;;
  shot)
    N=${2:-vm}
    curl -s -m 30 -o "$S/wine/build/$N.png" "http://127.0.0.1:$PORT/win/printscreen.png" && echo "wine/build/$N.png"
    ;;
  rips)
    mon "info registers -a" | grep -E "^CPU#|^RIP=" | paste - - | sed 's/^CPU#\([0-9]*\).*RIP=\([0-9a-f]*\).*/\1 \2/' |
      while read -r c r; do
        a=$((0x$r))
        w=$(grep -a "WINE.DLL base\|base=" "$V/serial.log" | tail -0)
        printf 'CPU%s rip=%s\n' "$c" "$r"
      done
    grep -a "\[mod\] exec WINE entry" "$V/serial.log" | tail -1
    ;;
  serial) grep -a "wine\]\|visual\] tela\|EXCEPTION\|\[exc\]\|\[kt\]" "$V/serial.log" | tail -${2:-25} ;;
  stop)
    [ -f "$V/pid" ] && kill "$(cat "$V/pid")" 2>/dev/null
    rm -f "$V/pid" "$MON"
    ;;
  *) echo "uso: vm.sh start|cmd|launch|keys|shot|rips|serial|stop"; exit 1 ;;
esac
