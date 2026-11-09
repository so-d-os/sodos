# env.sh - variaveis comuns dos scripts de build (use com ". scripts/env.sh").
S=${S:-$(cd "$(dirname "$0")/.." && pwd)}
SRC13=$(dirname "$S")
SYSROOT=$S/sodos/sysroot
TOOLS=$S/vendor/tools
TC=$SRC13/firefox/toolchain/bin
BUILD_ROOT=${SODOS_BUILD_ROOT:-$HOME/sodos-build}
export PATH="$TC:$S/build/bin:$TOOLS/usr/bin:$PATH"
CC64=x86_64-sodos-gcc64
CFLAGS64="-O2 -I$SYSROOT/include"

# pkg-config do alvo: so enxerga o sysroot do SODOS, nunca as libs do WSL.
# SODOS_PROBE_PC (probe-deps.sh) acrescenta os .pc falsos da sonda.
mkdir -p "$S/build/bin"
cat > "$S/build/bin/x86_64-sodos-pkg-config" <<PKG
#!/bin/sh
export LD_LIBRARY_PATH="$TOOLS/usr/lib/x86_64-linux-gnu\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig\${SODOS_PROBE_PC:+:\$SODOS_PROBE_PC}" PKG_CONFIG_SYSROOT_DIR= exec "$TOOLS/usr/bin/pkgconf" "\$@"
PKG
chmod +x "$S/build/bin/x86_64-sodos-pkg-config"

# meson do tarball do QEMU, instalado no venv das ferramentas.
MESON=$TOOLS/venv/bin/meson
if [ ! -x "$MESON" ]; then
  "$TOOLS/venv/bin/pip" install -q --no-index "$S"/vendor/qemu-*/python/wheels/meson-*.whl
fi
