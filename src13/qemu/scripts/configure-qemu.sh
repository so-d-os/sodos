#!/bin/sh
# configure-qemu.sh - configura o QEMU para o alvo x86_64-sodos (LP64/SOX2)
# com o toolchain do port do Firefox e as libs de sodos/sysroot (glib, zlib).
#
#   sh scripts/configure-qemu.sh            # i386-softmmu, minimo
#   sh scripts/configure-qemu.sh --foo=bar  # opcoes extras repassadas ao configure
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
. "$S/scripts/env.sh"
V=${QEMU_VERSION:-9.2.0}
Q=$S/vendor/qemu-$V
# Build no ext4 do WSL: no /mnt/c o meson acusa clock skew e o ninja e lento.
B=${QEMU_BUILD_DIR:-$BUILD_ROOT/qemu-$V}

[ -d "$Q" ] || { echo "falta $Q; rode setup.sh"; exit 1; }
[ -x "$TOOLS/venv/bin/python" ] || { echo "falta o venv; rode setup.sh"; exit 1; }

export PKG_CONFIG=x86_64-sodos-pkg-config
export QEMU_EXTRA_MACHINE_FILE="$S/sodos/cross-sodos.ini"
# Dispositivos: sodos/devices/i386-sodos.mak (o QEMU so procura em configs/).
cp "$S/sodos/devices/i386-sodos.mak" "$Q/configs/devices/i386-softmmu/sodos.mak"
mkdir -p "$B"
cd "$B"
# ucontext: sodos_ucontext.c; o backend sigaltstack precisaria de sinais.
"$Q/configure" \
  --python="$TOOLS/venv/bin/python" \
  --cc=x86_64-sodos-gcc64 \
  --cxx=x86_64-sodos-g++64 \
  --host-cc=gcc \
  --cross-prefix= \
  --objcc=false \
  --target-list=i386-softmmu \
  --without-default-features \
  --without-default-devices \
  --disable-docs \
  --disable-tools \
  --disable-guest-agent \
  --disable-werror \
  --enable-tcg \
  --disable-install-blobs \
  --with-coroutine=ucontext \
  --with-devices-i386=sodos \
  --extra-cflags="-I$SYSROOT/include -idirafter $SYSROOT/include-uapi" \
  "$@" > "$S/build/configure.log" 2>&1 || { tail -15 "$S/build/configure.log"; exit 1; }
tail -3 "$S/build/configure.log"
