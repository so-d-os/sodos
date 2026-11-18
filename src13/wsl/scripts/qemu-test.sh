#!/bin/sh
# qemu-test.sh - Testa o WSL.EXE no QEMU (KVM com SVM aninhado), sem mexer no
# disk.img: copia o disco, poe WSL.EXE em D:\ e liga wsl/build/windisk.img
# (make-testdisk.sh) como SSD NVMe. Cada argumento vira um comando no SODOS
# pela API (POST /exec); a serial (com a saida da VM Linux) vai para
# wsl/build/qemu-serial.log.
#   sh wsl/scripts/qemu-test.sh 'D:\WSL.EXE list' 'D:\WSL.EXE run -t 60 -x "uname -a\n"'
# WINDISK=... troca o disco NVMe; KEEP=1 deixa o QEMU de pe.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
W=$S/wsl/build
PORT=${PORT:-8799}
WINDISK=${WINDISK:-$W/windisk.img}
T=$(mktemp -d /tmp/sodos-wsl-XXXXXX)
cleanup() {
  [ -n "$QPID" ] && [ -z "$KEEP" ] && kill "$QPID" 2>/dev/null
  [ -f "$T/serial.log" ] && cp "$T/serial.log" "$W/qemu-serial.log"
  [ -z "$KEEP" ] && rm -rf "$T"
}
trap cleanup EXIT
cp --sparse=always "$S/disk.img" "$T/disk.img"
printf 'api start\r\n' > "$T/AUTOEXEC.BAT"
mcopy -o -i "$T/disk.img@@512" "$T/AUTOEXEC.BAT" ::/AUTOEXEC.BAT
mcopy -o -i "$T/disk.img@@512" "$W/WSL.EXE" ::/WSL.EXE
grep -qw svm /proc/cpuinfo || { echo "qemu-test: este host nao tem svm" >&2; exit 1; }
setsid qemu-system-x86_64 -accel kvm -cpu host,+svm -m ${MEM:-8192} -smp ${SMP:-4} \
  -vga std -display none -serial file:"$T/serial.log" \
  -netdev user,id=n0,hostfwd=tcp::$PORT-:8797 -device ne2k_pci,netdev=n0 -boot order=c \
  -drive format=raw,file="$S/os.img",if=ide,index=0,snapshot=on \
  -drive format=raw,file="$T/disk.img",if=ide,index=1 \
  -drive format=raw,file="$S/disk2.img",if=ide,index=2,snapshot=on \
  -drive format=raw,file="$WINDISK",if=none,id=nv0,snapshot=on -device nvme,drive=nv0,serial=wsltest \
  >"$T/qemu.log" 2>&1 </dev/null &
QPID=$!
i=0
until curl -s -m 2 "http://127.0.0.1:$PORT/win" >/dev/null 2>&1; do
  i=$((i + 1))
  if [ $i -gt 60 ]; then
    echo "qemu-test: a API nao respondeu (serial em $W/qemu-serial.log)" >&2
    cat "$T/qemu.log" >&2
    tail -20 "$T/serial.log" >&2
    exit 1
  fi
  sleep 1
done
for c in "$@"; do
  echo "## $c"
  t0=$(date +%s)
  curl -s -m ${TIMEOUT:-600} --data-binary "$c" "http://127.0.0.1:$PORT/exec" || echo "qemu-test: curl falhou ($?)"
  echo "## ($(( $(date +%s) - t0 )) s)"
done
[ -n "$KEEP" ] && { echo "qemu-test: QEMU pid $QPID, API em $PORT"; trap - EXIT; }
exit 0
