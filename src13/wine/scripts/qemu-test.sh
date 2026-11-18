#!/bin/sh
# qemu-test.sh [opcoes do dart/scripts/qemu-test.sh] -- COMANDO
#
# Testa o WINE.DLL no QEMU: copia do disk.img com wine/build/WINE.DLL em
# D:\SODOS\DRV e os EXEs de wine/build/tests em D:\. Precisa do os.img com o
# kernel novo (make os.img). Exemplo:
#   sh wine/scripts/qemu-test.sh -c 'SET WINE=1' -c 'D:\HELLOW.EXE a b'
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
W=$S/wine/build
SERIAL_LOG=${SERIAL_LOG:-$W/qemu-serial.log}
export SERIAL_LOG
exec sh "$S/dart/scripts/qemu-test.sh" -p ${PORT:-8799} -o "$W/qemu-out" \
  -x "mdir -i \"\$DISK@@512\" ::/SODOS/DRV >/dev/null 2>&1 || mmd -i \"\$DISK@@512\" ::/SODOS/DRV </dev/null; mcopy -o -i \"\$DISK@@512\" '$W/WINE.DLL' ::/SODOS/DRV/WINE.DLL" \
  "$W"/tests/*.EXE "$@"
