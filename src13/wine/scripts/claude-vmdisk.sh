#!/bin/sh
# claude-vmdisk.sh - monta o disco da VM do Claude Desktop sem rodar o
# instalador (que no QEMU leva ~47 min extraindo o .msix em PIO).
#
#   sh wine/scripts/claude-vmdisk.sh            baixa o .msix "latest" (se
#                                               ainda nao estiver em MSIXDIR)
#   MSIX=arquivo.msix sh wine/scripts/claude-vmdisk.sh
#
# Copia disk.img/os.img para VMDIR (padrao ~/sodos-wine-vm) e grava o pacote
# extraido em D:\WINE\APPS\Claude_<versao>_x64__pzs8sxrjxfjjc, como o
# AddPackageAsync do WINE.DLL (pkg.c) faria: nomes do ZIP com %xx decodificado.
# Depois: VMDIR=... KEEP=1 sh wine/scripts/vm.sh start win; claude-run.sh.
S=$(cd "$(dirname "$0")/../.." && pwd)
V=${VMDIR:-$HOME/sodos-wine-vm}
D=${MSIXDIR:-$HOME/dl-claude-msix}
URL=https://api.anthropic.com/api/desktop/win32/x64/msix/latest/redirect
set -e
mkdir -p "$V" "$D"
if [ -z "$MSIX" ]; then
  MSIX=$D/claude.msix
  [ -f "$MSIX" ] || curl -sfL -A sodos-test -o "$MSIX" "$URL"
fi
VER=$(python3 -I -c '
import sys, zipfile, re
x = zipfile.ZipFile(sys.argv[1]).read("AppxManifest.xml").decode("utf-8", "replace")
m = re.search(r"<Identity\b[^>]*?\bVersion=\"([^\"]+)\"", x, re.S)
print(m.group(1) if m else "")
' "$MSIX")
[ -n "$VER" ] || { echo "sem versao no AppxManifest.xml"; exit 1; }
PKG=Claude_${VER}_x64__pzs8sxrjxfjjc
X=$D/stage
rm -rf "$X"
mkdir -p "$X/$PKG"
# extrai com os nomes decodificados (fora da pasta dos arquivos baixados)
python3 -I -c '
import sys, zipfile, os, urllib.parse
z = zipfile.ZipFile(sys.argv[1]); out = sys.argv[2]
for i in z.infolist():
    if i.is_dir(): continue
    p = os.path.join(out, *urllib.parse.unquote(i.filename).split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f: f.write(z.read(i))
' "$MSIX" "$X/$PKG"
pgrep -f "[q]emu-system.*$V/disk.img" >/dev/null && { echo "pare a VM antes (vm.sh stop)"; exit 1; }
cp --sparse=always "$S/disk.img" "$V/disk.img"
cp "$S/os.img" "$V/os.img"
I="$V/disk.img@@512"
mmd -i "$I" ::/WINE ::/WINE/APPS 2>/dev/null || true
mcopy -s -i "$I" "$X/$PKG" ::/WINE/APPS/
rm -f "$V/wine.md5"
echo "pronto: $V/disk.img com D:\\WINE\\APPS\\$PKG"
