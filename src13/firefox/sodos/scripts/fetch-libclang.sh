#!/bin/sh
# fetch-libclang.sh - libclang 16 e os headers dela (sem root) em vendor/tools,
# para o bindgen do build do browser. A libc++ 18 exige clang >= 16; o Debian 12
# instala o 14. Baixa do repositorio apt ja configurado e so extrai os pacotes.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
V=$S/vendor/tools
VER=${LIBCLANG_VER:-16}
[ -e "$V/usr/lib/llvm-$VER/lib/libclang.so.1" ] && [ -d "$V/usr/lib/llvm-$VER/lib/clang/$VER/include" ] && exit 0
T=$(mktemp -d /tmp/sodos-libclang-XXXXXX)
trap 'rm -rf "$T"' EXIT
(cd "$T" && apt-get download "libclang1-$VER" "libllvm$VER" "libclang-common-$VER-dev")
mkdir -p "$V"
for d in "$T"/*.deb; do dpkg -x "$d" "$V"; done
echo "libclang $VER em $V/usr/lib/llvm-$VER/lib"
