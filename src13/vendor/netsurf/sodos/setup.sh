#!/bin/sh
# Prepara a arvore do NetSurf para o SODOS: baixa o netsurf-all-3.11 oficial,
# extrai em tree/ e aplica netsurf-3.11-sodos.patch. Tambem extrai o gperf
# (pacote Debian, sem root) em hosttools/. Depois: build.sh e link.sh.
set -e
S=$(cd "$(dirname "$0")" && pwd)
N=$S/..
TARBALL=netsurf-all-3.11.tar.gz
SHA=4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b
cd "$N"
if [ ! -f "$TARBALL" ]; then
  curl -sSfLO "https://download.netsurf-browser.org/netsurf/releases/source-full/$TARBALL"
fi
echo "$SHA  $TARBALL" | sha256sum -c -
if [ -e tree ]; then
  echo "tree/ ja existe (apague para refazer)"
else
  mkdir x
  tar xzf "$TARBALL" -C x
  mv x/netsurf-all-3.11 tree
  rmdir x
  (cd tree && patch -p1 < "$S/netsurf-3.11-sodos.patch")
fi
if ! command -v gperf > /dev/null && [ ! -x hosttools/root/usr/bin/gperf ]; then
  mkdir -p hosttools
  (cd hosttools && apt-get download gperf && dpkg -x gperf_*.deb root)
fi
echo "ok: agora sodos/build.sh e sodos/link.sh"
