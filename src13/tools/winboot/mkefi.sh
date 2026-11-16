#!/bin/sh
# mkefi.sh - gera o GRUB EFI autonomo que o Windows Boot Manager chama.
#
#   sh tools/winboot/mkefi.sh [saida]      (padrao: sodos-grubx64.efi)
#
# Embute tools/winboot/grub.cfg num grubx64.efi (grub-mkstandalone, x86_64-efi).
# Usa o GRUB instalado (grub-common + grub-efi-amd64-bin) se houver; senao baixa
# esses pacotes do apt para ~/.cache/sodos-grub (apt-get download, sem sudo) e
# usa de la. Roda no WSL ou num Linux.
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-sodos-grubx64.efi}
CFG=tools/winboot/grub.cfg

MK=grub-mkstandalone
DIR=/usr/lib/grub/x86_64-efi
if ! command -v $MK >/dev/null 2>&1 || [ ! -f $DIR/multiboot2.mod ]; then
  C=${SODOS_GRUB_CACHE:-$HOME/.cache/sodos-grub}
  if [ ! -x "$C/root/usr/bin/grub-mkstandalone" ] || [ ! -f "$C/root/usr/lib/grub/x86_64-efi/multiboot2.mod" ]; then
    command -v apt-get >/dev/null 2>&1 || {
      echo "mkefi: sem grub-mkstandalone e sem apt-get. Instale grub-common e grub-efi-amd64-bin."; exit 1; }
    echo "mkefi: baixando o GRUB EFI para $C (sem sudo)..."
    mkdir -p "$C/deb"
    ( cd "$C/deb" && apt-get download grub-common grub-efi-amd64-bin && \
      { apt-get download libefivar1t64 libefiboot1t64 2>/dev/null || apt-get download libefivar1 libefiboot1; } )
    rm -rf "$C/root"
    for d in "$C"/deb/*.deb; do dpkg -x "$d" "$C/root"; done
  fi
  MK="$C/root/usr/bin/grub-mkstandalone"
  DIR="$C/root/usr/lib/grub/x86_64-efi"
  LD_LIBRARY_PATH="$C/root/usr/lib/x86_64-linux-gnu:$C/root/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  export LD_LIBRARY_PATH
fi

# So o necessario no memdisk (o grub-mkstandalone poe as dependencias).
MODS="part_gpt part_msdos fat ntfs exfat multiboot2 all_video efi_gop efi_uga \
video_bochs video_cirrus gfxterm search search_label search_fs_file \
search_fs_uuid test normal configfile echo sleep reboot halt ls cat loadenv lsefimmap"

"$MK" -O x86_64-efi -d "$DIR" -o "$OUT" \
  --install-modules="$MODS" --modules="part_gpt fat search search_label search_fs_file" \
  --locales= --themes= --fonts= \
  "boot/grub/grub.cfg=$CFG"
echo "mkefi: $OUT ($(wc -c < "$OUT") bytes)"
