#!/bin/sh
# vm-stack.sh [BASE] [ELF] - "backtrace" heuristico do programa travado na VM de
# teste: le 4 KB a partir do RSP atual pelo monitor do QEMU de fora e mostra as
# palavras que caem dentro do executavel (BASE = base de carga do [mod] exec).
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
. "$S/scripts/env.sh"
B=$(grep -a "exec QEMU.EXE" "$BUILD_ROOT/vm/serial.log" | tail -1 | sed -E 's/.*base=([0-9A-Fa-f]+).*/\1/' | tr -d '\r')
BASE=${1:-$((0x$B))}
ELF=${2:-$BUILD_ROOT/qemu-${QEMU_VERSION:-9.2.0}/qemu-system-i386.elf}
RSP=$(sh "$S/scripts/vm.sh" mon "info registers" | grep -oE "RSP=[0-9a-f]+" | cut -d= -f2)
echo "base=$BASE rsp=0x$RSP"
sh "$S/scripts/vm.sh" mon "x/512gx 0x$RSP" | grep -oE "0x[0-9a-f]{16}" | while read -r w; do
  o=$((w - BASE))
  if [ "$o" -gt 0 ] && [ "$o" -lt $((0x1000000)) ]; then
    f=$(nm -n "$ELF" | awk -v c=$o '{x=strtonum("0x"$1); if (x<=c) l=$3; else {print l; exit}}')
    printf '%s  %s\n' "$w" "$f"
  fi
done | awk '!seen[$2]++' | head -40
