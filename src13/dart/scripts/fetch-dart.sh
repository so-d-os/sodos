#!/bin/bash
# Baixa o fonte do Dart SDK (mesma versao do Flutter do host) em ~/dartsrc.
set -e
VER=${1:-3.13.2}
mkdir -p ~/dartsrc && cd ~/dartsrc
[ -d depot_tools ] || git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH=$HOME/dartsrc/depot_tools:$PATH
export DEPOT_TOOLS_UPDATE=0
[ -d sdk/.git ] || git clone --depth 1 --branch "$VER" https://dart.googlesource.com/sdk.git sdk
cat > .gclient <<'EOF'
solutions = [
  {
    "name": "sdk",
    "url": "https://dart.googlesource.com/sdk.git",
    "deps_file": "DEPS",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
  },
]
EOF
gclient sync --no-history -D -j 8
echo "FETCH_OK $(cd sdk && git describe --tags --always)"
du -sh ~/dartsrc
