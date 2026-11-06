#!/bin/sh
# build-js64.sh - configura e compila o SpiderMonkey (JS.EXE, LP64/SOX2).
#
#   sh sodos/scripts/build-js64.sh configure
#   sh sodos/scripts/build-js64.sh build
#
# Variaveis: FIREFOX_DIR (fonte), FIREFOX_OBJ (objdir), GCC13 (prefixo de um
# gcc >= 13; a libc++ 18 precisa do builtin __is_convertible).
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${FIREFOX_DIR:-$HOME/ffsrc/firefox-153.6.0esr}
OBJ=${FIREFOX_OBJ:-$HOME/ffsrc/obj-js64}
GCC13=${GCC13:-$HOME/ffsrc/gcc-13}
TC=$S/toolchain/bin
RT=$S/sodos/rust/tools

[ -f "$SRC/configure" ] || { echo "Firefox nao encontrado em $SRC" >&2; exit 1; }
if [ -x "$GCC13/bin/g++" ]; then
  export PATH="$GCC13/bin:$PATH"
  export LD_LIBRARY_PATH="$GCC13/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
export PATH="$TC:$RT:$HOME/.cargo/bin:$PATH"
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
# So o configure exige: o build do shell nao precisa do llvm-objdump.
command -v llvm-objdump >/dev/null 2>&1 || export LLVM_OBJDUMP="$(command -v objdump)"

mkdir -p "$OBJ"
cd "$OBJ"

case "${1:-configure}" in
  configure)
    cat > mozconfig <<'EOF'
ac_add_options --target=x86_64-sodos
ac_add_options --enable-application=js
ac_add_options --disable-tests
ac_add_options --disable-jemalloc
ac_add_options --without-system-nspr
ac_add_options --without-system-icu
ac_add_options --without-system-zlib
ac_add_options --disable-shared-js
EOF
    export MOZCONFIG="$OBJ/mozconfig"
    if "$SRC/configure" > configure.log 2>&1; then
      echo "configure OK ($OBJ)"
    else
      tail -15 configure.log
      echo "log: $OBJ/configure.log" >&2
      exit 1
    fi
    ;;
  build)
    gmake -j"$(nproc)" > build.log 2>&1 || {
      grep -n -E 'error|Error [0-9]|No such file' build.log | head -20
      echo "log: $OBJ/build.log" >&2
      exit 1
    }
    ls -la dist/bin/js
    ;;
  *)
    echo "uso: $0 configure|build" >&2
    exit 2
    ;;
esac
