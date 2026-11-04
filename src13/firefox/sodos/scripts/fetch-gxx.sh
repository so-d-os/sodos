#!/bin/sh
# fetch-gxx.sh - instala um g++ local (sem root) em vendor/gxx.
#
# O WSL do projeto tem gcc mas nao g++/cc1plus. Baixamos o .deb do g++
# (Ubuntu) e extraimos com dpkg-deb; o sodos-cxx usa esse compilador.
# Precisa de apt-get (Ubuntu/Debian) e rede.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
D=$S/vendor/gxx
T=$S/build/gxx-deb

if [ -x "$D/usr/bin/x86_64-linux-gnu-g++-13" ]; then
  echo "g++ local ja existe em $D"
  exit 0
fi

mkdir -p "$T" "$D"
cd "$T"
if ! ls *.deb >/dev/null 2>&1; then
  apt-get download g++-13-x86-64-linux-gnu
fi
for deb in *.deb; do
  dpkg-deb -x "$deb" "$D"
done
echo "g++ local em $D/usr/bin/x86_64-linux-gnu-g++-13"
