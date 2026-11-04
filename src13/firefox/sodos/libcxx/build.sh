#!/bin/sh
# build.sh - compila a libc++ (subconjunto) e os testes para o SODOS.
#
#   build.sh             compila LIBCXX.LIB + STLTEST.EXE + THRTEST.EXE
#   build.sh --run       compila e roda os dois testes no soxrun
#   build.sh --run-only  so roda (o Makefile usa depois de "make libcxx")
#
# A saida do compilador vai para build/libcxx/build.log; o terminal so ve
# erros ou o resultado dos testes.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
C=$S/../c_compiler
L=$S/vendor/libcxx-18.1.8
O=$S/build/libcxx/obj
T=$S/build/libcxx
CC=$S/sodos/sodos-cxxlib
LOG=$T/build.log

BUILD=1
RUN=0
case "$1" in
  --run) RUN=1 ;;
  --run-only) BUILD=0; RUN=1 ;;
esac

mkdir -p "$O" "$T"

if [ "$BUILD" = 1 ]; then
  # Fontes do runtime da libc++ necessarias para containers/string/threads.
  # iostream/locale/atomic.cpp (futex) ficam para as proximas fases.
  SRCS="string.cpp algorithm.cpp stdexcept.cpp exception.cpp verbose_abort.cpp \
new.cpp new_handler.cpp new_helpers.cpp typeinfo.cpp memory.cpp functional.cpp \
hash.cpp vector.cpp optional.cpp any.cpp chrono.cpp \
system_error.cpp error_category.cpp \
thread.cpp mutex.cpp mutex_destructor.cpp condition_variable.cpp \
condition_variable_destructor.cpp call_once.cpp shared_mutex.cpp future.cpp"

  CFLAGS="-std=c++20 -O2 -g -D_LIBCPP_BUILDING_LIBRARY \
 -D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS -fno-exceptions -fno-rtti -I$L/src \
 -Wno-deprecated-declarations -Wno-alloc-size-larger-than -Wno-return-type \
 -Wno-noreturn -Wno-cpp"

  rm -rf "$O"
  mkdir -p "$O"
  : > "$LOG"

  echo "libc++: compilando $(echo $SRCS | wc -w) fontes..."
  for f in $SRCS; do
    if ! $CC $CFLAGS -c "$L/src/$f" -o "$O/${f%.cpp}.o" >>"$LOG" 2>&1; then
      echo "ERRO compilando $f:" >&2
      grep -E "error:" "$LOG" | head -10 >&2
      exit 1
    fi
  done

  OBJS=$(ls "$O"/*.o)

  if ! $S/sodos/sodos-cc -O2 -c "$C/lib/fpconv.c" -o "$T/fpconv.o" >>"$LOG" 2>&1; then
    echo "ERRO compilando fpconv.c" >&2
    exit 1
  fi
  if ! $S/sodos/sodos-cc -O2 -c "$C/lib/math.c" -o "$T/mathx.o" >>"$LOG" 2>&1; then
    echo "ERRO compilando math.c" >&2
    exit 1
  fi
  if ! $S/sodos/sodos-cxx -O2 -c "$S/sodos/cxx/cxx_runtime.cpp" -o "$T/cxx_runtime.o" >>"$LOG" 2>&1; then
    echo "ERRO compilando cxx_runtime.cpp" >&2
    exit 1
  fi
  if ! gcc -mx32 -c "$S/sodos/libcxx/override_stub.S" -o "$T/override_stub.o" >>"$LOG" 2>&1; then
    echo "ERRO compilando override_stub.S" >&2
    exit 1
  fi

  TFLAGS="-std=c++20 -O2 -g -Wall -Wno-alloc-size-larger-than"
  if ! $CC $TFLAGS -c "$S/sodos/libcxx/t_stl.cpp" -o "$T/t_stl.o" >>"$LOG" 2>&1; then
    echo "ERRO compilando STLTEST" >&2
    grep -E "error:" "$LOG" | head -10 >&2
    exit 1
  fi
  if ! $CC $TFLAGS -c "$S/sodos/libcxx/t_stl_thread.cpp" -o "$T/t_stl_thread.o" >>"$LOG" 2>&1; then
    echo "ERRO compilando THRTEST" >&2
    grep -E "error:" "$LOG" | head -10 >&2
    exit 1
  fi

  $C/build/link /lib /out:"$T/LIBCXX.LIB" $OBJS
  $C/build/link /out:"$T/STLTEST.EXE" "$C/build/x_crt0.o" "$T/cxx_runtime.o" \
    "$T/t_stl.o" "$T/override_stub.o" "$T/fpconv.o" "$T/mathx.o" \
    "$S/build/out/SODOS.DLL" "$C/out/POSIXX.DLL" "$C/out/LIBCX.DLL" \
    "$C/out/KERNEL.IMP" "$T/LIBCXX.LIB"
  $C/build/link /out:"$T/THRTEST.EXE" "$C/build/x_crt0.o" "$T/cxx_runtime.o" \
    "$T/t_stl_thread.o" "$T/override_stub.o" "$T/fpconv.o" "$T/mathx.o" \
    "$S/build/out/SODOS.DLL" "$C/out/POSIXX.DLL" "$C/out/LIBCX.DLL" \
    "$C/out/KERNEL.IMP" "$T/LIBCXX.LIB"

  cp "$C/out/LIBCX.DLL" "$C/out/KERNEL.IMP" "$T"/
  cp "$S/build/out/SODOS.DLL" "$C/out/POSIXX.DLL" "$T"/
  echo "libc++: LIBCXX.LIB, STLTEST.EXE e THRTEST.EXE prontos"
fi

if [ "$RUN" = 1 ]; then
  "$C/build/soxrun" -C "$T" STLTEST.EXE
  "$C/build/soxrun" -C "$T" THRTEST.EXE
fi
