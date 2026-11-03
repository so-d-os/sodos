#!/bin/sh
# Junta o NetSurf compilado (build.sh) num NETSURF.EXE do SODOS.
set -e
S=$(cd "$(dirname "$0")" && pwd)
N=$S/..
C=$S/../../../c_compiler
O=$S/obj
mkdir -p "$O/libs"
for a in "$N"/tree/inst-framebuffer/lib/*.a; do
  n=$(basename "$a" .a)
  rm -rf "$O/libs/$n" && mkdir -p "$O/libs/$n"
  (cd "$O/libs/$n" && ar x "$a")
  "$C/build/link" /lib /out:"$O/$n.lib" "$O"/libs/"$n"/*.o > /dev/null
done
python3 "$S/genres.py" "$N/tree/netsurf/frontends/framebuffer/res" "$O/sodos_res.c"
"$S/sodos-cc" -O2 -c "$O/sodos_res.c" -o "$O/sodos_res.o"
"$S/sodos-cc" -O2 -Wall -c "$S/shim/iconv.c" -o "$O/iconv.o"
"$S/sodos-cc" -O2 -Wall -c "$S/shim/posix_extra.c" -o "$O/posix_extra.o"
"$S/sodos-cc" -O2 -fno-builtin -c "$C/lib/math.c" -o "$O/mathx.o"
"$S/sodos-cc" -O2 -fno-builtin -c "$C/lib/fpconv.c" -o "$O/fpconvx.o"
# printf com %f (a LIBCX e -mno-sse): as definicoes locais vencem os imports
"$S/sodos-cc" -O2 -fno-builtin -c "$C/lib/printf.c" -o "$O/printfx.o"
"$C/build/link" /out:"$O/NETSURF.EXE" "$C/build/x_crt0.o" \
  "$N"/tree/netsurf/build/Linux-framebuffer/*.o \
  "$O/sodos_res.o" "$O/iconv.o" "$O/posix_extra.o" "$O/mathx.o" "$O/fpconvx.o" "$O/printfx.o" \
  "$O"/*.lib "$C/out/POSIXX.DLL" "$C/out/LIBCX.DLL" "$C/out/KERNEL.IMP"
ls -la "$O/NETSURF.EXE"
