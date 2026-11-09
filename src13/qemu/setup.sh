#!/bin/sh
# setup.sh - baixa o QEMU e as ferramentas de build para vendor/ (fora do git).
set -e
S=$(cd "$(dirname "$0")" && pwd)
sh "$S/scripts/fetch-tools.sh"
sh "$S/scripts/fetch-qemu.sh"
