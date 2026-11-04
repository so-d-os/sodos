#!/bin/sh
# fetch-libcxx.sh - baixa a libc++ (LLVM) para vendor/libcxx-<versao>.
# Usamos so os headers + as fontes de runtime; o build e o nosso (sodos-cxx).
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
V=${LIBCXX_VERSION:-18.1.8}
T="libcxx-$V.src"
D=$S/vendor/libcxx-$V

if [ -d "$D/include" ]; then
  echo "libc++ $V ja existe em $D"
  exit 0
fi

mkdir -p "$S/vendor"
cd "$S/vendor"
if [ ! -f "$T.tar.xz" ]; then
  echo "baixando libc++ $V"
  curl -fL -o "$T.tar.xz" \
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-$V/$T.tar.xz"
fi
tar xJf "$T.tar.xz"
mv "$T" "libcxx-$V"
echo "ok: $D"
