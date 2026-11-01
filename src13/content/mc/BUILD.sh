#!/bin/sh
# 1) Host ELF do GNU MC (referencia) em vendor/mc-build/
# 2) MC.EXE do SODOS (hibrido) em c_compiler/out/
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SRC="$ROOT/vendor/mc"
BLD="$ROOT/vendor/mc-build"
CCDIR="$ROOT/c_compiler"

if [ ! -f "$SRC/configure.ac" ]; then
  echo "faltam fontes: rode ./content/mc/FETCH.sh" >&2
  exit 1
fi

if [ ! -x "$SRC/configure" ]; then
  (cd "$SRC" && ./autogen.sh)
fi

mkdir -p "$BLD"
cd "$BLD"
if [ ! -f Makefile ]; then
  ../mc/configure \
    --disable-vfs \
    --without-subshell \
    --without-internal-edit \
    --without-diff-viewer \
    --disable-nls \
    --without-x \
    --without-gpm-mouse \
    --disable-background \
    --with-screen=ncurses \
    --prefix=/tmp/mc-sodos-host
fi
make -j"$(nproc 2>/dev/null || echo 2)"
echo "OK host: $BLD/src/mc (ELF Linux)"

echo "=== SODOS MC.EXE ==="
make -C "$CCDIR" out/MC.EXE
"$CCDIR/build/soxrun" -C "$CCDIR/out" MC.EXE auto
echo "OK SODOS: $CCDIR/out/MC.EXE"
