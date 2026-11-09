#!/bin/sh
# build-qemu.sh - compila o QEMU ja configurado (scripts/configure-qemu.sh).
#   sh scripts/build-qemu.sh          # alvo padrao
#   sh scripts/build-qemu.sh -k 0     # segue apos erros (para medir o que falta)
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
. "$S/scripts/env.sh"
V=${QEMU_VERSION:-9.2.0}
B=${QEMU_BUILD_DIR:-$BUILD_ROOT/qemu-$V}
ninja -C "$B" "$@"
