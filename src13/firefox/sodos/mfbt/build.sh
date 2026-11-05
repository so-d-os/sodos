#!/bin/sh
# build.sh - compila mfbt/Assertions + mozglue Printf + double-conversion
# da arvore do Firefox e liga o T_MFBT.EXE (x32, libc++).
#
#   FIREFOX_DIR=$HOME/ffsrc/firefox-153.6.0esr sh sodos/mfbt/build.sh [--run]
#
# O include de verdade e "mozilla/Vector.h": o build do Gecko exporta mfbt/ e
# mozglue/misc/ sob mozilla/, entao criamos esse diretorio em build/mfbt.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
C=$S/../c_compiler
FF=${FIREFOX_DIR:-$S/vendor/firefox-153.6.0esr}
O=$S/build/mfbt
INC=$O/include
CC=$S/sodos/sodos-cxxlib
CXX=$S/toolchain/bin/x86_64-sodos-g++
LOG=$O/build.log

if [ ! -f "$FF/mfbt/Vector.h" ]; then
  echo "mfbt nao encontrado em $FF (defina FIREFOX_DIR)" >&2
  exit 1
fi

if [ "$1" = "--run-only" ]; then
  cp "$C/out/LIBCX.DLL" "$C/out/POSIXX.DLL" "$S/build/out/SODOS.DLL" "$O"/ 2>/dev/null || true
  cp "$C/out/KERNEL.IMP" "$O"/
  "$C/build/soxrun" -C "$O" T_MFBT.EXE
  exit $?
fi

rm -rf "$O"; mkdir -p "$INC/mozilla" "$O/obj"
ln -s "$FF"/mfbt/* "$INC/mozilla/" 2>/dev/null || true
ln -s "$FF"/mozglue/misc/* "$INC/mozilla/" 2>/dev/null || true
: > "$LOG"

# -include ctype.h: Printf.cpp usa islower sem incluir <ctype.h>.
# emmintrin.h vem do GCC; -include stdint.h supre intptr_t em Assertions.h.
CF="-std=c++20 -O2 -DXP_UNIX -I$INC -I$FF -I$FF/mfbt/double-conversion"
CF="$CF -isystem $(gcc -print-file-name=include) -include stdint.h -include ctype.h"

compile() {
  if ! $CC $CF -c "$1" -o "$2" >>"$LOG" 2>&1; then
    echo "ERRO compilando $(basename "$1")" >&2
    grep -E "error:" "$LOG" | head -8 >&2
    exit 1
  fi
}

compile "$FF/mfbt/Assertions.cpp" "$O/obj/assertions.o"
compile "$FF/mozglue/misc/Printf.cpp" "$O/obj/printf.o"
for f in "$FF"/mfbt/double-conversion/double-conversion/*.cc; do
  case "$(basename "$f")" in string-to-double.cc|strtod.cc) continue ;; esac
  compile "$f" "$O/obj/$(basename "${f%.cc}").o"
done

$CXX -std=c++20 -O2 -I"$INC" -I"$FF" "$S/sodos/mfbt/t_mfbt.cpp" \
  "$O"/obj/*.o -o "$O/T_MFBT.EXE" >>"$LOG" 2>&1 || {
    echo "ERRO ligando T_MFBT" >&2
    grep -E "indefinido|error:" "$LOG" | head -10 >&2
    exit 1
  }

echo "mfbt: $O/T_MFBT.EXE pronto"
if [ "$1" = "--run" ]; then
  cp "$C/out/LIBCX.DLL" "$C/out/POSIXX.DLL" "$S/build/out/SODOS.DLL" "$O"/ 2>/dev/null || true
  cp "$C/out/KERNEL.IMP" "$O"/
  "$C/build/soxrun" -C "$O" T_MFBT.EXE
fi
