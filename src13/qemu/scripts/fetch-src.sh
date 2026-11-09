#!/bin/sh
# fetch-src.sh URL [DIR] - baixa um tarball para vendor/, registra/confere o
# SHA-256 em vendor/SHA256SUMS e extrai. DIR e o nome do diretorio extraido
# (padrao: nome do arquivo sem .tar.*). Aplica patches/<DIR>/*.patch.
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
URL=$1
F=$(basename "$URL")
DIR=${2:-$(echo "$F" | sed 's/\.tar\..*//')}
mkdir -p "$S/vendor"
cd "$S/vendor"
if [ ! -f "$F" ]; then
  echo "baixando $URL"
  curl -fL -o "$F.part" "$URL"
  mv "$F.part" "$F"
fi
if grep -q " $F\$" SHA256SUMS 2>/dev/null; then
  grep " $F\$" SHA256SUMS | sha256sum -c - >/dev/null
else
  sha256sum "$F" >> SHA256SUMS
fi
if [ ! -d "$DIR" ]; then
  echo "extraindo $F"
  tar xf "$F"
  for p in "$S/patches/$DIR"/*.patch; do
    [ -f "$p" ] || continue
    echo "aplicando $(basename "$p")"
    patch -d "$DIR" -p1 < "$p"
  done
fi
echo "$S/vendor/$DIR"
