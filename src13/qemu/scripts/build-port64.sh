#!/bin/sh
# build-port64.sh - recompila a libsodos LP64 (firefox/sodos/port/*.c) em
# firefox/build/libcxx64/port, onde o x86_64-sodos-gcc64 a encontra. Mesmos
# flags do firefox/sodos/libcxx64/build.sh, mas so os .c mais novos que o .o.
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
. "$S/scripts/env.sh"
F=$SRC13/firefox
PO=$F/build/libcxx64/port
mkdir -p "$PO"
for f in "$F"/sodos/port/*.c; do
  o=$PO/$(basename "${f%.c}").o
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$FORCE" ]; then
    echo "CC $(basename "$f")"
    "$F/sodos/sodos-cc64" -O2 -g -I "$F/sodos/include" -I "$SRC13/c_compiler/include" -c "$f" -o "$o"
  fi
done
# posix-lite (opendir/stat/...) tambem entra no link LP64.
P=$SRC13/libs/posix-lite/posix.c
o=$F/build/libcxx64/posix64.o
if [ ! -f "$o" ] || [ "$P" -nt "$o" ] || [ -n "$FORCE" ]; then
  echo "CC posix.c"
  "$F/sodos/sodos-cc64" -O2 -g -I "$F/sodos/include" -I "$SRC13/c_compiler/include" -c "$P" -o "$o"
fi
