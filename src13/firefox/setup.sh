#!/bin/sh
# setup.sh - baixa as arvores upstream para vendor/ (fora do git).
#
#   ./setup.sh          NSPR 4.35 (M1)
#   ./setup.sh firefox  Firefox ESR (arvore inteira, ~1 GB; so quando for usar)
#
# O Firefox em si ainda nao e compilado por este Makefile; ver docs/03-fases.md.
set -e
S=$(cd "$(dirname "$0")" && pwd)
sh "$S/sodos/scripts/fetch-nspr.sh"
# g++ local (sem root) para o runtime C++/CXXTEST. Se apt-get nao existir,
# o build C++ avisa; o resto do port nao depende dele.
if [ ! -x "$S/vendor/gxx/usr/bin/x86_64-linux-gnu-g++-13" ] && command -v apt-get >/dev/null 2>&1; then
  sh "$S/sodos/scripts/fetch-gxx.sh" || echo "aviso: nao foi possivel baixar o g++ local"
fi
# libc++ (headers + fontes do runtime) para o M2b.
sh "$S/sodos/scripts/fetch-libcxx.sh" || echo "aviso: nao foi possivel baixar a libc++"
case "$1" in
  firefox) echo "Firefox ESR: ainda nao habilitado; veja docs/03-fases.md (M2+)." ;;
esac
echo "pronto: $S/vendor/nspr-4.35"
