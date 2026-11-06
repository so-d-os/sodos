#!/bin/sh
# build-browser64.sh - configura e compila o browser (libxul) em LP64/SOX2.
#
#   sh sodos/scripts/build-browser64.sh configure
#   sh sodos/scripts/build-browser64.sh build [alvo...]
#
# Rode "build pre-export export" antes de "build compile", em chamadas separadas:
# no mesmo gmake -j, o compile nao espera os headers gerados pelo export.
#
# Copia sodos/widget para widget/sodos na arvore antes de cada passo. A arvore
# precisa dos patches 0001 a 0005 (patches/README.md) e do std do Rust instalado
# (make js64-std).
#
# Variaveis: FIREFOX_DIR (fonte), FIREFOX_OBJ (objdir), GCC13 (prefixo de um
# gcc >= 13), MAKE_ARGS (o padrao usa -k para listar todos os erros de uma vez).
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${FIREFOX_DIR:-$HOME/ffsrc/firefox-153.6.0esr}
OBJ=${FIREFOX_OBJ:-$HOME/ffsrc/obj-browser64}
GCC13=${GCC13:-$HOME/ffsrc/gcc-13}
TC=$S/toolchain/bin
RT=$S/sodos/rust/tools
TOOLS=$S/vendor/tools/usr/bin
TOOLLIB=$S/vendor/tools/usr/lib/x86_64-linux-gnu

[ -f "$SRC/configure" ] || { echo "Firefox nao encontrado em $SRC" >&2; exit 1; }
if [ -x "$GCC13/bin/g++" ]; then
  export PATH="$GCC13/bin:$PATH"
  export LD_LIBRARY_PATH="$GCC13/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
export PATH="$TC:$RT:$TOOLS:$HOME/.cargo/bin:$HOME/node/bin:$HOME/bin:$PATH"
export LD_LIBRARY_PATH="$TOOLLIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[ -x "$TOOLS/pkg-config" ] && export PKG_CONFIG="$TOOLS/pkg-config"
export MOZBUILD_STATE_PATH="${MOZBUILD_STATE_PATH:-$HOME/.mozbuild}"
export CC="$TC/x86_64-sodos-gcc64"
export CXX="$TC/x86_64-sodos-g++64"
export HOST_CC=gcc
if [ -z "$HOST_CXX" ]; then
  if command -v g++ >/dev/null 2>&1; then HOST_CXX=g++; else HOST_CXX="$HOME/bin/gxx"; fi
fi
export HOST_CXX
export RUSTC="$RT/rustc"
export CARGO="$RT/cargo"
# So o configure exige: o build nao depende do llvm-objdump.
command -v llvm-objdump >/dev/null 2>&1 || export LLVM_OBJDUMP="$(command -v objdump)"

# bindgen: clang de fachada e libclang do wheel Python (docs/05-firefox-build.md).
LIBCLANG=$HOME/libclang/libclang-18.1.1.data/platlib/clang/native
EXTRA=
[ -x "$HOME/bin/clang" ] && EXTRA="$EXTRA --with-clang-path=$HOME/bin/clang"
if [ -f "$LIBCLANG/libclang.so" ]; then
  export LIBCLANG_PATH="$LIBCLANG"
  EXTRA="$EXTRA --with-libclang-path=$LIBCLANG"
fi

# cp -p mantem as datas: arquivo igual nao recompila.
rm -rf "$SRC/widget/sodos"
mkdir -p "$SRC/widget/sodos"
cp -rp "$S/sodos/widget/." "$SRC/widget/sodos/"

mkdir -p "$OBJ"
cd "$OBJ"
export MOZCONFIG="$S/mozconfig/firefox-sodos.mozconfig"

case "${1:-configure}" in
  configure)
    if "$SRC/configure" $EXTRA > configure.log 2>&1; then
      echo "configure OK ($OBJ)"
    else
      tail -15 configure.log
      echo "log: $OBJ/configure.log" >&2
      exit 1
    fi
    ;;
  build)
    shift
    gmake -j"$(nproc)" ${MAKE_ARGS:--k} "$@" > build.log 2>&1 || {
      grep -n -E ' error: |fatal error|Error [0-9]+$' build.log | head -40
      echo "log: $OBJ/build.log" >&2
      exit 1
    }
    echo "build OK ($OBJ)"
    ;;
  *)
    echo "uso: $0 configure|build [alvo...]" >&2
    exit 2
    ;;
esac
