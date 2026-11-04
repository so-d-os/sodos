#!/bin/sh
# build.sh - compila e liga o teste C++ minimo (CXXTEST.EXE) e roda no soxrun.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
C=$S/../c_compiler
O=$S/build/cxx
CC=$S/sodos/sodos-cxx

mkdir -p "$O"
rm -f "$O"/*.o "$O"/CXXTEST.EXE

$CC -O2 -g -Wall -Wextra -Wno-unused-parameter -c "$S/sodos/cxx/cxx_runtime.cpp" -o "$O/cxx_runtime.o"
$CC -O2 -g -Wall -Wextra -Wno-unused-parameter -c "$S/sodos/cxx/t_cxx.cpp" -o "$O/t_cxx.o"

$C/build/link /out:"$O/CXXTEST.EXE" "$C/build/x_crt0.o" \
  "$O/cxx_runtime.o" "$O/t_cxx.o" "$C/out/LIBCX.DLL" "$C/out/KERNEL.IMP"

cp "$C/out/LIBCX.DLL" "$C/out/KERNEL.IMP" "$O"/
echo "CXXTEST.EXE pronto: $O/CXXTEST.EXE"
if [ "$1" = "--run" ]; then
  "$C/build/soxrun" -C "$O" CXXTEST.EXE
fi
