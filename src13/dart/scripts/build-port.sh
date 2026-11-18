#!/bin/sh
# build-port.sh - compila dart/port/*.c em dart/build/port/*.o (so os mais
# novos que o .o). O wrapper dart/toolchain/bin/x86_64-sodos64-g++ acrescenta
# esses objetos em todo link do Dart.
set -e
D=$(cd "$(dirname "$0")/.." && pwd)
F=$D/../firefox
C=$D/../c_compiler
PO=$D/build/port
mkdir -p "$PO"
for f in "$D"/port/*.c; do
  o=$PO/$(basename "${f%.c}").o
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$D/port/include/sodos_dart_compat.h" -nt "$o" ] || [ -n "$FORCE" ]; then
    echo "CC $(basename "$f")"
    "$F/sodos/sodos-cc64" -O2 -g -Wall -Wextra -Wno-unused-parameter \
      -I "$D/port/include" -I "$F/sodos/include" -I "$F/sodos/port" -I "$C/include" \
      -c "$f" -o "$o"
  fi
done
