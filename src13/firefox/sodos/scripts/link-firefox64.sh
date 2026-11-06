#!/bin/sh
# link-firefox64.sh - liga o browser em um unico executavel SOX2 (FIREFOX.EXE).
#
# O SODOS nao tem biblioteca compartilhada em LP64, entao libxul, mozglue, NSPR, NSS
# e os demais entram no executavel. As listas de objetos sao as que o build gera.
# Rode depois de: build-browser64.sh build pre-export export compile
#
# Saida: $FIREFOX_OBJ/dist/bin/FIREFOX.EXE
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
O=${FIREFOX_OBJ:-$HOME/ffsrc/obj-browser64}
T=$O/sodos-link
mkdir -p "$T"
: > "$T/objs.list"

add_list() {
  [ -f "$O/$1" ] || { echo "falta $1" >&2; exit 1; }
  d=$(dirname "$O/$1")
  sed -e "s#^\([^/]\)#$d/\1#" "$O/$1" >> "$T/objs.list"
  echo >> "$T/objs.list"  # as listas nao terminam em quebra de linha
}

add_list browser/app/firefox.list
add_list toolkit/library/build/libxul_so.list
add_list js/src/build/libjs_static_a.list
add_list config/external/gkcodecs/libgkcodecs_so.list
add_list media/ffvpx/libavcodec/libmozavcodec_so.list
add_list media/ffvpx/libavutil/libmozavutil_so.list
add_list config/external/lgpllibs/liblgpllibs_so.list
add_list config/external/sqlite/libmozsqlite3_so.list
add_list security/nss/lib/nss/nss_nss3/libnss3_so.list
add_list security/nss/lib/ssl/ssl_ssl3/libssl3_so.list
add_list security/nss/lib/smime/smime_smime3/libsmime3_so.list
add_list security/nss/lib/util/util_nssutil3/libnssutil3_so.list
for d in config/external/nspr/pr config/external/nspr/ds config/external/nspr/libc; do
  find "$O/$d" -maxdepth 1 -name '*.o' >> "$T/objs.list"
done
# Normaliza ../ e tira repetidos (as listas compartilham objetos).
python3 - "$T/objs.list" <<'EOF'
import os, sys
p = sys.argv[1]
seen, out = set(), []
for line in open(p):
    f = os.path.normpath(line.strip())
    if f and f not in seen:
        seen.add(f)
        out.append(f)
open(p, "w").write("\n".join(out) + "\n")
print("objetos:", len(out))
EOF

export PATH="$S/toolchain/bin:$PATH"
x86_64-sodos-g++64 -o "$O/dist/bin/FIREFOX.EXE" "@$T/objs.list" \
  "$O/x86_64-sodos/release/libgkrust.a" "$O/build/pure_virtual/libpure_virtual.a"
ls -la "$O/dist/bin/FIREFOX.EXE"
