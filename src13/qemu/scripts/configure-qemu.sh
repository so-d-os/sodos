#!/bin/sh
# configure-qemu.sh - Q0: configura o QEMU para o alvo x86_64-sodos (LP64/SOX2)
# usando o toolchain do port do Firefox. So configura (nao compila): serve para
# listar as dependencias que ainda faltam no SODOS.
#
#   sh scripts/configure-qemu.sh            # i386-softmmu, minimo
#   sh scripts/configure-qemu.sh --foo=bar  # opcoes extras repassadas ao configure
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
SRC13=$(dirname "$S")
V=${QEMU_VERSION:-9.2.0}
Q=$S/vendor/qemu-$V
# Build no ext4 do WSL: no /mnt/c o meson acusa clock skew e o ninja e lento.
B=${QEMU_BUILD_DIR:-$HOME/sodos-build/qemu-$V}
TC=$SRC13/firefox/toolchain/bin
TOOLS=$S/vendor/tools

[ -d "$Q" ] || { echo "falta $Q; rode setup.sh"; exit 1; }
[ -x "$TOOLS/venv/bin/python" ] || { echo "falta o venv; rode setup.sh"; exit 1; }

# pkg-config do alvo: so enxerga o sysroot do SODOS, nunca as libs do WSL.
mkdir -p "$S/build/bin"
cat > "$S/build/bin/x86_64-sodos-pkg-config" <<PKG
#!/bin/sh
export LD_LIBRARY_PATH="$TOOLS/usr/lib/x86_64-linux-gnu\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
PKG_CONFIG_LIBDIR="$S/sodos/sysroot/lib/pkgconfig${SODOS_PROBE_PC:+:$SODOS_PROBE_PC}" PKG_CONFIG_SYSROOT_DIR= exec "$TOOLS/usr/bin/pkgconf" "\$@"
PKG
chmod +x "$S/build/bin/x86_64-sodos-pkg-config"

export PKG_CONFIG=x86_64-sodos-pkg-config
export QEMU_EXTRA_MACHINE_FILE="$S/sodos/cross-sodos.ini"
export PATH="$TC:$S/build/bin:$TOOLS/usr/bin:$PATH"
mkdir -p "$B"
cd "$B"
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
  "$@" 2>&1 | tee "$S/build/configure.log"
