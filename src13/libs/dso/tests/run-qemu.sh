#!/bin/sh
# run-qemu.sh - roda os testes de biblioteca dinamica dentro do SODOS.
#
#   sh tests/run-qemu.sh            # make test + QEMU sem janela + compara
#   KEEP=1 sh tests/run-qemu.sh     # deixa o QEMU aberto (porta 8799) para investigar
#   DSO_TEST_CMD='D:\X.EXE a b' EXTRA_FILES=/caminho/X.EXE sh tests/run-qemu.sh
#                                   # roda outros comandos (um por linha); os
#                                   # arquivos extras vao para D:\DSOTEST
#
# Usa copias de os.img e disk.img em um diretorio temporario: o disco de
# trabalho do projeto nao e tocado. Os comandos entram pela API HTTP do kernel
# (POST /exec), a mesma de docs/05-firefox-build.md.
set -e
D=$(cd "$(dirname "$0")/.." && pwd)
S=$(cd "$D/../.." && pwd)
W=${DSO_TEST_DIR:-$(mktemp -d /tmp/sodos-dso-XXXXXX)}
PORT=${DSO_TEST_PORT:-8799}
LIB=$D/build/sysroot/lib
T=$D/build/test

[ -f "$S/os.img" ] && [ -f "$S/disk.img" ] || { echo "falta os.img/disk.img: rode make em src13" >&2; exit 1; }
mkdir -p "$W"
cp "$S/os.img" "$W/os.img"
cp --sparse=always "$S/disk.img" "$W/disk.img"
IMG="$W/disk.img@@512"
mmd -i "$IMG" ::/LIB 2>/dev/null || true
mmd -i "$IMG" ::/DSOTEST 2>/dev/null || true
for f in "$LIB"/*.so.*; do
  [ -L "$f" ] || mcopy -o -i "$IMG" "$f" "::/LIB/$(basename "$f")"
done
for f in "$T"/*; do
  [ -L "$f" ] || mcopy -o -i "$IMG" "$f" "::/DSOTEST/$(basename "$f")"
done
[ -n "$EXTRA_FILES" ] && for f in $EXTRA_FILES; do mcopy -o -i "$IMG" "$f" "::/DSOTEST/$(basename "$f")"; done

ACCEL=tcg
[ -r /dev/kvm ] && [ -w /dev/kvm ] && ACCEL=kvm
qemu-system-x86_64 -accel $ACCEL -m 4096 -display none -serial "file:$W/serial.log" \
  -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:8797" -device ne2k_pci,netdev=n0 -boot order=c \
  -drive "format=raw,file=$W/os.img,if=ide,index=0" \
  -drive "format=raw,file=$W/disk.img,if=ide,index=1" &
QPID=$!
[ -n "$KEEP" ] || trap 'kill $QPID 2>/dev/null; wait $QPID 2>/dev/null' EXIT

i=0
until curl -s -m 2 "http://127.0.0.1:$PORT/" >/dev/null 2>&1; do
  i=$((i + 1))
  if [ $i -gt 60 ]; then echo "o SODOS nao respondeu na porta $PORT (log: $W/serial.log)" >&2; exit 1; fi
  sleep 1
done

run() {
  echo "### $1"
  curl -s -m "${2:-60}" -X POST --data-binary "$1" "http://127.0.0.1:$PORT/exec"
  echo
}

rc=0
if [ -n "$DSO_TEST_CMD" ]; then
  # Um comando por linha, no lugar dos testes.
  echo "$DSO_TEST_CMD" | while IFS= read -r c; do
    [ -n "$c" ] && run "$c" "${DSO_TEST_TIMEOUT:-120}"
  done
else
  out=$(run 'D:\DSOTEST\DSOTEST.EXE um dois')
  echo "$out"
  echo "$out" | grep -q 'DSOTEST: [0-9]* ok, 0 falhas' || rc=1
  if [ -f "$T/CXXDSO.EXE" ]; then
    out=$(run 'D:\DSOTEST\CXXDSO.EXE')
    echo "$out"
    echo "$out" | grep -q 'CXXDSO: [0-9]* ok, 0 falhas' || rc=1
  fi
fi
[ -n "$KEEP" ] && echo "QEMU aberto (pid $QPID), API em http://127.0.0.1:$PORT, arquivos em $W"
[ $rc -eq 0 ] && echo "testes de biblioteca dinamica: OK" || { echo "testes de biblioteca dinamica: FALHA (serial: $W/serial.log)"; tail -30 "$W/serial.log"; }
exit $rc
