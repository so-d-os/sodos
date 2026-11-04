#!/bin/sh
# fetch-firefox.sh - baixa e extrai o fonte do Firefox ESR.
#
#   FIREFOX_VERSION=153.6.0esr sh sodos/scripts/fetch-firefox.sh
#   FIREFOX_DIR=/root/ffsrc/firefox-153.6.0esr sh ...   # fora do DrvFs
#
# O tarball tem ~800 MB e extrai ~3 GB. Extrair em /mnt/c (DrvFs) e MUITO
# lento (milhares de arquivos pequenos); prefira um diretorio no ext4 do WSL
# via FIREFOX_DIR e deixe so o tarball em vendor/.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
V=${FIREFOX_VERSION:-153.6.0esr}
T="firefox-$V"
D=${FIREFOX_DIR:-$S/vendor/$T}
TARBALL=$S/vendor/$T.source.tar.xz

if [ -f "$D/configure.py" ]; then
  echo "Firefox $V ja existe em $D"
  exit 0
fi

mkdir -p "$S/vendor"
if [ ! -f "$TARBALL" ]; then
  echo "baixando Firefox $V (~800 MB)"
  curl -fL --retry 3 --retry-delay 5 -o "$TARBALL" \
    "https://ftp.mozilla.org/pub/firefox/releases/$V/source/$T.source.tar.xz"
fi

mkdir -p "$(dirname "$D")"
cd "$(dirname "$D")"
if [ ! -d "$D" ]; then
  echo "extraindo $T em $(pwd)"
  tar xJf "$TARBALL"
  # O tarball extrai "firefox-153.6.0" (sem o sufixo esr).
  if [ ! -d "$D" ]; then
    for d in firefox-*; do
      [ -d "$d" ] || continue
      [ "$d" = "$(basename "$TARBALL")" ] && continue
      mv "$d" "$D"
      break
    done
  fi
fi
echo "ok: $D"
