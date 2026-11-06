#!/bin/sh
# build.sh - libc++ 64-bit (SOX2, sem threads) + STLTEST64.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
C=$S/../c_compiler
L=$S/vendor/libcxx-18.1.8
O=$S/build/libcxx64/obj
T=$S/build/libcxx64
CC=$S/sodos/sodos-cxxlib64
LOG=$T/build.log

mkdir -p "$O" "$T"
: > "$LOG"

# libc e crt0 64-bit (alvo do c_compiler)
make -C "$C" hello64c >/dev/null

SRCS="string.cpp algorithm.cpp stdexcept.cpp exception.cpp verbose_abort.cpp \
new.cpp new_handler.cpp new_helpers.cpp typeinfo.cpp memory.cpp functional.cpp \
hash.cpp vector.cpp optional.cpp any.cpp system_error.cpp error_category.cpp \
thread.cpp mutex.cpp mutex_destructor.cpp condition_variable.cpp \
condition_variable_destructor.cpp call_once.cpp shared_mutex.cpp future.cpp \
locale.cpp ios.cpp ios.instantiations.cpp iostream.cpp ostream.cpp \
fstream.cpp chrono.cpp regex.cpp"

CFLAGS="-std=c++20 -O2 -g -D_LIBCPP_BUILDING_LIBRARY \
 -D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS -fno-exceptions -I$L/src \
 -Wno-deprecated-declarations -Wno-alloc-size-larger-than -Wno-return-type \
 -Wno-noreturn -Wno-cpp"

rm -rf "$O"
mkdir -p "$O"
echo "libc++64: compilando $(echo $SRCS | wc -w) fontes..."
for f in $SRCS; do
  if ! $CC $CFLAGS -c "$L/src/$f" -o "$O/${f%.cpp}.o" >>"$LOG" 2>&1; then
    echo "ERRO compilando $f:" >&2
    grep -E "error:" "$LOG" | head -8 >&2
    exit 1
  fi
done

if ! $CC -std=c++20 -O2 -g -Wall -c "$S/sodos/libcxx64/t_stl64.cpp" -o "$T/t_stl64.o" >>"$LOG" 2>&1; then
  echo "ERRO compilando t_stl64.cpp" >&2
  grep -E "error:" "$LOG" | head -8 >&2
  exit 1
fi
if ! $CC -std=c++20 -O2 -g -Wall -c "$S/sodos/libcxx64/t_stl_thread64.cpp" -o "$T/t_stl_thread64.o" >>"$LOG" 2>&1; then
  echo "ERRO compilando t_stl_thread64.cpp" >&2
  grep -E "error:" "$LOG" | head -8 >&2
  exit 1
fi
if ! $CC -O2 -c "$S/sodos/cxx/cxx_runtime.cpp" -o "$T/cxx_runtime64.o" >>"$LOG" 2>&1; then
  echo "ERRO compilando cxx_runtime64" >&2
  grep -E "error:" "$LOG" | head -8 >&2
  exit 1
fi
gcc -m64 -c "$S/sodos/libcxx/override_stub.S" -o "$T/override_stub64.o" >>"$LOG" 2>&1
gcc -m64 -c "$S/sodos/libcxx64/init_array_stub.S" -o "$T/init_array_stub64.o" >>"$LOG" 2>&1

OBJS=$(ls "$O"/*.o)
LIBC64=$(ls "$C"/build/x64_*.o)
# libc++ com threads usa os pthreads cooperativos da libsodos64.
PO=$T/port
rm -rf "$PO"; mkdir -p "$PO"
for f in "$S"/sodos/port/*.c; do
  $S/sodos/sodos-cc64 -O2 -g -I "$S/sodos/include" -I "$C/include" \
    -c "$f" -o "$PO/$(basename "${f%.c}").o" >>"$LOG" 2>&1 || {
    echo "ERRO compilando $f:" >&2
    grep -E "error:" "$LOG" | tail -8 >&2
    exit 1
  }
done
$S/sodos/sodos-cc64 -O2 -g -I "$S/sodos/include" -I "$C/include" \
  -c "$S/../libs/posix-lite/posix.c" -o "$T/posix64.o" >>"$LOG" 2>&1
$C/build/link64 /out:"$T/STLTEST64.EXE" \
  "$C/build/x_crt0_64libc.o" "$T/cxx_runtime64.o" "$T/t_stl64.o" \
  "$T/override_stub64.o" "$T/init_array_stub64.o" $OBJS $LIBC64 "$PO"/*.o \
  "$T/posix64.o" "$C/out/KERNEL.IMP"

$C/build/link64 /out:"$T/THRTEST64.EXE" \
  "$C/build/x_crt0_64libc.o" "$T/cxx_runtime64.o" "$T/t_stl_thread64.o" \
  "$T/override_stub64.o" "$T/init_array_stub64.o" $OBJS $LIBC64 "$PO"/*.o \
  "$T/posix64.o" "$C/out/KERNEL.IMP"

echo "libc++64: STLTEST64.EXE pronto"
