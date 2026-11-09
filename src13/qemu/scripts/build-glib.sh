#!/bin/sh
# build-glib.sh - glib 2.76 estatica (LP64/SOX2) em sodos/sysroot.
# libglib-2.0.a + libgmodule-2.0.a (o QEMU so inclui gmodule.h); sem gobject/gio.
# O pcre2 vem como subprojeto do proprio glib (wrap) e entra no .a.
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
. "$S/scripts/env.sh"
V=2.76.6
G=$(sh "$S/scripts/fetch-src.sh" "https://download.gnome.org/sources/glib/${V%.*}/glib-$V.tar.xz" | tail -1)
B=$BUILD_ROOT/glib-$V
if [ ! -f "$B/build.ninja" ]; then
  rm -rf "$B"
  "$MESON" setup "$B" "$G" \
    --cross-file "$S/sodos/meson-cross.ini" --cross-file "$S/sodos/cross-sodos.ini" \
    --buildtype=release -Ddefault_library=static \
    -Dtests=false -Dinstalled_tests=false -Dnls=disabled -Dselinux=disabled \
    -Dxattr=false -Dlibmount=disabled -Dman=false -Dgtk_doc=false \
    -Dlibelf=disabled -Dsysprof=disabled \
    -Dglib_debug=disabled -Dbsymbolic_functions=false \
    -Dforce_posix_threads=true -Doss_fuzz=disabled \
    --wrap-mode=default
fi
P=subprojects/pcre2-10.42
LIBS="glib/libglib-2.0.a glib/gnulib/libgnulib.a glib/libcharset/libcharset.a
      gmodule/libgmodule-2.0.a $P/libpcre2-8.a subprojects/proxy-libintl/libintl.a"
ninja -C "$B" $LIBS "$@"

# Instala no sysroot: .a, headers (como em include/glib-2.0) e glib-2.0.pc.
I=$SYSROOT/include/glib-2.0
rm -rf "$I" "$SYSROOT/lib/glib-2.0"
mkdir -p "$I/glib/deprecated" "$SYSROOT/lib/glib-2.0/include"
for l in $LIBS; do cp "$B/$l" "$SYSROOT/lib/"; done
cp "$G/glib/glib.h" "$G/glib/glib-unix.h" "$G/gmodule/gmodule.h" "$I/"
# gmodule.h inclui <gmodule/gmodule-visibility.h> (gerado)
mkdir -p "$I/gmodule"
cp "$B"/gmodule/gmodule-visibility.h "$I/gmodule/"
cp "$G"/glib/*.h "$I/glib/"
cp "$G"/glib/deprecated/*.h "$I/glib/deprecated/"
cp "$B/glib/glibconfig.h" "$SYSROOT/lib/glib-2.0/include/"
# headers gerados (gversionmacros.h, glib-visibility.h...)
for h in "$B"/glib/*.h; do
  case "$(basename "$h")" in glibconfig.h|config.h) ;; *) cp "$h" "$I/glib/" ;; esac
done
cat > "$SYSROOT/lib/pkgconfig/glib-2.0.pc" <<PC
prefix=$SYSROOT
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: GLib
Description: C Utility Library (SODOS LP64, estatica)
Version: $V
Libs: -L\${libdir} -lglib-2.0 -lgmodule-2.0 -lgnulib -lcharset -lpcre2-8 -lintl
Cflags: -I\${includedir}/glib-2.0 -I\${libdir}/glib-2.0/include -DGLIB_STATIC_COMPILATION
PC
echo "ok: glib $V em $SYSROOT"
