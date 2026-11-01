#!/bin/sh
# Baixa o GNU Midnight Commander 4.8.33 em src13/vendor/mc (host only).
# Uso: a partir de src13/  ->  ./content/mc/FETCH.sh
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
DEST="$ROOT/vendor/mc"
TAG=4.8.33
URL=https://github.com/MidnightCommander/mc.git

mkdir -p "$ROOT/vendor"
if [ -d "$DEST/.git" ]; then
  echo "ja existe: $DEST"
  (cd "$DEST" && git fetch --depth 1 origin "refs/tags/$TAG:refs/tags/$TAG" && git checkout -q "$TAG" && git rev-parse --short HEAD)
  exit 0
fi
rm -rf "$DEST"
git clone --depth 1 --branch "$TAG" "$URL" "$DEST"
(cd "$DEST" && echo "OK $TAG $(git rev-parse --short HEAD)")
