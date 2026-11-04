#!/bin/sh
# link.sh - liga o NSPR + teste num PRTEST.EXE SOX e roda no soxrun.
#   sh sodos/nspr/link.sh [--run]
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
C=$S/../c_compiler
N=$S/vendor/nspr-4.35/nspr
O=$S/build/nspr/obj
T=$S/build/nspr/testobj
R=$S/build/nspr/run
CC=$S/sodos/sodos-cc

CFLAGS="-O2 -g -Wall -Wno-unused-parameter -DHAVE_LONG_LONG -I$S/sodos/nspr/include -I$S/sodos/include -I$C/include -I$N/pr/include"

mkdir -p "$T" "$R"
$CC $CFLAGS -c "$S/sodos/nspr/tests/t_nspr.c" -o "$T/t_nspr.o"

OBJS=$(find "$O" -name '*.o' | sort)
$C/build/link /out:"$S/build/nspr/PRTEST.EXE" /map \
  "$C/build/x_crt0.o" "$T/t_nspr.o" $OBJS \
  "$S/build/out/SODOS.DLL" "$C/out/POSIXX.DLL" "$C/out/LIBCX.DLL" \
  "$C/out/KERNEL.IMP"

cp "$S/build/nspr/PRTEST.EXE" "$S/build/out/SODOS.DLL" \
   "$C/out/POSIXX.DLL" "$C/out/LIBCX.DLL" "$C/out/KERNEL.IMP" "$R"/
echo "PRTEST.EXE pronto em $S/build/nspr/PRTEST.EXE"
if [ "$1" = "--run" ]; then
  "$C/build/soxrun" -C "$R" PRTEST.EXE
fi
