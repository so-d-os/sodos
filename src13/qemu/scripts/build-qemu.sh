#!/bin/sh
# build-qemu.sh - compila o QEMU ja configurado (scripts/configure-qemu.sh).
#   sh scripts/build-qemu.sh          # so o qemu-system-i386 (os testes de fp nao linkam)
#   sh scripts/build-qemu.sh -k 0     # segue apos erros (para medir o que falta)
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
. "$S/scripts/env.sh"
V=${QEMU_VERSION:-9.2.0}
B=${QEMU_BUILD_DIR:-$BUILD_ROOT/qemu-$V}
[ $# -gt 0 ] || set -- qemu-system-i386
ninja -C "$B" "$@"
