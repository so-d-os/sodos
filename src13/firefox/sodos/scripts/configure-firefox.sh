#!/bin/sh
# configure-firefox.sh - sonda o configure do Firefox com alvo x86_64-sodos.
#
#   FIREFOX_DIR=$HOME/ffsrc/firefox-153.6.0esr sh sodos/scripts/configure-firefox.sh
#
# Baixa (se preciso), aplica patches/0001-firefox-sodos-target.patch, roda o
# configure num objdir e mostra os erros. Hoje PARA no kernel do compilador
# de destino (gcc diz Linux, alvo diz SODOS); ver docs/05-firefox-build.md.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
D=${FIREFOX_DIR:-$S/vendor/firefox-153.6.0esr}
OBJ=${FIREFOX_OBJ:-$D/obj-sodos}
PATCH=$S/patches/0001-firefox-sodos-target.patch

if [ ! -f "$D/configure" ]; then
  FIREFOX_DIR="$D" sh "$S/sodos/scripts/fetch-firefox.sh"
fi

cd "$D"
if ! grep -q 'sodos\*' build/autoconf/config.sub 2>/dev/null; then
  echo "aplicando $(basename "$PATCH")"
  patch -p1 < "$PATCH"
fi

mkdir -p "$OBJ"
cat > "$OBJ/mozconfig" <<'EOF'
ac_add_options --target=x86_64-sodos
ac_add_options --enable-application=browser
ac_add_options --disable-tests
ac_add_options --disable-jemalloc
ac_add_options --without-system-nspr
ac_add_options --without-system-nss
ac_add_options --without-system-icu
EOF

export MOZCONFIG="$OBJ/mozconfig"
export MOZBUILD_STATE_PATH="${MOZBUILD_STATE_PATH:-$HOME/.mozbuild}"
export CC=${CC:-gcc}
if [ -z "$CXX" ]; then
  if command -v g++ >/dev/null 2>&1; then
    CXX=g++
  else
    G=$S/vendor/gxx
    if [ -x "$G/usr/bin/x86_64-linux-gnu-g++-13" ]; then
      cat > "$OBJ/gxx" <<EOF
#!/bin/sh
exec "$G/usr/bin/x86_64-linux-gnu-g++-13" \\
  -B"$G/usr/libexec/gcc/x86_64-linux-gnu/13/" \\
  -isystem /usr/lib/gcc/x86_64-linux-gnu/13/include \\
  -isystem "$G/usr/include/c++/13" \\
  -isystem "$G/usr/include/x86_64-linux-gnu/c++/13" \\
  -isystem "$G/usr/include" "\$@"
EOF
      chmod +x "$OBJ/gxx"
      CXX=$OBJ/gxx
    else
      echo "aviso: sem g++ e sem vendor/gxx (rode sodos/scripts/fetch-gxx.sh)" >&2
      CXX=g++
    fi
  fi
fi
export CXX
export HOST_CC=${HOST_CC:-gcc}
export HOST_CXX=${HOST_CXX:-$CXX}

cd "$OBJ"
echo "configurando em $OBJ (CC=$CC CXX=$CXX)"
if "$D/configure" --target=x86_64-sodos > configure.log 2>&1; then
  echo "configure OK (inesperado nesta fase)"
  exit 0
fi
echo "configure falhou; primeiros erros:"
grep -E "ERROR|Invalid|not found|Unknown OS|does not match" configure.log | head -15
echo "log: $OBJ/configure.log"
echo "veja docs/05-firefox-build.md para o que falta ajustar"
exit 1
