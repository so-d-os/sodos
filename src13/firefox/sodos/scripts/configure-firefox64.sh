#!/bin/sh
# configure-firefox64.sh - roda o configure do Firefox com o toolchain LP64.
#
#   FIREFOX_DIR=$HOME/ffsrc/firefox-153.6.0esr sh sodos/scripts/configure-firefox64.sh
#
# Hoje ele passa compilador/bitness/linker/pkg-config e para em Rust.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${FIREFOX_DIR:-$S/vendor/firefox-153.6.0esr}
OBJ=${FIREFOX_OBJ:-$SRC/obj-sodos64}
TC=$S/toolchain/bin
TOOLS=$S/vendor/tools/usr/bin
TOOLLIB=$S/vendor/tools/usr/lib/x86_64-linux-gnu

[ -f "$SRC/configure" ] || { echo "Firefox nao encontrado em $SRC" >&2; exit 1; }
mkdir -p "$OBJ"
cd "$OBJ"

cat > mozconfig <<'EOF'
ac_add_options --target=x86_64-sodos
ac_add_options --enable-application=browser
ac_add_options --disable-tests
ac_add_options --disable-jemalloc
ac_add_options --without-system-nspr
ac_add_options --without-system-nss
ac_add_options --without-system-icu
ac_add_options --disable-audio-backends
EOF

export MOZCONFIG="$OBJ/mozconfig"
export MOZBUILD_STATE_PATH="${MOZBUILD_STATE_PATH:-$HOME/.mozbuild}"
export PATH="$TC:$TOOLS:$HOME/.cargo/bin:$HOME/node/bin:$HOME/bin:$PATH"
export LD_LIBRARY_PATH="$TOOLLIB:${LD_LIBRARY_PATH:-}"
[ -x "$TOOLS/pkg-config" ] && export PKG_CONFIG="$TOOLS/pkg-config"
export CC="$TC/x86_64-sodos-gcc64"
export CXX="$TC/x86_64-sodos-g++64"
export HOST_CC=${HOST_CC:-gcc}
if [ -z "$HOST_CXX" ]; then
  if [ -x "$HOME/bin/gxx" ]; then HOST_CXX="$HOME/bin/gxx"; else HOST_CXX=g++; fi
fi
export HOST_CXX

CLANG_ARG=
LIBCLANG_ARG=
if [ -x "$HOME/bin/clang" ]; then
  export LIBCLANG_PATH="$HOME/libclang/libclang-18.1.1.data/platlib/clang/native"
  CLANG_ARG="--with-clang-path=$HOME/bin/clang"
fi
if [ -f "$HOME/libclang/libclang-18.1.1.data/platlib/clang/native/libclang.so" ]; then
  LIBCLANG_ARG="--with-libclang-path=$HOME/libclang/libclang-18.1.1.data/platlib/clang/native"
fi

if "$SRC/configure" --target=x86_64-sodos $CLANG_ARG $LIBCLANG_ARG > configure.log 2>&1; then
  echo "configure OK (inesperado nesta fase)"
  exit 0
fi
grep -E "ERROR|not found|Rust|rustc|cargo|clang|Package|gtk" configure.log | head -15
echo "log: $OBJ/configure.log"
exit 1
