#!/bin/sh
# fetch-rust.sh - instala rustc/cargo no $HOME (sem root), para o configure
# do Firefox. O alvo x86_64-sodos ainda nao existe no rustc; isto so destrava
# a checagem de host.
set -e
if command -v rustc >/dev/null 2>&1; then
  echo "rustc ja instalado: $(rustc --version)"
  exit 0
fi
if [ ! -f /tmp/rustup.sh ]; then
  curl -sSf https://sh.rustup.rs -o /tmp/rustup.sh
fi
sh /tmp/rustup.sh -y --profile minimal --default-toolchain stable --no-modify-path
echo "adicione \$HOME/.cargo/bin ao PATH:"
echo '  . "$HOME/.cargo/env"'
