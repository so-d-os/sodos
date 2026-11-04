#!/bin/sh
# fetch-nspr.sh - baixa e confere o NSPR 4.35 em vendor/ (mesmo codigo que o
# Firefox carrega em nsprpub/). O tarball e pequeno; o fonte extraido fica
# fora do git para nao inflar o repositorio.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
V=4.35
T="nspr-$V"
URL="https://ftp.mozilla.org/pub/nspr/releases/v$V/src/$T.tar.gz"
SHA="7ea3297ea5969b5d25a5dd8d47f2443cda88e9ee746301f6e1e1426f8a6abc8f"

mkdir -p "$S/vendor"
cd "$S/vendor"

if [ ! -f "$T.tar.gz" ]; then
  echo "baixando $URL"
  if command -v curl >/dev/null 2>&1; then
    curl -fL -o "$T.tar.gz" "$URL"
  else
    wget -O "$T.tar.gz" "$URL"
  fi
fi

echo "$SHA  $T.tar.gz" | sha256sum -c -

if [ ! -d "$T" ]; then
  echo "extraindo $T"
  tar xzf "$T.tar.gz"
fi

echo "ok: $S/vendor/$T"
