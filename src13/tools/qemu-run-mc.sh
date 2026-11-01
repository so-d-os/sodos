#!/bin/bash
# Sobe QEMU (GTK), espera o shell, digita mount 0 e MC.EXE auto via monitor.
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
MON=/tmp/sodos-qemu-mon.sock
rm -f "$MON"

qemu-system-x86_64 -m 256 -usb -device usb-tablet \
  -vga std -display gtk \
  -netdev user,id=n0,hostfwd=tcp::8797-:8797 -device ne2k_pci,netdev=n0 \
  -boot order=c \
  -drive format=raw,file=os.img,if=ide,index=0 \
  -drive format=raw,file=disk.img,if=ide,index=1 \
  -drive format=raw,file=disk2.img,if=ide,index=2 \
  -monitor unix:"$MON",server,nowait \
  -serial "file:$ROOT/qemu_serial.log" &
QPID=$!
echo "QEMU pid=$QPID monitor=$MON"

send() {
  # cada arg e um sendkey
  local k
  for k in "$@"; do
    printf 'sendkey %s\n' "$k" | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || \
      printf 'sendkey %s\n' "$k" | nc -U "$MON" >/dev/null 2>&1 || true
    sleep 0.05
  done
}

type_str() {
  local s="$1" i c
  for ((i=0; i<${#s}; i++)); do
    c=${s:i:1}
    case "$c" in
      ' ') send spc ;;
      '.') send dot ;;
      '\\') send backslash ;;
      '/') send slash ;;
      '-') send minus ;;
      '_') send shift-minus ;;
      '=') send equal ;;
      [A-Z]) send shift-$(echo "$c" | tr 'A-Z' 'a-z') ;;
      *) send "$c" ;;
    esac
  done
}

# espera boot (~4s) + monitor
for i in $(seq 1 40); do
  if [ -S "$MON" ]; then break; fi
  sleep 0.25
done
sleep 5

echo ">> mount 0"
type_str "mount 0"
send ret
sleep 1.5

echo ">> MC.EXE auto"
type_str "MC.EXE auto"
send ret
sleep 3

echo "QEMU a correr (pid $QPID). Feche a janela para sair."
echo "Interativo: na janela digite  MC.EXE   (sem auto) e use Tab/j/k/q"
wait $QPID || true
