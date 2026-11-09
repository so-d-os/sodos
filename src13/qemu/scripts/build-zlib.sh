#!/bin/sh
# build-zlib.sh - zlib estatica (LP64/SOX2) em sodos/sysroot.
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
. "$S/scripts/env.sh"
V=1.3.1
Z=$(sh "$S/scripts/fetch-src.sh" "https://github.com/madler/zlib/releases/download/v$V/zlib-$V.tar.gz" | tail -1)
B=$BUILD_ROOT/zlib-$V
rm -rf "$B"; mkdir -p "$B" "$SYSROOT/include" "$SYSROOT/lib/pkgconfig"
for f in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback \
         inffast inflate inftrees trees uncompr zutil; do
  $CC64 $CFLAGS64 -DHAVE_UNISTD_H -DHAVE_STDARG_H -c "$Z/$f.c" -o "$B/$f.o"
done
rm -f "$SYSROOT/lib/libz.a"
ar rcs "$SYSROOT/lib/libz.a" "$B"/*.o
cp "$Z/zlib.h" "$Z/zconf.h" "$SYSROOT/include/"
cat > "$SYSROOT/lib/pkgconfig/zlib.pc" <<PC
prefix=$SYSROOT
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: zlib
Description: zlib compression library (SODOS LP64)
Version: $V
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
PC
echo "ok: $SYSROOT/lib/libz.a"
