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
# precisa dos patches 0001 a 0009 (patches/README.md) e do std do Rust instalado
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

# bindgen: precisa de uma libclang >= 16 (a libc++ 18 nao compila em clang mais
# antigo) e dos headers de intrinsecos da mesma versao. Ordem: o wheel Python em
# ~/libclang (docs/05-firefox-build.md), a de vendor/tools (sodos/scripts/
# fetch-libclang.sh) e, por ultimo, a do sistema.
EXTRA=
[ -x "$HOME/bin/clang" ] && EXTRA="$EXTRA --with-clang-path=$HOME/bin/clang"
LIBCLANG=$HOME/libclang/libclang-18.1.1.data/platlib/clang/native
CLANG_INC=$S/vendor/tools/usr/lib/llvm-18/lib/clang/18/include
if [ ! -f "$LIBCLANG/libclang.so" ]; then
  LIBCLANG=
  for d in $(ls -d "$S"/vendor/tools/usr/lib/llvm-*/lib /usr/lib/llvm-*/lib 2>/dev/null | sort -t- -k2 -V -r); do
    ls "$d"/libclang.so* >/dev/null 2>&1 || continue
    LIBCLANG=$d   # a versao mais nova
    break
  done
fi
if [ -n "$LIBCLANG" ]; then
  export LIBCLANG_PATH="$LIBCLANG"
  EXTRA="$EXTRA --with-libclang-path=$LIBCLANG"
  inc=$(ls -d "$LIBCLANG"/clang/*/include 2>/dev/null | sort -V | tail -1)
  [ -d "$CLANG_INC" ] || CLANG_INC=$inc
fi
if [ ! -d "$CLANG_INC" ] && command -v clang >/dev/null 2>&1; then
  CLANG_INC=$(clang -print-resource-dir)/include
fi
# O Node so e usado por passos que este build nao executa.
command -v node >/dev/null 2>&1 || command -v nodejs >/dev/null 2>&1 || EXTRA="$EXTRA --disable-nodejs"

# bindgen ve o alvo como Linux, com os mesmos headers do compilador.
export BINDGEN_CFLAGS="-nostdinc -nostdinc++ -D__linux__=1 -D__SODOS__=1 -D_LIBCPP_PSTL_CPU_BACKEND_SERIAL \
-isystem $S/sodos/libcxx64/include -isystem $S/vendor/libcxx-18.1.8/include \
-isystem $S/sodos/libcxx/include -isystem $S/sodos/include \
-isystem $S/../c_compiler/include \
-isystem $CLANG_INC"

# Sem g++ no sistema, os build scripts do Rust acham a libstdc++ do g++ local.
if ! command -v g++ >/dev/null 2>&1; then
  export LIBRARY_PATH="$S/vendor/gxx/usr/lib/gcc/x86_64-linux-gnu/13${LIBRARY_PATH:+:$LIBRARY_PATH}"
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
