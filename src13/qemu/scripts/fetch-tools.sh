#!/bin/sh
# fetch-tools.sh - ninja e venv/pip locais (sem root) em vendor/tools, para o
# configure do QEMU (meson vem empacotado no tarball do QEMU).
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
D=$S/vendor/tools
mkdir -p "$D"
need=""
command -v ninja >/dev/null 2>&1 || [ -x "$D/usr/bin/ninja" ] || need="$need ninja-build"
[ -x "$D/usr/bin/pkgconf" ] || need="$need pkgconf-bin libpkgconf3"
python3 -c 'import ensurepip' 2>/dev/null || [ -f "$D/.venv-ok" ] || need="$need python3.12-venv python3-pip-whl python3-setuptools-whl"
[ -z "$need" ] && echo "ferramentas ok"
if [ -n "$need" ]; then
T=$(mktemp -d /tmp/sodos-qtools-XXXXXX)
trap 'rm -rf "$T"' EXIT
(cd "$T" && apt-get download $need)
for deb in "$T"/*.deb; do dpkg -x "$deb" "$D"; done
case "$need" in *venv*) touch "$D/.venv-ok" ;; esac
echo "ferramentas em $D"
fi

# venv proprio com pip: pelo ensurepip do sistema quando existe (Debian), senao
# direto do wheel baixado acima (WSL/Ubuntu sem ensurepip).
VENV=$D/venv
if [ ! -x "$VENV/bin/pip" ] || ! "$VENV/bin/python" -c "import setuptools" 2>/dev/null; then
  if python3 -c 'import ensurepip' 2>/dev/null; then
    rm -rf "$VENV"
    python3 -m venv --system-site-packages "$VENV"
    "$VENV/bin/python" -c "import setuptools" 2>/dev/null || "$VENV/bin/pip" install -q setuptools
    echo "venv em $VENV"
    exit 0
  fi
  python3 -m venv --system-site-packages --without-pip "$VENV"
  W=$(ls "$D"/usr/share/python-wheels/pip-*.whl | head -1)
  "$VENV/bin/python" "$W/pip" install --no-index -q "$W" "$D"/usr/share/python-wheels/setuptools-*.whl
fi
echo "venv em $VENV"
