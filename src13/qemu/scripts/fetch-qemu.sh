#!/bin/sh
# fetch-qemu.sh - baixa e extrai o QEMU em vendor/ (fora do git).
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
V=${QEMU_VERSION:-9.2.0}
T="qemu-$V"
URL="https://download.qemu.org/$T.tar.xz"

mkdir -p "$S/vendor"
cd "$S/vendor"
if [ ! -f "$T.tar.xz" ]; then
  echo "baixando $URL"
  curl -fL -o "$T.tar.xz.part" "$URL"
  mv "$T.tar.xz.part" "$T.tar.xz"
fi
# O SHA-256 fica registrado em SHA256SUMS na primeira vez e e conferido depois.
if grep -q " $T.tar.xz\$" SHA256SUMS 2>/dev/null; then
  grep " $T.tar.xz\$" SHA256SUMS | sha256sum -c -
else
  sha256sum "$T.tar.xz" >> SHA256SUMS
fi
if [ ! -d "$T" ]; then
  echo "extraindo $T"
  tar xJf "$T.tar.xz"
  for p in "$S"/patches/*.patch; do
    [ -f "$p" ] || continue
    echo "aplicando $(basename "$p")"
    patch -d "$T" -p1 < "$p"
  done
fi
echo "ok: $S/vendor/$T"
